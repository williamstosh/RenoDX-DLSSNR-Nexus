"""Bounded float32 reference for the production look compose shadow control.

Usage: generate --output fixture.json
       compare --fixture fixture.json --gpu gpu.json --output comparison.json
       selftest

Inputs/outputs are sRGB float triples, as consumed/written by look_compose.
Inputs and final outputs are quantized to RGBA16_FLOAT (binary16); linear
metadata is decoded from the stored inputs. All other look gains are 1,
knees/halo/flags are 0. Midtones and
Highlights stay 1. No bands, history, transport, codec encode/resolve, or G/B
ControlMask behavior is modeled. The recovery baseline is the codec proxy,
not the original image. At/below the ratio floor, trust=0 retains neural.

Independent implementation of shaders/{look_common.hlsli,codec_math.hlsli,
look_compose.cs_5_1.hlsl} under the repository MIT license. No old oracle or
EditRig code was imported. float32 operations round separately; GPU fused
operations and transcendental approximations require numerical tolerances.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def half(value):
    return struct.unpack("<e", struct.pack("<e", value))[0]


def half_ulps(a, b):
    # sRGB fixture/output values are nonnegative, so half bits are ordered.
    return abs(struct.unpack("<H", struct.pack("<e", a))[0]
               - struct.unpack("<H", struct.pack("<e", b))[0])


FLOOR = f32(1 / 1024)
TRUST_TOP = f32(1 / 64)
W = tuple(map(f32, (0.212639, 0.715169, 0.072192)))
GREY_LOG = f32(-2.4739312)
SHADOWS = (1.0, 0.5, 0.0)
ROOT = Path(__file__).resolve().parents[2]
SOURCES = ("shaders/look_common.hlsli", "shaders/codec_math.hlsli",
           "shaders/look_compose.cs_5_1.hlsl")


def dot(a, b):
    return f32(f32(f32(a[0] * b[0]) + f32(a[1] * b[1])) + f32(a[2] * b[2]))


def smoothstep(lo, hi, value):
    t = f32(max(0.0, min(1.0, f32(f32(value - lo) / f32(hi - lo)))))
    return f32(f32(t * t) * f32(3.0 - f32(2.0 * t)))


def encode(value):
    x = f32(max(0.0, min(1.0, value)))
    return f32(x * f32(12.92)) if x <= f32(0.0031308) else f32(
        f32(f32(1.055) * f32(x ** f32(1 / 2.4))) - f32(0.055))


def decode(value):
    x = f32(max(0.0, min(1.0, value)))
    return f32(x / f32(12.92)) if x <= f32(0.04045) else f32(
        f32(f32(x + f32(0.055)) / f32(1.055)) ** f32(2.4))


def reference(proxy, neural, shadows):
    """Neutral ShapeEdit algebra, rounded at each float32 operation."""
    p, n = [list(map(decode, rgb)) for rgb in (proxy, neural)]
    lp = [f32(math.log2(max(v, FLOOR))) for v in p]
    e = [f32(f32(math.log2(max(v, FLOOR))) - base) for v, base in zip(n, lp)]
    a = dot(W, e)
    c = [f32(v - a) for v in e]
    intensity = f32(math.log2(max(dot(W, p), FLOOR)))
    level = f32(intensity - GREY_LOG)
    w_sh = f32(1.0 - smoothstep(-3.5, -1.5, level))
    w_hi = smoothstep(0.5, 2.5, level)
    gain = f32(f32(f32(w_sh * f32(shadows)) + f32(f32(1.0 - w_sh) - w_hi)) + w_hi)
    trust = smoothstep(FLOOR, TRUST_TOP, max(p))
    # Retain the production neutral ColourSplit arithmetic: its radial and
    # tangential components cancel algebraically, but can round differently.
    mean_lp = dot(W, lp)
    o = [f32(v - mean_lp) for v in lp]
    oo = dot(W, [f32(v * v) for v in o])
    scale = f32(dot(W, [f32(x * y) for x, y in zip(c, o)]) / max(oo, f32(1e-6)))
    radial = [f32(scale * v) for v in o]
    tangent = [f32(v - r) for v, r in zip(c, radial)]
    split = smoothstep(f32(0.05), f32(0.25), f32(math.sqrt(oo)))
    colour = [f32(f32(split * f32(r + t)) + f32(f32(1 - split) * v))
              for r, t, v in zip(radial, tangent, c)]
    shaped = [f32(f32(a * gain) + f32(v * gain)) for v in colour]
    linear = [f32(v * f32(2 ** f32(trust * f32(s - edit))))
              for v, s, edit in zip(n, shaped, e)]
    # Identity look is bypassed by production; its baseline copies neural.
    output = list(map(half, neural)) if shadows == 1 else [half(encode(v)) for v in linear]
    return {"output": output, "zone_gain": gain,
            "floor_trust": trust, "proxy_linear": p, "neural_linear": n,
            "output_linear": linear}


def generate():
    levels = [("black", 0), ("below_floor", FLOOR / 2), ("at_floor", FLOOR),
              ("just_above_floor", FLOOR * 1.01), ("floor_transition_low", 0.002),
              ("floor_transition_mid", (FLOOR + TRUST_TOP) / 2),
              ("trust_top", TRUST_TOP), ("normal_dark", 0.01575),
              ("shadow_boundary_low", 0.18 * 2 ** -3.5),
              ("shadow_mid_transition", 0.18 * 2 ** -2.5),
              ("shadow_boundary_high", 0.18 * 2 ** -1.5),
              ("midgrey", 0.18), ("bright", 0.7)]
    proxies = [(name, [v] * 3) for name, v in levels]
    proxies += [("mixed_channel_floor", [0.02, FLOOR / 2, 0.004]),
                ("coloured_dark", [0.025, 0.018, 0.01])]
    samples = []
    for region, p in proxies:
        variants = {"identity": p, "brighten": [v * 1.25 + 0.002 for v in p],
                    "darken": [v * 0.5 for v in p],
                    "tint": [p[0] * 1.4 + 0.002, p[1] * 0.7, p[2] * 1.1 + 0.001]}
        for edit, n in variants.items():
            proxy, neural = [[half(encode(v)) for v in rgb] for rgb in (p, n)]
            samples.append({"id": f"{region}/{edit}", "region": region,
                            "proxy": proxy, "neural": neural,
                            "proxy_linear": list(map(decode, proxy)),
                            "neural_linear": list(map(decode, neural))})
    return {"schema": "look-shadow-v1", "space": "srgb", "format": "RGBA16_FLOAT",
            "shadows": list(SHADOWS), "settings": {
                "Flags": 0, "Strength": 1, "Brighten": 1, "Darken": 1,
                "ColourGain": 1, "HueGain": 1, "Midtones": 1, "Highlights": 1,
                "Tone": 1, "Detail": 1, "Halo": 0, "HaloBright": 0,
                "MaxBrighten": 0, "MaxDarken": 0, "MaxColour": 0},
            "baseline": "codec proxy; not the pre-codec original",
            "source_sha256": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                              for name in SOURCES}, "samples": samples}


def rgb(value):
    if not isinstance(value, list) or len(value) != 3 or any(
            isinstance(v, bool) or not isinstance(v, (float, int)) or
            not math.isfinite(v) or not 0 <= v <= 1 for v in value):
        raise ValueError("RGB must contain three finite sRGB values in [0, 1]")
    return value


def compare(fixture, gpu, atol=0.0, rtol=0.0, max_half_ulps=1):
    samples = {}
    for sample in fixture["samples"]:
        key = sample["id"]
        if key in samples:
            raise ValueError(f"duplicate fixture id: {key}")
        rgb(sample["proxy"])
        rgb(sample["neural"])
        if any(half(v) != v for field in ("proxy", "neural") for v in sample[field]):
            raise ValueError(f"fixture input is not half quantized: {key}")
        samples[key] = sample
    if not samples:
        raise ValueError("empty fixture")
    expected = {(key, sh) for key in samples for sh in SHADOWS}
    seen, rows = set(), []
    for row in gpu["rows"]:
        key = (row["id"], row["shadows"])
        if key not in expected or key in seen:
            raise ValueError(f"unexpected or duplicate GPU row: {key}")
        seen.add(key)
        sample = samples[key[0]]
        ref = reference(sample["proxy"], sample["neural"], key[1])
        actual = rgb(row["output"])
        errors = [abs(a - b) for a, b in zip(actual, ref["output"])]
        ulps = [half_ulps(a, b) for a, b in zip(actual, ref["output"])]
        passed = all((err <= atol + rtol * abs(b) or ulp <= max_half_ulps)
                     for err, ulp, b in zip(errors, ulps, ref["output"]))
        passed &= all(half(v) == v for v in actual)
        dispatch_ok = isinstance(row.get("dispatched"), bool) and row["dispatched"] == (key[1] != 1)
        passed &= dispatch_ok
        baseline_ok = key[1] != 1 or actual == sample["neural"]
        passed &= baseline_ok
        # Optional instrumented values use the same tolerance as RGB.
        for field in ("zone_gain", "floor_trust"):
            if field in row:
                value = row[field]
                passed &= (isinstance(value, (int, float)) and math.isfinite(value)
                           and abs(value - ref[field]) <= 3e-6 + 3e-5 * abs(ref[field]))
        linear = list(map(decode, actual))
        rows.append({"id": key[0], "shadows": key[1], "pass": bool(passed),
                     "max_abs_srgb_error": max(errors), "expected": ref["output"],
                     "max_half_ulps": max(ulps), "dispatch_pass": dispatch_ok,
                     "baseline_copy_pass": baseline_ok,
                     "actual": actual, "zone_gain": ref["zone_gain"],
                     "floor_trust": ref["floor_trust"],
                     "max_linear_distance_to_proxy": max(abs(a - b) for a, b in zip(linear, ref["proxy_linear"])),
                     "max_linear_distance_to_neural": max(abs(a - b) for a, b in zip(linear, ref["neural_linear"]))})
    missing = sorted(expected - seen)
    return {"pass": not missing and all(r["pass"] for r in rows),
            "evidence": "CPU_REFERENCE_COMPARISON; GPU identity supplied by caller",
            "gpu_identity": {k: v for k, v in gpu.items() if k != "rows"},
            "tolerance": {"atol_srgb": atol, "rtol": rtol, "max_half_ulps": max_half_ulps},
            "expected_rows": len(expected), "compared_rows": len(rows),
            "missing": missing, "failed_rows": sum(not r["pass"] for r in rows),
            "max_abs_srgb_error": max((r["max_abs_srgb_error"] for r in rows), default=None),
            "max_half_ulps": max((r["max_half_ulps"] for r in rows), default=None),
            "rows": rows}


def selftest():
    fixture = generate()
    for sample in fixture["samples"]:
        for sh in SHADOWS:
            ref = reference(sample["proxy"], sample["neural"], sh)
            assert all(math.isfinite(v) for v in ref["output"])
            if sh == 1 or ref["floor_trust"] == 0:
                assert ref["output"] == sample["neural"]
    # Full trust and full shadow weight overlap only in this narrow interval:
    # 1/64 <= luminance <= 0.18 * 2**-3.5 (about 0.01591).
    level = 0.01575
    p, n = [encode(level)] * 3, [encode(level * 2)] * 3
    for sh in SHADOWS:
        ref = reference(p, n, sh)
        assert abs(ref["output_linear"][0] - level * 2 ** sh) < 2e-7
    ref = reference([encode(FLOOR / 2)] * 3, [encode(0.01)] * 3, 0)
    assert ref["floor_trust"] == 0 and abs(ref["output_linear"][0] - 0.01) < 1e-7
    return {"pass": True, "evidence": "CPU_REFERENCE_SELFTEST", "samples": len(fixture["samples"]),
            "evaluations": len(fixture["samples"]) * len(SHADOWS)}


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    gen = commands.add_parser("generate")
    gen.add_argument("--output", type=Path, required=True)
    cmp = commands.add_parser("compare")
    cmp.add_argument("--fixture", type=Path, required=True)
    cmp.add_argument("--gpu", type=Path, required=True)
    cmp.add_argument("--output", type=Path, required=True)
    cmp.add_argument("--atol", type=float, default=0.0)
    cmp.add_argument("--rtol", type=float, default=0.0)
    cmp.add_argument("--max-half-ulps", type=int, default=1)
    commands.add_parser("selftest")
    args = parser.parse_args()
    try:
        if args.command == "generate":
            result = generate()
        elif args.command == "selftest":
            result = selftest()
        else:
            if not all(math.isfinite(v) and v >= 0 for v in (args.atol, args.rtol)):
                raise ValueError("tolerances must be finite and nonnegative")
            if args.max_half_ulps < 0:
                raise ValueError("half ULP tolerance must be nonnegative")
            result = compare(json.loads(args.fixture.read_text(encoding="utf-8-sig")),
                             json.loads(args.gpu.read_text(encoding="utf-8-sig")),
                             args.atol, args.rtol, args.max_half_ulps)
        if args.command != "selftest":
            write_json(args.output, result)
        print(json.dumps({k: v for k, v in result.items() if k not in ("rows", "samples")}, allow_nan=False))
        return 0 if result.get("pass", True) else 1
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"shadow reference: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
