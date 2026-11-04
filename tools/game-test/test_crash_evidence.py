"""Offline evidence tests; all inputs are synthetic."""
import json
import os
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock

import crash_evidence


class CrashEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.install = self.root / 'install'
        self.install.mkdir()
        self.cfg = {'executable': str(self.install / 'game.exe')}
        self.started = datetime.now(timezone.utc).isoformat()

    def test_pre_window_has_complete_ini_and_unavailable_modules(self):
        data = b'[GENERAL]\r\nKey=123\r\n'
        (self.install / 'ReShade.ini').write_bytes(data)
        crash_evidence.configurations(self.cfg, self.root / 'config')
        self.assertEqual((self.root / 'config/ReShade.ini').read_bytes(), data)
        self.assertEqual(crash_evidence.modules(None, mock.Mock())['status'], 'unavailable')

    def test_existing_dump_preserved_and_no_overwrite(self):
        source = self.install / 'game.dmp'
        source.write_bytes(b'dump')
        run = mock.Mock(return_value=mock.Mock(stdout='<Event/>'))
        with mock.patch.dict('os.environ', {'LOCALAPPDATA': str(self.root / 'local')}):
            crash_evidence.collect(self.cfg, self.root, self.started, 'first', run)
        target = self.root / 'crash-evidence/first/install-dumps/game.dmp'
        self.assertEqual(target.read_bytes(), b'dump')
        source.write_bytes(b'new')
        with self.assertRaises(FileExistsError):
            crash_evidence.copy_snapshot(source, target)
        self.assertEqual(target.read_bytes(), b'dump')
        self.assertEqual(source.read_bytes(), b'new')

    def test_query_failure_is_explicit(self):
        with mock.patch.dict('os.environ', {'LOCALAPPDATA': str(self.root / 'local')}):
            records = crash_evidence.collect(self.cfg, self.root, self.started, 'failed',
                                             mock.Mock(side_effect=OSError('query failed')))
        self.assertIn('query failed', records[0]['error'])
        self.assertEqual(json.loads((self.root / 'crash-evidence/failed/manifest.json').read_text())[0], records[0])

    def test_module_query_failure_is_not_loaded(self):
        result = crash_evidence.modules({'pid': 1}, mock.Mock(side_effect=OSError('gone')))
        self.assertEqual(result['status'], 'unavailable')
        self.assertEqual(result['modules'], [])

    def test_module_hash_budget_prioritizes_required_and_retains_paths(self):
        paths = [self.root / name for name in ('other.dll', 'sl.interposer.dll', 'required.addon64')]
        for path in paths:
            path.write_bytes(b'abcd')
        with mock.patch.object(crash_evidence, 'MAX_BYTES', 8):
            result = crash_evidence.modules({'pid': 1}, lambda _: [str(p) for p in paths], 'required.addon64')
        entries = result['modules']
        self.assertEqual([x['path'] for x in entries], [str(p) for p in paths])
        self.assertEqual([x['hash_status'] for x in entries], ['budget_skipped', 'hashed', 'hashed'])
        self.assertIn('disk_sha256', entries[2])
        self.assertNotIn('sha256', entries[2])
        self.assertEqual(result['hash_bytes_reserved'], 8)

    def test_module_path_limit_is_explicit(self):
        paths = [str(self.root / f'missing-{index}') for index in range(4097)]
        result = crash_evidence.modules({'pid': 1}, lambda _: paths)
        self.assertEqual(result['total'], 4097)
        self.assertEqual(len(result['modules']), 4096)
        self.assertTrue(result['paths_truncated'])

    def test_programdata_only_target_and_zero_events(self):
        shared = self.root / 'shared'
        archive = shared / 'Microsoft/Windows/WER/ReportArchive'
        target = archive / 'AppCrash_game.exe_abc'
        unrelated = archive / 'AppCrash_other.exe_abc'
        streamline = shared / 'NVIDIA/Streamline/game/run1'
        other_streamline = shared / 'NVIDIA/Streamline/other/run1'
        for directory in (target, unrelated, streamline, other_streamline):
            directory.mkdir(parents=True)
        (target / 'Report.wer').write_text('target')
        os.utime(target / 'Report.wer', (1, 1))
        (target / 'current.wer').write_text('current')
        (unrelated / 'Report.wer').write_text('other')
        (streamline / 'sl-sha.dmp').write_bytes(b'dump')
        (other_streamline / 'sl-sha.dmp').write_bytes(b'other')
        run = mock.Mock(return_value=mock.Mock(stdout=''))
        with mock.patch.dict('os.environ', {'PROGRAMDATA': str(shared), 'LOCALAPPDATA': str(self.root / 'local')}):
            records = crash_evidence.collect(self.cfg, self.root, self.started, 'shared', run)
        dest = self.root / 'crash-evidence/shared'
        self.assertEqual(records[0]['status'], 'zero_events')
        self.assertFalse((dest / 'shared-wer-archive/AppCrash_game.exe_abc/Report.wer').exists())
        self.assertEqual((dest / 'shared-wer-archive/AppCrash_game.exe_abc/current.wer').read_text(), 'current')
        self.assertEqual((dest / 'streamline/run1/sl-sha.dmp').read_bytes(), b'dump')
        self.assertNotIn('other', json.dumps(records))
        self.assertEqual(json.loads(run.call_args.kwargs['input'])['started'], self.started)

    def test_old_dumps_do_not_use_current_copy_budget(self):
        shared = self.root / 'shared'
        folder = shared / 'NVIDIA/Streamline/game/run'
        folder.mkdir(parents=True)
        for index in range(70):
            old = folder / f'old-{index}.dmp'
            old.write_bytes(b'old')
            os.utime(old, (1, 1))
        (folder / 'current.dmp').write_bytes(b'current')
        with mock.patch.dict('os.environ', {'PROGRAMDATA': str(shared), 'LOCALAPPDATA': str(self.root / 'local')}), \
             mock.patch.object(crash_evidence, 'MAX_FILES', 1):
            records = crash_evidence.collect(self.cfg, self.root, self.started, 'budget',
                                             mock.Mock(return_value=mock.Mock(stdout='')))
        self.assertEqual((self.root / 'crash-evidence/budget/streamline/run/current.dmp').read_bytes(), b'current')
        self.assertEqual(sum(r.get('reason') == 'outside_attempt_time' for r in records), 70)

    def test_budgets_and_configuration_settings(self):
        profile = self.root / 'profile'
        profile.mkdir()
        (profile / 'user.settings').write_bytes(b'config')
        (self.install / 'ReShade.ini').write_bytes(b'ini')
        self.cfg['profile'] = str(profile)
        records = crash_evidence.configurations(self.cfg, self.root / 'config')
        self.assertEqual((self.root / 'config/profile/user.settings').read_bytes(), b'config')
        self.assertEqual(len(records), 2)
        with mock.patch.object(crash_evidence, 'MAX_BYTES', 3):
            records = crash_evidence.configurations(self.cfg, self.root / 'limited')
        self.assertTrue(any(r.get('status') == 'skipped' for r in records))
        with mock.patch.object(crash_evidence, 'MAX_FILES', 1):
            records = crash_evidence.configurations(self.cfg, self.root / 'file-limited')
        self.assertTrue(any(r.get('status') == 'skipped' for r in records))

    def test_existing_partial_configuration_and_manifest_are_preserved(self):
        (self.install / 'ReShade.ini').write_bytes(b'new')
        dest = self.root / 'config'
        dest.mkdir()
        (dest / 'ReShade.ini').write_bytes(b'partial')
        (dest / 'manifest.json').write_text('historical')
        with self.assertRaises(FileExistsError):
            crash_evidence.configurations(self.cfg, dest)
        self.assertEqual((dest / 'ReShade.ini').read_bytes(), b'partial')
        self.assertEqual((dest / 'manifest.json').read_text(), 'historical')


if __name__ == '__main__':
    unittest.main()
