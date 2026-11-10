"""CPU math and comparison contract tests; no GPU success is inferred."""

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "look"))
import shadow_reference as ref


class ShadowReferenceTests(unittest.TestCase):
    def setUp(self):
        self.fixture = ref.generate()
        self.gpu = {"backend": "MOCK_CPU", "rows": [
            {"id": s["id"], "shadows": sh,
             "dispatched": sh != 1,
             "output": ref.reference(s["proxy"], s["neural"], sh)["output"]}
            for s in self.fixture["samples"] for sh in ref.SHADOWS]}

    def test_analytic_endpoints_and_finite_outputs(self):
        self.assertTrue(ref.selftest()["pass"])

    def test_floor_transition_retains_residual_edit(self):
        level = (ref.FLOOR + ref.TRUST_TOP) / 2
        p, n = [ref.encode(level)] * 3, [ref.encode(level * 2)] * 3
        result = ref.reference(p, n, 0)
        self.assertAlmostEqual(result["floor_trust"], 0.5, places=5)
        self.assertAlmostEqual(result["output_linear"][0], level * 2 ** 0.5, places=7)

    def test_zone_transition_and_bright_unchanged(self):
        def evaluate(level):
            return ref.reference([ref.encode(level)] * 3,
                                 [ref.encode(level * 1.1)] * 3, 0)
        self.assertAlmostEqual(evaluate(0.18 * 2 ** -2.5)["zone_gain"], 0.5, places=5)
        self.assertEqual(evaluate(0.18)["zone_gain"], 1)
        self.assertEqual(evaluate(0.7)["zone_gain"], 1)

    def test_complete_comparison_and_missing_row(self):
        result = ref.compare(self.fixture, self.gpu)
        self.assertTrue(result["pass"])
        self.assertEqual(result["expected_rows"], 180)
        self.gpu["rows"].pop()
        result = ref.compare(self.fixture, self.gpu)
        self.assertFalse(result["pass"])
        self.assertEqual(len(result["missing"]), 1)

    def test_duplicate_and_unknown_rows_rejected(self):
        bad = copy.deepcopy(self.gpu)
        bad["rows"].append(bad["rows"][0])
        with self.assertRaises(ValueError):
            ref.compare(self.fixture, bad)
        self.gpu["rows"][0]["id"] = "unknown"
        with self.assertRaises(ValueError):
            ref.compare(self.fixture, self.gpu)

    def test_bad_output_and_wrong_numeric_result(self):
        bad = copy.deepcopy(self.gpu)
        bad["rows"][0]["output"] = [float("nan"), 0, 0]
        with self.assertRaises(ValueError):
            ref.compare(self.fixture, bad)
        self.gpu["rows"][0]["output"] = [0.5] * 3
        self.assertFalse(ref.compare(self.fixture, self.gpu)["pass"])

    def test_half_storage_and_dispatch_contract(self):
        for sample in self.fixture["samples"]:
            for field in ("proxy", "neural"):
                self.assertEqual(sample[field], list(map(ref.half, sample[field])))
        self.gpu["rows"][0]["dispatched"] = True
        self.assertFalse(ref.compare(self.fixture, self.gpu)["pass"])

    def test_one_half_ulp_tolerance_and_exact_bypass(self):
        row = next(r for r in self.gpu["rows"] if r["id"] == "normal_dark/brighten" and r["shadows"] == 0)
        bits = ref.struct.unpack("<H", ref.struct.pack("<e", row["output"][0]))[0]
        row["output"][0] = ref.struct.unpack("<e", ref.struct.pack("<H", bits + 1))[0]
        self.assertTrue(ref.compare(self.fixture, self.gpu)["pass"])
        row["output"][0] = ref.struct.unpack("<e", ref.struct.pack("<H", bits + 2))[0]
        self.assertFalse(ref.compare(self.fixture, self.gpu)["pass"])


if __name__ == "__main__":
    unittest.main()
