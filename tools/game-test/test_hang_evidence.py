"""Synthetic-process tests for the local hang dump helper."""
import ctypes as C
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import hang_evidence


ROOT = Path(__file__).resolve().parents[2]
TEST_OUTPUT = ROOT / "tmp"


def process_identity(process):
    import ctypes.wintypes as W

    kernel = C.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    kernel.OpenProcess.restype = W.HANDLE
    kernel.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
    kernel.CloseHandle.argtypes = [W.HANDLE]
    handle = kernel.OpenProcess(0x0400 | 0x100000, False, process.pid)
    if not handle:
        raise C.WinError(C.get_last_error())
    try:
        created, exited, kernel_time, user_time = (W.FILETIME() for _ in range(4))
        if not kernel.GetProcessTimes(handle, C.byref(created), C.byref(exited),
                                      C.byref(kernel_time), C.byref(user_time)):
            raise C.WinError(C.get_last_error())
        return {"pid": process.pid, "created": (created.dwHighDateTime << 32) | created.dwLowDateTime,
                "path": str(Path(sys.executable).resolve())}
    finally:
        kernel.CloseHandle(handle)


def minidump_streams(path):
    data = path.read_bytes()
    signature, _, count, directory_rva = struct.unpack_from("<IIII", data)
    if signature != 0x504D444D:
        raise AssertionError("invalid MDMP signature")
    streams = {}
    for index in range(count):
        kind, size, rva = struct.unpack_from("<III", data, directory_rva + index * 12)
        streams[kind] = data[rva:rva + size]
    return streams


def stop_child(child):
    if child.poll() is None:
        child.kill()
    child.wait(timeout=5)


class HangEvidenceTests(unittest.TestCase):
    def setUp(self):
        TEST_OUTPUT.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="hang-evidence-test-", dir=TEST_OUTPUT)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    @unittest.skipUnless(os.name == "nt", "MiniDumpWriteDump is Windows-only")
    def test_captures_thread_context_stack_modules_and_thread_info(self):
        child = subprocess.Popen([sys.executable, "-B", "-c", "import time; time.sleep(60)"],
                                 creationflags=0x08000000)
        self.addCleanup(stop_child, child)
        result = hang_evidence.capture(process_identity(child), self.root / "out")
        self.assertEqual(result["status"], "captured", result)
        dump = self.root / "out" / "game.dmp"
        self.assertTrue(dump.is_file())
        self.assertEqual(json.loads((dump.parent / "manifest.json").read_text())["status"], "captured")
        streams = minidump_streams(dump)
        self.assertIn(3, streams, "ThreadListStream missing")
        self.assertIn(4, streams, "ModuleListStream missing")
        self.assertIn(17, streams, "ThreadInfoListStream missing")
        thread_data = streams[3]
        thread_count = struct.unpack_from("<I", thread_data)[0]
        self.assertGreater(thread_count, 0)
        thread = thread_data[4:52]
        stack_size = struct.unpack_from("<I", thread, 32)[0]
        context_size = struct.unpack_from("<I", thread, 40)[0]
        self.assertGreater(stack_size, 0, "thread stack missing")
        self.assertGreater(context_size, 0, "thread context missing")
        module_count = struct.unpack_from("<I", streams[4])[0]
        self.assertGreater(module_count, 0)
        header_size, entry_size, info_count = struct.unpack_from("<III", streams[17])
        self.assertEqual(header_size, 12)
        self.assertGreaterEqual(entry_size, 64)
        self.assertGreater(info_count, 0)
        self.assertEqual(child.poll(), None, "capture terminated the target")

    @unittest.skipUnless(os.name == "nt", "process identity is Windows-specific")
    def test_manual_cli_captures_to_unicode_directory(self):
        child = subprocess.Popen([sys.executable, "-B", "-c", "import time; time.sleep(60)"],
                                 creationflags=0x08000000)
        self.addCleanup(stop_child, child)
        identity = self.root / "owned-process.json"
        identity.write_text(json.dumps(process_identity(child)), encoding="utf-8")
        destination = self.root / "挂起采证"
        result = subprocess.run([sys.executable, "-B", hang_evidence.__file__,
                                 "--process", str(identity), "--output", str(destination)],
                                capture_output=True, text=True, encoding="utf-8", timeout=25,
                                creationflags=0x08000000)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(json.loads(result.stdout)["status"], "captured")
        self.assertIn(3, minidump_streams(destination / "game.dmp"))
        self.assertEqual(child.poll(), None)

    @unittest.skipUnless(os.name == "nt", "process identity is Windows-specific")
    def test_identity_mismatch_does_not_create_dump(self):
        child = subprocess.Popen([sys.executable, "-B", "-c", "import time; time.sleep(60)"],
                                 creationflags=0x08000000)
        self.addCleanup(stop_child, child)
        identity = process_identity(child)
        identity["created"] += 1
        destination = self.root / "mismatch"
        result = hang_evidence.capture(identity, destination)
        self.assertEqual(result["status"], "identity_mismatch", result)
        self.assertFalse((destination / "game.dmp").exists())
        self.assertEqual(child.poll(), None)

    def test_existing_dump_is_preserved(self):
        destination = self.root / "existing"
        destination.mkdir()
        dump = destination / "game.dmp"
        dump.write_bytes(b"prior evidence")
        result = hang_evidence.capture({"pid": 1, "created": 1, "path": "unused"}, destination)
        self.assertEqual(result["status"], "already_exists")
        self.assertEqual(dump.read_bytes(), b"prior evidence")

    def test_timeout_kills_and_joins_helper_without_touching_target(self):
        class StuckHelper:
            returncode = None

            def __init__(self):
                self.killed = False
                self.joined = False

            def communicate(self, input, timeout):
                raise subprocess.TimeoutExpired("helper", timeout)

            def kill(self):
                self.killed = True

            def wait(self):
                self.joined = True
                self.returncode = -9

        helper = StuckHelper()
        with mock.patch.object(hang_evidence.subprocess, "Popen", return_value=helper):
            result = hang_evidence.capture({"pid": 123, "created": 1, "path": "synthetic"},
                                           self.root / "timeout", timeout=0.01)
        self.assertEqual(result["status"], "timeout")
        self.assertTrue(helper.killed and helper.joined)
        self.assertEqual(json.loads((self.root / "timeout" / "manifest.json").read_text())["status"], "timeout")

    def test_helper_failure_is_explicit(self):
        class FailedHelper:
            returncode = 1

            def communicate(self, input, timeout):
                return "not json", "synthetic failure"

        with mock.patch.object(hang_evidence.subprocess, "Popen", return_value=FailedHelper()):
            result = hang_evidence.capture({"pid": 123, "created": 1, "path": "synthetic"},
                                           self.root / "failure")
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["reason"], "helper_invalid_response")
        self.assertEqual(json.loads((self.root / "failure" / "manifest.json").read_text())["status"], "failed")

    def test_communication_exception_kills_and_joins_helper(self):
        class BrokenHelper:
            returncode = None

            def __init__(self):
                self.killed = False
                self.joined = False

            def communicate(self, input, timeout):
                self.input = input
                raise OSError("synthetic pipe failure")

            def kill(self):
                self.killed = True

            def wait(self):
                self.joined = True
                self.returncode = -9

        helper = BrokenHelper()
        with mock.patch.object(hang_evidence.subprocess, "Popen", return_value=helper):
            result = hang_evidence.capture({"pid": 123, "created": 1, "path": "C:/游戏/game.exe"},
                                           self.root / "pipe-failure")
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["reason"], "helper_communication_failed")
        self.assertTrue(helper.killed and helper.joined)
        self.assertTrue(helper.input.isascii())
        self.assertEqual(json.loads((self.root / "pipe-failure" / "manifest.json").read_text())["status"],
                         "failed")


if __name__ == "__main__":
    unittest.main()
