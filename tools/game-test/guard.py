"""Windows file protection for explicitly enumerated, existing files.

Only listed files are protected. Callers must keep their parent directories
stable and stop game processes before leaving the context. Backups are never
automatically restored; partial backups are retained if setup fails.
"""

import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import stat


GENERIC_READ = 0x80000000
FILE_SHARE_READ = 0x00000001
OPEN_EXISTING = 3
FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000
FILE_ATTRIBUTE_REPARSE_POINT = 0x400
FILE_ATTRIBUTE_DIRECTORY = 0x10
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class _FileInfo(ctypes.Structure):
    _fields_ = [
        ("attributes", wintypes.DWORD),
        ("created", wintypes.FILETIME),
        ("accessed", wintypes.FILETIME),
        ("written", wintypes.FILETIME),
        ("volume", wintypes.DWORD),
        ("size_high", wintypes.DWORD),
        ("size_low", wintypes.DWORD),
        ("links", wintypes.DWORD),
        ("index_high", wintypes.DWORD),
        ("index_low", wintypes.DWORD),
    ]


if os.name == "nt":
    _kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    _kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD,
                                   wintypes.DWORD, ctypes.c_void_p,
                                   wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    _kernel.CreateFileW.restype = wintypes.HANDLE
    _kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    _kernel.CloseHandle.restype = wintypes.BOOL
    _kernel.GetFileInformationByHandle.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(_FileInfo)]
    _kernel.GetFileInformationByHandle.restype = wintypes.BOOL
    _kernel.SetFilePointerEx.argtypes = [wintypes.HANDLE, ctypes.c_longlong,
                                        ctypes.c_void_p, wintypes.DWORD]
    _kernel.SetFilePointerEx.restype = wintypes.BOOL
    _kernel.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                                wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                ctypes.c_void_p]
    _kernel.ReadFile.restype = wintypes.BOOL


def _plain_path(path, *, file=False):
    # Do not resolve(): doing so would hide symlinks/junctions from inspection.
    path = Path(os.path.abspath(os.fspath(path)))
    for part in (*reversed(path.parents), path):
        try:
            info = part.lstat()
        except FileNotFoundError:
            if file:
                raise
            continue
        if stat.S_ISLNK(info.st_mode) or (
                getattr(info, "st_file_attributes", 0) & FILE_ATTRIBUTE_REPARSE_POINT):
            raise ValueError("Reparse points are forbidden: {}".format(part))
    if file and not stat.S_ISREG(path.stat().st_mode):
        raise ValueError("Expected an existing regular file: {}".format(path))
    return path


def _open_locked(path):
    _plain_path(path, file=True)
    handle = _kernel.CreateFileW(str(path), GENERIC_READ, FILE_SHARE_READ,
                                 None, OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT, None)
    if handle == INVALID_HANDLE_VALUE:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        info = _FileInfo()
        if not _kernel.GetFileInformationByHandle(handle, ctypes.byref(info)):
            raise ctypes.WinError(ctypes.get_last_error())
        if info.attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY):
            raise ValueError("Expected a plain file: {}".format(path))
        _plain_path(path, file=True)
        return handle, (info.volume, info.index_high, info.index_low)
    except BaseException:
        _kernel.CloseHandle(handle)
        raise


def _stream(handle, destination=None):
    if not _kernel.SetFilePointerEx(handle, 0, None, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    digest = hashlib.sha256()
    size = 0
    buffer = ctypes.create_string_buffer(1024 * 1024)
    count = wintypes.DWORD()
    while True:
        if not _kernel.ReadFile(handle, buffer, len(buffer), ctypes.byref(count), None):
            raise ctypes.WinError(ctypes.get_last_error())
        if not count.value:
            return digest.hexdigest(), size
        chunk = buffer.raw[:count.value]
        digest.update(chunk)
        size += len(chunk)
        if destination is not None:
            destination.write(chunk)


def _backup_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class ProtectedFiles:
    """Hold read-only sharing locks, verified backups, and exit hash checks.

    ``manifest`` is a list of dictionaries with path, backup_path, sha256,
    size, and verified fields. manifest.json is written only after all backup
    files have been independently hashed. Instances are single-use.
    """

    def __init__(self, paths, backup_dir):
        if os.name != "nt":
            raise OSError("ProtectedFiles requires Windows")
        self._paths = list(paths)
        self._backup_dir = backup_dir
        self._handles = []
        self._manifest = []
        self._used = False

    @property
    def manifest(self):
        return [dict(entry) for entry in self._manifest]

    def _empty_destination(self):
        directory = _plain_path(self._backup_dir)
        if directory.exists() and (not directory.is_dir() or any(directory.iterdir())):
            raise FileExistsError("Backup directory must be absent or empty: {}".format(directory))
        return directory

    def _release(self):
        errors = []
        while self._handles:
            _, handle, _ = self._handles.pop()
            if not _kernel.CloseHandle(handle):
                errors.append(ctypes.WinError(ctypes.get_last_error()))
        if errors:
            raise errors[0]

    def __enter__(self):
        if self._used:
            raise RuntimeError("ProtectedFiles instances are single-use")
        self._used = True
        try:
            self._empty_destination()
            for raw_path in self._paths:
                path = _plain_path(raw_path, file=True)
                handle, identity = _open_locked(path)
                self._handles.append((path, handle, identity))
            directory = self._empty_destination()
            directory.mkdir(parents=True, exist_ok=True)
            _plain_path(directory)
            entries = []
            for index, (path, handle, _) in enumerate(self._handles):
                backup = directory / ("{:06d}.bin".format(index))
                with backup.open("xb") as output:
                    digest, size = _stream(handle, output)
                    output.flush()
                    os.fsync(output.fileno())
                if _backup_hash(backup) != digest or backup.stat().st_size != size:
                    raise RuntimeError("Backup verification failed: {}".format(backup))
                entries.append(dict(path=str(path), backup_path=str(backup),
                                    sha256=digest, size=size, verified=True))
            with (directory / "manifest.json").open("x", encoding="utf-8") as output:
                json.dump(entries, output, indent=2, ensure_ascii=True)
                output.write("\n")
                output.flush()
                os.fsync(output.fileno())
            self._manifest = entries
            return self
        except BaseException:
            self._release()
            raise

    def __exit__(self, exc_type, exc_value, traceback):
        try:
            for (path, handle, identity), entry in zip(self._handles, self._manifest):
                current, current_identity = _open_locked(path)
                try:
                    if (current_identity != identity or
                            _stream(handle) != (entry["sha256"], entry["size"]) or
                            _stream(current) != (entry["sha256"], entry["size"])):
                        raise RuntimeError("Protected file changed: {}".format(path))
                finally:
                    _kernel.CloseHandle(current)
        finally:
            self._release()
        return False
