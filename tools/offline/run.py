"""Headless offline runner for portable fixtures and E0 GPU replay."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

from contracts import (ContractError, RESULT_SCHEMA, artifact_path, compare_raw, digest, integer,
                       prepare_sequence, read_json, verify_sequence, write_json)

ROOT = Path(__file__).resolve().parents[2]


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
    path.mkdir(parents=True, exist_ok=False)
    return path


def verify_build(executable: Path, backend: str) -> dict:
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


def freeze_runner(directory: Path) -> dict:
    snapshot = directory / "runner"
    snapshot.mkdir()
    inputs = {}
    paths = [Path(__file__), Path(__file__).with_name("contracts.py")]
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
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
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
    replay = commands.add_parser("replay", help="Replay raw float32 inputs through actual GPU copies, without NGX History")
    replay.add_argument("--sequence", type=Path, required=True)
    replay.add_argument("--backend-exe", type=Path, required=True)
    replay.add_argument("--output", type=Path, required=True)
    replay.add_argument("--adapter", choices=("warp", "nvidia"), default="warp")
    replay.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    try:
        if args.command == "prepare":
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
