"""Mock-only desktop boundary tests; no game or window operations are issued."""
import ctypes as C
from ctypes import wintypes as W
import unittest
from unittest import mock

# Importing the adapter normally changes DPI awareness. Suppress all native calls
# at import too, so this suite never manipulates desktop state.
with mock.patch.object(C, 'WinDLL'):
    import desktop


class DesktopTests(unittest.TestCase):
    def setUp(self):
        self.window = dict(hwnd=123, pid=42, created=100, path=r'C:\game\game.exe')
        self.k = mock.Mock()
        self.u = mock.Mock()
        self.k.OpenProcess.return_value = 900
        self.k.WaitForSingleObject.return_value = 0x102
        self.k.GetProcessTimes.side_effect = self.times
        self.k.QueryFullProcessImageNameW.side_effect = self.path
        self.u.GetWindowThreadProcessId.side_effect = self.owner
        self.patchers = [mock.patch.object(desktop, 'k', self.k),
                         mock.patch.object(desktop, 'u', self.u)]
        for patcher in self.patchers:
            patcher.start()
            self.addCleanup(patcher.stop)

    def times(self, handle, created, exited, kernel, user):
        created._obj.dwLowDateTime = 100
        kernel._obj.dwLowDateTime = 10000000
        user._obj.dwLowDateTime = 20000000
        return True

    def path(self, handle, flags, buffer, size):
        buffer.value = self.window['path']
        return True

    def owner(self, hwnd, pid):
        if pid is not None:
            pid._obj.value = self.window['pid']
        return 7

    def test_metrics_live_and_single_handle_identity(self):
        self.assertEqual(desktop.process_metrics(42), dict(created=100, cpu_seconds=3))
        self.assertTrue(desktop.same_process(self.window))
        self.assertEqual(self.k.OpenProcess.call_count, 2)
        self.k.QueryFullProcessImageNameW.assert_called_once()
        self.assertEqual(self.k.CloseHandle.call_count, 2)

    def test_exited_handle_is_not_a_live_process(self):
        self.k.WaitForSingleObject.return_value = 0
        self.assertIsNone(desktop.process_metrics(42))
        self.assertFalse(desktop.same_process(self.window))
        self.k.GetProcessTimes.assert_not_called()

    def test_open_access_denied_is_unknown_not_a_successful_exit(self):
        self.k.OpenProcess.return_value = 0
        with mock.patch.object(C, 'get_last_error', return_value=5):
            with self.assertRaises(OSError):
                desktop.same_process(self.window)
            with self.assertRaises(OSError):
                desktop.process_metrics(42)
        with mock.patch.object(C, 'get_last_error', return_value=87):
            self.assertFalse(desktop.same_process(self.window))
            self.assertIsNone(desktop.process_metrics(42))

    def test_exit_during_metrics(self):
        self.k.WaitForSingleObject.side_effect = [0x102, 0]
        self.assertIsNone(desktop.process_metrics(42))

    def test_exit_time_rejects_stale_times(self):
        def exited(*args):
            self.times(*args)
            args[2]._obj.dwLowDateTime = 200
            return True
        self.k.GetProcessTimes.side_effect = exited
        self.assertIsNone(desktop.process_metrics(42))

    def test_pid_reuse_or_path_change_rejected(self):
        for field, value in [('created', 99), ('path', r'C:\other.exe')]:
            with self.subTest(field=field):
                identity = dict(self.window, **{field: value})
                self.assertFalse(desktop.same_process(identity))
                with self.assertRaisesRegex(RuntimeError, 'Refusing'):
                    desktop.terminate(identity)
        self.k.TerminateProcess.assert_not_called()

    def test_invalid_window_fails_activation_without_wait_or_post(self):
        self.u.IsWindow.return_value = False
        with mock.patch.object(desktop.time, 'sleep') as sleep:
            with self.assertRaisesRegex(RuntimeError, 'disappeared'):
                desktop.activate(self.window)
            desktop.close(self.window)
        sleep.assert_not_called()
        self.u.PostMessageW.assert_not_called()
        self.u.SetForegroundWindow.assert_not_called()

    def test_reused_hwnd_owner_does_not_close(self):
        def changed(hwnd, pid):
            pid._obj.value = 99
            return 7
        self.u.GetWindowThreadProcessId.side_effect = changed
        self.assertFalse(desktop.window_alive(self.window))
        desktop.close(self.window)
        self.u.PostMessageW.assert_not_called()

    def test_hwnd_owner_changes_during_identity_query(self):
        def changed(hwnd, pid):
            pid._obj.value = 42 if self.u.GetWindowThreadProcessId.call_count == 1 else 99
            return 7
        self.u.GetWindowThreadProcessId.side_effect = changed
        self.assertFalse(desktop.window_alive(self.window))

    def test_close_tolerates_only_invalid_hwnd_error(self):
        self.u.PostMessageW.return_value = False
        with mock.patch.object(C, 'get_last_error', return_value=1400):
            desktop.close(self.window)
        with mock.patch.object(C, 'get_last_error', return_value=5):
            with self.assertRaises(OSError):
                desktop.close(self.window)

    def test_close_live_window_posts_once(self):
        desktop.close(self.window)
        self.u.PostMessageW.assert_called_once_with(123, 0x10, 0, 0)

    def test_reused_pid_with_same_hwnd_does_not_close(self):
        with mock.patch.object(desktop, '_metrics', return_value=dict(created=101)):
            desktop.close(self.window)
        self.u.PostMessageW.assert_not_called()

    def test_activate_detects_disappearance_on_retry(self):
        with mock.patch.object(desktop, 'window_alive', side_effect=[True, False]), \
                mock.patch.object(desktop.time, 'sleep'):
            self.u.GetForegroundWindow.return_value = 456
            with self.assertRaisesRegex(RuntimeError, 'disappeared'):
                desktop.activate(self.window)
        self.u.SetForegroundWindow.assert_called_once()

    def test_terminate_waits_on_same_verified_handle(self):
        self.k.WaitForSingleObject.side_effect = [0x102, 0x102, 0x102, 0]
        desktop.terminate(self.window, timeout=.25)
        self.k.OpenProcess.assert_called_once_with(desktop.QUERY_SYNC | 1, False, 42)
        self.k.TerminateProcess.assert_called_once_with(900, 1)
        self.k.WaitForSingleObject.assert_called_with(900, 250)
        self.k.CloseHandle.assert_called_once_with(900)

    def test_terminate_timeout_is_reported(self):
        with self.assertRaises(TimeoutError):
            desktop.terminate(self.window, timeout=0)
        self.k.CloseHandle.assert_called_once_with(900)

    def test_exit_racing_terminate_failure_is_tolerated(self):
        self.k.WaitForSingleObject.side_effect = [0x102, 0x102, 0x102, 0, 0]
        self.k.TerminateProcess.return_value = False
        desktop.terminate(self.window)

    def test_live_terminate_failure_propagates(self):
        self.k.TerminateProcess.return_value = False
        with mock.patch.object(C, 'get_last_error', return_value=5):
            with self.assertRaises(OSError):
                desktop.terminate(self.window)
        self.k.CloseHandle.assert_called_once_with(900)

    def test_terminate_already_exited_never_kills(self):
        self.k.WaitForSingleObject.return_value = 0
        with self.assertRaisesRegex(RuntimeError, 'Refusing'):
            desktop.terminate(self.window)
        self.k.TerminateProcess.assert_not_called()
        self.k.CloseHandle.assert_called_once_with(900)

    def test_wait_exit_timeout_and_reused_pid(self):
        self.assertFalse(desktop.wait_exit(self.window, timeout=0))
        self.assertTrue(desktop.wait_exit(dict(self.window, created=99)))

    def test_wait_exit_missing_pid_and_access_denied(self):
        self.k.OpenProcess.return_value = None
        with mock.patch.object(C, 'get_last_error', return_value=87):
            self.assertTrue(desktop.wait_exit(self.window))
        with mock.patch.object(C, 'get_last_error', return_value=5):
            with self.assertRaises(OSError):
                desktop.wait_exit(self.window)

    def test_query_failure_does_not_claim_exit(self):
        self.k.QueryFullProcessImageNameW.return_value = False
        self.k.QueryFullProcessImageNameW.side_effect = None
        with mock.patch.object(C, 'get_last_error', return_value=5):
            with self.assertRaises(OSError):
                desktop.wait_exit(self.window)

    def test_wait_failure_and_timeout_validation(self):
        self.k.WaitForSingleObject.return_value = 0xffffffff
        with self.assertRaises(OSError):
            desktop.process_metrics(42)
        for value in (-1, float('inf'), float('nan'), 3601):
            with self.subTest(value=value), self.assertRaises(ValueError):
                desktop.terminate(self.window, timeout=value)

    def test_modules_read_only_paths_and_resize(self):
        psapi = mock.Mock()
        def enum(handle, handles, size, needed, flags):
            needed._obj.value = 300 * C.sizeof(W.HMODULE)
            if len(handles) >= 300:
                for i in range(300):
                    handles[i] = i + 1
            return True
        def name(handle, module, buffer, size):
            buffer.value = r'C:\game\addon.dll'
            return len(buffer.value)
        psapi.EnumProcessModulesEx.side_effect = enum
        psapi.GetModuleFileNameExW.side_effect = name
        with mock.patch.object(C, 'WinDLL', return_value=psapi):
            paths = desktop.modules(self.window)
        self.assertEqual(paths, [r'C:\game\addon.dll'] * 300)
        self.assertEqual(psapi.EnumProcessModulesEx.call_count, 2)
        self.k.OpenProcess.assert_called_once_with(desktop.QUERY_SYNC | 0x400 | 0x10, False, 42)
        self.k.TerminateProcess.assert_not_called()
        self.k.CloseHandle.assert_called_once_with(900)

    def test_modules_rejects_identity_change(self):
        with self.assertRaisesRegex(RuntimeError, 'identity changed'):
            desktop.modules(dict(self.window, created=99))
        self.k.CloseHandle.assert_called_once_with(900)

    def test_modules_errors_and_mid_enumeration_exit(self):
        psapi = mock.Mock()
        def enum(handle, handles, size, needed, flags):
            needed._obj.value = 0
            return True
        psapi.EnumProcessModulesEx.side_effect = enum
        with mock.patch.object(C, 'WinDLL', return_value=psapi), \
                mock.patch.object(desktop, '_same_handle', side_effect=[True, False]):
            with self.assertRaisesRegex(RuntimeError, 'exited during'):
                desktop.modules(self.window)
        psapi.EnumProcessModulesEx.side_effect = None
        psapi.EnumProcessModulesEx.return_value = False
        with mock.patch.object(C, 'WinDLL', return_value=psapi):
            with self.assertRaises(OSError):
                desktop.modules(self.window)

    def test_module_path_failure_and_truncation_release_handle(self):
        psapi = mock.Mock()
        def enum(handle, handles, size, needed, flags):
            handles[0] = 1
            needed._obj.value = C.sizeof(W.HMODULE)
            return True
        psapi.EnumProcessModulesEx.side_effect = enum
        for length, exception in [(0, OSError), (32768, RuntimeError)]:
            with self.subTest(length=length), mock.patch.object(C, 'WinDLL', return_value=psapi):
                psapi.GetModuleFileNameExW.return_value = length
                self.k.CloseHandle.reset_mock()
                with self.assertRaises(exception):
                    desktop.modules(self.window)
                self.k.CloseHandle.assert_called_once_with(900)


if __name__ == '__main__':
    unittest.main()
