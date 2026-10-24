"""Parent-side regression tests; never import the real desktop or launch a worker.

Run: python -B -m unittest discover -s tools/game-test -p test_monitor.py -v
All receipts and fake profiles live in a disposable directory under project tmp/.
"""

from contextlib import ExitStack
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock


HERE = Path(__file__).resolve().parent


class MonitorTests(unittest.TestCase):
    def setUp(self):
        scratch = HERE.parents[1] / "tmp"
        scratch.mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="test-monitor-", dir=scratch)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.out = self.root / "attempt"
        profile = self.root / "profile"
        (profile / "saves").mkdir(parents=True)
        (profile / "saves/manual.sav").write_bytes(b"original save")
        install = self.root / "install"
        install.mkdir()
        self.cfg = dict(profile=str(profile), save_dir="saves",
                        executable=str(install / "fake-game.exe"),
                        logs=[], scenario="menu", run_timeout=5)
        self.locked = False
        self.alive = False
        self.events = []
        self.guard_exit_error = None
        self.worker_result = dict(result="passed", safe_exit="menu_exit_and_detach_confirmed")
        self.result_text = None
        self.publish_owned = False
        self.owner = dict(pid=424242, created=123, path=self.cfg["executable"], hwnd=7)
        self.child = mock.Mock(name="worker")
        self.child.pid = 424243
        self.child.returncode = 0
        self.child.poll.side_effect = lambda: self.child.returncode
        self.child.terminate.side_effect = lambda: self.events.append(("stop_worker", self.locked))
        self.child.kill.side_effect = self.stop_worker
        self.child.wait.side_effect = self.wait_worker

        # Import in isolation: desktop's real import calls native Windows APIs.
        spec = importlib.util.spec_from_file_location("_monitor_runner_under_test", HERE / "runner.py")
        self.runner = importlib.util.module_from_spec(spec)
        self.desktop = types.SimpleNamespace(
            same_process=mock.Mock(side_effect=lambda owner: self.alive),
            window_alive=mock.Mock(return_value=True),
            process_metrics=mock.Mock(return_value=dict(created=123, cpu_seconds=0)),
            terminate=mock.Mock(side_effect=self.stop_owned),
            u=types.SimpleNamespace(IsHungAppWindow=mock.Mock(return_value=False)),
        )
        with mock.patch.dict(sys.modules, desktop=self.desktop):
            spec.loader.exec_module(self.runner)

        case = self

        class FakeProtectedFiles:
            manifest = []

            def __init__(self, paths, backup):
                pass

            def __enter__(self):
                case.locked = True
                case.events.append("guard_enter")
                return self

            def __exit__(self, *exc):
                case.events.append(("guard_exit", case.alive, case.child.returncode))
                case.locked = False
                if case.guard_exit_error:
                    raise case.guard_exit_error
                return False

        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.dict(
            sys.modules, guard=types.SimpleNamespace(ProtectedFiles=FakeProtectedFiles)))
        self.popen = mock.Mock(side_effect=self.launch_worker)
        # Replace the module, not subprocess.Popen globally (other tests may need it).
        self.runner.subprocess = types.SimpleNamespace(
            Popen=self.popen, run=mock.Mock(side_effect=AssertionError("unexpected subprocess.run")),
            TimeoutExpired=subprocess.TimeoutExpired)
        self.runner.process_inventory = mock.Mock(return_value=[])
        self.runner.crash_reports = mock.Mock(return_value={})
        self.runner.collect_crashes = mock.Mock(return_value=[])
        self.runner.freeze_logs = mock.Mock(side_effect=self.freeze_logs)
        ticks = iter(range(1000))
        self.runner.time = types.SimpleNamespace(
            monotonic=lambda: next(ticks), sleep=mock.Mock())
        self.stack.enter_context(mock.patch("sys.stdout", new=io.StringIO()))

    def freeze_logs(self, cfg, destination):
        self.events.append(("logs", destination.name, self.locked))

    def launch_worker(self, *args, **kwargs):
        self.events.append("worker_launch")
        if self.publish_owned:
            self.alive = True
            self.runner.dump(self.out / "owned-process.json", self.owner)
        if self.result_text is not None:
            (self.out / "result.json").write_text(self.result_text, encoding="utf-8")
        elif self.worker_result is not None:
            self.runner.dump(self.out / "result.json", self.worker_result)
        return self.child

    def stop_worker(self):
        self.events.append(("kill_worker", self.locked))
        self.child.returncode = -1

    def wait_worker(self, timeout):
        self.child.returncode = -1
        return self.child.returncode

    def stop_owned(self, owner):
        self.assertEqual(owner, self.owner)
        self.events.append(("stop_owned", self.locked))
        self.alive = False

    def run_and_read(self):
        final = self.runner.run_attempt(self.cfg, self.out)
        # Verify the durable contract, not just a returned in-memory status.
        receipt = json.loads((self.out / "result.json").read_text(encoding="utf-8"))
        complete = json.loads((self.out / "guard-complete.json").read_text(encoding="utf-8"))
        self.assertEqual(receipt, final)
        self.assertIn("finished", receipt)
        self.assertIsInstance(complete["verified"], bool)
        self.assertEqual(complete["verified"], receipt["guard"] == "verified_original_hashes_and_released")
        self.assertIs(complete["owned_process_exited"], True)
        self.assertFalse(self.locked)
        return receipt

    def assert_recovered_under_guard(self):
        self.assertIn(("stop_owned", True), self.events)
        self.assertIn(("logs", "recovery-logs", True), self.events)
        exits = [event for event in self.events if isinstance(event, tuple) and event[0] == "guard_exit"]
        self.assertEqual(len(exits), 1)
        self.assertFalse(exits[0][1], "owned process was alive when file protection ended")
        self.assertIsNotNone(exits[0][2], "worker was alive when file protection ended")
        self.assertLess(self.events.index(("stop_owned", True)), self.events.index(exits[0]))

    def test_success_control_can_pass(self):
        final = self.run_and_read()
        self.assertEqual(final["result"], "passed")
        self.assertEqual(final["guard"], "verified_original_hashes_and_released")
        self.assertEqual(final["safe_exit"], "menu_exit_and_detach_confirmed")

    def test_worker_launch_exception_keeps_final_receipts(self):
        self.popen.side_effect = OSError("worker launch denied")
        final = self.run_and_read()
        self.assertEqual(final["result"], "failed")
        self.assertIn("worker launch denied", final["parent_error"])

    def test_monitor_exception_recovers_before_guard_exit_and_keeps_receipts(self):
        self.publish_owned = True
        self.child.returncode = None
        self.desktop.process_metrics.side_effect = RuntimeError("monitor metrics unavailable")
        final = self.run_and_read()
        self.assertEqual(final["result"], "failed")
        self.assertIn("monitor metrics unavailable", final["parent_error"])
        self.assertIn(("stop_worker", True), self.events)
        self.assert_recovered_under_guard()

    def test_worker_without_result_is_failed_with_final_receipts(self):
        self.worker_result = None
        final = self.run_and_read()
        self.assertEqual(final["result"], "failed")
        self.assertEqual(final["parent_problem"], "worker_failed_without_result")

    def test_exited_worker_leaving_owned_process_is_recovered_under_guard(self):
        self.publish_owned = True
        final = self.run_and_read()
        self.assertEqual(final["result"], "failed")
        self.assert_recovered_under_guard()

    def test_timeout_stops_worker_and_owned_process_under_guard(self):
        self.publish_owned = True
        self.child.returncode = None
        final = self.run_and_read()
        self.assertEqual(final["result"], "failed")
        self.assertEqual(final["parent_problem"], "run_timeout")
        self.assertIn(("stop_worker", True), self.events)
        self.assert_recovered_under_guard()

    def test_failed_worker_result_survives_zero_exit_code(self):
        self.worker_result = dict(result="failed", error="main_menu_timeout", startup="not_tested")
        final = self.run_and_read()
        self.assertEqual(final["result"], "failed")
        self.assertEqual(final["error"], "main_menu_timeout")
        self.assertEqual(final["startup"], "not_tested")

    def test_nonzero_worker_exit_overrides_passed_result(self):
        self.child.returncode = 3
        self.assertEqual(self.run_and_read()["result"], "failed")

    def test_new_crash_report_overrides_passed_result(self):
        self.runner.collect_crashes.return_value = ["Witcher3_mock_crash"]
        final = self.run_and_read()
        self.assertEqual(final["result"], "failed")
        self.assertEqual(final["crashes"], ["Witcher3_mock_crash"])

    def test_guard_verification_failure_overrides_passed_result(self):
        self.guard_exit_error = RuntimeError("Protected file changed: manual.sav")
        final = self.run_and_read()
        self.assertEqual(final["result"], "failed")
        self.assertEqual(final["guard"], "failed")
        self.assertIn("Protected file changed", final["parent_error"])

    def test_corrupt_worker_result_still_gets_failed_final_receipts(self):
        # A killed worker or corrupt receipt must not bypass parent finalization.
        self.result_text = '{"result":'
        self.assertEqual(self.run_and_read()["result"], "failed")

    def test_worker_wait_failure_does_not_skip_owned_process_recovery(self):
        self.publish_owned = True
        self.child.returncode = None
        self.desktop.process_metrics.side_effect = RuntimeError("monitor metrics unavailable")
        self.child.wait.side_effect = subprocess.TimeoutExpired("fake-worker", 10)
        final = self.run_and_read()
        self.assertEqual(final["result"], "failed")
        self.assert_recovered_under_guard()

    def test_denied_game_termination_holds_guard_until_manual_recovery(self):
        self.publish_owned = True
        self.desktop.terminate.side_effect = PermissionError('termination denied')
        def recover_during_wait(seconds):
            self.assertTrue(self.locked, 'Original-file protection ended before intervention')
            self.assertTrue((self.out/'intervention-required.json').exists())
            self.alive = False
        self.runner.time.sleep.side_effect = recover_during_wait
        final = self.run_and_read()
        self.assertEqual(final['result'], 'failed')
        self.assertTrue((self.out/'intervention-resolved.json').exists())
        exits = [e for e in self.events if isinstance(e, tuple) and e[0]=='guard_exit']
        self.assertFalse(exits[0][1])

    def test_denied_worker_stop_keeps_protection_before_owned_recovery(self):
        self.publish_owned = True
        self.child.returncode = None
        self.desktop.process_metrics.side_effect = RuntimeError('monitor failed')
        self.child.terminate.side_effect = PermissionError('worker stop denied')
        self.child.kill.side_effect = PermissionError('worker kill denied')
        def manual_worker_exit(seconds):
            self.assertTrue(self.locked)
            self.assertTrue((self.out/'worker-intervention-required.json').exists())
            self.child.returncode = -1
        self.runner.time.sleep.side_effect = manual_worker_exit
        final = self.run_and_read()
        self.assertEqual(final['result'], 'failed')
        self.assert_recovered_under_guard()


if __name__ == "__main__":
    unittest.main()
