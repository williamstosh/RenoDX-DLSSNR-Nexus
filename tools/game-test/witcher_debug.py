"""Invoke the game's script debugger through the verified local CLI."""

import ipaddress
import re
import subprocess


HOST = "127.0.0.1"
PORT = 37001
EXEC_ID = (0x12345678, -2129264632)
EXEC_FAIL = "Warn: Failed to process command"
EXEC_EMPTY = "Spam: Command executed without errors"
CLI_SHA256 = "0f04cccc69a9d833d59006fc9a7ff57b6342ac0c7a5b4c985d9379fbc07adf4c"
RELOAD_MAX_COMPILE_TIME_MS = 180_000


class ProtocolError(RuntimeError):
    pass


class RemoteExecutionError(RuntimeError):
    pass


def quote_script_string(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", value):
        raise ValueError("Bridge string arguments must contain only ASCII letters, digits, dot, underscore or hyphen")
    return "'" + value + "'"


def parse_tcp_listeners(output, port=PORT):
    listeners = []
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 5 or fields[0].upper() != "TCP" or fields[-2].upper() != "LISTENING":
            continue
        try:
            local_port = int(fields[1].rsplit(":", 1)[1])
            pid = int(fields[-1])
        except (IndexError, ValueError):
            continue
        if local_port == port:
            listeners.append(dict(local_address=fields[1], pid=pid))
    return listeners


def listener_snapshot(port=PORT):
    try:
        result = subprocess.run(
            ["netstat.exe", "-ano", "-p", "tcp"], capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=3, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        return dict(port=port, listeners=[], query_error=repr(error))
    snapshot = dict(port=port, listeners=parse_tcp_listeners(result.stdout, port),
                    query_exit_code=result.returncode)
    if result.returncode:
        snapshot["query_error"] = result.stderr.strip()
    return snapshot


class WitcherDebugger:
    """Use the upstream CLI that has completed a real game round-trip."""

    def __init__(self, executable, host=HOST, port=PORT, timeout=3.0):
        if not ipaddress.ip_address(host).is_loopback:
            raise ValueError("Witcher debugger connections are restricted to loopback")
        if port != PORT:
            raise ValueError("The Witcher debugger CLI uses its fixed game port")
        if not isinstance(timeout, (int, float)) or timeout <= 0:
            raise ValueError("Witcher debugger timeout must be positive")
        if not executable:
            raise ValueError("A local Witcher debugger CLI path is required")
        self.executable = str(executable)
        self.host, self.port, self.timeout = host, port, float(timeout)
        self.last_invocation = None

    def close(self):
        return None

    def exec_raw(self, command):
        if not isinstance(command, str) or not command or "\x00" in command:
            raise ValueError("A non-empty script command is required")
        try:
            result = subprocess.run(
                [self.executable, "--ip", self.host, "--target", "game",
                 "--response-timeout", str(round(self.timeout * 1000)),
                 "--verbose", "exec", command],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
                timeout=max(10.0, self.timeout + 5.0), check=False)
        except subprocess.TimeoutExpired as error:
            self.last_invocation = dict(returncode=None, stdout="", stderr="process_timeout")
            raise TimeoutError("Witcher debugger CLI timed out") from error
        except OSError as error:
            self.last_invocation = dict(returncode=None, stdout="", stderr=repr(error))
            raise RuntimeError("Unable to start the Witcher debugger CLI") from error
        self.last_invocation = dict(returncode=result.returncode,
                                   stdout=result.stdout, stderr=result.stderr)
        if result.returncode:
            details = (result.stderr or result.stdout).strip()[-500:]
            lowered = details.casefold()
            if ('waited too long for the response' in lowered
                    or 'timed out waiting on channel' in lowered):
                raise TimeoutError('Witcher debugger CLI response timed out: ' + details)
            raise RemoteExecutionError(
                f"Witcher debugger CLI exited with code {result.returncode}: {details}")
        if EXEC_FAIL in result.stdout:
            raise RemoteExecutionError("Witcher script command was rejected")
        if EXEC_EMPTY in result.stdout:
            return ""
        packets = re.finditer(
            r"PACKET \(\d+ bytes\)\r?\n-{20,}\r?\n(.*?)(?=\r?\n-{20,}\r?\n)",
            result.stdout, re.DOTALL)
        responses = []
        for packet in packets:
            body = packet.group(1)
            integers = re.findall(r"Type: Int32\s+Value: (-?\d+)", body)
            strings = re.findall(
                r"Type: StringUTF8\s+Length: \d+\s+Value: ([^\r\n]*)", body)
            if integers[:2] == [str(value) for value in EXEC_ID] and len(strings) == 1:
                responses.append(strings[0])
        if len(responses) != 1:
            raise ProtocolError("Witcher debugger CLI returned no unique execution response")
        return responses[0]

    def exec_bridge(self, function, *arguments):
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", function):
            raise ValueError("Invalid Witcher bridge function name")
        command = function + "(" + ",".join(quote_script_string(arg) for arg in arguments) + ")"
        return self.exec_raw(command)

    def reload_scripts(self, max_compile_time_ms=RELOAD_MAX_COMPILE_TIME_MS):
        if not isinstance(max_compile_time_ms, int) or max_compile_time_ms <= 0:
            raise ValueError("Script reload compile limit must be a positive integer")
        response_timeout_ms = max_compile_time_ms + 15000
        try:
            result = subprocess.run(
                [self.executable, "--ip", self.host, "--target", "game",
                 "--response-timeout", str(response_timeout_ms), "--verbose",
                 "reload", "--max-compile-time", str(max_compile_time_ms)],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
                timeout=(response_timeout_ms / 1000) + 5.0, check=False)
        except subprocess.TimeoutExpired as error:
            self.last_invocation = dict(returncode=None, stdout="", stderr="process_timeout")
            raise TimeoutError("Witcher script reload timed out") from error
        except OSError as error:
            self.last_invocation = dict(returncode=None, stdout="", stderr=repr(error))
            raise RuntimeError("Unable to start the Witcher debugger CLI") from error
        self.last_invocation = dict(returncode=result.returncode,
                                   stdout=result.stdout, stderr=result.stderr)
        if result.returncode:
            details = (result.stderr or result.stdout).strip()[-500:]
            lowered = details.casefold()
            if ('waited too long for the response' in lowered
                    or 'timed out waiting on channel' in lowered):
                raise TimeoutError("Witcher script reload response timed out: " + details)
            raise RemoteExecutionError(
                f"Witcher script reload exited with code {result.returncode}: {details}")
        return result.stdout
