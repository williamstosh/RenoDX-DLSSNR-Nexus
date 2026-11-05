"""ControlMask fixture schema and output oracle."""

from __future__ import annotations

import math
import re
import struct
from typing import Any

from contracts import ContractError, compare_raw, integer

FIXTURE_SCHEMA = "nr-nexus-offline-fixture/1"
MODES = ("rgb", "neutral", "rect", "green_rect", "blue_rect")
# D3D12_FLOAT32_TO_INTEGER_TOLERANCE_IN_ULP; .5 may legally become 127 or 128.
# https://learn.microsoft.com/en-us/windows/win32/direct3d12/constants
UNORM_CONVERSION_TOLERANCE = struct.unpack("<f", struct.pack("<f", .6))[0]
IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,95}\Z")
CASE_DEFAULTS = {"mode": "neutral", "rgb": [1.0, 1.0, 1.0], "rect": [0, 0],
                 "model": "A", "auto_mask": False, "blend_mode": 0, "frames": 8, "warmup": 2}


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


def mask_values(case: dict):
    # Match the actual float32 root constants before considering UNORM conversion.
    fixed = [struct.unpack("<f", struct.pack("<f", channel))[0] * 255 for channel in case["rgb"]] + [255]
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
            yield pixel


def mask_reference(case: dict) -> bytes:
    """Nearest-even reference bytes, not a requirement for bitwise GPU equality."""
    if case["mode"] == "neutral":
        return bytes([255]) * (case["width"] * case["height"] * 4)
    return bytes(round(value) for pixel in mask_values(case) for value in pixel)


def compare_mask(case: dict, actual: bytes) -> dict:
    if len(actual) != case["width"] * case["height"] * 4:
        raise ContractError("Mask readback length mismatch")
    if case["mode"] == "neutral":
        reference = mask_reference(case)
        if actual == reference:
            return {"reference_policy": "d3d-float-to-unorm-0.6-ulp", "format_conformant": True,
                    "invalid_components": 0, "max_quantization_error_ulp": 0.0,
                    "conversion_tolerance_ulp": UNORM_CONVERSION_TOLERANCE,
                    "nearest_even_reference": {"components": len(actual), "different_components": 0,
                        "max_absolute_error": 0, "mae": 0.0, "rmse": 0.0, "psnr_db": None,
                        "identical": True, "data_range": 255.0, "storage": "u8"}}
    invalid, maximum = 0, 0.0
    for index, ideal in enumerate(value for pixel in mask_values(case) for value in pixel):
        error = abs(actual[index] - ideal)
        maximum = max(maximum, error)
        invalid += error > UNORM_CONVERSION_TOLERANCE
    return {"reference_policy": "d3d-float-to-unorm-0.6-ulp", "format_conformant": invalid == 0,
            "invalid_components": invalid, "max_quantization_error_ulp": maximum,
            "conversion_tolerance_ulp": UNORM_CONVERSION_TOLERANCE,
            "nearest_even_reference": compare_raw(mask_reference(case), actual, "u8", 255.0)}


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
