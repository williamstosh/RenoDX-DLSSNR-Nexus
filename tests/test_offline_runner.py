import copy
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

OFFLINE = Path(__file__).resolve().parents[1] / "tools" / "offline"
sys.path.insert(0, str(OFFLINE))
import contracts as c

spec = importlib.util.spec_from_file_location("offline_runner", OFFLINE / "run.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class OfflineRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.fixture = c.validate_fixture({"schema": c.FIXTURE_SCHEMA, "fixture_id": "runner-unit",
            "cases": [{"id": "one", "width": 2, "height": 2, "frames": 3, "warmup": 1}]})
        (self.directory / "one.rgba8").write_bytes(c.mask_reference(self.fixture["cases"][0]))
        self.backend = {"schema": "nr-nexus-offline-backend/1", "backend": "d3d12-control-mask",
            "execution_mode": "validation", "cpu_timing": {"clock": "std::chrono::steady_clock",
                "device_queue_init_ms": 1.0, "pipeline_init_ms": 1.0,
                "all_case_resource_and_execution_ms": 2.0},
            "adapter": "warp", "environment": {"unit_test_only": True, "adapter_flags": 2,
                "timestamp_frequency_hz": 1000,
                "timestamp_frequency_scope": "D3D12 command-queue timestamp ticks per second; not GPU core clock",
                "ngx_loaded": False, "network_evaluated": False}, "cases": [{
                "id": "one", "status": "passed", "evidence": "WARP", "width": 2, "height": 2,
                "format": "R8G8B8A8_UNORM", "pixels_file": "one.rgba8",
                "pixel_checks": 48, "validated_frames": 3,
                "allocation": {"texture_bytes_estimate": 512, "timestamp_query_count": 6},
                "cpu_timing": {"clock": "std::chrono::steady_clock", "resource_setup_ms": 1.0,
                    "command_recording_ms": 1.0, "case_wall_ms": 2.0},
                "parameter_contract": {"evidence": "MOCK", "model_request": "A", "auto_mask": False, "blend_mode": 0},
                "completion": {"gpu_completed": True, "fence_value": 3},
                "timing": {"scope": "control-mask-dispatch", "frequency_hz": 1000,
                    "includes_uav_barrier": True, "source": "D3D12_QUERY_TYPE_TIMESTAMP",
                    "execution_mode": "per-frame-readback", "validation_scope": "all-frames", "samples": [
                    {"frame": index, "begin_tick": 100, "end_tick": 101 + index, "gpu_ms": 1 + index,
                     "warmup": index == 0, "completion_serial": index + 1} for index in range(3)]}}]}

    def test_separate_evidence_and_actual_steady_sample_count(self):
        checks = runner.verify_gpu_result(self.fixture, self.backend, self.directory, "warp")[0]["checks"]
        self.assertEqual([check["evidence"] for check in checks], ["WARP", "MOCK", "WARP", "WARP"])
        self.assertEqual(checks[2]["statistics"]["count"], 2)
        self.assertEqual(checks[2]["statistics"]["p95"], 3)

    def test_benchmark_requires_batched_fence_and_one_full_readback(self):
        fixture = copy.deepcopy(self.fixture)
        fixture["cases"][0]["frames"] = 5
        fixture["cases"][0]["warmup"] = 1
        benchmark = copy.deepcopy(self.backend)
        benchmark["execution_mode"] = "performance"
        benchmark["adapter"] = "nvidia"
        benchmark["environment"]["adapter_flags"] = 0
        benchmark["environment"]["vendor_id"] = 0x10DE
        benchmark_case = benchmark["cases"][0]
        benchmark_case["evidence"] = "NATIVE_GPU"
        benchmark_case["pixel_checks"] = 16
        benchmark_case["validated_frames"] = 1
        benchmark_case["allocation"]["timestamp_query_count"] = 10
        benchmark_case["timing"]["execution_mode"] = "batched"
        benchmark_case["timing"]["validation_scope"] = "final-frame"
        benchmark_case["completion"]["fence_value"] = 5
        benchmark_case["timing"]["samples"] = [
            {"frame": index, "begin_tick": 100 + index, "end_tick": 101 + index,
             "gpu_ms": 1, "warmup": index == 0, "completion_serial": 5} for index in range(5)]
        checks = runner.verify_gpu_result(fixture, benchmark, self.directory, "nvidia", performance=True)[0]["checks"]
        self.assertEqual(checks[2]["statistics"]["count"], 4)
        self.assertEqual(checks[3]["pixel_validation"], "final-frame-only")
        benchmark_case["timing"]["samples"][2]["completion_serial"] = 3
        with self.assertRaises(c.ContractError):
            runner.verify_gpu_result(fixture, benchmark, self.directory, "nvidia", performance=True)

    def test_rejects_evidence_upgrade_and_adapter_fallback(self):
        self.backend["cases"][0]["evidence"] = "NATIVE_NGX"
        with self.assertRaises(c.ContractError):
            runner.verify_gpu_result(self.fixture, self.backend, self.directory, "warp")
        with self.assertRaises(c.ContractError):
            runner.verify_gpu_result(self.fixture, self.backend, self.directory, "nvidia")

    def test_environment_must_identify_actual_software_adapter(self):
        self.backend["environment"]["adapter_flags"] = 0
        with self.assertRaises(c.ContractError):
            runner.verify_gpu_result(self.fixture, self.backend, self.directory, "warp")

    def test_performance_rejects_warp_and_unknown_timestamp_scope(self):
        with self.assertRaises(c.ContractError):
            runner.verify_gpu_result(self.fixture, self.backend, self.directory, "warp", performance=True)
        value = copy.deepcopy(self.backend)
        value["cases"][0]["timing"]["includes_uav_barrier"] = False
        with self.assertRaises(c.ContractError):
            runner.verify_gpu_result(self.fixture, value, self.directory, "warp")

    def test_missing_case_or_sample_is_failure(self):
        for field in ("cases", "samples"):
            raw = copy.deepcopy(self.backend)
            if field == "cases":
                raw["cases"] = []
            else:
                raw["cases"][0]["timing"]["samples"].pop()
            with self.subTest(field=field), self.assertRaises(c.ContractError):
                runner.verify_gpu_result(self.fixture, raw, self.directory, "warp")

    def test_stale_fence_replay_and_cpu_time_are_rejected(self):
        for field, value in (("completion_serial", 1), ("gpu_ms", 100), ("warmup", True), ("end_tick", 99)):
            raw = copy.deepcopy(self.backend)
            raw["cases"][0]["timing"]["samples"][1][field] = value
            with self.subTest(field=field), self.assertRaises(c.ContractError):
                runner.verify_gpu_result(self.fixture, raw, self.directory, "warp")
        self.backend["cases"][0]["completion"]["gpu_completed"] = False
        with self.assertRaises(c.ContractError):
            runner.verify_gpu_result(self.fixture, self.backend, self.directory, "warp")

    def test_readback_not_backend_assertion_is_authoritative(self):
        (self.directory / "one.rgba8").write_bytes(b"\0" * 16)
        with self.assertRaises(c.ContractError):
            runner.verify_gpu_result(self.fixture, self.backend, self.directory, "warp")

    def test_parameter_contract_cannot_be_reported_as_network_execution(self):
        for field, value in (("evidence", "NATIVE_NGX"), ("model_request", "B"), ("auto_mask", True), ("blend_mode", 3)):
            raw = copy.deepcopy(self.backend)
            raw["cases"][0]["parameter_contract"][field] = value
            with self.subTest(field=field), self.assertRaises(c.ContractError):
                runner.verify_gpu_result(self.fixture, raw, self.directory, "warp")

    def test_readback_path_escape(self):
        self.backend["cases"][0]["pixels_file"] = "../one.rgba8"
        with self.assertRaises(c.ContractError):
            runner.verify_gpu_result(self.fixture, self.backend, self.directory, "warp")

    def make_build(self):
        exe = self.directory / "backend.exe"
        exe.write_bytes(b"unit-test-identity-only")
        source = self.directory / "source.txt"
        source.write_text("one")
        c.write_json(self.directory / "build-manifest.json", {"schema": "nr-nexus-offline-build/1",
            "status": "built", "backend": "gpu", "executable": exe.name, "executable_sha256": c.digest(exe),
            "inputs": [{"path": str(source), "sha256": c.digest(source)}]})
        return exe, source

    def test_build_identity_binds_executable_and_inputs(self):
        exe, source = self.make_build()
        self.assertEqual(runner.verify_build(exe)["backend"], "gpu")
        source.write_text("two")
        with self.assertRaises(c.ContractError):
            runner.verify_build(exe)
        source.write_text("one")
        exe.write_bytes(b"different")
        with self.assertRaises(c.ContractError):
            runner.verify_build(exe)

    def test_timeout_retains_failed_receipt_and_never_reports_cases_passed(self):
        exe, _ = self.make_build()
        output = self.directory / "run"
        output.mkdir()
        with patch.object(runner.subprocess, "run", side_effect=subprocess.TimeoutExpired(str(exe), 1)):
            result = runner.run_gpu(self.fixture, exe, output, "warp", 1)
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["failure_kind"], "timeout")
        self.assertEqual(result["cases"], [])
        self.assertTrue((output / "run-start.json").exists())
        self.assertEqual(c.read_json(output / "result.json")["status"], "failed")

    def test_new_output_rejects_repository_escape_and_existing_directory(self):
        for path in (runner.ROOT, runner.ROOT / "tmp" / ".." / ".." / "escape"):
            with self.subTest(path=str(path)), self.assertRaises(c.ContractError):
                runner.new_output(path)
        with patch.object(runner, "ROOT", self.directory):
            path = self.directory / "tmp" / "new"
            self.assertEqual(runner.new_output(path), path)
            with self.assertRaises(FileExistsError):
                runner.new_output(path)


if __name__ == "__main__":
    unittest.main()
