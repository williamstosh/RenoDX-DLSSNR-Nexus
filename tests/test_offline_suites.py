import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "offline" / "suites.py"
spec = importlib.util.spec_from_file_location("offline_suites", MODULE_PATH)
suites = importlib.util.module_from_spec(spec)
spec.loader.exec_module(suites)


def fixture(*cases):
    return {"schema": suites.FIXTURE_SCHEMA, "fixture_id": "unit-fixture", "cases": list(cases)}


class OfflineSuitesFixtureTests(unittest.TestCase):
    def test_source_identity_preflight_failure_does_not_leave_output_directory(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "tmp").mkdir()
            fixture_path = root / "fixture.json"
            fixture_path.write_text(json.dumps(fixture(
                {"id": "life", "suite": "lifecycle", "case": "dll-lifecycle"})))
            output = root / "tmp" / "identity-failed"
            with patch.object(suites, "ROOT", root), \
                 patch.object(suites, "_source_identity", side_effect=OSError("missing source")):
                with self.assertRaisesRegex(OSError, "missing source"):
                    suites.run(fixture_path, output, "pwsh-test")
            self.assertFalse(output.exists())

    def test_accepts_requested_evidence_boundaries(self):
        checked = suites.validate_fixture(fixture(
            {"id": "native", "suite": "fg-native", "case": "feature-switch-hardware"},
            {"id": "warp", "suite": "fg-native", "case": "present-stage-resolution"},
            {"id": "xefg", "suite": "fg-xefg", "case": "resource-lifecycle"},
            {"id": "lifecycle", "suite": "lifecycle", "case": "dll-lifecycle"},
        ))
        records = suites._case_records(checked, Path("out"), True)
        self.assertEqual(records[0]["evidence"], ["NATIVE_GPU", "MOCK"])
        self.assertEqual(records[1]["evidence"], ["WARP", "MOCK"])
        self.assertEqual(records[2]["evidence"], ["WARP", "MOCK"])
        self.assertEqual(records[3]["evidence"], ["MOCK"])

    def test_rejects_unknown_fields_invalid_native_case_and_fake_native_claim(self):
        invalid = [
            {**fixture({"id": "x", "suite": "fg-native", "case": "all"}), "extra": True},
            fixture({"id": "x", "suite": "fg-native", "case": "native-full"}),
            fixture({"id": "x", "suite": "fg-xefg", "case": "native-sdk"}),
            fixture({"id": "x", "suite": "fg-native", "case": "feature-switch", "evidence": "NATIVE_NGX"}),
        ]
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(suites.FixtureError):
                suites.validate_fixture(value)

    def test_rejects_duplicate_cases_and_unbounded_timeout(self):
        duplicate = fixture({"id": "a", "suite": "fg-xefg", "case": "resource-lifecycle"},
                            {"id": "b", "suite": "fg-xefg", "case": "resource-lifecycle"})
        long_timeout = {**fixture({"id": "a", "suite": "lifecycle", "case": "dll-lifecycle"}),
                        "timeout_ms": suites.MAX_TIMEOUT_MS + 1}
        for value in (duplicate, long_timeout):
            with self.subTest(value=value), self.assertRaises(suites.FixtureError):
                suites.validate_fixture(value)

    def test_builds_one_allowlisted_existing_entry_call(self):
        checked = suites.validate_fixture(fixture(
            {"id": "pair", "suite": "fg-native", "case": "same-stage-model-pair"},
            {"id": "xe", "suite": "fg-xefg", "case": "resource-lifecycle"},
            {"id": "life", "suite": "lifecycle", "case": "dll-lifecycle"},
        ))
        command = suites.build_command(checked, Path("tmp/offline/run"), "pwsh-test")
        self.assertEqual(command[-2], "-Command")
        script = command[-1]
        self.assertIn("-Suite @('fg-native','fg-xefg','lifecycle')", script)
        self.assertIn("-NativeCase @('same-stage-model-pair')", script)
        self.assertIn("-UiPreview 'None'", script)

    def test_case_status_requires_suite_and_case_evidence(self):
        checked = suites.validate_fixture(fixture(
            {"id": "native", "suite": "fg-native", "case": "feature-switch-hardware"},
            {"id": "xefg", "suite": "fg-xefg", "case": "resource-lifecycle"},
            {"id": "lifecycle", "suite": "lifecycle", "case": "dll-lifecycle"},
        ))
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            run_output = output / suites.RUN_OUTPUT_NAME
            for directory in ("fg-native", "fg-xefg"):
                (run_output / directory).mkdir(parents=True)
            (run_output / "fg-native" / "feature-switch-hardware.stdout.log").write_text("passed")
            (run_output / "fg-native" / "result.json").write_text(
                json.dumps({"suite": "fg-native", "build": "passed", "run": "passed"}))
            (run_output / "fg-xefg" / "test.stdout.log").write_text("passed")
            (run_output / "fg-xefg" / "result.json").write_text(
                json.dumps({"suite": "fg-xefg", "build": "passed", "run": "passed"}))
            (run_output / "lifecycle").mkdir()
            (run_output / "lifecycle" / "host-exit.json").write_text(json.dumps({"exit_code": 0}))
            records = suites._case_records(checked, output, True)
            self.assertEqual([record["status"] for record in records], ["passed"] * 3)
            self.assertEqual(records[0]["evidence"], ["NATIVE_GPU", "MOCK"])
            self.assertEqual(records[1]["evidence"], ["WARP", "MOCK"])
            (run_output / "fg-xefg" / "result.json").write_text(
                json.dumps({"suite": "fg-xefg", "build": "passed", "run": "not_run"}))
            records = suites._case_records(checked, output, True)
            self.assertEqual(records[1]["status"], "failed")
            self.assertIn("missing-or-failed-suite-result", records[1]["failure_reasons"])

    def test_timeout_retains_logs_and_reports_tree_termination(self):
        class TimedOutProcess:
            pid = 123
            stdout = None
            stderr = None
            returncode = None
            calls = 0

            def communicate(self, timeout=None):
                self.calls += 1
                if self.calls == 1:
                    raise suites.subprocess.TimeoutExpired("pwsh-test", timeout, output=b"partial out", stderr=b"partial err")
                self.returncode = -9
                return "partial out", "partial err"

            def poll(self):
                return self.returncode

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "tmp").mkdir()
            fixture_path = root / "fixture.json"
            fixture_path.write_text(json.dumps(fixture(
                {"id": "life", "suite": "lifecycle", "case": "dll-lifecycle"})))
            process = TimedOutProcess()
            identity = {"files": [], "sha256": "same"}
            with patch.object(suites, "ROOT", root), patch.object(suites, "_source_identity", return_value=identity), \
                 patch.object(suites.subprocess, "Popen", return_value=process), \
                 patch.object(suites, "_WindowsProcessJob", side_effect=OSError("job assignment unavailable")), \
                 patch.object(suites, "_terminate_process_tree", return_value={"tree_terminated": True, "method": "test-tree-kill"}):
                result = suites.run(fixture_path, root / "tmp" / "timed-out", "pwsh-test")
            output = root / result["output_directory"]
            self.assertEqual(result["status"], "failed")
            self.assertTrue(result["runner"]["timed_out"])
            self.assertEqual(result["runner"]["termination"]["method"], "test-tree-kill")
            self.assertIn("partial out", (output / "runner.stdout.log").read_text())
            self.assertTrue((output / "result.json").is_file())


if __name__ == "__main__":
    unittest.main()
