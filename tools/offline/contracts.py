"""Versioned offline fixtures, deterministic inputs, and numeric measurements."""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import re
import struct
from typing import Any

FIXTURE_SCHEMA = "nr-nexus-offline-fixture/1"
RESULT_SCHEMA = "nr-nexus-offline-result/1"
EVIDENCE = frozenset({"STATIC", "MOCK", "WARP", "NATIVE_GPU", "NATIVE_NGX", "IN_GAME", "VISUAL"})
MODES = ("rgb", "neutral", "rect", "green_rect", "blue_rect")
IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,95}\Z")
CASE_DEFAULTS = {"mode": "neutral", "rgb": [1.0, 1.0, 1.0], "rect": [0, 0],
                 "model": "A", "auto_mask": False, "blend_mode": 0, "frames": 8, "warmup": 2}


class ContractError(ValueError):
    pass


def read_json(path: Path) -> Any:
    if path.stat().st_size > 16 * 1024 * 1024:
        raise ContractError(f"JSON input exceeds 16 MiB: {path.name}")

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


def validate_fixture(value: Any) -> dict:
    if not isinstance(value, dict) or set(value) - {"schema", "fixture_id", "description", "cases"}:
        raise ContractError("Unknown fixture fields or non-object fixture")
    if value.get("schema") != FIXTURE_SCHEMA:
        raise ContractError("Unsupported fixture schema")
    if not isinstance(value.get("fixture_id"), str) or not IDENTIFIER.fullmatch(value["fixture_id"]):
        raise ContractError("Invalid fixture_id")
    if "description" in value and not isinstance(value["description"], str):
        raise ContractError("description must be text")
    cases = value.get("cases")
    if not isinstance(cases, list) or not 1 <= len(cases) <= 4096:
        raise ContractError("Fixture requires 1..4096 cases")
    result, seen = [], set()
    for raw in cases:
        if not isinstance(raw, dict) or set(raw) - (set(CASE_DEFAULTS) | {"id", "width", "height"}):
            raise ContractError("Unknown case fields or non-object case")
        case = {**CASE_DEFAULTS, **raw}
        name = case.get("id")
        if not isinstance(name, str) or not IDENTIFIER.fullmatch(name) or name.casefold() in seen:
            raise ContractError("Case IDs must be safe and unique (also on Windows)")
        seen.add(name.casefold())
        width = integer(case.get("width"), 1, 8192, "width")
        height = integer(case.get("height"), 1, 8192, "height")
        if case["mode"] not in MODES or case["model"] not in ("A", "B"):
            raise ContractError("Unsupported mask mode or model request")
        rgb = case["rgb"]
        if not isinstance(rgb, list) or len(rgb) != 3 or any(
            type(channel) not in (int, float) or not math.isfinite(channel) or not 0 <= channel <= 1 for channel in rgb
        ):
            raise ContractError("rgb requires three finite numbers in [0, 1]")
        rect = case["rect"]
        if not isinstance(rect, list) or len(rect) != 2:
            raise ContractError("rect requires width and height")
        integer(rect[0], 0, width, "rect width")
        integer(rect[1], 0, height, "rect height")
        if type(case["auto_mask"]) is not bool:
            raise ContractError("auto_mask must be a boolean")
        integer(case["blend_mode"], 0, 3, "blend_mode")
        integer(case["frames"], 1, 4096, "frames")
        integer(case["warmup"], 0, case["frames"] - 1, "warmup")
        result.append(case)
    if sum(case["width"] * case["height"] for case in result) > 64 * 1024 * 1024:
        raise ContractError("Fixture exceeds 64 million output texels")
    return {"schema": FIXTURE_SCHEMA, "fixture_id": value["fixture_id"],
            "description": value.get("description", ""), "cases": result}


def mask_reference(case: dict) -> bytes:
    # RGBA8 conversion is checked independently of the shader implementation.
    fixed = [round(channel * 255) for channel in case["rgb"]] + [255]
    pixels = bytearray(case["width"] * case["height"] * 4)
    for y in range(case["height"]):
        for x in range(case["width"]):
            pixel = fixed[:]
            if case["mode"] == "neutral":
                pixel = [255] * 4
            elif case["mode"] in ("green_rect", "blue_rect"):
                pixel = [255] * 4
                if x < case["width"] // 2 and y < case["height"] // 2:
                    pixel[1 if case["mode"] == "green_rect" else 2] = 0
            elif case["mode"] == "rect" and x < case["rect"][0] and y < case["rect"][1]:
                pixel = [0, 0, 0, 255]
            offset = (y * case["width"] + x) * 4
            pixels[offset:offset + 4] = bytes(pixel)
    return bytes(pixels)


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
    differences = [abs(a - b) for a, b in zip(left, right)]
    mse = math.fsum(value * value for value in differences) / len(differences)
    return {"components": len(differences), "different_components": sum(value != 0 for value in differences),
            "max_absolute_error": max(differences), "mae": math.fsum(differences) / len(differences),
            "rmse": math.sqrt(mse), "psnr_db": None if mse == 0 else 10 * math.log10(data_range ** 2 / mse),
            "identical": first == second, "data_range": data_range, "storage": storage}


def prepare_sequence(directory: Path, width: int, height: int, frames: int, seed: int = 0) -> dict:
    """Generate portable CPU fixtures; this function makes no GPU/NGX claim."""
    integer(width, 1, 8192, "width")
    integer(height, 1, 8192, "height")
    integer(frames, 1, 4096, "frames")
    integer(seed, 0, 2 ** 32 - 1, "seed")
    if width * height * frames * 28 > 256 * 1024 * 1024:
        raise ContractError("Synthetic sequence exceeds 256 MiB")
    directory.mkdir(parents=True, exist_ok=False)
    manifest = {"schema": "nr-nexus-offline-sequence/1", "generator": "moving-ramp/1", "evidence": "STATIC",
                "seed": seed, "width": width, "height": height, "byte_order": "little",
                "motion_units": "pixels", "motion_direction": "current-to-previous",
                "depth_inverted": False, "pre_exposure": 1.0, "frames": []}
    for index in range(frames):
        color, depth, motion = bytearray(), bytearray(), bytearray()
        shift = (index + seed) % width
        for y in range(height):
            for x in range(width):
                color.extend(struct.pack("<4f", ((x - shift) % width) / max(width - 1, 1),
                                         y / max(height - 1, 1), .25, 1.0))
                depth.extend(struct.pack("<f", .1 + .8 * y / max(height - 1, 1)))
                motion.extend(struct.pack("<2f", -1.0 if index else 0.0, 0.0))
        frame = {"index": index, "reset": index == 0, "jitter": [0.0, 0.0], "motion_scale": [1.0, 1.0],
                 "subrect": [0, 0, width, height], "resources": {}}
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


def parameter_matrix() -> dict:
    cases = []
    for model in ("A", "B"):
        for auto in (False, True):
            for blend in range(4):
                for mode in MODES:
                    cases.append({"id": f"{model}-auto{int(auto)}-blend{blend}-{mode}",
                                  "width": 19, "height": 17, "mode": mode, "rgb": [.25, .5, .75],
                                  "rect": [9, 8], "model": model, "auto_mask": auto, "blend_mode": blend})
    for width, height in ((1, 3), (3, 1), (16, 16)):
        for mode in ("green_rect", "blue_rect"):
            cases.append({"id": f"boundary-{width}x{height}-{mode}", "width": width, "height": height, "mode": mode})
    for model in ("A", "B"):
        for name, rgb in (("all-one", [1, 1, 1]), ("r-zero", [0, 1, 1]), ("r-half", [.5, 1, 1]),
                          ("g-zero", [1, 0, 1]), ("b-zero", [1, 1, 0])):
            cases.append({"id": f"{model}-{name}", "width": 19, "height": 17, "mode": "rgb",
                          "rgb": rgb, "model": model})
    return validate_fixture({"schema": FIXTURE_SCHEMA, "fixture_id": "control-mask-matrix-v1",
                             "description": "Shader bytes and MOCK model/AutoMask/BlendMode contracts; no NGX output",
                             "cases": cases})


def verify_sequence(directory: Path) -> dict:
    manifest = read_json(directory / "sequence.json")
    if manifest.get("schema") != "nr-nexus-offline-sequence/1":
        raise ContractError("Unsupported sequence schema")
    unsigned = {key: value for key, value in manifest.items() if key != "sequence_sha256"}
    if manifest.get("sequence_sha256") != identity(unsigned):
        raise ContractError("Sequence identity mismatch")
    width = integer(manifest.get("width"), 1, 8192, "width")
    height = integer(manifest.get("height"), 1, 8192, "height")
    frames = manifest.get("frames")
    if not isinstance(frames, list) or not 1 <= len(frames) <= 4096:
        raise ContractError("Sequence needs 1..4096 frames")
    formats = {"color": ("R32G32B32A32_FLOAT", 16), "depth": ("R32_FLOAT", 4), "motion": ("R32G32_FLOAT", 8)}
    for index, frame in enumerate(frames):
        if frame["index"] != index or type(frame["reset"]) is not bool:
            raise ContractError("Invalid frame index or Reset")
        for field in ("jitter", "motion_scale"):
            values = frame[field]
            if not isinstance(values, list) or len(values) != 2 or any(
                type(value) not in (int, float) or not math.isfinite(value) for value in values
            ):
                raise ContractError(f"Invalid {field}")
        if frame["subrect"] != [0, 0, width, height] or set(frame["resources"]) != set(formats):
            raise ContractError("Unsupported sequence subrect or resources")
        for name, (fmt, stride) in formats.items():
            resource = frame["resources"][name]
            path = artifact_path(directory, resource["file"])
            if (resource["format"] != fmt or resource["bytes"] != width * height * stride
                    or path.stat().st_size != resource["bytes"] or digest(path) != resource["sha256"]):
                raise ContractError(f"Sequence resource mismatch: frame {index} {name}")
    return manifest
