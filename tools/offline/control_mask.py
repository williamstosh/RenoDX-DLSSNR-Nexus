"""ControlMask-specific GPU runner; generic replay remains in run.py."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import math
import os
from pathlib import Path
import subprocess
import time
import uuid

from contracts import (ContractError, RESULT_SCHEMA, artifact_path, digest, identity,
                       integer, read_json, sample_statistics, write_json)
from run import new_output, verify_build, verify_environment
from control_mask_contracts import validate_fixture


def freeze_runner(directory: Path) -> dict:
    snapshot = directory / "runner"
    snapshot.mkdir()
    inputs = {}
    for name in ("control_mask.py", "control_mask_contracts.py", "run.py", "contracts.py"):
        path = Path(__file__).with_name(name)
        target = snapshot / name
        with target.open("xb") as stream:
            stream.write(path.read_bytes())
        inputs[str(path)] = digest(target)
        if digest(path) != inputs[str(path)]:
            raise ContractError("Runner changed while freezing its source snapshot")
    return inputs


def verify_gpu_result(fixture: dict, backend: dict, directory: Path, adapter: str) -> list[dict]:
    from control_mask_contracts import compare_mask

    evidence = "WARP" if adapter == "warp" else "NATIVE_GPU"
    if (backend.get("schema") != "nr-nexus-offline-backend/1"
            or backend.get("backend") != "d3d12-control-mask" or backend.get("adapter") != adapter):
        raise ContractError("Backend identity mismatch")
    if backend.get("execution_mode") != "validation":
        raise ContractError("Backend execution mode mismatch")
    verify_environment(backend.get("environment"), adapter)
    timestamp_frequency = integer(backend["environment"].get("timestamp_frequency_hz"),
                                  1, 2 ** 64 - 1, "environment timestamp frequency")
    if backend["environment"].get("timestamp_frequency_scope") != "D3D12 command-queue timestamp ticks per second; not GPU core clock":
        raise ContractError("Unknown timestamp frequency scope")
    process_timing = backend.get("cpu_timing")
    if (not isinstance(process_timing, dict)
            or process_timing.get("clock") != "std::chrono::steady_clock"
            or any(type(process_timing.get(key)) not in (int, float)
                   or not math.isfinite(process_timing[key]) or process_timing[key] < 0
                   for key in ("device_queue_init_ms", "pipeline_init_ms", "all_case_resource_and_execution_ms"))):
        raise ContractError("Missing process-level CPU initialization timings")
    observed_cases = backend.get("cases")
    expected_ids = [case["id"] for case in fixture["cases"]]
    if (not isinstance(observed_cases, list) or len(observed_cases) != len(expected_ids)
            or [case.get("id") for case in observed_cases] != expected_ids):
        raise ContractError("Missing, duplicate, reordered or unexpected backend cases")
    results = []
    for case, observed in zip(fixture["cases"], observed_cases):
        if observed.get("status") != "passed" or observed.get("evidence") != evidence:
            raise ContractError(f"Case failed or has incorrect evidence: {case['id']}")
        if (observed.get("width") != case["width"] or observed.get("height") != case["height"]
                or observed.get("format") != "R8G8B8A8_UNORM"):
            raise ContractError("Readback geometry/format mismatch")
        pixels = artifact_path(directory, observed["pixels_file"])
        if pixels.stat().st_size != case["width"] * case["height"] * 4:
            raise ContractError("Readback length mismatch")
        comparison = compare_mask(case, pixels.read_bytes())
        if not comparison["format_conformant"]:
            raise ContractError(f"Independent shader reference mismatch: {case['id']}")
        expected_checks = case["width"] * case["height"] * 4 * case["frames"]
        if observed.get("pixel_checks") != expected_checks or observed.get("validated_frames") != case["frames"]:
            raise ContractError("Pixel validation sample count differs from the requested execution mode")
        contract = observed["parameter_contract"]
        if (contract.get("evidence") != "MOCK" or contract.get("model_request") != case["model"]
                or contract.get("auto_mask") != case["auto_mask"] or contract.get("blend_mode") != case["blend_mode"]):
            raise ContractError("MOCK parameter observations differ from the fixture")
        timing = observed["timing"]
        frequency = integer(timing["frequency_hz"], 1, 2 ** 64 - 1, "timestamp frequency")
        if (frequency != timestamp_frequency or timing.get("scope") != "control-mask-dispatch"
                or timing.get("includes_uav_barrier") is not True
                or timing.get("source") != "D3D12_QUERY_TYPE_TIMESTAMP"
                or timing.get("execution_mode") != "per-frame-readback"
                or timing.get("validation_scope") != "all-frames"):
            raise ContractError("Unknown timing scope")
        samples = timing["samples"]
        if not isinstance(samples, list) or len(samples) != case["frames"]:
            raise ContractError("Missing raw GPU samples; no synthetic filling is permitted")
        completion = observed["completion"]
        completed_serial = integer(completion.get("fence_value"), 1, 2 ** 64 - 1, "completion fence")
        steady, serial = [], 0
        for index, sample in enumerate(samples):
            begin = integer(sample["begin_tick"], 0, 2 ** 64 - 1, "begin tick")
            end = integer(sample["end_tick"], begin, 2 ** 64 - 1, "end tick")
            current_serial = integer(sample["completion_serial"], 1, 2 ** 64 - 1, "completion serial")
            if current_serial <= serial:
                raise ContractError("Per-frame sample completion serial did not advance")
            milliseconds = (end - begin) * 1000.0 / frequency
            if (sample["frame"] != index or type(sample["warmup"]) is not bool
                    or sample["warmup"] != (index < case["warmup"])
                    or type(sample["gpu_ms"]) not in (int, float)
                    or not math.isfinite(sample["gpu_ms"])
                    or not math.isclose(sample["gpu_ms"], milliseconds, rel_tol=1e-6, abs_tol=1e-7)):
                raise ContractError("Timestamp/sample identity mismatch")
            serial = current_serial
            if not sample["warmup"]:
                steady.append(milliseconds)
        completion = observed["completion"]
        if completion.get("gpu_completed") is not True or completion.get("fence_value") != completed_serial:
            raise ContractError("Missing exact submission completion")
        cpu_timing = observed.get("cpu_timing")
        allocation = observed.get("allocation")
        if (not isinstance(cpu_timing, dict) or cpu_timing.get("clock") != "std::chrono::steady_clock"
                or any(type(cpu_timing.get(key)) not in (int, float) or not math.isfinite(cpu_timing[key])
                       or cpu_timing[key] < 0 for key in ("resource_setup_ms", "command_recording_ms", "case_wall_ms"))
                or not isinstance(allocation, dict)
                or integer(allocation.get("texture_bytes_estimate"), 1, 2 ** 64 - 1, "texture allocation estimate")
                    < case["width"] * case["height"] * 4
                or integer(allocation.get("timestamp_query_count"), 2, 8192, "timestamp query count") != 2 * case["frames"]):
            raise ContractError("Missing CPU timing or resource allocation evidence")
        results.append({"id": case["id"], "status": "passed", "fixture_case_sha256": identity(case),
                        "checks": [
                            {"name": "production-control-mask-shader", "evidence": evidence,
                             "comparison": comparison, "pixels_file": str(Path("backend") / observed["pixels_file"]),
                             "pixels_sha256": digest(pixels), "completion": completion},
                            {"name": "model-auto-mask-blend-parameter-contract", "evidence": "MOCK", "observed": contract},
                            {"name": "control-mask-dispatch-timing", "evidence": evidence,
                             "scope": timing["scope"], "unit": "ms", "statistics": sample_statistics(steady),
                             "first_gpu_sample_ms": samples[0]["gpu_ms"],
                             "steady_sample_count": len(steady),
                             "raw_samples_file": "backend/backend-result.json", "warmup_excluded": case["warmup"]},
                            {"name": "cpu-orchestration-and-resource-allocation", "evidence": evidence,
                              "scope": "host timing and D3D12 allocation estimates; excludes NR Evaluate",
                              "cpu_timing": cpu_timing, "allocation": allocation,
                              "process_cpu_timing": backend["cpu_timing"],
                              "timestamp_frequency_hz": backend["environment"].get("timestamp_frequency_hz"),
                              "pixel_validation": "all-frames"}
                        ]})
    return results


def run_gpu(fixture: dict, executable: Path, directory: Path, adapter: str, timeout: int) -> dict:
    report = {"schema": RESULT_SCHEMA, "run_id": uuid.uuid4().hex, "status": "incomplete",
              "started_utc": datetime.now(timezone.utc).isoformat(), "fixture_sha256": identity(fixture),
              "fixture_id": fixture["fixture_id"], "adapter_request": adapter,
              "execution_mode": "validation", "cases": [],
              "not_evaluated": ["NATIVE_NGX", "IN_GAME", "VISUAL"],
              "limits": ["Model A/B, AutoMask and BlendMode are parameter contracts, not NVIDIA network results.",
                         "Timing covers only ControlMask dispatch and its UAV barrier, not NR Evaluate or game performance."]}
    write_json(directory / "run-start.json", report)
    write_json(directory / "fixture.json", fixture)
    fixture_hash = digest(directory / "fixture.json")
    try:
        runner_inputs = freeze_runner(directory)
        build = verify_build(executable, "gpu")
        report["build"] = build
        report["runner_inputs"] = runner_inputs
        backend_directory = directory / "backend"
        backend_directory.mkdir()
        command = [str(executable), "--fixture", str(directory / "fixture.json"),
                   "--output", str(backend_directory), "--adapter", adapter]
        with (directory / "stdout.log").open("x", encoding="utf-8") as stdout, (directory / "stderr.log").open("x", encoding="utf-8") as stderr:
            process_start = time.perf_counter()
            process = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=timeout, cwd=directory,
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0, check=False)
            report["backend_wall_ms"] = (time.perf_counter() - process_start) * 1000.0
        report["process_exit_code"] = process.returncode
        if process.returncode != 0:
            raise ContractError(f"Native backend failed with exit code {process.returncode}")
        if verify_build(executable, "gpu") != build or digest(directory / "fixture.json") != fixture_hash:
            raise ContractError("Executable, build inputs or frozen fixture changed during execution")
        if any(digest(Path(path)) != value for path, value in runner_inputs.items()):
            raise ContractError("Runner inputs changed during execution")
        backend_path = backend_directory / "backend-result.json"
        backend = read_json(backend_path, max_bytes=256 * 1024 * 1024)
        report["cases"] = verify_gpu_result(fixture, backend, backend_directory, adapter)
        report["environment"] = backend["environment"]
        report["backend_result_sha256"] = digest(backend_path)
        report["status"] = "passed"
    except subprocess.TimeoutExpired:
        report.update(status="failed", failure_kind="timeout", error=f"Owned native process exceeded {timeout} seconds and was terminated")
    except (ContractError, OSError, KeyError, TypeError, ValueError) as error:
        report.update(status="failed", failure_kind="validation-or-backend", error=str(error))
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        write_json(directory / "result.json", report)
    return report


def run_contracts(executable: Path, directory: Path, timeout: int) -> dict:
    report = {"schema": RESULT_SCHEMA, "run_id": uuid.uuid4().hex, "status": "incomplete",
              "started_utc": datetime.now(timezone.utc).isoformat(), "adapter_request": "warp",
              "not_evaluated": ["NATIVE_GPU", "NATIVE_NGX", "IN_GAME", "VISUAL"]}
    write_json(directory / "run-start.json", report)
    try:
        build = verify_build(executable, "gpu")
        report["build"] = build
        runner_inputs = freeze_runner(directory)
        report["runner_inputs"] = runner_inputs
        backend_directory = directory / "backend"
        backend_directory.mkdir()
        command = [str(executable), "--contracts", "--adapter", "warp", "--output", str(backend_directory)]
        write_json(directory / "command.json", command)
        with (directory / "stdout.log").open("x", encoding="utf-8") as stdout, (directory / "stderr.log").open("x", encoding="utf-8") as stderr:
            process = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=timeout, cwd=directory,
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0, check=False)
        report["process_exit_code"] = process.returncode
        if process.returncode != 0:
            raise ContractError(f"Native contracts failed with exit code {process.returncode}")
        if verify_build(executable, "gpu") != build or any(digest(Path(path)) != value for path, value in runner_inputs.items()):
            raise ContractError("Build or runner inputs changed during execution")
        backend_path = backend_directory / "backend-result.json"
        backend = read_json(backend_path)
        if (backend.get("schema") != "nr-nexus-offline-backend/1"
                or backend.get("backend") != "d3d12-control-mask" or backend.get("adapter") != "warp"
                or backend.get("contracts") != {"status": "passed", "evidence": ["MOCK", "WARP"],
                    "entry": "control_mask_gpu_cases::Run", "network_evaluated": False}):
            raise ContractError("Unexpected WARP contracts result")
        report.update(status="passed", contracts=backend["contracts"], environment=backend["environment"],
                      backend_result_sha256=digest(backend_path))
    except subprocess.TimeoutExpired:
        report.update(status="failed", failure_kind="timeout", error=f"Owned native process exceeded {timeout} seconds and was terminated")
    except (ContractError, OSError, KeyError, TypeError, ValueError) as error:
        report.update(status="failed", failure_kind="validation-or-backend", error=str(error))
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        write_json(directory / "result.json", report)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", nargs="?", type=Path)
    parser.add_argument("--contracts", action="store_true")
    parser.add_argument("--backend-exe", required=True, type=Path)
    parser.add_argument("--adapter", required=True, choices=("warp", "nvidia"))
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", default=120, type=int)
    args = parser.parse_args()
    if args.contracts == (args.fixture is not None):
        parser.error("Select either a fixture or --contracts")
    if args.contracts and args.adapter != "warp":
        parser.error("--contracts requires --adapter warp")
    integer(args.timeout, 1, 3600, "timeout")
    fixture = None if args.contracts else validate_fixture(read_json(args.fixture))
    verify_build(args.backend_exe.resolve(), "gpu")
    directory = new_output(args.output)
    if args.contracts:
        report = run_contracts(args.backend_exe.resolve(), directory, args.timeout)
    else:
        report = run_gpu(fixture, args.backend_exe.resolve(), directory, args.adapter, args.timeout)
    print(f"{report['status']}: {directory / 'result.json'}")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())