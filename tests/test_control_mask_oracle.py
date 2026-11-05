"""Real WARP/backend regressions. Run with python -B; retain evidence in tmp/offline.

Set NR_NEXUS_OFFLINE_GPU_EXE to an already built offline_gpu_tests.exe.
An absent executable is an explicit skip; no test substitutes a GPU result.
"""

import copy
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
EXECUTABLE = os.environ.get("NR_NEXUS_OFFLINE_GPU_EXE")
sys.path.insert(0, str(ROOT / "tools" / "offline"))
import contracts as c
import control_mask_contracts as mask


# SDK float constant; D3D11 functional spec section 3.2.3.6 defines integer-side ULPs.
# https://github.com/microsoft/DirectX-Headers/blob/main/include/directx/d3d12.idl
# https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#FLOATtoUNORM
D3D12_FLOAT32_TO_INTEGER_TOLERANCE_IN_ULP = struct.unpack("<f", struct.pack("<f", 0.6))[0]
REFERENCE_POLICY = "d3d-float-to-unorm-0.6-ulp"


def measure_mask_quantization(case, pixels):
    """Independent CPU oracle, also exercised with deliberately invalid synthetic bytes."""
    if len(pixels) != case["width"] * case["height"] * 4:
        raise ValueError("Invalid RGBA8 byte length")
    base = [struct.unpack("<f", struct.pack("<f", value))[0] * 255.0 for value in case["rgb"]] + [255.0]
    conforms, differences, maximum = True, 0, 0.0
    for y in range(case["height"]):
        for x in range(case["width"]):
            ideal = base[:]
            if case["mode"] == "neutral":
                ideal = [255.0] * 4
            elif case["mode"] in ("green_rect", "blue_rect"):
                ideal = [255.0] * 4
                if x < case["width"] // 2 and y < case["height"] // 2:
                    ideal[1 if case["mode"] == "green_rect" else 2] = 0.0
            elif case["mode"] == "rect" and x < case["rect"][0] and y < case["rect"][1]:
                ideal = [0.0, 0.0, 0.0, 255.0]
            offset = (y * case["width"] + x) * 4
            for channel, continuous in enumerate(ideal):
                actual = pixels[offset + channel]
                error = abs(actual - continuous)
                maximum = max(maximum, error)
                differences += actual != round(continuous)
                if continuous in (0.0, 255.0):
                    conforms = conforms and actual == continuous
                else:
                    conforms = conforms and error <= D3D12_FLOAT32_TO_INTEGER_TOLERANCE_IN_ULP
    return {"format_conformant": conforms, "exact_reference_differences": differences,
            "max_quantization_error_ulp": maximum, "bitwise_identical_to_reference": differences == 0}


class OfflineGpuOracleTests(unittest.TestCase):
    """CPU oracle checks only; these synthetic inputs never assert a GPU success."""

    def setUp(self):
        self.case = mask.validate_fixture({"schema": mask.FIXTURE_SCHEMA, "fixture_id": "oracle",
            "cases": [{"id": "one", "width": 1, "height": 1, "mode": "rgb",
                       "rgb": [.5, .25, 1.0]}]})["cases"][0]

    def test_half_accepts_127_128_and_rejects_126_129(self):
        for actual in (126, 127, 128, 129):
            with self.subTest(actual=actual):
                result = measure_mask_quantization(self.case, bytes([actual, 64, 255, 255]))
                self.assertEqual(result["format_conformant"], actual in (127, 128))
                self.assertEqual(result["max_quantization_error_ulp"], abs(actual - 127.5))
                self.assertEqual(result["bitwise_identical_to_reference"], actual == 128)

    def test_quarter_does_not_allow_arbitrary_one_lsb(self):
        for actual in (63, 64, 65):
            with self.subTest(actual=actual):
                result = measure_mask_quantization(self.case, bytes([128, actual, 255, 255]))
                self.assertEqual(result["format_conformant"], actual == 64)
        self.assertEqual(measure_mask_quantization(self.case, bytes([128, 63, 255, 255]))["max_quantization_error_ulp"], .75)

    def test_zero_one_and_alpha_remain_exact(self):
        self.case["rgb"] = [0.0, 1.0, 1.0]
        reference = bytes([0, 255, 255, 255])
        self.assertTrue(measure_mask_quantization(self.case, reference)["format_conformant"])
        for channel in range(4):
            pixels = bytearray(reference)
            pixels[channel] = 1 if channel == 0 else 254
            with self.subTest(channel=channel):
                self.assertFalse(measure_mask_quantization(self.case, pixels)["format_conformant"])

    def test_region_boundaries_remain_exact(self):
        for mode in ("rect", "green_rect", "blue_rect"):
            self.case.update(width=3, height=3, mode=mode, rect=[1, 1], rgb=[1.0, 1.0, 1.0])
            reference = mask.mask_reference(self.case)
            self.assertTrue(measure_mask_quantization(self.case, reference)["format_conformant"])
            for offset in (0, 4, 12):
                pixels = bytearray(reference)
                channel = {"rect": 0, "green_rect": 1, "blue_rect": 2}[mode]
                pixels[offset + channel] += 1 if pixels[offset + channel] == 0 else -1
                with self.subTest(mode=mode, offset=offset):
                    self.assertFalse(measure_mask_quantization(self.case, pixels)["format_conformant"])

    def test_oracle_uses_float32_shader_input(self):
        self.case["rgb"][0] = .3
        observed = measure_mask_quantization(self.case, bytes([77, 64, 255, 255]))
        self.assertTrue(observed["bitwise_identical_to_reference"])
        self.assertTrue(observed["format_conformant"])
        self.assertGreater(measure_mask_quantization(self.case, bytes([76, 64, 255, 255]))["max_quantization_error_ulp"], .5)



if __name__ == '__main__':
    unittest.main()
