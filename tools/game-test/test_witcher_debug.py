import subprocess
import unittest
from unittest import mock

import witcher_debug as debug


def cli_output(response):
    return f"""---------------------------------------------------
                PACKET (47 bytes)
---------------------------------------------------
Type: Int32
Value: 305419896

Type: Int32
Value: -2129264632

Type: StringUTF8
Length: {len(response.encode('utf-8'))}
Value: {response}

---------------------------------------------------
"""


class WitcherDebuggerTests(unittest.TestCase):
    def setUp(self):
        self.client = debug.WitcherDebugger(r"C:\tools\rw3d_cli.exe")

    def test_cli_exec_uses_verified_game_target_and_parses_script_receipt(self):
        response = "NRNX|4f9b|ready|1"
        result = subprocess.CompletedProcess([], 0, cli_output(response), "")
        with mock.patch.object(debug.subprocess, "run", return_value=result) as run:
            actual = self.client.exec_bridge("NRNX_TestBridgeReady", "4f9b")

        self.assertEqual(actual, response)
        args = run.call_args.args[0]
        self.assertEqual(args[:8], [
            r"C:\tools\rw3d_cli.exe", "--ip", "127.0.0.1", "--target", "game",
            "--response-timeout", "3000", "--verbose"])
        self.assertEqual(args[8:], ["exec", "NRNX_TestBridgeReady('4f9b')"])
        self.assertFalse(run.call_args.kwargs.get("shell", False))
        self.assertEqual(run.call_args.kwargs["timeout"], 10.0)
        self.assertEqual(self.client.last_invocation["returncode"], 0)

    def test_cli_empty_success_is_normalized(self):
        result = subprocess.CompletedProcess([], 0, debug.EXEC_EMPTY, "")
        with mock.patch.object(debug.subprocess, "run", return_value=result):
            self.assertEqual(self.client.exec_raw("logstats()"), "")

    def test_reload_compiles_game_scripts_with_a_bounded_wait(self):
        result = subprocess.CompletedProcess([], 0, "scripts reloaded", "")
        with mock.patch.object(debug.subprocess, "run", return_value=result) as run:
            output = self.client.reload_scripts()
        self.assertEqual(output, "scripts reloaded")
        self.assertEqual(run.call_args.args[0], [
            r"C:\tools\rw3d_cli.exe", "--ip", "127.0.0.1", "--target", "game",
            "--response-timeout", "195000", "--verbose", "reload", "--max-compile-time", "180000"])
        self.assertEqual(run.call_args.kwargs["timeout"], 200.0)
        self.assertEqual(self.client.last_invocation["returncode"], 0)

    def test_reload_failure_and_invalid_compile_limits_are_not_success(self):
        with self.assertRaises(ValueError):
            self.client.reload_scripts(0)
        failed = subprocess.CompletedProcess([], 2, "", "script compile failed")
        with mock.patch.object(debug.subprocess, "run", return_value=failed):
            with self.assertRaisesRegex(debug.RemoteExecutionError, "script compile failed"):
                self.client.reload_scripts()
        timed_out = subprocess.CompletedProcess(
            [], 1, "", "Error: Waited too long for the response\nCaused by:\n    timed out waiting on channel")
        with mock.patch.object(debug.subprocess, "run", return_value=timed_out):
            with self.assertRaisesRegex(TimeoutError, "reload response timed out"):
                self.client.reload_scripts()

    def test_cli_rejection_and_nonzero_exit_are_errors(self):
        rejected = subprocess.CompletedProcess([], 0, debug.EXEC_FAIL, "")
        with mock.patch.object(debug.subprocess, "run", return_value=rejected):
            with self.assertRaises(debug.RemoteExecutionError):
                self.client.exec_raw("missing_function()")

        failed = subprocess.CompletedProcess([], 2, "", "connection failed")
        with mock.patch.object(debug.subprocess, "run", return_value=failed):
            with self.assertRaisesRegex(debug.RemoteExecutionError, "connection failed"):
                self.client.exec_raw("NRNX_TestBridgeReady('4f9b')")

        timed_out = subprocess.CompletedProcess(
            [], 1, "", "Error: Waited too long for the response\nCaused by:\n    timed out waiting on channel")
        with mock.patch.object(debug.subprocess, "run", return_value=timed_out):
            with self.assertRaisesRegex(TimeoutError, "response timed out"):
                self.client.exec_raw("NRNX_TestBridgeState('4f9b')")

    def test_cli_timeout_and_invalid_response_are_not_success(self):
        with mock.patch.object(debug.subprocess, "run",
                               side_effect=subprocess.TimeoutExpired("rw3d_cli.exe", 10)):
            with self.assertRaises(TimeoutError):
                self.client.exec_raw("logstats()")

        malformed = subprocess.CompletedProcess([], 0, "connected, no response packet", "")
        with mock.patch.object(debug.subprocess, "run", return_value=malformed):
            with self.assertRaises(debug.ProtocolError):
                self.client.exec_raw("logstats()")

    def test_remote_targets_and_unsafe_bridge_arguments_are_rejected(self):
        with self.assertRaises(ValueError):
            debug.WitcherDebugger("rw3d_cli.exe", host="192.0.2.10")
        with self.assertRaises(ValueError):
            debug.WitcherDebugger("rw3d_cli.exe", port=37002)
        for value in ("../save.sav", "save.sav'()", "save name.sav", "保存.sav"):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    debug.quote_script_string(value)

    def test_parse_tcp_listeners_matches_only_requested_listening_port(self):
        output = "\n".join([
            "  TCP    0.0.0.0:37001      0.0.0.0:0      LISTENING       1234",
            "  TCP    [::]:37001         [::]:0         LISTENING       1234",
            "  TCP    127.0.0.1:37002   0.0.0.0:0      LISTENING       9999",
            "  TCP    127.0.0.1:37001   127.0.0.1:9   ESTABLISHED     8888",
        ])
        self.assertEqual(debug.parse_tcp_listeners(output), [
            dict(local_address="0.0.0.0:37001", pid=1234),
            dict(local_address="[::]:37001", pid=1234),
        ])

    def test_listener_snapshot_records_query_failure_without_raising(self):
        with mock.patch.object(debug.subprocess, "run", side_effect=OSError("netstat unavailable")):
            snapshot = debug.listener_snapshot()
        self.assertEqual(snapshot["listeners"], [])
        self.assertIn("netstat unavailable", snapshot["query_error"])


if __name__ == "__main__":
    unittest.main()
