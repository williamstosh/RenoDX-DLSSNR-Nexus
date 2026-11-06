import copy
from contextlib import redirect_stdout
import io
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

OFFLINE = Path(__file__).resolve().parents[1] / "tools" / "offline"
sys.path.insert(0, str(OFFLINE))
import contracts as c

spec = importlib.util.spec_from_file_location("offline_replay_runner", OFFLINE / "run.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class OfflineReplayContractsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.sequence = c.prepare_sequence(self.source, 3, 2, 2, 7)
        self.output = self.root / "backend"
        self.output.mkdir()
        self.backend = {"schema": "nr-nexus-offline-replay-backend/1", "status": "passed", "adapter": "warp",
            "environment": {"adapter_flags": 2, "ngx_loaded": False, "network_evaluated": False}, "frames": []}
        for frame in self.sequence["frames"]:
            resources = {}
            for name, resource in frame["resources"].items():
                (self.output / resource["file"]).write_bytes((self.source / resource["file"]).read_bytes())
                resources[name] = {"output_file": resource["file"], "format": resource["format"],
                                   "bytes": resource["bytes"], "bitwise_equal": True}
            self.backend["frames"].append({"index": frame["index"], "width": frame.get("width", self.sequence["width"]),
                "height": frame.get("height", self.sequence["height"]), "evidence": "WARP", "resources": resources,
                "completion": {"gpu_completed": True, "fence_value": frame["index"] + 1},
                "controls": {"evidence": "STATIC", "history_evaluated": False,
                             **{key: frame[key] for key in ("reset", "jitter", "motion_scale", "subrect")}}})

    def test_generic_cli_lists_all_portable_commands(self):
        output = io.StringIO()
        with patch.object(sys, "argv", [str(OFFLINE / "run.py"), "--help"]), redirect_stdout(output):
            with self.assertRaises(SystemExit) as raised:
                runner.main()
        self.assertEqual(raised.exception.code, 0)
        help_text = output.getvalue()
        self.assertIn("validate", help_text)
        self.assertIn("matrix", help_text)
        self.assertIn("prepare", help_text)
        self.assertIn("verify-sequence", help_text)
        self.assertIn("compare", help_text)
        self.assertIn("run", help_text)
        self.assertIn("replay", help_text)

    def test_independent_raw_f32_identity_and_separate_control_evidence(self):
        results = runner.verify_replay_result(self.sequence, self.source, self.backend, self.output, "warp")
        self.assertEqual(len(results), 2)
        self.assertEqual([check["evidence"] for check in results[0]["checks"]], ["WARP", "WARP", "WARP", "STATIC"])
        self.assertTrue(all(check["comparison"]["identical"] for check in results[0]["checks"][:3]))

    def test_output_corruption_cannot_be_hidden_by_backend_pass(self):
        resource = self.backend["frames"][0]["resources"]["color"]
        path = self.output / resource["output_file"]
        path.write_bytes(b"\0" * path.stat().st_size)
        with self.assertRaises(c.ContractError):
            runner.verify_replay_result(self.sequence, self.source, self.backend, self.output, "warp")

    def test_control_metadata_is_not_history_execution(self):
        self.backend["frames"][0]["controls"]["history_evaluated"] = True
        with self.assertRaises(c.ContractError):
            runner.verify_replay_result(self.sequence, self.source, self.backend, self.output, "warp")

    def test_partial_frame_and_replayed_fence_fail(self):
        for change in ("partial", "fence"):
            value = copy.deepcopy(self.backend)
            if change == "partial":
                value["frames"][1]["resources"].pop("motion")
            else:
                value["frames"][1]["completion"]["fence_value"] = 1
            with self.subTest(change=change), self.assertRaises(c.ContractError):
                runner.verify_replay_result(self.sequence, self.source, value, self.output, "warp")

    def test_bad_source_is_rejected_before_starting_any_backend(self):
        (self.source / "0000-color.f32").write_bytes(b"bad")
        run = self.root / "run"
        run.mkdir()
        with patch.object(runner.subprocess, "run") as start:
            report = runner.run_replay(self.source, self.root / "unused.exe", run, "warp", 1)
            start.assert_not_called()
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["cases"], [])

    def test_non_object_sequence_json_retains_controlled_failed_result(self):
        for malformed in ("[]", "null"):
            with self.subTest(manifest=malformed):
                (self.source / "sequence.json").write_text(malformed, encoding="utf-8")
                run = self.root / ("run-" + str(len(malformed)))
                run.mkdir()
                with patch.object(runner.subprocess, "run") as start:
                    report = runner.run_replay(self.source, self.root / "unused.exe", run, "warp", 1)
                    start.assert_not_called()
                self.assertEqual(report["status"], "failed")
                self.assertEqual(report["failure_kind"], "validation-or-backend")
                self.assertEqual(c.read_json(run / "result.json")["status"], "failed")
                self.assertIn("JSON object", report["error"])


if __name__ == "__main__":
    unittest.main()
