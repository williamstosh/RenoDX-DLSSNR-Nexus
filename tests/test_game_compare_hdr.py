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
        candidate = self.generate("--synthetic", "--note", "Synthetic fixture only")
        report = gc.verify_candidate(candidate)
        self.assertEqual(candidate, self.generate("--synthetic", "--note", "Synthetic fixture only"))
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

    def test_approval_plan_includes_all_hdr_and_fallback_files(self):
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
            for name in ("off.jpg", "on.jpg", "off-sdr.webp", "on-sdr.webp", "preview.webp", "preview-hdr.jpg"):
                self.assertIn(name, stdout.getvalue())
            args.apply = args.reviewed = args.publish_authorized = True
            gc.approve(args)
            gc.approve(args)
            catalog = gc.approved_catalog()
            self.assertIn("Captures are 8 seconds apart", catalog["scenes"][0]["warnings"])
            for key in gc.scene_asset_keys(catalog["scenes"][0]):
                self.assertTrue((public / catalog["scenes"][0][key]).is_file())
            self.assertFalse(list(public.rglob("report.json")))
            self.assertFalse(list(public.rglob("*.jxr")))


if __name__ == "__main__":
    unittest.main()
