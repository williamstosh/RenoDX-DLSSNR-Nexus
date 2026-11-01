"""Offline, review-first NR Nexus comparisons. Requires Pillow; no game/runtime needed."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import unicodedata
from urllib.parse import quote, urlparse
from urllib.request import urlopen
import warnings

try:
    from PIL import Image, ImageChops, ImageCms, ImageDraw, ImageFilter, ImageFont, ImageOps, ImageStat
    import PIL
except ImportError as exc:
    raise SystemExit("Pillow is required. Use your local Python environment: python -m pip install Pillow") from exc

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "docs/compare"
DEFAULT_OUTPUT = ROOT / "artifacts/game-comparisons"
DATA_RE = re.compile(r'(<script id="catalog-data" type="application/json">).*?(</script>)', re.S)
STYLE_VERSION = 1


class CompareError(ValueError):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def json_text(data):
    return json.dumps(data, ensure_ascii=False, indent=2) + "\n"


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path, data):
    path.write_text(json_text(data), encoding="utf-8")


def clean_text(value):
    value = unicodedata.normalize("NFC", value).strip()
    if not value or len(value) > 160 or re.search(r"[\x00-\x1f<>]|[A-Za-z]:[\\/]|\\\\", value):
        raise CompareError("Names/metadata must be plain text, without local paths (1–160 characters).")
    return value


def slug(value):
    value = unicodedata.normalize("NFKC", value).casefold()
    ascii_part = re.sub(r"[^a-z0-9]+", "-", value).strip("-")[:48]
    # Non-ASCII names keep a hash: transliteration and Windows reserved names cannot collide.
    if not value.isascii() or not ascii_part or ascii_part in {"con", "prn", "aux", "nul"} or re.fullmatch(r"(?:com|lpt)[1-9]", ascii_part):
        return (ascii_part or "scene") + "-" + digest(value.encode())[:10]
    return ascii_part


def local_output(path):
    path = path.resolve()
    if path.is_relative_to(ROOT) and not any(path.is_relative_to(ROOT / p) for p in ("artifacts", "tmp", ".local")):
        raise CompareError("Candidate output inside this repository must be under artifacts/, tmp/ or .local/.")
    if path == ROOT or path == PUBLIC or path.is_relative_to(PUBLIC):
        raise CompareError("Candidates cannot be written to the public site.")
    return path


def filename_state(path):
    tokens = re.findall(r"(?:^|[\W_])(off|on)(?=$|[\W_])", path.stem.casefold())
    return set(tokens)


def load_image(path, state):
    if not path.is_file():
        raise CompareError(f"{state}: file does not exist: {path}")
    if path.suffix.casefold() not in {".png", ".jpg", ".jpeg", ".webp"}:
        raise CompareError(f"{state}: use PNG, JPEG or WebP.")
    states = filename_state(path)
    if states and states != {state.casefold()}:
        raise CompareError(f"{state}: filename state is conflicting or reversed: {path.name}")
    raw = path.read_bytes()
    # Pillow RGB is eight-bit. Do not silently truncate high-bit PNG or HDR gain maps.
    if raw.startswith(b"\x89PNG") and len(raw) > 25 and raw[24] > 8:
        raise CompareError(f"{state}: high-bit-depth PNG/HDR is unsupported; supply an explicitly tone-mapped SDR export.")
    lower = raw.lower()
    if any(marker in lower for marker in (b"hdrgm:", b"hdrgainmap", b"gainmap:", b"urn:iso:std:iso:ts:21496")):
        raise CompareError(f"{state}: HDR gain-map image is unsupported; supply an SDR export.")
    if raw.startswith(b"\x89PNG"):
        pos = 8
        while pos + 12 <= len(raw):
            length = int.from_bytes(raw[pos:pos + 4], "big")
            kind = raw[pos + 4:pos + 8]
            payload = raw[pos + 8:pos + 8 + length]
            if kind == b"cICP" and len(payload) >= 2 and (payload[0] != 1 or payload[1] != 13):
                raise CompareError(f"{state}: unsupported cICP transfer/primaries (including PQ/HLG HDR).")
            pos += length + 12
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            with Image.open(io.BytesIO(raw)) as source:
                source.load()
                if source.format not in {"PNG", "JPEG", "WEBP"} or getattr(source, "n_frames", 1) != 1:
                    raise CompareError(f"{state}: animated or unsupported image.")
                if source.mode not in {"RGB", "RGBA", "L", "LA", "P"}:
                    raise CompareError(f"{state}: unsupported color mode {source.mode}; export SDR RGB first.")
                info = dict(source.info)
                orientation = source.getexif().get(274, 1)
                image = ImageOps.exif_transpose(source).convert("RGBA")
                if image.getchannel("A").getextrema() != (255, 255):
                    raise CompareError(f"{state}: transparent screenshots are unsupported.")
                image = image.convert("RGB")
                profile = info.get("icc_profile")
                notes = []
                if profile:
                    cms = ImageCms.ImageCmsProfile(io.BytesIO(profile))
                    description = ImageCms.getProfileDescription(cms).casefold()
                    if any(x in description for x in ("hdr", "2020", "2100", "pq", "hlg")):
                        raise CompareError(f"{state}: HDR/wide-gamut profile needs an explicit SDR export.")
                    image = ImageCms.profileToProfile(image, cms, ImageCms.createProfile("sRGB"), outputMode="RGB")
                else:
                    if "gamma" in info and abs(info["gamma"] - 0.45455) > 0.005 and "srgb" not in info:
                        raise CompareError(f"{state}: non-sRGB gamma without ICC is unsupported.")
                    notes.append("No ICC profile: interpreted as SDR sRGB; confirm the source is an SDR export.")
                image.info.clear()
                return image, {"filename": path.name, "sha256": digest(raw), "dimensions": list(source.size),
                               "mode": source.mode, "format": source.format, "orientation": orientation,
                               "icc_sha256": digest(profile) if profile else None}, notes
    except (OSError, SyntaxError, Image.DecompressionBombError, Image.DecompressionBombWarning, ImageCms.PyCMSError) as exc:
        raise CompareError(f"{state}: cannot decode/process image: {exc}") from exc


def alignment(off, on):
    size = (160, max(32, round(160 * off.height / off.width)))
    a, b = (im.convert("L").resize(size, Image.Resampling.BOX) for im in (off, on))
    base = ImageStat.Stat(ImageChops.difference(a, b)).mean[0] / 255
    changed = sum(v for i, v in enumerate(ImageChops.difference(a, b).histogram()) if i > 32) / (size[0] * size[1])
    edge_a, edge_b = (im.filter(ImageFilter.FIND_EDGES) for im in (a, b))
    border = 6
    box = (border, border, size[0] - border, size[1] - border)
    center = edge_a.crop(box)
    def error(dx, dy):
        other = edge_b.crop((box[0] + dx, box[1] + dy, box[2] + dx, box[3] + dy))
        return ImageStat.Stat(ImageChops.difference(center, other)).mean[0]
    zero = error(0, 0)
    best = min((error(dx, dy), dx, dy) for dx in range(-4, 5) for dy in range(-4, 5))
    issues = []
    if base > 0.16 or changed > 0.42:
        issues.append("Large scene/color differences: check camera, world motion, capture timing and menu overlays.")
    if best[1:] != (0, 0) and zero > 1 and best[0] < zero * 0.70:
        issues.append("Possible edge/camera offset; no alignment or crop was applied.")
    return {"mean_absolute_luma_difference": round(base, 5), "changed_fraction": round(changed, 5),
            "best_thumbnail_offset": list(best[1:]), "thumbnail_size": list(size),
            "method": "coarse luma/edge heuristic; not a scene or alignment certification"}, issues


def find_font(override=None):
    choices = [override] if override else [os.path.join(os.environ.get("WINDIR", "C:/Windows"), "Fonts/seguisb.ttf"),
                                           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]
    for choice in choices:
        if choice and Path(choice).is_file():
            return Path(choice)
    raise CompareError("No suitable system font found; pass --font with a locally licensed font file.")


def preview_overlay(size, labels, font_path):
    result = Image.new("RGBA", size)
    split = result.width // 2
    overlay = Image.new("RGBA", result.size)
    draw = ImageDraw.Draw(overlay)
    margin = max(3, round(min(result.size) * 0.018))
    font_size = max(8, round(min(result.size) * 0.026))
    while True:
        font = ImageFont.truetype(str(font_path), font_size)
        widths = [draw.textbbox((0, 0), text, font=font)[2] for text in labels.values()]
        if max(widths) + margin * 2 < result.width // 2 - margin * 2:
            break
        font_size -= 1
        if font_size < 6:
            raise CompareError("Labels do not fit this image; shorten labels or use larger screenshots.")
    height = font_size + margin * 2
    for state, x in (("off", margin), ("on", result.width - widths[1] - margin * 3)):
        width = draw.textbbox((0, 0), labels[state], font=font)[2] + margin * 2
        draw.rounded_rectangle((x, margin, x + width, margin + height), radius=margin // 2, fill=(8, 17, 27, 195))
        draw.line((x, margin, x + width, margin), fill=(77, 229, 238, 235), width=max(1, margin // 5))
        draw.text((x + margin, margin + height / 2), labels[state], font=font, fill=(242, 249, 252), anchor="lm")
    draw.line((split, 0, split, result.height), fill=(77, 229, 238, 240), width=max(1, result.width // 1600))
    small = ImageFont.truetype(str(font_path), max(7, font_size * 3 // 4))
    badge = "NR Nexus"
    width = draw.textbbox((0, 0), badge, font=small)[2] + margin * 2
    draw.rounded_rectangle((margin, result.height - height - margin, margin + width, result.height - margin),
                           radius=margin // 2, fill=(8, 17, 27, 195))
    draw.text((margin * 2, result.height - margin - height / 2), badge, font=small, fill=(242, 249, 252), anchor="lm")
    return overlay


def preview_image(off, on, labels, font_path):
    result = off.copy()
    split = result.width // 2
    result.paste(on.crop((split, 0, on.width, on.height)), (split, 0))
    return Image.alpha_composite(result.convert("RGBA"), preview_overlay(result.size, labels, font_path)).convert("RGB")


def scene_asset_keys(entry):
    return ("off", "on", "preview", "sdr_off", "sdr_on", "hdr_preview") if entry.get("dynamic_range") == "ultra-hdr" else ("off", "on", "preview")


def render_page(template, scenes, candidate=False):
    if len(DATA_RE.findall(template)) != 1:
        raise CompareError("HTML must contain exactly one catalog-data JSON element.")
    payload = json_text({"schema": 1, "scenes": scenes}).replace("<", r"\u003c").replace("&", r"\u0026")
    page = DATA_RE.sub(lambda m: m[1] + payload + m[2], template)
    return page.replace("<body>", '<body data-candidate="true">', 1) if candidate else page


def save_image(image, path, quality=None):
    # New images carry only a standardized color profile: never EXIF/GPS/XMP or source text.
    image = image.copy()
    image.info.clear()
    icc = ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()
    if path.suffix == ".png":
        image.save(path, "PNG", icc_profile=icc, optimize=True)
    else:
        image.save(path, "WEBP", lossless=quality is None, quality=100 if quality is None else quality, method=6, icc_profile=icc)


def file_manifest(directory):
    return {p.name: {"sha256": digest(p.read_bytes()), "bytes": p.stat().st_size}
            for p in sorted(directory.iterdir()) if p.is_file() and p.name != "report.json"}


def generate(args):
    off_path, on_path = Path(args.off).resolve(), Path(args.on).resolve()
    if off_path == on_path:
        raise CompareError("OFF and ON are the same file.")
    if args.format == "png" and args.quality is not None:
        raise CompareError("--quality controls lossy WebP only; omit it for PNG.")
    if not args.game or not args.scene:
        raise CompareError("Supply --game and --scene; filenames alone do not reliably identify them.")
    game, scene = clean_text(args.game), clean_text(args.scene)
    labels = {"off": clean_text(args.label_off), "on": clean_text(args.label_on)}
    metadata = {key: clean_text(value) for key in ("api", "gpu", "plugin_version", "runtime", "pass", "model", "working_resolution")
                if (value := getattr(args, key, None)) is not None}
    hdr_bundle = None
    if args.hdr:
        if args.match_size or args.max_width or args.format != "webp":
            raise CompareError("HDR mode preserves full dimensions; omit --match-size/--max-width/--format.")
        for path, state in ((off_path, "off"), (on_path, "on")):
            if filename_state(path) and filename_state(path) != {state}:
                raise CompareError(f"{state}: filename state is reversed or conflicting.")
        import game_compare_hdr as hdr
        hdr_bundle = hdr.prepare_pair(off_path, on_path, args.quality or 95)
        off, on = (Image.fromarray(hdr_bundle[s + "_sdr"]).convert("RGB") for s in ("off", "on"))
        off_info, on_info = (hdr_bundle["sources"][s] for s in ("off", "on"))
        notes_a, notes_b = hdr_bundle["notes"], []
    else:
        off, off_info, notes_a = load_image(off_path, "OFF")
        on, on_info, notes_b = load_image(on_path, "ON")
    if off_info["sha256"] == on_info["sha256"] or (not hdr_bundle and off.size == on.size and off.tobytes() == on.tobytes()):
        raise CompareError("OFF and ON contain identical image data (possibly a duplicate export).")
    if min(*off.size, *on.size) < 64:
        raise CompareError("Screenshots must be at least 64 pixels in each dimension.")
    if off_info.get("icc_sha256") != on_info.get("icc_sha256"):
        raise CompareError("ICC profiles differ; supply matching SDR exports to avoid ambiguous color conversion.")
    original_sizes = [list(off.size), list(on.size)]
    if off.size != on.size:
        if not args.match_size or off.width * on.height != on.width * off.height:
            raise CompareError("Different dimensions: no stretch/crop allowed. Matching aspect ratios may use --match-size for common downscaling.")
        target = min((off.size, on.size), key=lambda size: size[0])
        off, on = (im.resize(target, Image.Resampling.LANCZOS) if im.size != target else im for im in (off, on))
    if args.max_width and off.width > args.max_width:
        target = (args.max_width, round(off.height * args.max_width / off.width))
        off, on = (im.resize(target, Image.Resampling.LANCZOS) for im in (off, on))
    if not hdr_bundle and off.tobytes() == on.tobytes():
        raise CompareError("OFF and ON become identical after common resizing; use the full-resolution inputs.")
    metrics, issues = alignment(off, on)
    notes = list(dict.fromkeys(notes_a + notes_b)) + ["Human review required: scene/alignment, identity, overlays, color and timing are not certified by image analysis."]
    notes += issues
    notes += [clean_text(note) for note in args.note]
    font = find_font(args.font)
    template = (PUBLIC / "index.html").read_text(encoding="utf-8")
    parameters = {"style_version": STYLE_VERSION, "format": args.format, "comparison_quality": args.quality,
                  "preview_quality": 95, "max_width": args.max_width, "match_size": args.match_size,
                  "labels": labels, "metadata": metadata, "synthetic": args.synthetic, "font_sha256": digest(font.read_bytes()),
                  "template_sha256": digest(render_page(template, []).encode()), "tool_sha256": digest(Path(__file__).read_bytes()),
                  "pillow_version": PIL.__version__,
                  "color": "common SDR sRGB conversion", "crop": None, "geometric_transform": None}
    parameters["notes"] = [clean_text(note) for note in args.note]
    if hdr_bundle:
        parameters.update({"hdr": hdr_bundle["options"], "comparison_quality": args.quality or 95,
                           "color": "common scRGB to Ultra HDR Rec.2020, with fixed Hable SDR fallback"})
    identity = digest(json_text({"off": off_info["sha256"], "on": on_info["sha256"], "game": game, "scene": scene, "parameters": parameters}).encode())
    root = local_output(Path(args.output))
    relative = Path(slug(game)) / slug(scene) / identity[:16]
    candidate = root / "candidates" / relative
    entry = {"id": relative.as_posix(), "game": game, "scene": scene, "off": "off." + args.format,
             "on": "on." + args.format, "preview": "preview." + args.format, "width": off.width, "height": off.height,
             "status": "candidate", "labels": labels, "metadata": metadata, "warnings": notes, "synthetic": args.synthetic}
    if hdr_bundle:
        entry.update({"off": "off.jpg", "on": "on.jpg", "dynamic_range": "ultra-hdr",
                      "sdr_off": "off-sdr.webp", "sdr_on": "on-sdr.webp", "hdr_preview": "preview-hdr.jpg"})
    if candidate.exists():
        report = verify_candidate(candidate)
        if report["identity"] != identity:
            raise CompareError("Candidate ID collision; existing material was retained.")
    else:
        candidate.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".generate-", dir=candidate.parent) as temporary:
            work = Path(temporary)
            preview = preview_image(off, on, labels, font)
            if hdr_bundle:
                save_image(off, work / entry["sdr_off"])
                save_image(on, work / entry["sdr_on"])
                hdr_outputs = hdr.write_pair(work, hdr_bundle, preview_overlay(off.size, labels, font), preview)
            else:
                save_image(off, work / entry["off"], args.quality)
                save_image(on, work / entry["on"], args.quality)
            save_image(preview, work / entry["preview"], 95)
            (work / "index.html").write_text(render_page(template, [entry], True), encoding="utf-8")
            report = {"schema": 1, "status": "candidate", "identity": identity, "scene": entry,
                      "sources": {"off": off_info, "on": on_info}, "oriented_source_sizes": original_sizes,
                      "parameters": parameters, "alignment": metrics, "review_required": issues,
                      "outputs": file_manifest(work)}
            if hdr_bundle:
                report["hdr_outputs"] = hdr_outputs
            write_json(work / "report.json", report)
            # A new content-addressed directory; never replace an existing candidate or approved asset.
            candidate.mkdir(exist_ok=False)
            for file in work.iterdir():
                file.replace(candidate / file.name)
    records = []
    for report_path in sorted((root / "candidates").glob("*/*/*/report.json")):
        item = read_json(report_path)
        records.append({"id": item["scene"]["id"], "game": item["scene"]["game"], "scene": item["scene"]["scene"],
                        "status": "candidate", "preview": report_path.parent.relative_to(root).as_posix() + "/index.html"})
    write_json(root / "candidates.json", {"schema": 1, "scenes": records})
    print(json_text({"candidate": str(candidate), "preview": str(candidate / "index.html"), "warnings": notes, "review_required": issues}))
    return candidate


def verify_candidate(candidate):
    report = read_json(candidate / "report.json")
    if report.get("schema") != 1 or report.get("status") != "candidate" or report["scene"].get("status") != "candidate":
        raise CompareError("Not a valid unapproved candidate report.")
    if report["outputs"] != file_manifest(candidate):
        raise CompareError("Candidate files changed; regenerate from the original inputs before review.")
    entry = report["scene"]
    if not re.fullmatch(r"[a-z0-9-]+/[a-z0-9-]+/[a-f0-9]{16}", entry["id"]):
        raise CompareError("Invalid candidate identifier.")
    hdr_names = {"off": "off.jpg", "on": "on.jpg", "preview": "preview.webp",
                 "sdr_off": "off-sdr.webp", "sdr_on": "on-sdr.webp", "hdr_preview": "preview-hdr.jpg"}
    for key in scene_asset_keys(entry):
        valid_names = (hdr_names[key],) if entry.get("dynamic_range") == "ultra-hdr" else (key + ".webp", key + ".png")
        if entry[key] not in valid_names:
            raise CompareError("Unsafe candidate asset name.")
        is_hdr = entry.get("dynamic_range") == "ultra-hdr" and key in ("off", "on", "hdr_preview")
        if is_hdr:
            import game_compare_hdr as hdr
            data = hdr.inspect((candidate / entry[key]).read_bytes())
            if data["dimensions"] != [entry["width"], entry["height"]]:
                raise CompareError("Invalid HDR candidate dimensions.")
        with Image.open(candidate / entry[key]) as image:
            image.load()
            if list(image.size) != [entry["width"], entry["height"]] or image.getexif() or (not is_hdr and image.info.get("xmp")):
                raise CompareError("Invalid candidate dimensions or private image metadata.")
    return report


def approved_catalog():
    catalog = read_json(PUBLIC / "catalog.json")
    if catalog.get("schema") != 1 or any(s.get("status") != "approved" for s in catalog["scenes"]):
        raise CompareError("Public catalog may contain only explicitly approved scenes.")
    return catalog


def approve(args):
    candidate = Path(args.candidate).resolve()
    report = verify_candidate(candidate)
    entry = dict(report["scene"])
    if entry["synthetic"]:
        raise CompareError("Synthetic test material cannot enter the public comparison catalog.")
    if report["review_required"] and not args.ack_warnings:
        raise CompareError("Scene/offset warnings require an explicit human decision and --ack-warnings.")
    catalog = approved_catalog()
    asset_dir = PUBLIC / "assets" / entry["id"]
    asset_keys = scene_asset_keys(entry)
    files = [asset_dir / entry[key] for key in asset_keys]
    for key in asset_keys:
        entry[key] = "assets/" + entry["id"] + "/" + entry[key]
    # Public catalog excludes raw filenames, hashes of originals, local reports and EXIF.
    entry["status"] = "approved"
    entry["warnings"] = ((["Camera/scene differences were acknowledged during human review."] if report["review_required"] else [])
                         + report["parameters"].get("notes", []))
    existing = next((s for s in catalog["scenes"] if s["id"] == entry["id"]), None)
    if existing is not None:
        if existing != entry:
            raise CompareError("Approved ID already exists with different data; no overwrite allowed.")
        for key, target in zip(asset_keys, files):
            if not target.is_file() or digest(target.read_bytes()) != report["outputs"][Path(entry[key]).name]["sha256"]:
                raise CompareError("Existing approved assets differ; no overwrite allowed.")
        print("Already approved; no changes.")
        return
    if asset_dir.exists():
        raise CompareError("Target asset directory already exists; retained without overwriting.")
    catalog["scenes"].append(entry)
    template = (PUBLIC / "index.html").read_text(encoding="utf-8")
    page = render_page(template, catalog["scenes"])
    plan = {"add": [p.relative_to(ROOT).as_posix() for p in files],
            "update": ["docs/compare/catalog.json", "docs/compare/index.html"], "delete": [],
            "readme": "Use readme after Pages is reachable; no site URL is assumed."}
    print(json_text(plan))
    if not args.apply:
        print("DRY RUN. Review this plan before authorizing a public copy.")
        return
    if not args.reviewed or not args.publish_authorized:
        raise CompareError("--apply requires explicit human review AND public-copy authorization (--reviewed --publish-authorized).")
    asset_dir.mkdir(parents=True, exist_ok=False)
    for key, target in zip(asset_keys, files):
        target.write_bytes((candidate / Path(entry[key]).name).read_bytes())
    (PUBLIC / "index.html").write_text(page, encoding="utf-8")
    write_json(PUBLIC / "catalog.json", catalog)
    print("Approved assets copied locally. No Git operation or deployment was performed.")


def readme(args):
    catalog = approved_catalog()
    selected = [s for s in catalog["scenes"] if s["id"] in args.scene]
    if len(selected) != len(set(args.scene)) or len(selected) > 3:
        raise CompareError("Select 1–3 existing approved scene IDs for README.")
    base = args.pages_url.rstrip("/") + "/"
    url = urlparse(base)
    if url.scheme != "https" or url.hostname != "williamstosh.github.io" or url.path != "/RenoDX-DLSSNR-Nexus/compare/" or url.query or url.fragment:
        raise CompareError("Use the repository Pages /compare/ HTTPS address after it is live.")
    with urlopen(base, timeout=15) as response:
        live = response.read(8_000_000).decode("utf-8")
        if response.status != 200 or not DATA_RE.search(live):
            raise CompareError("Pages comparison page is not available; README retained.")
    live_catalog = json.loads(DATA_RE.search(live)[0].split(">", 1)[1].rsplit("<", 1)[0])
    if any(not any(l.get("id") == s["id"] and l.get("status") == "approved" for l in live_catalog["scenes"]) for s in selected):
        raise CompareError("Selected scenes are not live on Pages; README retained.")
    path = ROOT / "README.md"
    text = path.read_text(encoding="utf-8")
    for language, caption in (("zh", "左右拖动查看对比"), ("en", "Interactive Comparison")):
        lines = []
        for s in selected:
            title = re.sub(r"([\\`*_{}\[\]()#+.!|>~-])", r"\\\1", s["game"] + " — " + s["scene"])
            link = base + "#scene=" + quote(s["id"], safe="")
            lines += [title, "", f'[![{title}](docs/compare/{s["preview"]})]({link})', "", f"[{caption}]({link})", ""]
        pattern = re.compile(rf"(<!-- game-compare:{language}:start -->).*?(<!-- game-compare:{language}:end -->)", re.S)
        if len(pattern.findall(text)) != 1:
            raise CompareError("README comparison markers missing or repeated.")
        text = pattern.sub(lambda m: m[1] + "\n\n" + "\n".join(lines) + "\n" + m[2], text)
    print("Update: README.md (selected approved scenes, both languages). No additions/deletions elsewhere.")
    if args.apply:
        if not args.publish_authorized:
            raise CompareError("README --apply requires --publish-authorized after reviewing the plan.")
        path.write_text(text, encoding="utf-8")
    else:
        out = DEFAULT_OUTPUT / "readme-candidate.md"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8")
        print(f"DRY RUN: {out}")


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    sub = result.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("generate", help="Generate an offline candidate; never changes public files.")
    gen.add_argument("--off", required=True)
    gen.add_argument("--on", required=True)
    gen.add_argument("--game")
    gen.add_argument("--scene")
    gen.add_argument("--output", default=str(DEFAULT_OUTPUT))
    gen.add_argument("--format", choices=("webp", "png"), default="webp")
    gen.add_argument("--quality", type=int, choices=range(80, 101), metavar="80..100", help="Opt-in lossy WebP comparisons; default lossless.")
    gen.add_argument("--max-width", type=int, default=0, help="0 keeps full resolution; same downscale on both images.")
    gen.add_argument("--match-size", action="store_true", help="Explicit common downscale for different sizes with exactly equal aspect ratio.")
    gen.add_argument("--label-off", default="DLSS 5 OFF")
    gen.add_argument("--label-on", default="DLSS 5 ON")
    gen.add_argument("--font", help="Local system/licensed font; never copied or distributed.")
    gen.add_argument("--synthetic", action="store_true", help="Mark neutral test fixtures; cannot approve for publication.")
    gen.add_argument("--hdr", action="store_true", help="Explicit Windows scRGB JXR input; output Ultra HDR JPEG plus SDR fallbacks. Optional imagecodecs/NumPy required.")
    gen.add_argument("--note", action="append", default=[], help="Record a short, non-private capture/review note; repeat as needed.")
    for name in ("api", "gpu", "plugin-version", "runtime", "pass", "model", "working-resolution"):
        gen.add_argument("--" + name)
    app = sub.add_parser("approve", help="Print public-copy plan; --apply needs separate review/public-copy authorization.")
    app.add_argument("--candidate", required=True)
    app.add_argument("--apply", action="store_true")
    app.add_argument("--reviewed", action="store_true")
    app.add_argument("--publish-authorized", action="store_true")
    app.add_argument("--ack-warnings", action="store_true")
    md = sub.add_parser("readme", help="Prepare bilingual README entries only after selected scenes are live.")
    md.add_argument("--scene", action="append", required=True, help="Approved scene ID; repeat for up to three scenes.")
    md.add_argument("--pages-url", required=True)
    md.add_argument("--apply", action="store_true")
    md.add_argument("--publish-authorized", action="store_true")
    return result


def main():
    args = parser().parse_args()
    try:
        if args.command == "generate" and (args.max_width < 0 or 0 < args.max_width < 128):
            raise CompareError("--max-width must be 0 or at least 128.")
        {"generate": generate, "approve": approve, "readme": readme}[args.command](args)
        return 0
    except (CompareError, OSError, ValueError, KeyError, RuntimeError) as exc:
        print(f"game-compare: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
