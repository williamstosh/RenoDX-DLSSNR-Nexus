"""Small Windows desktop adapter. No injection, hooks, or game-memory reads."""
import ctypes as C
from ctypes import wintypes as W
import time
from pathlib import Path
from PIL import ImageGrab, ImageStat

u = C.WinDLL('user32', use_last_error=True)
k = C.WinDLL('kernel32', use_last_error=True)
u.SetProcessDpiAwarenessContext.argtypes = [W.HANDLE]
u.SetProcessDpiAwarenessContext(C.c_void_p(-4))
k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
k.OpenProcess.restype = W.HANDLE
k.CloseHandle.argtypes = [W.HANDLE]
k.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
u.GetForegroundWindow.restype = W.HWND
u.AllowSetForegroundWindow.argtypes = [W.DWORD]
u.AllowSetForegroundWindow.restype = W.BOOL
u.IsHungAppWindow.argtypes = [W.HWND]
u.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
u.SetForegroundWindow.argtypes = [W.HWND]
u.GetClientRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
u.ClientToScreen.argtypes = [W.HWND, C.POINTER(W.POINT)]
u.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
u.IsWindowVisible.argtypes = [W.HWND]
u.IsWindow.argtypes = [W.HWND]
u.ShowWindow.argtypes = [W.HWND, C.c_int]
u.IsIconic.argtypes = [W.HWND]
u.AttachThreadInput.argtypes = [W.DWORD, W.DWORD, W.BOOL]
u.BringWindowToTop.argtypes = [W.HWND]
k.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
k.WaitForSingleObject.argtypes = [W.HANDLE, W.DWORD]
k.WaitForSingleObject.restype = W.DWORD
k.TerminateProcess.argtypes = [W.HANDLE, W.UINT]
QUERY_SYNC = 0x1000 | 0x100000

class ForegroundActivationDenied(RuntimeError):
    pass

def _running(handle):
    status = k.WaitForSingleObject(handle, 0)
    if status == 0x102:
        return True
    if status == 0:
        return False
    raise C.WinError(C.get_last_error())

def _metrics(handle):
    if not _running(handle):
        return None
    created, exited, kernel, user = (W.FILETIME() for _ in range(4))
    if not k.GetProcessTimes(handle, C.byref(created), C.byref(exited), C.byref(kernel), C.byref(user)):
        raise C.WinError(C.get_last_error())
    value = lambda f: (f.dwHighDateTime << 32) | f.dwLowDateTime
    if value(exited) or not _running(handle):
        return None
    return dict(created=value(created), cpu_seconds=(value(kernel)+value(user))/1e7)

def _path(handle):
    buf, size = C.create_unicode_buffer(32768), W.DWORD(32768)
    return buf.value if k.QueryFullProcessImageNameW(handle, 0, buf, C.byref(size)) else None

def _same_handle(handle, window):
    metrics = _metrics(handle)
    if not metrics or metrics['created'] != window['created']:
        return False
    path = _path(handle)
    if path is None:
        error = C.get_last_error()
        if not _running(handle):
            return False
        raise C.WinError(error)
    return path == window['path'] and _running(handle)

def process_metrics(pid):
    h = k.OpenProcess(QUERY_SYNC, False, pid)
    if not h:
        error = C.get_last_error()
        if error == 87:
            return None
        raise C.WinError(error)
    try:
        return _metrics(h)
    finally:
        k.CloseHandle(h)

def same_process(window):
    h = k.OpenProcess(QUERY_SYNC, False, window['pid'])
    if not h:
        error = C.get_last_error()
        if error == 87:
            return False
        raise C.WinError(error)
    try:
        return _same_handle(h, window)
    finally:
        k.CloseHandle(h)

def window_alive(window):
    """Check HWND existence, ownership, and the recorded process identity."""
    if not u.IsWindow(window['hwnd']):
        return False
    pid = W.DWORD()
    if not (u.GetWindowThreadProcessId(window['hwnd'], C.byref(pid))
            and pid.value == window['pid'] and same_process(window)):
        return False
    # Recheck ownership after the potentially slower process query.
    return bool(u.IsWindow(window['hwnd'])
                and u.GetWindowThreadProcessId(window['hwnd'], C.byref(pid))
                and pid.value == window['pid'])

def process_path(pid):
    h = k.OpenProcess(0x1000, False, pid)
    if not h:
        return None
    try:
        return _path(h)
    finally:
        k.CloseHandle(h)

def windows(exe=None):
    result = []
    callback_type = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    def callback(hwnd, _):
        pid = W.DWORD()
        u.GetWindowThreadProcessId(hwnd, C.byref(pid))
        path = process_path(pid.value)
        if path and u.IsWindowVisible(hwnd) and (exe is None or Path(path) == Path(exe)):
            title = C.create_unicode_buffer(1024)
            u.GetWindowTextW(hwnd, title, 1024)
            metrics = process_metrics(pid.value)
            if metrics:
                result.append(dict(hwnd=int(hwnd), pid=pid.value, path=path, title=title.value, created=metrics['created']))
        return True
    u.EnumWindows(callback_type(callback), 0)
    return result

def activate(window):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if not window_alive(window):
            raise RuntimeError('Target window disappeared or process identity changed')
        foreground = u.GetForegroundWindow()
        if foreground == window['hwnd']:
            return
        thread = u.GetWindowThreadProcessId(foreground, None)
        current = k.GetCurrentThreadId()
        attached = bool(thread and thread != current and u.AttachThreadInput(current, thread, True))
        try:
            if u.IsIconic(window['hwnd']):
                u.ShowWindow(window['hwnd'], 9)
            u.BringWindowToTop(window['hwnd'])
            u.SetForegroundWindow(window['hwnd'])
        finally:
            if attached:
                u.AttachThreadInput(current, thread, False)
        if u.GetForegroundWindow() == window['hwnd'] and window_alive(window):
            return
        time.sleep(min(.05, max(0, deadline - time.monotonic())))
    raise ForegroundActivationDenied('Target cannot acquire foreground; no input sent')

def ensure_foreground(window):
    """Acquire and verify the exact target window without a human handoff."""
    started = time.monotonic()
    activate(window)
    if not window_alive(window) or u.GetForegroundWindow() != window['hwnd']:
        raise ForegroundActivationDenied('Target lost foreground or identity after activation; no input sent')
    return dict(hwnd=window['hwnd'], pid=window['pid'], created=window['created'],
                elapsed_ms=round((time.monotonic() - started) * 1000, 3))

def grant_foreground_permission(pid):
    """Ask Windows to let this test worker activate its owned game window."""
    if not 0 < pid <= 0xffffffff:
        raise ValueError('Invalid foreground permission process id')
    C.set_last_error(0)
    granted = bool(u.AllowSetForegroundWindow(pid))
    return dict(granted=granted, error=None if granted else C.get_last_error())

def capture(window, destination):
    ensure_foreground(window)
    rect, point = W.RECT(), W.POINT()
    if not u.GetClientRect(window['hwnd'], C.byref(rect)) or not u.ClientToScreen(window['hwnd'], C.byref(point)):
        raise C.WinError(C.get_last_error())
    if rect.right < 64 or rect.bottom < 64:
        raise RuntimeError('Target window is minimized or too small')
    img = ImageGrab.grab(bbox=(point.x, point.y, point.x + rect.right, point.y + rect.bottom), all_screens=True)
    if not window_alive(window) or u.GetForegroundWindow() != window['hwnd']:
        raise RuntimeError('Target identity or foreground changed during capture')
    img.save(destination)
    thumb = img.resize((64, 36)).convert('L')
    stat = ImageStat.Stat(thumb)
    return dict(width=img.width, height=img.height, mean_luma=stat.mean[0], std_luma=stat.stddev[0],
                dark_fraction=sum(v < 8 for v in thumb.getdata()) / (64 * 36))

ULONG_PTR = C.c_size_t
class KEYBDINPUT(C.Structure):
    _fields_ = [('wVk', W.WORD), ('wScan', W.WORD), ('dwFlags', W.DWORD), ('time', W.DWORD), ('dwExtraInfo', ULONG_PTR)]
class MOUSEINPUT(C.Structure):
    _fields_ = [('dx', W.LONG), ('dy', W.LONG), ('mouseData', W.DWORD), ('dwFlags', W.DWORD), ('time', W.DWORD), ('dwExtraInfo', ULONG_PTR)]
class INPUTUNION(C.Union):
    _fields_ = [('ki', KEYBDINPUT), ('mi', MOUSEINPUT)]
class INPUT(C.Structure):
    _anonymous_ = ('value',)
    _fields_ = [('type', W.DWORD), ('value', INPUTUNION)]
u.SendInput.argtypes = [W.UINT, C.POINTER(INPUT), C.c_int]
u.SendInput.restype = W.UINT
KEYS = dict(ENTER=0x1C, ESC=0x01, SPACE=0x39, UP=0x48, DOWN=0x50, LEFT=0x4B, RIGHT=0x4D, HOME=0x47, W=0x11, A=0x1E, S=0x1F, D=0x20, E=0x12)

def key(window, name, seconds=.2):
    ensure_foreground(window)
    if name not in KEYS or not 0 < seconds <= 2:
        raise ValueError('Unsupported key or hold duration')
    scan = KEYS[name]
    flags = 8 | (1 if name in ('UP', 'DOWN', 'LEFT', 'RIGHT', 'HOME') else 0)
    down = INPUT(type=1, ki=KEYBDINPUT(0, scan, flags, 0, 0))
    up = INPUT(type=1, ki=KEYBDINPUT(0, scan, flags | 2, 0, 0))
    try:
        if u.SendInput(1, C.byref(down), C.sizeof(INPUT)) != 1:
            raise C.WinError(C.get_last_error())
        # Hold across render polls; short menu taps can be consumed between frames.
        time.sleep(seconds)
    finally:
        if u.SendInput(1, C.byref(up), C.sizeof(INPUT)) != 1:
            raise RuntimeError('Key release failed')

def move_camera(window, dx, dy):
    """Send bounded relative movement to the verified active capture window."""
    if any(type(delta) is not int or not -200 <= delta <= 200 for delta in (dx, dy)):
        raise ValueError('Camera deltas must be integers between -200 and 200')
    movement = INPUT(type=0, mi=MOUSEINPUT(dx, dy, 0, 0x0001, 0, 0))
    if not window_alive(window):
        raise RuntimeError('Target window disappeared or process identity changed; no input sent')
    if u.GetForegroundWindow() != window['hwnd']:
        raise RuntimeError('Target is not foreground; no camera input sent')
    if u.SendInput(1, C.byref(movement), C.sizeof(INPUT)) != 1:
        raise RuntimeError('Camera movement failed')


def park_cursor(window):
    """Keep the rendered menu cursor outside the text without clicking."""
    ensure_foreground(window)
    rect = W.RECT()
    if not u.GetClientRect(window['hwnd'], C.byref(rect)):
        raise C.WinError(C.get_last_error())
    if rect.right <= 0 or rect.bottom <= 0:
        raise ValueError('Empty client area')
    point = W.POINT(rect.right - 1, rect.bottom - 1)
    if not u.ClientToScreen(window['hwnd'], C.byref(point)):
        raise C.WinError(C.get_last_error())
    left, top = u.GetSystemMetrics(76), u.GetSystemMetrics(77)
    width, height = u.GetSystemMetrics(78), u.GetSystemMetrics(79)
    if width <= 1 or height <= 1 or not (left <= point.x < left + width and top <= point.y < top + height):
        raise ValueError('Cursor target outside virtual desktop')
    # SetCursorPos alone did not move the rendered game cursor. Send a real
    # movement event; VIRTUALDESK also supports windows on a secondary display.
    movement = INPUT(type=0, mi=MOUSEINPUT(
        round((point.x - left) * 65535 / (width - 1)),
        round((point.y - top) * 65535 / (height - 1)), 0, 0xC001, 0, 0))
    if u.SendInput(1, C.byref(movement), C.sizeof(INPUT)) != 1:
        raise RuntimeError('Cursor movement failed')
    # The game renders its cursor; let the next frame consume the movement.
    time.sleep(.12)


def click(window, x, y):
    ensure_foreground(window)
    rect, p = W.RECT(), W.POINT()
    u.GetClientRect(window['hwnd'], C.byref(rect))
    u.ClientToScreen(window['hwnd'], C.byref(p))
    if not 0 <= x < rect.right or not 0 <= y < rect.bottom:
        raise ValueError('Click outside client area')
    if not u.SetCursorPos(p.x + int(x), p.y + int(y)):
        raise C.WinError(C.get_last_error())
    down, up = INPUT(type=0, mi=MOUSEINPUT(0, 0, 0, 2, 0, 0)), INPUT(type=0, mi=MOUSEINPUT(0, 0, 0, 4, 0, 0))
    try:
        if u.SendInput(1, C.byref(down), C.sizeof(INPUT)) != 1:
            raise RuntimeError('Mouse down failed')
        # Frame-polled overlays can miss a down/up pair delivered between frames.
        time.sleep(.12)
    finally:
        if u.SendInput(1, C.byref(up), C.sizeof(INPUT)) != 1:
            raise RuntimeError('Mouse release failed')

def close(window):
    if window_alive(window):
        if not u.PostMessageW(window['hwnd'], 0x10, 0, 0):
            error = C.get_last_error()
            if error != 1400:
                raise C.WinError(error)

def _timeout_ms(timeout):
    if not 0 <= timeout <= 3600:
        raise ValueError('Exit timeout must be between 0 and 3600 seconds')
    return int(timeout * 1000)

def _wait_exit(handle, milliseconds):
    status = k.WaitForSingleObject(handle, milliseconds)
    if status == 0:
        return True
    if status == 0x102:
        return False
    raise C.WinError(C.get_last_error())

def wait_exit(window, timeout=5):
    """Return whether the recorded process exited within timeout seconds."""
    milliseconds = _timeout_ms(timeout)
    h = k.OpenProcess(QUERY_SYNC, False, window['pid'])
    if not h:
        error = C.get_last_error()
        if error == 87:  # No process with this PID.
            return True
        raise C.WinError(error)
    try:
        return not _same_handle(h, window) or _wait_exit(h, milliseconds)
    finally:
        k.CloseHandle(h)

def terminate(window, timeout=5):
    milliseconds = _timeout_ms(timeout)
    h = k.OpenProcess(QUERY_SYNC | 1, False, window['pid'])
    if not h:
        raise C.WinError(C.get_last_error())
    try:
        # Validate the very handle being terminated; a separate PID lookup can race reuse.
        if not _same_handle(h, window):
            raise RuntimeError('Refusing to terminate changed process identity')
        if not k.TerminateProcess(h, 1):
            error = C.get_last_error()
            if _running(h):
                raise C.WinError(error)
        if not _wait_exit(h, milliseconds):
            raise TimeoutError('Target process did not exit after termination')
    finally:
        k.CloseHandle(h)

def modules(window):
    """Return loaded module paths for the recorded process (read-only PSAPI).

    Paths identify loaded images; callers must hash files themselves. Enumeration
    is a snapshot and can fail if modules unload or the process exits during it.
    """
    h = k.OpenProcess(QUERY_SYNC | 0x400 | 0x10, False, window['pid'])
    if not h:
        raise C.WinError(C.get_last_error())
    try:
        if not _same_handle(h, window):
            raise RuntimeError('Target process identity changed')
        psapi = C.WinDLL('psapi', use_last_error=True)
        psapi.EnumProcessModulesEx.argtypes = [W.HANDLE, C.POINTER(W.HMODULE), W.DWORD, C.POINTER(W.DWORD), W.DWORD]
        psapi.GetModuleFileNameExW.argtypes = [W.HANDLE, W.HMODULE, W.LPWSTR, W.DWORD]
        capacity = 256
        for _ in range(4):
            handles, needed = (W.HMODULE * capacity)(), W.DWORD()
            if not psapi.EnumProcessModulesEx(h, handles, C.sizeof(handles), C.byref(needed), 3):
                raise C.WinError(C.get_last_error())
            if needed.value <= C.sizeof(handles):
                break
            capacity = (needed.value + C.sizeof(W.HMODULE) - 1) // C.sizeof(W.HMODULE)
        else:
            raise RuntimeError('Module list kept growing during enumeration')
        paths = []
        for module in handles[:needed.value // C.sizeof(W.HMODULE)]:
            buf = C.create_unicode_buffer(32768)
            length = psapi.GetModuleFileNameExW(h, module, buf, len(buf))
            if not length:
                raise C.WinError(C.get_last_error())
            if length >= len(buf):
                raise RuntimeError('Module path was truncated')
            paths.append(buf.value)
        if not _same_handle(h, window):
            raise RuntimeError('Target process exited during module enumeration')
        return paths
    finally:
        k.CloseHandle(h)
