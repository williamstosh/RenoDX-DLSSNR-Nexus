"""Versioned fixture adapter for the existing FG/XeFG/lifecycle test suites."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from typing import Any
import uuid
from ctypes import wintypes
try:
    import output_lifecycle
except ModuleNotFoundError:  # Loaded by the repository's unit tests via a file spec.
    import importlib.util
    _lifecycle_spec = importlib.util.spec_from_file_location("output_lifecycle", Path(__file__).with_name("output_lifecycle.py"))
    output_lifecycle = importlib.util.module_from_spec(_lifecycle_spec)
    assert _lifecycle_spec.loader is not None
    _lifecycle_spec.loader.exec_module(output_lifecycle)

ROOT = Path(__file__).resolve().parents[2]
FIXTURE_SCHEMA = "nr-nexus-offline-suite-fixture/1"
RESULT_SCHEMA = "nr-nexus-offline-result/1"
EVIDENCE_LEVELS = ("STATIC", "MOCK", "WARP", "NATIVE_GPU", "NATIVE_NGX", "IN_GAME", "VISUAL")
DEFAULT_TIMEOUT_MS = 600_000
MAX_TIMEOUT_MS = 900_000
RUN_OUTPUT_NAME = "test-run"

# Explicit mappings keep case evidence independent from the outer process status.
NATIVE_CASE_EVIDENCE = {
    "same-stage-resolution": ("WARP", "MOCK"),
    "same-stage-resolution-hardware": ("NATIVE_GPU", "MOCK"),
    "same-stage-model-pair": ("NATIVE_GPU", "MOCK"),
    "same-stage-mixed-pair": ("WARP", "MOCK"),
    "stage-resolution": ("WARP", "MOCK"),
    "stage-resolution-hardware": ("NATIVE_GPU", "MOCK"),
    "render-stage-resolution": ("WARP", "MOCK"),
    "render-stage-resolution-hardware": ("NATIVE_GPU", "MOCK"),
    "present-stage-resolution": ("WARP", "MOCK"),
    "present-stage-resolution-hardware": ("NATIVE_GPU", "MOCK"),
    "present-guide-models": ("WARP", "MOCK"),
    "present-chain": ("WARP", "MOCK"),
    "present-chain-hardware": ("NATIVE_GPU", "MOCK"),
    "feature-switch": ("WARP", "MOCK"),
    "feature-switch-hardware": ("NATIVE_GPU", "MOCK"),
}
CASE_ENTRIES = {
    "fg-xefg": {"resource-lifecycle": ("fg-xefg", "test.stdout.log")},
    "lifecycle": {"dll-lifecycle": ("lifecycle", "host-exit.json")},
}
SOURCE_INPUTS = (
    "tools/offline/suites.py", "tools/run-tests.ps1", "tests/test-support.ps1", "tests/fg_native_tests.cpp",
    "tests/fg_xefg_tests.cpp", "tests/fg_lifecycle_tests.cpp",
    "tests/fg_lifecycle_sdk.cpp", "tests/fg_lifecycle_fixture.cpp",
    "tests/run-fg-lifecycle-tests.ps1", "tests/stage_execution_cases.hpp",
    "tests/same_stage_resolution_cases.hpp", "tests/feature_switch_cases.hpp",
    "tests/present_stage_resolution_cases.hpp",
)


class FixtureError(ValueError):
    """Invalid fixture or invocation contract."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fixture_identity(fixture: dict[str, Any]) -> str:
    payload = json.dumps(fixture, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result = {}
    for key, value in pairs:
        if key in result:
            raise FixtureError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def validate_fixture(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) - {"schema", "fixture_id", "timeout_ms", "cases"}:
        raise FixtureError("Fixture must be an object with only schema, fixture_id, timeout_ms and cases")
    if value.get("schema") != FIXTURE_SCHEMA:
        raise FixtureError(f"schema must be {FIXTURE_SCHEMA}")
    fixture_id = value.get("fixture_id")
    if not isinstance(fixture_id, str) or not fixture_id or len(fixture_id) > 128:
        raise FixtureError("fixture_id must be a nonempty string of at most 128 characters")
    timeout_ms = value.get("timeout_ms", DEFAULT_TIMEOUT_MS)
    if type(timeout_ms) is not int or not 1_000 <= timeout_ms <= MAX_TIMEOUT_MS:
        raise FixtureError(f"timeout_ms must be an integer from 1000 to {MAX_TIMEOUT_MS}")
    cases = value.get("cases")
    if not isinstance(cases, list) or not cases:
        raise FixtureError("cases must be a nonempty array")
    ids: set[str] = set()
    selected: set[tuple[str, str]] = set()
    checked_cases = []
    for case in cases:
        if not isinstance(case, dict) or set(case) != {"id", "suite", "case"}:
            raise FixtureError("Each case must contain exactly id, suite and case")
        case_id, suite, name = case["id"], case["suite"], case["case"]
        if not isinstance(case_id, str) or not case_id or len(case_id) > 128:
            raise FixtureError("case id must be a nonempty string of at most 128 characters")
        if case_id.casefold() in ids:
            raise FixtureError(f"Duplicate case id: {case_id}")
        ids.add(case_id.casefold())
        if not isinstance(suite, str) or not isinstance(name, str):
            raise FixtureError("suite and case must be strings")
        if suite == "fg-native":
            if name not in NATIVE_CASE_EVIDENCE:
                raise FixtureError(f"Unsupported fg-native NativeCase: {name!r}")
        elif suite in CASE_ENTRIES:
            if name not in CASE_ENTRIES[suite]:
                raise FixtureError(f"Unsupported {suite} case: {name!r}")
        else:
            raise FixtureError(f"Unsupported suite: {suite!r}")
        key = (suite, name)
        if key in selected:
            raise FixtureError(f"Duplicate suite case: {suite}/{name}")
        selected.add(key)
        checked_cases.append({"id": case_id, "suite": suite, "case": name})
    return {"schema": FIXTURE_SCHEMA, "fixture_id": fixture_id,
            "timeout_ms": timeout_ms, "cases": checked_cases}


def build_command(fixture: dict[str, Any], output: Path, powershell: str = "pwsh") -> list[str]:
    fixture = validate_fixture(fixture)
    suites = [name for name in ("fg-native", "fg-xefg", "lifecycle")
              if any(case["suite"] == name for case in fixture["cases"])]
    quote = lambda value: "'" + str(value).replace("'", "''") + "'"
    suite_array = "@(" + ",".join(quote(value) for value in suites) + ")"
    suite_output = output / RUN_OUTPUT_NAME
    script = ("$ErrorActionPreference='Stop'; & " + quote(ROOT / "tools" / "run-tests.ps1")
              + " -Suite " + suite_array + " -OutputDirectory " + quote(suite_output)
              + " -UiPreview 'None' -RetentionMode " + quote(output_lifecycle.retention_mode()))
    native_cases = [case["case"] for case in fixture["cases"] if case["suite"] == "fg-native"]
    if native_cases:
        native_array = "@(" + ",".join(quote(value) for value in native_cases) + ")"
        script += " -NativeCase " + native_array
    return [powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", script]


def _source_identity() -> dict[str, Any]:
    files = []
    for relative in SOURCE_INPUTS:
        path = ROOT / relative
        if not path.is_file():
            raise FileNotFoundError(f"Required source input is missing: {relative}")
        files.append({"path": relative, "sha256": _sha256(path)})
    digest = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    return {"files": files, "sha256": digest}


def _new_output(requested: Path | None) -> Path:
    root = Path(os.path.abspath(ROOT))
    requested_path = (requested if requested is not None else root / "tmp" / "offline" / "suites" /
                      (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]))
    path = Path(os.path.abspath(requested_path))
    allowed_roots = [root / name for name in ("tmp", "artifacts")]
    allowed = next((candidate for candidate in allowed_roots
                    if path != candidate and candidate in path.parents), None)
    if allowed is None:
        raise FixtureError("Output must be a new directory below repository tmp/ or artifacts/")
    current = path.parent
    while current != root and current != allowed.parent:
        if current.exists():
            item = current.stat()
            if current.is_symlink() or getattr(item, "st_file_attributes", 0) & 0x400:
                raise FixtureError(f"Output path crosses a reparse point: {current}")
        current = current.parent
    if path.exists():
        raise FileExistsError(f"Output already exists: {path}")
    output_lifecycle.maintain(path.parent)
    path.mkdir(parents=True, exist_ok=False)
    output_lifecycle.initialize(path)
    return path


def _case_records(fixture: dict[str, Any], output: Path, process_ok: bool) -> list[dict[str, Any]]:
    records = []
    for item in fixture["cases"]:
        if item["suite"] == "fg-native":
            directory = output / RUN_OUTPUT_NAME / "fg-native"
            raw = Path(item["case"] + ".stdout.log")
            evidence = list(NATIVE_CASE_EVIDENCE[item["case"]])
        else:
            directory_name, raw_name = CASE_ENTRIES[item["suite"]][item["case"]]
            directory, raw = output / RUN_OUTPUT_NAME / directory_name, Path(raw_name)
            evidence = ["WARP", "MOCK"] if item["suite"] == "fg-xefg" else ["MOCK"]
        artifact = directory / raw
        suite_result = directory / "result.json"
        entry_ok = artifact.is_file()
        if item["suite"] == "lifecycle" and entry_ok:
            try:
                exit_record = json.loads(artifact.read_text(encoding="utf-8"))
                entry_ok = type(exit_record.get("exit_code")) is int and exit_record["exit_code"] == 0
            except (OSError, json.JSONDecodeError, AttributeError):
                entry_ok = False
        suite_ok = True
        if item["suite"] in {"fg-native", "fg-xefg"}:
            try:
                suite_record = json.loads(suite_result.read_text(encoding="utf-8"))
                suite_ok = (suite_record.get("suite") == item["suite"]
                            and suite_record.get("build") == "passed"
                            and suite_record.get("run") == "passed")
            except (OSError, json.JSONDecodeError, AttributeError):
                suite_ok = False
        failure_reasons = []
        if not process_ok:
            failure_reasons.append("runner-failed")
        if not entry_ok:
            failure_reasons.append("missing-or-invalid-case-artifact")
        if not suite_ok:
            failure_reasons.append("missing-or-failed-suite-result")
        status = "passed" if not failure_reasons else "failed"
        records.append({"id": item["id"], "suite": item["suite"], "case": item["case"],
                        "status": status, "evidence": evidence,
                        "failure_reasons": failure_reasons,
                        "raw_artifacts": [str(artifact.relative_to(output))] if artifact.is_file() else [],
                        "suite_result": str(suite_result.relative_to(output)) if suite_result.is_file() else None})
    return records


class _WindowsProcessJob:
    """Own the Windows test process and children so timeout cleanup is bounded."""

    def __init__(self) -> None:
        class IoCounters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_ulonglong) for name in (
                "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

        class BasicLimitInformation(ctypes.Structure):
            _fields_ = [
                ("PerProcessUserTimeLimit", ctypes.c_longlong),
                ("PerJobUserTimeLimit", ctypes.c_longlong),
                ("LimitFlags", wintypes.DWORD),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t),
                ("PriorityClass", wintypes.DWORD),
                ("SchedulingClass", wintypes.DWORD),
            ]

        class ExtendedLimitInformation(ctypes.Structure):
            _fields_ = [
                ("BasicLimitInformation", BasicLimitInformation),
                ("IoInfo", IoCounters),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t),
            ]

        class BasicAccountingInformation(ctypes.Structure):
            _fields_ = [
                ("TotalUserTime", ctypes.c_longlong),
                ("TotalKernelTime", ctypes.c_longlong),
                ("ThisPeriodTotalUserTime", ctypes.c_longlong),
                ("ThisPeriodTotalKernelTime", ctypes.c_longlong),
                ("TotalPageFaultCount", wintypes.DWORD),
                ("TotalProcesses", wintypes.DWORD),
                ("ActiveProcesses", wintypes.DWORD),
                ("TotalTerminatedProcesses", wintypes.DWORD),
            ]

        self._extended_limit_type = ExtendedLimitInformation
        self._accounting_type = BasicAccountingInformation
        self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        self._kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        self._kernel32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int,
                                                            ctypes.c_void_p, wintypes.DWORD]
        self._kernel32.SetInformationJobObject.restype = wintypes.BOOL
        self._kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        self._kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
        self._kernel32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
        self._kernel32.TerminateJobObject.restype = wintypes.BOOL
        self._kernel32.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int,
                                                              ctypes.c_void_p, wintypes.DWORD,
                                                              ctypes.POINTER(wintypes.DWORD)]
        self._kernel32.QueryInformationJobObject.restype = wintypes.BOOL
        self._kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        self._kernel32.CloseHandle.restype = wintypes.BOOL
        self._handle = self._kernel32.CreateJobObjectW(None, None)
        if not self._handle:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = self._extended_limit_type()
        limits.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self._kernel32.SetInformationJobObject(self._handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            error = ctypes.get_last_error()
            self.close()
            raise ctypes.WinError(error)

    def assign(self, process: subprocess.Popen[str]) -> None:
        if not self._kernel32.AssignProcessToJobObject(self._handle, wintypes.HANDLE(int(process._handle))):
            raise ctypes.WinError(ctypes.get_last_error())

    def terminate(self) -> bool:
        return bool(self._kernel32.TerminateJobObject(self._handle, 1))

    def active_processes(self) -> int:
        info = self._accounting_type()
        returned = wintypes.DWORD()
        if not self._kernel32.QueryInformationJobObject(self._handle, 1, ctypes.byref(info),
                                                         ctypes.sizeof(info), ctypes.byref(returned)):
            raise ctypes.WinError(ctypes.get_last_error())
        return int(info.ActiveProcesses)

    def close(self) -> None:
        handle = getattr(self, "_handle", None)
        if handle:
            self._handle = None
            self._kernel32.CloseHandle(handle)


def _terminate_process_tree(process: subprocess.Popen[str], job: _WindowsProcessJob | None = None) -> dict[str, Any]:
    if os.name == "nt":
        if job is not None:
            try:
                active_before = job.active_processes()
                if active_before == 0:
                    return {"tree_terminated": True, "method": "windows-job-already-exited",
                            "active_processes_after_termination": 0}
                if job.terminate():
                    deadline = time.monotonic() + 5
                    while True:
                        active = job.active_processes()
                        if active == 0:
                            return {"tree_terminated": True, "method": "windows-job-object",
                                    "active_processes_before_termination": active_before,
                                    "active_processes_after_termination": active}
                        if time.monotonic() >= deadline:
                            return {"tree_terminated": False, "method": "windows-job-object",
                                    "active_processes_before_termination": active_before,
                                    "active_processes_after_termination": active}
                        time.sleep(0.05)
                job_error = "terminate-job-failed"
            except (OSError, subprocess.TimeoutExpired) as error:
                job_error = type(error).__name__
        else:
            job_error = "job-object-unavailable"
        executable = shutil.which("taskkill")
        if executable is not None:
            try:
                result = subprocess.run([executable, "/PID", str(process.pid), "/T", "/F"],
                                        capture_output=True, text=True, encoding="utf-8",
                                        errors="replace", timeout=15, check=False)
                if result.returncode == 0:
                    return {"tree_terminated": True, "method": "taskkill-tree",
                            "return_code": result.returncode}
                detail = result.stderr.strip() or result.stdout.strip()
            except (OSError, subprocess.TimeoutExpired) as error:
                detail = type(error).__name__
        else:
            detail = "taskkill-not-found"
        try:
            process.kill()
        except OSError:
            pass
        return {"tree_terminated": False, "method": "root-kill-fallback", "detail": detail,
                "job_object": job_error}
    try:
        os.killpg(process.pid, signal.SIGKILL)
        return {"tree_terminated": True, "method": "process-group-kill"}
    except ProcessLookupError:
        return {"tree_terminated": True, "method": "process-group-already-exited"}
    except OSError as error:
        try:
            process.kill()
        except OSError:
            pass
        return {"tree_terminated": False, "method": "root-kill-fallback",
                "detail": type(error).__name__}


def _captured_text(value: str | bytes | None) -> str:
    if value is None:
        return ""
    return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value


def _evidence_summary(cases: list[dict[str, Any]], source_unchanged: bool) -> dict[str, Any]:
    summary: dict[str, Any] = {
        "STATIC": {"status": "passed" if source_unchanged else "failed",
                    "scope": "fixture schema, fixture identity and before/after source identity"},
    }
    for level in EVIDENCE_LEVELS[1:4]:
        applicable = [case for case in cases if level in case["evidence"]]
        summary[level] = {
            "status": ("not_applicable" if not applicable else
                        "passed" if all(case["status"] == "passed" for case in applicable) else "failed"),
            "case_ids": [case["id"] for case in applicable],
        }
    for level in EVIDENCE_LEVELS[4:]:
        summary[level] = {"status": "not_evaluated"}
    return summary


def run(fixture_path: Path, output_path: Path | None = None, powershell: str = "pwsh") -> dict[str, Any]:
    fixture = validate_fixture(json.loads(fixture_path.read_text(encoding="utf-8"), object_pairs_hook=_strict_object))
    identity = fixture_identity(fixture)
    source_before = _source_identity()
    output = _new_output(output_path)
    (output / "fixture.json").write_text(json.dumps(fixture, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (output / "source-identity.json").write_text(
        json.dumps({"before": source_before}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    command = build_command(fixture, output, powershell)
    started = datetime.now(timezone.utc)
    return_code: int | None = None
    timed_out = False
    termination = {"tree_terminated": True, "method": "not-needed"}
    process_job: _WindowsProcessJob | None = None
    process_job_error: str | None = None
    stdout, stderr = "", ""
    try:
        process_options: dict[str, Any] = {
            "cwd": ROOT, "stdout": subprocess.PIPE, "stderr": subprocess.PIPE,
            "text": True, "encoding": "utf-8", "errors": "replace",
        }
        if os.name == "nt":
            process_options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            process_options["start_new_session"] = True
        if os.name == "nt":
            try:
                process_job = _WindowsProcessJob()
            except OSError as error:
                process_job_error = f"job-create:{getattr(error, 'winerror', None)}"
        process = subprocess.Popen(command, **process_options)
        if process_job is not None:
            try:
                process_job.assign(process)
            except OSError as error:
                process_job_error = f"job-assign:{getattr(error, 'winerror', None)}"
                process_job.close()
                process_job = None
        try:
            stdout, stderr = process.communicate(timeout=fixture["timeout_ms"] / 1000)
            return_code = process.returncode
            if process_job is not None:
                active = process_job.active_processes()
                if active:
                    termination = _terminate_process_tree(process, process_job)
                    termination["children_active_after_runner_exit"] = active
                    termination["cleanup_reason"] = "terminate remaining members of this runner-owned job"
                else:
                    termination = {"tree_terminated": True, "method": "windows-job-exited",
                                   "active_processes_after_exit": 0}
        except subprocess.TimeoutExpired as error:
            timed_out = True
            stdout, stderr = _captured_text(error.stdout), _captured_text(error.stderr)
            termination = _terminate_process_tree(process, process_job)
            try:
                stdout, stderr = process.communicate(timeout=15)
            except subprocess.TimeoutExpired as cleanup_error:
                termination["tree_terminated"] = False
                termination["detail"] = "pipes-did-not-close-after-termination"
                stdout = _captured_text(cleanup_error.stdout) or stdout
                stderr = _captured_text(cleanup_error.stderr) or stderr
                try:
                    process.kill()
                    process.wait(timeout=5)
                    termination["root_reaped"] = True
                except (OSError, subprocess.TimeoutExpired):
                    termination["root_reaped"] = False
                for stream in (process.stdout, process.stderr):
                    if stream is not None:
                        stream.close()
            return_code = process.poll()
        finally:
            if process_job is not None:
                process_job.close()
    except subprocess.TimeoutExpired as error:
        timed_out = True
        stdout, stderr = error.stdout or "", error.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", errors="replace")
    except OSError as error:
        stderr = f"Runner could not start existing test entry: {error}\n"
    (output / "runner.stdout.log").write_text(stdout, encoding="utf-8")
    (output / "runner.stderr.log").write_text(stderr, encoding="utf-8")
    try:
        source_after: dict[str, Any] = _source_identity()
        source_unchanged = source_before["sha256"] == source_after["sha256"]
    except OSError as error:
        source_after = {"error": str(error)}
        source_unchanged = False
    source = {"before": source_before, "after": source_after, "unchanged": source_unchanged}
    (output / "source-identity.json").write_text(
        json.dumps(source, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    process_ok = (return_code == 0 and not timed_out and source_unchanged
                  and termination["tree_terminated"])
    cases = _case_records(fixture, output, process_ok)
    status = "passed" if process_ok and all(case["status"] == "passed" for case in cases) else "failed"
    raw_artifacts = [
        {"path": str(path.relative_to(output)).replace(os.sep, "/"),
         "bytes": path.stat().st_size, "sha256": _sha256(path)}
        for path in sorted(output.rglob("*"))
        if path.is_file() and path.name not in {"runner.stdout.log", "runner.stderr.log",
                                                "fixture.json", "source-identity.json", "result.json"}
    ]
    result = {
        "schema": RESULT_SCHEMA,
        "output_directory": str(output.relative_to(ROOT)).replace(os.sep, "/"),
        "fixture": {"id": fixture["fixture_id"], "sha256": identity, "path": "fixture.json"},
        "source": {**source, "identity_path": "source-identity.json"},
        "runner": {"entry": "tools/run-tests.ps1", "command": command,
                   "timeout_ms": fixture["timeout_ms"], "timed_out": timed_out,
                   "termination": termination,
                   "process_job_error": process_job_error,
                   "exit_code": return_code, "started_utc": started.isoformat(),
                   "finished_utc": datetime.now(timezone.utc).isoformat(),
                   "stdout": "runner.stdout.log", "stderr": "runner.stderr.log"},
        "status": status, "source_unchanged": source_unchanged,
        "evidence_levels": _evidence_summary(cases, source_unchanged),
        "cases": cases, "raw_artifacts": raw_artifacts,
    }
    (output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    output_lifecycle.finish(output, "Passed" if status == "passed" else "Failed")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path, help="Strict nr-nexus-offline-suite-fixture/1 JSON")
    parser.add_argument("--output", type=Path, help="New directory under repository tmp/ or artifacts/")
    parser.add_argument("--powershell", default="pwsh", help="PowerShell executable for the existing test entry")
    args = parser.parse_args(argv)
    try:
        result = run(args.fixture, args.output, args.powershell)
    except (FixtureError, OSError, json.JSONDecodeError, subprocess.SubprocessError) as error:
        print(f"offline suite runner: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"status": result["status"], "output": result["output_directory"],
                      "result": "result.json"}, ensure_ascii=False))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
