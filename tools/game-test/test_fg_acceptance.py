"""Input regression checks for the static NR-Off flicker control."""
import json
import hashlib
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import fg_acceptance


class ModelInstallationTests(unittest.TestCase):
    def test_control_mask_overrides_reject_before_installation(self):
        script = fg_acceptance.ROOT / 'tools' / 'run-fg-acceptance.ps1'
        cases = []
        for option, maximum in (('-ControlMaskAutoMask', 1), ('-ControlMaskBlendMode', 3)):
            for value in (-1, 0, maximum):
                cases.append(([option, str(value)], 'require an explicit protected -ControlMaskCase'))
            for value in (-2, maximum + 1, 'invalid'):
                cases.append(([option, str(value), '-ControlMaskCase', 'T1'], option[1:]))
        protected = ['-ControlMaskCase', 'BRect', '-ControlMaskAutoMask', '1',
                     '-ControlMaskBlendMode', '3']
        for options in ([], ['-RestoreInstallation'], ['-RestoreInstallation', '-SrOnly'],
                        ['-RestoreInstallation', '-SrOnly', '-NrRuntimePath', 'missing-runtime'],
                        ['-RestoreInstallation', '-SrOnly', '-NrRuntimePath', 'missing-runtime',
                         '-NrPreset', '2', '-InstallOnly']):
            cases.append((protected + options, 'ControlMask requires -RestoreInstallation'))
        cases.append(([], 'Provide -Python containing Pillow'))
        cases.append((protected + ['-RestoreInstallation', '-SrOnly', '-NrRuntimePath',
                                  'missing-runtime', '-NrPreset', '2'],
                      'Provide -Python containing Pillow'))
        with tempfile.TemporaryDirectory(dir=fg_acceptance.ROOT / 'tmp') as directory:
            root = Path(directory)
            output = root / 'output'
            base = ['pwsh.exe', '-NoProfile', '-File', str(script),
                    '-CandidateManifest', str(root / 'missing-manifest'),
                    '-GameBinaryDirectory', str(root / 'missing-game'),
                    '-Python', str(root / 'missing-python'), '-OutputRoot', str(output)]
            for overrides, message in cases:
                with self.subTest(overrides=overrides):
                    result = subprocess.run(base + overrides, capture_output=True, text=True,
                                            encoding='utf-8', timeout=15)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(message, result.stderr)
                    self.assertEqual(list(root.iterdir()), [])

    def test_control_mask_override_mapping_and_receipt_in_actual_powershell(self):
        script = fg_acceptance.ROOT / 'tools' / 'run-fg-acceptance.ps1'
        # Evaluate only the actual mapping and receipt AST nodes, never installation code.
        command = r'''
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($args[0], [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Wrapper parse failed' }
$function = $ast.Find({param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq 'Get-ControlMaskSettings'
}, $true)
. ([scriptblock]::Create($function.Extent.Text))
$mapping = $ast.Find({param($node)
    $node -is [System.Management.Automation.Language.IfStatementAst] -and
    $node.Extent.Text.Contains('foreach ($taskSetting in (Get-ControlMaskSettings $ControlMaskCase)')
}, $true)
$receipt = $ast.Find({param($node)
    $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
    $node.Left.Extent.Text -eq '$taskReceipt'
}, $true)
$defaults = @{}
foreach ($parameter in $ast.ParamBlock.Parameters) {
    if ($parameter.Name.VariablePath.UserPath -in @('ControlMaskAutoMask','ControlMaskBlendMode')) {
        $defaults[$parameter.Name.VariablePath.UserPath] = $parameter.DefaultValue.SafeGetValue()
    }
}
$rows = @(foreach ($ControlMaskCase in @('T0','T1','T2','T3','T4','T5','T6','GRect','BRect')) {
    foreach ($ControlMaskAutoMask in -1,0,1) {
        foreach ($ControlMaskBlendMode in -1,0,1,2,3) {
            $taskModelSettings = @{}
            . ([scriptblock]::Create($mapping.Extent.Text))
            . ([scriptblock]::Create($receipt.Extent.Text))
            @{case=$ControlMaskCase; auto=$ControlMaskAutoMask; blend=$ControlMaskBlendMode
              settings=$taskModelSettings; receipt_auto=$taskReceipt.requested_control_mask_auto_mask
              receipt_blend=$taskReceipt.requested_control_mask_blend_mode}
        }
    }
})
@{defaults=$defaults; rows=$rows} | ConvertTo-Json -Depth 6 -Compress
'''
        with tempfile.TemporaryDirectory(dir=fg_acceptance.ROOT / 'tmp') as directory:
            driver = Path(directory) / 'mapping.ps1'
            driver.write_text(command, encoding='utf-8')
            result = subprocess.run(['pwsh.exe', '-NoProfile', '-File', str(driver), str(script)],
                                    capture_output=True, text=True, encoding='utf-8', timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload['defaults'], dict(ControlMaskAutoMask=-1, ControlMaskBlendMode=-1))
        rows = payload['rows']
        self.assertEqual(len(rows), 135)
        defaults = {row['case']: row['settings'] for row in rows
                    if row['auto'] == -1 and row['blend'] == -1}
        for row in rows:
            with self.subTest(case=row['case'], auto=row['auto'], blend=row['blend']):
                expected = dict(defaults[row['case']])
                self.assertEqual(expected['NRAutoMask'], 0)
                self.assertEqual(expected['NRControlMaskBlendMode'], 0)
                if row['auto'] >= 0:
                    expected['NRAutoMask'] = row['auto']
                if row['blend'] >= 0:
                    expected['NRControlMaskBlendMode'] = row['blend']
                self.assertEqual(row['settings'], expected)
                self.assertEqual(row['receipt_auto'], row['auto'])
                self.assertEqual(row['receipt_blend'], row['blend'])
        self.assertIn('-Parameters $PSBoundParameters', script.read_text(encoding='utf-8'))

    def test_core_context_requires_protected_control_mask_case(self):
        script = fg_acceptance.ROOT / 'tools' / 'run-fg-acceptance.ps1'
        base = ['pwsh.exe', '-NoProfile', '-File', str(script),
                '-CandidateManifest', 'missing-test-manifest', '-GameBinaryDirectory', 'missing-test-game',
                '-Python', 'missing-test-python', '-ControlMaskUseCoreContext']
        for overrides, message in (([], 'requires an explicit protected -ControlMaskCase'),
                (['-ControlMaskCase', 'T0'], 'ControlMask requires -RestoreInstallation'),
                (['-ControlMaskCase', 'T0', '-RestoreInstallation', '-SrOnly',
                  '-NrRuntimePath', 'runtime', '-NrPreset', '1', '-NrPasses', '2'],
                 'ControlMask requires -RestoreInstallation')):
            with self.subTest(overrides=overrides):
                result = subprocess.run(base + overrides, capture_output=True, text=True,
                                        encoding='utf-8', timeout=15)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_core_context_static_request_and_restart_contract(self):
        script = (fg_acceptance.ROOT / 'tools' / 'run-fg-acceptance.ps1').read_text(encoding='utf-8')
        self.assertIn('if ($ControlMaskUseCoreContext) { $taskModelSettings.NRControlMaskUseCoreContext = 1 }', script)
        self.assertIn('requested_control_mask_use_core_context=[bool]$ControlMaskUseCoreContext', script)
        self.assertIn('-Parameters $PSBoundParameters', script)
        self.assertEqual(script.count('$taskModelSettings.NRControlMaskUseCoreContext'), 1)

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

    def test_block_optional_hudless_rejects_upscaled_hook(self):
        arguments = ['fg_acceptance.py', '--candidate-manifest', 'candidate.json',
            '--game-binary-directory', 'game', '--block-optional-hudless', '--nr-hook-point', '0']
        with mock.patch.object(fg_acceptance.sys, 'argv', arguments), self.assertRaises(SystemExit) as error:
            fg_acceptance.main()
        self.assertEqual(error.exception.code, 2)

    def test_xefg_detour_probe_requires_nr_off_control(self):
        arguments = ['fg_acceptance.py', '--candidate-manifest', 'candidate.json',
            '--game-binary-directory', 'game', '--disable-xefg-sdk-detours']
        with mock.patch.object(fg_acceptance.sys, 'argv', arguments), self.assertRaises(SystemExit) as error:
            fg_acceptance.main()
        self.assertEqual(error.exception.code, 2)


class FgIntervalTests(unittest.TestCase):
    def test_interval_preserves_actual_service_and_evaluate_counters(self):
        line = ('DLSS5 FG interval seconds=5.001 application_presents=120 nr_submitted=120 '
            'status_samples=120 displayed_frames=480 ui_mode=0 ui_composition=0 nr_colors=1 '
            'sdk_fg_enabled=1 fresh_present_id=1 nr_service[frames=120 process=120 evals=240 '
            'hudless=0 backbuffer=120 two_color=0 composed=0]')
        parsed = fg_acceptance.parse_fg_interval(line)
        self.assertEqual(parsed['nr_service'], dict(frames=120, process=120, evals=240,
            hudless=0, backbuffer=120, two_color=0, composed=0))
        self.assertEqual(parsed['nr_service']['evals'], 240)


def setUpModule():
    (fg_acceptance.ROOT / 'tmp').mkdir(exist_ok=True)


class TelemetryTests(unittest.TestCase):
    def test_window_evaluates_do_not_read_cumulative_stage_counter(self):
        for evaluates in (0, 25):
            with self.subTest(evaluates=evaluates):
                line = (f'23:07:00:785 [40308] | INFO | NR Nexus: telemetry t=143.24s '
                        f'win=1.01s presents=116 ticks=116 fps=115.13 '
                        f'nr[evals={evaluates} cpu_us_avg=0.00] '
                        f'nr_stage[Upscaled range=1+1 groups=475 evals=475 dimensions=3840x2160]')
                self.assertEqual(fg_acceptance.parse_nr_telemetry(line),
                    dict(window_seconds=1.01, presents=116, fps=115.13, evaluates=evaluates))


class SdkIsolationTests(unittest.TestCase):
    def verify_fixture(self, evaluates=0, extra_log='', config_value=0):
        with tempfile.TemporaryDirectory(dir=fg_acceptance.ROOT / 'tmp') as directory:
            root = Path(directory)
            run, acceptance = root / 'run', root / 'acceptance'
            (run / 'final-logs').mkdir(parents=True)
            acceptance.mkdir()
            config = root / 'ReShade.ini'
            config.write_text('[RenoDX.DLSS5]\nNeuralUplift=1\n', encoding='utf-8')
            protected = root / 'protected-ReShade.ini'
            protected.write_text(f'[RenoDX.DLSS5]\nNeuralUplift=0\nNRXeFGSdkDetours={config_value}\n',
                encoding='utf-8')
            original_hash = hashlib.sha256(config.read_bytes()).hexdigest()
            fg_acceptance.dump(acceptance / 'receipt.json', dict(restored=True,
                original_files=[dict(path=str(config), sha256=original_hash)],
                candidate_sha256='candidate', disable_xefg_sdk_detours=True, start_nr_disabled=True))
            fg_acceptance.dump(run / 'protected-files.json', [dict(path=str(config),
                backup_path=str(protected), sha256=hashlib.sha256(protected.read_bytes()).hexdigest())])
            fg_acceptance.dump(run / 'result.json', dict(guard='verified_original_hashes_and_released',
                safe_exit='menu_exit_and_detach_confirmed', result='passed'))
            fg_acceptance.dump(run / 'loaded-modules.json',
                dict(matching_paths=['candidate'], actual_sha256='candidate'))
            fg_acceptance.dump(run / 'capture-ready.json', dict(time='2026-11-03T01:00:00+00:00'))
            fg_acceptance.dump(run / 'scene-health.json', [dict(time='2026-11-03T01:01:00+00:00')])
            fg_acceptance.dump(run / 'nr-toggle-captures.json', [])
            local_time = fg_acceptance.datetime.fromisoformat('2026-11-03T01:00:05+00:00').astimezone()
            (run / 'final-logs/ReShade.log').write_text(
                local_time.strftime('%H:%M:%S') + f':001 | NR Nexus: telemetry t=5.0s win=1.0s '
                f'presents=30 ticks=30 fps=30.00 nr[evals={evaluates} cpu_us_avg=0.00]\n' + extra_log,
                encoding='utf-8')
            return fg_acceptance.verify(run, acceptance, False, 2, nr_off_control=True)

    def test_disabled_sdk_control_does_not_claim_fg_routing_or_visual_acceptance(self):
        result = self.verify_fixture()
        self.assertTrue(result['diagnostic_completed'])
        self.assertIsNone(result['routing_passed'])

    def test_disabled_sdk_control_rejects_evaluates_and_unexpected_hook_telemetry(self):
        for arguments in (dict(evaluates=1), dict(extra_log='DLSS5 FG interval\n'), dict(config_value=1)):
            with self.subTest(arguments=arguments), self.assertRaises(RuntimeError):
                self.verify_fixture(**arguments)


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
    def write_capture_ready(self, run):
        window = dict(hwnd=456, pid=123, created=1, path='game.exe', title='The Witcher 3')
        (run / 'capture-ready.json').write_text(json.dumps(dict(
            pid=window['pid'], identity=window, automatic_foreground=True)))
        (run / 'owned-process.json').write_text(json.dumps(window))
        return window

    def test_composition_override_requires_restore_before_launch(self):
        arguments = ['fg_acceptance.py', '--candidate-manifest', 'absent.json',
            '--game-binary-directory', 'absent-game', '--xefg-ui-composition', '0']
        with mock.patch.object(fg_acceptance.sys, 'argv', arguments), \
                mock.patch.object(fg_acceptance.subprocess, 'Popen') as launch, \
                mock.patch('sys.stderr'):
            with self.assertRaises(SystemExit):
                fg_acceptance.main()
        launch.assert_not_called()

    def test_restore_installation_cli_forwards_existing_wrapper_switch(self):
        with tempfile.TemporaryDirectory(dir=fg_acceptance.ROOT / 'tmp') as directory:
            root = Path(directory)
            target = root / 'target.json'
            target.write_text(json.dumps(dict(expected_hdr_monitor='fixture monitor')), encoding='utf-8')
            manifest, game, tool = root / 'manifest.json', root / 'game', root / 'hdr.exe'
            manifest.write_text('{}', encoding='utf-8')
            game.mkdir()
            tool.touch()
            run = root / 'run'
            run.mkdir()
            child = mock.Mock(stdout=[f'CAPTURE_READY={run}\n'], returncode=0)
            query = mock.Mock(stdout=json.dumps(dict(targets=[dict(active=True,
                monitorFriendlyDeviceName='fixture monitor', highDynamicRangeUserEnabled=True)])))
            arguments = ['fg_acceptance.py', '--candidate-manifest', str(manifest),
                '--game-binary-directory', str(game), '--target', str(target),
                '--output-root', str(root / 'output'), '--hdr-tool', str(tool), '--restore-installation',
                '--nr-hook-point', '0', '--nr-passes', '2', '--gpu-timers', '--telemetry-seconds', '2',
                '--nr-off-control', '--disable-xefg-sdk-detours', '--xefg-ui-composition', '0']
            print_output = mock.patch('builtins.print')
            printed = print_output.start()
            self.addCleanup(print_output.stop)
            with mock.patch.object(fg_acceptance.sys, 'argv', arguments), \
                    mock.patch.object(fg_acceptance.subprocess, 'run', return_value=query), \
                    mock.patch.object(fg_acceptance.subprocess, 'Popen', return_value=child) as launch, \
                    mock.patch.object(fg_acceptance, 'capture') as capture:
                self.assertEqual(fg_acceptance.main(), 1)  # No fake routing/acceptance claim.
            capture.assert_called_once_with(run, True, False)
            printed.assert_any_call('CAPTURE_STARTED=' + str(run), flush=True)
            printed.assert_any_call('CAPTURE_FINISHED; valid completion recorded; protected worker resumes', flush=True)
            command = launch.call_args.args[0]
            self.assertIn('-RestoreInstallation', command)
            self.assertEqual(command[command.index('-NrHookPoint') + 1], '0')
            self.assertEqual(command[command.index('-NrPasses') + 1], '2')
            self.assertEqual(command[command.index('-TelemetrySeconds') + 1], '2')
            self.assertIn('-GpuTimers', command)
            self.assertIn('-StartNrDisabled', command)
            self.assertIn('-DisableXeFGSdkDetours', command)
            self.assertEqual(command[command.index('-XeFGUiComposition') + 1], '0')
            self.assertEqual(command[command.index('-TargetPath') + 1], str(target.resolve()))
            self.assertEqual(command[command.index('-OutputRoot') + 1], str((root / 'output').resolve()))
            self.assertEqual(command[command.index('-Python') + 1], str(Path(fg_acceptance.sys.executable).resolve()))

    def test_addon_menu_capture_error_closes_menu_and_releases_hold(self):
        with tempfile.TemporaryDirectory(dir=fg_acceptance.ROOT / 'tmp') as directory:
            run = Path(directory)
            self.write_capture_ready(run)
            with mock.patch.object(fg_acceptance.desktop, 'ensure_foreground', return_value={}), \
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
            self.assertFalse((run / 'capture-complete.json').exists())
            failure = json.loads((run / 'capture-failed.json').read_text())
            self.assertFalse(failure['no_game_input_sent'])
            self.assertTrue(failure['game_input_may_have_been_sent'])

    def test_nr_off_control_never_toggles_nr_or_moves_camera(self):
        with tempfile.TemporaryDirectory(dir=fg_acceptance.ROOT / 'tmp') as directory:
            run = Path(directory)
            window = self.write_capture_ready(run)
            receipt = dict(hwnd=456, pid=123, created=1, elapsed_ms=12)
            with mock.patch.object(fg_acceptance.desktop, 'ensure_foreground', return_value=receipt) as activate, \
                    mock.patch.object(fg_acceptance.desktop, 'modules', return_value=[]), \
                    mock.patch.object(fg_acceptance.desktop, 'same_process', return_value=True), \
                    mock.patch.object(fg_acceptance.desktop.u, 'GetForegroundWindow', return_value=456), \
                    mock.patch.object(fg_acceptance.desktop, 'capture') as capture, \
                    mock.patch.object(fg_acceptance.desktop, 'key') as key, \
                    mock.patch.object(fg_acceptance.time, 'sleep'):
                fg_acceptance.capture(run, nr_off_control=True)
            key.assert_not_called()
            capture.assert_called_once()
            self.assertTrue(all(call == mock.call(window) for call in activate.call_args_list))
            self.assertGreaterEqual(activate.call_count, 1)
            events = json.loads((run / 'nr-toggle-captures.json').read_text())
            self.assertEqual(events[0]['action'], 'automatic_foreground_confirmed')
            for field, value in receipt.items():
                self.assertEqual(events[0][field], value)
            self.assertFalse(list(run.glob('computer-use-*.json')))
            self.assertEqual(json.loads((run / 'capture-complete.json').read_text()),
                dict(pid=123, completed=True))

    def test_failed_foreground_check_never_releases_capture_hold(self):
        with tempfile.TemporaryDirectory(dir=fg_acceptance.ROOT / 'tmp') as directory:
            run = Path(directory)
            self.write_capture_ready(run)
            with mock.patch.object(fg_acceptance.desktop, 'ensure_foreground') as activate, \
                    mock.patch.object(fg_acceptance.desktop, 'modules', return_value=[]), \
                    mock.patch.object(fg_acceptance.desktop, 'same_process', return_value=False), \
                    mock.patch.object(fg_acceptance.desktop, 'key') as key, \
                    mock.patch.object(fg_acceptance.time, 'sleep'):
                with self.assertRaisesRegex(RuntimeError, 'identity changed'):
                    fg_acceptance.capture(run, nr_off_control=True)
            key.assert_not_called()
            activate.assert_not_called()
            self.assertFalse((run / 'capture-complete.json').exists())

    def test_automatic_activation_failure_sends_no_input_and_keeps_worker_gated(self):
        with tempfile.TemporaryDirectory(dir=fg_acceptance.ROOT / 'tmp') as directory:
            run = Path(directory)
            window = self.write_capture_ready(run)
            with mock.patch.object(fg_acceptance.desktop, 'same_process', return_value=True), \
                    mock.patch.object(fg_acceptance.desktop, 'ensure_foreground',
                                      side_effect=RuntimeError('automatic activation failed')) as activate, \
                    mock.patch.object(fg_acceptance.desktop, 'capture') as capture, \
                    mock.patch.object(fg_acceptance.desktop, 'key') as key, \
                    mock.patch('builtins.print'):
                with self.assertRaisesRegex(RuntimeError, 'automatic activation failed'):
                    fg_acceptance.capture(run, nr_off_control=True)
            capture.assert_not_called()
            activate.assert_called_once_with(window)
            key.assert_not_called()
            self.assertFalse((run / 'capture-complete.json').exists())
            failure = json.loads((run / 'capture-failed.json').read_text())
            self.assertTrue(failure['no_game_input_sent'])
            self.assertFalse(failure['game_input_may_have_been_sent'])

    def test_capture_rejects_mismatched_owned_identity_before_activation(self):
        for field, value in (('hwnd', 457), ('pid', 124), ('created', 2), ('path', 'other.exe'),
                             ('ready_pid', 124), ('automatic_foreground', False), ('missing_owned', None)):
            with self.subTest(field=field), tempfile.TemporaryDirectory(dir=fg_acceptance.ROOT / 'tmp') as directory:
                run = Path(directory)
                window = self.write_capture_ready(run)
                if field == 'missing_owned':
                    (run / 'owned-process.json').unlink()
                elif field in ('ready_pid', 'automatic_foreground'):
                    ready = json.loads((run / 'capture-ready.json').read_text())
                    ready['pid' if field == 'ready_pid' else field] = value
                    fg_acceptance.dump(run / 'capture-ready.json', ready)
                else:
                    fg_acceptance.dump(run / 'owned-process.json', window | {field: value})
                with mock.patch.object(fg_acceptance.desktop, 'ensure_foreground') as activate, \
                        mock.patch.object(fg_acceptance.desktop, 'capture') as capture, \
                        mock.patch.object(fg_acceptance.desktop, 'key') as key:
                    with self.assertRaises((RuntimeError, FileNotFoundError)):
                        fg_acceptance.capture(run, nr_off_control=True)
                activate.assert_not_called()
                capture.assert_not_called()
                key.assert_not_called()
                self.assertFalse((run / 'capture-complete.json').exists())
                self.assertTrue(json.loads((run / 'capture-failed.json').read_text())['no_game_input_sent'])


if __name__ == '__main__':
    unittest.main()
