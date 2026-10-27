"""Renderer compatibility and run-local atlas reference tests; synthetic inputs only."""
import importlib.util
import json
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
from PIL import Image

spec = importlib.util.spec_from_file_location('capture_renderer', Path(__file__).with_name('render-ui-captures.py'))
renderer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(renderer)
TEMP_ROOT = Path(__file__).resolve().parents[1] / 'tmp'


class CaptureArtifacts(unittest.TestCase):
    def test_semantic_crop_rejects_stale_or_nonfinite_bounds(self):
        image = Image.new('RGB', (40, 30))
        self.assertEqual(renderer.checked_crop(image, [2, 3, 38, 28]).size, (36, 25))
        for bounds in ([0, 0, 41, 30], [-1, 0, 40, 30], [3, 3, 3, 20], [0, 0, float('nan'), 4]):
            with self.subTest(bounds=bounds), self.assertRaises(ValueError):
                renderer.checked_crop(image, bounds)

    def test_legacy_and_shared_atlas_render_identically(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as directory:
            root = Path(directory)
            texture = bytes([255, 255, 255, 255]) * 4
            mesh = dict(width=2, height=2, atlas_width=2, atlas_height=2, lists=[])
            legacy = root / 'legacy.json'
            legacy.write_text(json.dumps(mesh), encoding='utf-8')
            legacy.with_suffix('.rgba').write_bytes(texture)
            shared = root / 'shared.json'
            shared.write_text(json.dumps(dict(mesh, atlas_file='atlas-0.rgba')), encoding='utf-8')
            (root / 'atlas-0.rgba').write_bytes(texture)
            renderer.render(legacy)
            renderer.render(shared)
            self.assertEqual(legacy.with_suffix('.png').read_bytes(), shared.with_suffix('.png').read_bytes())

    def test_atlas_reference_cannot_escape(self):
        for name in ('../outside.rgba', '..\\outside.rgba', 'C:outside.rgba', '/outside.rgba', '', 3, 'x.png'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                renderer.capture_atlas(Path('capture.json'), dict(atlas_file=name))

    def test_missing_shared_atlas_is_an_error_when_rendered(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as directory:
            capture = Path(directory) / 'capture.json'
            capture.write_text(json.dumps(dict(width=1, height=1, atlas_width=1, atlas_height=1,
                                               lists=[], atlas_file='missing.rgba')), encoding='utf-8')
            with self.assertRaises(FileNotFoundError):
                renderer.render(capture)

    def test_truncated_capture_does_not_block_valid_outputs(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as directory:
            root = Path(directory)
            (root / 'broken.json').write_text('{', encoding='utf-8')
            (root / 'missing.json').write_text(json.dumps(dict(width=1, height=1, atlas_width=1,
                atlas_height=1, lists=[], atlas_file='missing.rgba')), encoding='utf-8')
            (root / 'valid.json').write_text(json.dumps(dict(width=1, height=1, atlas_width=1,
                                                            atlas_height=1, lists=[])), encoding='utf-8')
            (root / 'valid.rgba').write_bytes(bytes([255, 255, 255, 255]))
            result = subprocess.run([sys.executable, '-B', str(Path(renderer.__file__)), str(root)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn('Skipping broken.json', result.stderr)
            self.assertIn('Skipping missing.json', result.stderr)
            self.assertTrue((root / 'valid.png').exists())


if __name__ == '__main__':
    unittest.main()
