import json
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch


OFFLINE = Path(__file__).resolve().parents[1] / "tools" / "offline"
sys.path.insert(0, str(OFFLINE))
import output_lifecycle


class OutputLifecycleTests(unittest.TestCase):
    def setUp(self):
        environment = patch.dict(output_lifecycle.os.environ, {"NR_NEXUS_TEST_KEEP_RAW": "0",
                                                              "NR_NEXUS_TEST_BUDGET_BYTES": "0"})
        environment.start()
        self.addCleanup(environment.stop)

    def test_owner_query_does_not_terminate_an_active_process(self):
        process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(10)"])
        try:
            self.assertTrue(output_lifecycle._owner_active(process.pid))
            self.assertIsNone(process.poll())
        finally:
            process.terminate()
            process.wait(timeout=5)

    def test_every_retention_mode_cleans_registered_raw_data(self):
        for mode in ("Standard", "Full", "Acceptance"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temp:
                previous_mode = output_lifecycle.os.environ.get("NR_NEXUS_TEST_RETENTION")
                output_lifecycle.os.environ["NR_NEXUS_TEST_RETENTION"] = mode
                try:
                    run = Path(temp) / "run"
                    output_lifecycle.initialize(run)
                    raw = run / "backend" / "frame.f32"
                    raw.parent.mkdir()
                    raw.write_bytes(b"raw")
                    output_lifecycle.register_disposable(run, [raw])
                    output_lifecycle.finish(run, "Passed")
                    self.assertFalse(raw.exists())
                    self.assertEqual(json.loads((run / "artifact-lifecycle.json").read_text())["status"], "Passed")
                finally:
                    if previous_mode is None:
                        output_lifecycle.os.environ.pop("NR_NEXUS_TEST_RETENTION", None)
                    else:
                        output_lifecycle.os.environ["NR_NEXUS_TEST_RETENTION"] = previous_mode

    def test_maintenance_protects_active_and_cleans_orphaned_runs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            active = root / "active"
            output_lifecycle.initialize(active)
            active_raw = active / "frame.f32"
            active_raw.write_bytes(b"active")
            output_lifecycle.register_disposable(active, [active_raw])

            orphaned = root / "orphaned"
            output_lifecycle.initialize(orphaned)
            orphan_raw = orphaned / "frame.f32"
            orphan_raw.write_bytes(b"orphan")
            output_lifecycle.register_disposable(orphaned, [orphan_raw])
            marker = json.loads((orphaned / "artifact-lifecycle.json").read_text())
            marker["ownerPid"] = 2**31 - 1
            (orphaned / "artifact-lifecycle.json").write_text(json.dumps(marker))

            output_lifecycle.maintain(root)
            self.assertTrue(active_raw.exists())
            self.assertEqual(json.loads((active / "artifact-lifecycle.json").read_text())["status"], "Running")
            self.assertFalse(orphan_raw.exists())
            self.assertEqual(json.loads((orphaned / "artifact-lifecycle.json").read_text())["status"], "Failed")
            child = active / "child"
            output_lifecycle.initialize(child)
            child_raw = child / "still-used.f32"
            child_raw.write_bytes(b"used by active parent")
            output_lifecycle.register_disposable(child, [child_raw])
            state = output_lifecycle._read(child)
            state["ownerPid"] = 2**31 - 1
            output_lifecycle._write(child, state)
            output_lifecycle.maintain(active, limit=1)
            self.assertTrue(child_raw.exists())

    def test_explicit_raw_retention_keeps_registered_data(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with patch.dict(output_lifecycle.os.environ, {"NR_NEXUS_TEST_KEEP_RAW": "1",
                                                        "NR_NEXUS_TEST_RETENTION": "Acceptance"}):
                run = root / "run"
                output_lifecycle.initialize(run)
                raw = run / "frame.f32"
                raw.write_bytes(b"raw")
                output_lifecycle.register_disposable(run, [raw])
                output_lifecycle.finish(run, "Failed")
                self.assertTrue(raw.exists())

    def test_budget_compacts_bulk_in_every_mode_and_protects_evidence(self):
        for mode in ("Standard", "Full", "Acceptance"):
            for status in ("Passed", "Failed"):
                with self.subTest(mode=mode, status=status), tempfile.TemporaryDirectory() as temp:
                    root = Path(temp)
                    with patch.dict(output_lifecycle.os.environ, {"NR_NEXUS_TEST_RETENTION": mode}):
                        run = root / "ended"
                        output_lifecycle.initialize(run)
                    # These files were never registered as disposables. An empty
                    # inventory must not make the capacity budget ineffective.
                    (run / "screenshot.png").write_bytes(b"x" * (1024 * 1024))
                    (run / "repro.json").write_text('{"seed":7}')
                    (run / "critical.log").write_text("diagnostic")
                    (run / "failure.f32").write_bytes(b"sample")
                    output_lifecycle.retain(run, [run / "failure.f32"])
                    (run / "inputs").mkdir()
                    (run / "inputs" / "frame.f32").write_bytes(b"input")
                    output_lifecycle.finish(run, status)
                    state = output_lifecycle._read(run)
                    state["ownerPid"] = 2**31 - 1
                    output_lifecycle._write(run, state)
                    # Also exercise backward compatibility with existing markers.
                    if mode == "Acceptance":
                        state.pop("keepRawArtifacts")
                        output_lifecycle._write(run, state)
                    before = output_lifecycle._tree_bytes(root)
                    output_lifecycle.maintain(root, limit=16384)
                    self.assertLess(output_lifecycle._tree_bytes(root), before - 1000000)
                    self.assertFalse((run / "screenshot.png").exists())
                    for name in ("repro.json", "critical.log", "failure.f32", "inputs/frame.f32"):
                        self.assertTrue((run / name).exists(), name)
                    self.assertEqual(output_lifecycle._read(run)["status"], status)

    def test_budget_protects_active_descendants_pins_and_build_inputs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            parent = root / "parent"
            output_lifecycle.initialize(parent)
            output_lifecycle.finish(parent, "Passed")
            state = output_lifecycle._read(parent)
            state["ownerPid"] = 2**31 - 1
            output_lifecycle._write(parent, state)
            child = parent / "active"
            output_lifecycle.initialize(child)
            (child / "raw.f32").write_bytes(b"active")
            (parent / "image.png").write_bytes(b"parent")
            with patch.dict(output_lifecycle.os.environ, {"NR_NEXUS_TEST_KEEP_RAW": "1"}):
                pinned = root / "pinned"
                output_lifecycle.initialize(pinned)
                (pinned / "raw.f32").write_bytes(b"pinned")
                output_lifecycle.finish(pinned, "Failed")
            state = output_lifecycle._read(pinned)
            state["ownerPid"] = 2**31 - 1
            output_lifecycle._write(pinned, state)
            build = root / "build"
            output_lifecycle.initialize(build)
            (build / "build-manifest.json").write_text("{}")
            (build / "backend.exe").write_bytes(b"input executable")
            output_lifecycle.finish(build, "Passed")
            state = output_lifecycle._read(build)
            state["ownerPid"] = 2**31 - 1
            output_lifecycle._write(build, state)
            output_lifecycle.maintain(root, limit=1)
            for path in (child / "raw.f32", parent / "image.png", pinned / "raw.f32", build / "backend.exe"):
                self.assertTrue(path.exists(), path)
            self.assertEqual(output_lifecycle._read(child)["status"], "Running")

    def test_budget_keeps_failure_capture_with_shared_atlas(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            run = root / "ui"
            output_lifecycle.initialize(run)
            menu = run / "menu"
            menu.mkdir()
            for name, content in {"captures.json": "{}", "normal.json": '{"atlas_file":"atlas.rgba"}',
                                  "failure.json": '{"atlas_file":"atlas.rgba"}',
                                  "atlas.rgba": "shared failure atlas", "unused.rgba": "unused"}.items():
                (menu / name).write_text(content)
            output_lifecycle.finish(run, "Failed")
            state = output_lifecycle._read(run)
            state["ownerPid"] = 2**31 - 1
            output_lifecycle._write(run, state)
            output_lifecycle.maintain(root, limit=1)
            self.assertTrue((menu / "failure.json").exists())
            self.assertTrue((menu / "atlas.rgba").exists())
            self.assertFalse((menu / "normal.json").exists())
            self.assertFalse((menu / "unused.rgba").exists())

    def test_workspace_orphan_cleanup_does_not_require_a_capacity_limit(self):
        for limit in ('0', '100000'):
            with self.subTest(limit=limit), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                run = root / 'artifacts/run'
                output_lifecycle.initialize(run)
                raw = run / 'orphan.f32'
                raw.write_bytes(b'orphaned')
                output_lifecycle.register_disposable(run, [raw])
                state = output_lifecycle._read(run)
                state['ownerPid'] = 2**31 - 1
                output_lifecycle._write(run, state)
                with patch.dict(output_lifecycle.os.environ, {'NR_NEXUS_TEST_BUDGET_BYTES': limit}):
                    output_lifecycle.maintain_workspace(root)
                self.assertFalse(raw.exists())
                self.assertEqual(output_lifecycle._read(run)['status'], 'Failed')

    def test_workspace_budget_is_shared_across_tmp_and_artifacts(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in ('tmp', 'artifacts'):
                run = root / name / 'run'
                output_lifecycle.initialize(run)
                (run / 'bulk.png').write_bytes(b'x' * 65536)
                (run / 'backup').mkdir()
                (run / 'backup/runtime.dll').write_bytes(b'recovery')
                output_lifecycle.finish(run, 'Passed')
                state = output_lifecycle._read(run)
                state['ownerPid'] = 2**31 - 1
                output_lifecycle._write(run, state)
            with patch.dict(output_lifecycle.os.environ, {'NR_NEXUS_TEST_BUDGET_BYTES': '100000'}):
                result = output_lifecycle.maintain_workspace(root)
            self.assertLessEqual(result['bytes'], 100000)
            self.assertTrue((root / 'tmp/run/backup/runtime.dll').exists())
            self.assertTrue((root / 'artifacts/run/backup/runtime.dll').exists())


if __name__ == "__main__":
    unittest.main()
