"""Bounded real-game smoke tests. All receipts are local; no automatic upload."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import traceback
from datetime import datetime, timezone
import winreg
from contextlib import contextmanager
import msvcrt
from PIL import Image, ImageDraw, ImageFont, ImageChops, ImageStat
import desktop
import normal_process

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
PS = ['pwsh.exe', '-NoProfile', '-NonInteractive', '-Command']

def local_output_root(path):
    path = Path(path).resolve()
    if not any(path.is_relative_to((ROOT / name).resolve()) for name in ('tmp', 'artifacts')):
        raise ValueError('Output must remain below this repository tmp/ or artifacts/')
    path.mkdir(parents=True, exist_ok=True)
    return path

def stamp():
    return datetime.now(timezone.utc).isoformat()

def parse_capture_hold_seconds(value):
    try:
        seconds = int(value)
    except (TypeError, ValueError) as error:
        raise argparse.ArgumentTypeError('must be 0 or an integer from 10 to 900') from error
    if seconds != 0 and not 10 <= seconds <= 900:
        raise argparse.ArgumentTypeError('must be 0 or an integer from 10 to 900')
    return seconds

def validate_capture_hold_scenario(scenario, seconds):
    if seconds and scenario not in ('smoke', 'menu'):
        raise ValueError('--capture-hold-seconds is only supported for smoke or menu scenarios')

def _unique_json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate JSON object key')
        result[key] = value
    return result

def _reject_json_constant(value):
    raise ValueError('Invalid JSON constant: ' + value)

def capture_complete(out, pid):
    try:
        signal = json.loads((out / 'capture-complete.json').read_text(encoding='utf-8'),
                            object_pairs_hook=_unique_json_object, parse_constant=_reject_json_constant)
    except (OSError, UnicodeError, ValueError):
        return False
    return (isinstance(signal, dict) and type(signal.get('pid')) is int
            and signal['pid'] == pid and signal.get('completed') is True)

def dump(path, value):
    temp = Path(str(path) + '.tmp')
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')
    os.replace(temp, path)

def sha(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def resolve_target(game, platform, save, addon_absent_control=False, target_path=None):
    if not re.fullmatch(r'[a-z0-9-]+', game) or not re.fullmatch(r'[a-z0-9-]+', platform):
        raise ValueError('Invalid target name')
    target_path = target_path or ROOT / '.local/game-test' / f'{game}-{platform}.json'
    cfg = json.loads(Path(target_path).read_text(encoding='utf-8'))
    if cfg['game'] != game or cfg['platform'] != platform:
        raise ValueError('Target identity does not match the selected adapter')
    if game != 'witcher3':
        raise ValueError('Only the existing Witcher adapter is supported')
    if not cfg.get('scene_patterns'):
        raise ValueError('Configure scene_patterns in the local target before use')
    if platform != 'steam':
        raise ValueError('Platform adapter has not been implemented or validated')
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as key:
        steam = Path(winreg.QueryValueEx(key, 'SteamPath')[0])
    libraries = re.findall(r'"path"\s+"([^"]+)"', (steam / 'steamapps/libraryfolders.vdf').read_text(encoding='utf-8'))
    matches = []
    for library in libraries:
        lib = Path(library.replace('\\\\', '\\'))
        manifest = lib / f'steamapps/appmanifest_{cfg["app_id"]}.acf'
        if manifest.exists():
            text = manifest.read_text(encoding='utf-8')
            install = re.search(r'"installdir"\s+"([^"]+)"', text).group(1)
            if install != cfg['install_dir']:
                raise RuntimeError('Unexpected Steam install directory')
            matches.append((lib / 'steamapps/common' / install, manifest))
    if len(matches) != 1:
        raise RuntimeError(f'Expected one installed target; found {len(matches)}')
    install, manifest = matches[0]
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders') as key:
        documents = Path(os.path.expandvars(winreg.QueryValueEx(key, 'Personal')[0]))
    profile = documents / cfg['documents_dir']
    save = save or cfg['default_save']
    if Path(save).name != save or not save.endswith('.sav'):
        raise ValueError('Save must be a filename within the target save directory')
    cfg.update(steam=str(steam / 'steam.exe'), install=str(install), executable=str(install / cfg['exe']),
               profile=str(profile), save=save, manifest_sha256=sha(manifest))
    for path in (cfg['steam'], cfg['executable'], profile / cfg['save_dir'] / save):
        if not Path(path).is_file():
            raise FileNotFoundError(path)
    cfg['save_sha256'] = sha(profile / cfg['save_dir'] / save)
    cfg['save_modified'] = datetime.fromtimestamp((profile / cfg['save_dir'] / save).stat().st_mtime).isoformat()
    selected_time = datetime.fromisoformat(cfg['save_modified']).replace(microsecond=0)
    collisions = [p.name for p in (profile / cfg['save_dir']).glob('*.sav')
                  if datetime.fromtimestamp(p.stat().st_mtime).replace(microsecond=0) == selected_time]
    if collisions != [save]:
        raise RuntimeError('Save timestamp is not unique; UI identity cannot safely select this file')
    cfg['addon_absent_control'] = addon_absent_control
    if addon_absent_control:
        if (Path(cfg['executable']).parent / cfg['required_module']).exists():
            raise RuntimeError('Baseline control requires the add-on file to be absent')
        cfg['addon_sha256'] = None
    else:
        cfg['addon_sha256'] = sha(Path(cfg['executable']).parent / cfg['required_module'])
    return cfg

def process_inventory(exe):
    # Query every matching process, including those without a window; never adopt a prior session.
    name = Path(exe).name
    if not re.fullmatch(r'[A-Za-z0-9_.-]+', name):
        raise ValueError('Invalid executable filename')
    command = "Get-CimInstance Win32_Process -Filter \"Name='" + name + "'\" | Select-Object ProcessId,ExecutablePath,CreationDate,CommandLine | ConvertTo-Json -Compress"
    result = subprocess.run(PS + [command], capture_output=True, encoding='utf-8', timeout=20, check=True)
    if not result.stdout.strip():
        return []
    data = json.loads(result.stdout)
    return data if isinstance(data, list) else [data]

def crash_reports():
    root = Path(os.environ['LOCALAPPDATA']) / 'REDEngine/ReportQueue'
    return {p.name: p for p in root.glob('Witcher3_*') if p.is_dir()}

def collect_crashes(before, out):
    fresh = {name: path for name, path in crash_reports().items() if name not in before}
    for name, source in fresh.items():
        dest = out / 'crashes' / name
        dest.mkdir(parents=True, exist_ok=True)
        for item in source.iterdir():
            if item.is_file() and item.stat().st_size < 128 * 1024 * 1024:
                shutil.copyfile(item, dest / item.name)
    return list(fresh)

def freeze_logs(cfg, out):
    out.mkdir(exist_ok=True)
    records = []
    for name in cfg['logs']:
        source = Path(cfg['executable']).parent / name
        if source.exists():
            try:
                with source.open('rb') as stream:
                    content = stream.read(32 * 1024 * 1024)
                (out / name).write_bytes(content)
                records.append(dict(name=name, bytes=len(content), sha256=hashlib.sha256(content).hexdigest(),
                                    truncated=source.stat().st_size > len(content)))
            except OSError as error:
                records.append(dict(name=name, error=str(error)))
    dump(out / 'files.json', records)

class Session:
    def __init__(self, cfg, out):
        self.cfg, self.out, self.window, self.number = cfg, out, None, 0
        self.crashes_before = crash_reports()
        self.exit_requested = False
        self.ui_state = 'starting'
        crash_log = Path(cfg['executable']).parent / 'RenoDX-DLSS5-crash.log'
        self.log_offset = crash_log.stat().st_size if crash_log.exists() else 0
        self.status = dict(started=stamp(), startup='not_tested', save_load='not_tested', mod_load='not_tested',
                           functionality='not_tested', visual_quality='not_tested', stability='not_tested', safe_exit='not_tested')

    def event(self, kind, **data):
        with (self.out / 'events.jsonl').open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(dict(time=stamp(), kind=kind, **data), ensure_ascii=False) + '\n')

    def observe(self, label='observe'):
        if not desktop.same_process(self.window):
            raise RuntimeError('unexpected_exit')
        desktop.activate(self.window)
        self.number += 1
        path = self.out / 'latest.png'
        metrics = desktop.capture(self.window, path)
        region = {
            'main_menu': (.08, .31, .27, .73), 'pause_menu': (.08, .31, .27, .73),
            'load_menu': (.29, .15, .84, .80), 'scene': (.78, .25, .995, .37),
            'loading': (.78, .25, .995, .37), 'exit_confirm': (.35, .35, .65, .50),
        }.get(self.ui_state)
        with Image.open(path) as img:
            # Preserve text detail in small UI crops; full-screen OCR remains bounded to 1920 pixels.
            scale = min(1, 1920 / (img.width * (region[2]-region[0]) if self.ui_state in ('scene', 'loading') else img.width))
            ocr_image = img.resize((round(img.width * scale), round(img.height * scale)))
        offset_x = offset_y = 0
        if region:
            # Isolate UI text from animated backgrounds/overlays which distort OCR's angle estimate.
            box = tuple(round(n * (ocr_image.width if i % 2 == 0 else ocr_image.height)) for i, n in enumerate(region))
            offset_x, offset_y = box[:2]
            ocr_image = ocr_image.crop(box)
        ocr_image.save(self.out / 'ocr.png')
        result = subprocess.run([self.cfg['ocr_executable'], str(self.out / 'ocr.png')], capture_output=True, timeout=25)
        if result.returncode:
            raise RuntimeError('OCR failed: ' + result.stderr.decode('utf-8', errors='replace'))
        data = json.loads(result.stdout.decode('utf-8-sig'))
        for line in data['lines']:
            for word in line['words']:
                word['x'] += offset_x
                word['y'] += offset_y
        data.update(metrics=metrics, scale=scale, time=stamp(), label=label)
        dump(self.out / 'observation.json', data)
        self.event('observation', label=label, text=data['text'], metrics=metrics)
        return data

    def evidence(self, label):
        shutil.copyfile(self.out / 'latest.png', self.out / f'{label}.png')
        shutil.copyfile(self.out / 'observation.json', self.out / f'{label}.json')

    def key(self, name, seconds=.2):
        desktop.key(self.window, name, seconds)
        self.event('key', key=name, seconds=seconds, state=self.ui_state)

    def main_menu(self, timeout=None):
        deadline = time.monotonic() + (timeout or self.cfg['startup_timeout'])
        last_skip = 0
        while time.monotonic() < deadline:
            obs = self.observe('startup')
            text = compact(obs['text'])
            if '读取游戏' in text and '退出游戏' in text and '新游戏' in text:
                self.ui_state = 'main_menu'
                self.status['startup'] = 'main_menu_confirmed'
                self.evidence('main-menu')
                return obs
            if '跳过' in text and time.monotonic() - last_skip > 3:
                self.key('SPACE')
                last_skip = time.monotonic()
            time.sleep(1)
        raise TimeoutError('main_menu_timeout')

    def select_menu(self, label, labels):
        # Keyboard selection is accepted only after the observed focus rectangle moves.
        unknown_observations = 0
        for _ in range(len(labels) + 1):
            obs = self.observe('menu-focus')
            selected = focused_menu(self.out / 'latest.png', obs, labels)
            self.event('menu_focus', selected=selected, target=label)
            if selected == label:
                self.evidence('selected-' + str(labels.index(label)))
                return obs
            if selected is None:
                unknown_observations += 1
                if unknown_observations >= 3:
                    raise RuntimeError('menu_focus_unknown')
                time.sleep(.5)
                continue
            unknown_observations = 0
            down = (labels.index(label) - labels.index(selected)) % len(labels)
            up = (labels.index(selected) - labels.index(label)) % len(labels)
            step = 'DOWN' if down <= up else 'UP'
            self.key(step)
            time.sleep(.25)
        raise RuntimeError('menu_focus_did_not_move')

    def request_exit(self):
        paused = self.ui_state == 'pause_menu'
        self.select_menu('退出游戏', PAUSE_MENU if paused else MAIN_MENU)
        if paused:
            self.key('ENTER')
            self.ui_state = 'exit_confirm'
            self.wait_text('你确定要退出.*未存储的进度将无法恢复', timeout=10, label='exit-confirm')
        self.ui_state = 'exit_requested'
        self.exit_requested = True
        # Do not take another screenshot: the window can be destroyed before the process exits.
        self.key('E' if paused else 'ENTER')
        self.event('exit_requested', method='game_menu')
        self.wait_exit()

    def wait_exit(self, timeout=45):
        deadline = time.monotonic() + timeout
        while desktop.same_process(self.window) and time.monotonic() < deadline:
            time.sleep(.25)
        if desktop.same_process(self.window):
            raise TimeoutError('menu_exit_timeout')
        self.ui_state = 'exited'
        self.status['safe_exit'] = 'menu_exit_pending_log_and_crash_check'

    def menu(self):
        if self.cfg.get('manual_ime_session'):
            # The user handles startup and every UI action in a manual session.
            # Module/process checks do not require capture or foreground input.
            self.ui_state = 'manual_menu'
            self.status['startup'] = 'process_and_dx12_path_confirmed_user_menu_required'
        else:
            self.main_menu()
        if self.cfg.get('capture_hold_seconds', 0):
            self.check_mod()
            self.capture_hold()

    def check_mod(self):
        paths = desktop.modules(self.window)
        expected = Path(self.cfg['executable']).parent / self.cfg['required_module']
        matches = [p for p in paths if Path(p) == expected]
        if self.cfg.get('addon_absent_control'):
            matches = [p for p in paths if Path(p).name.lower() == expected.name.lower()]
            dump(self.out / 'loaded-modules.json', dict(time=stamp(), process=self.window,
                expected_absent=str(expected), matching_paths=matches, modules=paths))
            if matches or expected.exists():
                raise RuntimeError('baseline_addon_still_present')
            self.status['mod_load'] = 'baseline_addon_absence_confirmed'
            return
        receipt = dict(time=stamp(), process=self.window, expected_path=str(expected),
                       expected_sha256=self.cfg['addon_sha256'], matching_paths=matches,
                       modules=paths)
        if len(matches) == 1:
            receipt['actual_sha256'] = sha(matches[0])
        dump(self.out / 'loaded-modules.json', receipt)
        if len(matches) != 1 or receipt['actual_sha256'] != self.cfg['addon_sha256']:
            raise RuntimeError('required_mod_identity_mismatch')
        self.status['mod_load'] = 'loaded_path_and_disk_hash_confirmed'

    def wait_text(self, pattern, timeout=60, label='wait'):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            obs = self.observe(label)
            if re.search(pattern, re.sub(r'\s+', '', obs['text'])):
                self.evidence(label)
                return obs
            time.sleep(1)
        raise TimeoutError(f'{label}: expected text {pattern!r} not found')

    def click_text(self, obs, text):
        candidates = [line for line in obs['lines'] if text in re.sub(r'\s+', '', line['text'])]
        if len(candidates) != 1:
            raise RuntimeError(f'OCR target must be unique: {text}, matches={len(candidates)}')
        words = candidates[0]['words']
        x = (min(w['x'] for w in words) + max(w['x']+w['width'] for w in words))/2/obs['scale']
        y = (min(w['y'] for w in words) + max(w['y']+w['height'] for w in words))/2/obs['scale']
        desktop.click(self.window, x, y)
        self.event('click_text', text=text, x=x, y=y)

    def launch(self):
        subprocess.Popen([self.cfg['steam']] + self.cfg['launch_args'], creationflags=0x08000000)
        deadline = time.monotonic() + self.cfg['startup_timeout']
        while time.monotonic() < deadline:
            found = desktop.windows(self.cfg['executable'])
            if len(found) > 1:
                raise RuntimeError('Multiple target windows; refusing input')
            if found:
                self.window = found[0]
                dump(self.out / 'owned-process.json', self.window)
                self.status['startup'] = 'process_and_dx12_path_confirmed'
                self.event('window', **self.window)
                return
            if self.window is None:
                candidates = [p for p in process_inventory(self.cfg['executable'])
                              if p.get('ExecutablePath') and Path(p['ExecutablePath']) == Path(self.cfg['executable'])]
                if len(candidates) > 1:
                    raise RuntimeError('Multiple newly launched processes; ownership ambiguous')
                if candidates:
                    pid = candidates[0]['ProcessId']
                    metrics = desktop.process_metrics(pid)
                    if metrics:
                        self.window = dict(pid=pid, created=metrics['created'], path=candidates[0]['ExecutablePath'], hwnd=0)
                        dump(self.out / 'owned-process.json', self.window)
            time.sleep(1)
        raise TimeoutError('startup_timeout: Steam did not produce a DX12 game window')

    def calibrate(self):
        # First-adapter commissioning only: bounded, local command mailbox, no arbitrary shell commands.
        self.main_menu()
        print('Calibration ready: ' + str(self.out), flush=True)
        deadline = time.monotonic() + 300
        while time.monotonic() < deadline:
            command = self.out / 'command.json'
            if not command.exists():
                time.sleep(.5)
                continue
            request = json.loads(command.read_text(encoding='utf-8-sig'))
            command.unlink()
            action = request['action']
            if action == 'exit':
                return
            if action == 'menu_exit':
                self.request_exit()
                return
            if action == 'load_menu':
                self.select_menu('读取游戏', MAIN_MENU)
                self.key('ENTER')
                self.ui_state = 'load_menu'
                time.sleep(.5)
            elif action == 'load_save':
                if self.ui_state != 'load_menu':
                    raise ValueError('Calibration save loading requires the observed load menu')
                self.check_mod()
                self.load_save()
                self.ui_state = 'loading'
            elif action == 'pause_menu':
                if self.ui_state not in ('loading', 'scene'):
                    raise ValueError('Calibration pause requires a requested scene load')
                self.key('ESC')
                self.ui_state = 'pause_menu'
                self.wait_text('读取游戏.*教程.*返回主菜单.*退出游戏', timeout=10, label='calibration-pause-menu')
            elif action == 'key':
                self.key(request['key'], request.get('seconds', .08))
            elif action == 'click_text':
                self.click_text(self.observe(), request['text'])
            elif action != 'observe':
                raise ValueError('Unknown calibration action')
            self.observe(action)
            self.evidence(f'calibration-{self.number}')

    def smoke(self):
        self.main_menu()
        self.check_mod()
        self.select_menu('读取游戏', MAIN_MENU)
        self.key('ENTER')
        self.ui_state = 'load_menu'
        self.load_save()
        self.ui_state = 'loading'
        self.wait_scene()
        self.status['save_load'] = 'unique_file_timestamp_selected_and_scene_confirmed'
        self.status['visual_quality'] = 'not_tested_screenshots_only'
        self.evidence('scene-loaded')
        self.capture_hold()
        self.scene_health()
        self.key('ESC')
        self.ui_state = 'pause_menu'
        self.select_menu('继续', PAUSE_MENU)
        self.evidence('pause-menu')
        # Escape resumes the observed pause menu even when OCR inferred the
        # focused row from its neighbours and did not recognize its own text.
        self.key('ESC')
        self.ui_state = 'scene'
        self.wait_scene(timeout=20)
        self.status['functionality'] = 'pause_and_resume_input_response_confirmed_mod_features_not_tested'

    def capture_hold(self):
        seconds = self.cfg.get('capture_hold_seconds', 0)
        if not seconds:
            return
        validate_capture_hold_scenario(self.cfg.get('scenario'), seconds)
        identity = dict(self.window)
        dump(self.out / 'capture-ready.json', dict(time=stamp(), pid=self.window['pid'], identity=identity))
        self.status['capture_hold'] = 'waiting'
        self.event('capture_hold_ready', pid=self.window['pid'], seconds=seconds, identity=identity)
        deadline = time.monotonic() + seconds
        while True:
            if not desktop.same_process(self.window):
                self.status['capture_hold'] = 'process_exited'
                self.event('capture_hold_process_exit', pid=self.window['pid'])
                raise RuntimeError('capture_hold_process_exited')
            if capture_complete(self.out, self.window['pid']):
                self.status['capture_hold'] = 'completed_by_signal'
                self.event('capture_hold_complete', pid=self.window['pid'])
                return
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                if self.cfg.get('manual_ime_session'):
                    if self.status['capture_hold'] != 'deadline_waiting_for_user':
                        self.status['capture_hold'] = 'deadline_waiting_for_user'
                        self.event('manual_ime_deadline', state='protection_held_no_automatic_game_input')
                        dump(self.out / 'manual-intervention-required.json', dict(
                            time=stamp(), pid=self.window['pid'], state='finish_editing_then_request_manual_exit'))
                    time.sleep(1)
                    continue
                self.status['capture_hold'] = 'timeout_not_mod_acceptance'
                self.event('capture_hold_timeout', pid=self.window['pid'], seconds=seconds,
                           result='timeout_not_mod_acceptance')
                return
            time.sleep(min(1, remaining))

    def load_save(self):
        expected = datetime.fromisoformat(self.cfg['save_modified']).replace(microsecond=0).isoformat()
        deadline = time.monotonic() + 60
        seen = set()
        clicked_target = False
        for _ in range(self.cfg.get('max_save_steps', 40)):
            if time.monotonic() > deadline:
                break
            obs = self.observe('save-list')
            rows = save_rows(self.out / 'latest.png', obs)
            selected = [row for row in rows if row['selected']]
            if len(selected) != 1:
                time.sleep(.5)
                continue
            current = selected[0]['timestamp']
            self.event('save_selection', expected=expected, current=current, rows=rows)
            if current == expected:
                self.evidence('selected-save')
                dump(self.out / 'save-selection.json', dict(time=stamp(), filename=self.cfg['save'], sha256=self.cfg['save_sha256'],
                                                           timestamp=expected, selected=selected[0], basis='unique original-file mtime, UI timestamp, observed focus frame'))
                self.key('ENTER')
                return
            target = [row for row in rows if row['timestamp'] == expected]
            if len(target) > 1:
                raise RuntimeError('ambiguous_ui_save_timestamp')
            if current in seen:
                if target and not clicked_target:
                    # Mouse focus can remain on the left main menu while
                    # the save panel is visible. Re-observe its selected
                    # timestamp after one unique-row click before loading.
                    self.click_text(obs, re.sub(r'\s+', '', target[0]['text']))
                    clicked_target = True
                    time.sleep(.5)
                    continue
                raise RuntimeError('save_selection_did_not_move')
            seen.add(current)
            self.key('UP' if target and target[0]['y'] < selected[0]['y'] else 'DOWN')
            time.sleep(.5)
        raise TimeoutError('save_selection_timeout')

    def wait_scene(self, timeout=None):
        deadline = time.monotonic() + (timeout or self.cfg['load_timeout'])
        consecutive = 0
        while time.monotonic() < deadline:
            obs = self.observe('scene-wait')
            text = compact(obs['text'])
            if all(re.search(pattern, text) for pattern in self.cfg['scene_patterns']):
                consecutive += 1
                if consecutive >= 2:
                    self.ui_state = 'scene'
                    return obs
            else:
                consecutive = 0
            time.sleep(1)
        raise TimeoutError('scene_load_timeout')

    def scene_health(self):
        samples, last, black_since = [], None, None
        deadline = time.monotonic() + self.cfg.get('scene_seconds', 8)
        while time.monotonic() < deadline:
            obs = self.observe('scene-health')
            with Image.open(self.out / 'latest.png') as img:
                scene = img.crop((img.width*.3, img.height*.3, img.width*.75, img.height*.9)).resize((160, 90)).convert('L')
            stat = ImageStat.Stat(scene)
            delta = ImageStat.Stat(ImageChops.difference(scene, last)).mean[0] if last else None
            sample = dict(time=stamp(), mean=stat.mean[0], std=stat.stddev[0], delta=delta,
                          process=desktop.process_metrics(self.window['pid']), hung=bool(desktop.u.IsHungAppWindow(self.window['hwnd'])))
            samples.append(sample)
            last = scene
            black_since = (black_since or time.monotonic()) if stat.mean[0] < 3 and stat.stddev[0] < 3 else None
            dump(self.out / 'scene-health.json', samples)
            if black_since and time.monotonic() - black_since > 5:
                raise RuntimeError('persistent_black_scene')
            time.sleep(1)
        if any(s['hung'] or s['process'] is None for s in samples):
            raise RuntimeError('scene_unresponsive')
        if not any((s['delta'] or 0) > .05 for s in samples):
            raise RuntimeError('suspected_frozen_scene')
        self.evidence('scene-observed')
        self.status['stability'] = 'bounded_scene_responsive_and_changing'

    def shutdown(self):
        if not self.window:
            self.status['safe_exit'] = 'no_owned_window'
            return
        if self.exit_requested:
            self.wait_exit()
            return
        if self.cfg.get('manual_ime_session'):
            # Completion is the owner's explicit request to end the session.
            # The owner closes native/overlay windows and exits from the game
            # menu; this path sends no keys and never terminates the game.
            self.exit_requested = self.status.get('capture_hold') == 'completed_by_signal'
            self.event('manual_exit_wait', requested=self.exit_requested, pid=self.window['pid'])
            dump(self.out / 'manual-exit-ready.json', dict(
                time=stamp(), pid=self.window['pid'], requested=self.exit_requested))
            while desktop.same_process(self.window):
                time.sleep(.5)
            if not self.exit_requested:
                self.status['safe_exit'] = 'manual_exit_without_recorded_request'
                raise RuntimeError('Manual game exit preceded the completion request')
            self.event('exit_requested', method='user_game_menu')
            self.wait_exit(timeout=0)
            return
        if not desktop.same_process(self.window):
            self.status['safe_exit'] = 'unexpected_exit'
            raise RuntimeError('Game exited without a recorded menu exit request')
        try:
            if self.ui_state == 'scene':
                self.key('ESC')
                self.ui_state = 'pause_menu'
            if self.ui_state == 'pause_menu':
                self.request_exit()
                return
            if self.ui_state == 'load_menu':
                self.key('ESC')
                self.ui_state = 'main_menu'
                self.wait_text('读取游戏', timeout=10, label='return-main-menu')
            obs = self.observe('before-exit')
            text = compact(obs['text'])
            if '读取游戏' in text and '新游戏' in text and '退出游戏' in text:
                self.request_exit()
                return
            raise RuntimeError('No commissioned exit route for current screen')
        finally:
            if desktop.same_process(self.window):
                freeze_logs(self.cfg, self.out / 'recovery-logs')
                self.status['safe_exit'] = 'failed_required_termination'
                desktop.terminate(self.window)
                self.event('recovery', method='owned_process_termination', reason='menu_exit_unavailable')


def compact(text):
    return re.sub(r'\s+', '', text)


MAIN_MENU = ['继续', '新游戏', '读取游戏', '设置', '游戏模组', '更新亮点', '制作人员', '退出游戏']
PAUSE_MENU = ['继续', '存储游戏', '读取游戏', '设置', '教程', '返回主菜单', '退出游戏']


def frame_at(img, left, right, y, half_height=.035, threshold=125):
    above, below = [], []
    for yy in range(max(0, round(y-img.height*half_height)), min(img.height, round(y+img.height*half_height))):
        if abs(yy-y) < img.height*.012:
            continue
        pixels = [img.getpixel((xx, yy)) for xx in range(max(0, round(left)), min(img.width, round(right))) ]
        (above if yy < y else below).append(sum(min(p) > threshold and max(p)-min(p) < 35 for p in pixels)/max(1,len(pixels)))
    return bool(above and below and min(max(above), max(below)) > .8)


def save_rows(image, obs):
    with Image.open(image) as raw:
        img = raw.resize((round(raw.width*obs['scale']), round(raw.height*obs['scale']))).convert('RGB')
    rows = []
    pattern = r'(\d{4})年(\d{1,2})月(\d{1,2})日(\d{1,2})[：:.．](\d{2})[：:.．](\d{2})'
    for line in obs['lines']:
        match = re.search(pattern, compact(line['text']))
        if not match:
            continue
        timestamp = datetime(*map(int, match.groups())).isoformat()
        words = line['words']
        y = (min(w['y'] for w in words)+max(w['y']+w['height'] for w in words))/2
        rows.append(dict(timestamp=timestamp, y=y, text=line['text'],
                         selected=frame_at(img, img.width*.31, img.width*.81, y, threshold=90)))
    return rows


def focused_menu(image, obs, labels):
    """Find the white focus rectangle around a known OCR menu row; reject ambiguity."""
    with Image.open(image) as raw:
        img = raw.resize((round(raw.width * obs['scale']), round(raw.height * obs['scale']))).convert('RGB')
    anchors = []
    for index, label in enumerate(labels):
        lines = [line for line in obs['lines'] if compact(line['text']) == label]
        if len(lines) != 1:
            continue
        words = lines[0]['words']
        x = (min(w['x'] for w in words) + max(w['x'] + w['width'] for w in words)) / 2
        y = (min(w['y'] for w in words) + max(w['y'] + w['height'] for w in words)) / 2
        anchors.append((index, x, y))
    if len(anchors) < 3:
        return None
    n = len(anchors)
    mean_i, mean_y = sum(a[0] for a in anchors)/n, sum(a[2] for a in anchors)/n
    pitch = sum((i-mean_i)*(y-mean_y) for i, x, y in anchors) / sum((i-mean_i)**2 for i, x, y in anchors)
    origin, x = mean_y - pitch * mean_i, sum(a[1] for a in anchors)/n
    if not img.height * .035 < pitch < img.height * .055 or any(abs(y-origin-i*pitch) > img.height*.007 for i, _, y in anchors):
        return None
    matches = []
    for index, label in enumerate(labels):
        y = origin + index * pitch
        # Infer rows only from a consistent observed menu layout; the pointer can occlude the selected text.
        left, right = max(0, round(x - img.width * .078)), min(img.width, round(x + img.width * .078))
        above, below = [], []
        for yy in range(max(0, round(y - img.height * .035)), min(img.height, round(y + img.height * .035))):
            if abs(yy - y) < img.height * .012:
                continue
            pixels = [img.getpixel((xx, yy)) for xx in range(left, right)]
            (above if yy < y else below).append(sum(min(p) > 100 and max(p)-min(p) < 35 for p in pixels) / max(1, len(pixels)))
        if above and below and min(max(above), max(below)) > .8:
            matches.append(label)
    return matches[0] if len(matches) == 1 else None

def worker(out):
    cfg = json.loads((out / 'target.json').read_text(encoding='utf-8'))
    session = Session(cfg, out)
    try:
        session.launch()
        getattr(session, cfg['scenario'])()
        session.status['result'] = 'calibrated' if cfg['scenario'] == 'calibrate' else 'passed'
    except Exception as error:
        session.status.update(result='failed', error=str(error), traceback=traceback.format_exc())
    finally:
        try:
            session.shutdown()
        except Exception as error:
            session.status.update(result='failed', exit_error=str(error))
        try:
            freeze_logs(cfg, out / 'logs')
            crashes = collect_crashes(session.crashes_before, out)
        except Exception as error:
            crashes = []
            session.status.update(result='failed', diagnostic_error=str(error))
        if session.status['safe_exit'] == 'menu_exit_pending_log_and_crash_check':
            # Process disappearance alone also occurs on a D3D crash. Require this PID's detach receipt.
            log = out / 'logs/RenoDX-DLSS5-crash.log'
            content = log.read_bytes() if log.exists() else b''
            content = content[session.log_offset:] if len(content) >= session.log_offset else content
            content = content.decode('utf-8', errors='replace')
            pid = session.window['pid']
            if cfg.get('addon_absent_control'):
                # This diagnostic has no NR add-on to produce a detach marker.
                # Preserve the distinct exit evidence instead of claiming one.
                session.status['safe_exit'] = 'baseline_menu_exit_without_addon_detach_marker'
            elif re.search(r'\[exit\][^\n]*pid=' + str(pid) + r'\b[^\n]*process-exit', content):
                session.status['safe_exit'] = 'menu_exit_and_detach_confirmed'
            else:
                session.status.update(result='failed', safe_exit='exit_log_unconfirmed')
        session.status['finished'] = stamp()
        if crashes:
            session.status.update(result='failed', safe_exit='failed_crash_report_created', crashes=crashes)
        dump(out / 'result.json', session.status)
    return 0 if session.status['result'] != 'failed' else 1

def recover_owned(cfg, out, owned):
    """Do not release original-file locks while the owned game may still be alive."""
    if not owned:
        return False
    try:
        if not desktop.same_process(owned):
            return False
        freeze_logs(cfg, out / 'recovery-logs')
        if cfg.get('manual_ime_session'):
            raise RuntimeError('Manual IME session requires user exit; automatic game termination disabled')
        desktop.terminate(owned)
        if desktop.same_process(owned):
            raise RuntimeError('Owned process survived termination')
    except Exception as error:
        dump(out / 'intervention-required.json', dict(time=stamp(), process=owned, error=str(error),
                                                     state='waiting_with_original_file_locks_held'))
        print('HUMAN_INTERVENTION_REQUIRED=' + str(out), flush=True)
        # Permission failures are unknown state, not proof of exit. Keep protection until it is known.
        while True:
            try:
                if not desktop.same_process(owned):
                    break
            except OSError:
                pass
            time.sleep(2)
        dump(out / 'intervention-resolved.json', dict(time=stamp(), owned_process_exited=True))
    return True


def run_attempt(cfg, out):
    from guard import ProtectedFiles
    out.mkdir(parents=True)
    dump(out / 'target.json', cfg)
    source = out / 'source'
    source.mkdir()
    sources = []
    for name in ('runner.py', 'desktop.py', 'guard.py', 'normal_process.py'):
        shutil.copyfile(HERE / name, source / name)
        sources.append(dict(name=name, sha256=sha(source / name)))
    dump(out / 'source-manifest.json', dict(files=sources, target_sha256=sha(out / 'target.json'),
                                          ocr_sha256=sha(cfg['ocr_executable']) if cfg.get('ocr_executable') else None))
    print('RUN_DIRECTORY=' + str(out), flush=True)
    profile = Path(cfg['profile'])
    protected = [p for p in (profile / cfg['save_dir']).iterdir() if p.is_file()]
    protected += list(profile.glob('*.settings*'))
    protected += list(Path(cfg['executable']).parent.glob('*.ini'))
    child, owned, problem, guard_ok = None, None, None, False
    crashes_before = crash_reports()
    final = dict(result='failed', started=stamp())
    try:
        with ProtectedFiles(protected, out / 'protected') as guard:
            dump(out / 'protected-files.json', guard.manifest)
            freeze_logs(cfg, out / 'logs-before')
            try:
                with (out / 'worker.log').open('w', encoding='utf-8') as log:
                    child = subprocess.Popen([sys.executable, '-B', str(source / 'runner.py'), '--worker', str(out)], stdout=log, stderr=log, creationflags=0x08000000)
                    started, hung_since = time.monotonic(), None
                    capture_ready_announced = False
                    while child.poll() is None:
                        if (cfg.get('capture_hold_seconds', 0) and not capture_ready_announced
                                and (out / 'capture-ready.json').exists()):
                            print('CAPTURE_READY=' + str(out), flush=True)
                            capture_ready_announced = True
                        ownership = out / 'owned-process.json'
                        if ownership.exists():
                            owned = json.loads(ownership.read_text(encoding='utf-8'))
                            alive = desktop.same_process(owned)
                            hung = alive and desktop.window_alive(owned) and bool(desktop.u.IsHungAppWindow(owned['hwnd']))
                            hung_since = (hung_since or time.monotonic()) if hung else None
                            with (out / 'monitor.jsonl').open('a', encoding='utf-8') as monitor:
                                monitor.write(json.dumps(dict(time=stamp(), alive=alive, hung=hung, process=desktop.process_metrics(owned['pid'])))+'\n')
                            if hung_since and time.monotonic() - hung_since > 30:
                                problem = 'persistent_unresponsive'
                        if time.monotonic() - started > cfg['run_timeout']:
                            problem = 'run_timeout'
                        if problem:
                            freeze_logs(cfg, out / 'watchdog-logs')
                            dump(out / 'watchdog.json', dict(time=stamp(), reason=problem))
                            break
                        time.sleep(2)
            finally:
                # Keep file locks held through recovery, even if the worker or monitor raises.
                try:
                    if child and child.poll() is None:
                        try:
                            child.terminate()
                            child.wait(10)
                        except Exception as error:
                            problem = problem or 'worker_stop_failed'
                            dump(out / 'worker-stop-error.json', dict(time=stamp(), error=str(error)))
                            child.kill()
                            try:
                                child.wait(10)
                            except subprocess.TimeoutExpired:
                                pass
                finally:
                    if child and child.poll() is None:
                        dump(out / 'worker-intervention-required.json', dict(time=stamp(), worker_pid=child.pid,
                                                                            state='waiting_with_original_file_locks_held'))
                        print('HUMAN_INTERVENTION_REQUIRED=' + str(out), flush=True)
                        while child.poll() is None:
                            time.sleep(2)
                    ownership = out / 'owned-process.json'
                    if ownership.exists():
                        owned = json.loads(ownership.read_text(encoding='utf-8'))
                    if recover_owned(cfg, out, owned):
                        problem = problem or 'worker_left_owned_process'
                        dump(out / 'recovery.json', dict(time=stamp(), reason=problem, method='owned_process_termination_or_manual_recovery'))
        guard_ok = True
    except BaseException as error:
        problem = problem or str(error)
        final.update(parent_error=str(error), parent_traceback=traceback.format_exc())
    finally:
        if (out / 'result.json').exists():
            try:
                data = json.loads((out / 'result.json').read_text(encoding='utf-8'))
                if not isinstance(data, dict) or data.get('result') not in ('passed', 'failed', 'calibrated'):
                    raise ValueError('Invalid worker result schema')
                final.update(data)
            except (OSError, ValueError) as error:
                problem = problem or 'invalid_worker_result'
                final['worker_receipt_error'] = str(error)
        elif not problem:
            problem = 'worker_failed_without_result'
        try:
            crashes = collect_crashes(crashes_before, out)
            freeze_logs(cfg, out / 'final-logs')
            if crashes:
                problem = problem or 'crash_report_created'
                final['crashes'] = crashes
        except Exception as error:
            problem = problem or 'diagnostic_collection_failed'
            final['diagnostic_error'] = str(error)
        final.update(guard='verified_original_hashes_and_released' if guard_ok else 'failed', finished=stamp())
        if problem or not guard_ok or (child and child.returncode):
            final.update(result='failed', parent_problem=problem)
        dump(out / 'guard-complete.json', dict(time=stamp(), verified=guard_ok, owned_process_exited=not owned or not desktop.same_process(owned)))
        dump(out / 'result.json', final)
    return final


def should_retry(final, scenario):
    if (scenario == 'calibrate' or final.get('guard') != 'verified_original_hashes_and_released'
            or final.get('crashes') or final.get('diagnostic_error')):
        return False
    if final.get('parent_problem'):
        return final['parent_problem'] in ('run_timeout', 'persistent_unresponsive')
    return final.get('error') in (
        'main_menu_timeout', 'startup_timeout: Steam did not produce a DX12 game window',
        'scene_load_timeout', 'save_selection_timeout', 'persistent_black_scene', 'suspected_frozen_scene',
        'Target cannot acquire foreground; no input sent')


def run_trials(cfg, out, retries):
    if retries not in range(3):
        raise ValueError('At most two automatic retries are allowed')
    attempts = []
    for number in range(retries + 1):
        if process_inventory(cfg['executable']) or process_inventory('CrashReporter.exe'):
            raise RuntimeError('Existing game or crash reporter; no safe retry')
        attempt_out = out if number == 0 else out / f'retry-{number}'
        final = run_attempt(cfg, attempt_out)
        attempts.append(dict(attempt=number, directory=str(attempt_out), result=final['result'],
                             error=final.get('error'), parent_problem=final.get('parent_problem')))
        dump(out / 'attempts.json', attempts)
        dump(out / 'suite-summary.json', dict(result=final['result'], attempts=attempts, finished=stamp()))
        print(json.dumps(final, ensure_ascii=False, indent=2), flush=True)
        if final['result'] != 'failed':
            return 0
        # Retry diagnosed transient failures only after recovery and file verification complete.
        if cfg.get('manual_ime_session') or not should_retry(final, cfg['scenario']):
            break
    return 1


@contextmanager
def runner_lock():
    directory = ROOT / 'artifacts/game-tests'
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / '.runner.lock').open('a+b') as stream:
        stream.seek(0, 2)
        if not stream.tell():
            stream.write(b'0')
            stream.flush()
        stream.seek(0)
        try:
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError as error:
            raise RuntimeError('Another game-test runner holds the workspace lock') from error
        try:
            yield
        finally:
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--game', default='witcher3')
    parser.add_argument('--platform', default='steam')
    parser.add_argument('--target', type=Path)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'artifacts/game-tests')
    parser.add_argument('--scenario', default='smoke', choices=['smoke', 'menu', 'calibrate'])
    parser.add_argument('--capture-hold-seconds', type=parse_capture_hold_seconds, default=0)
    parser.add_argument('--manual-ime-session', action='store_true')
    parser.add_argument('--save')
    parser.add_argument('--preflight', action='store_true')
    parser.add_argument('--addon-absent-control', action='store_true')
    parser.add_argument('--retries', type=int, choices=range(3), default=2)
    parser.add_argument('--worker', type=Path)
    args = parser.parse_args()
    filesystem_context = normal_process.require_normal_context()
    try:
        validate_capture_hold_scenario(args.scenario, args.capture_hold_seconds)
    except ValueError as error:
        parser.error(str(error))
    if args.worker:
        return worker(args.worker)
    if args.manual_ime_session and (args.scenario != 'menu' or not args.capture_hold_seconds or args.retries != 0):
        parser.error('--manual-ime-session requires menu, nonzero capture hold and --retries 0')
    output_root = local_output_root(args.output_root)
    cfg = resolve_target(args.game, args.platform, args.save, args.addon_absent_control, args.target)
    cfg['scenario'] = args.scenario
    cfg['capture_hold_seconds'] = args.capture_hold_seconds
    cfg['manual_ime_session'] = args.manual_ime_session
    cfg['filesystem_context'] = filesystem_context
    if process_inventory(cfg['executable']):
        raise RuntimeError('A game session already exists; refusing to adopt or stop it')
    if process_inventory(Path(cfg['executable']).parent / 'crashreporter/CrashReporter.exe'):
        raise RuntimeError('An existing crash reporter blocks launch; preserve and dismiss it first')
    ocr_output = output_root / 'ocr-tool'
    build = subprocess.run(['pwsh.exe', '-NoProfile', '-File', str(HERE / 'build-ocr.ps1'),
                            '-OutputDirectory', str(ocr_output)], capture_output=True, text=True, timeout=60, check=True)
    cfg['ocr_executable'] = build.stdout.strip()
    fixture = ocr_output / 'selftest.png'
    img = Image.new('RGB', (600, 100), 'white')
    ImageDraw.Draw(img).text((15, 15), 'Load Game 123', fill='black', font=ImageFont.truetype(str(Path(os.environ['WINDIR']) / 'Fonts/arial.ttf'), 40))
    img.save(fixture)
    check = subprocess.run([cfg['ocr_executable'], str(fixture)], capture_output=True, encoding='utf-8', timeout=25, check=True)
    if '123' not in json.loads(check.stdout)['text']:
        raise RuntimeError('OCR self-test did not recognize the fixture')
    if args.preflight:
        print(json.dumps(cfg, ensure_ascii=False, indent=2))
        return 0
    out = output_root / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + os.urandom(3).hex())
    return run_trials(cfg, out, args.retries)

if __name__ == '__main__':
    if '--worker' in sys.argv:
        sys.exit(main())
    with runner_lock():
        sys.exit(main())
