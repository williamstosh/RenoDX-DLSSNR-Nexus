"""Optional scRGB JPEG XR -> Ultra HDR support for game-compare.py.

Hable SDR-base math and 80/203-nit scaling adapted from jxr2uhdr:
https://github.com/tfx2001/jxr2uhdr/tree/4b7519195a52eee6b8f5316412e02ef738eb149d
Copyright (c) 2026 tfx2001, MIT. Full notice: THIRD_PARTY_NOTICES.md.
JPEG XR and Ultra HDR use the imagecodecs jxrlib/libultrahdr bindings.
"""
from __future__ import annotations

import hashlib
import io
import math
from pathlib import Path
import struct

try:
    import imagecodecs as codecs
    import numpy as np
except ImportError as exc:
    raise ValueError("HDR requires imagecodecs and NumPy: python -m pip install -r tools/game-compare-hdr-requirements.txt") from exc
from PIL import Image

# Linear D65 Rec.709 -> Rec.2020: a change of primaries, not a creative grade.
TO_2020 = np.array([[.6274038959, .3292830384, .0433130657],
                    [.0690972894, .9195403951, .0113623156],
                    [.0163914389, .0880133079, .8955952532]], dtype=np.float32)
SCRGB_WHITE = 80.0
UHDR_WHITE = 203.0
ROUNDING_TOLERANCE = 1e-5


def jxr_header(raw):
    if raw[:4] != b"II\xbc\x01" or len(raw) < 10:
        raise ValueError("Not a supported little-endian JPEG XR container.")
    offset = struct.unpack_from("<I", raw, 4)[0]
    if offset + 2 > len(raw):
        raise ValueError("Invalid JPEG XR directory.")
    count = struct.unpack_from("<H", raw, offset)[0]
    if offset + 2 + count * 12 > len(raw):
        raise ValueError("Truncated JPEG XR directory.")
    entries = {tag: (kind, size, value) for i in range(count)
               for tag, kind, size, value in [struct.unpack_from("<HHII", raw, offset + 2 + i * 12)]}
    if entries.get(0xbc02, (0, 0, 0))[2] != 0:
        raise ValueError("Rotated/mirrored JXR is unsupported; no silent geometric transformation is allowed.")
    if 0x8773 in entries:
        raise ValueError("JXR with an embedded ICC profile needs explicit color-space review; --hdr expects Windows scRGB.")
    kind, size, value = entries.get(0xbc01, (0, 0, 0))
    if kind != 1 or size != 16 or value + 16 > len(raw):
        raise ValueError("Missing JPEG XR pixel format GUID.")
    return {"pixel_format_guid_bytes": raw[value:value + 16].hex(), "orientation": 0}


def decode_jxr(path):
    path = Path(path)
    if path.suffix.casefold() != ".jxr" or not path.is_file():
        raise ValueError(f"HDR input must be an accessible .jxr file: {path.name}")
    raw = path.read_bytes()
    header = jxr_header(raw)
    try:
        image = codecs.jpegxr_decode(raw)
    except (ValueError, RuntimeError) as exc:
        raise ValueError(f"Cannot decode HDR JXR {path.name}: {exc}") from exc
    if image.ndim != 3 or image.shape[2] != 4 or image.dtype not in (np.float16, np.float32):
        raise ValueError("HDR input must be float16/float32 RGBA Windows scRGB, not an integer SDR JXR.")
    if min(image.shape[:2]) < 64 or max(image.shape[:2]) > 16384:
        raise ValueError("HDR image dimensions must be 64–16384 pixels.")
    image = image.astype(np.float32)
    if not np.isfinite(image).all() or not np.all(image[..., 3] == 1):
        raise ValueError("HDR input must contain finite RGB values and fully opaque alpha.")
    info = {"filename": path.name, "sha256": hashlib.sha256(raw).hexdigest(),
            "dimensions": [image.shape[1], image.shape[0]], "format": "JPEG XR",
            "mode": "linear scRGB RGBA", "decoded_dtype": "float32",
            "rgb_min": float(image[..., :3].min()), "rgb_max": float(image[..., :3].max()),
            "negative_scRGB_channels": int((image[..., :3] < 0).sum()), **header}
    return image, info


def tonemap_sdr(image):
    """One fixed, max-RGB Hable curve; no per-image exposure or white balance."""
    rgb = image[..., :3]
    peak = rgb.max(axis=-1, keepdims=True)
    positive = np.maximum(peak, 0)
    def hable(x):
        return ((x * (.15 * x + .05) + .004) / (x * (.15 * x + .5) + .06)) - .02 / .30
    scale = np.minimum(hable(positive) / hable(11.2), 1) / np.maximum(positive, 1e-20)
    linear = np.clip(rgb * scale, 0, 1)
    srgb = np.where(linear <= .0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - .055)
    result = np.empty(image.shape, np.uint8)
    result[..., :3] = np.rint(srgb * 255).astype(np.uint8)
    result[..., 3] = 255
    return result


def hdr_intent(image):
    rgb = image[..., :3] @ TO_2020.T
    minimum = float(rgb.min())
    if minimum < -ROUNDING_TOLERANCE:
        raise ValueError(f"HDR colors extend outside Rec.2020 (minimum {minimum:.7g}); no automatic gamut clipping was performed.")
    rounded = int((rgb < 0).sum())
    result = np.empty(image.shape, np.float16)
    result[..., :3] = (np.maximum(rgb, 0) * (SCRGB_WHITE / UHDR_WHITE)).astype(np.float16)
    result[..., 3] = 1
    return result, rounded


def prepare_pair(off_path, on_path, quality=95, *, gainmap_scale=1, gainmap_quality=100,
                 gainmap_channels=3, gainmap_gamma=1):
    if not isinstance(quality, int) or not 1 <= quality <= 100:
        raise ValueError("HDR quality must be 1–100.")
    if not isinstance(gainmap_scale, int) or not 1 <= gainmap_scale <= 128:
        raise ValueError("gainmap_scale must be an integer from 1 to 128.")
    if not isinstance(gainmap_quality, int) or not 1 <= gainmap_quality <= 100:
        raise ValueError("gainmap_quality must be 1–100.")
    if gainmap_channels not in (1, 3) or not math.isfinite(gainmap_gamma) or gainmap_gamma <= 0:
        raise ValueError("gainmap_channels must be 1 or 3; gainmap_gamma must be finite and positive.")
    if not codecs.JPEGXR.available or not codecs.ULTRAHDR.available:
        raise ValueError("Installed imagecodecs lacks JPEG XR or Ultra HDR codec support.")
    off, off_info = decode_jxr(off_path)
    on, on_info = decode_jxr(on_path)
    if off.shape != on.shape:
        raise ValueError("HDR images must have the same full dimensions; no resize, crop or registration is supported.")
    if off_info["sha256"] == on_info["sha256"] or np.array_equal(off, on):
        raise ValueError("HDR OFF and ON contain identical data.")
    # Shared peak prevents independent display-capacity normalization. Do not discard highlights.
    peak = max(float(off[..., :3].max()), float(on[..., :3].max())) * SCRGB_WHITE
    if peak > 10000:
        raise ValueError("HDR source exceeds 10,000 nits; explicit source review is required.")
    target = max(UHDR_WHITE, math.ceil(peak))
    off_sdr, on_sdr = tonemap_sdr(off), tonemap_sdr(on)
    off_hdr, off_rounded = hdr_intent(off)
    on_hdr, on_rounded = hdr_intent(on)
    off_info["rec2020_rounding_clamped_channels"] = off_rounded
    on_info["rec2020_rounding_clamped_channels"] = on_rounded
    options = {"quality": quality, "gainmap_quality": gainmap_quality, "gainmap_scale": gainmap_scale,
               "gainmap_channels": gainmap_channels, "gainmap_gamma": gainmap_gamma, "shared_target_peak_nits": target,
               "source": "Windows linear scRGB / Rec.709 D65 / 1.0 = 80 nits (explicit --hdr declaration)",
               "hdr_intent": "linear Rec.2020 D65 / 1.0 = 203 nits",
               "rec709_to_rec2020_matrix": TO_2020.tolist(), "hdr_rgb_scale": SCRGB_WHITE / UHDR_WHITE,
               "sdr_base": "fixed max-RGB Hable, exposure=1, white=11.2, sRGB transfer, 8-bit",
               "sdr_gamut": "Rec.709; negative channels clamped only in SDR base",
               "rec2020_rounding_tolerance": ROUNDING_TOLERANCE,
               "imagecodecs_version": codecs.__version__, "numpy_version": np.__version__,
               "jpegxr_version": codecs.jpegxr_version(), "ultrahdr_version": codecs.ultrahdr_version(),
               "module_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    notes = ["Ultra HDR JPEG: HDR display requires a compatible browser, OS HDR setting and display; otherwise the embedded SDR base is shown.",
             "SDR/README previews are tone-mapped with the same fixed curve; they are not HDR screenshots."]
    if off_rounded or on_rounded:
        notes.append(f"Rec.2020 rounding residuals within {ROUNDING_TOLERANCE:g} were clamped to zero: OFF {off_rounded}, ON {on_rounded} channels.")
    return {"off": off, "on": on, "off_hdr": off_hdr, "on_hdr": on_hdr,
            "off_sdr": off_sdr, "on_sdr": on_sdr, "sources": {"off": off_info, "on": on_info},
            "options": options, "notes": notes}


def encode(hdr, sdr, options):
    return codecs.ultrahdr_encode(
        np.ascontiguousarray(hdr), level=options["quality"], sdr=np.ascontiguousarray(sdr),
        gamut=codecs.ULTRAHDR.CG.BT_2100, nits=options["shared_target_peak_nits"],
        scale=options["gainmap_scale"], multichannel=options["gainmap_channels"] == 3,
        gainmap_gamma=options["gainmap_gamma"], gainmap_level=options["gainmap_quality"])


def inspect(data):
    if not codecs.ultrahdr_check(data):
        raise ValueError("Output has no valid Ultra HDR gain map.")
    base, gainmap, metadata = codecs.ultrahdr_decode_jpeg(data)
    for blob in (base, gainmap):
        with Image.open(io.BytesIO(blob)) as image:
            if image.getexif():
                raise ValueError("Unexpected EXIF in generated HDR JPEG.")
    with Image.open(io.BytesIO(base)) as image:
        dimensions = list(image.size)
    with Image.open(io.BytesIO(gainmap)) as image:
        gainmap_dimensions = list(image.size)
    return {"dimensions": dimensions, "gainmap_dimensions": gainmap_dimensions,
            "base_bytes": len(base), "gainmap_bytes": len(gainmap),
            "gainmap_metadata": metadata}


def roundtrip_metrics(data, reference):
    decoded = codecs.ultrahdr_decode(data)
    if decoded.shape != reference.shape or not np.isfinite(decoded).all():
        raise ValueError("HDR output failed floating-point decode validation.")
    # Decode is linear Rec.2020 in the same 203-nit units as the supplied intent.
    error = np.abs(decoded[..., :3].astype(np.float32) - reference[..., :3].astype(np.float32))
    return {"linear_rgb_mean_absolute_error": float(error.mean()),
            "linear_rgb_p99_absolute_error": float(np.quantile(error, .99)),
            "decoded_max_channel_nits": float(decoded[..., :3].max()) * UHDR_WHITE,
            "reference_max_channel_nits": float(reference[..., :3].max()) * UHDR_WHITE,
            "sample_count_channels": int(error.size),
            "units": "linear Rec.2020, 1.0 = 203 nits; lossy JPEG/gainmap, not pixel-identical"}


def write_pair(directory, bundle, overlay=None, sdr_preview=None):
    options = bundle["options"]
    results = {}
    for side in ("off", "on"):
        data = encode(bundle[side + "_hdr"], bundle[side + "_sdr"], options)
        results[side] = inspect(data)
        results[side]["roundtrip"] = roundtrip_metrics(data, bundle[side + "_hdr"])
        (directory / (side + ".jpg")).write_bytes(data)
    if overlay is None:
        return results
    # Compose the original linear HDR planes, then the same visual annotation in light.
    split = bundle["off"].shape[1] // 2
    combined = bundle["off"].copy()
    combined[:, split:] = bundle["on"][:, split:]
    annotation = np.asarray(overlay, np.float32) / 255
    alpha = annotation[..., 3:4]
    srgb = annotation[..., :3]
    linear = np.where(srgb <= .04045, srgb / 12.92, ((srgb + .055) / 1.055) ** 2.4)
    combined[..., :3] *= 1 - alpha
    combined[..., :3] += linear * (UHDR_WHITE / SCRGB_WHITE) * alpha
    combined_hdr, rounded = hdr_intent(combined)
    combined_sdr = np.asarray(sdr_preview.convert("RGBA"))
    data = encode(combined_hdr, combined_sdr, options)
    results["preview"] = inspect(data)
    results["preview"]["rec2020_rounding_clamped_channels"] = rounded
    (directory / "preview-hdr.jpg").write_bytes(data)
    return results
