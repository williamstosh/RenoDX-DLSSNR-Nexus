"""Bounded real-game smoke tests. All receipts are local; no automatic upload."""
import argparse
import hashlib
import json
from itertools import islice
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time
import traceback
import uuid
from datetime import datetime, timezone
import winreg
from contextlib import contextmanager
import msvcrt
from PIL import Image, ImageDraw, ImageFont, ImageChops, ImageStat
import desktop
import normal_process
import crash_evidence
import hang_evidence
import temporary_mod
import witcher_debug
try:
    import output_lifecycle
except ModuleNotFoundError:
    import importlib.util
    _spec = importlib.util.spec_from_file_location("output_lifecycle", Path(__file__).resolve().parents[1] / 'offline/output_lifecycle.py')
    output_lifecycle = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(output_lifecycle)

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

def validate_scenario_save(scenario, save):
    if scenario == 'smoke' and not save:
        raise ValueError('--save is required for every smoke run')

def capture_script_path(path):
    path = Path(path).resolve(strict=True)
    if (not path.is_file() or path.suffix.lower() != '.py'
            or not any(path.is_relative_to((ROOT / name).resolve()) for name in ('tools', 'tmp'))):
        raise ValueError('Capture script must be a Python file below repository tools/ or tmp/')
    return path

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

def capture_script_complete(out, pid):
    try:
        result = json.loads((out / 'capture-script-result.json').read_text(encoding='utf-8'),
                            object_pairs_hook=_unique_json_object, parse_constant=_reject_json_constant)
    except (OSError, UnicodeError, ValueError):
        return False
    return (isinstance(result, dict) and type(result.get('pid')) is int and result['pid'] == pid
            and type(result.get('exit_code')) is int and result['exit_code'] == 0)

def dump(path, value):
    temp = Path(str(path) + '.tmp')
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')
    os.replace(temp, path)

def sha(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def resolve_target(game, platform, save, addon_absent_control=False, target_path=None, control_backend='script'):
    if not re.fullmatch(r'[a-z0-9-]+', game) or not re.fullmatch(r'[a-z0-9-]+', platform):
        raise ValueError('Invalid target name')
    target_path = target_path or ROOT / '.local/game-test' / f'{game}-{platform}.json'
    cfg = json.loads(Path(target_path).read_text(encoding='utf-8'))
    if cfg['game'] != game or cfg['platform'] != platform:
        raise ValueError('Target identity does not match the selected adapter')
    if game != 'witcher3':
        raise ValueError('Only the existing Witcher adapter is supported')
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
    if (Path(save).name != save or not re.fullmatch(r'[A-Za-z0-9_.-]+\.sav', save, re.IGNORECASE)):
        raise ValueError('Save must be a plain ASCII .sav filename within the target save directory')
    cfg.update(steam=str(steam / 'steam.exe'), install=str(install), executable=str(install / cfg['exe']),
               profile=str(profile), save=save, manifest_sha256=sha(manifest))
    if control_backend == 'script':
        debugger_cli = Path(cfg.get('debugger_cli') or ROOT / '.local/game-test/rw3d_cli.exe')
        if not debugger_cli.is_absolute():
            debugger_cli = ROOT / debugger_cli
        if not debugger_cli.is_file():
            raise FileNotFoundError('Place the verified Witcher debugger CLI at ' + str(debugger_cli))
        debugger_cli_sha256 = sha(debugger_cli)
        if debugger_cli_sha256 != witcher_debug.CLI_SHA256:
            raise RuntimeError('Unexpected Witcher debugger CLI SHA-256: ' + debugger_cli_sha256)
        cfg.update(debugger_cli=str(debugger_cli.resolve()), debugger_cli_sha256=debugger_cli_sha256)
    for path in (cfg['steam'], cfg['executable'], profile / cfg['save_dir'] / save):
        if not Path(path).is_file():
            raise FileNotFoundError(path)
    cfg['save_sha256'] = sha(profile / cfg['save_dir'] / save)
    cfg['save_modified'] = datetime.fromtimestamp((profile / cfg['save_dir'] / save).stat().st_mtime).isoformat()
    if control_backend == 'desktop':
        selected_time = datetime.fromisoformat(cfg['save_modified']).replace(microsecond=0)
        collisions = [p.name for p in (profile / cfg['save_dir']).glob('*.sav')
                      if datetime.fromtimestamp(p.stat().st_mtime).replace(microsecond=0) == selected_time]
        if collisions != [save]:
            raise RuntimeError('Save timestamp is not unique; legacy desktop selection cannot safely identify this file')
    elif control_backend != 'script':
        raise ValueError('Unknown Witcher game control backend')
    cfg['addon_absent_control'] = addon_absent_control
    cfg['control_backend'] = control_backend
    if addon_absent_control:
        if (Path(cfg['executable']).parent / cfg['required_module']).exists():
            raise RuntimeError('Baseline control requires the add-on file to be absent')
        cfg['addon_sha256'] = None
    else:
        cfg['addon_sha256'] = sha(Path(cfg['executable']).parent / cfg['required_module'])
    return cfg

def prepare_ocr(cfg, output_root):
    if cfg['control_backend'] not in ('desktop', 'script'):
        raise ValueError('Unknown Witcher game control backend')
    ocr_output = output_root / 'ocr-tool'
    build = subprocess.run(['pwsh.exe', '-NoProfile', '-File', str(HERE / 'build-ocr.ps1'),
                            '-OutputDirectory', str(ocr_output)], capture_output=True, text=True, timeout=60, check=True)
    executable = build.stdout.strip()
    fixture = ocr_output / 'selftest.png'
    img = Image.new('RGB', (600, 100), 'white')
    ImageDraw.Draw(img).text((15, 15), 'Load Game 123', fill='black',
        font=ImageFont.truetype(str(Path(os.environ['WINDIR']) / 'Fonts/arial.ttf'), 40))
    img.save(fixture)
    check = subprocess.run([executable, str(fixture)], capture_output=True, encoding='utf-8', timeout=25, check=True)
    if '123' not in json.loads(check.stdout)['text']:
        raise RuntimeError('OCR self-test did not recognize the fixture')
    return executable

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
    records, budget = [], crash_evidence.Budget()
    if len(fresh) > crash_evidence.MAX_FILES:
        records.append(dict(status='skipped', reason='report_enumeration_limit'))
    for name, source in islice(fresh.items(), crash_evidence.MAX_FILES):
        dest = out / 'crashes' / name
        dest.mkdir(parents=True, exist_ok=True)
        for index, item in enumerate(islice(source.iterdir(), crash_evidence.MAX_FILES + 1)):
            if index == crash_evidence.MAX_FILES:
                records.append(dict(source=str(source), status='skipped', reason='file_enumeration_limit'))
                break
            if item.is_file():
                target = dest / item.name
                try:
                    records.append(budget.copy(item, target))
                except Exception as error:
                    records.append(dict(source=str(item), error=str(error)))
    if records:
        crash_evidence.write_new(out / ('crash-copy-' + uuid.uuid4().hex + '.json'), json.dumps(records, indent=2))
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

def script_log_identity(path):
    try:
        size = path.stat().st_size
        digest = hashlib.sha256()
        with path.open('rb') as stream:
            remaining = size
            while remaining:
                block = stream.read(min(1024 * 1024, remaining))
                if not block:
                    return dict(exists=True, size=size, error='short_read')
                digest.update(block)
                remaining -= len(block)
        return dict(exists=True, size=size, sha256=digest.hexdigest())
    except FileNotFoundError:
        return dict(exists=False)
    except OSError as error:
        return dict(exists=None, error=repr(error))


def script_log_prefix_sha256(path, size):
    digest = hashlib.sha256()
    remaining = size
    try:
        with path.open('rb') as stream:
            while remaining:
                block = stream.read(min(1024 * 1024, remaining))
                if not block:
                    return None
                digest.update(block)
                remaining -= len(block)
    except OSError:
        return None
    return digest.hexdigest()


def stage_script_mod(cfg, source_root):
    if cfg.get('control_backend', 'desktop') != 'script':
        return None, []
    lease = temporary_mod.TemporaryScriptMod(cfg['install'], source_root)
    return lease, lease.install()


class Session:
    def __init__(self, cfg, out):
        self.cfg, self.out, self.window, self.number = cfg, out, None, 0
        self.control_backend = cfg.get('control_backend', 'desktop')
        self.debugger = (witcher_debug.WitcherDebugger(
            cfg.get('debugger_cli', ROOT / '.local/game-test/rw3d_cli.exe'),
            timeout=cfg.get('debug_timeout', 3))
                         if self.control_backend == 'script' else None)
        self.script_bridge_endpoint_recorded = False
        self.last_script_bridge_request = None
        self.script_log_path = None
        self.script_log_baseline = None
        self.crashes_before = crash_reports()
        self.exit_requested = False
        self.automation_input_blocked = False
        self.ui_state = 'starting'
        crash_log = Path(cfg['executable']).parent / 'RenoDX-DLSS5-crash.log'
        self.log_offset = crash_log.stat().st_size if crash_log.exists() else 0
        self.status = dict(started=stamp(), startup='not_tested', save_load='not_tested', mod_load='not_tested',
                           script_mod_reload='not_tested', script_bridge='not_tested',
                           functionality='not_tested', visual_quality='not_tested', stability='not_tested', safe_exit='not_tested')

    def event(self, kind, **data):
        with (self.out / 'events.jsonl').open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(dict(time=stamp(), kind=kind, **data), ensure_ascii=False) + '\n')

    def activate(self):
        try:
            receipt = desktop.ensure_foreground(self.window)
            self.event('automatic_foreground_confirmed', phase='runner_input', **receipt)
        except Exception as error:
            self.automation_input_blocked = True
            self.event('automatic_foreground_failed', phase='runner_input', error=repr(error),
                       diagnostics=getattr(error, 'diagnostics', None))
            raise

    def refresh_startup_window(self):
        """Rebind a recreated startup HWND only while process identity is unchanged."""
        if not self.window or not desktop.same_process(self.window):
            return False
        if desktop.window_alive(self.window):
            return True
        candidates = [window for window in desktop.windows(self.cfg['executable'])
                      if window['pid'] == self.window['pid']
                      and window['created'] == self.window['created']]
        if len(candidates) != 1:
            return False
        previous = dict(self.window)
        self.window = candidates[0]
        self.window['command_line'] = previous.get('command_line')
        dump(self.out / 'owned-process.json', self.window)
        self.event('startup_window_rebound', previous_hwnd=previous['hwnd'],
                   hwnd=self.window['hwnd'], pid=self.window['pid'],
                   created=self.window['created'])
        return True

    def observe(self, label='observe', *, region_state=None, script_startup=False):
        if self.control_backend == 'script' and not script_startup:
            raise RuntimeError('script_backend_disallows_ocr_observation')
        if not desktop.same_process(self.window):
            raise RuntimeError('unexpected_exit')
        capture_hold = self.ui_state == 'capture_hold'
        if not capture_hold:
            self.activate()
        self.number += 1
        path = self.out / 'latest.png'
        if region_state is None and self.ui_state in ('main_menu', 'pause_menu', 'load_menu'):
            try:
                desktop.park_cursor(self.window)
            except Exception:
                self.automation_input_blocked = True
                raise
        region_state = region_state or self.ui_state
        metrics = desktop.capture(self.window, path, activate=not capture_hold)
        region = {
            'main_menu': (.08, .31, .27, .73), 'pause_menu': (.08, .31, .27, .73),
            'load_menu': (.29, .15, .84, .80), 'scene': (.78, .25, .995, .37),
            'loading': (.78, .25, .995, .37), 'exit_confirm': (.35, .35, .65, .50),
        }.get(region_state)
        with Image.open(path) as img:
            # Preserve text detail in small UI crops; full-screen OCR remains bounded to 1920 pixels.
            scale = min(1, 1920 / (img.width * (region[2]-region[0]) if region_state in ('scene', 'loading') else img.width))
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

    def frame_evidence(self, label):
        if not desktop.same_process(self.window):
            raise RuntimeError('unexpected_exit')
        self.number += 1
        path = self.out / 'latest.png'
        metrics = desktop.capture(self.window, path, activate=self.ui_state != 'capture_hold')
        shutil.copyfile(path, self.out / f'{label}.png')
        record = dict(time=stamp(), label=label, backend='script', metrics=metrics,
                      process=dict(self.window))
        dump(self.out / f'{label}.json', record)
        self.event('frame_capture', label=label, metrics=metrics)
        return metrics

    def record_script_bridge_endpoint(self):
        if self.debugger is not None and not self.script_bridge_endpoint_recorded:
            endpoint = witcher_debug.listener_snapshot(self.debugger.port)
            game_pid = self.window.get('pid') if self.window else None
            endpoint.update(time=stamp(), game_pid=game_pid,
                            debugger_cli_sha256=self.cfg.get('debugger_cli_sha256'),
                            game_pid_matches_listener=any(
                                listener['pid'] == game_pid for listener in endpoint['listeners'])
                            if game_pid is not None else None)
            dump(self.out / 'script-bridge-endpoint.json', endpoint)
            event_data = dict(endpoint)
            event_data.pop('time', None)
            self.event('script_bridge_endpoint', **event_data)
            self.script_bridge_endpoint_recorded = True
            if endpoint['game_pid_matches_listener'] is not True:
                raise RuntimeError('script_bridge_listener_not_owned_by_game')

    def bridge_call(self, function, *arguments):
        self.record_script_bridge_endpoint()
        request_id = uuid.uuid4().hex
        self.last_script_bridge_request = dict(function=function, request_id=request_id)
        output = self.debugger.exec_bridge(function, *arguments, request_id)
        invocation = self.debugger.last_invocation
        if invocation is not None:
            self.event('script_bridge_cli_result', function=function, request_id=request_id,
                       executable_sha256=self.cfg.get('debugger_cli_sha256'), **invocation)
        marker = 'NRNX|' + request_id + '|'
        matches = re.findall(re.escape(marker) + r'([^\r\n]+)', output)
        if len(matches) != 1:
            raise RuntimeError('script_bridge_receipt_missing:' + function)
        result = matches[0].strip().split('|')
        self.event('script_bridge_receipt', function=function, request_id=request_id, result=result)
        return result

    def script_log_confirms_bridge_receipt(self, function, request_id, result):
        if function != 'NRNX_TestBridgeExit' or self.script_log_path is None:
            return False
        receipt = 'NRNX|' + request_id + '|' + '|'.join(result)
        try:
            with self.script_log_path.open('r', encoding='utf-8', errors='replace') as stream:
                return any(receipt in line for line in stream)
        except OSError:
            return False

    def begin_script_log_capture(self):
        if self.control_backend != 'script':
            return
        log_root = Path(self.cfg.get('profile') or Path(self.cfg['executable']).parent)
        self.script_log_path = log_root / 'scriptslog.txt'
        self.script_log_baseline = script_log_identity(self.script_log_path)
        dump(self.out / 'script-log-start.json', dict(
            time=stamp(), log_name=self.script_log_path.name, identity=self.script_log_baseline))

    def finish_script_log_capture(self):
        if self.script_log_baseline is None:
            return
        current = script_log_identity(self.script_log_path)
        baseline = self.script_log_baseline
        diagnostic = dict(time=stamp(), log_name=self.script_log_path.name,
                          baseline=baseline, current=current)
        capture_from = None
        if current.get('exists') is None or baseline.get('exists') is None:
            diagnostic['change'] = 'unreadable'
        elif not baseline['exists'] and not current['exists']:
            diagnostic['change'] = 'absent'
        elif not current['exists']:
            diagnostic['change'] = 'missing_after_start'
        elif not baseline['exists']:
            diagnostic['change'] = 'created'
            capture_from = 0
        elif (current.get('sha256') == baseline.get('sha256')
              and current.get('size') == baseline.get('size')):
            diagnostic['change'] = 'unchanged'
        elif (current['size'] >= baseline['size'] and
              script_log_prefix_sha256(self.script_log_path, baseline['size']) == baseline['sha256']):
            diagnostic['change'] = 'appended'
            capture_from = baseline['size']
        else:
            diagnostic['change'] = 'rewritten'
            capture_from = 0
        if capture_from is not None:
            destination = self.out / 'scriptslog-delta.txt'
            try:
                with self.script_log_path.open('rb') as source:
                    source.seek(capture_from)
                    with destination.open('wb') as target:
                        shutil.copyfileobj(source, target)
                diagnostic['captured_bytes'] = destination.stat().st_size
                diagnostic['captured_sha256'] = sha(destination)
                diagnostic['capture_file'] = destination.name
            except OSError as error:
                diagnostic['capture_error'] = repr(error)
        dump(self.out / 'script-log-diagnostic.json', diagnostic)

    def key(self, name, seconds=.2, *, script_startup=False):
        if self.control_backend == 'script' and not script_startup:
            raise RuntimeError('script_backend_disallows_keyboard_input')
        try:
            desktop.key(self.window, name, seconds, activate=self.ui_state != 'capture_hold')
        except Exception:
            self.automation_input_blocked = True
            raise
        self.event('key', key=name, seconds=seconds, state=self.ui_state)

    def main_menu(self, timeout=None):
        if self.control_backend == 'script':
            self.wait_main_menu_ocr(timeout, script_startup=True)
            self.status['script_mod_reload'] = 'not_requested_predeployed_mod'
            self.event('script_mod_reload', result='not_requested',
                       basis='temporary_mod_deployed_before_game_start')
            deadline = time.monotonic() + (timeout or self.cfg['startup_timeout'])
            while time.monotonic() < deadline:
                try:
                    result = self.bridge_call('NRNX_TestBridgeReady')
                    if result == ['ready', '1']:
                        self.ui_state = 'main_menu'
                        self.status['startup'] = 'script_start_screen_confirmed'
                        self.status['script_bridge'] = 'ready_receipt_confirmed'
                        dump(self.out / 'game-start-state.json', dict(
                            time=stamp(), state='start_screen_ready', backend='script'))
                        return result
                    if result != ['ready', '0']:
                        raise RuntimeError('script_bridge_unexpected_ready_state:' + repr(result))
                except (OSError, TimeoutError, witcher_debug.ProtocolError) as error:
                    self.event('script_bridge_transport_failure',
                               function='NRNX_TestBridgeReady', error=repr(error))
                    raise RuntimeError('script_bridge_ready_transport_failure') from error
                time.sleep(1)
            raise TimeoutError('script_start_screen_timeout')
        return self.wait_main_menu_ocr(timeout)

    def wait_main_menu_ocr(self, timeout=None, *, script_startup=False):
        deadline = time.monotonic() + (timeout or self.cfg['startup_timeout'])
        last_skip = 0
        while time.monotonic() < deadline:
            if self.control_backend == 'script':
                if not desktop.same_process(self.window):
                    raise RuntimeError('unexpected_exit')
                if not self.refresh_startup_window():
                    time.sleep(.25)
                    continue
            obs = self.observe('startup', script_startup=script_startup)
            text = compact(obs['text'])
            normalized_text = text.casefold()
            if 'unexpected' in normalized_text and 'token_' in normalized_text:
                self.event('script_compilation_error', ocr_text=text)
                raise RuntimeError('script_compilation_error')
            if '读取游戏' in text and '退出游戏' in text and '新游戏' in text:
                self.ui_state = 'main_menu'
                self.status['startup'] = ('ocr_title_confirmed_for_script_bridge' if script_startup
                                          else 'main_menu_confirmed')
                self.evidence('main-menu')
                return obs
            if '跳过' in text and time.monotonic() - last_skip > 3:
                if script_startup:
                    self.activate()
                self.key('SPACE', script_startup=script_startup)
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
        if self.control_backend == 'script':
            if not desktop.same_process(self.window):
                raise RuntimeError('unexpected_exit_before_script_exit')
            try:
                result = self.bridge_call('NRNX_TestBridgeExit')
            except Exception as error:
                if desktop.same_process(self.window):
                    raise RuntimeError('script_exit_result_unknown_game_still_alive') from error
                request = self.last_script_bridge_request or {}
                if (request.get('function') != 'NRNX_TestBridgeExit'
                        or not self.script_log_confirms_bridge_receipt(
                            'NRNX_TestBridgeExit', request.get('request_id', ''), ['exit', 'requested'])):
                    self.status['safe_exit'] = 'unexpected_exit_before_script_exit_confirmation'
                    raise RuntimeError('script_exit_result_unknown_game_exited_without_receipt') from error
                self.exit_requested = True
                self.ui_state = 'exited'
                self.event('exit_requested', method='game_script_api_log_receipt_response_lost',
                           request_id=request['request_id'], error=repr(error))
                self.wait_exit()
                return
            if result != ['exit', 'requested']:
                raise RuntimeError('script_exit_not_acknowledged:' + repr(result))
            self.ui_state = 'exit_requested'
            self.exit_requested = True
            self.event('exit_requested', method='game_script_api')
            self.wait_exit()
            return
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
        self.status['safe_exit'] = ('script_exit_requested_process_exit_confirmed'
                                    if self.control_backend == 'script'
                                    else 'menu_exit_pending_log_and_crash_check')

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
        identities = self.out / 'verified-module-identities.json'
        if not identities.exists():
            snapshot = crash_evidence.modules(self.window, lambda _: paths, self.cfg['required_module'])
            snapshot['time'] = stamp()
            crash_evidence.write_new(identities, json.dumps(snapshot, ensure_ascii=False, indent=2))

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
        if self.control_backend == 'script':
            raise RuntimeError('script_backend_disallows_mouse_input')
        candidates = [line for line in obs['lines'] if text in re.sub(r'\s+', '', line['text'])]
        if len(candidates) != 1:
            raise RuntimeError(f'OCR target must be unique: {text}, matches={len(candidates)}')
        words = candidates[0]['words']
        x = (min(w['x'] for w in words) + max(w['x']+w['width'] for w in words))/2/obs['scale']
        y = (min(w['y'] for w in words) + max(w['y']+w['height'] for w in words))/2/obs['scale']
        try:
            desktop.click(self.window, x, y, activate=self.ui_state != 'capture_hold')
        except Exception:
            self.automation_input_blocked = True
            raise
        self.event('click_text', text=text, x=x, y=y)

    def launch(self):
        dump(self.out / 'early-modules.json', crash_evidence.modules(None, desktop.modules))
        arguments = list(self.cfg['launch_args'])
        if self.control_backend == 'script':
            for flag in ('-net', '-debugscripts'):
                if flag not in arguments:
                    arguments.append(flag)
        command = [self.cfg['steam']] + arguments
        dump(self.out / 'game-launch.json', dict(time=stamp(), backend=self.control_backend,
                                                 command=command, steam_arguments=arguments))
        self.begin_script_log_capture()
        subprocess.Popen(command, creationflags=0x08000000)
        deadline = time.monotonic() + self.cfg['startup_timeout']
        while time.monotonic() < deadline:
            found = desktop.windows(self.cfg['executable'])
            if len(found) > 1:
                raise RuntimeError('Multiple target windows; refusing input')
            if found:
                self.window = found[0]
                for process in process_inventory(self.cfg['executable']):
                    if process.get('ProcessId') == self.window['pid']:
                        self.window['command_line'] = process.get('CommandLine')
                        break
                dump(self.out / 'owned-process.json', self.window)
                dump(self.out / 'early-modules.json', crash_evidence.modules(self.window, desktop.modules, self.cfg.get('required_module')))
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
                        self.window['command_line'] = candidates[0].get('CommandLine')
                        dump(self.out / 'owned-process.json', self.window)
                        dump(self.out / 'early-modules.json', crash_evidence.modules(self.window, desktop.modules, self.cfg.get('required_module')))
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
            if self.control_backend == 'script':
                if action == 'exit':
                    return
                if action == 'menu_exit':
                    self.request_exit()
                    return
                if action == 'load_save':
                    if self.ui_state != 'main_menu':
                        raise ValueError('Script calibration load requires the confirmed start screen')
                    self.check_mod()
                    self.load_save()
                    self.wait_scene()
                    self.status['save_load'] = 'script_exact_save_loaded_gameplay_ready'
                elif action == 'state':
                    result = self.bridge_call('NRNX_TestBridgeState')
                    dump(self.out / 'calibration-state.json', dict(time=stamp(), result=result))
                elif action == 'exec':
                    function = request.get('function', '')
                    if not isinstance(function, str) or not function.startswith('NRNX_'):
                        raise ValueError('Script calibration may call only NRNX_ bridge functions')
                    arguments = request.get('arguments', [])
                    if not isinstance(arguments, list) or any(not isinstance(item, str) for item in arguments):
                        raise ValueError('Script calibration arguments must be a list of strings')
                    result = self.bridge_call(function, *arguments)
                    dump(self.out / 'calibration-command-result.json', dict(
                        time=stamp(), function=function, result=result))
                else:
                    raise ValueError('Unknown script calibration action')
                continue
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
                if self.ui_state != 'scene':
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
        self.load_save()
        self.wait_scene()
        if self.control_backend == 'desktop':
            self.status['save_load'] = 'unique_save_selected_load_menu_exited'
            self.status['visual_quality'] = 'not_tested_screenshots_only'
            self.evidence('scene-loaded')
        else:
            self.status['visual_quality'] = 'not_tested_no_visual_capture_requested'
        self.scene_health()
        self.capture_hold()
        if self.control_backend == 'script':
            self.status['functionality'] = 'script_bridge_save_load_and_exit_confirmed_mod_features_not_tested'
            self.request_exit()
            return
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
        # Until this hold receives a valid completion receipt, every failure
        # path must leave shutdown to protected recovery instead of game keys.
        self.automation_input_blocked = True
        identity = dict(self.window)
        ready = dict(time=stamp(), pid=self.window['pid'], identity=identity)
        if not self.cfg.get('manual_ime_session'):
            try:
                foreground = desktop.ensure_foreground(self.window)
            except Exception as error:
                self.status['capture_hold'] = 'automatic_foreground_activation_failed'
                dump(self.out / 'capture-failed.json', dict(time=stamp(), phase='capture_ready',
                    stage='automatic_foreground_activation', error=repr(error),
                    diagnostics=getattr(error, 'diagnostics', None), no_game_input_sent=True))
                raise
            ready.update(automatic_foreground=True, foreground=foreground)
        dump(self.out / 'capture-ready.json', ready)
        self.status['capture_hold'] = 'waiting'
        self.event('capture_hold_ready', pid=self.window['pid'], seconds=seconds, identity=identity,
                   foreground='automatic')
        deadline = time.monotonic() + seconds
        while True:
            if not desktop.same_process(self.window):
                self.status['capture_hold'] = 'process_exited'
                self.event('capture_hold_process_exit', pid=self.window['pid'])
                raise RuntimeError('capture_hold_process_exited')
            capture_failure = self.out / 'capture-failed.json'
            if capture_failure.exists():
                try:
                    details = json.loads(capture_failure.read_text(encoding='utf-8'))
                except (OSError, UnicodeError, ValueError):
                    details = {}
                if not isinstance(details, dict):
                    details = {}
                self.status['capture_hold'] = 'automatic_capture_failed'
                self.event('capture_hold_capture_failed', error=details.get('error', 'invalid_failure_receipt'))
                raise RuntimeError('capture_automatic_failed')
            if (capture_complete(self.out, self.window['pid'])
                    and (not self.cfg.get('capture_script')
                         or capture_script_complete(self.out, self.window['pid']))):
                self.status['capture_hold'] = 'completed_by_signal'
                self.event('capture_hold_complete', pid=self.window['pid'])
                self.automation_input_blocked = False
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
                           result='timeout_not_mod_acceptance_no_game_input')
                raise TimeoutError('capture_hold_timeout')
            time.sleep(min(1, remaining))

    def load_save(self):
        if self.control_backend == 'script':
            path = Path(self.cfg['profile']) / self.cfg['save_dir'] / self.cfg['save']
            if sha(path) != self.cfg['save_sha256']:
                raise RuntimeError('target_save_changed_before_script_load')
            game_filename = Path(self.cfg['save']).stem.lower()
            deadline = time.monotonic() + self.cfg.get('load_timeout', 120)
            while True:
                result = self.bridge_call('NRNX_TestBridgeLoadSave', game_filename)
                if result != ['load', 'pending', 'saves_not_initialized']:
                    break
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('script_save_list_initialization_timeout')
                self.event('script_save_list_wait', state='saves_not_initialized')
                time.sleep(min(1, remaining))
            if len(result) != 3 or result[:2] != ['load', 'requested'] or result[2] != game_filename:
                raise RuntimeError('script_save_load_not_acknowledged:' + repr(result))
            self.ui_state = 'loading'
            self.status['save_load'] = 'script_requested_exact_save'
            dump(self.out / 'save-selection.json', dict(
                time=stamp(), filename=self.cfg['save'], game_filename=game_filename,
                sha256=self.cfg['save_sha256'],
                basis='disk save hash plus canonical game ListSavedGames filename match and LoadGameInit receipt'))
            return
        expected = datetime.fromisoformat(self.cfg['save_modified']).replace(microsecond=0).isoformat()
        deadline = time.monotonic() + 60
        seen = set()
        clicked_target = False
        for _ in range(self.cfg.get('max_save_steps', 40)):
            if time.monotonic() > deadline:
                break
            obs = (self.observe('save-list', region_state='load_menu') if clicked_target
                   else self.observe('save-list'))
            rows = save_rows(self.out / 'latest.png', obs)
            if clicked_target:
                self.ui_state = 'load_menu' if rows else 'save_click_pending'
                if not rows:
                    self.ui_state = 'loading'
                    dump(self.out / 'save-selection.json', dict(
                        time=stamp(), filename=self.cfg['save'], sha256=self.cfg['save_sha256'],
                        timestamp=expected, clicked=clicked_row,
                        basis='unique original-file mtime, unique UI timestamp click, load menu exited'))
                    return
            target = [row for row in rows if row['timestamp'] == expected]
            if len(target) > 1:
                raise RuntimeError('ambiguous_ui_save_timestamp')
            selected = [row for row in rows if row['selected']]
            if target and not clicked_target and (not selected or
                    (len(selected) == 1 and selected[0]['timestamp'] != expected)):
                # Navigate directly to the unique visible target. An intervening
                # focused row may have an unreadable timestamp and be absent
                # from rows. Reobserve focus or confirm the expected scene.
                self.click_text(obs, compact(target[0]['text']))
                clicked_target = True
                clicked_row = target[0]
                self.ui_state = 'save_click_pending'
                time.sleep(.5)
                continue
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
                self.ui_state = 'loading'
                return
            if current in seen:
                raise RuntimeError('save_selection_did_not_move')
            seen.add(current)
            self.key('UP' if target and target[0]['y'] < selected[0]['y'] else 'DOWN')
            time.sleep(.5)
        raise TimeoutError('save_selection_timeout')

    def wait_scene(self, timeout=None):
        if self.control_backend == 'script':
            if self.ui_state != 'loading':
                raise RuntimeError('script_gameplay_wait_requires_requested_save')
            deadline = time.monotonic() + (timeout or self.cfg['load_timeout'])
            ready_count = 0
            last_error = None
            while time.monotonic() < deadline:
                try:
                    result = self.bridge_call('NRNX_TestBridgeState')
                    last_error = None
                except (OSError, TimeoutError, witcher_debug.ProtocolError) as error:
                    message = repr(error)
                    if message != last_error:
                        self.event('script_load_state_wait', error=message)
                        last_error = message
                    ready_count = 0
                    time.sleep(1)
                    continue
                if result == ['state', 'gameplay_ready']:
                    ready_count += 1
                    if ready_count >= 2:
                        self.ui_state = 'scene'
                        self.status['save_load'] = 'script_exact_save_loaded_gameplay_ready'
                        self.event('gameplay_transition_confirmed', from_state='loading',
                                   basis='non_error_load_progress_and_player_present_twice')
                        dump(self.out / 'load-state.json', dict(time=stamp(), result=result,
                            stable_observations=ready_count, filename=self.cfg['save'],
                            save_sha256=self.cfg['save_sha256']))
                        self.frame_evidence('scene-loaded')
                        return result
                elif result == ['state', 'missing_content'] or result == ['state', 'error']:
                    raise RuntimeError('script_game_load_failed:' + result[1])
                elif result not in (['state', 'loading'], ['state', 'waiting']):
                    raise RuntimeError('script_game_state_unrecognized:' + repr(result))
                else:
                    ready_count = 0
                time.sleep(1)
            raise TimeoutError('script_gameplay_load_timeout')
        loading = self.ui_state == 'loading'
        if not loading and self.ui_state != 'scene':
            raise RuntimeError('scene_transition_requires_loading_or_scene_state')
        deadline = time.monotonic() + (timeout or self.cfg['load_timeout'])
        consecutive = 0
        while time.monotonic() < deadline:
            region_state = 'load_menu' if loading else 'pause_menu'
            obs = self.observe('scene-wait', region_state=region_state)
            if loading:
                menu_visible = bool(save_rows(self.out / 'latest.png', obs))
            else:
                text = compact(obs['text'])
                menu_visible = any(compact(label) in text for label in PAUSE_MENU)
            consecutive = 0 if menu_visible else consecutive + 1
            if consecutive >= 2:
                previous_state = self.ui_state
                self.ui_state = 'scene'
                self.event('gameplay_transition_confirmed', from_state=previous_state,
                           basis='menu_absent_in_two_observations')
                return obs
            time.sleep(1)
        raise TimeoutError('scene_load_timeout' if loading else 'scene_resume_timeout')

    def scene_health(self):
        if self.control_backend == 'script':
            samples = []
            deadline = time.monotonic() + self.cfg.get('scene_seconds', 8)
            while time.monotonic() < deadline:
                result = self.bridge_call('NRNX_TestBridgeState')
                if result in (['state', 'missing_content'], ['state', 'error']):
                    raise RuntimeError('script_gameplay_state_lost:' + result[1])
                if result != ['state', 'gameplay_ready']:
                    raise RuntimeError('script_gameplay_state_unconfirmed:' + repr(result))
                metrics = desktop.process_metrics(self.window['pid'])
                hung = bool(desktop.window_alive(self.window) and desktop.u.IsHungAppWindow(self.window['hwnd']))
                sample = dict(time=stamp(), state='gameplay_ready', process=metrics, hung=hung)
                samples.append(sample)
                dump(self.out / 'scene-health.json', samples)
                if metrics is None or hung:
                    raise RuntimeError('scene_unresponsive')
                time.sleep(1)
            self.status['stability'] = 'script_gameplay_state_and_process_responsive'
            return
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
        if self.automation_input_blocked:
            self.status['safe_exit'] = 'protected_recovery_required_no_game_input'
            self.event('shutdown_deferred_to_protected_recovery', pid=self.window['pid'],
                       reason='Automatic foreground or capture verification failed')
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
            if self.control_backend == 'script':
                self.request_exit()
                return
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
PAUSE_MENU = ['继续', '存储游戏', '读取游戏', '设置', '教程', '昆特牌组', '返回主菜单', '退出游戏']


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
        finally:
            if session.debugger is not None:
                session.debugger.close()
        try:
            session.finish_script_log_capture()
        except Exception as error:
            session.status.update(result='failed', script_log_diagnostic_error=repr(error))
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


def summarize_motion(recording):
    """Verify sampled JPEGs and their smaller playback before releasing source frames."""
    record = json.loads(recording.read_text(encoding='utf-8'))
    frames = record['frames']
    if record['status'] != 'complete' or len(frames) < 2:
        raise ValueError('Motion recording is incomplete; preserve source frames')
    starts = [frame['capture_begin_ns'] for frame in frames]
    intervals = [(b - a) / 1e6 for a, b in zip(starts, starts[1:])]
    if any(value <= 0 for value in intervals):
        raise ValueError('Motion timestamps must increase')
    durations, elapsed, rounded = [], 0.0, 0
    for duration in intervals + [statistics.median(intervals)]:
        elapsed += duration
        next_rounded = round(elapsed)
        durations.append(max(1, next_rounded - rounded))
        rounded = next_rounded
    images, sources = [], []
    preview = recording.parent / 'preview.webp'
    try:
        for index, frame in enumerate(frames):
            _, source = output_lifecycle._relative_file(recording.parent, Path(frame['file']))
            if frame['index'] != index or source.stat().st_size != frame['bytes'] or sha(source) != frame['sha256']:
                raise ValueError(f'Motion source identity mismatch: {index}')
            with Image.open(source) as image:
                if image.size != (record['width'], record['height']) or image.mode != 'RGB':
                    raise ValueError(f'Motion source format mismatch: {index}')
                sample = image.copy()
                sample.thumbnail((1280, 720), Image.Resampling.LANCZOS)
                images.append(sample)
            sources.append(source)
        with preview.open('xb') as stream:
            images[0].save(stream, format='WEBP', save_all=True, append_images=images[1:],
                           duration=durations, loop=0, quality=90, method=4)
        with Image.open(preview) as image:
            if image.n_frames != len(frames):
                raise ValueError('Motion preview frame count mismatch')
            for index, duration in enumerate(durations):
                image.seek(index)
                image.load()
                if image.info.get('duration') != duration:
                    raise ValueError('Motion preview timing mismatch')
        dump(recording.parent / 'preview-summary.json', dict(
            source_recording=recording.name, source_sha256=sha(recording), source_hashes_verified=True,
            preview=preview.name, preview_sha256=sha(preview), frame_count=len(frames),
            duration_ms=durations, preview_size=list(images[0].size),
            limitation='Reduced SDR sampling preview; original resolution and native game FPS are not preserved'))
        return sources
    finally:
        for image in images:
            image.close()


def finish_artifacts(out, final, guard_ok):
    """Release navigation captures and verified save copies after their consumers finish."""
    retained = list(out.glob('hdr-*.png')) + list(out.glob('addon-menu-*.png'))
    retained += [out / 'crash-evidence', out / 'hang-evidence', out / 'hang-evidence-manual', out / 'model-pair']
    retained += [out / name for name in (
        'script-bridge-endpoint.json', 'script-bridge-state.json', 'script-log-start.json',
        'script-log-diagnostic.json', 'scriptslog-delta.txt') if (out / name).exists()]
    if final['result'] == 'failed':
        retained.append(out / 'latest.png')
    disposables = [path for path in out.glob('*.png') if path not in retained]
    if guard_ok:
        for entry in json.loads((out / 'protected-files.json').read_text(encoding='utf-8')):
            backup = Path(entry['backup_path'])
            if Path(entry['path']).suffix.lower() in {'.ini', '.settings', '.bak'}:
                retained.append(backup)
            else:
                disposables.append(backup)
    else:
        retained.append(out / 'protected')
    for recording in out.glob('motion-*/recording.json'):
        if output_lifecycle._read(out).get('keepRawArtifacts'):
            continue
        try:
            frames = summarize_motion(recording)
            retained += frames[:1] + frames[-1:]
            retained.append(recording.parent / 'preview.webp')
            disposables += frames[1:-1]
        except (OSError, ValueError, KeyError) as error:
            retained.append(recording.parent)
            dump(recording.parent / 'preview-error.json', dict(error=str(error)))
    output_lifecycle.retain(out, retained)
    output_lifecycle.register_disposable(out, disposables)
    output_lifecycle.finish(out, 'Failed' if final['result'] == 'failed' else 'Passed')


def run_attempt(cfg, out):
    from guard import ProtectedFiles
    output_lifecycle.initialize(out)
    dump(out / 'target.json', cfg)
    source = out / 'source'
    source.mkdir()
    sources = []
    for name in ('runner.py', 'desktop.py', 'guard.py', 'normal_process.py', 'crash_evidence.py', 'hang_evidence.py',
                 'temporary_mod.py', 'witcher_debug.py'):
        shutil.copyfile(HERE / name, source / name)
        sources.append(dict(name=name, sha256=sha(source / name)))
    bridge_source = HERE / 'mods' / 'modNRNexusTestBridge'
    shutil.copytree(bridge_source, source / 'mods' / bridge_source.name)
    for bridge_file in sorted((source / 'mods' / bridge_source.name).rglob('*.ws')):
        sources.append(dict(name=str(bridge_file.relative_to(source)), sha256=sha(bridge_file)))
    shutil.copyfile(Path(output_lifecycle.__file__), source / 'output_lifecycle.py')
    sources.append(dict(name='output_lifecycle.py', sha256=sha(source / 'output_lifecycle.py')))
    dump(out / 'source-manifest.json', dict(files=sources, target_sha256=sha(out / 'target.json'),
                                          ocr_sha256=sha(cfg['ocr_executable']) if cfg.get('ocr_executable') else None))
    print('RUN_DIRECTORY=' + str(out), flush=True)
    profile = Path(cfg['profile'])
    protected = [p for p in (profile / cfg['save_dir']).iterdir() if p.is_file()]
    protected += list(profile.glob('*.settings*'))
    protected += list(Path(cfg['executable']).parent.glob('*.ini'))
    child, capture_child, capture_log, owned, problem, guard_ok = None, None, None, None, None, False
    mod_lease = None
    mod_installed_by_runner = False
    capture_started = False
    hang_capture_reason = None
    crashes_before = crash_reports()
    final = dict(result='failed', started=stamp())
    try:
        mod_lease, mod_files = stage_script_mod(
            cfg, source / 'mods' / 'modNRNexusTestBridge')
        if mod_lease is not None:
            mod_installed_by_runner = mod_lease.owned
            protected.extend(mod_files)
            dump(out / 'temporary-mod.json', dict(time=stamp(), name='modNRNexusTestBridge',
                installed_by_runner=mod_lease.owned,
                files=[dict(path=str(path), sha256=sha(path)) for path in mod_files]))
        with ProtectedFiles(protected, out / 'protected') as guard:
            dump(out / 'protected-files.json', guard.manifest)
            freeze_logs(cfg, out / 'logs-before')
            crash_evidence.configurations(cfg, out / 'deployment-config')
            dump(out / 'early-modules.json', crash_evidence.modules(None, lambda p: desktop.modules(p)))
            try:
                with (out / 'worker.log').open('w', encoding='utf-8') as log:
                    child = subprocess.Popen([sys.executable, '-B', str(source / 'runner.py'), '--worker', str(out)], stdout=log, stderr=log, creationflags=0x08000000)
                    foreground_permission = desktop.grant_foreground_permission(child.pid)
                    dump(out / 'foreground-grant.json', dict(time=stamp(), process_id=child.pid,
                                                             **foreground_permission))
                    started, hung_since = time.monotonic(), None
                    capture_ready_announced = False
                    while child.poll() is None:
                        if (cfg.get('capture_hold_seconds', 0) and not capture_ready_announced
                                and (out / 'capture-ready.json').exists()):
                            print('CAPTURE_READY=' + str(out), flush=True)
                            capture_ready_announced = True
                        if (cfg.get('capture_script') and not capture_started
                                and (out / 'capture-ready.json').exists()
                                and not (out / 'capture-failed.json').exists()):
                            capture_started = True
                            try:
                                script = capture_script_path(cfg['capture_script'])
                                if sha(script) != cfg['capture_script_sha256']:
                                    raise RuntimeError('Capture script changed after preflight')
                                capture_log = (out / 'automatic-capture.log').open('w', encoding='utf-8')
                                capture_child = subprocess.Popen([sys.executable, '-B', str(script), str(out)],
                                    stdout=capture_log, stderr=capture_log, creationflags=0x08000000)
                                dump(out / 'automatic-capture-process.json', dict(time=stamp(),
                                    pid=capture_child.pid, script=str(script), sha256=cfg['capture_script_sha256']))
                                desktop.grant_foreground_permission(capture_child.pid)
                            except Exception as error:
                                dump(out / 'capture-failed.json', dict(time=stamp(),
                                    stage='automatic_capture_start', error=repr(error)))
                        if capture_child is not None and capture_child.poll() is not None:
                            capture_result = capture_child.returncode
                            capture_pid = json.loads((out / 'owned-process.json').read_text(encoding='utf-8'))['pid']
                            if capture_result != 0 or not capture_complete(out, capture_pid):
                                dump(out / 'capture-failed.json', dict(time=stamp(),
                                    stage='automatic_capture_exit', exit_code=capture_result,
                                    error='Automatic capture failed or omitted its completion receipt'))
                            else:
                                dump(out / 'capture-script-result.json', dict(time=stamp(),
                                    pid=capture_pid, exit_code=capture_result))
                            capture_child = None
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
                            hang_capture_reason = problem
                            freeze_logs(cfg, out / 'watchdog-logs')
                            dump(out / 'watchdog.json', dict(time=stamp(), reason=problem))
                            break
                        time.sleep(.2 if cfg.get('capture_script') else 2)
            finally:
                # Keep file locks held through recovery, even if the worker or monitor raises.
                try:
                    if capture_child is not None:
                        capture_stop_errors = []

                        def note_capture_stop_error(stage, error):
                            nonlocal problem
                            problem = 'capture_stop_failed'
                            capture_stop_errors.append(dict(stage=stage, error=repr(error)))

                        def capture_is_alive():
                            try:
                                return capture_child.poll() is None
                            except Exception as error:
                                note_capture_stop_error('poll', error)
                                return True

                        def wait_for_capture(timeout, stage):
                            try:
                                capture_child.wait(timeout=timeout)
                            except subprocess.TimeoutExpired:
                                return False
                            except Exception as error:
                                note_capture_stop_error(stage, error)
                                return False
                            return not capture_is_alive()

                        def best_effort_capture_receipt(name, receipt):
                            try:
                                dump(out / name, receipt)
                            except Exception as error:
                                note_capture_stop_error('write_' + name, error)

                        if capture_is_alive():
                            best_effort_capture_receipt('capture-cancel.json',
                                dict(time=stamp(), reason='protected_runner_stopping'))
                            if not wait_for_capture(5, 'cancel_wait') and capture_is_alive():
                                try:
                                    capture_child.terminate()
                                except Exception as error:
                                    note_capture_stop_error('terminate', error)
                                if not wait_for_capture(5, 'terminate_wait') and capture_is_alive():
                                    try:
                                        capture_child.kill()
                                    except Exception as error:
                                        note_capture_stop_error('kill', error)
                                    if not wait_for_capture(5, 'kill_wait') and capture_is_alive():
                                        note_capture_stop_error('exit_unconfirmed',
                                            RuntimeError('Capture process remained alive after bounded stop'))

                            if capture_stop_errors:
                                best_effort_capture_receipt('capture-stop-error.json', dict(
                                    time=stamp(), capture_pid=capture_child.pid, errors=capture_stop_errors))
                            if capture_is_alive():
                                best_effort_capture_receipt('capture-intervention-required.json', dict(
                                    time=stamp(), capture_pid=capture_child.pid,
                                    state='waiting_with_original_file_locks_held'))
                                print('HUMAN_INTERVENTION_REQUIRED=' + str(out), flush=True)
                                while capture_is_alive():
                                    time.sleep(2)
                                best_effort_capture_receipt('capture-intervention-resolved.json', dict(
                                    time=stamp(), capture_pid=capture_child.pid, capture_process_exited=True))
                        elif capture_stop_errors:
                            best_effort_capture_receipt('capture-stop-error.json', dict(
                                time=stamp(), capture_pid=capture_child.pid, errors=capture_stop_errors))
                    if capture_log is not None:
                        capture_log.close()
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
                    if hang_capture_reason and owned:
                        try:
                            final['hang_evidence'] = dict(reason=hang_capture_reason,
                                **hang_evidence.capture(owned, out / 'hang-evidence'))
                        except Exception as error:
                            final['hang_evidence'] = dict(reason=hang_capture_reason,
                                status='failed', error=str(error))
                    for action in (
                        lambda: dump(out / 'recovery-modules.json', crash_evidence.modules(owned, lambda p: desktop.modules(p), cfg.get('required_module'))),
                        lambda: freeze_logs(cfg, out / 'pre-restore-logs'),
                        lambda: collect_crashes(crashes_before, out),
                        lambda: crash_evidence.collect(cfg, out, final['started'], 'pre-restore'),
                    ):
                        try:
                            action()
                        except Exception as error:
                            final['diagnostic_error'] = str(error)
                            problem = problem or 'diagnostic_collection_failed'
                    if recover_owned(cfg, out, owned):
                        problem = problem or 'worker_left_owned_process'
                        dump(out / 'recovery.json', dict(time=stamp(), reason=problem, method='owned_process_termination_or_manual_recovery'))
        guard_ok = True
    except BaseException as error:
        problem = problem or str(error)
        final.update(parent_error=str(error), parent_traceback=traceback.format_exc())
    finally:
        if mod_lease is not None:
            try:
                mod_lease.cleanup()
                dump(out / 'temporary-mod-restored.json', dict(time=stamp(), restored=True,
                    installed_by_runner=mod_installed_by_runner))
            except Exception as error:
                problem = problem or 'temporary_mod_restore_failed'
                final['temporary_mod_restore_error'] = repr(error)
                dump(out / 'temporary-mod-restore-error.json', dict(time=stamp(), error=repr(error)))
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
            diagnostics = crash_evidence.collect(cfg, out, final['started'], 'post-exit')
            process_dumps = crash_evidence.owned_process_dumps(diagnostics, owned)
            if process_dumps:
                problem = problem or 'owned_process_crash_dump'
                final['owned_process_dumps'] = process_dumps
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
        finish_artifacts(out, final, guard_ok)
    return final


def should_retry(final, scenario):
    if (scenario == 'calibrate' or final.get('guard') != 'verified_original_hashes_and_released'
            or final.get('crashes') or final.get('owned_process_dumps') or final.get('diagnostic_error')):
        return False
    if final.get('parent_problem'):
        return final['parent_problem'] in ('run_timeout', 'persistent_unresponsive')
    return final.get('error') in (
        'main_menu_timeout', 'startup_timeout: Steam did not produce a DX12 game window',
        'scene_load_timeout', 'save_selection_timeout', 'persistent_black_scene', 'suspected_frozen_scene')


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
    parser.add_argument('--capture-script', type=Path,
                        help='Run a local capture script automatically when capture-ready is published')
    parser.add_argument('--manual-ime-session', action='store_true')
    parser.add_argument('--save')
    parser.add_argument('--preflight', action='store_true')
    parser.add_argument('--addon-absent-control', action='store_true')
    parser.add_argument('--retries', type=int, choices=range(3), default=0,
                        help='Retries after a safely recovered failure (default: 0)')
    parser.add_argument('--worker', type=Path)
    args = parser.parse_args()
    filesystem_context = normal_process.require_normal_context()
    try:
        validate_capture_hold_scenario(args.scenario, args.capture_hold_seconds)
    except ValueError as error:
        parser.error(str(error))
    if args.worker:
        return worker(args.worker)
    try:
        validate_scenario_save(args.scenario, args.save)
    except ValueError as error:
        parser.error(str(error))
    if args.manual_ime_session and (args.scenario != 'menu' or not args.capture_hold_seconds or args.retries != 0):
        parser.error('--manual-ime-session requires menu, nonzero capture hold and --retries 0')
    if args.capture_script and (not args.capture_hold_seconds or args.manual_ime_session):
        parser.error('--capture-script requires nonzero capture hold and cannot use --manual-ime-session')
    script = capture_script_path(args.capture_script) if args.capture_script else None
    output_root = local_output_root(args.output_root)
    output_lifecycle.maintain_workspace(ROOT)
    control_backend = 'desktop' if args.manual_ime_session else 'script'
    cfg = resolve_target(args.game, args.platform, args.save, args.addon_absent_control,
                         args.target, control_backend=control_backend)
    cfg['scenario'] = args.scenario
    cfg['capture_hold_seconds'] = args.capture_hold_seconds
    cfg['manual_ime_session'] = args.manual_ime_session
    cfg['filesystem_context'] = filesystem_context
    if script:
        cfg.update(capture_script=str(script), capture_script_sha256=sha(script))
    if process_inventory(cfg['executable']):
        raise RuntimeError('A game session already exists; refusing to adopt or stop it')
    if process_inventory(Path(cfg['executable']).parent / 'crashreporter/CrashReporter.exe'):
        raise RuntimeError('An existing crash reporter blocks launch; preserve and dismiss it first')
    if args.preflight:
        cfg['ocr_executable'] = None
        print(json.dumps(cfg, ensure_ascii=False, indent=2))
        return 0
    cfg['ocr_executable'] = None if args.manual_ime_session else prepare_ocr(cfg, output_root)
    out = output_root / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + os.urandom(3).hex())
    try:
        return run_trials(cfg, out, args.retries)
    finally:
        output_lifecycle.maintain_workspace(ROOT)

if __name__ == '__main__':
    if '--worker' in sys.argv:
        sys.exit(main())
    with runner_lock():
        sys.exit(main())
