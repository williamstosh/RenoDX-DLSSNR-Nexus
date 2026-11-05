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


if __name__ == "__main__":
    unittest.main()
