"""Start a protected entry point in the shell's filesystem context, without UI."""
import argparse
import codecs
import ctypes
from ctypes import wintypes
import io
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
JOBS = ROOT / 'tmp' / 'normal-process'
MARKER = 'NR_NEXUS_NORMAL_USER_CONTEXT'


class STARTUPINFO(ctypes.Structure):
    _fields_ = [('cb', wintypes.DWORD), ('reserved', wintypes.LPWSTR),
                ('desktop', wintypes.LPWSTR), ('title', wintypes.LPWSTR),
                ('x', wintypes.DWORD), ('y', wintypes.DWORD),
                ('width', wintypes.DWORD), ('height', wintypes.DWORD),
                ('columns', wintypes.DWORD), ('rows', wintypes.DWORD),
                ('fill', wintypes.DWORD), ('flags', wintypes.DWORD),
                ('show', wintypes.WORD), ('reserved_size', wintypes.WORD),
                ('reserved_data', ctypes.POINTER(ctypes.c_byte)),
                ('stdin', wintypes.HANDLE), ('stdout', wintypes.HANDLE), ('stderr', wintypes.HANDLE)]


class STARTUPINFOEX(ctypes.Structure):
    _fields_ = [('startup', STARTUPINFO), ('attributes', ctypes.c_void_p)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [('process', wintypes.HANDLE), ('thread', wintypes.HANDLE),
                ('pid', wintypes.DWORD), ('tid', wintypes.DWORD)]


if os.name == 'nt':
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    user = ctypes.WinDLL('user32', use_last_error=True)
    user.GetShellWindow.argtypes = []
    user.GetShellWindow.restype = wintypes.HWND
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetWindowThreadProcessId.restype = wintypes.DWORD
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    kernel.InitializeProcThreadAttributeList.argtypes = [ctypes.c_void_p, wintypes.DWORD,
                                                       wintypes.DWORD, ctypes.POINTER(ctypes.c_size_t)]
    kernel.InitializeProcThreadAttributeList.restype = wintypes.BOOL
    kernel.UpdateProcThreadAttribute.argtypes = [ctypes.c_void_p, wintypes.DWORD, ctypes.c_size_t,
                                               ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p]
    kernel.UpdateProcThreadAttribute.restype = wintypes.BOOL
    kernel.DeleteProcThreadAttributeList.argtypes = [ctypes.c_void_p]
    kernel.DeleteProcThreadAttributeList.restype = None
    kernel.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p,
                                     ctypes.c_void_p, wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p,
                                     wintypes.LPCWSTR, ctypes.POINTER(STARTUPINFOEX), ctypes.POINTER(PROCESS_INFORMATION)]
    kernel.CreateProcessW.restype = wintypes.BOOL
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel.GetExitCodeProcess.restype = wintypes.BOOL
    kernel.GetFinalPathNameByHandleW.argtypes = [wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD]
    kernel.GetFinalPathNameByHandleW.restype = wintypes.DWORD


def probe():
    """Test an actual unique file; package identity is not a filesystem oracle."""
    if os.name != 'nt':
        raise OSError('Normal-context launching requires Windows.')
    import msvcrt
    directory = Path(os.environ['LOCALAPPDATA']) / 'NR Nexus'
    created_directory = False
    try:
        directory.mkdir()
        created_directory = True
    except FileExistsError:
        pass
    path = directory / ('.normal-context-' + uuid.uuid4().hex + '.tmp')
    created_file = False
    try:
        with path.open('xb') as stream:
            created_file = True
            stream.write(b'NR Nexus synthetic filesystem context probe\n')
            stream.flush()
            handle = msvcrt.get_osfhandle(stream.fileno())
            size = kernel.GetFinalPathNameByHandleW(handle, None, 0, 0)
            if not size:
                raise ctypes.WinError(ctypes.get_last_error())
            buffer = ctypes.create_unicode_buffer(size + 1)
            length = kernel.GetFinalPathNameByHandleW(handle, buffer, len(buffer), 0)
            if not length or length >= len(buffer):
                raise OSError('Cannot obtain the probe file final path.')
            actual = buffer.value
        if actual.startswith('\\\\?\\UNC\\'):
            actual = '\\\\' + actual[8:]
        elif actual.startswith('\\\\?\\'):
            actual = actual[4:]
        expected = os.path.abspath(path)
        return dict(pid=os.getpid(), expected=expected, actual=actual,
                    redirected=os.path.normcase(os.path.normpath(actual)) != os.path.normcase(os.path.normpath(expected)))
    finally:
        if created_file:
            path.unlink()
        if created_directory:
            directory.rmdir()


def require_normal_context():
    result = probe()
    if result['redirected']:
        raise RuntimeError('Filesystem redirection detected; use the normal-context protected entry point. '
                           f"Expected {result['expected']}; actual {result['actual']}")
    return result


def launch(argv, cwd=None):
    """Return a process HANDLE; caller must wait and CloseHandle, never terminate."""
    if os.name != 'nt':
        raise OSError('Normal-context launching requires Windows.')
    cwd = Path.cwd() if cwd is None else Path(cwd)
    shell = user.GetShellWindow()
    if not shell:
        raise RuntimeError('No shell window exists; refusing a fallback launch.')
    pid = wintypes.DWORD()
    if not user.GetWindowThreadProcessId(shell, ctypes.byref(pid)) or not pid.value:
        raise ctypes.WinError(ctypes.get_last_error())
    parent = kernel.OpenProcess(0x0080, False, pid.value)  # PROCESS_CREATE_PROCESS
    if not parent:
        raise ctypes.WinError(ctypes.get_last_error())
    initialized = False
    try:
        size = ctypes.c_size_t()
        kernel.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(size))
        if not size.value:
            raise ctypes.WinError(ctypes.get_last_error())
        storage = ctypes.create_string_buffer(size.value)
        if not kernel.InitializeProcThreadAttributeList(storage, 1, 0, ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        initialized = True
        parent_value = wintypes.HANDLE(parent)
        if not kernel.UpdateProcThreadAttribute(storage, 0, 0x00020000, ctypes.byref(parent_value),
                                                ctypes.sizeof(parent_value), None, None):
            raise ctypes.WinError(ctypes.get_last_error())
        startup = STARTUPINFOEX()
        startup.startup.cb = ctypes.sizeof(startup)
        startup.attributes = ctypes.cast(storage, ctypes.c_void_p)
        process = PROCESS_INFORMATION()
        command = ctypes.create_unicode_buffer(subprocess.list2cmdline([str(arg) for arg in argv]))
        if not kernel.CreateProcessW(str(argv[0]), command, None, None, False, 0x08080000,
                                     None, str(cwd), ctypes.byref(startup), ctypes.byref(process)):
            raise ctypes.WinError(ctypes.get_last_error())
        kernel.CloseHandle(process.thread)
        return process.process
    finally:
        if initialized:
            kernel.DeleteProcThreadAttributeList(storage)
        kernel.CloseHandle(parent)


def worker(job_path):
    job_path = Path(job_path).resolve(strict=True)
    directory = job_path.parent
    if (job_path.name != 'job.json' or directory.parent != JOBS.resolve()
            or uuid.UUID(directory.name).hex != directory.name):
        raise ValueError('Worker jobs must be ROOT/tmp/normal-process/<uuid>/job.json.')
    result = dict(exitcode=1, pid=os.getpid())
    with (directory / 'stdout.log').open('ab', buffering=0) as stdout, (directory / 'stderr.log').open('ab', buffering=0) as stderr:
        try:
            result['filesystem_context'] = require_normal_context()
            job = json.loads(job_path.read_text(encoding='utf-8'))
            environment = os.environ.copy()
            environment[MARKER] = '1'
            environment['PYTHONIOENCODING'] = 'utf-8'
            child = subprocess.run(job['argv'], cwd=job['cwd'], env=environment, stdin=subprocess.DEVNULL,
                                   stdout=stdout, stderr=stderr, creationflags=0x08000000)
            result['exitcode'] = child.returncode
        except Exception as error:
            result['error'] = str(error)
            stderr.write((str(error) + '\n').encode('utf-8', errors='replace'))
    (directory / 'result.json').write_text(json.dumps(result, ensure_ascii=False), encoding='utf-8')
    return result['exitcode']


def run(argv=None, *, entry=None, parameters=None):
    """Run one complete entry, streaming logs while its protected lifetime lasts."""
    cwd = str(Path.cwd())
    directory = JOBS / uuid.uuid4().hex
    directory.mkdir(parents=True)
    if entry is not None:
        entry = Path(entry)
        parameters = Path(parameters)
        if not entry.is_absolute() or entry.suffix.lower() != '.ps1' or not entry.is_file():
            raise ValueError('Entry must be an existing absolute .ps1 path.')
        if not parameters.is_absolute() or not parameters.is_file():
            raise ValueError('Parameters must be an existing absolute JSON path.')
        data = json.loads(parameters.read_text(encoding='utf-8-sig'))
        if not isinstance(data, dict):
            raise ValueError('Entry parameters must be a JSON object.')
        input_path = directory / 'input.json'
        input_path.write_text(json.dumps(data, ensure_ascii=False), encoding='utf-8')
        # Only literal paths enter the command text; JSON values remain data.
        quoted_entry = "'" + str(entry).replace("'", "''") + "'"
        quoted_input = "'" + str(input_path).replace("'", "''") + "'"
        executable = shutil.which('pwsh.exe')
        if executable is None:
            raise FileNotFoundError('pwsh.exe is required for --entry.')
        command = ("$ErrorActionPreference='Stop'; [Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); "
                   "$OutputEncoding=[Console]::OutputEncoding; $params=Get-Content -LiteralPath " + quoted_input
                   + " -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable; $global:LASTEXITCODE=0; & "
                   + quoted_entry + " @params; exit $LASTEXITCODE")
        argv = [executable, '-NoProfile', '-NonInteractive', '-Command', command]
    if not argv or any(not isinstance(arg, str) or '\0' in arg for arg in argv):
        raise ValueError('Run requires a nonempty argv of strings without NUL.')
    job_path = directory / 'job.json'
    job_path.write_text(json.dumps(dict(argv=argv, cwd=cwd), ensure_ascii=False), encoding='utf-8')
    paths = [directory / 'stdout.log', directory / 'stderr.log']
    for path in paths:
        path.touch(exist_ok=False)
    # The worker has no console; Ctrl+C must not abandon its protection/recovery.
    signals = [signal.SIGINT]
    if hasattr(signal, 'SIGBREAK'):
        signals.append(signal.SIGBREAK)
    previous = {number: signal.signal(number, signal.SIG_IGN) for number in signals}
    handle = None
    streams = []
    try:
        handle = launch([sys.executable, '-B', str(Path(__file__).resolve()), '--worker', str(job_path)], cwd=cwd)
        streams = [path.open('rb') for path in paths]
        # Normalize CRLF across chunk boundaries before TextIO adds console
        # newlines again; receipts such as CAPTURE_READY must remain one line.
        decoders = [io.IncrementalNewlineDecoder(codecs.getincrementaldecoder('utf-8')(errors='replace'), True)
                    for _ in paths]
        outputs = [sys.stdout, sys.stderr]
        while True:
            status = kernel.WaitForSingleObject(handle, 125)
            if status not in (0, 258):
                raise ctypes.WinError(ctypes.get_last_error())
            for index, stream in enumerate(streams):
                while chunk := stream.read(65536):
                    text = decoders[index].decode(chunk)
                    if outputs[index] is not None:
                        try:
                            outputs[index].write(text)
                            outputs[index].flush()
                        except (OSError, UnicodeError):
                            outputs[index] = None  # Raw logs remain intact; still wait for recovery.
                if status == 0 and outputs[index] is not None:
                    try:
                        outputs[index].write(decoders[index].decode(b'', final=True))
                        outputs[index].flush()
                    except (OSError, UnicodeError):
                        outputs[index] = None
            if status == 0:
                break
        code = wintypes.DWORD()
        if not kernel.GetExitCodeProcess(handle, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        result = json.loads((directory / 'result.json').read_text(encoding='utf-8'))
        if code.value != (result['exitcode'] & 0xffffffff):
            raise RuntimeError('Worker exit code does not match its result receipt.')
        return result['exitcode']
    finally:
        if handle is not None:
            kernel.WaitForSingleObject(handle, 0xffffffff)
            kernel.CloseHandle(handle)
        for stream in streams:
            stream.close()
        for number, handler in previous.items():
            signal.signal(number, handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--probe', action='store_true')
    mode.add_argument('--run', nargs=argparse.REMAINDER)
    mode.add_argument('--worker', type=Path)
    mode.add_argument('--entry', type=Path)
    parser.add_argument('--parameters', type=Path)
    args = parser.parse_args()
    try:
        if args.probe:
            result = probe()
            print(json.dumps(result, ensure_ascii=False), flush=True)
            return 2 if result['redirected'] else 0
        if args.worker:
            return worker(args.worker)
        if args.entry:
            if args.parameters is None:
                raise ValueError('--entry requires --parameters.')
            return run(entry=args.entry, parameters=args.parameters)
        return run(args.run)
    except Exception as error:
        if args.probe:
            print(json.dumps(dict(error=str(error)), ensure_ascii=False), flush=True)
        else:
            print(f'Normal-context launch failed: {error}', file=sys.stderr, flush=True)
        return 1


if __name__ == '__main__':
    for output in (sys.stdout, sys.stderr):
        if output is not None:
            output.reconfigure(encoding='utf-8', errors='replace')
    sys.exit(main())
