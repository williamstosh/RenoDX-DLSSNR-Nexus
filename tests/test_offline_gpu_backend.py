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
        self.case = c.validate_fixture({"schema": c.FIXTURE_SCHEMA, "fixture_id": "oracle",
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
            reference = c.mask_reference(self.case)
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


@unittest.skipUnless(os.name == "nt" and EXECUTABLE and Path(EXECUTABLE).is_file(),
                     "Set NR_NEXUS_OFFLINE_GPU_EXE to an already built real Windows GPU backend")
class OfflineGpuBackendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = Path(EXECUTABLE).resolve(strict=True)
        temporary_root = ROOT / "tmp" / "offline"
        temporary_root.mkdir(parents=True, exist_ok=True)
        # Keep inputs, output, and process logs after assertion failures and timeouts.
        # Successful evidence is also retained; this suite never deletes run directories.
        cls.artifacts = Path(tempfile.mkdtemp(prefix="gpu-backend-regression-", dir=temporary_root))
        print(f"Offline GPU regression artifacts: {cls.artifacts}", file=sys.stderr)

    def setUp(self):
        self.root = self.artifacts / self._testMethodName
        self.root.mkdir()
        self.invocations = 0
        self.fixture = c.validate_fixture({
            "schema": c.FIXTURE_SCHEMA, "fixture_id": "backend-regression",
            "cases": [{"id": "one", "width": 3, "height": 1}],
        })

    def invoke(self, arguments):
        self.invocations += 1
        prefix = self.root / f"process-{self.invocations:02d}"
        command = [str(self.executable), *map(str, arguments)]
        c.write_json(prefix.with_suffix(".command.json"), command)
        with prefix.with_suffix(".stdout.log").open("xb") as stdout, prefix.with_suffix(".stderr.log").open("xb") as stderr:
            completed = subprocess.run(
                command, cwd=self.root, stdout=stdout, stderr=stderr,
                timeout=45, check=False, creationflags=subprocess.CREATE_NO_WINDOW,
            )
        c.write_json(prefix.with_suffix(".exit.json"), {"returncode": completed.returncode})
        return completed

    def fixture_arguments(self, fixture=None, name="case", adapter="warp"):
        path = self.root / f"{name}.fixture.json"
        c.write_json(path, self.fixture if fixture is None else fixture)
        output = self.root / f"{name}.output"
        return ["--fixture", path, "--output", output, "--adapter", adapter], output

    def assert_failed_case(self, output):
        result = c.read_json(output / "backend-result.json")
        self.assertEqual(result["status"], "failed")
        self.assertEqual(len(result["cases"]), 1)
        case = result["cases"][0]
        self.assertEqual(case["status"], "failed")
        self.assertFalse(case["completion"]["gpu_completed"])
        self.assertEqual(case["timing"]["samples"], [])
        self.assertIsNone(case["pixels_file"])
        self.assertFalse(list(output.glob("*.rgba8")))
        return case

    def test_real_warp_smoke(self):
        fixture = c.validate_fixture(c.read_json(ROOT / "tests" / "fixtures" / "offline" / "control-mask-smoke.json"))
        arguments, output = self.fixture_arguments(fixture, "smoke")
        self.assertEqual(self.invoke(arguments).returncode, 0)
        result = c.read_json(output / "backend-result.json")
        self.assertEqual(result["schema"], "nr-nexus-offline-backend/1")
        self.assertEqual(result["backend"], "d3d12-control-mask")
        self.assertEqual(result["adapter"], "warp")
        self.assertEqual(result["status"], "passed")
        self.assertEqual(len(result["cases"]), len(fixture["cases"]))
        self.assertIs(result["environment"]["network_evaluated"], False)
        self.assertIsInstance(result["environment"]["description"], str)
        for expected, observed in zip(fixture["cases"], result["cases"]):
            with self.subTest(case=expected["id"]):
                self.assertEqual(observed["id"], expected["id"])
                self.assertEqual(observed["status"], "passed")
                self.assertEqual(observed["evidence"], "WARP")
                self.assertEqual(observed["format"], "R8G8B8A8_UNORM")
                self.assertEqual((observed["width"], observed["height"]), (expected["width"], expected["height"]))
                pixels = c.artifact_path(output, observed["pixels_file"]).read_bytes()
                comparison = measure_mask_quantization(expected, pixels)
                self.assertTrue(comparison["format_conformant"])
                self.assertEqual(observed["reference_policy"], REFERENCE_POLICY)
                self.assertEqual(observed["pixel_checks"], len(pixels) * expected["frames"])
                self.assertEqual(observed["exact_reference_differences"],
                                 comparison["exact_reference_differences"] * expected["frames"])
                self.assertAlmostEqual(observed["max_quantization_error_ulp"], comparison["max_quantization_error_ulp"])
                self.assertEqual(observed["bitwise_identical_to_reference"], comparison["bitwise_identical_to_reference"])
                contract = observed["parameter_contract"]
                self.assertEqual(contract["evidence"], "MOCK")
                self.assertEqual(contract["model_request"], expected["model"])
                self.assertIs(contract["auto_mask"], expected["auto_mask"])
                self.assertEqual(contract["blend_mode"], expected["blend_mode"])
                self.assertIs(contract["network_evaluated"], False)
                timing = observed["timing"]
                self.assertEqual(timing["scope"], "control-mask-dispatch")
                self.assertGreater(timing["frequency_hz"], 0)
                self.assertEqual(len(timing["samples"]), expected["frames"])
                previous_serial = 0
                for index, sample in enumerate(timing["samples"]):
                    self.assertIs(sample["format_conformant"], True)
                    self.assertEqual(sample["exact_reference_differences"], comparison["exact_reference_differences"])
                    self.assertAlmostEqual(sample["max_quantization_error_ulp"], comparison["max_quantization_error_ulp"])
                    self.assertEqual(sample["bitwise_identical_to_reference"], comparison["bitwise_identical_to_reference"])
                    self.assertEqual(sample["frame"], index)
                    self.assertIs(sample["warmup"], index < expected["warmup"])
                    self.assertGreaterEqual(sample["end_tick"], sample["begin_tick"])
                    self.assertAlmostEqual(sample["gpu_ms"], (sample["end_tick"] - sample["begin_tick"]) * 1000 / timing["frequency_hz"])
                    self.assertGreater(sample["completion_serial"], previous_serial)
                    previous_serial = sample["completion_serial"]
                self.assertIs(observed["completion"]["gpu_completed"], True)
                self.assertEqual(observed["completion"]["fence_value"], previous_serial)

    def test_unsafe_case_ids_cannot_escape_output(self):
        for index, identifier in enumerate(("../escape", "..\\escape", "bad/name", "C:escape", "", "CON")):
            with self.subTest(identifier=identifier):
                fixture = copy.deepcopy(self.fixture)
                fixture["cases"][0]["id"] = identifier
                arguments, output = self.fixture_arguments(fixture, f"invalid-id-{index}")
                self.assertNotEqual(self.invoke(arguments).returncode, 0)
                self.assert_failed_case(output)
        self.assertFalse((self.root / "escape.rgba8").exists())
        self.assertEqual(list(self.root.glob("*.rgba8")), [])

    def test_invalid_frames_and_warmup(self):
        for index, (field, value) in enumerate((("frames", 0), ("frames", -1), ("frames", True),
                                                ("frames", 1.5), ("warmup", -1), ("warmup", 8),
                                                ("warmup", 9), ("warmup", True))):
            with self.subTest(field=field, value=value):
                fixture = copy.deepcopy(self.fixture)
                fixture["cases"][0][field] = value
                arguments, output = self.fixture_arguments(fixture, f"invalid-frame-{index}")
                self.assertNotEqual(self.invoke(arguments).returncode, 0)
                self.assert_failed_case(output)

    def test_required_cli_arguments_and_values(self):
        arguments, output = self.fixture_arguments()
        self.assertNotEqual(self.invoke([]).returncode, 0)
        for index in (0, 2, 4):
            with self.subTest(missing=arguments[index]):
                self.assertNotEqual(self.invoke(arguments[:index] + arguments[index + 2:]).returncode, 0)
                self.assertFalse(output.exists())
        for option in ("--fixture", "--output", "--adapter"):
            with self.subTest(missing_value=option):
                self.assertNotEqual(self.invoke([option]).returncode, 0)
                self.assertFalse(output.exists())

    def test_duplicate_cli_is_rejected(self):
        arguments, output = self.fixture_arguments()
        for index in (0, 2, 4):
            with self.subTest(option=arguments[index]):
                self.assertNotEqual(self.invoke(arguments + arguments[index:index + 2]).returncode, 0)
                self.assertFalse(output.exists())

    def test_nonempty_output_is_not_overwritten(self):
        arguments, output = self.fixture_arguments()
        output.mkdir()
        sentinel = output / "keep.bin"
        sentinel.write_bytes(b"keep existing output\0")
        self.assertNotEqual(self.invoke(arguments).returncode, 0)
        self.assertEqual(sentinel.read_bytes(), b"keep existing output\0")
        self.assertEqual(list(output.iterdir()), [sentinel])

    def test_invalid_adapter_never_falls_back(self):
        for index, adapter in enumerate(("auto", "cpu", "NVIDIA", "warp-typo", "")):
            with self.subTest(adapter=adapter):
                arguments, output = self.fixture_arguments(name=f"adapter-{index}", adapter=adapter)
                completed = self.invoke(arguments)
                self.assertNotEqual(completed.returncode, 0)
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
