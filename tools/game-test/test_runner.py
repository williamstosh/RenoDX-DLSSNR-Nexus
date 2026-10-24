"""Regression checks for exit races and focus validation without launching a game."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
from PIL import Image, ImageDraw
import runner


class ExitTests(unittest.TestCase):
    def setUp(self):
        (runner.ROOT / 'tmp').mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=runner.ROOT / 'tmp')
        self.addCleanup(self.temp.cleanup)
        self.out = Path(self.temp.name)
        self.session = runner.Session(dict(executable=str(self.out / 'game.exe'), logs=[]), self.out)
        self.session.window = dict(pid=123, created=1, hwnd=456, path='game.exe')

    def test_shutdown_scene_uses_exit_focus_validation_without_full_ocr_gate(self):
        self.session.ui_state = 'scene'
        with mock.patch.object(runner.desktop, 'same_process', side_effect=[True, False]), \
                mock.patch.object(self.session, 'key') as key, \
                mock.patch.object(self.session, 'request_exit') as request_exit, \
                mock.patch.object(self.session, 'wait_text', side_effect=AssertionError('redundant OCR gate')):
            self.session.shutdown()
        key.assert_called_once_with('ESC')
        request_exit.assert_called_once()

    def test_baseline_confirms_addon_absence_in_disk_and_modules(self):
        self.session.cfg.update(required_module='renodx-dlss5.addon64', addon_absent_control=True)
        with mock.patch.object(runner.desktop, 'modules', return_value=['C:/game/dxgi.dll']):
            self.session.check_mod()
        self.assertEqual(self.session.status['mod_load'], 'baseline_addon_absence_confirmed')

    def test_baseline_rejects_addon_loaded_from_another_directory(self):
        self.session.cfg.update(required_module='renodx-dlss5.addon64', addon_absent_control=True)
        with mock.patch.object(runner.desktop, 'modules', return_value=['C:/other/RENODX-DLSS5.ADDON64']):
            with self.assertRaisesRegex(RuntimeError, 'baseline_addon_still_present'):
                self.session.check_mod()

    def test_smoke_pause_uses_observed_focus_without_requiring_full_ocr_sentence(self):
        with mock.patch.object(self.session, 'main_menu'), \
                mock.patch.object(self.session, 'check_mod'), \
                mock.patch.object(self.session, 'select_menu') as select_menu, \
                mock.patch.object(self.session, 'key') as key, \
                mock.patch.object(self.session, 'load_save'), \
                mock.patch.object(self.session, 'wait_scene'), \
                mock.patch.object(self.session, 'evidence'), \
                mock.patch.object(self.session, 'capture_hold'), \
                mock.patch.object(self.session, 'scene_health'), \
                mock.patch.object(self.session, 'wait_text', side_effect=AssertionError('redundant ordered OCR gate')):
            self.session.smoke()
        self.assertEqual(select_menu.call_args_list,
            [mock.call('读取游戏', runner.MAIN_MENU), mock.call('继续', runner.PAUSE_MENU)])
        self.assertEqual(key.call_args_list, [mock.call('ENTER'), mock.call('ESC'), mock.call('ENTER')])
        self.assertEqual(self.session.status['functionality'],
            'pause_and_resume_input_response_confirmed_mod_features_not_tested')

    def test_visible_unique_save_can_gain_focus_after_keyboard_did_not_move(self):
        self.session.cfg.update(save_modified='2026-10-14T01:08:30', save='manual.sav', save_sha256='hash')
        wrong = dict(timestamp='2026-10-14T01:35:49', y=100, selected=True, text='autosave')
        target = dict(timestamp='2026-10-14T01:08:30', y=200, selected=False, text='unique manual timestamp')
        selected = dict(target, selected=True)
        with mock.patch.object(self.session, 'observe', return_value={'lines': []}), \
                mock.patch.object(runner, 'save_rows', side_effect=[[wrong, target], [wrong, target], [selected]]), \
                mock.patch.object(self.session, 'key') as key, \
                mock.patch.object(self.session, 'click_text') as click, \
                mock.patch.object(self.session, 'evidence'), \
                mock.patch.object(runner.time, 'sleep'):
            self.session.load_save()
        self.assertEqual(key.call_args_list, [mock.call('DOWN'), mock.call('ENTER')])
        click.assert_called_once_with({'lines': []}, 'uniquemanualtimestamp')
        receipt = json.loads((self.out / 'save-selection.json').read_text(encoding='utf-8'))
        self.assertEqual(receipt['timestamp'], '2026-10-14T01:08:30')

    def test_calibration_load_and_pause_keep_exit_route_without_claiming_scene_acceptance(self):
        commands = iter([{'action': 'load_menu'}, {'action': 'load_save'},
                         {'action': 'pause_menu'}, {'action': 'menu_exit'}])

        def next_command(*_args):
            if not (self.out / 'command.json').exists():
                runner.dump(self.out / 'command.json', next(commands))

        def exit_with_pause_state():
            self.assertEqual(self.session.ui_state, 'pause_menu')

        with mock.patch.object(self.session, 'main_menu', side_effect=next_command), \
                mock.patch.object(self.session, 'select_menu'), \
                mock.patch.object(self.session, 'key'), \
                mock.patch.object(self.session, 'check_mod') as check_mod, \
                mock.patch.object(self.session, 'load_save') as load_save, \
                mock.patch.object(self.session, 'wait_text') as wait_text, \
                mock.patch.object(self.session, 'observe', side_effect=next_command), \
                mock.patch.object(self.session, 'evidence'), \
                mock.patch.object(self.session, 'request_exit', side_effect=exit_with_pause_state), \
                mock.patch.object(runner.time, 'sleep'):
            self.session.calibrate()
        check_mod.assert_called_once()
        load_save.assert_called_once()
        wait_text.assert_called_once()
        self.assertEqual(self.session.status['save_load'], 'not_tested')

    def test_calibration_rejects_pause_before_requesting_scene_load(self):
        runner.dump(self.out / 'command.json', {'action': 'pause_menu'})
        self.session.ui_state = 'main_menu'
        with mock.patch.object(self.session, 'main_menu'), \
                mock.patch.object(self.session, 'key') as key:
            with self.assertRaisesRegex(ValueError, 'requires a requested scene load'):
                self.session.calibrate()
        key.assert_not_called()

    def test_capture_hold_argument_range_and_smoke_only(self):
        for value in ('-1', '1', '9', '121', 'not-an-integer'):
            with self.subTest(value=value), self.assertRaises(runner.argparse.ArgumentTypeError):
                runner.parse_capture_hold_seconds(value)
        self.assertEqual(runner.parse_capture_hold_seconds('0'), 0)
        self.assertEqual(runner.parse_capture_hold_seconds('10'), 10)
        self.assertEqual(runner.parse_capture_hold_seconds('120'), 120)
        with self.assertRaisesRegex(ValueError, 'only supported for the smoke'):
            runner.validate_capture_hold_scenario('menu', 10)

    def test_capture_hold_requires_strict_matching_pid_completion_signal(self):
        self.session.cfg.update(scenario='smoke', capture_hold_seconds=10)
        signal = self.out / 'capture-complete.json'
        for contents in ('[]', '{"pid":123,"pid":123,"completed":true}',
                         '{"pid":123,"completed":1}', '{"pid":123,"completed":NaN}'):
            signal.write_text(contents, encoding='utf-8')
            self.assertFalse(runner.capture_complete(self.out, 123), contents)
        runner.dump(signal, dict(pid=999, completed=True))

        def write_matching_signal(_seconds):
            runner.dump(signal, dict(pid=123, completed=True))

        with mock.patch.object(runner.desktop, 'same_process', return_value=True) as same_process, \
                mock.patch.object(runner.time, 'monotonic', side_effect=[0, 0]), \
                mock.patch.object(runner.time, 'sleep', side_effect=write_matching_signal) as sleep:
            self.session.capture_hold()
        self.assertEqual(same_process.call_count, 2)
        sleep.assert_called_once_with(1)
        ready = json.loads((self.out / 'capture-ready.json').read_text(encoding='utf-8'))
        self.assertEqual(ready['pid'], 123)
        self.assertEqual(ready['identity'], self.session.window)
        self.assertTrue(ready['time'])
        self.assertEqual(self.session.status['capture_hold'], 'completed_by_signal')

    def test_capture_hold_process_exit_fails(self):
        self.session.cfg.update(scenario='smoke', capture_hold_seconds=10)
        with mock.patch.object(runner.desktop, 'same_process', return_value=False), \
                mock.patch.object(runner.time, 'sleep') as sleep:
            with self.assertRaisesRegex(RuntimeError, 'capture_hold_process_exited'):
                self.session.capture_hold()
        sleep.assert_not_called()
        self.assertEqual(self.session.status['capture_hold'], 'process_exited')
        events = [json.loads(line) for line in (self.out / 'events.jsonl').read_text(encoding='utf-8').splitlines()]
        self.assertIn('capture_hold_process_exit', [event['kind'] for event in events])

    def test_capture_hold_expiry_is_recorded_without_failing_smoke(self):
        self.session.cfg.update(scenario='smoke', capture_hold_seconds=10)
        with mock.patch.object(runner.desktop, 'same_process', return_value=True), \
                mock.patch.object(runner.time, 'monotonic', side_effect=[0, 0, 10]), \
                mock.patch.object(runner.time, 'sleep'):
            self.session.capture_hold()
        self.assertEqual(self.session.status['capture_hold'], 'timeout_not_mod_acceptance')
        events = [json.loads(line) for line in (self.out / 'events.jsonl').read_text(encoding='utf-8').splitlines()]
        self.assertEqual(events[-1]['kind'], 'capture_hold_timeout')
        self.assertEqual(events[-1]['result'], 'timeout_not_mod_acceptance')

    def test_requested_exit_waits_without_capture_or_close(self):
        self.session.exit_requested = True
        with mock.patch.object(runner.desktop, 'same_process', side_effect=[True, False, False]), \
                mock.patch.object(runner.desktop, 'capture') as capture, \
                mock.patch.object(runner.desktop, 'close') as close, \
                mock.patch.object(runner.time, 'sleep'):
            self.session.shutdown()
        capture.assert_not_called()
        close.assert_not_called()
        self.assertEqual(self.session.status['safe_exit'], 'menu_exit_pending_log_and_crash_check')

    def test_unrequested_exit_is_not_success(self):
        with mock.patch.object(runner.desktop, 'same_process', return_value=False):
            with self.assertRaisesRegex(RuntimeError, 'without a recorded'):
                self.session.shutdown()
        self.assertEqual(self.session.status['safe_exit'], 'unexpected_exit')

    def test_exit_action_does_not_observe_destroyed_window(self):
        with mock.patch.object(self.session, 'select_menu'), mock.patch.object(self.session, 'key') as key, \
                mock.patch.object(self.session, 'wait_exit') as wait, mock.patch.object(self.session, 'observe') as observe:
            self.session.request_exit()
        key.assert_called_once_with('ENTER')
        wait.assert_called_once()
        observe.assert_not_called()

    def test_paused_exit_requires_observed_confirmation_before_confirm_key(self):
        self.session.ui_state = 'pause_menu'
        with mock.patch.object(self.session, 'select_menu'), mock.patch.object(self.session, 'key') as key, \
                mock.patch.object(self.session, 'wait_text', side_effect=TimeoutError('confirmation missing')), \
                mock.patch.object(self.session, 'wait_exit') as wait:
            with self.assertRaises(TimeoutError):
                self.session.request_exit()
        key.assert_called_once_with('ENTER')
        wait.assert_not_called()
        self.assertFalse(self.session.exit_requested)

    def test_pause_exit_confirmation_uses_displayed_key_then_only_waits(self):
        self.session.ui_state = 'pause_menu'
        with mock.patch.object(self.session, 'select_menu'), mock.patch.object(self.session, 'key') as key, \
                mock.patch.object(self.session, 'wait_text'), mock.patch.object(self.session, 'wait_exit'):
            self.session.request_exit()
        self.assertEqual(key.call_args_list, [mock.call('ENTER'), mock.call('E')])

    def test_save_identity_and_selection_use_timestamp_and_two_frame_edges(self):
        image = Image.new('RGB', (1920, 1080), 'black')
        ImageDraw.Draw(image).rectangle((560, 230, 1610, 295), outline=(110,110,110), width=2)
        path = self.out / 'saves.png'
        image.save(path)
        obs = dict(scale=1, lines=[dict(text=text, words=[dict(x=570,y=y,width=400,height=23)]) for text,y in [
            ('凯尔莫罕 - 2026年10月14日 1：35：49', 189), ('凯尔莫罕 - 2026年10月14日 1：08：30', 252)]])
        rows = runner.save_rows(path,obs)
        self.assertEqual([r['timestamp'] for r in rows if r['selected']], ['2026-10-14T01:08:30'])

    def test_focus_rectangle_requires_both_sides_not_neighbor(self):
        image = Image.new('RGB', (1920, 1080), 'black')
        draw = ImageDraw.Draw(image)
        draw.rectangle((160, 677, 500, 738), outline='white', width=2)
        path = self.out / 'frame.png'
        image.save(path)
        obs = dict(scale=1, lines=[dict(text=label, words=[dict(x=285,y=375+i*46,width=95,height=23)])
                                  for i, label in enumerate(runner.MAIN_MENU)])
        self.assertEqual(runner.focused_menu(path, obs, runner.MAIN_MENU), '退出游戏')
        obs['lines'].pop()
        self.assertEqual(runner.focused_menu(path, obs, runner.MAIN_MENU), '退出游戏')

    def test_dim_menu_focus_keeps_two_edge_and_ambiguity_checks(self):
        image = Image.new('RGB', (1920, 1080), 'black')
        draw = ImageDraw.Draw(image)
        draw.rectangle((160, 400, 500, 461), outline=(115, 115, 115), width=2)
        path = self.out / 'dim-focus.png'
        image.save(path)
        obs = dict(scale=1, lines=[dict(text=label, words=[dict(x=285,y=375+i*46,width=95,height=23)])
                                  for i, label in enumerate(runner.MAIN_MENU)])
        self.assertEqual(runner.focused_menu(path, obs, runner.MAIN_MENU), '新游戏')
        draw.rectangle((160, 677, 500, 738), outline=(115, 115, 115), width=2)
        image.save(path)
        self.assertIsNone(runner.focused_menu(path, obs, runner.MAIN_MENU))

    def test_transient_unknown_menu_focus_reobserves_without_input(self):
        with mock.patch.object(self.session, 'observe', return_value={}), \
                mock.patch.object(runner, 'focused_menu', side_effect=[None, '继续']), \
                mock.patch.object(self.session, 'evidence'), \
                mock.patch.object(self.session, 'key') as key, \
                mock.patch.object(runner.time, 'sleep'):
            self.session.select_menu('继续', runner.PAUSE_MENU)
        key.assert_not_called()

    def test_persistent_unknown_menu_focus_still_rejects_input(self):
        with mock.patch.object(self.session, 'observe', return_value={}), \
                mock.patch.object(runner, 'focused_menu', return_value=None), \
                mock.patch.object(self.session, 'key') as key, \
                mock.patch.object(runner.time, 'sleep'):
            with self.assertRaisesRegex(RuntimeError, 'menu_focus_unknown'):
                self.session.select_menu('继续', runner.PAUSE_MENU)
        key.assert_not_called()

    def test_worker_diagnostic_failure_still_writes_result(self):
        runner.dump(self.out / 'target.json', self.session.cfg | dict(scenario='menu'))
        with mock.patch.object(runner.Session, 'launch', side_effect=RuntimeError('launch failure')), \
                mock.patch.object(runner, 'freeze_logs', side_effect=OSError('log inaccessible')):
            self.assertEqual(runner.worker(self.out), 1)
        result = json.loads((self.out / 'result.json').read_text(encoding='utf-8'))
        self.assertEqual(result['result'], 'failed')
        self.assertIn('log inaccessible', result['diagnostic_error'])

    def test_transient_failure_retries_at_most_twice_and_keeps_attempt_records(self):
        cfg = dict(executable='game.exe', scenario='smoke')
        failure = dict(result='failed', guard='verified_original_hashes_and_released', error='scene_load_timeout')
        with mock.patch.object(runner, 'process_inventory', return_value=[]), \
                mock.patch.object(runner, 'run_attempt', return_value=failure) as attempt, \
                mock.patch('builtins.print'):
            self.assertEqual(runner.run_trials(cfg, self.out, 2), 1)
        self.assertEqual(attempt.call_count, 3)
        summary = json.loads((self.out/'suite-summary.json').read_text(encoding='utf-8'))
        self.assertEqual(len(summary['attempts']), 3)

    def test_crash_or_guard_failure_prevents_retry(self):
        cfg = dict(executable='game.exe', scenario='smoke')
        for extra in [dict(crashes=['crash']), dict(guard='failed'), dict(error='menu_focus_unknown')]:
            failure = dict(result='failed', guard='verified_original_hashes_and_released', error='scene_load_timeout') | extra
            with self.subTest(extra=extra), mock.patch.object(runner, 'process_inventory', return_value=[]), \
                    mock.patch.object(runner, 'run_attempt', return_value=failure) as attempt, mock.patch('builtins.print'):
                self.assertEqual(runner.run_trials(cfg, self.out, 2), 1)
                attempt.assert_called_once()

    def test_workspace_lock_rejects_second_runner_and_releases_on_error(self):
        with mock.patch.object(runner, 'ROOT', self.out):
            with self.assertRaisesRegex(ValueError, 'body'):
                with runner.runner_lock():
                    with self.assertRaisesRegex(RuntimeError, 'Another'):
                        with runner.runner_lock():
                            self.fail('Concurrent runner accepted')
                    raise ValueError('body')
            with runner.runner_lock():
                pass


if __name__ == '__main__':
    unittest.main()
