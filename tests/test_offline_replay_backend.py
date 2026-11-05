"""Real D3D12 sequence transport regression; metadata is not NGX History."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "offline"))
import contracts

EXECUTABLE = os.environ.get("NR_NEXUS_REPLAY_BACKEND_EXE")
ADAPTER = os.environ.get("NR_NEXUS_REPLAY_TEST_ADAPTER", "warp")


@unittest.skipUnless(EXECUTABLE and Path(EXECUTABLE).is_file(),
                     "Set NR_NEXUS_REPLAY_BACKEND_EXE to an already built real backend")
class OfflineReplayBackendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary_root = ROOT / "tmp" / "offline"
        temporary_root.mkdir(parents=True, exist_ok=True)
        cls.artifacts = Path(tempfile.mkdtemp(prefix="replay-backend-test-", dir=temporary_root))
        print(f"Replay backend regression artifacts: {cls.artifacts}", flush=True)

    def setUp(self):
        self.root = self.artifacts / self._testMethodName
        self.root.mkdir()
        self.sequence = self.root / "sequence"
        self.output = self.root / "output"

    def generate(self, width=19, height=17, frames=3):
        return contracts.prepare_sequence(self.sequence, width, height, frames, seed=7)

    def write_manifest(self, manifest):
        manifest["sequence_sha256"] = contracts.identity({key: value for key, value in manifest.items()
                                                         if key != "sequence_sha256"})
        (self.sequence / "sequence.json").write_text(json.dumps(manifest), encoding="utf-8")

    def run_backend(self, *extra, output=None, adapter=ADAPTER):
        return subprocess.run([EXECUTABLE, "--sequence", str(self.sequence), "--output", str(output or self.output),
                               "--adapter", adapter, *extra], capture_output=True, text=True, timeout=45, check=False)

    def read_result(self):
        return contracts.read_json(self.output / "replay-backend-result.json")

    def check_roundtrip(self, manifest, result):
        self.assertEqual(result["schema"], "nr-nexus-offline-replay-backend/1")
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["adapter"], ADAPTER)
        self.assertEqual(result["evidence"], "WARP" if ADAPTER == "warp" else "NATIVE_GPU")
        self.assertFalse(result["environment"]["ngx_loaded"])
        self.assertFalse(result["environment"]["network_evaluated"])
        self.assertFalse(result["history_rebuilt"])
        self.assertEqual(result["environment"]["software_adapter"], ADAPTER == "warp")
        self.assertEqual(result["sequence_sha256"], manifest["sequence_sha256"])
        self.assertEqual(len(result["frames"]), len(manifest["frames"]))
        previous_fence = 0
        for source, actual in zip(manifest["frames"], result["frames"]):
            self.assertEqual(actual["index"], source["index"])
            self.assertEqual(actual["status"], "passed")
            self.assertEqual(actual["controls"]["evidence"], "STATIC")
            for field in ("reset", "jitter", "motion_scale", "subrect"):
                self.assertEqual(actual["controls"][field], source[field])
            self.assertFalse(actual["controls"]["history_evaluated"])
            self.assertFalse(actual["network_evaluated"])
            self.assertTrue(actual["completion"]["gpu_completed"])
            self.assertGreater(actual["completion"]["fence_value"], previous_fence)
            previous_fence = actual["completion"]["fence_value"]
            for name, expected in source["resources"].items():
                resource = actual["resources"][name]
                self.assertEqual(resource["format"], expected["format"])
                self.assertEqual(resource["bytes"], expected["bytes"])
                self.assertTrue(resource["bitwise_equal"])
                output_file = contracts.artifact_path(self.output, resource["output_file"])
                source_file = contracts.artifact_path(self.sequence, expected["file"])
                self.assertEqual(output_file.read_bytes(), source_file.read_bytes())
                self.assertEqual(contracts.digest(output_file), expected["sha256"])

    def test_odd_dimensions_row_pitch_and_metadata_transcription(self):
        manifest = self.generate()
        manifest["frames"][1].update(reset=True, jitter=[.25, -.5], motion_scale=[2.0, .5])
        self.write_manifest(manifest)
        contracts.verify_sequence(self.sequence)
        completed = self.run_backend()
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        result = self.read_result()
        self.check_roundtrip(manifest, result)
        for resource in result["frames"][0]["resources"].values():
            self.assertGreater(resource["row_pitch"], resource["row_bytes"])
            self.assertEqual(resource["row_pitch"] % 256, 0)

    def test_one_pixel_wide_roundtrip(self):
        manifest = self.generate(width=1, height=3, frames=2)
        contracts.verify_sequence(self.sequence)
        completed = self.run_backend()
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.check_roundtrip(manifest, self.read_result())

    def test_nonempty_output_is_untouched(self):
        self.generate(frames=1)
        self.output.mkdir()
        sentinel = self.output / "sentinel"
        sentinel.write_bytes(b"keep existing evidence")
        self.assertNotEqual(self.run_backend().returncode, 0)
        self.assertEqual(sentinel.read_bytes(), b"keep existing evidence")
        self.assertEqual(list(self.output.iterdir()), [sentinel])

    def test_output_cannot_modify_sequence(self):
        self.generate(frames=1)
        before = {path.name: contracts.digest(path) for path in self.sequence.iterdir()}
        self.assertNotEqual(self.run_backend(output=self.sequence / "output").returncode, 0)
        self.assertEqual({path.name: contracts.digest(path) for path in self.sequence.iterdir()}, before)

    def test_truncated_later_frame_retains_completed_earlier_frame(self):
        manifest = self.generate(frames=2)
        path = self.sequence / manifest["frames"][1]["resources"]["depth"]["file"]
        path.write_bytes(path.read_bytes()[:-4])
        self.assertNotEqual(self.run_backend().returncode, 0)
        result = self.read_result()
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["frames"][0]["status"], "passed")
        self.assertTrue(result["frames"][0]["completion"]["gpu_completed"])
        self.assertEqual(result["frames"][1]["status"], "failed")
        self.assertFalse(result["frames"][1]["completion"]["gpu_completed"])
        resource = result["frames"][0]["resources"]["color"]
        self.assertEqual((self.output / resource["output_file"]).read_bytes(),
                         (self.sequence / manifest["frames"][0]["resources"]["color"]["file"]).read_bytes())

    def test_path_traversal_rejected(self):
        manifest = self.generate(frames=1)
        manifest["frames"][0]["resources"]["color"]["file"] = "../escape.f32"
        self.write_manifest(manifest)
        self.assertNotEqual(self.run_backend().returncode, 0)
        result = self.read_result()
        self.assertEqual(result["status"], "failed")
        self.assertFalse(result["frames"][0]["completion"]["gpu_completed"])
        self.assertEqual(result["frames"][0]["resources"], {})


if __name__ == "__main__":
    unittest.main()
