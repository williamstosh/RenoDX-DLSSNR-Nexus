import copy
import json
from pathlib import Path
import struct
import sys
import subprocess
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "offline"))
import contracts as c
import control_mask_contracts as mask


class OfflineContractsTests(unittest.TestCase):
    def setUp(self):
        self.fixture = {"schema": mask.FIXTURE_SCHEMA, "fixture_id": "test",
                        "cases": [{"id": "one", "width": 3, "height": 3}]}

    def test_defaults_and_canonical_identity(self):
        parsed = mask.validate_fixture(self.fixture)
        self.assertEqual(parsed["cases"][0]["frames"], 8)
        self.assertEqual(c.identity(parsed), c.identity(mask.validate_fixture(parsed)))
        self.assertEqual(c.identity({"a": 1, "b": 2}), c.identity({"b": 2, "a": 1}))

    def test_generic_sequence_contract_does_not_load_control_mask_oracle(self):
        probe = "import sys, contracts, run; assert 'control_mask_contracts' not in sys.modules; print(contracts.RESULT_SCHEMA)"
        completed = subprocess.run([sys.executable, "-c", probe],
                                   cwd=Path(__file__).resolve().parents[1] / "tools" / "offline",
                                   capture_output=True, text=True, check=True)
        self.assertEqual(completed.stdout.strip(), c.RESULT_SCHEMA)

    def test_rejects_unsupported_or_mistyped_fields(self):
        for field, value in (("width", True), ("height", 0), ("width", 8193), ("frames", 0),
                             ("warmup", 8), ("rgb", [1, float("nan"), 1]), ("rgb", [1, 2, 1]),
                             ("rect", [4, 0]), ("model", "C"), ("mode", "none"),
                             ("auto_mask", 1), ("blend_mode", 4), ("id", "../escape"), ("jitter", [1, 0])):
            with self.subTest(field=field, value=value):
                value_fixture = copy.deepcopy(self.fixture)
                value_fixture["cases"][0][field] = value
                with self.assertRaises(c.ContractError):
                    mask.validate_fixture(value_fixture)

    def test_windows_case_collisions(self):
        self.fixture["cases"].append({"id": "ONE", "width": 1, "height": 1})
        with self.assertRaises(c.ContractError):
            mask.validate_fixture(self.fixture)

    def test_future_schema_is_not_silently_accepted(self):
        self.fixture["schema"] = "nr-nexus-offline-fixture/2"
        with self.assertRaises(c.ContractError):
            mask.validate_fixture(self.fixture)

    def test_mask_channels_half_open_and_floor(self):
        case = mask.validate_fixture(self.fixture)["cases"][0]
        case["mode"] = "green_rect"
        pixels = mask.mask_reference(case)
        self.assertEqual(pixels[:4], bytes([255, 0, 255, 255]))
        self.assertEqual(pixels[4:], bytes([255]) * 32)
        case.update(width=1, mode="blue_rect")
        self.assertEqual(mask.mask_reference(case), bytes([255]) * 12)

    def test_fixed_rgb_and_legacy_rect(self):
        case = mask.validate_fixture(self.fixture)["cases"][0]
        case.update(mode="rect", rgb=[.25, .5, .75], rect=[1, 1])
        self.assertEqual(mask.mask_reference(case)[:8], bytes([0, 0, 0, 255, 64, 128, 191, 255]))

    def test_d3d_unorm_tolerance_is_not_an_arbitrary_one_byte_allowance(self):
        case = mask.validate_fixture(self.fixture)["cases"][0]
        case.update(width=1, height=1, mode="rgb", rgb=[.25, .5, .75])
        for green in (127, 128):
            result = mask.compare_mask(case, bytes([64, green, 191, 255]))
            self.assertTrue(result["format_conformant"])
            self.assertEqual(result["nearest_even_reference"]["identical"], green == 128)
        for pixel in ([63, 128, 191, 255], [65, 128, 191, 255], [64, 126, 191, 255],
                      [64, 129, 191, 255], [64, 128, 192, 255], [64, 128, 191, 254]):
            with self.subTest(pixel=pixel):
                self.assertFalse(mask.compare_mask(case, bytes(pixel))["format_conformant"])

    def test_mask_endpoints_and_rectangle_boundaries_remain_exact(self):
        case = mask.validate_fixture(self.fixture)["cases"][0]
        case["mode"] = "green_rect"
        pixels = bytearray(mask.mask_reference(case))
        pixels[1] = 1
        self.assertFalse(mask.compare_mask(case, pixels)["format_conformant"])
        pixels[1], pixels[5] = 0, 254
        self.assertFalse(mask.compare_mask(case, pixels)["format_conformant"])


if __name__ == '__main__':
    unittest.main()
