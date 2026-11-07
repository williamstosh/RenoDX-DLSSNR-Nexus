"""Small lifecycle helper for offline test output directories."""

from __future__ import annotations

from datetime import datetime, timezone
import json
import os
from pathlib import Path
from typing import Any, Iterable


SCHEMA = "nr-nexus-test-output/1"
DEFAULT_BUDGET_BYTES = 4 * 1024 * 1024 * 1024


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def retention_mode() -> str:
    value = os.environ.get("NR_NEXUS_TEST_RETENTION", "Standard").casefold()
    return {"standard": "Standard", "full": "Full", "acceptance": "Acceptance"}.get(value, "Standard")


def budget_bytes() -> int:
    value = os.environ.get("NR_NEXUS_TEST_BUDGET_BYTES")
    if value is None or not value.strip():
        return DEFAULT_BUDGET_BYTES
    try:
        budget = int(value)
    except ValueError as error:
        raise ValueError("NR_NEXUS_TEST_BUDGET_BYTES must be a non-negative integer") from error
    if budget < 0:
        raise ValueError("NR_NEXUS_TEST_BUDGET_BYTES must be a non-negative integer")
    return budget


def _state_path(directory: Path) -> Path:
    return directory / "artifact-lifecycle.json"


def _read(directory: Path) -> dict[str, Any]:
    return json.loads(_state_path(directory).read_text(encoding="utf-8"))


def _write(directory: Path, state: dict[str, Any]) -> None:
    _state_path(directory).write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _owner_active(pid: int) -> bool:
    if pid == os.getpid():
        return True
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except (OSError, ProcessLookupError):
        return False
    return True


def _relative_file(directory: Path, path: Path) -> tuple[str, Path]:
    root = directory.resolve()
    candidate = path if path.is_absolute() else directory / path
    if candidate.is_symlink() or any(parent.is_symlink() for parent in candidate.parents if parent != directory):
        raise ValueError(f"lifecycle path is a link: {path}")
    candidate = candidate.resolve(strict=False)
    if not candidate.is_relative_to(root) or candidate == root:
        raise ValueError(f"lifecycle path escapes output directory: {path}")
    return candidate.relative_to(root).as_posix(), candidate


def initialize(directory: Path) -> None:
    directory = directory.resolve()
    if directory.exists():
        if any(directory.iterdir()):
            raise FileExistsError(f"Output already exists and is not empty: {directory}")
    else:
        directory.mkdir(parents=True)
    _write(directory, {
        "schema": SCHEMA,
        "runId": os.urandom(16).hex(),
        "ownerPid": os.getpid(),
        "mode": retention_mode(),
        "status": "Running",
        "startedUtc": utc_now(),
        "budgetBytes": budget_bytes(),
        "disposables": [],
        "cleanup": [],
        "cleanupErrors": [],
    })


def register_disposable(directory: Path, paths: Iterable[Path]) -> None:
    directory = directory.resolve()
    state = _read(directory)
    if state.get("status") != "Running" or state.get("ownerPid") != os.getpid():
        raise RuntimeError("disposable requires the active owner of an output directory")
    records = state.setdefault("disposables", [])
    known = {item.get("path") for item in records}
    for path in paths:
        relative, _ = _relative_file(directory, path)
        if relative not in known:
            records.append({"path": relative, "kind": "raw-readback"})
            known.add(relative)
    _write(directory, state)


def _cleanup(directory: Path, state: dict[str, Any]) -> None:
    if state.get("mode") != "Standard":
        return
    remaining = []
    for record in state.get("disposables", []):
        try:
            relative, path = _relative_file(directory, Path(str(record.get("path", ""))))
            if not path.exists():
                continue
            if path.is_file():
                size = path.stat().st_size
                path.unlink()
                state.setdefault("cleanup", []).append({"path": relative, "bytes": size,
                                                        "kind": record.get("kind", "raw-readback")})
            else:
                remaining.append(record)
        except (OSError, ValueError) as error:
            state.setdefault("cleanupErrors", []).append({"path": record.get("path"), "error": str(error)})
            remaining.append(record)
    state["disposables"] = remaining


def finish(directory: Path, status: str) -> None:
    if status not in {"Passed", "Failed"}:
        raise ValueError("lifecycle status must be Passed or Failed")
    directory = directory.resolve()
    state = _read(directory)
    if state.get("status") != "Running":
        return
    state["status"] = status
    _cleanup(directory, state)
    state["finishedUtc"] = utc_now()
    _write(directory, state)


def _tree_bytes(root: Path) -> int:
    total = 0
    for path in root.rglob("*"):
        if path.is_symlink() or not path.is_file():
            continue
        try:
            total += path.stat().st_size
        except OSError:
            continue
    return total


def maintain(root: Path, limit: int | None = None) -> None:
    root = root.resolve()
    if not root.is_dir():
        return
    limit = budget_bytes() if limit is None else limit
    states: list[tuple[Path, dict[str, Any]]] = []
    for marker in root.rglob("artifact-lifecycle.json"):
        if marker.is_symlink() or not marker.is_file():
            continue
        directory = marker.parent
        try:
            state = _read(directory)
        except (OSError, json.JSONDecodeError):
            continue
        if state.get("schema") != SCHEMA:
            continue
        if state.get("status") == "Running":
            if _owner_active(int(state.get("ownerPid", 0))):
                continue
            state["status"] = "Failed"
            state["failureReason"] = "owner process no longer exists"
            state["orphanedUtc"] = utc_now()
        elif _owner_active(int(state.get("ownerPid", 0))):
            continue
        _cleanup(directory, state)
        state["lastMaintenanceUtc"] = utc_now()
        _write(directory, state)
        states.append((directory, state))
    if limit <= 0:
        return
    for directory, state in sorted(states, key=lambda item: item[1].get("finishedUtc", "")):
        if _tree_bytes(root) <= limit:
            break
        if state.get("mode") != "Standard":
            continue
        _cleanup(directory, state)
        state["budgetCleanupUtc"] = utc_now()
        _write(directory, state)
