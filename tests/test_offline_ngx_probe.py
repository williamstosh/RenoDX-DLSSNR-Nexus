"""Real executable boundary tests; no simulated NGX success or Runtime copies."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
EXECUTABLE = os.environ.get("NR_NEXUS_NGX_PROBE_EXE")
RUNTIME = os.environ.get("NR_NEXUS_NGX_RUNTIME_DIR")


@unittest.skipUnless(EXECUTABLE and Path(EXECUTABLE).is_file(),
                     "Set NR_NEXUS_NGX_PROBE_EXE to an already built real probe")
class OfflineNgxProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary_root = ROOT / "tmp" / "offline"
        temporary_root.mkdir(parents=True, exist_ok=True)
        cls.artifacts = Path(tempfile.mkdtemp(prefix="offline_ngx_test_", dir=temporary_root))
        print(f"NGX probe regression artifacts: {cls.artifacts}", flush=True)

    def setUp(self):
        self.root = self.artifacts / self._testMethodName
        self.root.mkdir()
        self.runtime = self.root / "runtime"
        self.runtime.mkdir()
        self.output = self.root / "output"

    def run_probe(self, *extra, runtime=None, output=None):
        return subprocess.run(
            [EXECUTABLE, "--runtime-dir", str(runtime or self.runtime),
             "--output", str(output or self.output), *extra],
            capture_output=True, text=True, timeout=30, check=False,
        )

    def read_report(self):
        return json.loads((self.output / "ngx-probe-result.json").read_text(encoding="utf-8"))

    def assert_no_completion_claim(self, result):
        self.assertEqual(result["schema"], "nr-nexus-offline-ngx-probe/1")
        self.assertEqual(result["status"], "blocked")
        for field in ("created", "evaluated", "gpu_completed"):
            self.assertIs(result[field], False)
        self.assertEqual(result["feature"]["sdk_name"], "Reserved18")
        for stage in ("inventory", "device", "exports", "initialize", "capability",
                      "create", "evaluate", "gpu_completion", "cleanup"):
            self.assertIn("status", result["stages"][stage])
            self.assertIn("reason", result["stages"][stage])
            self.assertIn("detail", result["stages"][stage])
        serialized = json.dumps(result)
        self.assertNotIn(str(self.root), serialized)
        self.assertNotIn(str(self.root).replace("\\", "/"), serialized)

    def test_missing_arguments(self):
        completed = subprocess.run([EXECUTABLE], capture_output=True, timeout=5, check=False)
        self.assertEqual(completed.returncode, 1)

    def test_missing_runtime_does_not_create_output(self):
        completed = self.run_probe(runtime=self.root / "absent")
        self.assertEqual(completed.returncode, 1)
        self.assertFalse(self.output.exists())

    def test_nonempty_output_is_untouched(self):
        self.output.mkdir()
        sentinel = self.output / "sentinel.txt"
        sentinel.write_text("keep", encoding="utf-8")
        self.assertEqual(self.run_probe().returncode, 1)
        self.assertEqual(sentinel.read_text(encoding="utf-8"), "keep")
        self.assertEqual(list(self.output.iterdir()), [sentinel])

    def test_output_cannot_be_runtime_or_its_child(self):
        for output in (self.runtime, self.runtime / "output"):
            with self.subTest(output=output.name):
                self.assertEqual(self.run_probe(output=output).returncode, 1)
        self.assertEqual(list(self.runtime.iterdir()), [])

    def test_invalid_timeouts_do_not_create_output(self):
        for timeout in ("0", "99", "60001", "-1", "100ms"):
            with self.subTest(timeout=timeout):
                self.assertEqual(self.run_probe("--timeout-ms", timeout).returncode, 1)
                self.assertFalse(self.output.exists())

    def test_duplicate_runtime_is_rejected(self):
        self.assertEqual(self.run_probe("--runtime-dir", str(self.runtime)).returncode, 1)
        self.assertFalse(self.output.exists())

    def test_directory_arguments_accept_trailing_separator(self):
        self.assertEqual(self.run_probe(runtime=str(self.runtime) + os.sep,
                                        output=str(self.output) + os.sep).returncode, 2)
        self.assert_no_completion_claim(self.read_report())

    def test_empty_runtime_is_blocked_with_honest_stage_evidence(self):
        self.assertEqual(self.run_probe().returncode, 2)
        result = self.read_report()
        self.assert_no_completion_claim(result)
        self.assertEqual(result["runtime"], [])
        self.assertEqual(result["stages"]["exports"]["status"], "blocked")
        self.assertEqual(result["stages"]["initialize"]["status"], "not_attempted")
        self.assertEqual(result["stages"]["gpu_completion"]["status"], "not_attempted")
        original = (self.output / "ngx-probe-result.json").read_bytes()
        self.assertEqual(self.run_probe().returncode, 1)
        self.assertEqual((self.output / "ngx-probe-result.json").read_bytes(), original)

    def test_invalid_pe_has_mapping_failure_and_no_api_call(self):
        invalid = self.runtime / "nvngx_dlssnr.dll"
        invalid.write_bytes(b"not a PE image")
        self.assertEqual(self.run_probe().returncode, 2)
        result = self.read_report()
        self.assert_no_completion_claim(result)
        identity = result["runtime"][0]
        self.assertFalse(identity["image_mapped"])
        self.assertFalse(identity["code_executed"])
        self.assertNotEqual(identity["mapping_win32_error"], 0)
        self.assertEqual(identity["sha256"], hashlib.sha256(invalid.read_bytes()).hexdigest())
        self.assertTrue(all(value is False for value in identity["exports"].values()))

    @unittest.skipUnless(RUNTIME, "No explicit existing local Runtime directory supplied")
    def test_existing_runtime_identity_and_export_boundary(self):
        runtime = Path(RUNTIME).resolve(strict=True)
        dll = runtime / "nvngx_dlssnr.dll"
        with dll.open("rb") as stream:
            before = hashlib.file_digest(stream, "sha256").hexdigest()
        self.assertEqual(self.run_probe(runtime=runtime).returncode, 2)
        result = self.read_report()
        self.assert_no_completion_claim(result)
        identity = next(item for item in result["runtime"] if item["basename"] == dll.name)
        self.assertEqual(identity["sha256"], before)
        self.assertTrue(identity["image_mapped"])
        self.assertFalse(identity["code_executed"])
        self.assertTrue(identity["exports"]["NVSDK_NGX_D3D12_Init_Ext"])
        with dll.open("rb") as stream:
            self.assertEqual(before, hashlib.file_digest(stream, "sha256").hexdigest())
        self.assertNotIn(str(runtime), json.dumps(result))

    @unittest.skipUnless(RUNTIME, "No explicit existing local Runtime directory supplied")
    def test_existing_runtime_short_deadline_is_bounded(self):
        completed = self.run_probe(runtime=Path(RUNTIME), *["--timeout-ms", "100"])
        self.assertEqual(completed.returncode, 2)
        result = self.read_report()
        self.assert_no_completion_claim(result)
        self.assertLess(result["worker_elapsed_ms"], 10000)
        if result.get("worker_forcibly_terminated"):
            self.assertEqual(result["blocker"], "worker_timeout")
            self.assertEqual(result["worker_exit_code"], 124)


if __name__ == "__main__":
    unittest.main()
