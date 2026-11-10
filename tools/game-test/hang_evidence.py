"""Capture a bounded Windows minidump for one verified process identity."""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import subprocess
import sys

MINIDUMP_TYPE = 0x1000 | 0x20  # WithThreadInfo | WithUnloadedModules
PROCESS_ACCESS = 0x0400 | 0x0010 | 0x100000  # QUERY_INFORMATION | VM_READ | SYNCHRONIZE


def _write_manifest(destination, record):
    path = destination / "manifest.json"
    with path.open("x", encoding="utf-8") as stream:
        json.dump(record, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def _record_parent_result(destination, record):
    try:
        _write_manifest(destination, record)
    except FileExistsError:
        pass
    except OSError as error:
        record["manifest_error"] = f"{type(error).__name__}: {error}"
    return record


def _worker(destination, identity):
    record = {"status": "failed", "process": identity}
    process_handle = dump_handle = None
    kernel = None
    dump_path = destination / "game.dmp"
    opened_dump = False
    try:
        if os.name != "nt":
            raise OSError("Windows is required")
        import ctypes.wintypes as W

        kernel = C.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
        kernel.OpenProcess.restype = W.HANDLE
        kernel.CloseHandle.argtypes = [W.HANDLE]
        kernel.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
        kernel.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
        kernel.WaitForSingleObject.argtypes = [W.HANDLE, W.DWORD]
        kernel.WaitForSingleObject.restype = W.DWORD
        kernel.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE]
        kernel.CreateFileW.restype = W.HANDLE

        process_handle = kernel.OpenProcess(PROCESS_ACCESS, False, int(identity["pid"]))
        if not process_handle:
            raise C.WinError(C.get_last_error())
        if kernel.WaitForSingleObject(process_handle, 0) != 0x102:
            record["status"] = "identity_mismatch"
            record["reason"] = "process_not_running"
        else:
            created, exited, kernel_time, user_time = (W.FILETIME() for _ in range(4))
            if not kernel.GetProcessTimes(process_handle, C.byref(created), C.byref(exited),
                                          C.byref(kernel_time), C.byref(user_time)):
                raise C.WinError(C.get_last_error())
            created_value = (created.dwHighDateTime << 32) | created.dwLowDateTime
            image, size = C.create_unicode_buffer(32768), W.DWORD(32768)
            if not kernel.QueryFullProcessImageNameW(process_handle, 0, image, C.byref(size)):
                raise C.WinError(C.get_last_error())
            if (created_value != int(identity["created"]) or image.value != identity["path"]
                    or kernel.WaitForSingleObject(process_handle, 0) != 0x102):
                record["status"] = "identity_mismatch"
                record["reason"] = "created_path_or_running_state_mismatch"
            elif (destination / "manifest.json").exists():
                record["status"] = "already_exists"
                record["reason"] = "manifest_exists"
            else:
                dump_handle = kernel.CreateFileW(str(dump_path), 0x40000000, 0, None, 1, 0x80, None)
                if dump_handle == C.c_void_p(-1).value:
                    dump_handle = None
                    error = C.get_last_error()
                    if error == 80 or error == 183:
                        record["status"] = "already_exists"
                        record["reason"] = "dump_exists"
                    else:
                        raise C.WinError(error)
                else:
                    opened_dump = True
                    dbghelp = C.WinDLL("Dbghelp", use_last_error=True)
                    write_dump = dbghelp.MiniDumpWriteDump
                    write_dump.argtypes = [W.HANDLE, W.DWORD, W.HANDLE, W.DWORD, C.c_void_p, C.c_void_p, C.c_void_p]
                    write_dump.restype = W.BOOL
                    record["dump_type"] = MINIDUMP_TYPE
                    if not write_dump(process_handle, int(identity["pid"]), dump_handle, MINIDUMP_TYPE, None, None, None):
                        raise C.WinError(C.get_last_error())
                    record["status"] = "captured"
                    record["dump"] = "game.dmp"
    except Exception as error:
        record["status"] = "failed"
        record["error"] = f"{type(error).__name__}: {error}"
    finally:
        if dump_handle:
            kernel.CloseHandle(dump_handle)
            dump_handle = None
        if process_handle:
            kernel.CloseHandle(process_handle)
        if opened_dump and record["status"] != "captured":
            try:
                dump_path.unlink()
            except OSError:
                record["partial_dump"] = True
        if record["status"] == "captured":
            record["bytes"] = dump_path.stat().st_size
        try:
            _write_manifest(destination, record)
        except FileExistsError:
            record["status"] = "already_exists"
            record["reason"] = "manifest_exists"
        except OSError as error:
            record["status"] = "failed"
            record["error"] = f"manifest: {type(error).__name__}: {error}"
    return record


def capture(process, destination, timeout=20):
    """Run one isolated helper and return its explicit capture outcome."""
    try:
        destination = Path(destination)
        destination.mkdir(parents=True, exist_ok=True)
        identity = {key: process[key] for key in ("pid", "created", "path")}
        if (destination / "game.dmp").exists() or (destination / "manifest.json").exists():
            record = {"status": "already_exists", "reason": "evidence_exists", "process": identity}
            try:
                _write_manifest(destination, record)
            except FileExistsError:
                pass
            return record
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000) if os.name == "nt" else 0
        helper = subprocess.Popen([sys.executable, "-B", str(Path(__file__).resolve()), "--worker", str(destination)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, encoding="utf-8", creationflags=creationflags)
    except Exception as error:
        return {"status": "failed", "error": f"{type(error).__name__}: {error}"}
    try:
        stdout, stderr = helper.communicate(json.dumps(identity, ensure_ascii=True), timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            helper.kill()
        finally:
            helper.wait()
        record = {"status": "timeout", "process": identity}
        if (destination / "game.dmp").exists():
            record["partial_dump"] = True
        try:
            _write_manifest(destination, record)
        except FileExistsError:
            record["status"] = "already_exists"
        except OSError as error:
            record["manifest_error"] = f"{type(error).__name__}: {error}"
        return record
    except BaseException as error:
        try:
            helper.kill()
        finally:
            helper.wait()
        if isinstance(error, (KeyboardInterrupt, SystemExit)):
            raise
        return _record_parent_result(destination,
                                     {"status": "failed", "reason": "helper_communication_failed",
                                      "error": f"{type(error).__name__}: {error}", "process": identity})
    try:
        record = json.loads(stdout)
    except (TypeError, json.JSONDecodeError):
        record = {"status": "failed", "reason": "helper_invalid_response",
                  "returncode": helper.returncode, "process": identity}
        if stderr:
            record["helper_stderr"] = stderr[-1000:]
        return _record_parent_result(destination, record)
    if not isinstance(record, dict) or not isinstance(record.get("status"), str):
        return _record_parent_result(destination,
                                     {"status": "failed", "reason": "helper_invalid_response",
                                      "returncode": helper.returncode, "process": identity})
    if helper.returncode != 0 and record.get("status") == "captured":
        record = {"status": "failed", "reason": "helper_exit_nonzero",
                  "returncode": helper.returncode, "process": identity}
        return _record_parent_result(destination, record)
    return record


def main(argv=None):
    parser = argparse.ArgumentParser()
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--worker", metavar="DESTINATION")
    group.add_argument("--process", metavar="OWNED_PROCESS_JSON")
    parser.add_argument("--output")
    args = parser.parse_args(argv)
    if args.worker:
        try:
            identity = json.load(sys.stdin)
            record = _worker(Path(args.worker), identity)
        except Exception as error:
            record = {"status": "failed", "error": f"{type(error).__name__}: {error}"}
    else:
        if not args.output:
            parser.error("--process requires --output")
        try:
            with open(args.process, encoding="utf-8") as stream:
                process = json.load(stream)
            record = capture(process, args.output)
        except Exception as error:
            record = {"status": "failed", "error": f"{type(error).__name__}: {error}"}
    print(json.dumps(record, ensure_ascii=True))
    return 0 if record.get("status") == "captured" else 1


if __name__ == "__main__":
    raise SystemExit(main())
