import json
from pathlib import Path
import sys
import tempfile
import unittest


OFFLINE = Path(__file__).resolve().parents[1] / "tools" / "offline"
sys.path.insert(0, str(OFFLINE))
import output_lifecycle


class OutputLifecycleTests(unittest.TestCase):
    def test_standard_finish_removes_registered_raw_data(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            run = root / "run"
            output_lifecycle.initialize(run)
            raw = run / "backend" / "frame.f32"
            raw.parent.mkdir()
            raw.write_bytes(b"raw")
            output_lifecycle.register_disposable(run, [raw])
            output_lifecycle.finish(run, "Passed")
            self.assertFalse(raw.exists())
            self.assertEqual(json.loads((run / "artifact-lifecycle.json").read_text())["status"], "Passed")

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

    def test_full_retention_keeps_registered_raw_data(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            previous = output_lifecycle.os.environ.get("NR_NEXUS_TEST_RETENTION")
            output_lifecycle.os.environ["NR_NEXUS_TEST_RETENTION"] = "Full"
            try:
                run = root / "run"
                output_lifecycle.initialize(run)
                raw = run / "frame.f32"
                raw.write_bytes(b"raw")
                output_lifecycle.register_disposable(run, [raw])
                output_lifecycle.finish(run, "Failed")
                self.assertTrue(raw.exists())
            finally:
                if previous is None:
                    output_lifecycle.os.environ.pop("NR_NEXUS_TEST_RETENTION", None)
                else:
                    output_lifecycle.os.environ["NR_NEXUS_TEST_RETENTION"] = previous


if __name__ == "__main__":
    unittest.main()
