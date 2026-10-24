"""Run with python -B -m unittest discover -s tools/game-test -p test_guard.py."""

import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import tempfile
import struct
import unittest
from unittest import mock

import guard
from guard import ProtectedFiles


@unittest.skipUnless(os.name == "nt", "Windows sharing semantics required")
class ProtectedFilesTests(unittest.TestCase):
    def setUp(self):
        scratch = Path(__file__).resolve().parents[2] / 'tmp'
        scratch.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "save.dat"
        self.payload = b"original save\x00\xff" * 100000
        self.source.write_bytes(self.payload)
        self.backups = self.root / "backups"

    def assert_writable(self):
        with self.source.open("ab") as output:
            output.write(b"released")

    def test_reads_allowed_writes_deletes_and_renames_denied(self):
        with ProtectedFiles([self.source], self.backups):
            self.assertEqual(self.source.read_bytes(), self.payload)
            with self.assertRaises(PermissionError):
                self.source.write_bytes(b"changed")
            with self.assertRaises(PermissionError):
                self.source.unlink()
            with self.assertRaises(PermissionError):
                self.source.rename(self.root / "renamed.dat")
        self.assertEqual(self.source.read_bytes(), self.payload)
        self.assert_writable()

    def test_verified_backup_and_manifest(self):
        second = self.root / "settings.ini"
        second.write_bytes(b"setting=1\n")
        self.backups.mkdir()
        with ProtectedFiles([self.source, second], self.backups) as protected:
            manifest = protected.manifest
            self.assertEqual(len(manifest), 2)
            self.assertEqual(json.loads((self.backups / "manifest.json").read_text()), manifest)
            for entry in manifest:
                original = Path(entry["path"]).read_bytes()
                self.assertEqual(Path(entry["backup_path"]).read_bytes(), original)
                self.assertEqual(entry["sha256"], hashlib.sha256(original).hexdigest())
                self.assertEqual(entry["size"], len(original))
                self.assertIs(entry["verified"], True)
            manifest[0]["sha256"] = "tampered"
            self.assertNotEqual(protected.manifest[0]["sha256"], "tampered")

    def test_body_exception_releases_handles(self):
        with self.assertRaisesRegex(RuntimeError, "body failure"):
            with ProtectedFiles([self.source], self.backups):
                raise RuntimeError("body failure")
        self.assert_writable()

    def test_partial_lock_failure_releases_handles_without_backup(self):
        with self.assertRaises(FileNotFoundError):
            with ProtectedFiles([self.source, self.root / "missing"], self.backups):
                self.fail("Entered despite a missing file")
        self.assertFalse(self.backups.exists())
        self.assert_writable()

    def test_backup_failure_releases_handles(self):
        with mock.patch.object(guard, "_backup_hash", return_value="bad"):
            with self.assertRaisesRegex(RuntimeError, "Backup verification failed"):
                with ProtectedFiles([self.source], self.backups):
                    self.fail("Entered with an unverified backup")
        self.assertFalse((self.backups / "manifest.json").exists())
        self.assert_writable()

    def test_nonempty_destination_is_preserved(self):
        self.backups.mkdir()
        marker = self.backups / "keep"
        marker.write_bytes(b"keep")
        with self.assertRaises(FileExistsError):
            with ProtectedFiles([self.source], self.backups):
                self.fail("Entered with nonempty destination")
        self.assertEqual(marker.read_bytes(), b"keep")
        self.assert_writable()

    def test_existing_writer_prevents_entry(self):
        with self.source.open("ab"):
            with self.assertRaises(OSError):
                with ProtectedFiles([self.source], self.backups):
                    self.fail("Entered while a writer already existed")
        self.assertFalse(self.backups.exists())
        self.assert_writable()

    def test_exit_hash_check_and_release(self):
        protected = ProtectedFiles([self.source], self.backups)
        protected.__enter__()
        with mock.patch.object(guard, "_stream", return_value=("changed", 1)) as stream:
            with self.assertRaisesRegex(RuntimeError, "Protected file changed"):
                protected.__exit__(None, None, None)
            self.assertTrue(stream.called)
        self.assertEqual(self.source.read_bytes(), self.payload)
        self.assert_writable()

    def test_exit_checks_even_on_body_exception(self):
        with mock.patch.object(guard, "_stream", wraps=guard._stream) as stream:
            with self.assertRaisesRegex(ValueError, "body"):
                with ProtectedFiles([self.source], self.backups):
                    raise ValueError("body")
            self.assertEqual(stream.call_count, 3)
        self.assert_writable()

    def test_directory_input_rejected(self):
        with self.assertRaises(ValueError):
            with ProtectedFiles([self.root], self.backups):
                self.fail("Entered with a directory")

    def test_junction_input_and_destination_rejected(self):
        # Mount-point reparse data allows junction creation without symlink
        # privilege. Everything referenced here lives under TemporaryDirectory.
        junction = self.root / "junction"
        junction.mkdir()
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = guard._kernel.CreateFileW.argtypes
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.DeviceIoControl.argtypes = [
            wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
            ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
            ctypes.c_void_p]
        kernel.DeviceIoControl.restype = wintypes.BOOL
        target = ("\\??\\" + str(self.root)).encode("utf-16-le")
        display = str(self.root).encode("utf-16-le")
        names = target + b"\0\0" + display + b"\0\0"
        data = struct.pack("<HHHH", 0, len(target), len(target) + 2, len(display)) + names
        packet = struct.pack("<IHH", 0xA0000003, len(data), 0) + data
        handle = kernel.CreateFileW(str(junction), 0x40000000, 0, None,
                                    guard.OPEN_EXISTING, 0x02200000, None)
        if handle == guard.INVALID_HANDLE_VALUE:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            returned = wintypes.DWORD()
            buffer = ctypes.create_string_buffer(packet)
            if not kernel.DeviceIoControl(handle, 0x000900A4, buffer, len(packet),
                                          None, 0, ctypes.byref(returned), None):
                raise ctypes.WinError(ctypes.get_last_error())
        finally:
            guard._kernel.CloseHandle(handle)
        self.addCleanup(junction.rmdir)
        for paths, destination in [([junction / self.source.name], self.backups),
                                   ([self.source], junction / "backups")]:
            with self.subTest(paths=paths, destination=destination):
                with self.assertRaisesRegex(ValueError, "Reparse"):
                    with ProtectedFiles(paths, destination):
                        self.fail("Entered through a junction")
        self.assert_writable()

    def test_symlink_and_parent_symlink_rejected(self):
        link = self.root / "link"
        try:
            link.symlink_to(self.root, target_is_directory=True)
        except OSError as error:
            self.skipTest("Symlink creation unavailable: {}".format(error))
        self.addCleanup(link.unlink)
        for paths, destination in [([link / self.source.name], self.backups),
                                   ([self.source], link / "backups")]:
            with self.subTest(paths=paths, destination=destination):
                with self.assertRaisesRegex(ValueError, "Reparse"):
                    with ProtectedFiles(paths, destination):
                        self.fail("Entered through a symlink")


if __name__ == "__main__":
    unittest.main()
