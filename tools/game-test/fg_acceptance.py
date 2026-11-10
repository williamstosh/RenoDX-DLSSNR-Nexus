"""Run the existing Witcher guard/menu harness and a bounded NR comparison together."""
import argparse
from datetime import datetime, timedelta
import hashlib
import math
import json
from pathlib import Path
import re
import subprocess
import sys
import time

import desktop
from runner import ROOT, dump, stamp, local_output_root


def select_hdr_target(display, target):
    expected_monitor = target['expected_hdr_monitor']
    if not expected_monitor:
        raise ValueError('Set expected_hdr_monitor in the local target')
    luid = target.get('expected_hdr_adapter_luid')
    target_id = target.get('expected_hdr_target_id')
    active = [item for item in display['targets'] if item.get('active') is True]
    if luid is not None or target_id is not None:
        if (not isinstance(luid, str) or re.fullmatch(r'[0-9a-fA-F]{16}', luid) is None
                or type(target_id) is not int or not 0 <= target_id <= 0xffffffff):
            raise ValueError('Configure both expected_hdr_adapter_luid (16 hex digits) and expected_hdr_target_id (uint32)')
        matches = [item for item in active
                   if isinstance(item.get('adapterLUID'), str) and item['adapterLUID'].lower() == luid.lower()
                   and type(item.get('targetId')) is int and item['targetId'] == target_id]
    else:
        matches = active
    if (len(matches) != 1 or matches[0].get('monitorFriendlyDeviceName') != expected_monitor
            or matches[0].get('highDynamicRangeUserEnabled') is not True):
        raise RuntimeError('Expected unique configured Windows HDR output; installation untouched')
    return matches[0]


def parse_fg_interval(line):
    if 'DLSS5 FG interval' not in line:
        return None
    values = dict(re.findall(
        r'(?<![\w])(seconds|application_presents|nr_submitted|status_samples|displayed_frames|ui_mode|ui_composition|nr_colors|sdk_fg_enabled|fresh_present_id)=([0-9.]+)',
        line))
    service = re.search(r'(?<![\w])nr_service\[([^]]*)\]', line)
    if not service:
        return None
    service_values = dict(re.findall(r'([a-z_]+)=(\d+)', service.group(1)))
    return dict(
        seconds=float(values['seconds']),
        application_presents=int(values['application_presents']),
        nr_submitted=int(values['nr_submitted']),
        status_samples=int(values['status_samples']),
        displayed_frames=int(values['displayed_frames']),
        ui_mode=int(values['ui_mode']),
        ui_composition=int(values['ui_composition']),
        nr_colors=int(values['nr_colors']),
        sdk_enabled=int(values['sdk_fg_enabled']),
        fresh_present_id=int(values['fresh_present_id']),
        nr_service={key: int(value) for key, value in service_values.items()})


def parse_nr_telemetry(line):
    if ' telemetry t=' not in line or ' nr[evals=' not in line:
        return None
    prefix, nr_counters = line.split(' nr[', 1)
    values = dict(re.findall(r'(?<![\w])(win|presents|fps)=([0-9.]+)', prefix))
    return dict(window_seconds=float(values['win']), presents=int(values['presents']),
                fps=float(values['fps']), evaluates=int(re.match(r'evals=(\d+)(?=\s|\])', nr_counters)[1]))


def capture(run, nr_off_control=False, check_addon_menu=False):
    ready = json.loads((run / 'capture-ready.json').read_text(encoding='utf-8'))
    window = ready['identity']
    events = []
    menu_open = False
    game_input_attempted = False
    failure = None
    try:
        owned = json.loads((run / 'owned-process.json').read_text(encoding='utf-8'))
        keys = ('hwnd', 'pid', 'created', 'path')
        if (ready.get('pid') != window['pid'] or ready.get('automatic_foreground') is not True
                or any(owned.get(key) != window[key] for key in keys)):
            raise RuntimeError('capture-ready automatic foreground identity mismatch')
        if not desktop.same_process(window):
            raise RuntimeError('Game identity changed after automatic foreground activation')
        if desktop.u.GetForegroundWindow() != window['hwnd']:
            raise RuntimeError('Game lost foreground after runner capture activation')
        events.append(dict(time=stamp(), action='automatic_foreground_confirmed',
                           hwnd=window['hwnd'], pid=window['pid'], created=window['created'],
                           activation='runner_capture_ready'))
        paths = desktop.modules(window)
        rejected = [p for p in paths if 'rtsshooks' in p.lower()
            or (p.lower().endswith(('.addon', '.addon64')) and Path(p).name.lower() != 'renodx-dlss5.addon64')]
        dump(run / 'isolation-modules.json', dict(time=stamp(), modules=paths, rejected=rejected,
            retained_overlays='NVIDIA and OptiScaler FPS per user'))
        if rejected:
            raise RuntimeError('Unexpected add-on or RTSS remains')
        desktop.KEYS['F6'] = 0x40
        stages = ([('hdr-control-nr-off', 30, None)] if nr_off_control else
            [('hdr-nr-on', 15, 'off'), ('hdr-nr-off', 22, 'on'), ('hdr-nr-on-restored', 18, None)])
        for label, seconds, next_state in stages:
            time.sleep(seconds)
            if not desktop.same_process(window):
                raise RuntimeError('Game identity changed; comparison stopped')
            desktop.capture(window, run / (label + '.png'), activate=False)
            events.append(dict(time=stamp(), action='capture', name=label))
            if next_state:
                game_input_attempted = True
                desktop.key(window, 'F6', .25, activate=False)
                events.append(dict(time=stamp(), action='F6', intended_state=next_state))
        if not nr_off_control:
            game_input_attempted = True
            desktop.key(window, 'D', .3, activate=False)
            time.sleep(2)
            desktop.capture(window, run / 'hdr-short-move.png', activate=False)
            events.append(dict(time=stamp(), action='capture', name='hdr-short-move'))
        if check_addon_menu:
            game_input_attempted = True
            desktop.key(window, 'HOME', activate=False)
            menu_open = True
            events.append(dict(time=stamp(), action='HOME', intended_state='open_addon_menu'))
            for index in (1, 2):
                time.sleep(6)
                if not desktop.same_process(window):
                    raise RuntimeError('Game identity changed during addon menu check')
                desktop.capture(window, run / f'addon-menu-{index}.png', activate=False)
                events.append(dict(time=stamp(), action='capture', name=f'addon-menu-{index}'))
    except Exception as error:
        failure = error
    finally:
        try:
            if menu_open and desktop.same_process(window) and desktop.u.GetForegroundWindow() == window['hwnd']:
                game_input_attempted = True
                desktop.key(window, 'HOME', activate=False)
                events.append(dict(time=stamp(), action='HOME', intended_state='close_addon_menu'))
        except Exception as error:
            failure = failure or error
        try:
            dump(run / 'nr-toggle-captures.json', events)
        except Exception as error:
            failure = failure or error
    if failure:
        dump(run / 'capture-failed.json', dict(time=stamp(), phase='capture_ready',
            stage='capture_actions', error=repr(failure),
            diagnostics=getattr(failure, 'diagnostics', None),
            no_game_input_sent=not game_input_attempted,
            game_input_may_have_been_sent=game_input_attempted))
        raise failure
    dump(run / 'capture-complete.json', dict(pid=ready['pid'], completed=True))


def supported_control_mask_route(nr_hook_point, use_core_context, xefg_ui_composition):
    return ((nr_hook_point == 2 and not use_core_context and xefg_ui_composition == 0)
            or (nr_hook_point == 0 and use_core_context))


def capture_events_for_verification(run, receipt):
    toggle_events = run / 'nr-toggle-captures.json'
    if toggle_events.is_file():
        return json.loads(toggle_events.read_text(encoding='utf-8'))

    sequence_path = run / 'live-control-mask-sequence.json'
    if not sequence_path.is_file():
        raise RuntimeError('Capture event record is missing')
    sequence = json.loads(sequence_path.read_text(encoding='utf-8'))
    if sequence.get('candidate_sha256', '').lower() != receipt['candidate_sha256'].lower():
        raise RuntimeError('Capture sequence candidate identity mismatch')

    selection = json.loads((run / 'save-selection.json').read_text(encoding='utf-8'))
    if sequence.get('save_sha256', '').lower() != selection['sha256'].lower():
        raise RuntimeError('Capture sequence save identity mismatch')

    transitions = [(entry.get('from_state'), entry.get('state'))
        for entry in sequence.get('transitions', [])]
    turned_on = next((index for index, pair in enumerate(transitions) if pair == ('off', 'on')), None)
    if turned_on is None or ('on', 'off') not in transitions[turned_on + 1:]:
        raise RuntimeError('Capture sequence did not confirm ControlMask OFF-ON-OFF')

    scene_events = [json.loads(line) for line in (run / 'events.jsonl').read_text(encoding='utf-8').splitlines()
        if line.strip()]
    captures = [event for event in scene_events if event.get('kind') == 'live_control_mask_scene_capture']
    names = [event.get('name') for event in captures]
    if names != ['mask-off-before.png', 'mask-on.png', 'mask-off-after.png']:
        raise RuntimeError('Live ControlMask scene captures are incomplete or out of order')
    return [dict(time=event['time'], action='capture', name=event['name']) for event in captures]


def file_sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest().lower()


def verify_restoration(run, receipt, result):
    if not (receipt['restored'] or receipt.get('test_changes_restored')) or result['guard'] != 'verified_original_hashes_and_released':
        raise RuntimeError('Restoration not verified')

    guard = json.loads((run / 'protected-files.json').read_text(encoding='utf-8'))
    outer = {str(Path(e['path'])).replace('\\', '/').casefold(): e for e in receipt['original_files']}
    retained = {str(Path(e['path'])).replace('\\', '/').casefold(): e for e in receipt.get('retained_files', [])}
    owned_temporary_files = {}
    temporary_manifest_path = run / 'temporary-mod.json'
    if temporary_manifest_path.is_file():
        temporary_manifest = json.loads(temporary_manifest_path.read_text(encoding='utf-8'))
        if temporary_manifest.get('installed_by_runner'):
            restored_path = run / 'temporary-mod-restored.json'
            restored = json.loads(restored_path.read_text(encoding='utf-8')) if restored_path.is_file() else {}
            if not restored.get('restored') or not restored.get('installed_by_runner'):
                raise RuntimeError('Runner-owned temporary script mod cleanup was not verified')
            for item in temporary_manifest.get('files', []):
                key = str(Path(item['path'])).replace('\\', '/').casefold()
                if key in owned_temporary_files:
                    raise RuntimeError('Temporary script mod manifest contains duplicate paths')
                owned_temporary_files[key] = item['sha256'].lower()

    guard_by_path = {str(Path(e['path'])).replace('\\', '/').casefold(): e for e in guard}
    if not owned_temporary_files.keys() <= guard_by_path.keys():
        raise RuntimeError('Temporary script mod files are missing from the protection record')
    for entry in guard:
        key = str(Path(entry['path'])).replace('\\', '/').casefold()
        original = outer.get(key)
        if original:
            if file_sha256(entry['backup_path']) != entry['sha256'].lower():
                raise RuntimeError('Guard backup changed')
        elif key in owned_temporary_files:
            expected = owned_temporary_files[key]
            if entry['sha256'].lower() != expected:
                raise RuntimeError('Temporary script mod protection hash mismatch')
            if Path(entry['path']).exists():
                raise RuntimeError('Runner-owned temporary script mod was not removed')
        elif file_sha256(entry['path']) != entry['sha256'].lower():
            raise RuntimeError('Protected original changed')
    for entry in receipt['original_files'] + receipt.get('isolated_addons', []):
        key = str(Path(entry['path'])).replace('\\', '/').casefold()
        expected = retained.get(key, entry)
        if file_sha256(entry['path']) != expected['sha256'].lower():
            raise RuntimeError('Installation original changed')
        if entry.get('disabled_path') and Path(entry['disabled_path']).exists():
            raise RuntimeError('Isolated add-on not restored')
    return guard


def verify(run, acceptance, block_hudless, nr_hook_point, nr_off_control=False, addon_absent_control=False):
    receipt = json.loads((acceptance / 'receipt.json').read_text(encoding='utf-8-sig'))
    result = json.loads((run / 'result.json').read_text(encoding='utf-8'))
    guard = verify_restoration(run, receipt, result)
    loaded = json.loads((run / 'loaded-modules.json').read_text(encoding='utf-8'))
    if addon_absent_control:
        if loaded.get('matching_paths') or result['mod_load'] != 'baseline_addon_absence_confirmed':
            raise RuntimeError('Baseline add-on absence not confirmed')
        evidence = dict(verified_at=stamp(), diagnostic_kind='addon_absent_control',
            baseline_addon_absence_confirmed=True, original_files_verified=len(guard),
            installation_restored=receipt['restored'], safe_exit=result['safe_exit'],
            automation_result=result['result'], visual_quality='requires_user_flicker_judgment',
            nr_acceptance=False)
        dump(acceptance / 'baseline-evidence.json', evidence)
        receipt.update(baseline_verified=True, verification='baseline-evidence.json')
        dump(acceptance / 'receipt.json', receipt)
        return dict(diagnostic_kind='addon_absent_control', routing_passed=True,
            baseline_addon_absence_confirmed=True, safe_exit=result['safe_exit'],
            restored=receipt['restored'], test_changes_restored=True, candidate_retained=False,
            protected_files=len(guard), automation_result=result['result'])
    if not loaded['matching_paths'] or loaded['actual_sha256'].lower() != receipt['candidate_sha256'].lower():
        raise RuntimeError('Loaded candidate identity mismatch')
    ready = json.loads((run / 'capture-ready.json').read_text(encoding='utf-8'))
    start = datetime.fromisoformat(ready['time']).astimezone()
    events = capture_events_for_verification(run, receipt)
    capture_times = [datetime.fromisoformat(event['time']).astimezone()
                     for event in events if event.get('action') == 'capture']
    if not capture_times:
        raise RuntimeError('Capture events do not define an acceptance window')
    end = max(capture_times)
    if end <= start:
        raise RuntimeError('Capture events do not follow capture-ready')
    intervals, resizes, contracts, gpu_timer_lines, telemetry_lines = [], [], [], [], []
    telemetry_times = []
    log_entries = []
    # Only this process's newly generated acceptance log is consumed here.
    lines = (run / 'final-logs/ReShade.log').read_text(encoding='utf-8-sig', errors='replace').splitlines()
    for number, line in enumerate(lines, 1):
        match = re.match(r'(\d\d:\d\d:\d\d):(\d{3})', line)
        if not match:
            continue
        local = datetime.fromisoformat(start.date().isoformat() + 'T' + match[1] + '.' + match[2]).replace(tzinfo=start.tzinfo)
        if not start <= local < end:
            continue
        log_entries.append((number, line, match, local))
        if 'ResizeBuffers' in line or 'SetFullscreenState' in line:
            resizes.append(dict(line=number, text=line))
        if 'frame contract' in line:
            contracts.append(dict(line=number, text=line))
        if 'NR gpu timers [' in line or 'NR gpu FG frame:' in line:
            gpu_timer_lines.append(dict(line=number, local_time=match[1], text=line))
        telemetry = parse_nr_telemetry(line)
        if telemetry is not None:
            telemetry.update(line=number, local_time=match[1])
            telemetry_lines.append(telemetry)
            telemetry_times.append((local, telemetry))
    if receipt.get('disable_xefg_sdk_detours'):
        if not nr_off_control or not receipt.get('start_nr_disabled'):
            raise RuntimeError('XeFG SDK isolation requires NR Off')
        configs = [entry for entry in guard if Path(entry['path']).name.lower() == 'reshade.ini']
        if len(configs) != 1:
            raise RuntimeError('Missing unique protected startup configuration')
        config = Path(configs[0]['backup_path']).read_text(encoding='utf-8-sig')
        sections = re.findall(r'(?ms)^\[RenoDX\.DLSS5\]\r?\n(?:(?!^\[).)*', config)
        if len(sections) != 1 or any(
                re.findall(r'(?m)^' + key + r'=([^\r\n]*)', sections[0]) != ['0']
                for key in ('NRXeFGSdkDetours', 'NeuralUplift')):
            raise RuntimeError('Protected startup configuration does not disable SDK detours and NR')
        if any('DLSS5 FG interval' in line for line in lines):
            raise RuntimeError('XeFG SDK isolation unexpectedly emitted Nexus Present telemetry')
        if not telemetry_lines or any(sample['evaluates'] != 0 for sample in telemetry_lines):
            raise RuntimeError('NR Off telemetry is missing or contains Evaluate calls')
        evidence = dict(verified_at=stamp(), diagnostic_kind='xefg_sdk_detours_disabled',
            candidate_sha256=receipt['candidate_sha256'], runtime_load_verified=True,
            original_files_verified=len(guard), installation_restored=receipt['restored'],
            startup_configuration_verified=True, nr_telemetry=telemetry_lines,
            nexus_xefg_present_telemetry_absent=True,
            sdk_resource_delivery='unobserved_with_Nexus_SDK_detours_disabled',
            sdk_fg_status='unobserved_with_Nexus_SDK_detours_disabled',
            ui_composition_visual_quality='requires_manual_dynamic_comparison',
            safe_exit=result['safe_exit'], automation_result=result['result'])
        dump(acceptance / 'sdk-isolation-evidence.json', evidence)
        receipt.update(runtime_verified=True, verification='sdk-isolation-evidence.json',
            visual_quality='requires_manual_dynamic_comparison')
        dump(acceptance / 'receipt.json', receipt)
        return dict(diagnostic_kind='xefg_sdk_detours_disabled', diagnostic_completed=True,
            routing_passed=None, nr_off_control=True, safe_exit=result['safe_exit'],
            restored=receipt['restored'], test_changes_restored=True, candidate_retained=False,
            protected_files=len(guard), automation_result=result['result'])
    for number, line, match, local in log_entries:
        interval = parse_fg_interval(line)
        if interval is not None:
            if (local - start).total_seconds() < interval['seconds']:
                continue
            app, nr, displayed = (interval[key] for key in
                ('application_presents', 'nr_submitted', 'displayed_frames'))
            interval_start = local - timedelta(seconds=interval['seconds'])
            samples = [(sample_time, sample) for sample_time, sample in telemetry_times
                if interval_start < sample_time <= local]
            interval['telemetry_seconds'] = sum(sample['window_seconds'] for _, sample in samples)
            interval['telemetry_evaluates'] = sum(sample['evaluates'] for _, sample in samples)
            interval['nr_evaluates_per_service_frame'] = (
                interval['nr_service']['evals'] / interval['nr_service']['frames']
                if interval['nr_service']['frames'] else 0.0)
            interval['evaluates_per_application_frame'] = (
                interval['telemetry_evaluates'] / app if app else 0.0)
            if nr_hook_point == 0:
                if interval['telemetry_seconds'] >= interval['seconds'] * 0.5:
                    rate = interval['evaluates_per_application_frame']
                    interval['nr_state'] = 'on' if rate >= 0.75 else 'off' if rate <= 0.1 else 'mixed'
                else:
                    interval['nr_state'] = 'unknown'
            else:
                interval['nr_state'] = 'on' if app == nr else 'off' if nr == 0 else 'mixed'
            interval.update(line=number, local_time=match[1],
                sdk_4x=displayed == 4 * app)
            intervals.append(interval)
    performance = {}
    for state in ('on', 'off'):
        selected = [i for i in intervals if i['nr_state'] == state and i['sdk_enabled'] and i['sdk_4x']]
        duration = sum(i['seconds'] for i in selected)
        performance[state] = dict(windows=len(selected), seconds=duration,
            displayed_fps=sum(i['displayed_frames'] for i in selected) / duration if duration else None,
            application_fps=sum(i['application_presents'] for i in selected) / duration if duration else None)
    on = [i for i in intervals if i['nr_state'] == 'on' and i['sdk_enabled']]
    if nr_hook_point == 2:
        expected_colors = 1 if block_hudless else 2
        configured_passes = receipt.get('nr_model_test', {}).get('passes')
        routing_passed = bool(on) and all(
            i['ui_mode'] == 0 and i['ui_composition'] == 0
            and i['fresh_present_id'] == 1 and i['nr_colors'] == expected_colors
            and i['nr_service']['frames'] == i['application_presents']
            and i['nr_service']['backbuffer'] == i['nr_service']['frames']
            and i['nr_service']['hudless'] == (0 if block_hudless else i['nr_service']['frames'])
            and i['nr_service']['process'] == i['nr_service']['frames'] * expected_colors
            and i['nr_service']['evals'] == i['nr_service']['frames'] * expected_colors *
                (configured_passes if configured_passes else round(i['nr_evaluates_per_service_frame'] / expected_colors))
            and i['nr_evaluates_per_service_frame'] / expected_colors in (1, 2) for i in on)
    else:
        upscaled = [i for i in intervals if i['nr_state'] == 'on' and i['sdk_enabled'] and i['sdk_4x']]
        routing_passed = bool(upscaled) and all(
            i['telemetry_seconds'] >= i['seconds'] * 0.5
            and round(i['evaluates_per_application_frame']) in (1, 2)
            and abs(i['evaluates_per_application_frame'] - round(i['evaluates_per_application_frame'])) <= 0.25
            and (not receipt.get('nr_model_test', {}).get('passes')
                or round(i['evaluates_per_application_frame']) == receipt['nr_model_test']['passes'])
            for i in upscaled)
    if nr_off_control:
        routing_passed = bool(intervals) and all(i['nr_state'] == 'off' and i['sdk_enabled']
            and i['sdk_4x'] for i in intervals)
    performance_target = receipt['performance_target_fps']
    evidence = dict(verified_at=stamp(), candidate_sha256=receipt['candidate_sha256'],
        runtime_load_verified=True, original_files_verified=len(guard), installation_restored=receipt['restored'],
        test_changes_restored=True, candidate_retained=receipt.get('candidate_retained', False),
        block_optional_hudless=block_hudless, nr_hook_point=nr_hook_point, routing_passed=routing_passed,
        nr_off_control=nr_off_control,
        performance_target_fps=performance_target, performance=performance, intervals=intervals,
        capture_events=events, capture_window_resizes=resizes, contracts=contracts,
        gpu_timer_lines=gpu_timer_lines, telemetry_lines=telemetry_lines,
        safe_exit=result['safe_exit'], automation_result=result['result'],
        visual_quality='not_accepted; saved captures require visual judgment',
        not_accepted=['HDR/SDR and HUD visual quality', 'continuous no-flicker']
            + ([f'{performance_target:g} FPS target not evaluated (NR-Off control)'] if nr_off_control
                else [f'{performance_target:g} FPS target not met or not verified'] if not routing_passed
                    or performance['on']['displayed_fps'] is None
                    or performance['on']['displayed_fps'] < performance_target else []))
    dump(acceptance / 'rendering-evidence.json', evidence)
    receipt.update(runtime_verified=True, verification='rendering-evidence.json', visual_quality=evidence['visual_quality'])
    dump(acceptance / 'receipt.json', receipt)
    return dict(performance=performance, routing_passed=routing_passed, nr_off_control=nr_off_control,
        nr_hook_point=nr_hook_point, interval_count=len(intervals), gpu_timer_samples=len(gpu_timer_lines),
        safe_exit=result['safe_exit'],
        restored=receipt['restored'], test_changes_restored=True, candidate_retained=receipt.get('candidate_retained', False),
        protected_files=len(guard), automation_result=result['result'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate-manifest', required=True, type=Path)
    parser.add_argument('--game-binary-directory', required=True, type=Path)
    parser.add_argument('--target', type=Path, default=ROOT / '.local/game-test/witcher3-steam.json')
    parser.add_argument('--save', help='Explicit save filename for this game launch; required for ControlMask cases')
    parser.add_argument('--python', type=Path, default=Path(sys.executable))
    parser.add_argument('--output-root', type=Path, default=ROOT / 'artifacts/game-tests')
    parser.add_argument('--hdr-tool', type=Path, default=ROOT / 'tmp/hdr-state/hdr-state.exe')
    parser.add_argument('--block-optional-hudless', action='store_true')
    parser.add_argument('--xefg-ui-composition', type=int, choices=(0, 1),
        help='Temporarily set the existing XeFG UIComposition option; requires full restoration')
    parser.add_argument('--disable-xefg-sdk-detours', action='store_true',
        help='Startup-only P1-C probe: leave XeFG SDK exports and proxy Present hooks untouched')
    parser.add_argument('--nr-hook-point', type=int, choices=(0, 2), default=2,
        help='Run the NR path at Upscaled (0) or Present (2); defaults to Present')
    parser.add_argument('--gpu-timers', action='store_true',
        help='Enable the existing per-stage NR GPU timers for this restored session')
    parser.add_argument('--telemetry-seconds', type=int, default=1,
        help='Set the temporary NR telemetry interval in seconds (1..3600; default: 1)')
    parser.add_argument('--nr-passes', type=int, choices=(1, 2),
        help='Temporarily select one or two passes; omitted keeps the protected config value')
    parser.add_argument('--control-mask-case', choices=('depth-far', 'depth-far-off'),
        help='Run a protected depth-far comparison at Present or Upscaled')
    parser.add_argument('--control-mask-use-core-context', action='store_true',
        help='Use the successful host SR Core context for an Upscaled depth-far case')
    parser.add_argument('--control-mask-depth-near', type=float,
        help='Override the normalized near threshold for a protected depth-far case; requires --control-mask-depth-far')
    parser.add_argument('--control-mask-depth-far', type=float,
        help='Override the normalized far threshold for a protected depth-far case; requires --control-mask-depth-near')
    parser.add_argument('--nr-runtime-path', type=Path,
        help='Runtime file to verify for a protected ControlMask game case')
    parser.add_argument('--capture-script', type=Path,
        help='Delegate the guarded capture-ready session to a repository tools/ or tmp/ Python script')
    parser.add_argument('--start-nr-disabled', action='store_true',
        help='Start NR disabled so a delegated capture script can verify and control the state')
    parser.add_argument('--nr-preset', type=int, choices=(1, 2), default=2,
        help='NR model preset for a protected ControlMask game case (default: B)')
    parser.add_argument('--enable-streamline-fallback', action='store_true',
        help='Temporarily enable Streamline hooks for this protected ControlMask case')
    parser.add_argument('--nr-off-control', action='store_true',
        help='Keep NR Off and SDK HUDLess forwarding unchanged for a static flicker control')
    parser.add_argument('--addon-absent-control', action='store_true',
        help='Temporarily remove the project add-on and restore it after the static baseline')
    parser.add_argument('--check-addon-menu', action='store_true')
    parser.add_argument('--restore-installation', action='store_true',
        help='Forward the existing full-installation restore switch to the protected wrapper')
    args = parser.parse_args()
    if args.start_nr_disabled and args.capture_script is None:
        parser.error('--start-nr-disabled requires --capture-script')
    if args.control_mask_use_core_context and not args.control_mask_case:
        parser.error('--control-mask-use-core-context requires a protected depth-far case')
    if not 1 <= args.telemetry_seconds <= 3600:
        parser.error('--telemetry-seconds must be between 1 and 3600')
    if args.xefg_ui_composition is not None and not args.restore_installation:
        parser.error('--xefg-ui-composition requires --restore-installation')
    if args.block_optional_hudless and args.nr_hook_point != 2:
        parser.error('--block-optional-hudless applies only to the Present hook')
    if args.disable_xefg_sdk_detours and not args.nr_off_control:
        parser.error('--disable-xefg-sdk-detours requires --nr-off-control')
    if args.nr_off_control and args.block_optional_hudless:
        parser.error('--nr-off-control requires unchanged HUDLess forwarding')
    if args.addon_absent_control and not args.nr_off_control:
        parser.error('--addon-absent-control requires --nr-off-control')
    if args.check_addon_menu and args.nr_off_control:
        parser.error('--check-addon-menu requires the NR comparison run')
    if args.save is not None and (Path(args.save).name != args.save or not args.save.endswith('.sav')):
        parser.error('--save must be a .sav filename within the configured save directory')
    if args.control_mask_case and not args.save:
        parser.error('--control-mask-case requires an explicit --save for this launch')
    if (args.control_mask_depth_near is None) != (args.control_mask_depth_far is None):
        parser.error('depth threshold overrides require both --control-mask-depth-near and --control-mask-depth-far')
    if args.control_mask_depth_near is not None:
        if args.control_mask_case not in ('depth-far', 'depth-far-off'):
            parser.error('depth threshold overrides require --control-mask-case depth-far or depth-far-off')
        if (not math.isfinite(args.control_mask_depth_near) or not math.isfinite(args.control_mask_depth_far)
                or not 0 <= args.control_mask_depth_near < args.control_mask_depth_far <= 1):
            parser.error('depth thresholds must satisfy 0 <= near < far <= 1')
    if args.enable_streamline_fallback and (not args.control_mask_case or not args.restore_installation):
        parser.error('--enable-streamline-fallback requires a restored ControlMask case')
    if args.control_mask_case and (not args.restore_installation
            or not supported_control_mask_route(args.nr_hook_point,
                args.control_mask_use_core_context, args.xefg_ui_composition)
            or args.block_optional_hudless or args.nr_off_control or args.disable_xefg_sdk_detours
            or args.nr_runtime_path is None or not args.nr_runtime_path.is_file() or args.nr_passes != 1):
        parser.error('depth-far cases require restore, a supported one-pass route, both colors and an NR Runtime')
    output_root = local_output_root(args.output_root)
    target = json.loads(args.target.read_text(encoding='utf-8'))
    expected_monitor = target['expected_hdr_monitor']
    if not expected_monitor:
        raise ValueError('Set expected_hdr_monitor in the local target')
    out = output_root / time.strftime('automated-witcher-%Y%m%dT%H%M%SZ', time.gmtime())
    out.mkdir()
    display = subprocess.run([str(args.hdr_tool.resolve(strict=True)), 'query'],
        capture_output=True, encoding='utf-8', check=True, timeout=15)
    display = json.loads(display.stdout)
    dump(out / 'display.json', display)
    selected_display = select_hdr_target(display, target)
    dump(out / 'selected-display.json', selected_display)
    command = ['pwsh.exe', '-NoProfile', '-File', str(ROOT / 'tools/run-fg-acceptance.ps1'),
        '-CandidateManifest', str(args.candidate_manifest.resolve(strict=True)),
        '-GameBinaryDirectory', str(args.game_binary_directory.resolve(strict=True)),
        '-Python', str(args.python.resolve(strict=True)), '-TargetPath', str(args.target.resolve(strict=True)),
        '-OutputRoot', str(output_root),
        '-CaptureHoldSeconds', '90', '-CleanEnvironment', '-ShowGameHud',
        '-NrHookPoint', str(args.nr_hook_point), '-TelemetrySeconds', str(args.telemetry_seconds)]
    if args.save:
        command += ['-Save', args.save]
    if args.nr_passes is not None:
        command += ['-NrPasses', str(args.nr_passes)]
    if args.capture_script is not None:
        command += ['-CaptureScript', str(args.capture_script.resolve(strict=True))]
    if args.start_nr_disabled:
        command.append('-StartNrDisabled')
    if args.control_mask_case:
        command += ['-NrPreset', str(args.nr_preset), '-NrRuntimePath', str(args.nr_runtime_path.resolve(strict=True)),
            '-ControlMaskCase', 'DepthFar' if args.control_mask_case == 'depth-far' else 'DepthFarOff']
        if args.nr_hook_point == 0:
            command.append('-SrOnly')
    if args.control_mask_use_core_context:
        command.append('-ControlMaskUseCoreContext')
    if args.control_mask_depth_near is not None:
        command += ['-ControlMaskDepthNear', format(args.control_mask_depth_near, '.9g'),
            '-ControlMaskDepthFar', format(args.control_mask_depth_far, '.9g')]
    if args.enable_streamline_fallback:
        command.append('-EnableStreamlineFallback')
    if args.xefg_ui_composition is not None:
        command += ['-XeFGUiComposition', str(args.xefg_ui_composition)]
    if args.gpu_timers:
        command.append('-GpuTimers')
    if args.block_optional_hudless:
        command.append('-BlockOptionalHudless')
    if args.disable_xefg_sdk_detours:
        command.append('-DisableXeFGSdkDetours')
    if args.nr_off_control:
        command.append('-StartNrDisabled')
    if args.addon_absent_control:
        command.append('-AddonAbsentControl')
    if args.restore_installation:
        command.append('-RestoreInstallation')
    dump(out / 'command.json', command)
    if args.control_mask_case:
        dump(out / 'input-language-preflight.json', desktop.activate_english_input_language())
    print('ORCHESTRATION=' + str(out), flush=True)
    acceptance = run = None
    capture_error = None
    child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        encoding='utf-8', errors='replace', creationflags=0x08000000)
    with (out / 'wrapper.stdout.log').open('w', encoding='utf-8') as log:
        for line in child.stdout:
            log.write(line)
            log.flush()
            text = line.strip()
            if text.startswith('ACCEPTANCE_RECEIPT='):
                acceptance = Path(text.split('=', 1)[1]).parent
            elif text.startswith('RUN_DIRECTORY='):
                run = Path(text.split('=', 1)[1])
            elif text.startswith('CAPTURE_READY='):
                run = Path(text.split('=', 1)[1])
                print('CAPTURE_STARTED=' + str(run), flush=True)
                if args.capture_script is not None:
                    continue
                try:
                    capture(run, args.nr_off_control, args.check_addon_menu)
                except Exception as error:
                    capture_error = repr(error)
                    dump(out / 'capture-error.json', dict(time=stamp(), error=capture_error,
                        diagnostics=getattr(error, 'diagnostics', None)))
                    print('CAPTURE_FAILED; automatic game input is blocked; protected recovery will restore', flush=True)
                else:
                    print('CAPTURE_FINISHED; valid completion recorded; protected worker resumes', flush=True)
    child.wait()
    summary = dict(time=stamp(), exit_code=child.returncode, acceptance=str(acceptance), run=str(run),
        selected_save=args.save, capture_error=capture_error)
    if acceptance and run and (run / 'capture-ready.json').exists():
        try:
            summary.update(verify(run, acceptance, args.block_optional_hudless, args.nr_hook_point,
                args.nr_off_control, args.addon_absent_control))
        except Exception as error:
            summary['verification_error'] = repr(error)
    elif acceptance:
        receipt = json.loads((acceptance / 'receipt.json').read_text(encoding='utf-8-sig'))
        summary['restored'] = receipt['restored']
        if run and (run / 'result.json').exists():
            result = json.loads((run / 'result.json').read_text(encoding='utf-8'))
            summary.update(automation_result=result['result'], error=result.get('error'), safe_exit=result['safe_exit'])
    dump(out / 'result.json', summary)
    print(json.dumps(summary), flush=True)
    completed = summary.get('routing_passed') or summary.get('diagnostic_completed')
    return 0 if child.returncode == 0 and not capture_error and 'verification_error' not in summary and completed else 1


if __name__ == '__main__':
    sys.exit(main())
