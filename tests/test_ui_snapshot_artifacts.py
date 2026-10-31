"""Synthetic-only packaging tests. All generated inputs/outputs live under repo tmp."""
import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image

import ui_snapshot_artifacts as snapshots


TEMP_ROOT = Path(__file__).resolve().parents[1] / 'tmp'


class SnapshotArtifacts(unittest.TestCase):
    def setUp(self):
        TEMP_ROOT.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=TEMP_ROOT)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.captures = self.root / 'menu'
        self.captures.mkdir()
        self.registry_path = self.captures / 'captures.json'
        self.source = self.root / 'source-state.json'
        self.source.write_text('\ufeff' + json.dumps({'head': 'synthetic', 'dirty': True,
                                                     'files_sha256': {'fixture': '123'}}), encoding='utf-8')
        self.renderer = snapshots.renderer_module()
        self.entry = dict(id='quality-expanded', file='mesh.json', page=0, category='page',
                          state='expanded', theme='cyber_neon', language=14, font_size=18,
                          width=24, height=32, kind='long', interaction='click expand all',
                          regions={'doc': [0, 0, 24, 32]}, doc_role='quality')
        # Two triangles form an inset rectangle; the surrounding background makes
        # both the full image and a semantic crop visibly nonblank.
        white = 0xFFFFFFFF
        mesh = {key: self.entry[key] for key in ('page', 'theme', 'language', 'font_size', 'width', 'height', 'regions')}
        mesh.update(atlas_width=1, atlas_height=1, atlas_file='atlas.rgba', lists=[{
            'vertices': [[3, 3, 0, 0, white], [20, 3, 0, 0, white],
                         [20, 27, 0, 0, white], [3, 27, 0, 0, white]],
            'indices': [0, 1, 2, 0, 2, 3], 'commands': [[0, 6, 0, 0, 0, 24, 32]]}])
        self.mesh = mesh
        (self.captures / 'atlas.rgba').write_bytes(bytes([255, 255, 255, 255]))
        self.write_inputs()

    def write_inputs(self, entries=None, expected=None):
        snapshots.write_json(self.captures / 'mesh.json', self.mesh)
        snapshots.write_json(self.registry_path, {'schema': 'synthetic-captures/1',
                             'expected': expected if expected is not None else [self.entry['id']],
                             'captures': entries if entries is not None else [self.entry],
                             'limitations': ['synthetic only']})

    def build(self, name='candidate', **kwargs):
        with contextlib.redirect_stdout(io.StringIO()):
            return snapshots.build_snapshot(self.registry_path, self.root / name, self.source,
                                            renderer=self.renderer, **kwargs)

    def complete_doc_registry(self):
        entries = []
        for role in sorted(snapshots.REQUIRED_DOC_ROLES):
            for language in (1, 14):
                identifier = role + ('-zh' if language == 14 else '-en')
                if role == 'quality' and language == 14:
                    identifier = self.entry['id']
                theme = role[6:] if role.startswith('theme-') else 'cyber_neon'
                entry = dict(self.entry, id=identifier, file=identifier + '.json',
                             language=language, theme=theme, doc_role=role)
                snapshots.write_json(self.captures / entry['file'],
                                     dict(self.mesh, theme=theme, language=language))
                entries.append(entry)
        self.write_inputs(entries=entries, expected=[entry['id'] for entry in entries])

    def test_complete_metadata_source_hashes_and_offline_html(self):
        manifest = self.build()
        self.assertEqual(manifest['status'], 'complete', manifest['failures'])
        self.assertEqual(manifest['source'], snapshots.read_json(self.source))
        self.assertTrue(manifest['synthetic'])
        item = manifest['entries'][0]
        path = self.root / 'candidate' / item['image']
        self.assertEqual(item['sha256'], snapshots.sha256(path))
        self.assertEqual(item['language'], 14)
        with Image.open(path) as image:
            self.assertEqual(image.size, (24, 32))
        overview = (self.root / 'candidate/index.html').read_text(encoding='utf-8')
        self.assertIn('href="images/quality-expanded.webp"', overview)
        self.assertNotIn('https://', overview)
        self.assertNotIn('../previous/index.html', overview)
        self.assertEqual(sorted(p.suffix for p in (self.root / 'candidate/images').iterdir()), ['.webp'])

    def test_incomplete_registry_fails_with_manifest_and_index(self):
        self.write_inputs(expected=[self.entry['id'], 'missing'])
        manifest = self.build()
        self.assertEqual(manifest['status'], 'partial')
        self.assertTrue(any('missing expected' in failure['reason'] for failure in manifest['failures']))
        self.assertTrue((self.root / 'candidate/index.html').is_file())
        self.assertEqual(snapshots.read_json(self.root / 'candidate/manifest.json')['status'], 'partial')

    def test_path_traversal_and_unsafe_ids_rejected(self):
        for index, name in enumerate(('../escape.json', '..\\escape.json', '/escape.json',
                                      'C:escape.json', 'folder/../mesh.json', 'mesh.json:stream')):
            with self.subTest(name=name):
                entry = dict(self.entry, file=name)
                self.write_inputs(entries=[entry])
                result = self.build(name='bad' + str(index))
                self.assertEqual(result['status'], 'partial')
                self.assertEqual(result['entries'], [])
        self.write_inputs(entries=[dict(self.entry, id='../escape')])
        self.assertEqual(self.build(name='unsafe-id')['status'], 'partial')

    def test_duplicate_and_unexpected_ids_fail(self):
        self.write_inputs(entries=[self.entry, copy.deepcopy(self.entry)])
        result = self.build(name='duplicate')
        self.assertEqual(result['status'], 'partial')
        self.assertTrue(any('duplicate' in failure['reason'] for failure in result['failures']))
        self.write_inputs(entries=[dict(self.entry, id='unexpected')])
        self.assertEqual(self.build(name='unexpected')['status'], 'partial')

    def test_html_escapes_metadata_source_and_errors(self):
        payload = '<script>alert("x")</script>'
        self.entry.update(category=payload, interaction=payload, limitations=[payload])
        self.write_inputs(expected=[self.entry['id'], 'missing'])
        self.build()
        overview = (self.root / 'candidate/index.html').read_text(encoding='utf-8')
        self.assertNotIn(payload, overview)
        self.assertIn('&lt;script&gt;', overview)

    def test_mesh_metadata_mismatch_fails(self):
        for key, value in (('width', 25), ('height', 33), ('theme', 'other'),
                           ('language', 1), ('font_size', 20), ('page', 3)):
            with self.subTest(key=key):
                self.write_inputs(entries=[dict(self.entry, **{key: value})])
                result = self.build(name='mismatch-' + key)
                self.assertEqual(result['status'], 'partial')
                self.assertIn('metadata mismatch', result['failures'][0]['reason'])

    def test_blank_and_failed_checks_rejected(self):
        self.mesh['lists'][0]['commands'] = []
        self.write_inputs()
        self.assertEqual(self.build(name='blank')['status'], 'partial')
        self.entry['checks'] = {'bottom_visible': False}
        self.write_inputs()
        self.assertIn('failed checks', self.build(name='checks')['failures'][0]['reason'])

    def test_doc_bounds_reject_and_existing_preserved(self):
        existing = self.root / 'existing'
        existing.mkdir()
        (existing / 'quality-zh.webp').write_bytes(b'protected old doc')
        for index, bounds in enumerate(([0, 0, 25, 32], [0, 0, 1, 1], [0, -1, 24, 32],
                                         [0, 0, float('nan'), 32], None)):
            self.entry['regions'] = self.mesh['regions'] = {'doc': bounds}
            self.write_inputs()
            docs = self.root / ('docs-' + str(index))
            result = self.build(name='bounds-' + str(index), documentation=docs, existing_docs=existing)
            self.assertEqual(result['status'], 'partial')
            report = snapshots.read_json(docs / 'docs-report.json')
            self.assertEqual(report['status'], 'partial')
            self.assertEqual(report['images'], [])
            self.assertEqual((existing / 'quality-zh.webp').read_bytes(), b'protected old doc')

    def test_no_change_comparison_and_previous_links(self):
        self.complete_doc_registry()
        docs = self.root / 'docs'
        first = self.build(documentation=docs)
        self.assertEqual(first['status'], 'complete', first['failures'])
        result = self.build(name='next', documentation=self.root / 'next-docs', existing_docs=docs,
                            previous=self.root / 'candidate')
        self.assertEqual(result['status'], 'complete', result['failures'])
        report = snapshots.read_json(self.root / 'next-docs/docs-report.json')
        item = next(item for item in report['images'] if item['file'] == 'quality-zh.webp')
        self.assertEqual(item['file'], 'quality-zh.webp')
        self.assertFalse(item['changed'])
        self.assertTrue(item['pixel_identical'])
        self.assertEqual(item['old_sha256'], item['candidate_sha256'])
        self.assertEqual(item['mean_absolute_error'], 0)
        self.assertIn('../previous/images/quality-expanded.webp',
                      (self.root / 'next/index.html').read_text(encoding='utf-8'))

    def test_existing_output_refused_without_touching_files(self):
        output = self.root / 'candidate'
        output.mkdir()
        (output / 'sentinel').write_text('keep', encoding='utf-8')
        with self.assertRaises(FileExistsError):
            self.build()
        self.assertEqual((output / 'sentinel').read_text(), 'keep')

    def test_rendered_dimensions_checked_independently(self):
        original = self.renderer.render

        def wrong_dimensions(path, output, image_format):
            result = original(path, output, image_format)
            Image.new('RGB', (3, 4)).save(result)
            return result

        with patch.object(self.renderer, 'render', wrong_dimensions):
            result = self.build()
        self.assertEqual(result['status'], 'partial')
        self.assertIn('rendered dimensions', result['failures'][0]['reason'])

    def test_theme_sheet_preserves_native_tile_dimensions(self):
        self.complete_doc_registry()
        result = self.build(documentation=self.root / 'docs')
        self.assertEqual(result['status'], 'complete', result['failures'])
        with Image.open(self.root / 'docs/themes-zh.webp') as sheet:
            self.assertEqual(sheet.size, (48, 144))
            with Image.open(self.root / 'docs/theme-cyber_neon-zh.webp') as tile:
                self.assertEqual(sheet.crop((0, 40, 24, 72)).tobytes(), tile.convert('RGB').tobytes())

    def test_directory_input_and_invalid_registry(self):
        with contextlib.redirect_stdout(io.StringIO()):
            result = snapshots.build_snapshot(self.captures, self.root / 'directory', self.source)
        self.assertEqual(result['status'], 'complete', result['failures'])
        self.registry_path.write_text('{', encoding='utf-8')
        result = self.build(name='invalid')
        self.assertEqual(result['status'], 'partial')
        self.assertTrue((self.root / 'invalid/index.html').is_file())

    def test_documentation_missing_role_languages_is_partial(self):
        result = self.build(documentation=self.root / 'docs')
        self.assertEqual(result['status'], 'partial')
        report = snapshots.read_json(self.root / 'docs/docs-report.json')
        missing = {(item.get('role'), item.get('language')) for item in report['failures']}
        self.assertIn(('quality', 1), missing)
        self.assertIn(('theme-midnight_oled', 14), missing)

    def test_cli_nonzero_partial_and_clear_status(self):
        self.write_inputs(expected=[self.entry['id'], 'missing'])
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            code = snapshots.main(['--captures', str(self.registry_path), '--output', str(self.root / 'cli'),
                                   '--source', str(self.source), '--mode', 'Quick'])
        self.assertEqual(code, 1)
        self.assertIn('"status": "partial"', buffer.getvalue())
        self.assertIn('missing expected captures', buffer.getvalue())


if __name__ == '__main__':
    unittest.main()
