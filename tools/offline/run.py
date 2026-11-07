"""Headless offline runner. All GPU evidence comes from a bound native backend."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid

from contracts import (ContractError, RESULT_SCHEMA, artifact_path, compare_raw, digest, identity,
                       integer, prepare_sequence, read_json, sample_statistics, verify_sequence, write_json)
import output_lifecycle

ROOT = Path(__file__).resolve().parents[2]


def finish_lifecycle(directory: Path, status: str, raw_files: list[Path]) -> None:
    marker = directory / "artifact-lifecycle.json"
    if not marker.is_file():
        return
    if status == "passed":
        output_lifecycle.register_disposable(directory, raw_files)
    else:
        if raw_files:
            output_lifecycle.retain(directory, raw_files[:1])
        output_lifecycle.register_disposable(directory, raw_files[1:])
    output_lifecycle.finish(directory, "Passed" if status == "passed" else "Failed")


def new_output(path: Path) -> Path:
    path = path.absolute()
    if ".." in path.parts:
        raise ContractError("Output contains parent traversal")
    allowed = any(path.is_relative_to(ROOT / name) and path != ROOT / name for name in ("tmp", "artifacts"))
    if not allowed:
        raise ContractError("Output must be a new directory below this checkout's tmp/ or artifacts/")
    cursor = path
    while cursor != ROOT:
        if cursor.is_symlink() or (hasattr(cursor, "is_junction") and cursor.is_junction()):
            raise ContractError("Output crosses a reparse point")
        cursor = cursor.parent
    output_lifecycle.maintain(path.parent)
    path.mkdir(parents=True, exist_ok=False)
    output_lifecycle.initialize(path)
    return path


def verify_build(executable: Path, backend: str = "gpu") -> dict:
    manifest_path = executable.parent / "build-manifest.json"
    manifest = read_json(manifest_path)
    if (manifest.get("schema") != "nr-nexus-offline-build/1" or manifest.get("status") != "built"
            or manifest.get("backend") != backend or manifest.get("executable") != executable.name
            or manifest.get("executable_sha256") != digest(executable)):
        raise ContractError("Executable does not match its offline build manifest")
    inputs = manifest.get("inputs")
    if not isinstance(inputs, list) or not inputs:
        raise ContractError("Build manifest has no source/dependency identity")
    for entry in inputs:
        if digest(Path(entry["path"])) != entry["sha256"]:
            raise ContractError(f"Build input changed: {Path(entry['path']).name}")
    return {"manifest_sha256": digest(manifest_path), **manifest}


def freeze_runner(directory: Path, include_control_mask: bool = False) -> dict:
    snapshot = directory / "runner"
    snapshot.mkdir()
    inputs = {}
    paths = [Path(__file__), Path(__file__).with_name("contracts.py"), Path(output_lifecycle.__file__)]
    if include_control_mask:
        paths.append(Path(__file__).with_name("control_mask_contracts.py"))
    for path in paths:
        target = snapshot / path.name
        with target.open("xb") as stream:
            stream.write(path.read_bytes())
        inputs[str(path)] = digest(target)
        if digest(path) != inputs[str(path)]:
            raise ContractError("Runner changed while its source snapshot was being frozen")
    return inputs


def verify_environment(environment: dict, adapter: str) -> None:
    if not isinstance(environment, dict):
        raise ContractError("Missing hardware/software environment identity")
    flags = integer(environment.get("adapter_flags"), 0, 2 ** 32 - 1, "adapter flags")
    if (bool(flags & 2) != (adapter == "warp") or (adapter == "nvidia" and environment.get("vendor_id") != 0x10DE)
            or environment.get("ngx_loaded") is not False or environment.get("network_evaluated") is not False):
        raise ContractError("Actual adapter/runtime does not match the requested non-NGX backend")


def verify_gpu_result(fixture: dict, backend: dict, directory: Path, adapter: str,
                      performance: bool = False) -> list[dict]:
    from control_mask_contracts import compare_mask

    evidence = "WARP" if adapter == "warp" else "NATIVE_GPU"
    if (backend.get("schema") != "nr-nexus-offline-backend/1"
            or backend.get("backend") != "d3d12-control-mask" or backend.get("adapter") != adapter):
        raise ContractError("Backend identity mismatch")
    if backend.get("execution_mode") != ("performance" if performance else "validation"):
        raise ContractError("Backend execution mode mismatch")
    if performance and adapter != "nvidia":
        raise ContractError("Performance evidence requires an explicitly selected NVIDIA adapter")
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
        expected_checks = case["width"] * case["height"] * 4 * (1 if performance else case["frames"])
        if observed.get("pixel_checks") != expected_checks or observed.get("validated_frames") != (1 if performance else case["frames"]):
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
                or timing.get("execution_mode") != ("batched" if performance else "per-frame-readback")
                or timing.get("validation_scope") != ("final-frame" if performance else "all-frames")):
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
            if performance:
                if current_serial != completed_serial:
                    raise ContractError("Batched sample lacks its exact submission completion")
            elif current_serial <= serial:
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
                              "pixel_validation": "final-frame-only" if performance else "all-frames"}
                        ]})
    return results


def capture_gpu_clock() -> dict:
    """Record optional NVIDIA clock/utilization snapshots without making them a run prerequisite."""
    captured_utc = datetime.now(timezone.utc).isoformat()
    executable = shutil.which("nvidia-smi")
    if executable is None:
        return {"available": False, "captured_utc": captured_utc, "reason": "nvidia-smi-not-found"}
    query = ("--query-gpu=name,index,clocks.current.graphics,clocks.current.memory,"
             "utilization.gpu,power.draw,temperature.gpu --format=csv,noheader,nounits")
    try:
        process = subprocess.run([executable, *query.split()], capture_output=True, text=True,
                                 timeout=5, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"available": False, "captured_utc": captured_utc,
                "reason": "query-timeout" if isinstance(error, subprocess.TimeoutExpired) else "query-unavailable"}
    if process.returncode != 0:
        return {"available": False, "captured_utc": captured_utc, "reason": "query-failed",
                "return_code": process.returncode}
    records = []
    for row in csv.reader(process.stdout.splitlines(), skipinitialspace=True):
        if len(row) != 7:
            continue
        records.append({"name": row[0], "index": row[1], "graphics_clock_mhz": row[2],
                        "memory_clock_mhz": row[3], "utilization_percent": row[4],
                        "power_w": row[5], "temperature_c": row[6]})
    return {"available": bool(records), "captured_utc": captured_utc,
            "source": "nvidia-smi snapshot; auxiliary telemetry, not D3D12 timestamp frequency",
            "records": records, **({} if records else {"reason": "no-parseable-gpu-records"})}


def parse_resolution(value: str) -> tuple[int, int]:
    try:
        width, height = value.lower().split("x", 1)
        parsed_width, parsed_height = int(width), int(height)
    except (AttributeError, ValueError) as error:
        raise ContractError("Resize values must use WIDTHxHEIGHT") from error
    integer(parsed_width, 1, 8192, "resize width")
    integer(parsed_height, 1, 8192, "resize height")
    return parsed_width, parsed_height


def parse_subrect(value: str) -> tuple[int, int, int, int]:
    try:
        parts = tuple(int(part) for part in value.split(","))
    except (AttributeError, ValueError) as error:
        raise ContractError("Subrect must use X,Y,WIDTH,HEIGHT") from error
    if len(parts) != 4:
        raise ContractError("Subrect must use X,Y,WIDTH,HEIGHT")
    return tuple(integer(part, 0, 8192, "subrect component") for part in parts)


def verify_replay_result(sequence: dict, source: Path, backend: dict, directory: Path, adapter: str) -> list[dict]:
    evidence = "WARP" if adapter == "warp" else "NATIVE_GPU"
    if (backend.get("schema") != "nr-nexus-offline-replay-backend/1" or backend.get("adapter") != adapter
            or backend.get("status") != "passed"):
        raise ContractError("Replay backend failed or has an unsupported identity")
    verify_environment(backend.get("environment"), adapter)
    frames = backend.get("frames")
    if not isinstance(frames, list) or len(frames) != len(sequence["frames"]):
        raise ContractError("Missing or unexpected replay frames")
    results, serial = [], 0
    for expected, frame in zip(sequence["frames"], frames):
        expected_width = expected.get("width", sequence["width"])
        expected_height = expected.get("height", sequence["height"])
        if (frame.get("index") != expected["index"] or frame.get("evidence") != evidence
                or frame.get("width") != expected_width or frame.get("height") != expected_height):
            raise ContractError("Replay frame/evidence mismatch")
        completion = frame["completion"]
        serial = integer(completion.get("fence_value"), serial + 1, 2 ** 64 - 1, "replay completion serial")
        if completion.get("gpu_completed") is not True:
            raise ContractError("Replay has no real GPU completion")
        controls = frame["controls"]
        if (controls.get("evidence") != "STATIC" or controls.get("history_evaluated") is not False
                or any(controls.get(key) != expected[key] for key in ("reset", "jitter", "motion_scale", "subrect"))):
            raise ContractError("Replay control metadata is missing or claims unexecuted History")
        if set(frame["resources"]) != set(expected["resources"]):
            raise ContractError("Replay resource set mismatch")
        checks = []
        for name, resource in expected["resources"].items():
            observed = frame["resources"][name]
            path = artifact_path(directory, observed["output_file"])
            if (observed.get("format") != resource["format"] or observed.get("bytes") != resource["bytes"]
                    or path.stat().st_size != resource["bytes"] or observed.get("bitwise_equal") is not True
                    or digest(path) != resource["sha256"]):
                raise ContractError(f"GPU replay output identity mismatch: frame {expected['index']} {name}")
            original = artifact_path(source, resource["file"])
            checks.append({"name": f"{name}-upload-readback", "evidence": evidence, "format": resource["format"],
                           "bytes": resource["bytes"], "sha256": digest(path),
                           "output_file": str(Path("backend") / observed["output_file"]), "completion": completion,
                           "comparison": compare_raw(original.read_bytes(), path.read_bytes(), "f32le", 1.0)})
        checks.append({"name": "recorded-frame-controls", "evidence": "STATIC", "observed": controls,
                       "width": expected_width, "height": expected_height})
        results.append({"id": f"frame-{expected['index']:04d}", "status": "passed",
                        "width": expected_width, "height": expected_height, "checks": checks})
    return results


def run_gpu(fixture: dict, executable: Path, directory: Path, adapter: str, timeout: int,
            performance: bool = False) -> dict:
    report = {"schema": RESULT_SCHEMA, "run_id": uuid.uuid4().hex, "status": "incomplete",
              "started_utc": datetime.now(timezone.utc).isoformat(), "fixture_sha256": identity(fixture),
              "fixture_id": fixture["fixture_id"], "adapter_request": adapter,
              "execution_mode": "performance" if performance else "validation", "cases": [],
              "not_evaluated": ["NATIVE_NGX", "IN_GAME", "VISUAL"],
              "limits": ["Model A/B, AutoMask and BlendMode are parameter contracts, not NVIDIA network results.",
                         "Timing covers only ControlMask dispatch and its UAV barrier, not NR Evaluate or game performance."]}
    if performance:
        report["limits"].append("Performance batches dispatches in one command list and validates one final readback; this is not the production NR scheduler.")
        report["gpu_clock_snapshots"] = {"before": capture_gpu_clock(), "after": None}
    write_json(directory / "run-start.json", report)
    write_json(directory / "fixture.json", fixture)
    fixture_hash = digest(directory / "fixture.json")
    try:
        runner_inputs = freeze_runner(directory, include_control_mask=True)
        build = verify_build(executable)
        report["build"] = build
        report["runner_inputs"] = runner_inputs
        backend_directory = directory / "backend"
        backend_directory.mkdir()
        command = [str(executable), "--fixture", str(directory / "fixture.json"),
                   "--output", str(backend_directory), "--adapter", adapter]
        if performance:
            command.append("--benchmark")
        with (directory / "stdout.log").open("x", encoding="utf-8") as stdout, (directory / "stderr.log").open("x", encoding="utf-8") as stderr:
            process_start = time.perf_counter()
            process = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=timeout, cwd=directory,
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0, check=False)
            report["backend_wall_ms"] = (time.perf_counter() - process_start) * 1000.0
        report["process_exit_code"] = process.returncode
        if process.returncode != 0:
            raise ContractError(f"Native backend failed with exit code {process.returncode}")
        if verify_build(executable) != build or digest(directory / "fixture.json") != fixture_hash:
            raise ContractError("Executable, build inputs or frozen fixture changed during execution")
        if any(digest(Path(path)) != value for path, value in runner_inputs.items()):
            raise ContractError("Runner inputs changed during execution")
        backend_path = backend_directory / "backend-result.json"
        backend = read_json(backend_path, max_bytes=256 * 1024 * 1024)
        report["cases"] = verify_gpu_result(fixture, backend, backend_directory, adapter,
                                             performance=performance)
        report["environment"] = backend["environment"]
        report["backend_result_sha256"] = digest(backend_path)
        report["status"] = "passed"
    except subprocess.TimeoutExpired:
        report.update(status="failed", failure_kind="timeout", error=f"Owned native process exceeded {timeout} seconds and was terminated")
    except (ContractError, OSError, KeyError, TypeError, ValueError) as error:
        report.update(status="failed", failure_kind="validation-or-backend", error=str(error))
    finally:
        if performance:
            report["gpu_clock_snapshots"]["after"] = capture_gpu_clock()
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        write_json(directory / "result.json", report)
        finish_lifecycle(directory, report["status"], sorted((directory / "backend").glob("*.rgba8")))
    return report


def run_replay(source: Path, executable: Path, directory: Path, adapter: str, timeout: int) -> dict:
    report = {"schema": RESULT_SCHEMA, "run_id": uuid.uuid4().hex, "status": "incomplete",
              "started_utc": datetime.now(timezone.utc).isoformat(), "adapter_request": adapter, "cases": [],
              "not_evaluated": ["NATIVE_NGX", "NGX_HISTORY", "IN_GAME", "VISUAL"],
              "limits": ["GPU copy/readback only; Reset, Jitter and Motion Scale are recorded STATIC metadata."]}
    write_json(directory / "run-start.json", report)
    try:
        sequence = verify_sequence(source)
        report["sequence_sha256"] = sequence["sequence_sha256"]
        write_json(directory / "sequence.json", sequence)
        build = verify_build(executable, "replay")
        report["build"] = build
        runner_inputs = freeze_runner(directory)
        report["runner_inputs"] = runner_inputs
        backend_directory = directory / "backend"
        backend_directory.mkdir()
        command = [str(executable), "--sequence", str(source), "--output", str(backend_directory), "--adapter", adapter]
        with (directory / "stdout.log").open("x", encoding="utf-8") as stdout, (directory / "stderr.log").open("x", encoding="utf-8") as stderr:
            process = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=timeout, cwd=directory,
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0, check=False)
        report["process_exit_code"] = process.returncode
        if process.returncode != 0:
            raise ContractError(f"Native replay backend failed with exit code {process.returncode}")
        if verify_build(executable, "replay") != build or verify_sequence(source) != sequence:
            raise ContractError("Replay inputs or backend changed during execution")
        if any(digest(Path(path)) != value for path, value in runner_inputs.items()):
            raise ContractError("Replay runner changed during execution")
        backend_path = backend_directory / "replay-backend-result.json"
        backend = read_json(backend_path)
        report["cases"] = verify_replay_result(sequence, source, backend, backend_directory, adapter)
        report["environment"] = backend["environment"]
        report["backend_result_sha256"] = digest(backend_path)
        report["status"] = "passed"
    except subprocess.TimeoutExpired:
        report.update(status="failed", failure_kind="timeout", error=f"Owned replay process exceeded {timeout} seconds and was terminated")
    except (ContractError, OSError, KeyError, TypeError, ValueError) as error:
        report.update(status="failed", failure_kind="validation-or-backend", error=str(error))
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        write_json(directory / "result.json", report)
        finish_lifecycle(directory, report["status"], sorted((directory / "backend").glob("*.f32")))
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate", help="Validate and identify a small fixture definition (STATIC)")
    validate.add_argument("fixture", type=Path)
    matrix = commands.add_parser("matrix", help="Write the versioned 96-case parameter matrix")
    matrix.add_argument("--output", type=Path, required=True)
    matrix.add_argument("--frames", type=int, default=8)
    matrix.add_argument("--warmup", type=int, default=2)
    prepare = commands.add_parser("prepare", help="Generate a portable deterministic CPU sequence (STATIC)")
    prepare.add_argument("--output", type=Path, required=True)
    prepare.add_argument("--width", type=int, default=64)
    prepare.add_argument("--height", type=int, default=36)
    prepare.add_argument("--frames", type=int, default=8)
    prepare.add_argument("--seed", type=int, default=0)
    prepare.add_argument("--resize-cycle", nargs="+", metavar="WIDTHxHEIGHT",
                         help="repeat deterministic per-frame geometry schedule, for example 64x36 96x54")
    prepare.add_argument("--subrect", metavar="X,Y,WIDTH,HEIGHT",
                         help="record one STATIC subrect on every generated frame")
    verify = commands.add_parser("verify-sequence", help="Verify full raw sequence content identity (STATIC)")
    verify.add_argument("directory", type=Path)
    compare = commands.add_parser("compare", help="Compare raw outputs; does not assert their provenance")
    compare.add_argument("first", type=Path)
    compare.add_argument("second", type=Path)
    compare.add_argument("--storage", choices=("u8", "f32le"), required=True)
    compare.add_argument("--data-range", type=float, default=1.0)
    run = commands.add_parser("run", help="Execute a manifest-bound native GPU backend without a game")
    source = run.add_mutually_exclusive_group(required=True)
    source.add_argument("--fixture", type=Path)
    source.add_argument("--matrix", action="store_true")
    run.add_argument("--backend-exe", type=Path, required=True)
    run.add_argument("--output", type=Path, required=True)
    run.add_argument("--adapter", choices=("warp", "nvidia"), default="warp")
    run.add_argument("--timeout", type=int, default=120)
    run.add_argument("--performance", action="store_true",
                     help="batch timestamp samples and validate the final readback once")
    replay = commands.add_parser("replay", help="Replay raw float32 inputs through actual GPU copies, without NGX History")
    replay.add_argument("--sequence", type=Path, required=True)
    replay.add_argument("--backend-exe", type=Path, required=True)
    replay.add_argument("--output", type=Path, required=True)
    replay.add_argument("--adapter", choices=("warp", "nvidia"), default="warp")
    replay.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    try:
        if args.command == "validate":
            from control_mask_contracts import validate_fixture
            fixture = validate_fixture(read_json(args.fixture))
            result = {"fixture_id": fixture["fixture_id"], "sha256": identity(fixture), "cases": len(fixture["cases"]), "evidence": "STATIC"}
        elif args.command == "matrix":
            from control_mask_contracts import parameter_matrix, validate_fixture
            fixture = parameter_matrix()
            for case in fixture["cases"]:
                case.update(frames=args.frames, warmup=args.warmup)
            write_json(args.output, validate_fixture(fixture))
            result = {"fixture_file": str(args.output), "cases": 96, "evidence": "STATIC"}
        elif args.command == "prepare":
            # Validate the output root without creating the actual sequence directory twice.
            directory = new_output(args.output)
            sizes = [parse_resolution(value) for value in args.resize_cycle] if args.resize_cycle else None
            resolutions = ([sizes[index % len(sizes)] for index in range(args.frames)] if sizes else None)
            rect = parse_subrect(args.subrect) if args.subrect else None
            subrects = ([rect] * args.frames if rect else None)
            sequence = prepare_sequence(directory / "inputs", args.width, args.height, args.frames, args.seed,
                                        resolutions=resolutions, subrects=subrects)
            result = {"sequence": str(directory / "inputs" / "sequence.json"), "frames": len(sequence["frames"]),
                      "sha256": sequence["sequence_sha256"], "evidence": "STATIC"}
        elif args.command == "verify-sequence":
            sequence = verify_sequence(args.directory)
            result = {"frames": len(sequence["frames"]), "sha256": sequence["sequence_sha256"], "evidence": "STATIC"}
        elif args.command == "compare":
            result = compare_raw(args.first.read_bytes(), args.second.read_bytes(), args.storage, args.data_range)
        elif args.command == "run":
            from control_mask_contracts import parameter_matrix, validate_fixture
            integer(args.timeout, 1, 600, "timeout seconds")
            fixture = parameter_matrix() if args.matrix else validate_fixture(read_json(args.fixture))
            directory = new_output(args.output)
            report = run_gpu(fixture, args.backend_exe.resolve(), directory, args.adapter, args.timeout,
                             performance=args.performance)
            result = {"status": report["status"], "cases": len(report["cases"]), "result": str(directory / "result.json")}
            print(json.dumps(result, ensure_ascii=False, allow_nan=False))
            return 0 if report["status"] == "passed" else 1
        else:
            integer(args.timeout, 1, 600, "timeout seconds")
            directory = new_output(args.output)
            report = run_replay(args.sequence.resolve(), args.backend_exe.resolve(), directory, args.adapter, args.timeout)
            print(json.dumps({"status": report["status"], "frames": len(report["cases"]),
                              "result": str(directory / "result.json")}, ensure_ascii=False))
            return 0 if report["status"] == "passed" else 1
        print(json.dumps(result, ensure_ascii=False, allow_nan=False))
        return 0
    except (ContractError, OSError, KeyError, TypeError, ValueError) as error:
        print(json.dumps({"status": "failed", "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
