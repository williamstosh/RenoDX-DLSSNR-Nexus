"""Parent-side regression tests; never import the real desktop or launch a worker.

Run: python -B -m unittest discover -s tools/game-test -p test_monitor.py -v
All receipts and fake profiles live in a disposable directory under project tmp/.
"""

from contextlib import ExitStack
import importlib.util
import io
import importlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
from PIL import Image


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
        self.cfg = dict(profile=str(profile), save_dir="saves", install=str(install),
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
            grant_foreground_permission=mock.Mock(return_value=dict(granted=True, error=None)),
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
        self.stack.enter_context(mock.patch.object(self.runner.crash_evidence, 'collect', return_value=[]))
        self.hang_capture = self.stack.enter_context(mock.patch.object(
            self.runner.hang_evidence, 'capture', return_value=dict(status='captured')))
        ticks = iter(range(1000))
        self.runner.time = types.SimpleNamespace(
            monotonic=lambda: next(ticks), sleep=mock.Mock())
        self.stack.enter_context(mock.patch("sys.stdout", new=io.StringIO()))

    def freeze_logs(self, cfg, destination):
        self.events.append(("logs", destination.name, self.locked))

    def test_evidence_failure_does_not_prevent_guard_restore(self):
        self.runner.crash_evidence.collect.side_effect = OSError('synthetic failure')
        final = self.runner.run_attempt(self.cfg, self.out)
        self.assertEqual(final['guard'], 'verified_original_hashes_and_released')
        self.assertIn('synthetic failure', final['diagnostic_error'])
        self.assertFalse(self.locked)

    def test_artifact_cleanup_keeps_failure_sample_and_recovery_configuration(self):
        lifecycle = self.runner.output_lifecycle
        with mock.patch.dict(lifecycle.os.environ, {'NR_NEXUS_TEST_BUDGET_BYTES': '0', 'NR_NEXUS_TEST_KEEP_RAW': '0'}):
            lifecycle.initialize(self.out)
            for name in ('ocr.png', 'menu.png', 'latest.png', 'hdr-nr-on.png'):
                (self.out / name).write_bytes(b'image')
            (self.out / 'protected').mkdir()
            save, config = self.out / 'protected/save.bin', self.out / 'protected/config.bin'
            save.write_bytes(b'save copy')
            config.write_bytes(b'config copy')
            evidence = self.out / 'hang-evidence/game.dmp'
            evidence.parent.mkdir()
            evidence.write_bytes(b'synthetic dump')
            self.runner.dump(self.out / 'protected-files.json', [
                dict(path='original.sav', backup_path=str(save)),
                dict(path='ReShade.ini', backup_path=str(config))])
            self.runner.finish_artifacts(self.out, dict(result='failed'), True)
            self.assertFalse((self.out / 'ocr.png').exists())
            self.assertFalse((self.out / 'menu.png').exists())
            self.assertFalse(save.exists())
            for path in (config, self.out / 'latest.png', self.out / 'hdr-nr-on.png', evidence):
                self.assertTrue(path.exists(), path)

    def test_motion_cleanup_without_budget_keeps_verified_preview_or_originals(self):
        lifecycle = self.runner.output_lifecycle
        for mode in ('normal', 'keep_raw', 'bad_hash'):
            with self.subTest(mode=mode), mock.patch.dict(lifecycle.os.environ, {
                    'NR_NEXUS_TEST_BUDGET_BYTES': '0',
                    'NR_NEXUS_TEST_KEEP_RAW': '1' if mode == 'keep_raw' else '0'}):
                out = self.root / mode
                lifecycle.initialize(out)
                motion = out / 'motion-off'
                (motion / 'frames').mkdir(parents=True)
                frames = []
                for index, color in enumerate(('red', 'green', 'blue')):
                    path = motion / f'frames/{index:04}.jpg'
                    Image.new('RGB', (16, 9), color).save(path)
                    frames.append(dict(index=index, file=f'frames/{index:04}.jpg',
                        sha256=self.runner.sha(path), bytes=path.stat().st_size,
                        capture_begin_ns=index * 200000000))
                if mode == 'bad_hash':
                    frames[1]['sha256'] = 'incorrect'
                self.runner.dump(motion / 'recording.json', dict(
                    status='complete', width=16, height=9, frames=frames))
                self.runner.finish_artifacts(out, dict(result='passed'), False)
                self.assertTrue((motion / 'frames/0000.jpg').exists())
                self.assertTrue((motion / 'frames/0002.jpg').exists())
                self.assertEqual((motion / 'frames/0001.jpg').exists(), mode != 'normal')
                if mode == 'normal':
                    summary = json.loads((motion / 'preview-summary.json').read_text())
                    self.assertTrue(summary['source_hashes_verified'])
                    self.assertEqual(summary['frame_count'], 3)
                    state = lifecycle._read(out)
                    state['ownerPid'] = 2**31 - 1
                    lifecycle._write(out, state)
                    lifecycle.maintain(out, limit=1)
                    self.assertTrue((motion / 'preview.webp').exists())
                elif mode == 'bad_hash':
                    self.assertTrue((motion / 'preview-error.json').exists())

    def test_legacy_cleanup_only_adopts_verified_finished_runs(self):
        with mock.patch.dict(sys.modules, runner=self.runner):
            spec = importlib.util.spec_from_file_location('_game_cleanup_under_test', HERE / 'cleanup_artifacts.py')
            cleanup = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(cleanup)
        lifecycle = self.runner.output_lifecycle
        for name, verified in [('finished', True), ('unrecovered', False)]:
            directory = self.root / name
            directory.mkdir()
            (directory / 'ocr.png').write_bytes(b'intermediate')
            self.runner.dump(directory / 'guard-complete.json', dict(verified=verified, owned_process_exited=True))
            self.runner.dump(directory / 'result.json', dict(result='failed', finished='2026-11-07',
                guard='verified_original_hashes_and_released'))
            self.runner.dump(directory / 'protected-files.json', [])
        with mock.patch.dict(lifecycle.os.environ, {'NR_NEXUS_TEST_BUDGET_BYTES': '0', 'NR_NEXUS_TEST_KEEP_RAW': '0'}):
            result = cleanup.adopt_completed(self.root)
        self.assertEqual(len(result['adopted']), 1)
        self.assertEqual(len(result['skipped']), 1)
        self.assertFalse((self.root / 'finished/ocr.png').exists())
        self.assertTrue((self.root / 'unrecovered/ocr.png').exists())

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
        self.hang_capture.assert_not_called()

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

    def test_watchdog_captures_live_process_after_worker_stops_before_recovery(self):
        self.publish_owned = True
        self.child.returncode = None

        def capture(owner, destination):
            self.assertEqual(owner, self.owner)
            self.assertEqual(destination, self.out / 'hang-evidence')
            self.events.append(('hang_capture', self.locked, self.alive, self.child.returncode))
            return dict(status='captured')

        self.hang_capture.side_effect = capture
        final = self.run_and_read()
        self.assertEqual(final['hang_evidence'], dict(reason='run_timeout', status='captured'))
        self.assertIn(('hang_capture', True, True, -1), self.events)
        self.assertLess(self.events.index(('hang_capture', True, True, -1)),
                        self.events.index(('stop_owned', True)))

    def test_persistent_unresponsive_requests_one_dump(self):
        self.publish_owned = True
        self.child.returncode = None
        self.cfg['run_timeout'] = 500
        self.desktop.u.IsHungAppWindow.return_value = True
        final = self.run_and_read()
        self.assertEqual(final['parent_problem'], 'persistent_unresponsive')
        self.assertEqual(final['hang_evidence']['reason'], 'persistent_unresponsive')
        self.hang_capture.assert_called_once_with(self.owner, self.out / 'hang-evidence')
        self.assert_recovered_under_guard()

    def test_hang_capture_failure_keeps_original_watchdog_failure_and_recovers(self):
        self.publish_owned = True
        self.child.returncode = None
        self.hang_capture.side_effect = OSError('synthetic dump failure')
        final = self.run_and_read()
        self.assertEqual(final['parent_problem'], 'run_timeout')
        self.assertEqual(final['hang_evidence']['status'], 'failed')
        self.assertIn('synthetic dump failure', final['hang_evidence']['error'])
        self.assert_recovered_under_guard()

    def test_normal_exit_does_not_request_dump(self):
        final = self.run_and_read()
        self.assertEqual(final['result'], 'passed')
        self.hang_capture.assert_not_called()

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

    def configure_automatic_capture(self, exit_code=0, complete=True, finish_worker=True):
        script = self.root / 'capture.py'
        script.write_text('# synthetic capture script; never executed\n', encoding='utf-8')
        self.cfg.update(capture_script=str(script), capture_script_sha256=self.runner.sha(script),
                        capture_hold_seconds=10, run_timeout=100 if finish_worker else 5)
        self.publish_owned = True
        self.child.returncode = None
        capture = mock.Mock(name='automatic_capture', pid=424244, returncode=exit_code)
        capture.poll.side_effect = lambda: capture.returncode
        capture.terminate.side_effect = lambda: self.events.append(('stop_capture', self.locked))
        capture.kill.side_effect = lambda: self.events.append(('kill_capture', self.locked))

        def wait_capture(timeout=None):
            self.assertTrue(self.locked)
            cancel = json.loads((self.out / 'capture-cancel.json').read_text())
            self.assertEqual(cancel['reason'], 'protected_runner_stopping')
            self.events.append(('wait_capture', self.locked))
            capture.returncode = -1
            return capture.returncode

        capture.wait.side_effect = wait_capture

        def launch(command, **kwargs):
            if '--worker' in command:
                worker = self.launch_worker(command, **kwargs)
                self.runner.dump(self.out / 'capture-ready.json', dict(
                    pid=self.owner['pid'], identity=self.owner, automatic_foreground=True))
                return worker
            self.events.append(('capture_launch', self.locked))
            self.assertEqual(command, [sys.executable, '-B', str(script), str(self.out)])
            self.assertEqual(kwargs['creationflags'], 0x08000000)
            self.assertIs(kwargs['stdout'], kwargs['stderr'])
            if complete:
                self.runner.dump(self.out / 'capture-complete.json',
                                 dict(pid=self.owner['pid'], completed=True))
            return capture

        sleeps = 0

        def worker_progress(seconds):
            nonlocal sleeps
            sleeps += 1
            if finish_worker and sleeps == 3:
                # Model the worker consuming the parent's durable capture receipt.
                if (self.out / 'capture-failed.json').exists():
                    self.runner.dump(self.out / 'result.json',
                                     dict(result='failed', error='capture_automatic_failed'))
                self.child.returncode = 0
                self.alive = False

        self.popen.side_effect = launch
        self.runner.time.sleep.side_effect = worker_progress
        return script, capture

    def test_ready_launches_automatic_capture_once_without_confirmation(self):
        script, capture = self.configure_automatic_capture()
        final = self.run_and_read()
        self.assertEqual(final['result'], 'passed')
        self.assertEqual(self.popen.call_count, 2)
        self.assertEqual(self.events.count(('capture_launch', True)), 1)
        receipt = json.loads((self.out / 'automatic-capture-process.json').read_text())
        self.assertEqual(receipt['pid'], capture.pid)
        self.assertEqual(receipt['script'], str(script))
        self.assertEqual(receipt['sha256'], self.cfg['capture_script_sha256'])
        self.assertEqual(self.desktop.grant_foreground_permission.call_args_list,
                         [mock.call(self.child.pid), mock.call(capture.pid)])
        self.assertFalse((self.out / 'capture-failed.json').exists())
        result = json.loads((self.out / 'capture-script-result.json').read_text())
        self.assertEqual((result['pid'], result['exit_code']), (self.owner['pid'], 0))
        self.assertFalse(list(self.out.glob('computer-use-*.json')))
        capture.terminate.assert_not_called()
        self.assertTrue(self.popen.call_args.kwargs['stdout'].closed)

    def test_changed_capture_script_writes_failure_without_launch_or_retry(self):
        script, capture = self.configure_automatic_capture()
        script.write_text('# changed after preflight\n', encoding='utf-8')
        final = self.run_and_read()
        self.assertEqual(final['result'], 'failed')
        self.assertEqual(self.popen.call_count, 1)
        failure = json.loads((self.out / 'capture-failed.json').read_text())
        self.assertEqual(failure['stage'], 'automatic_capture_start')
        self.assertIn('changed after preflight', failure['error'])
        self.assertFalse((self.out / 'automatic-capture-process.json').exists())
        self.assertFalse((self.out / 'capture-complete.json').exists())
        capture.poll.assert_not_called()

    def assert_capture_exit_failed(self, exit_code, complete):
        self.configure_automatic_capture(exit_code=exit_code, complete=complete)
        final = self.run_and_read()
        self.assertEqual(final['result'], 'failed')
        self.assertEqual(self.popen.call_count, 2)
        failure = json.loads((self.out / 'capture-failed.json').read_text())
        self.assertEqual(failure['stage'], 'automatic_capture_exit')
        self.assertEqual(failure['exit_code'], exit_code)
        self.assertFalse((self.out / 'capture-script-result.json').exists())
        self.assertEqual(self.events.count(('capture_launch', True)), 1)

    def test_nonzero_capture_exit_rejects_even_an_existing_completion(self):
        self.assert_capture_exit_failed(exit_code=3, complete=True)

    def test_zero_capture_exit_without_completion_writes_failure(self):
        self.assert_capture_exit_failed(exit_code=0, complete=False)

    def test_early_completion_does_not_ack_a_running_capture_script(self):
        _, capture = self.configure_automatic_capture(
            exit_code=None, complete=True, finish_worker=False)
        self.cfg['run_timeout'] = 100
        sleeps = 0

        def script_progress(_seconds):
            nonlocal sleeps
            sleeps += 1
            self.assertTrue(self.locked)
            self.assertTrue(self.runner.capture_complete(self.out, self.owner['pid']))
            if sleeps <= 2:
                self.assertFalse((self.out / 'capture-script-result.json').exists())
                self.assertFalse(self.runner.capture_script_complete(self.out, self.owner['pid']))
                self.assertIsNone(capture.returncode)
                if sleeps == 2:
                    capture.returncode = 0
            else:
                self.assertTrue(self.runner.capture_script_complete(self.out, self.owner['pid']))
                self.child.returncode = 0
                self.alive = False

        self.runner.time.sleep.side_effect = script_progress
        final = self.run_and_read()
        self.assertEqual(final['result'], 'passed')
        self.assertEqual(sleeps, 3)
        self.assertEqual(self.popen.call_count, 2)
        capture.terminate.assert_not_called()
        self.assertFalse((self.out / 'capture-cancel.json').exists())

    def test_recovery_cancels_and_waits_for_capture_before_worker_and_owned_process(self):
        _, capture = self.configure_automatic_capture(
            exit_code=None, complete=False, finish_worker=False)
        final = self.run_and_read()
        self.assertEqual(final['result'], 'failed')
        self.assertEqual(final['parent_problem'], 'run_timeout')
        capture.terminate.assert_not_called()
        capture.wait.assert_called_once_with(timeout=5)
        capture.kill.assert_not_called()
        self.assertLess(self.events.index(('wait_capture', True)),
                        self.events.index(('stop_worker', True)))
        self.assertLess(self.events.index(('wait_capture', True)),
                        self.events.index(('stop_owned', True)))
        self.assertTrue(self.popen.call_args.kwargs['stdout'].closed)
        self.assert_recovered_under_guard()

    def assert_capture_stop_escalation(self, timeouts):
        _, capture = self.configure_automatic_capture(
            exit_code=None, complete=False, finish_worker=False)
        order = []

        def wait_capture(timeout=None):
            self.assertTrue(self.locked)
            self.assertTrue((self.out / 'capture-cancel.json').exists())
            order.append(('wait', timeout))
            if capture.wait.call_count <= timeouts:
                raise subprocess.TimeoutExpired('synthetic capture', timeout)
            capture.returncode = -1
            self.events.append(('capture_stopped', self.locked))
            return capture.returncode

        def stop_capture(action):
            self.assertTrue(self.locked)
            self.assertTrue((self.out / 'capture-cancel.json').exists())
            order.append((action, None))

        capture.wait.side_effect = wait_capture
        capture.terminate.side_effect = lambda: stop_capture('terminate')
        capture.kill.side_effect = lambda: stop_capture('kill')
        final = self.run_and_read()
        self.assertEqual(final['result'], 'failed')
        self.assertEqual(final['parent_problem'], 'run_timeout')
        expected = [('wait', 5), ('terminate', None), ('wait', 5)]
        if timeouts == 2:
            expected += [('kill', None), ('wait', 5)]
        self.assertEqual(order, expected)
        self.assertLess(self.events.index(('capture_stopped', True)),
                        self.events.index(('stop_worker', True)))
        self.assertLess(self.events.index(('capture_stopped', True)),
                        self.events.index(('stop_owned', True)))
        self.assertTrue(self.popen.call_args.kwargs['stdout'].closed)
        self.assert_recovered_under_guard()

    def test_capture_cancel_timeout_terminates_then_waits_before_recovery(self):
        self.assert_capture_stop_escalation(timeouts=1)

    def test_capture_terminate_timeout_kills_then_waits_before_recovery(self):
        self.assert_capture_stop_escalation(timeouts=2)

    def assert_failed_capture_stop_holds_protection(self, stage, failed_dump=None):
        _, capture = self.configure_automatic_capture(
            exit_code=None, complete=False, finish_worker=False)

        def failed_wait(timeout=None):
            if stage == 'initial_wait':
                raise OSError('synthetic capture wait failure')
            if stage == 'terminate_wait' and capture.wait.call_count == 2:
                raise OSError('synthetic capture wait failure')
            raise subprocess.TimeoutExpired('synthetic capture', timeout)

        capture.wait.side_effect = failed_wait
        if stage == 'terminate':
            capture.terminate.side_effect = PermissionError('synthetic capture terminate failure')
        if stage == 'kill':
            capture.kill.side_effect = PermissionError('synthetic capture kill failure')
        waiting = 0
        required_write_attempted = False
        real_dump = self.runner.dump

        def dump_with_optional_failure(path, value):
            nonlocal required_write_attempted
            name = Path(path).name
            if name == 'capture-intervention-required.json':
                required_write_attempted = True
            if name == failed_dump:
                raise OSError('synthetic diagnostic write failure')
            return real_dump(path, value)

        dump_patch = mock.patch.object(self.runner, 'dump', side_effect=dump_with_optional_failure)
        dump_patch.start()
        self.addCleanup(dump_patch.stop)

        def manual_capture_exit(_seconds):
            nonlocal waiting
            required = self.out / 'capture-intervention-required.json'
            if not required_write_attempted:
                self.assertFalse((self.out / 'worker-intervention-required.json').exists(),
                                 'Worker recovery started before capture exit was confirmed')
                return
            waiting += 1
            self.assertTrue(self.locked, 'File protection ended with capture still alive')
            self.assertIsNone(capture.returncode)
            self.child.terminate.assert_not_called()
            self.desktop.terminate.assert_not_called()
            self.assertFalse((self.out / 'recovery.json').exists())
            self.assertFalse((self.out / 'guard-complete.json').exists())
            self.assertFalse(any(isinstance(event, tuple) and event[0] == 'guard_exit'
                                 for event in self.events))
            if failed_dump != 'capture-stop-error.json':
                error = json.loads((self.out / 'capture-stop-error.json').read_text())
                self.assertEqual(error['capture_pid'], capture.pid)
                self.assertTrue(error['errors'])
            if failed_dump != 'capture-intervention-required.json':
                intervention = json.loads(required.read_text())
                self.assertEqual(intervention['capture_pid'], capture.pid)
                self.assertEqual(intervention['state'], 'waiting_with_original_file_locks_held')
            if waiting == 2:
                self.events.append(('capture_manual_exit', self.locked))
                capture.returncode = -1

        self.runner.time.sleep.side_effect = manual_capture_exit
        final = self.run_and_read()
        self.assertEqual(final['result'], 'failed')
        self.assertEqual(final['parent_problem'], 'capture_stop_failed')
        self.assertFalse(self.runner.should_retry(final, self.cfg['scenario']))
        self.assertEqual(waiting, 2)
        if failed_dump:
            self.assertFalse((self.out / failed_dump).exists())
        else:
            self.assertTrue((self.out / 'capture-stop-error.json').exists())
            self.assertTrue((self.out / 'capture-intervention-required.json').exists())
        self.assertLessEqual(capture.terminate.call_count, 1)
        self.assertLessEqual(capture.kill.call_count, 1)
        self.assertLessEqual(capture.wait.call_count, 3)
        self.assertTrue(all(call.kwargs == dict(timeout=5) for call in capture.wait.call_args_list))
        self.assertTrue(json.loads((self.out / 'capture-intervention-resolved.json').read_text())
                        ['capture_process_exited'])
        self.assertLess(self.events.index(('capture_manual_exit', True)),
                        self.events.index(('stop_worker', True)))
        self.assertLess(self.events.index(('capture_manual_exit', True)),
                        self.events.index(('stop_owned', True)))
        self.assertTrue(self.popen.call_args.kwargs['stdout'].closed)
        self.assert_recovered_under_guard()

    def test_capture_initial_wait_error_holds_guard_until_capture_exits(self):
        self.assert_failed_capture_stop_holds_protection('initial_wait')

    def test_capture_terminate_denial_holds_guard_until_capture_exits(self):
        self.assert_failed_capture_stop_holds_protection('terminate')

    def test_capture_terminate_wait_error_holds_guard_until_capture_exits(self):
        self.assert_failed_capture_stop_holds_protection('terminate_wait')

    def test_capture_kill_denial_holds_guard_until_capture_exits(self):
        self.assert_failed_capture_stop_holds_protection('kill')

    def test_capture_final_wait_timeout_holds_guard_until_capture_exits(self):
        self.assert_failed_capture_stop_holds_protection('final_wait')

    def test_capture_stop_error_receipt_write_failure_still_waits_before_recovery(self):
        self.assert_failed_capture_stop_holds_protection('terminate', 'capture-stop-error.json')

    def test_capture_intervention_receipt_write_failure_still_waits_before_recovery(self):
        self.assert_failed_capture_stop_holds_protection('terminate', 'capture-intervention-required.json')


if __name__ == "__main__":
    unittest.main()
