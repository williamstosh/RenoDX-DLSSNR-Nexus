"""Synthetic-only image/export tests; never publishes or requires a game."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

from PIL import Image, ImageDraw, ImageCms, PngImagePlugin

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("game_compare", ROOT / "tools/game-compare.py")
gc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gc)


class Comparisons(unittest.TestCase):
    def setUp(self):
        (ROOT / "tmp").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "tmp")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        template = (gc.PUBLIC / "index.html").read_text(encoding="utf-8")
        public = self.root / "site-fixture"
        public.mkdir()
        (public / "index.html").write_text(gc.render_page(template, []), encoding="utf-8")
        gc.write_json(public / "catalog.json", {"schema": 1, "scenes": []})
        public_patch = patch.object(gc, "PUBLIC", public)
        public_patch.start()
        self.addCleanup(public_patch.stop)
        self.input = self.root / "中文路径 with spaces"
        self.input.mkdir()
        self.off = self.input / "场景 OFF.png"
        self.on = self.input / "场景 ON.png"
        self.image = Image.new("RGB", (640, 360), (35, 60, 80))
        draw = ImageDraw.Draw(self.image)
        draw.rectangle((180, 70, 450, 260), fill=(120, 140, 160))
        draw.line((20, 300, 610, 50), fill=(210, 200, 170), width=4)
        self.image.save(self.off)
        other = self.image.copy()
        ImageDraw.Draw(other).rectangle((210, 110, 280, 200), fill=(128, 146, 164))
        other.save(self.on)
        self.output = self.root / "output"

    def args(self, *extra):
        return gc.parser().parse_args(["generate", "--off", str(self.off), "--on", str(self.on),
                                       "--game", "中性测试", "--scene", "几何 场景", "--output", str(self.output), *extra])

    def generate(self, *extra):
        with contextlib.redirect_stdout(io.StringIO()):
            return gc.generate(self.args(*extra))

    def approve_args(self, candidate, *extra):
        return gc.parser().parse_args(["approve", "--candidate", str(candidate), *extra])

    def test_readme_hdr_requires_hdr_and_quality_is_bounded(self):
        with self.assertRaisesRegex(gc.CompareError, "--readme-hdr requires --hdr"):
            self.generate("--readme-hdr")
        for command in (["generate", "--off", str(self.off), "--on", str(self.on)], ["optimize", "--scene", "test"]):
            for value in ("0", "101", "90.5"):
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    gc.parser().parse_args([*command, "--readme-quality", value])

    def public_fixture(self):
        public = self.root / "public"
        public.mkdir()
        (public / "index.html").write_text((gc.PUBLIC / "index.html").read_text(encoding="utf-8"), encoding="utf-8")
        gc.write_json(public / "catalog.json", {"schema": 1, "scenes": []})
        return public

    def test_lossless_dimensions_privacy_report_and_references(self):
        info = PngImagePlugin.PngInfo()
        info.add_text("private", "GPS secret C:/Users/private")
        exif = Image.Exif()
        exif[315] = "Private author"
        self.image.save(self.off, pnginfo=info, exif=exif)
        hashes = [gc.digest(p.read_bytes()) for p in (self.off, self.on)]
        candidate = self.generate("--synthetic")
        report = gc.verify_candidate(candidate)
        with Image.open(candidate / "off.webp") as output:
            self.assertEqual(output.format, "WEBP")
            self.assertEqual(output.size, (640, 360))
            self.assertEqual(output.convert("RGB").tobytes(), self.image.tobytes())
            self.assertFalse(output.getexif())
            self.assertNotIn("xmp", output.info)
            self.assertNotIn("private", output.info)
        self.assertEqual(report["parameters"]["crop"], None)
        text = (candidate / "report.json").read_text(encoding="utf-8")
        self.assertNotIn(str(self.input), text)
        self.assertNotIn("GPS secret", text)
        page = (candidate / "index.html").read_text(encoding="utf-8")
        self.assertIn('data-candidate="true"', page)
        data = json.loads(gc.DATA_RE.search(page)[0].split(">", 1)[1].rsplit("<", 1)[0])
        for key in ("off", "on", "preview"):
            self.assertTrue((candidate / data["scenes"][0][key]).is_file())
        self.assertEqual(hashes, [gc.digest(p.read_bytes()) for p in (self.off, self.on)])

    def test_dimensions_explicit_downscale_only(self):
        Image.open(self.on).resize((1280, 720)).save(self.on)
        with self.assertRaisesRegex(gc.CompareError, "Different dimensions"):
            self.generate()
        candidate = self.generate("--match-size", "--max-width", "320")
        self.assertEqual(gc.verify_candidate(candidate)["scene"]["width"], 320)
        self.assertEqual(gc.verify_candidate(candidate)["scene"]["height"], 180)
        Image.open(self.on).resize((1280, 800)).save(self.on)
        with self.assertRaisesRegex(gc.CompareError, "Different dimensions"):
            self.generate("--match-size")

    def test_readme_is_complete_on_thumbnail_and_sizes_are_recorded(self):
        self.image.resize((1920, 1080)).save(self.off)
        other = Image.new("RGB", (1920, 1080), (45, 80, 115))
        other.save(self.on)
        candidate = self.generate("--game-en", "English Game", "--scene-en", "English Scene")
        report = gc.verify_candidate(candidate)
        self.assertEqual(report["scene"]["game_en"], "English Game")
        self.assertEqual(report["parameters"]["readme"], {"source": "complete ON SDR", "max_width": 1600, "quality": 90})
        with Image.open(candidate / "readme.webp") as image:
            self.assertEqual(image.size, (1600, 900))
            self.assertLess(max(abs(a - b) for a, b in zip(image.getpixel((0, 0)), (45, 80, 115))), 4)
            self.assertEqual(image.getpixel((0, 0)), image.getpixel((1599, 899)), "No split, labels or watermark in complete ON thumbnail")
        self.assertEqual(report["sizes"]["local_total_bytes"], sum(p.stat().st_size for p in candidate.iterdir()))
        self.assertEqual(report["sizes"]["expected_public_asset_bytes"], sum(report["outputs"][report["scene"][k]]["bytes"] for k in gc.public_asset_keys(report["scene"])))

    def test_preserve_hdr_pair_plan_apply_backup_and_selected_scene_only(self):
        public = gc.PUBLIC
        scene_id = "game/scene/0123456789abcdef"
        assets = public / "assets" / scene_id
        assets.mkdir(parents=True)
        self.image.save(assets / "off.jpg")
        Image.open(self.on).save(assets / "on.jpg")
        for name in ("preview.webp", "off-sdr.webp", "on-sdr.webp", "preview-hdr.jpg"):
            (assets / name).write_bytes(b"legacy derivative " + name.encode())
        entry = {"id": scene_id, "game": "Game", "scene": "Scene", "status": "approved", "dynamic_range": "ultra-hdr",
                 "width": 640, "height": 360, "synthetic": False}
        for key, name in (("off", "off.jpg"), ("on", "on.jpg"), ("preview", "preview.webp"), ("sdr_off", "off-sdr.webp"), ("sdr_on", "on-sdr.webp"), ("hdr_preview", "preview-hdr.jpg")):
            entry[key] = "assets/" + scene_id + "/" + name
        other = {**entry, "id": "game/other/fedcba9876543210", "scene": "Other"}
        # This second scene shares a derivative: selected-scene cleanup must retain that file.
        gc.write_json(public / "catalog.json", {"schema": 1, "scenes": [entry, other]})
        args = gc.parser().parse_args(["optimize", "--scene", scene_id, "--output", str(self.root / "tmp/output"), "--game-en", "Game EN", "--delete-old-assets"])
        before = {p.relative_to(public).as_posix(): p.read_bytes() for p in public.rglob("*") if p.is_file()}
        with patch.object(gc, "ROOT", self.root), contextlib.redirect_stdout(io.StringIO()):
            candidate = gc.optimize(args)
            self.assertEqual(before, {p.relative_to(public).as_posix(): p.read_bytes() for p in public.rglob("*") if p.is_file()})
            with self.assertRaisesRegex(gc.CompareError, "explicit human"):
                args.apply = True
                gc.optimize(args)
            args.reviewed = args.publish_authorized = True
            gc.optimize(args)
        updated = gc.approved_catalog()["scenes"]
        self.assertEqual(updated[1], other)
        self.assertEqual(updated[0]["id"], scene_id)
        self.assertEqual(updated[0]["off"], entry["off"])
        self.assertEqual(updated[0]["on"], entry["on"])
        self.assertEqual(updated[0]["game_en"], "Game EN")
        self.assertNotIn("preview", updated[0])
        self.assertEqual((public / entry["off"]).read_bytes(), before[entry["off"]])
        self.assertEqual((public / entry["on"]).read_bytes(), before[entry["on"]])
        self.assertFalse(list(candidate.rglob("backup/*/*")), "Shared derivatives cannot be deleted or backed up as deletions")
        self.assertTrue((public / updated[0]["readme"]).is_file())

    def test_optimize_deletion_requires_readme_update_and_verified_backup(self):
        public = gc.PUBLIC
        scene_id = "game/scene/0123456789abcdef"
        assets = public / "assets" / scene_id
        assets.mkdir(parents=True)
        self.image.save(assets / "off.jpg")
        Image.open(self.on).save(assets / "on.jpg")
        legacy = assets / "preview.webp"
        legacy.write_bytes(b"old preview bytes")
        prefix = "assets/" + scene_id + "/"
        entry = {"id": scene_id, "game": "Game", "scene": "Scene", "status": "approved", "dynamic_range": "ultra-hdr", "width": 640, "height": 360,
                 "off": prefix + "off.jpg", "on": prefix + "on.jpg", "preview": prefix + "preview.webp"}
        gc.write_json(public / "catalog.json", {"schema": 1, "scenes": [entry]})
        args = gc.parser().parse_args(["optimize", "--scene", scene_id, "--output", str(self.root / "tmp/output"), "--delete-old-assets", "--apply", "--reviewed", "--publish-authorized"])
        readme = self.root / "README.md"
        readme.write_text("![old](docs/compare/" + entry["preview"] + ")", encoding="utf-8")
        with patch.object(gc, "ROOT", self.root), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(gc.CompareError, "README still references"):
                gc.optimize(args)
            self.assertTrue(legacy.exists())
            self.assertEqual(gc.approved_catalog()["scenes"], [entry])
            readme.write_text("New image links are maintained separately.", encoding="utf-8")
            candidate = gc.optimize(args)
        self.assertFalse(legacy.exists())
        self.assertEqual([p.read_bytes() for p in (candidate / "backup").rglob("preview.webp")], [b"old preview bytes"])
        self.assertEqual(gc.read_json(candidate / "report.json")["plan"]["bytes"]["delete"], len(b"old preview bytes"))

    def test_corrupt_missing_and_wrong_mode(self):
        self.on.write_bytes(b"broken")
        with self.assertRaisesRegex(gc.CompareError, "cannot decode"):
            self.generate()
        self.on.unlink()
        with self.assertRaisesRegex(gc.CompareError, "does not exist"):
            self.generate()
        Image.new("RGBA", (640, 360), (100, 100, 100, 50)).save(self.on)
        with self.assertRaisesRegex(gc.CompareError, "transparent"):
            self.generate()

    def test_duplicate_and_reversed_identity(self):
        args = self.args()
        args.on = args.off
        with self.assertRaisesRegex(gc.CompareError, "same file"):
            gc.generate(args)
        args = self.args()
        args.off, args.on = args.on, args.off
        with self.assertRaisesRegex(gc.CompareError, "reversed"):
            gc.generate(args)
        self.image.save(self.on)
        with self.assertRaisesRegex(gc.CompareError, "identical"):
            self.generate()
        # Role flags are the explicit identity for neutral filenames; pixels cannot prove ON/OFF.
        self.assertEqual(gc.filename_state(Path("capture_001.png")), set())

    def test_idempotency_and_same_scene_no_overwrite(self):
        first = self.generate()
        before = gc.file_manifest(first)
        self.assertEqual(first, self.generate())
        self.assertEqual(len(gc.read_json(self.output / "candidates.json")["scenes"]), 1)
        altered = Image.open(self.on).copy()
        altered.putpixel((300, 100), (130, 140, 150))
        altered.save(self.on)
        second = self.generate()
        self.assertNotEqual(first, second)
        self.assertEqual(before, gc.file_manifest(first))
        self.assertEqual(len(gc.read_json(self.output / "candidates.json")["scenes"]), 2)
        (first / "off.webp").write_bytes(b"changed")
        with self.assertRaisesRegex(gc.CompareError, "changed"):
            gc.verify_candidate(first)

    def test_widescreen_and_labels_png_jpeg_webp(self):
        for size in ((640, 360), (840, 360), (1280, 360), (256, 256)):
            a = Image.new("RGB", size, (40, 70, 80))
            b = a.copy()
            b.putpixel((size[0] // 2, size[1] // 2), (90, 100, 110))
            a.save(self.off)
            b.save(self.on)
            candidate = self.generate("--format", "png")
            with Image.open(candidate / "preview.png") as image:
                self.assertEqual(image.size, size)
                self.assertNotEqual(image.getpixel((size[0] // 2, size[1] - 2)), a.getpixel((size[0] // 2, size[1] - 2)))
                self.assertEqual(image.getpixel((0, 0)), a.getpixel((0, 0)))
        for extension in ("jpg", "webp"):
            old_off, old_on = self.off, self.on
            self.off, self.on = self.input / ("OFF." + extension), self.input / ("ON." + extension)
            self.image.save(self.off)
            other = self.image.copy()
            ImageDraw.Draw(other).rectangle((200, 100, 300, 200), fill=(110, 130, 150))
            other.save(self.on)
            self.generate()
            self.off, self.on = old_off, old_on

    def test_hdr_and_invalid_icc_rejected(self):
        Image.new("I;16", (640, 360)).save(self.off)
        with self.assertRaisesRegex(gc.CompareError, "high-bit"):
            self.generate()
        self.image.save(self.off, icc_profile=b"invalid ICC")
        with self.assertRaisesRegex(gc.CompareError, "cannot decode"):
            self.generate()
        self.image.save(self.off)
        raw = self.off.read_bytes()
        body = b"cICP" + bytes((9, 16, 0, 1))
        chunk = struct.pack(">I", 4) + body + struct.pack(">I", zlib.crc32(body))
        self.off.write_bytes(raw[:33] + chunk + raw[33:])
        with self.assertRaisesRegex(gc.CompareError, "cICP"):
            self.generate()

    def test_same_icc_common_conversion(self):
        icc = ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()
        self.image.save(self.off, icc_profile=icc)
        b = Image.open(self.on).copy()
        b.save(self.on, icc_profile=icc)
        self.assertEqual(gc.verify_candidate(self.generate())["sources"]["off"]["icc_sha256"], gc.digest(icc))

    def test_scene_changes_block_approval_until_acknowledged(self):
        Image.new("RGB", (640, 360), "white").save(self.on)
        candidate = self.generate()
        self.assertTrue(gc.verify_candidate(candidate)["review_required"])
        with self.assertRaisesRegex(gc.CompareError, "human decision"):
            gc.approve(self.approve_args(candidate))
        shifted = Image.new("RGB", self.image.size, (35, 60, 80))
        shifted.paste(self.image.crop((0, 0, 628, 360)), (12, 0))
        shifted.save(self.on)
        report = gc.verify_candidate(self.generate())
        self.assertTrue(any("offset" in warning for warning in report["review_required"]))

    def test_public_copy_dry_run_review_gates_and_no_private_paths(self):
        candidate = self.generate("--gpu", "User provided GPU")
        public = self.public_fixture()
        before = {p.name: p.read_bytes() for p in public.iterdir()}
        with patch.object(gc, "PUBLIC", public), contextlib.redirect_stdout(io.StringIO()):
            gc.approve(self.approve_args(candidate))
            self.assertEqual(before, {p.name: p.read_bytes() for p in public.iterdir()})
            with self.assertRaisesRegex(gc.CompareError, "explicit human"):
                gc.approve(self.approve_args(candidate, "--apply"))
            gc.approve(self.approve_args(candidate, "--apply", "--reviewed", "--publish-authorized"))
            gc.approve(self.approve_args(candidate, "--apply", "--reviewed", "--publish-authorized"))
            catalog = gc.approved_catalog()
        self.assertEqual(len(catalog["scenes"]), 1)
        self.assertEqual(catalog["scenes"][0]["status"], "approved")
        for path in public.rglob("*"):
            if path.is_file():
                self.assertNotIn(str(self.input).encode(), path.read_bytes())
                self.assertNotIn(self.off.name.encode(), path.read_bytes())
        self.assertFalse(list(public.rglob("report.json")))

    def test_synthetic_never_public_and_candidate_not_in_public_output(self):
        candidate = self.generate("--synthetic")
        with self.assertRaisesRegex(gc.CompareError, "Synthetic"):
            gc.approve(self.approve_args(candidate, "--apply", "--reviewed", "--publish-authorized"))
        args = self.args()
        args.output = str(ROOT / "docs/compare/candidate")
        with self.assertRaisesRegex(gc.CompareError, "Candidate output"):
            gc.generate(args)
        self.assertEqual(gc.approved_catalog()["scenes"], [])

    def test_replacement_preserves_identity_and_blocks_live_readme_deletion(self):
        original = self.generate()
        with contextlib.redirect_stdout(io.StringIO()):
            gc.approve(self.approve_args(original, "--apply", "--reviewed", "--publish-authorized"))
        old = gc.approved_catalog()["scenes"][0]
        replacement = self.generate("--quality", "95")
        readme = self.root / "README.md"
        readme.write_text("![old](" + (gc.PUBLIC / old["readme"]).relative_to(self.root).as_posix() + ")", encoding="utf-8")
        args = self.approve_args(replacement, "--replace-scene", old["id"], "--original-candidate", str(original),
                                 "--delete-old-assets", "--apply", "--reviewed", "--publish-authorized")
        with patch.object(gc, "ROOT", self.root), patch.object(gc, "DEFAULT_OUTPUT", self.root / "tmp/backups"), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(gc.CompareError, "README still references"):
                gc.approve(args)
            self.assertEqual(gc.approved_catalog()["scenes"][0], old)
            readme.write_text("Image links updated for replacement.", encoding="utf-8")
            gc.approve(args)
        updated = gc.approved_catalog()["scenes"][0]
        self.assertEqual(updated["id"], old["id"])
        self.assertIn("/revisions/", updated["on"])
        self.assertTrue((gc.PUBLIC / updated["on"]).is_file())
        self.assertFalse((gc.PUBLIC / old["on"]).exists())
        self.assertTrue(list((self.root / "tmp/backups/replacement-backups").rglob("on.webp")))

    def test_names_and_html_embedding(self):
        for name in ("CON", "nul", "LPT1", "中文 场景", "x/y", "X y"):
            self.assertRegex(gc.slug(name), r"^[a-z0-9-]+$")
        template = (gc.PUBLIC / "index.html").read_text(encoding="utf-8")
        html = gc.render_page(template, [{"scene": "</script><script>alert(1)</script>"}])
        self.assertNotIn("</script><script>alert", html)
        with self.assertRaises(gc.CompareError):
            gc.clean_text("C:/Users/private")

    def test_readme_requires_live_approved_scene_and_preserves_other_text(self):
        public = self.public_fixture()
        entry = {"id": "game/scene/0123456789abcdef", "game": "Game", "scene": "Scene",
                 "status": "approved", "readme": "assets/game/scene/0123456789abcdef/readme.webp", "game_en": "English Game", "scene_en": "English Scene"}
        gc.write_json(public / "catalog.json", {"schema": 1, "scenes": [entry]})
        text = "Existing text\n<!-- game-compare:zh:start -->old<!-- game-compare:zh:end -->\n"
        text += "<!-- game-compare:en:start -->old<!-- game-compare:en:end -->\nOther text\n"
        (self.root / "README.md").write_text(text, encoding="utf-8")
        class Response(io.BytesIO):
            status = 200
        args = gc.parser().parse_args(["readme", "--scene", entry["id"], "--pages-url",
                                      "https://williamstosh.github.io/DLSS5-NR-Nexus/compare/",
                                      "--apply", "--publish-authorized"])
        live = gc.render_page((public / "index.html").read_text(encoding="utf-8"), [entry]).encode()
        with patch.object(gc, "PUBLIC", public), patch.object(gc, "ROOT", self.root), contextlib.redirect_stdout(io.StringIO()):
            with patch.object(gc, "urlopen", return_value=Response(b"not the comparison page")):
                with self.assertRaisesRegex(gc.CompareError, "not available"):
                    gc.readme(args)
            self.assertEqual((self.root / "README.md").read_text(encoding="utf-8"), text)
            with patch.object(gc, "urlopen", return_value=Response(live)):
                gc.readme(args)
        updated = (self.root / "README.md").read_text(encoding="utf-8")
        self.assertTrue(updated.startswith("Existing text\n"))
        self.assertTrue(updated.endswith("Other text\n"))
        self.assertEqual(updated.count("readme.webp"), 2)
        self.assertEqual(updated.count("](" + args.pages_url + "#scene="), 2, "Only the image is linked per language")
        self.assertIn("English Game", updated)
        self.assertIn("#scene=game%2Fscene%2F0123456789abcdef", updated)
        self.assertIn("左右拖动查看对比", updated)
        self.assertIn("Interactive Comparison", updated)
        del entry["readme"]
        entry["preview"] = "assets/game/scene/0123456789abcdef/preview.webp"
        gc.write_json(public / "catalog.json", {"schema": 1, "scenes": [entry]})
        with patch.object(gc, "PUBLIC", public), self.assertRaisesRegex(gc.CompareError, "run optimize first"):
            gc.readme(args)


if __name__ == "__main__":
    unittest.main()
