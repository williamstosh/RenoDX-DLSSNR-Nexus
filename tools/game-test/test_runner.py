"""Regression checks for exit races and focus validation without launching a game."""
import json
from pathlib import Path
import tempfile
import sys
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
        park_patch = mock.patch.object(runner.desktop, 'park_cursor')
        self.park_cursor = park_patch.start()
        self.addCleanup(park_patch.stop)

    def test_manual_menu_checks_module_without_automated_ui(self):
        self.session.cfg.update(manual_ime_session=True, capture_hold_seconds=900)
        with (mock.patch.object(self.session, 'main_menu', side_effect=AssertionError('No automatic UI')),
              mock.patch.object(self.session, 'check_mod') as check_mod,
              mock.patch.object(self.session, 'capture_hold') as capture_hold):
            self.session.menu()
        check_mod.assert_called_once_with()
        capture_hold.assert_called_once_with()
        self.assertEqual(self.session.ui_state, 'manual_menu')

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

    def test_verified_modules_reuse_observed_paths_once(self):
        addon = self.out / 'renodx-dlss5.addon64'
        runtime = self.out / 'nvngx_dlss.dll'
        addon.write_bytes(b'addon')
        runtime.write_bytes(b'runtime')
        self.session.cfg.update(required_module=addon.name, addon_sha256=runner.sha(addon))
        paths = [str(addon), str(runtime)]
        with mock.patch.object(runner.desktop, 'modules', return_value=paths) as enumerate_modules, \
             mock.patch.object(runner.crash_evidence, 'modules', wraps=runner.crash_evidence.modules) as snapshot:
            self.session.check_mod()
            first = (self.out / 'verified-module-identities.json').read_bytes()
            self.session.check_mod()
        self.assertEqual(enumerate_modules.call_count, 2)
        snapshot.assert_called_once()
        self.assertEqual((self.out / 'verified-module-identities.json').read_bytes(), first)
        entries = json.loads(first)['modules']
        self.assertEqual([entry['path'] for entry in entries], paths)
        self.assertTrue(all(entry['hash_status'] == 'hashed' for entry in entries))
        self.assertEqual(entries[1]['disk_sha256'], runner.sha(runtime))

    def test_smoke_pause_uses_observed_focus_without_requiring_full_ocr_sentence(self):
        order = []
        with mock.patch.object(self.session, 'main_menu'), \
                mock.patch.object(self.session, 'check_mod'), \
                mock.patch.object(self.session, 'select_menu') as select_menu, \
                mock.patch.object(self.session, 'key') as key, \
                mock.patch.object(self.session, 'load_save'), \
                mock.patch.object(self.session, 'wait_scene'), \
                mock.patch.object(self.session, 'evidence'), \
                mock.patch.object(self.session, 'capture_hold', side_effect=lambda: order.append('capture')), \
                mock.patch.object(self.session, 'scene_health', side_effect=lambda: order.append('health')), \
                mock.patch.object(self.session, 'wait_text', side_effect=AssertionError('redundant ordered OCR gate')):
            self.session.smoke()
        self.assertEqual(select_menu.call_args_list,
            [mock.call('读取游戏', runner.MAIN_MENU), mock.call('继续', runner.PAUSE_MENU)])
        self.assertEqual(key.call_args_list, [mock.call('ENTER'), mock.call('ESC'), mock.call('ESC')])
        self.assertEqual(order, ['health', 'capture'])
        self.assertEqual(self.session.status['functionality'],
            'pause_and_resume_input_response_confirmed_mod_features_not_tested')

    def test_visible_unique_save_is_clicked_before_traversing_other_rows(self):
        self.session.cfg.update(save_modified='2026-10-14T01:08:30', save='manual.sav', save_sha256='hash')
        wrong = dict(timestamp='2026-10-14T01:35:49', y=100, selected=True, text='autosave')
        target = dict(timestamp='2026-10-14T01:08:30', y=200, selected=False, text='unique manual timestamp')
        selected = dict(target, selected=True)
        with mock.patch.object(self.session, 'observe', return_value={'lines': []}), \
                mock.patch.object(runner, 'save_rows', side_effect=[[wrong, target], [selected]]), \
                mock.patch.object(self.session, 'key') as key, \
                mock.patch.object(self.session, 'click_text') as click, \
                mock.patch.object(self.session, 'evidence'), \
                mock.patch.object(runner.time, 'sleep'):
            self.session.load_save()
        self.assertEqual(key.call_args_list, [mock.call('ENTER')])
        click.assert_called_once_with({'lines': []}, 'uniquemanualtimestamp')
        receipt = json.loads((self.out / 'save-selection.json').read_text(encoding='utf-8'))
        self.assertEqual(receipt['timestamp'], '2026-10-14T01:08:30')

    def test_unparsed_focused_save_can_click_unique_target_then_reobserve(self):
        # A focused 10:33:13 row may be read as 10:3:13,
        # while the next row's target timestamp remained intact.
        self.session.cfg.update(save_modified='2026-10-14T01:08:30',
                                save='manual.sav', save_sha256='hash')
        image = Image.new('RGB', (1920, 1080), 'black')
        draw = ImageDraw.Draw(image)
        draw.rectangle((557, 544, 1610, 610), outline='white', width=2)
        image.save(self.out / 'latest.png')
        obs = dict(scale=1, lines=[dict(text=text, words=[dict(x=570, y=y, width=440, height=23)])
            for text, y in [('其他存档 - 2026年11月1日 10：3：13', 566),
                            ('目标存档 - 2026年10月14日 1：08：30', 629)]])
        rows = runner.save_rows(self.out / 'latest.png', obs)
        self.assertEqual(len(rows), 1)
        self.assertFalse(rows[0]['selected'])
        selected = dict(rows[0], selected=True)
        with mock.patch.object(self.session, 'observe', return_value=obs) as observe, \
                mock.patch.object(runner, 'save_rows', side_effect=[rows, [selected]]), \
                mock.patch.object(self.session, 'click_text') as click, \
                mock.patch.object(self.session, 'key') as key, \
                mock.patch.object(self.session, 'evidence'), mock.patch.object(runner.time, 'sleep'):
            self.session.load_save()
        self.assertEqual(observe.call_count, 2)
        click.assert_called_once_with(obs, runner.compact(rows[0]['text']))
        key.assert_called_once_with('ENTER')

    def test_target_click_without_observed_focus_never_loads_or_repeats_click(self):
        self.session.cfg.update(save_modified='2026-10-14T01:08:30', max_save_steps=3)
        target = dict(timestamp='2026-10-14T01:08:30', y=640.5, selected=False, text='target')
        with mock.patch.object(self.session, 'observe', return_value={}), \
                mock.patch.object(runner, 'save_rows', return_value=[target]), \
                mock.patch.object(self.session, 'click_text') as click, \
                mock.patch.object(self.session, 'key') as key, mock.patch.object(runner.time, 'sleep'):
            with self.assertRaisesRegex(TimeoutError, 'save_selection_timeout'):
                self.session.load_save()
        click.assert_called_once()
        key.assert_not_called()
        self.assertFalse((self.out / 'save-selection.json').exists())

    def test_duplicate_target_timestamp_rejected_even_if_one_has_focus(self):
        self.session.cfg.update(save_modified='2026-10-14T01:08:30')
        target = dict(timestamp='2026-10-14T01:08:30', y=640.5, selected=True, text='target')
        with mock.patch.object(self.session, 'observe', return_value={}), \
                mock.patch.object(runner, 'save_rows', return_value=[target, dict(target, selected=False)]), \
                mock.patch.object(self.session, 'click_text') as click, \
                mock.patch.object(self.session, 'key') as key:
            with self.assertRaisesRegex(RuntimeError, 'ambiguous_ui_save_timestamp'):
                self.session.load_save()
        click.assert_not_called()
        key.assert_not_called()

    def test_save_click_direct_load_waits_for_save_list_exit_without_enter(self):
        self.session.cfg.update(save_modified='2026-10-14T01:08:30', save='manual.sav',
                                save_sha256='hash', load_timeout=120)
        self.session.ui_state = 'load_menu'
        target = dict(timestamp='2026-10-14T01:08:30', selected=False, text='target', y=640)
        observations = [dict(text='target'), dict(text='loading tip')]
        with mock.patch.object(self.session, 'observe', side_effect=observations) as observe, \
                mock.patch.object(runner, 'save_rows', side_effect=[[target], []]), \
                mock.patch.object(self.session, 'click_text') as click, \
                mock.patch.object(self.session, 'key') as key, \
                mock.patch.object(self.session, 'evidence'), \
                mock.patch.object(self.session, 'wait_scene', side_effect=AssertionError('load transition is separate')), \
                mock.patch.object(runner.time, 'sleep'):
            self.session.load_save()
        click.assert_called_once()
        key.assert_not_called()
        self.assertEqual(self.session.ui_state, 'loading')
        self.assertEqual(observe.call_count, 2)
        receipt = json.loads((self.out / 'save-selection.json').read_text())
        self.assertEqual(receipt['timestamp'], target['timestamp'])
        self.assertIn('load menu exited', receipt['basis'])
        self.session.ui_state = 'scene'
        with mock.patch.object(runner.desktop, 'same_process', side_effect=[True, False]), \
                mock.patch.object(self.session, 'key') as exit_key, \
                mock.patch.object(self.session, 'request_exit') as request_exit:
            self.session.shutdown()
        exit_key.assert_called_once_with('ESC')
        request_exit.assert_called_once()

    def test_save_click_does_not_confirm_if_save_list_remains(self):
        self.session.cfg.update(save_modified='2026-10-14T01:08:30', max_save_steps=3)
        self.session.ui_state = 'load_menu'
        target = dict(timestamp='2026-10-14T01:08:30', selected=False, text='target', y=640)
        with mock.patch.object(self.session, 'observe', return_value=dict(text='unrelated screen text')), \
                mock.patch.object(runner, 'save_rows', return_value=[target]), \
                mock.patch.object(self.session, 'click_text') as click, \
                mock.patch.object(self.session, 'key') as key, \
                mock.patch.object(runner.time, 'sleep'):
            with self.assertRaisesRegex(TimeoutError, 'save_selection_timeout'):
                self.session.load_save()
        click.assert_called_once()
        key.assert_not_called()
        self.assertEqual(self.session.ui_state, 'load_menu')
        self.assertFalse((self.out / 'save-selection.json').exists())

    def test_unknown_timestamp_does_not_click_or_enter(self):
        self.session.cfg.update(save_modified='2026-10-14T01:08:30', max_save_steps=2)
        with mock.patch.object(self.session, 'observe', return_value=dict(text='unknown')), \
                mock.patch.object(runner, 'save_rows', return_value=[]), \
                mock.patch.object(self.session, 'click_text') as click, \
                mock.patch.object(self.session, 'key') as key, mock.patch.object(runner.time, 'sleep'):
            with self.assertRaises(TimeoutError):
                self.session.load_save()
        click.assert_not_called()
        key.assert_not_called()

    def test_load_wait_requires_menu_exit_and_ignores_scene_text(self):
        self.session.ui_state = 'loading'
        self.session.cfg['load_timeout'] = 120
        self.session.cfg.pop('scene_patterns', None)
        target = dict(timestamp='2026-10-14T01:08:30', selected=True, text='save row', y=640)
        observations = [dict(text='expected quest'), dict(text='wrong quest'), dict(text='')]
        with mock.patch.object(self.session, 'observe', side_effect=observations) as observe, \
                mock.patch.object(runner, 'save_rows', side_effect=[[target], [], []]), \
                mock.patch.object(self.session, 'event') as event, \
                mock.patch.object(runner.time, 'sleep'):
            result = self.session.wait_scene()
        self.assertEqual(result['text'], '')
        self.assertEqual(observe.call_count, 3)
        self.assertTrue(all(call.kwargs['region_state'] == 'load_menu' for call in observe.call_args_list))
        self.assertEqual(self.session.ui_state, 'scene')
        event.assert_called_once_with('gameplay_transition_confirmed', from_state='loading',
                                      basis='menu_absent_in_two_observations')

    def test_save_list_parks_cursor_before_capture_without_clicking(self):
        self.session.ui_state = 'load_menu'
        self.session.cfg['ocr_executable'] = 'unused-mocked-ocr'
        Image.new('RGB', (3840, 2160)).save(self.out / 'latest.png')
        order = []
        self.park_cursor.side_effect = lambda *a, **k: order.append('park')
        result = mock.Mock(returncode=0, stdout=b'{"text":"target","lines":[]}')
        with mock.patch.object(runner.desktop, 'same_process', return_value=True), \
                mock.patch.object(self.session, 'activate'), \
                mock.patch.object(runner.desktop, 'capture', side_effect=lambda *a: order.append('capture') or {}), \
                mock.patch.object(runner.subprocess, 'run', return_value=result), \
                mock.patch.object(runner.desktop, 'click') as click:
            obs = self.session.observe('save-list')
        self.assertEqual(order, ['park', 'capture'])
        self.assertEqual(obs['scale'], .5)
        click.assert_not_called()
        self.assertEqual(self.session.ui_state, 'load_menu')

    def test_cursor_park_failure_blocks_capture_and_further_game_input(self):
        self.session.ui_state = 'load_menu'
        self.park_cursor.side_effect = RuntimeError('focus changed')
        with mock.patch.object(self.session, 'activate'), \
                mock.patch.object(runner.desktop, 'same_process', return_value=True), \
                mock.patch.object(runner.desktop, 'capture') as capture, \
                mock.patch.object(self.session, 'key') as key:
            with self.assertRaisesRegex(RuntimeError, 'focus changed'):
                self.session.observe('save-list')
            self.session.shutdown()
        capture.assert_not_called()
        key.assert_not_called()
        self.assertTrue(self.session.automation_input_blocked)

    def test_post_click_menu_probe_never_moves_cursor_even_after_menu_frame(self):
        self.session.ui_state = 'load_menu'
        self.session.cfg['ocr_executable'] = 'unused-mocked-ocr'
        Image.new('RGB', (1920, 1080)).save(self.out / 'latest.png')
        result = mock.Mock(returncode=0, stdout=b'{"text":"","lines":[]}')
        with mock.patch.object(self.session, 'activate'), \
                mock.patch.object(runner.desktop, 'same_process', return_value=True), \
                mock.patch.object(runner.desktop, 'capture', return_value={}), \
                mock.patch.object(runner.subprocess, 'run', return_value=result):
            self.session.observe('save-list', region_state='load_menu')
        self.park_cursor.assert_not_called()

    def test_save_click_remaining_menu_does_not_start_scene_wait(self):
        self.session.cfg.update(save_modified='2026-10-14T01:08:30', max_save_steps=3)
        target = dict(timestamp='2026-10-14T01:08:30', y=640, selected=False, text='target')
        with mock.patch.object(self.session, 'observe', return_value=dict(text='expected quest')), \
                mock.patch.object(runner, 'save_rows', return_value=[target]), \
                mock.patch.object(self.session, 'click_text'), \
                mock.patch.object(self.session, 'key') as key, \
                mock.patch.object(self.session, 'wait_scene') as wait_scene, \
                mock.patch.object(runner.time, 'sleep'):
            with self.assertRaisesRegex(TimeoutError, 'save_selection_timeout'):
                self.session.load_save()
        key.assert_not_called()
        wait_scene.assert_not_called()
        self.assertEqual(self.session.ui_state, 'load_menu')

    def test_resume_wait_requires_pause_menu_exit_without_scene_text(self):
        self.session.ui_state = 'scene'
        self.session.cfg.pop('scene_patterns', None)
        observations = [dict(text='继续'), dict(text=''), dict(text='')]
        with mock.patch.object(self.session, 'observe', side_effect=observations) as observe, \
                mock.patch.object(self.session, 'event'), \
                mock.patch.object(runner.time, 'sleep'):
            self.session.wait_scene(timeout=20)
        self.assertEqual(observe.call_count, 3)
        self.assertTrue(all(call.kwargs['region_state'] == 'pause_menu' for call in observe.call_args_list))
        self.assertEqual(self.session.ui_state, 'scene')

    def test_pending_save_click_shutdown_does_not_send_menu_escape(self):
        self.session.ui_state = 'save_click_pending'
        with mock.patch.object(runner.desktop, 'same_process', side_effect=[True, False]), \
                mock.patch.object(self.session, 'observe', return_value=dict(text='unknown')), \
                mock.patch.object(self.session, 'key') as key:
            with self.assertRaisesRegex(RuntimeError, 'No commissioned exit route'):
                self.session.shutdown()
        key.assert_not_called()

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

    def test_capture_hold_argument_range_and_supported_scenarios(self):
        for value in ('-1', '1', '9', '901', 'not-an-integer'):
            with self.subTest(value=value), self.assertRaises(runner.argparse.ArgumentTypeError):
                runner.parse_capture_hold_seconds(value)
        self.assertEqual(runner.parse_capture_hold_seconds('0'), 0)
        self.assertEqual(runner.parse_capture_hold_seconds('10'), 10)
        self.assertEqual(runner.parse_capture_hold_seconds('120'), 120)
        self.assertEqual(runner.parse_capture_hold_seconds('900'), 900)
        runner.validate_capture_hold_scenario('menu', 900)
        with self.assertRaisesRegex(ValueError, 'only supported for smoke or menu'):
            runner.validate_capture_hold_scenario('calibrate', 10)

    def test_menu_hold_verifies_loaded_addon_without_loading_a_save(self):
        with mock.patch.object(self.session, 'main_menu') as main_menu, \
                mock.patch.object(self.session, 'check_mod') as check_mod, \
                mock.patch.object(self.session, 'capture_hold') as hold, \
                mock.patch.object(self.session, 'load_save', side_effect=AssertionError('IME needs no save')):
            self.session.cfg['capture_hold_seconds'] = 900
            self.session.menu()
        main_menu.assert_called_once()
        check_mod.assert_called_once()
        hold.assert_called_once()

    def test_capture_hold_requires_strict_matching_pid_completion_signal(self):
        self.session.cfg.update(scenario='smoke', capture_hold_seconds=10)
        foreground = dict(hwnd=456, pid=123, created=1, elapsed_ms=12)
        signal = self.out / 'capture-complete.json'
        for contents in ('[]', '{"pid":123,"pid":123,"completed":true}',
                         '{"pid":123,"completed":1}', '{"pid":123,"completed":NaN}'):
            signal.write_text(contents, encoding='utf-8')
            self.assertFalse(runner.capture_complete(self.out, 123), contents)
        runner.dump(signal, dict(pid=999, completed=True))

        def write_matching_signal(_seconds):
            runner.dump(signal, dict(pid=123, completed=True))

        with mock.patch.object(runner.desktop, 'ensure_foreground', return_value=foreground) as activate, \
                mock.patch.object(runner.desktop, 'same_process', return_value=True) as same_process, \
                mock.patch.object(runner.time, 'monotonic', side_effect=[0, 0]), \
                mock.patch.object(runner.time, 'sleep', side_effect=write_matching_signal) as sleep:
            self.session.capture_hold()
        activate.assert_called_once_with(self.session.window)
        self.assertEqual(same_process.call_count, 2)
        sleep.assert_called_once_with(1)
        ready = json.loads((self.out / 'capture-ready.json').read_text(encoding='utf-8'))
        self.assertEqual(ready['pid'], 123)
        self.assertEqual(ready['identity'], self.session.window)
        self.assertIs(ready['automatic_foreground'], True)
        self.assertEqual(ready['foreground'], foreground)
        self.assertFalse(self.session.automation_input_blocked)
        self.assertTrue(ready['time'])
        self.assertEqual(self.session.status['capture_hold'], 'completed_by_signal')

    def test_capture_hold_process_exit_fails(self):
        self.session.cfg.update(scenario='smoke', capture_hold_seconds=10)
        with mock.patch.object(runner.desktop, 'ensure_foreground', return_value={}), \
                mock.patch.object(runner.desktop, 'same_process', return_value=False), \
                mock.patch.object(runner.time, 'sleep') as sleep:
            with self.assertRaisesRegex(RuntimeError, 'capture_hold_process_exited'):
                self.session.capture_hold()
        sleep.assert_not_called()
        self.assertEqual(self.session.status['capture_hold'], 'process_exited')
        events = [json.loads(line) for line in (self.out / 'events.jsonl').read_text(encoding='utf-8').splitlines()]
        self.assertIn('capture_hold_process_exit', [event['kind'] for event in events])

    def test_automatic_capture_completion_releases_hold_without_human_confirmation(self):
        import fg_acceptance

        self.session.cfg.update(scenario='smoke', capture_hold_seconds=10)
        runner.dump(self.out / 'owned-process.json', self.session.window)
        receipt = dict(hwnd=456, pid=123, created=1, elapsed_ms=12)

        def collect_when_ready(_seconds):
            self.assertTrue(self.session.automation_input_blocked)
            self.assertTrue((self.out / 'capture-ready.json').exists())
            with mock.patch.object(fg_acceptance.time, 'sleep'):
                fg_acceptance.capture(self.out, nr_off_control=True)

        with mock.patch.object(runner.desktop, 'ensure_foreground', return_value=receipt), \
                mock.patch.object(runner.desktop, 'same_process', return_value=True), \
                mock.patch.object(runner.desktop, 'modules', return_value=[]), \
                mock.patch.object(runner.desktop, 'capture') as capture, \
                mock.patch.object(runner.desktop, 'key') as key, \
                mock.patch.object(runner.time, 'monotonic', return_value=0), \
                mock.patch.object(runner.time, 'sleep', side_effect=collect_when_ready) as sleep:
            self.session.capture_hold()
        sleep.assert_called_once_with(1)
        capture.assert_called_once()
        key.assert_not_called()
        self.assertTrue(runner.capture_complete(self.out, 123))
        self.assertEqual(self.session.status['capture_hold'], 'completed_by_signal')
        self.assertFalse(self.session.automation_input_blocked)
        self.assertFalse((self.out / 'capture-failed.json').exists())
        self.assertFalse(list(self.out.glob('computer-use-*.json')))

    def test_early_completion_keeps_input_blocked_until_strict_parent_script_ack(self):
        self.session.cfg.update(scenario='smoke', capture_hold_seconds=10, capture_script='fixture.py')
        runner.dump(self.out / 'capture-complete.json', dict(pid=123, completed=True))
        invalid = [
            '[]', '{', '{"pid":999,"exit_code":0}',
            '{"pid":true,"exit_code":0}', '{"pid":"123","exit_code":0}',
            '{"pid":123,"exit_code":1}', '{"pid":123,"exit_code":false}',
            '{"pid":123,"exit_code":0.0}', '{"pid":123,"exit_code":"0"}',
            '{"pid":123,"exit_code":0,"exit_code":0}',
            '{"pid":123,"exit_code":NaN}',
        ]
        pending = iter(invalid + ['{"pid":123,"exit_code":0}'])

        def parent_progress(_seconds):
            self.assertTrue(self.session.automation_input_blocked)
            self.assertEqual(self.session.status['capture_hold'], 'waiting')
            self.assertTrue(runner.capture_complete(self.out, 123))
            self.assertFalse(runner.capture_script_complete(self.out, 123))
            (self.out / 'capture-script-result.json').write_text(next(pending), encoding='utf-8')

        with mock.patch.object(runner.desktop, 'ensure_foreground', return_value={}), \
                mock.patch.object(runner.desktop, 'same_process', return_value=True), \
                mock.patch.object(runner.desktop, 'key') as key, \
                mock.patch.object(runner.time, 'monotonic', return_value=0), \
                mock.patch.object(runner.time, 'sleep', side_effect=parent_progress) as sleep:
            self.session.capture_hold()
        self.assertEqual(sleep.call_count, len(invalid) + 1)
        key.assert_not_called()
        self.assertTrue(runner.capture_script_complete(self.out, 123))
        self.assertEqual(self.session.status['capture_hold'], 'completed_by_signal')
        self.assertFalse(self.session.automation_input_blocked)

    def test_capture_hold_expiry_blocks_game_input_and_fails_closed(self):
        self.session.cfg.update(scenario='smoke', capture_hold_seconds=10)
        with mock.patch.object(runner.desktop, 'ensure_foreground', return_value={}), \
                mock.patch.object(runner.desktop, 'same_process', return_value=True), \
                mock.patch.object(runner.time, 'monotonic', side_effect=[0, 0, 10]), \
                mock.patch.object(runner.time, 'sleep'):
            with self.assertRaisesRegex(TimeoutError, 'capture_hold_timeout'):
                self.session.capture_hold()
        self.assertEqual(self.session.status['capture_hold'], 'timeout_not_mod_acceptance')
        self.assertTrue(self.session.automation_input_blocked)
        events = [json.loads(line) for line in (self.out / 'events.jsonl').read_text(encoding='utf-8').splitlines()]
        self.assertEqual(events[-1]['kind'], 'capture_hold_timeout')
        self.assertEqual(events[-1]['result'], 'timeout_not_mod_acceptance_no_game_input')
        with mock.patch.object(runner.desktop, 'same_process', return_value=True), \
                mock.patch.object(self.session, 'key', side_effect=AssertionError('No automatic game input')):
            self.session.shutdown()
        self.assertEqual(self.session.status['safe_exit'], 'protected_recovery_required_no_game_input')

    def test_invalid_capture_failure_receipt_still_blocks_game_input(self):
        self.session.cfg.update(scenario='smoke', capture_hold_seconds=10)
        (self.out / 'capture-failed.json').write_text('[]', encoding='utf-8')
        with mock.patch.object(runner.desktop, 'ensure_foreground', return_value={}), \
                mock.patch.object(runner.desktop, 'same_process', return_value=True):
            with self.assertRaisesRegex(RuntimeError, 'capture_automatic_failed'):
                self.session.capture_hold()
        self.assertTrue(self.session.automation_input_blocked)
        with mock.patch.object(self.session, 'key', side_effect=AssertionError('No automatic game input')):
            self.session.shutdown()
        self.assertEqual(self.session.status['safe_exit'], 'protected_recovery_required_no_game_input')

    def test_capture_automatic_activation_failure_keeps_gate_without_ready_or_input(self):
        self.session.cfg.update(scenario='smoke', capture_hold_seconds=60)
        with mock.patch.object(runner.desktop, 'ensure_foreground',
                               side_effect=RuntimeError('automatic activation failed')) as activate, \
                mock.patch.object(runner.desktop, 'key') as key, \
                mock.patch.object(runner.desktop, 'capture') as capture:
            with self.assertRaisesRegex(RuntimeError, 'automatic activation failed'):
                self.session.capture_hold()
        activate.assert_called_once_with(self.session.window)
        key.assert_not_called()
        capture.assert_not_called()
        self.assertTrue(self.session.automation_input_blocked)
        failure = json.loads((self.out / 'capture-failed.json').read_text(encoding='utf-8'))
        self.assertEqual(failure['stage'], 'automatic_foreground_activation')
        self.assertTrue(failure['no_game_input_sent'])
        self.assertFalse((self.out / 'capture-complete.json').exists())
        self.assertFalse((self.out / 'capture-ready.json').exists())

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

    def test_manual_ime_deadline_keeps_waiting_until_matching_completion(self):
        self.session.cfg.update(scenario='menu', capture_hold_seconds=10, manual_ime_session=True)
        with mock.patch.object(runner.desktop, 'ensure_foreground',
                               side_effect=AssertionError('No automatic activation')), \
                mock.patch.object(runner.desktop, 'same_process', return_value=True), \
                mock.patch.object(runner, 'capture_complete', side_effect=[False, False, True]), \
                mock.patch.object(runner.time, 'monotonic', side_effect=[0, 10, 11]), \
                mock.patch.object(runner.time, 'sleep'), \
                mock.patch.object(self.session, 'key', side_effect=AssertionError('No automatic input')):
            self.session.capture_hold()
        self.assertTrue((self.out / 'manual-intervention-required.json').exists())
        self.assertEqual(self.session.status['capture_hold'], 'completed_by_signal')

    def test_manual_ime_shutdown_waits_for_user_without_game_input_or_termination(self):
        self.session.cfg['manual_ime_session'] = True
        self.session.status['capture_hold'] = 'completed_by_signal'
        with mock.patch.object(runner.desktop, 'same_process', side_effect=[True, False]), \
                mock.patch.object(runner.time, 'sleep'), \
                mock.patch.object(self.session, 'wait_exit') as wait_exit, \
                mock.patch.object(self.session, 'key', side_effect=AssertionError('No automatic input')), \
                mock.patch.object(runner.desktop, 'terminate', side_effect=AssertionError('No force quit')):
            self.session.shutdown()
        wait_exit.assert_called_once_with(timeout=0)
        self.assertTrue(json.loads((self.out / 'manual-exit-ready.json').read_text())['requested'])

    def test_manual_ime_recovery_holds_protection_instead_of_terminating_game(self):
        self.session.cfg['manual_ime_session'] = True
        with mock.patch.object(runner.desktop, 'same_process', side_effect=[True, True, False]), \
                mock.patch.object(runner, 'freeze_logs'), \
                mock.patch.object(runner.time, 'sleep'), \
                mock.patch.object(runner.desktop, 'terminate', side_effect=AssertionError('No force quit')):
            self.assertTrue(runner.recover_owned(self.session.cfg, self.out, self.session.window))
        self.assertTrue((self.out / 'intervention-required.json').exists())

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

    def test_observe_parks_cursor_before_capturing_menu_states(self):
        for ui_state, region_state, should_park in (
                ('main_menu', None, True), ('pause_menu', None, True),
                ('load_menu', None, True), ('starting', 'load_menu', False),
                ('scene', None, False)):
            with self.subTest(ui_state=ui_state, region_state=region_state):
                self.session.ui_state = ui_state
                self.session.cfg['ocr_executable'] = 'ocr.exe'
                order = []
                image = mock.MagicMock()
                image.__enter__.return_value = image
                image.width, image.height = 1920, 1080
                image.resize.return_value = image
                image.crop.return_value = image
                result = mock.Mock(returncode=0, stdout=b'{"text":"","lines":[]}')
                with mock.patch.object(runner.desktop, 'same_process', return_value=True), \
                        mock.patch.object(self.session, 'activate'), \
                        mock.patch.object(runner.desktop, 'park_cursor',
                                          side_effect=lambda *_: order.append('park')), \
                        mock.patch.object(runner.desktop, 'capture',
                                          side_effect=lambda *_: order.append('capture') or {}), \
                        mock.patch.object(runner.Image, 'open', return_value=image), \
                        mock.patch.object(runner.subprocess, 'run', return_value=result):
                    self.session.observe('cursor-test', region_state=region_state)
                self.assertEqual(order, ['park', 'capture'] if should_park else ['capture'])

    def test_worker_diagnostic_failure_still_writes_result(self):
        runner.dump(self.out / 'target.json', self.session.cfg | dict(scenario='menu'))
        with mock.patch.object(runner.Session, 'launch', side_effect=RuntimeError('launch failure')), \
                mock.patch.object(runner, 'freeze_logs', side_effect=OSError('log inaccessible')):
            self.assertEqual(runner.worker(self.out), 1)
        result = json.loads((self.out / 'result.json').read_text(encoding='utf-8'))
        self.assertEqual(result['result'], 'failed')
        self.assertIn('log inaccessible', result['diagnostic_error'])

    def test_frozen_worker_imports_all_runtime_dependencies(self):
        (self.out / 'saves').mkdir()
        cfg = self.session.cfg | dict(profile=str(self.out), save_dir='saves')
        with mock.patch('guard.ProtectedFiles', side_effect=RuntimeError('synthetic stop before launch')):
            runner.run_attempt(cfg, self.out / 'attempt')
        source = self.out / 'attempt/source'
        manifest = json.loads((self.out / 'attempt/source-manifest.json').read_text(encoding='utf-8'))
        self.assertIn('normal_process.py', [item['name'] for item in manifest['files']])
        result = runner.subprocess.run([sys.executable, '-B', '-c', 'import runner'], cwd=source,
                                       capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)

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

    def test_foreground_denial_is_not_retried(self):
        cfg = dict(executable='game.exe', scenario='smoke')
        for error in ('Target cannot acquire foreground; no input sent',
                      'automatic_foreground_activation_failed', 'capture_automatic_failed', 'capture_hold_timeout'):
            failure = dict(result='failed', guard='verified_original_hashes_and_released', error=error)
            with self.subTest(error=error), \
                    mock.patch.object(runner, 'process_inventory', return_value=[]), \
                    mock.patch.object(runner, 'run_attempt', return_value=failure) as attempt, \
                    mock.patch('builtins.print'):
                self.assertEqual(runner.run_trials(cfg, self.out, 2), 1)
            attempt.assert_called_once()

    def test_automatic_activation_records_verified_target_identity(self):
        receipt = dict(hwnd=456, pid=123, created=1, elapsed_ms=12)
        with mock.patch.object(runner.desktop, 'ensure_foreground', return_value=receipt) as activate:
            self.session.activate()
        activate.assert_called_once_with(self.session.window)
        events = [json.loads(line) for line in (self.out / 'events.jsonl').read_text(encoding='utf-8').splitlines()]
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]['kind'], 'automatic_foreground_confirmed')
        self.assertEqual(events[0]['phase'], 'runner_input')
        for field, value in receipt.items():
            self.assertEqual(events[0][field], value)
        self.assertIn('time', events[0])
        self.assertFalse(list(self.out.glob('computer-use-*.json')))

    def test_background_launch_automatically_activates_before_observation(self):
        self.session.cfg.update(steam='steam.exe', launch_args=['-applaunch', 'fixture'],
                                startup_timeout=10, ocr_executable='fixture-ocr.exe')
        self.session.window = None
        window = dict(hwnd=456, pid=123, created=1, path='game.exe')
        order = []

        def capture_fixture(target, path):
            order.append(('capture', target))
            Image.new('RGB', (1920, 1080)).save(path)
            return dict(width=1920, height=1080)

        with mock.patch.object(runner.subprocess, 'Popen') as launch, \
                mock.patch.object(runner.desktop, 'windows', return_value=[window]), \
                mock.patch.object(runner.desktop, 'modules', return_value=[]), \
                mock.patch.object(runner.desktop, 'same_process', return_value=True), \
                mock.patch.object(runner.desktop, 'ensure_foreground',
                    side_effect=lambda target: order.append(('activate', target)) or
                        dict(hwnd=456, pid=123, created=1, elapsed_ms=12)), \
                mock.patch.object(runner.desktop, 'capture', side_effect=capture_fixture), \
                mock.patch.object(runner.subprocess, 'run', return_value=mock.Mock(
                    returncode=0, stdout=b'{"text":"fixture","lines":[]}')):
            self.session.launch()
            self.session.observe('startup')
        launch.assert_called_once()
        self.assertEqual(order, [('activate', window), ('capture', window)])
        self.assertEqual(json.loads((self.out / 'owned-process.json').read_text()), window)

    def test_automatic_activation_failure_blocks_observation_without_input(self):
        with mock.patch.object(runner.desktop, 'same_process', return_value=True), \
                mock.patch.object(runner.desktop, 'ensure_foreground',
                    side_effect=RuntimeError('automatic activation failed')) as activate, \
                mock.patch.object(runner.desktop, 'capture') as capture, \
                mock.patch.object(runner.desktop, 'key') as key:
            with self.assertRaisesRegex(RuntimeError, 'automatic activation failed'):
                self.session.observe('startup')
            self.session.shutdown()
        activate.assert_called_once_with(self.session.window)
        capture.assert_not_called()
        key.assert_not_called()
        self.assertTrue(self.session.automation_input_blocked)
        self.assertEqual(self.session.status['safe_exit'], 'protected_recovery_required_no_game_input')

    def test_automatic_capture_failure_blocks_game_input_and_requests_protected_recovery(self):
        self.session.cfg.update(capture_hold_seconds=60, scenario='menu')
        (self.out / 'capture-failed.json').write_text(json.dumps(dict(
            error='automatic capture failed')))
        with mock.patch.object(runner.desktop, 'ensure_foreground', return_value={}), \
                mock.patch.object(runner.desktop, 'same_process', return_value=True):
            with self.assertRaisesRegex(RuntimeError, 'capture_automatic_failed'):
                self.session.capture_hold()
        self.assertTrue(self.session.automation_input_blocked)
        with mock.patch.object(runner.desktop, 'same_process', return_value=True), \
                mock.patch.object(runner.desktop, 'key') as key:
            self.session.shutdown()
        key.assert_not_called()
        self.assertEqual(self.session.status['safe_exit'], 'protected_recovery_required_no_game_input')

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
