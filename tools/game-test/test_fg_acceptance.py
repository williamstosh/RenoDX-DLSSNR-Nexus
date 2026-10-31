"""Input regression checks for the static NR-Off flicker control."""
import json
import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import fg_acceptance


class ModelInstallationTests(unittest.TestCase):
    def test_model_overrides_require_full_restoration_before_input_access(self):
        script = fg_acceptance.ROOT / 'tools' / 'run-fg-acceptance.ps1'
        for overrides in (['-NrPreset', '2'], ['-NrPasses', '2'],
                          ['-NrPass2Preset', '1', '-RestoreInstallation', '-InstallOnly'],
                          ['-NrHookPoint', '1'], ['-NrPass2Stage', '2'],
                          ['-NrPass3Stage', '0'], ['-NrPass4Stage', '3'],
                          ['-SrOnly'], ['-SrOnly', '-RestoreInstallation', '-InstallOnly'],
                          ['-TelemetrySeconds', '1'],
                          ['-NrHookPoint', '1', '-NrPass2Stage', '2', '-RestoreInstallation', '-InstallOnly']):
            with self.subTest(overrides=overrides):
                result = subprocess.run(['pwsh.exe', '-NoProfile', '-File', str(script),
                    '-CandidateManifest', 'missing-test-manifest', '-GameBinaryDirectory', 'missing-test-game',
                    '-Python', 'missing-test-python', *overrides],
                    capture_output=True, text=True, encoding='utf-8', timeout=15)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Model tests require a protected session', result.stderr)


def setUpModule():
    (fg_acceptance.ROOT / 'tmp').mkdir(exist_ok=True)


class HdrTargetTests(unittest.TestCase):
    def setUp(self):
        self.first = dict(adapterLUID='00000000000140d9', targetId=268, active=True,
            monitorFriendlyDeviceName='LG TV SSCR2', highDynamicRangeUserEnabled=True)
        self.second = self.first | dict(adapterLUID='0000000000012b42', targetId=4352)
        self.target = dict(expected_hdr_monitor='LG TV SSCR2',
            expected_hdr_adapter_luid=self.first['adapterLUID'], expected_hdr_target_id=self.first['targetId'])

    def test_same_name_dual_hdr_outputs_select_only_explicit_identity(self):
        for selected in (self.first, self.second):
            for outputs in ([self.first, self.second], [self.second, self.first]):
                with self.subTest(identity=selected['adapterLUID'], order=outputs[0]['targetId']):
                    config = self.target | dict(expected_hdr_adapter_luid=selected['adapterLUID'].upper(),
                        expected_hdr_target_id=selected['targetId'])
                    self.assertIs(fg_acceptance.select_hdr_target(dict(targets=outputs), config), selected)

    def test_identity_configuration_requires_complete_valid_pair(self):
        for changes in (dict(expected_hdr_adapter_luid=None), dict(expected_hdr_target_id=None),
                        dict(expected_hdr_adapter_luid=''), dict(expected_hdr_adapter_luid='140d9'),
                        dict(expected_hdr_target_id='268'), dict(expected_hdr_target_id=True),
                        dict(expected_hdr_target_id=-1), dict(expected_hdr_target_id=0x100000000)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                fg_acceptance.select_hdr_target(dict(targets=[self.first, self.second]), self.target | changes)

    def test_query_missing_identity_never_falls_back_to_friendly_name(self):
        for field in ('adapterLUID', 'targetId'):
            incomplete = self.first.copy()
            del incomplete[field]
            with self.subTest(field=field), self.assertRaises(RuntimeError):
                fg_acceptance.select_hdr_target(dict(targets=[incomplete, self.second]), self.target)

    def test_mismatched_pair_is_not_matched_by_either_component_alone(self):
        with self.assertRaises(RuntimeError):
            fg_acceptance.select_hdr_target(dict(targets=[self.first, self.second]),
                self.target | dict(expected_hdr_target_id=self.second['targetId']))

    def test_duplicate_configured_identity_is_rejected(self):
        with self.assertRaises(RuntimeError):
            fg_acceptance.select_hdr_target(dict(targets=[self.first, self.first.copy()]), self.target)

    def test_selected_output_still_requires_exact_friendly_name(self):
        with self.assertRaises(RuntimeError):
            fg_acceptance.select_hdr_target(dict(targets=[self.first | dict(monitorFriendlyDeviceName='other'),
                self.second]), self.target)

    def test_selected_output_requires_hdr_even_when_other_output_has_hdr(self):
        with self.assertRaises(RuntimeError):
            fg_acceptance.select_hdr_target(dict(targets=[self.first | dict(highDynamicRangeUserEnabled=False),
                self.second]), self.target)

    def test_inactive_configured_output_is_rejected(self):
        with self.assertRaises(RuntimeError):
            fg_acceptance.select_hdr_target(dict(targets=[self.first | dict(active=False), self.second]), self.target)

    def test_unconfigured_identity_preserves_single_active_output_requirement(self):
        for config in (dict(expected_hdr_monitor='LG TV SSCR2'),
                       dict(expected_hdr_monitor='LG TV SSCR2', expected_hdr_adapter_luid=None, expected_hdr_target_id=None)):
            self.assertIs(fg_acceptance.select_hdr_target(dict(targets=[self.first]), config), self.first)
            with self.assertRaises(RuntimeError):
                fg_acceptance.select_hdr_target(dict(targets=[self.first, self.second]), config)
            with self.assertRaises(RuntimeError):
                fg_acceptance.select_hdr_target(dict(targets=[self.first | dict(highDynamicRangeUserEnabled=False)]), config)


class CaptureTests(unittest.TestCase):
    def test_restore_installation_cli_forwards_existing_wrapper_switch(self):
        with tempfile.TemporaryDirectory(dir=fg_acceptance.ROOT / 'tmp') as directory:
            root = Path(directory)
            target = root / 'target.json'
            target.write_text(json.dumps(dict(expected_hdr_monitor='fixture monitor')), encoding='utf-8')
            manifest, game, tool = root / 'manifest.json', root / 'game', root / 'hdr.exe'
            manifest.write_text('{}', encoding='utf-8')
            game.mkdir()
            tool.touch()
            child = mock.Mock(stdout=[], returncode=0)
            query = mock.Mock(stdout=json.dumps(dict(targets=[dict(active=True,
                monitorFriendlyDeviceName='fixture monitor', highDynamicRangeUserEnabled=True)])))
            arguments = ['fg_acceptance.py', '--candidate-manifest', str(manifest),
                '--game-binary-directory', str(game), '--target', str(target),
                '--output-root', str(root / 'output'), '--hdr-tool', str(tool), '--restore-installation']
            with mock.patch.object(fg_acceptance.sys, 'argv', arguments), \
                    mock.patch.object(fg_acceptance.subprocess, 'run', return_value=query), \
                    mock.patch.object(fg_acceptance.subprocess, 'Popen', return_value=child) as launch:
                self.assertEqual(fg_acceptance.main(), 1)  # No fake routing/acceptance claim.
            command = launch.call_args.args[0]
            self.assertIn('-RestoreInstallation', command)
            self.assertEqual(command[command.index('-TargetPath') + 1], str(target.resolve()))
            self.assertEqual(command[command.index('-OutputRoot') + 1], str((root / 'output').resolve()))
            self.assertEqual(command[command.index('-Python') + 1], str(Path(fg_acceptance.sys.executable).resolve()))

    def test_addon_menu_capture_error_closes_menu_and_releases_hold(self):
        with tempfile.TemporaryDirectory(dir=fg_acceptance.ROOT / 'tmp') as directory:
            run = Path(directory)
            (run / 'capture-ready.json').write_text(json.dumps(dict(pid=123, identity=dict(hwnd=456))))
            with mock.patch.object(fg_acceptance.desktop, 'activate'), \
                    mock.patch.object(fg_acceptance.desktop, 'modules', return_value=[]), \
                    mock.patch.object(fg_acceptance.desktop, 'same_process', return_value=True), \
                    mock.patch.object(fg_acceptance.desktop.u, 'GetForegroundWindow', return_value=456), \
                    mock.patch.object(fg_acceptance.desktop, 'capture',
                        side_effect=[None, None, None, None, RuntimeError('menu capture failed')]), \
                    mock.patch.object(fg_acceptance.desktop, 'key') as key, \
                    mock.patch.object(fg_acceptance.time, 'sleep'):
                with self.assertRaisesRegex(RuntimeError, 'menu capture failed'):
                    fg_acceptance.capture(run, check_addon_menu=True)
            self.assertEqual([call.args[1] for call in key.call_args_list], ['F6', 'F6', 'D', 'HOME', 'HOME'])
            self.assertTrue(json.loads((run / 'capture-complete.json').read_text())['completed'])

    def test_nr_off_control_never_toggles_nr_or_moves_camera(self):
        with tempfile.TemporaryDirectory(dir=fg_acceptance.ROOT / 'tmp') as directory:
            run = Path(directory)
            (run / 'capture-ready.json').write_text(json.dumps(dict(pid=123, identity=dict(hwnd=456))))
            with mock.patch.object(fg_acceptance.desktop, 'activate'), \
                    mock.patch.object(fg_acceptance.desktop, 'modules', return_value=[]), \
                    mock.patch.object(fg_acceptance.desktop, 'same_process', return_value=True), \
                    mock.patch.object(fg_acceptance.desktop.u, 'GetForegroundWindow', return_value=456), \
                    mock.patch.object(fg_acceptance.desktop, 'capture') as capture, \
                    mock.patch.object(fg_acceptance.desktop, 'key') as key, \
                    mock.patch.object(fg_acceptance.time, 'sleep'):
                fg_acceptance.capture(run, nr_off_control=True)
            key.assert_not_called()
            capture.assert_called_once()
            self.assertEqual(json.loads((run / 'capture-complete.json').read_text()),
                dict(pid=123, completed=True))

    def test_failed_foreground_check_releases_owned_worker_hold(self):
        with tempfile.TemporaryDirectory(dir=fg_acceptance.ROOT / 'tmp') as directory:
            run = Path(directory)
            (run / 'capture-ready.json').write_text(json.dumps(dict(pid=123, identity=dict(hwnd=456))))
            with mock.patch.object(fg_acceptance.desktop, 'activate'), \
                    mock.patch.object(fg_acceptance.desktop, 'modules', return_value=[]), \
                    mock.patch.object(fg_acceptance.desktop, 'same_process', return_value=False), \
                    mock.patch.object(fg_acceptance.desktop, 'key') as key, \
                    mock.patch.object(fg_acceptance.time, 'sleep'):
                with self.assertRaisesRegex(RuntimeError, 'identity or foreground changed'):
                    fg_acceptance.capture(run, nr_off_control=True)
            key.assert_not_called()
            self.assertTrue(json.loads((run / 'capture-complete.json').read_text())['completed'])


if __name__ == '__main__':
    unittest.main()
