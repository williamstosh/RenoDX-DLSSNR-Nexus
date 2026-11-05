"""Versioned offline fixtures, deterministic inputs, and numeric measurements."""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import struct
from typing import Any

RESULT_SCHEMA = "nr-nexus-offline-result/1"
EVIDENCE = frozenset({"STATIC", "MOCK", "WARP", "NATIVE_GPU", "NATIVE_NGX", "IN_GAME", "VISUAL"})


class ContractError(ValueError):
    pass


def read_json(path: Path, max_bytes: int = 16 * 1024 * 1024) -> Any:
    if path.stat().st_size > max_bytes:
        raise ContractError(f"JSON input exceeds {max_bytes} bytes: {path.name}")

    def pairs(values):
        result = {}
        for key, value in values:
            if key in result:
                raise ContractError(f"Duplicate JSON key: {key}")
            result[key] = value
        return result

    def constant(value):
        raise ContractError(f"Non-finite JSON number: {value}")

    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=pairs, parse_constant=constant)


def write_json(path: Path, value: Any) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, allow_nan=False, indent=2)
        stream.write("\n")


def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def artifact_path(directory: Path, name: str) -> Path:
    if not isinstance(name, str) or not name or "\\" in name or ":" in name:
        raise ContractError("Invalid relative artifact path")
    root = directory.resolve()
    path = directory / name
    if Path(name).is_absolute() or ".." in Path(name).parts or not path.resolve().is_relative_to(root):
        raise ContractError("Artifact path escapes its run directory")
    cursor = path
    while cursor != directory:
        if cursor.is_symlink() or (hasattr(cursor, "is_junction") and cursor.is_junction()):
            raise ContractError("Artifact path crosses a reparse point")
        cursor = cursor.parent
    return path


def identity(value: Any) -> str:
    canonical = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False, allow_nan=False)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def integer(value: Any, minimum: int, maximum: int, name: str) -> int:
    if type(value) is not int or not minimum <= value <= maximum:
        raise ContractError(f"{name} must be an integer in [{minimum}, {maximum}]")
    return value


def sample_statistics(samples: list[float]) -> dict:
    if not samples or any(type(value) not in (int, float) or not math.isfinite(value) or value < 0 for value in samples):
        raise ContractError("Statistics require finite nonnegative samples")
    ordered = sorted(samples)
    count = len(samples)
    return {"count": count, "minimum": ordered[0], "maximum": ordered[-1],
            "mean": math.fsum(samples) / count,
            "p50": ordered[math.ceil(count * .5) - 1], "p95": ordered[math.ceil(count * .95) - 1],
            "percentile_method": "nearest-rank"}


def compare_raw(first: bytes, second: bytes, storage: str, data_range: float = 1.0) -> dict:
    if len(first) != len(second) or not first:
        raise ContractError("Raw arrays must have the same nonzero byte length")
    if storage == "u8":
        left, right = first, second
    elif storage == "f32le" and len(first) % 4 == 0:
        left = [value[0] for value in struct.iter_unpack("<f", first)]
        right = [value[0] for value in struct.iter_unpack("<f", second)]
    else:
        raise ContractError("Storage must be u8 or aligned f32le")
    if not math.isfinite(data_range) or data_range <= 0:
        raise ContractError("data_range must be finite and positive")
    if any(not math.isfinite(value) for values in (left, right) for value in values):
        raise ContractError("Non-finite image component")
    if first == second:
        return {"components": len(first) if storage == "u8" else len(first) // 4,
                "different_components": 0, "max_absolute_error": 0, "mae": 0.0, "rmse": 0.0,
                "psnr_db": None, "identical": True, "data_range": data_range, "storage": storage}
    differences = [abs(a - b) for a, b in zip(left, right)]
    mse = math.fsum(value * value for value in differences) / len(differences)
    return {"components": len(differences), "different_components": sum(value != 0 for value in differences),
            "max_absolute_error": max(differences), "mae": math.fsum(differences) / len(differences),
            "rmse": math.sqrt(mse), "psnr_db": None if mse == 0 else 10 * math.log10(data_range ** 2 / mse),
            "identical": first == second, "data_range": data_range, "storage": storage}


def prepare_sequence(directory: Path, width: int, height: int, frames: int, seed: int = 0,
                     resolutions: list[tuple[int, int]] | None = None,
                     subrects: list[tuple[int, int, int, int]] | None = None) -> dict:
    """Generate portable CPU fixtures; this function makes no GPU/NGX claim."""
    integer(width, 1, 8192, "width")
    integer(height, 1, 8192, "height")
    integer(frames, 1, 4096, "frames")
    integer(seed, 0, 2 ** 32 - 1, "seed")
    if resolutions is None:
        resolutions = [(width, height)] * frames
    if len(resolutions) != frames:
        raise ContractError("Resolution schedule must contain one size per frame")
    checked_resolutions = [(integer(frame_width, 1, 8192, "frame width"),
                            integer(frame_height, 1, 8192, "frame height"))
                           for frame_width, frame_height in resolutions]
    if subrects is None:
        subrects = [(0, 0, frame_width, frame_height) for frame_width, frame_height in checked_resolutions]
    if len(subrects) != frames:
        raise ContractError("Subrect schedule must contain one rectangle per frame")
    checked_subrects = []
    for (frame_width, frame_height), rect in zip(checked_resolutions, subrects):
        if not isinstance(rect, (tuple, list)) or len(rect) != 4:
            raise ContractError("Subrect requires x, y, width and height")
        x, y, rect_width, rect_height = (integer(value, 0, 8192, "subrect component") for value in rect)
        if x + rect_width > frame_width or y + rect_height > frame_height:
            raise ContractError("Subrect exceeds frame geometry")
        checked_subrects.append((x, y, rect_width, rect_height))
    if sum(frame_width * frame_height * 28 for frame_width, frame_height in checked_resolutions) > 256 * 1024 * 1024:
        raise ContractError("Synthetic sequence exceeds 256 MiB")
    directory.mkdir(parents=True, exist_ok=False)
    manifest = {"schema": "nr-nexus-offline-sequence/2", "generator": "moving-ramp/1", "evidence": "STATIC",
                "seed": seed, "width": width, "height": height, "byte_order": "little",
                "motion_units": "pixels", "motion_direction": "current-to-previous",
                "depth_inverted": False, "pre_exposure": 1.0, "frames": []}
    for index in range(frames):
        frame_width, frame_height = checked_resolutions[index]
        color, depth, motion = bytearray(), bytearray(), bytearray()
        shift = (index + seed) % frame_width
        for y in range(frame_height):
            for x in range(frame_width):
                color.extend(struct.pack("<4f", ((x - shift) % frame_width) / max(frame_width - 1, 1),
                                         y / max(frame_height - 1, 1), .25, 1.0))
                depth.extend(struct.pack("<f", .1 + .8 * y / max(frame_height - 1, 1)))
                motion.extend(struct.pack("<2f", -1.0 if index else 0.0, 0.0))
        frame = {"index": index, "width": frame_width, "height": frame_height,
                 "reset": index == 0, "jitter": [0.0, 0.0], "motion_scale": [1.0, 1.0],
                 "subrect": list(checked_subrects[index]), "resources": {}}
        for name, data, fmt in (("color", color, "R32G32B32A32_FLOAT"),
                                ("depth", depth, "R32_FLOAT"), ("motion", motion, "R32G32_FLOAT")):
            path = directory / f"{index:04d}-{name}.f32"
            with path.open("xb") as stream:
                stream.write(data)
            frame["resources"][name] = {"file": path.name, "format": fmt, "bytes": len(data), "sha256": digest(path)}
        manifest["frames"].append(frame)
    manifest["sequence_sha256"] = identity(manifest)
    write_json(directory / "sequence.json", manifest)
    return manifest


def verify_sequence(directory: Path) -> dict:
    manifest = read_json(directory / "sequence.json")
    if not isinstance(manifest, dict):
        raise ContractError("Sequence manifest must be a JSON object")
    if manifest.get("schema") not in ("nr-nexus-offline-sequence/1", "nr-nexus-offline-sequence/2"):
        raise ContractError("Unsupported sequence schema")
    unsigned = {key: value for key, value in manifest.items() if key != "sequence_sha256"}
    if manifest.get("sequence_sha256") != identity(unsigned):
        raise ContractError("Sequence identity mismatch")
    manifest_width = integer(manifest.get("width"), 1, 8192, "width")
    manifest_height = integer(manifest.get("height"), 1, 8192, "height")
    frames = manifest.get("frames")
    if not isinstance(frames, list) or not 1 <= len(frames) <= 4096:
        raise ContractError("Sequence needs 1..4096 frames")
    formats = {"color": ("R32G32B32A32_FLOAT", 16), "depth": ("R32_FLOAT", 4), "motion": ("R32G32_FLOAT", 8)}
    total_bytes = 0
    for index, frame in enumerate(frames):
        if frame["index"] != index or type(frame["reset"]) is not bool:
            raise ContractError("Invalid frame index or Reset")
        if manifest["schema"] == "nr-nexus-offline-sequence/1":
            width, height = manifest_width, manifest_height
        else:
            width = integer(frame.get("width"), 1, 8192, "frame width")
            height = integer(frame.get("height"), 1, 8192, "frame height")
        total_bytes += width * height * sum(stride for _, stride in formats.values())
        for field in ("jitter", "motion_scale"):
            values = frame[field]
            if not isinstance(values, list) or len(values) != 2 or any(
                type(value) not in (int, float) or not math.isfinite(value) for value in values
            ):
                raise ContractError(f"Invalid {field}")
        rect = frame["subrect"]
        if (not isinstance(rect, list) or len(rect) != 4 or any(type(value) is not int for value in rect)
                or rect[0] < 0 or rect[1] < 0 or rect[2] < 0 or rect[3] < 0
                or rect[0] + rect[2] > width or rect[1] + rect[3] > height
                or set(frame["resources"]) != set(formats)):
            raise ContractError("Unsupported sequence subrect or resources")
        for name, (fmt, stride) in formats.items():
            resource = frame["resources"][name]
            path = artifact_path(directory, resource["file"])
            if (resource["format"] != fmt or resource["bytes"] != width * height * stride
                    or path.stat().st_size != resource["bytes"] or digest(path) != resource["sha256"]):
                raise ContractError(f"Sequence resource mismatch: frame {index} {name}")
    if total_bytes > 256 * 1024 * 1024:
        raise ContractError("Sequence exceeds 256 MiB")
    return manifest
