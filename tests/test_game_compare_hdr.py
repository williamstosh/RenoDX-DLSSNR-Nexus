"""Synthetic scRGB/JXR/Ultra HDR tests. No real screenshots or display required."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
try:
    import imagecodecs as codecs
    import numpy as np
    import game_compare_hdr as hdr
except ImportError:
    codecs = None
from PIL import Image

spec = importlib.util.spec_from_file_location("compare_hdr_test_entry", ROOT / "tools/game-compare.py")
gc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gc)


@unittest.skipIf(codecs is None, "Optional HDR codecs are not installed")
class HdrComparisons(unittest.TestCase):
    def setUp(self):
        (ROOT / "tmp").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "tmp")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.inputs = self.root / "中文 HDR paths"
        self.inputs.mkdir()
        self.off = self.inputs / "测试 OFF.jxr"
        self.on = self.inputs / "测试 ON.jxr"
        self.output = self.root / "output"
        self.original = np.ones((144, 256, 4), np.float32)
        self.original[..., :3] = np.linspace(.05, 10, 256)[None, :, None]
        self.other = self.original.copy()
        self.other[48:80, 100:140, :3] *= .98
        self.save_sources()

    def save_sources(self):
        for image, path in ((self.original, self.off), (self.other, self.on)):
            path.write_bytes(codecs.jpegxr_encode(image.astype(np.float16), level=1, hasalpha=True))

    def generate(self, *extra):
        args = gc.parser().parse_args(["generate", "--off", str(self.off), "--on", str(self.on), "--hdr",
                                      "--game", "Synthetic HDR", "--scene", "Gradient", "--output", str(self.output), *extra])
        with contextlib.redirect_stdout(io.StringIO()):
            return gc.generate(args)

    def test_hdr_roundtrip_common_peak_preview_and_idempotency(self):
        before = {p.name: gc.digest(p.read_bytes()) for p in (self.off, self.on)}
        candidate = self.generate("--synthetic", "--note", "Synthetic fixture only", "--hdr-profile", "archival", "--hdr-preview", "--sdr-fallback")
        report = gc.verify_candidate(candidate)
        self.assertEqual(candidate, self.generate("--synthetic", "--note", "Synthetic fixture only", "--hdr-profile", "archival", "--hdr-preview", "--sdr-fallback"))
        self.assertEqual(len(gc.read_json(self.output / "candidates.json")["scenes"]), 1)
        self.assertEqual(report["scene"]["dynamic_range"], "ultra-hdr")
        self.assertEqual(report["sources"]["off"]["format"], "JPEG XR")
        self.assertNotIn(str(self.inputs), json.dumps(report))
        self.assertEqual(before, {p.name: gc.digest(p.read_bytes()) for p in (self.off, self.on)})
        nits = report["parameters"]["hdr"]["shared_target_peak_nits"]
        self.assertEqual(nits, 800)
        capacities = []
        for key in ("off", "on", "hdr_preview"):
            data = (candidate / report["scene"][key]).read_bytes()
            decoded = codecs.ultrahdr_decode(data)
            self.assertEqual(decoded.shape, (144, 256, 4))
            self.assertTrue(np.isfinite(decoded).all())
            self.assertGreater(float(decoded[..., :3].max()), 2)
            base, gainmap, meta = codecs.ultrahdr_decode_jpeg(data)
            self.assertEqual(Image.open(io.BytesIO(base)).size, (256, 144))
            self.assertEqual(Image.open(io.BytesIO(gainmap)).size, (256, 144))
            self.assertFalse(Image.open(io.BytesIO(base)).getexif())
            capacities.append(meta["hdr_capacity_max"])
        np.testing.assert_allclose(capacities, [800 / 203] * 3, rtol=1e-6)
        self.assertLess(report["hdr_outputs"]["off"]["roundtrip"]["linear_rgb_mean_absolute_error"], .05)
        original, _ = hdr.decode_jxr(self.off)
        with Image.open(candidate / "off-sdr.webp") as image:
            np.testing.assert_array_equal(np.asarray(image), hdr.tonemap_sdr(original)[..., :3])
        with self.assertRaisesRegex(gc.CompareError, "Synthetic"):
            gc.approve(gc.parser().parse_args(["approve", "--candidate", str(candidate), "--apply", "--reviewed", "--publish-authorized"]))

    def test_physical_white_and_extended_gamut(self):
        image = np.ones((64, 64, 4), np.float32)
        intent, count = hdr.hdr_intent(image)
        self.assertEqual(count, 0)
        np.testing.assert_allclose(intent[..., :3], 80 / 203, rtol=.001)
        image[..., :3] = (-.02, .1, .3)
        intent, count = hdr.hdr_intent(image)
        self.assertEqual(count, 0)
        expected = np.array([-.02, .1, .3]) @ hdr.TO_2020.T * (80 / 203)
        np.testing.assert_allclose(intent[0, 0, :3], expected, rtol=.002)
        self.assertTrue(np.all(intent[..., :3] > 0))
        image[..., :3] = (-1, 0, 0)
        with self.assertRaisesRegex(ValueError, "outside Rec.2020"):
            hdr.hdr_intent(image)

    def test_same_pixel_uses_same_tone_curve_independent_of_peak(self):
        a = np.ones((64, 64, 4), np.float32)
        b = a.copy()
        a[0, 0, :3] = 3
        b[0, 0, :3] = 30
        np.testing.assert_array_equal(hdr.tonemap_sdr(a)[1:, 1:], hdr.tonemap_sdr(b)[1:, 1:])

    def test_corruption_size_duplicate_and_explicit_hdr(self):
        self.on.write_bytes(b"broken")
        with self.assertRaisesRegex(ValueError, "container"):
            self.generate()
        self.save_sources()
        self.on.write_bytes(self.off.read_bytes())
        with self.assertRaisesRegex(ValueError, "identical"):
            self.generate()
        self.other = self.other[:128]
        self.save_sources()
        with self.assertRaisesRegex(ValueError, "same full dimensions"):
            self.generate()
        with self.assertRaisesRegex(gc.CompareError, "preserves full dimensions"):
            self.generate("--max-width", "128")
        with self.assertRaisesRegex(gc.CompareError, "use PNG"):
            gc.load_image(self.off, "OFF")

    def test_invalid_float_values_alpha_and_orientation(self):
        self.original[..., 3] = .5
        self.save_sources()
        with self.assertRaisesRegex(ValueError, "opaque"):
            hdr.decode_jxr(self.off)
        self.original[..., 3] = 1
        self.save_sources()
        raw = bytearray(self.off.read_bytes())
        offset = struct.unpack_from("<I", raw, 4)[0]
        count = struct.unpack_from("<H", raw, offset)[0]
        for i in range(count):
            entry = offset + 2 + i * 12
            if struct.unpack_from("<H", raw, entry)[0] == 0xbc02:
                struct.pack_into("<I", raw, entry + 8, 1)
        self.off.write_bytes(raw)
        with self.assertRaisesRegex(ValueError, "Rotated"):
            hdr.decode_jxr(self.off)
        with patch.object(codecs, "jpegxr_decode", return_value=np.full((64, 64, 4), np.nan, np.float32)):
            self.save_sources()
            with self.assertRaisesRegex(ValueError, "finite"):
                hdr.decode_jxr(self.off)

    def test_approval_plan_only_public_pair_and_readme(self):
        candidate = self.generate("--note", "Captures are 8 seconds apart")
        public = self.root / "public"
        public.mkdir()
        template = (gc.PUBLIC / "index.html").read_text(encoding="utf-8")
        (public / "index.html").write_text(gc.render_page(template, []), encoding="utf-8")
        gc.write_json(public / "catalog.json", {"schema": 1, "scenes": []})
        args = gc.parser().parse_args(["approve", "--candidate", str(candidate)])
        with patch.object(gc, "PUBLIC", public), patch.object(gc, "ROOT", self.root), contextlib.redirect_stdout(io.StringIO()) as stdout:
            gc.approve(args)
            self.assertFalse((public / "assets").exists())
            for name in ("off.jpg", "on.jpg", "readme.webp"):
                self.assertIn(name, stdout.getvalue())
            for name in ("off-sdr.webp", "on-sdr.webp", "preview.webp", "preview-hdr.jpg"):
                self.assertNotIn(name, stdout.getvalue())
            args.apply = args.reviewed = args.publish_authorized = True
            gc.approve(args)
            gc.approve(args)
            catalog = gc.approved_catalog()
            self.assertIn("Captures are 8 seconds apart", catalog["scenes"][0]["warnings"])
            for key in gc.scene_asset_keys(catalog["scenes"][0]):
                self.assertTrue((public / catalog["scenes"][0][key]).is_file())
            self.assertFalse(list(public.rglob("report.json")))
            self.assertFalse(list(public.rglob("*.jxr")))
            entry = catalog["scenes"][0]
            pair_before = {side: (public / entry[side]).read_bytes() for side in ("off", "on")}
            with Image.open(public / entry["on"]) as primary:
                self.assertIn(primary.format, ("JPEG", "MPO"))
                expected = primary.convert("RGB")
            opt = gc.parser().parse_args(["optimize", "--scene", entry["id"], "--preserve-hdr-pair", "--output", str(self.root / "tmp/optimized")])
            optimized = gc.optimize(opt)
            expected_path = self.root / "expected.webp"
            gc.save_image(expected, expected_path, 90)
            with Image.open(expected_path) as a, Image.open(optimized / "readme.webp") as b:
                np.testing.assert_array_equal(np.asarray(a), np.asarray(b))
            self.assertEqual(pair_before, {side: (public / entry[side]).read_bytes() for side in ("off", "on")})

    def test_balanced_options_embedded_base_and_overrides(self):
        candidate = self.generate()
        report = gc.verify_candidate(candidate)
        self.assertEqual(report["parameters"]["hdr"]["gainmap_scale"], 2)
        self.assertEqual(report["parameters"]["hdr"]["gainmap_quality"], 95)
        self.assertNotIn("hdr_preview", report["scene"])
        self.assertNotIn("sdr_off", report["scene"])
        self.assertEqual(report["hdr_outputs"]["off"]["gainmap_dimensions"], [128, 72])
        data = (candidate / "off.jpg").read_bytes()
        base, _, _ = codecs.ultrahdr_decode_jpeg(data)
        # Ordinary JPEG decoders read the exact embedded SDR base, without gain-map reconstruction.
        with Image.open(io.BytesIO(data)) as ordinary, Image.open(io.BytesIO(base)) as embedded:
            np.testing.assert_array_equal(np.asarray(ordinary), np.asarray(embedded))
        second = self.generate("--gainmap-scale", "4", "--gainmap-quality", "90", "--quality", "92")
        options = gc.verify_candidate(second)["parameters"]["hdr"]
        self.assertEqual((options["quality"], options["gainmap_scale"], options["gainmap_quality"]), (92, 4, 90))
        self.assertEqual(gc.verify_candidate(second)["hdr_outputs"]["on"]["gainmap_dimensions"], [64, 36])
        with self.assertRaisesRegex(ValueError, "finite and positive"):
            self.generate("--gainmap-gamma", "nan")

    def test_full_resolution_hdr_readme_from_original_without_pair_changes(self):
        self.original = np.ones((144, 1664, 4), np.float32)
        self.original[..., :3] = np.linspace(.05, 10, 1664)[None, :, None]
        self.other = self.original.copy()
        self.other[48:80, 100:140, :3] *= .98
        self.save_sources()
        baseline = self.generate()
        candidate = self.generate("--readme-hdr")
        self.assertEqual(candidate, self.generate("--readme-hdr"))
        report = gc.verify_candidate(candidate)
        self.assertEqual(report["scene"]["readme"], "readme.jpg")
        options = report["parameters"]["readme"]
        self.assertEqual(options["source_sha256"], gc.digest(self.on.read_bytes()))
        self.assertFalse(options["lossy_reencode"])
        self.assertTrue(options["full_resolution"])
        self.assertEqual((options["quality"], options["gainmap_quality"], options["gainmap_scale"],
                          options["gainmap_gamma"], options["gainmap_channels"]), (90, 90, 1, 1, 3))
        self.assertEqual(options["shared_target_peak_nits"], 800)
        for side in ("off", "on"):
            self.assertEqual((baseline / (side + ".jpg")).read_bytes(), (candidate / (side + ".jpg")).read_bytes())
        output = report["hdr_outputs"]["readme"]
        self.assertEqual(output["dimensions"], [1664, 144])
        self.assertEqual(output["gainmap_dimensions"], [1664, 144])
        self.assertLess(output["roundtrip"]["linear_rgb_mean_absolute_error"], .06)
        data = (candidate / "readme.jpg").read_bytes()
        with Image.open(io.BytesIO(data)) as ordinary:
            self.assertEqual(ordinary.size, (1664, 144))
            self.assertFalse(ordinary.getexif())
            self.assertEqual(ordinary.convert("RGB").size, (1664, 144))
        changed = self.generate("--readme-hdr", "--readme-quality", "85")
        self.assertNotEqual(changed, candidate)
        self.assertEqual(gc.verify_candidate(changed)["parameters"]["readme"]["gainmap_quality"], 85)

    def test_hdr_readme_approval_and_optimize_preserve_pair_and_backup(self):
        candidate = self.generate("--readme-hdr")
        public = self.root / "public"
        public.mkdir()
        template = (gc.PUBLIC / "index.html").read_text(encoding="utf-8")
        (public / "index.html").write_text(gc.render_page(template, []), encoding="utf-8")
        gc.write_json(public / "catalog.json", {"schema": 1, "scenes": []})
        with patch.object(gc, "PUBLIC", public), patch.object(gc, "ROOT", self.root), contextlib.redirect_stdout(io.StringIO()) as stdout:
            approve = gc.parser().parse_args(["approve", "--candidate", str(candidate)])
            gc.approve(approve)
            self.assertIn("readme.jpg", stdout.getvalue())
            self.assertNotIn("readme.webp", stdout.getvalue())
            approve.apply = approve.reviewed = approve.publish_authorized = approve.ack_warnings = True
            gc.approve(approve)
            entry = gc.approved_catalog()["scenes"][0]
            before = {side: (public / entry[side]).read_bytes() for side in ("off", "on")}
            opt = gc.parser().parse_args(["optimize", "--scene", entry["id"], "--readme-hdr",
                                         "--delete-old-assets", "--output", str(self.root / "tmp/optimized")])
            optimized = gc.optimize(opt)
            self.assertEqual(optimized, gc.optimize(opt))
            report = gc.read_json(optimized / "report.json")
            options = report["parameters"]["readme"]
            self.assertTrue(options["lossy_reencode"])
            self.assertEqual(options["source_sha256"], gc.digest(before["on"]))
            self.assertAlmostEqual(options["shared_target_peak_nits"], 800, places=3)
            self.assertEqual(report["hdr_outputs"]["readme"]["gainmap_dimensions"], [256, 144])
            self.assertLess(report["hdr_outputs"]["readme"]["roundtrip"]["linear_rgb_mean_absolute_error"], .06)
            with patch.object(hdr, "tonemap_sdr", side_effect=AssertionError("No retone allowed")):
                reference, base, _ = hdr.readme_from_jpeg(before["on"])
            with Image.open(io.BytesIO(before["on"])) as source:
                np.testing.assert_array_equal(base, np.asarray(source.convert("RGBA")))
            np.testing.assert_array_equal(reference, codecs.ultrahdr_decode(before["on"]))
            opt.apply = True
            with self.assertRaisesRegex(gc.CompareError, "human review"):
                gc.optimize(opt)
            opt.reviewed = opt.publish_authorized = True
            (self.root / "README.md").write_text("docs/compare/" + entry["readme"], encoding="utf-8")
            with self.assertRaisesRegex(gc.CompareError, "README still references"):
                gc.optimize(opt)
            (self.root / "README.md").write_text("Updated image links", encoding="utf-8")
            gc.optimize(opt)
            updated = gc.approved_catalog()["scenes"][0]
            self.assertEqual(updated["id"], entry["id"])
            self.assertTrue(updated["readme"].endswith("/readme.jpg"))
            self.assertEqual(before, {side: (public / updated[side]).read_bytes() for side in ("off", "on")})
            self.assertFalse((public / entry["readme"]).exists())
            self.assertEqual(next((optimized / "backup").rglob("readme.jpg")).read_bytes(), (candidate / "readme.jpg").read_bytes())


if __name__ == "__main__":
    unittest.main()
