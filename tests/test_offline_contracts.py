import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "offline"))
import contracts as c


class OfflineContractsTests(unittest.TestCase):
    def setUp(self):
        self.fixture = {"schema": c.FIXTURE_SCHEMA, "fixture_id": "test",
                        "cases": [{"id": "one", "width": 3, "height": 3}]}

    def test_defaults_and_canonical_identity(self):
        parsed = c.validate_fixture(self.fixture)
        self.assertEqual(parsed["cases"][0]["frames"], 8)
        self.assertEqual(c.identity(parsed), c.identity(c.validate_fixture(parsed)))
        self.assertEqual(c.identity({"a": 1, "b": 2}), c.identity({"b": 2, "a": 1}))

    def test_rejects_unsupported_or_mistyped_fields(self):
        for field, value in (("width", True), ("height", 0), ("width", 8193), ("frames", 0),
                             ("warmup", 8), ("rgb", [1, float("nan"), 1]), ("rgb", [1, 2, 1]),
                             ("rect", [4, 0]), ("model", "C"), ("mode", "none"),
                             ("auto_mask", 1), ("blend_mode", 4), ("id", "../escape"), ("jitter", [1, 0])):
            with self.subTest(field=field, value=value):
                value_fixture = copy.deepcopy(self.fixture)
                value_fixture["cases"][0][field] = value
                with self.assertRaises(c.ContractError):
                    c.validate_fixture(value_fixture)

    def test_windows_case_collisions(self):
        self.fixture["cases"].append({"id": "ONE", "width": 1, "height": 1})
        with self.assertRaises(c.ContractError):
            c.validate_fixture(self.fixture)

    def test_future_schema_is_not_silently_accepted(self):
        self.fixture["schema"] = "nr-nexus-offline-fixture/2"
        with self.assertRaises(c.ContractError):
            c.validate_fixture(self.fixture)

    def test_mask_channels_half_open_and_floor(self):
        case = c.validate_fixture(self.fixture)["cases"][0]
        case["mode"] = "green_rect"
        pixels = c.mask_reference(case)
        self.assertEqual(pixels[:4], bytes([255, 0, 255, 255]))
        self.assertEqual(pixels[4:], bytes([255]) * 32)
        case.update(width=1, mode="blue_rect")
        self.assertEqual(c.mask_reference(case), bytes([255]) * 12)

    def test_fixed_rgb_and_legacy_rect(self):
        case = c.validate_fixture(self.fixture)["cases"][0]
        case.update(mode="rect", rgb=[.25, .5, .75], rect=[1, 1])
        self.assertEqual(c.mask_reference(case)[:8], bytes([0, 0, 0, 255, 64, 128, 191, 255]))

    def test_d3d_unorm_tolerance_is_not_an_arbitrary_one_byte_allowance(self):
        case = c.validate_fixture(self.fixture)["cases"][0]
        case.update(width=1, height=1, mode="rgb", rgb=[.25, .5, .75])
        for green in (127, 128):
            result = c.compare_mask(case, bytes([64, green, 191, 255]))
            self.assertTrue(result["format_conformant"])
            self.assertEqual(result["nearest_even_reference"]["identical"], green == 128)
        for pixel in ([63, 128, 191, 255], [65, 128, 191, 255], [64, 126, 191, 255],
                      [64, 129, 191, 255], [64, 128, 192, 255], [64, 128, 191, 254]):
            with self.subTest(pixel=pixel):
                self.assertFalse(c.compare_mask(case, bytes(pixel))["format_conformant"])

    def test_mask_endpoints_and_rectangle_boundaries_remain_exact(self):
        case = c.validate_fixture(self.fixture)["cases"][0]
        case["mode"] = "green_rect"
        pixels = bytearray(c.mask_reference(case))
        pixels[1] = 1
        self.assertFalse(c.compare_mask(case, pixels)["format_conformant"])
        pixels[1], pixels[5] = 0, 254
        self.assertFalse(c.compare_mask(case, pixels)["format_conformant"])

    def test_nearest_rank_p95(self):
        stats = c.sample_statistics(list(range(1, 101)))
        self.assertEqual(stats["p95"], 95)
        self.assertEqual(stats["mean"], 50.5)
        for bad in ([], [float("inf")], [-1]):
            with self.assertRaises(c.ContractError):
                c.sample_statistics(bad)

    def test_f32_metrics_and_nonfinite_failure(self):
        first = struct.pack("<2f", 0, 1)
        diff = c.compare_raw(first, struct.pack("<2f", .5, .5), "f32le")
        self.assertEqual(diff["mae"], .5)
        self.assertEqual(diff["rmse"], .5)
        self.assertAlmostEqual(diff["psnr_db"], 6.020599913)
        self.assertIsNone(c.compare_raw(first, first, "f32le")["psnr_db"])
        with self.assertRaises(c.ContractError):
            c.compare_raw(first, struct.pack("<2f", float("nan"), 1), "f32le")
        with self.assertRaises(c.ContractError):
            c.compare_raw(b"a", b"aa", "u8")

    def test_sequence_is_reproducible_and_explicit(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = c.prepare_sequence(Path(tmp) / "a", 3, 2, 2, 7)
            b = c.prepare_sequence(Path(tmp) / "b", 3, 2, 2, 7)
            self.assertEqual(a, b)
            self.assertTrue(a["frames"][0]["reset"])
            self.assertFalse(a["frames"][1]["reset"])
            self.assertEqual(a["evidence"], "STATIC")
            self.assertEqual(c.verify_sequence(Path(tmp) / "a"), a)
            for frame in a["frames"]:
                for resource in frame["resources"].values():
                    path = Path(tmp) / "a" / resource["file"]
                    self.assertEqual(c.digest(path), resource["sha256"])
                    self.assertEqual(path.stat().st_size, resource["bytes"])
            with self.assertRaises(FileExistsError):
                c.prepare_sequence(Path(tmp) / "a", 3, 2, 2)

    def test_resized_sequence_subrect_bounds_and_format_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / "resize"
            manifest = c.prepare_sequence(directory, 4, 3, 4, seed=2,
                resolutions=[(4, 3), (6, 4), (6, 4), (2, 1)],
                subrects=[(0, 0, 4, 3), (1, 1, 4, 2), (0, 0, 6, 4), (0, 0, 2, 1)])
            self.assertEqual(manifest["schema"], "nr-nexus-offline-sequence/2")
            self.assertEqual([(frame["width"], frame["height"]) for frame in manifest["frames"]],
                             [(4, 3), (6, 4), (6, 4), (2, 1)])
            self.assertEqual(c.verify_sequence(directory), manifest)
            frame = manifest["frames"][1]
            self.assertEqual(frame["resources"]["color"]["bytes"], 6 * 4 * 16)
            unsigned = {key: value for key, value in manifest.items() if key != "sequence_sha256"}
            manifest["frames"][1]["subrect"] = [4, 3, 4, 2]
            manifest["sequence_sha256"] = c.identity(unsigned)
            (directory / "sequence.json").write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaises(c.ContractError):
                c.verify_sequence(directory)

    def test_nonfinite_identical_f32_is_still_rejected(self):
        bad = struct.pack("<f", float("nan"))
        with self.assertRaises(c.ContractError):
            c.compare_raw(bad, bad, "f32le")

    def test_sequence_tamper_and_path_escape(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / "sequence"
            manifest = c.prepare_sequence(directory, 2, 2, 1)
            path = directory / manifest["frames"][0]["resources"]["depth"]["file"]
            path.write_bytes(b"\0" * path.stat().st_size)
            with self.assertRaises(c.ContractError):
                c.verify_sequence(directory)
            for name in ("../private.f32", "C:/private.f32", "/private.f32", "..\\private.f32"):
                with self.subTest(name=name), self.assertRaises(c.ContractError):
                    c.artifact_path(directory, name)

    def test_sequence_budget_before_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "large"
            with self.assertRaises(c.ContractError):
                c.prepare_sequence(target, 8192, 8192, 2)
            self.assertFalse(target.exists())

    def test_json_duplicate_keys_nan_and_no_clobber(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fixture.json"
            path.write_text('{"a": 1, "a": 2}')
            with self.assertRaises(c.ContractError):
                c.read_json(path)
            path.write_text('{"a": NaN}')
            with self.assertRaises(c.ContractError):
                c.read_json(path)
            with self.assertRaises(FileExistsError):
                c.write_json(path, {})

    def test_matrix_covers_contract_cross_product(self):
        matrix = c.parameter_matrix()
        self.assertEqual(len(matrix["cases"]), 96)
        combinations = {(case["model"], case["auto_mask"], case["blend_mode"], case["mode"])
                        for case in matrix["cases"]}
        self.assertEqual(len(combinations), 80)

    def test_raster_performance_fixture_covers_common_resolutions(self):
        path = Path(__file__).parent / "fixtures" / "offline" / "control-mask-raster-performance.json"
        fixture = c.validate_fixture(c.read_json(path))
        self.assertEqual([(item["width"], item["height"]) for item in fixture["cases"]],
                         [(1920, 1080), (2560, 1440), (3840, 2160)])
        self.assertTrue(all(item["frames"] == 1008 and item["warmup"] == 8 for item in fixture["cases"]))


if __name__ == "__main__":
    unittest.main()
