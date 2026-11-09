"""Small lifecycle helper for offline test output directories."""

from __future__ import annotations

from datetime import datetime, timezone
import json
import os
import sys
from pathlib import Path
from typing import Any, Iterable


SCHEMA = "nr-nexus-test-output/1"
DEFAULT_BUDGET_BYTES = 10 * 1024 * 1024 * 1024


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
    return json.loads(_state_path(directory).read_text(encoding="utf-8-sig"))


def _write(directory: Path, state: dict[str, Any]) -> None:
    _state_path(directory).write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _owner_active(pid: int) -> bool:
    if pid == os.getpid():
        return True
    if pid <= 0:
        return False
    if os.name == "nt":
        # os.kill(pid, 0) terminates other processes on Windows. Query a process
        # handle instead; access denial is conservatively treated as active.
        import ctypes
        from ctypes import wintypes

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.OpenProcess(0x100000, False, pid)
        if not handle:
            return ctypes.get_last_error() != 87
        try:
            return kernel.WaitForSingleObject(handle, 0) != 0
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except (OSError, ProcessLookupError):
        return False
    return True


def _relative_file(directory: Path, path: Path) -> tuple[str, Path]:
    root = directory.resolve()
    candidate = path if path.is_absolute() else directory / path
    if _is_link(candidate) or any(_is_link(parent) for parent in candidate.parents if parent != directory):
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
        "keepRawArtifacts": os.environ.get("NR_NEXUS_TEST_KEEP_RAW") == "1",
        "status": "Running",
        "startedUtc": utc_now(),
        "budgetBytes": budget_bytes(),
        "disposables": [],
        "retainedPaths": [],
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


def retain(directory: Path, paths: Iterable[Path]) -> None:
    state = _read(directory)
    if state.get("status") != "Running" or state.get("ownerPid") != os.getpid():
        raise RuntimeError("retained evidence requires the active output owner")
    for path in paths:
        relative, _ = _relative_file(directory, path)
        if relative not in state.setdefault("retainedPaths", []):
            state["retainedPaths"].append(relative)
    _write(directory, state)


def _cleanup(directory: Path, state: dict[str, Any]) -> None:
    if state.get("keepRawArtifacts") is True:
        return
    remaining = []
    for record in state.get("disposables", []):
        if any(record.get("path") == saved or record.get("path", "").startswith(saved.rstrip("/") + "/")
               for saved in state.get("retainedPaths", [])):
            remaining.append(record)
            continue
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
    maintain(directory.parent)


def _tree_bytes(root: Path) -> int:
    total = 0
    pending = [os.fspath(root)]
    while pending:
        try:
            with os.scandir(pending.pop()) as entries:
                for entry in entries:
                    try:
                        if entry.is_symlink() or (hasattr(entry, "is_junction") and entry.is_junction()):
                            continue
                        if entry.is_dir(follow_symlinks=False):
                            pending.append(entry.path)
                        else:
                            total += entry.stat(follow_symlinks=False).st_size
                    except OSError:
                        continue
        except OSError:
            continue
    return total


def maintain_workspace(repository: Path) -> dict[str, int]:
    """Share one capacity budget across tmp/ and artifacts/ at top-level entry points."""
    roots = [repository / name for name in ("tmp", "artifacts")]
    limit = budget_bytes()
    sizes = [_tree_bytes(root) if root.is_dir() else 0 for root in roots]
    for index, root in enumerate(roots):
        capacity = max(1, limit - (sum(sizes) - sizes[index])) if limit > 0 and sum(sizes) > limit else 0
        maintain(root, limit=capacity)
        sizes[index] = _tree_bytes(root) if root.is_dir() else 0
    if limit > 0 and sum(sizes) > limit:
        print(f"Test outputs remain above the shared budget: {sum(sizes)} / {limit} bytes; "
              "active runs, recovery backups, retained evidence and unmanaged directories are protected.",
              file=sys.stderr)
    return {"budgetBytes": limit, "bytes": sum(sizes)}


def _is_link(path: Path) -> bool:
    return path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction())


def _walk_paths(root: Path) -> Iterable[Path]:
    # Path.rglob follows Windows junctions, including links back to this checkout.
    for directory, folders, files in os.walk(root, followlinks=False):
        for name in folders + files:
            yield Path(directory) / name
        folders[:] = [name for name in folders if not _is_link(Path(directory) / name)]


def _compact(directory: Path, state: dict[str, Any]) -> None:
    """Drop bulk run artifacts without relying on the disposable-file list."""
    if state.get("keepRawArtifacts") is True or (directory / "build-manifest.json").is_file():
        return
    items = list(_walk_paths(directory))
    if _is_link(directory) or any(_is_link(path) for path in items):
        raise ValueError("budget cleanup refuses reparse points")
    captures = {path.parent for path in items if path.name == "captures.json"}
    keep = list(state.get("retainedPaths", []))
    for marker in (path for path in items if path.name == "artifact-lifecycle.json"):
        nested = _read(marker.parent)
        if (nested.get("schema") != SCHEMA or nested.get("keepRawArtifacts") is True
                or _owner_active(int(nested.get("ownerPid", 0)))):
            return
        for saved in nested.get("retainedPaths", []):
            relative, _ = _relative_file(directory, marker.parent / saved)
            keep.append(relative)
        if (marker.parent / "build-manifest.json").is_file():
            keep.append(marker.parent.relative_to(directory).as_posix())
    for failure in (path for path in items if path.name == "failure.json"):
        capture = json.loads(failure.read_text(encoding="utf-8-sig"))
        keep.extend([failure.relative_to(directory).as_posix(),
                     failure.with_suffix(".rgba").relative_to(directory).as_posix()])
        if capture.get("atlas_file"):
            relative, _ = _relative_file(directory, failure.parent / capture["atlas_file"])
            keep.append(relative)
    evidence = {".json", ".jsonl", ".log", ".txt", ".md", ".csv", ".ini", ".xml", ".settings",
                ".py", ".ps1", ".cpp", ".h", ".hpp"}
    for path in items:
        if not path.is_file():
            continue
        relative = path.relative_to(directory).as_posix()
        if any(relative == saved or relative.startswith(saved.rstrip("/") + "/") for saved in keep):
            continue
        if any(part in {"inputs", "runner", "source", "failure-sample", "backup", "protected"}
               for part in path.relative_to(directory).parts[:-1]):
            continue
        mesh = path.parent in captures and path.suffix in {".json", ".rgba"}
        if path.name == "artifact-lifecycle.json" or (not mesh and path.suffix.lower() in evidence):
            continue
        state.setdefault("disposables", []).append({"path": relative, "kind": "budget-intermediate"})
    _cleanup(directory, state)


def maintain(root: Path, limit: int | None = None) -> None:
    root = root.resolve()
    if not root.is_dir():
        return
    limit = budget_bytes() if limit is None else limit
    states: list[tuple[Path, dict[str, Any]]] = []
    markers = [path for path in _walk_paths(root) if path.name == "artifact-lifecycle.json"]
    protected = []
    ancestor_markers = [parent / "artifact-lifecycle.json" for parent in root.parents
                        if (parent / "artifact-lifecycle.json").is_file()]
    for marker in markers + ancestor_markers:
        try:
            state = _read(marker.parent)
            if (state.get("schema") != SCHEMA or state.get("keepRawArtifacts") is True
                    or _owner_active(int(state.get("ownerPid", 0)))):
                protected.append(marker.parent)
        except (OSError, ValueError):
            protected.append(marker.parent)
    for marker in markers:
        if marker.is_symlink() or not marker.is_file():
            continue
        directory = marker.parent
        if any(saved.is_relative_to(directory) or directory.is_relative_to(saved) for saved in protected):
            continue
        if any(_is_link(path) for path in (directory, *directory.parents)):
            continue
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
        if state.get("status") in {"Passed", "Failed"}:
            states.append((directory, state))
    if limit <= 0:
        return
    size = _tree_bytes(root)
    for directory, state in sorted(states, key=lambda item: item[1].get("startedUtc", "")):
        if size <= limit:
            break
        if any(directory.is_relative_to(saved) for saved in protected):
            continue
        if state.get("keepRawArtifacts") is True:
            continue
        try:
            state = _read(directory)
            if state.get("keepRawArtifacts") is True or _owner_active(int(state.get("ownerPid", 0))):
                continue
            before = _tree_bytes(directory)
            _compact(directory, state)
            state["budgetCleanupUtc"] = utc_now()
            _write(directory, state)
            size -= before - _tree_bytes(directory)
        except (OSError, ValueError) as error:
            state.setdefault("cleanupErrors", []).append({"path": ".", "error": str(error)})
            _write(directory, state)
