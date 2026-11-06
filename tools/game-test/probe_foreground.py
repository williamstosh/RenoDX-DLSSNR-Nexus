"""Bounded native foreground probe using two disposable fixture windows, no game input."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time


def fixture(title):
    import tkinter as tk
    root = tk.Tk()
    root.title(title)
    root.geometry('380x110+100+100')
    tk.Label(root, text='NR Nexus foreground test fixture\nNo game input is sent.').pack(pady=22)
    root.update()
    user = ctypes.WinDLL('user32')
    user.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    user.GetAncestor.restype = ctypes.c_void_p
    hwnd = user.GetAncestor(root.winfo_id(), 2)
    print(json.dumps(dict(pid=os.getpid(), hwnd=hwnd)), flush=True)
    root.after(20000, root.destroy)
    root.mainloop()


def probe(output):
    import desktop
    from runner import dump, stamp, local_output_root
    out = local_output_root(output)
    report = dict(started=stamp(), result='failed', game_started=False, input_sent=False, cases=[])
    fixtures = []
    original = [w for w in desktop.windows() if w['hwnd'] == desktop.u.GetForegroundWindow()]

    def launch(title):
        child = subprocess.Popen([sys.executable, '-B', str(Path(__file__).resolve()), '--fixture', title],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                 creationflags=0x08000000)
        entry = dict(child=child, window=None)
        fixtures.append(entry)
        messages = queue.Queue()
        threading.Thread(target=lambda: messages.put(child.stdout.readline()), daemon=True).start()
        data = json.loads(messages.get(timeout=6))
        metrics = desktop.process_metrics(data['pid'])
        if metrics is None:
            raise RuntimeError('Fixture process exited before discovery')
        window = dict(**data, created=metrics['created'], path=desktop.process_path(data['pid']), title=title)
        if not desktop.window_alive(window):
            raise RuntimeError('Fixture window identity does not match')
        entry['window'] = window
        return window

    try:
        target = launch('NR Nexus foreground probe - target')
        cover = launch('NR Nexus foreground probe - cover')
        desktop.ensure_foreground(cover)
        if desktop.u.GetForegroundWindow() == target['hwnd']:
            raise RuntimeError('Target was not behind the cover fixture')
        receipt = desktop.ensure_foreground(target)
        report['cases'].append(dict(case='background_to_foreground', result='passed', **receipt))
        desktop.u.ShowWindow(target['hwnd'], 6)
        deadline = time.monotonic() + 2
        while not desktop.u.IsIconic(target['hwnd']) and time.monotonic() < deadline:
            time.sleep(.02)
        if not desktop.u.IsIconic(target['hwnd']):
            raise RuntimeError('Fixture did not minimize; scenario was not established')
        receipt = desktop.ensure_foreground(target)
        if desktop.u.IsIconic(target['hwnd']):
            raise RuntimeError('Target remained minimized after activation')
        report['cases'].append(dict(case='minimized_to_foreground', result='passed', **receipt))
        desktop.ensure_foreground(cover)
        receipt = desktop.ensure_foreground(target)
        report['cases'].append(dict(case='foreground_reacquisition', result='passed', **receipt))
        invalid = target | dict(created=target['created'] + 1)
        try:
            desktop.ensure_foreground(invalid)
        except RuntimeError:
            report['cases'].append(dict(case='replaced_process_rejected', result='passed'))
        else:
            raise RuntimeError('Altered process identity was accepted')
        report['result'] = 'passed'
    except Exception as error:
        report['error'] = repr(error)
    finally:
        cleanup = []
        for entry in reversed(fixtures):
            child, window = entry['child'], entry['window']
            try:
                if window and desktop.window_alive(window):
                    desktop.close(window)
                child.wait(timeout=6)
                cleanup.append(dict(pid=window['pid'] if window else child.pid, exited=True))
            except Exception as error:
                report['result'] = 'failed'
                cleanup.append(dict(pid=child.pid, error=repr(error)))
            finally:
                child.stdout.close()
                child.stderr.close()
        report['cleanup'] = cleanup
        if len(original) == 1 and desktop.window_alive(original[0]):
            try:
                desktop.ensure_foreground(original[0])
            except RuntimeError as error:
                report['foreground_restore_error'] = repr(error)
        report['finished'] = stamp()
        dump(out / 'result.json', report)
    print(json.dumps(report), flush=True)
    return 0 if report['result'] == 'passed' else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture')
    parser.add_argument('--output-root', type=Path)
    args = parser.parse_args()
    if args.fixture:
        fixture(args.fixture)
    else:
        if args.output_root is None:
            parser.error('--output-root is required')
        sys.exit(probe(args.output_root))
