"""Stage and remove this runner's isolated, source-only Witcher script mod."""

import hashlib
import os
from pathlib import Path
import shutil
import stat


REPARSE_POINT = 0x400


def _plain(path):
    path = Path(os.path.abspath(os.fspath(path)))
    for part in (*reversed(path.parents), path):
        try:
            info = part.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & REPARSE_POINT:
            raise ValueError("Reparse points are forbidden in the temporary mod path")
    return path


def _sha(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class TemporaryScriptMod:
    def __init__(self, game_root, source_root, name="modNRNexusTestBridge"):
        if not name.startswith("mod") or not name.isascii() or not name.isalnum():
            raise ValueError("Temporary mod name must be an ASCII alphanumeric mod name")
        self.game_root = _plain(game_root)
        self.source_root = _plain(source_root)
        self.name = name
        self.target_root = self.game_root / "Mods" / name
        self.created_dirs = []
        self.files = []
        self.manifest = {}
        self.owned = False

    def _source_files(self):
        if not self.source_root.is_dir():
            raise FileNotFoundError("Temporary script mod source directory is missing")
        files = sorted(self.source_root.rglob("*.ws"))
        if not files:
            raise ValueError("Temporary script mod must contain at least one .ws file")
        result = []
        for path in files:
            _plain(path)
            if not path.is_file():
                raise ValueError("Temporary script mod source must contain regular files only")
            relative = path.relative_to(self.source_root)
            if any(part in (".", "..") for part in relative.parts):
                raise ValueError("Invalid temporary script mod path")
            result.append((path, relative, _sha(path)))
        return result

    def install(self):
        source_files = self._source_files()
        target = _plain(self.target_root)
        if target.exists():
            if not target.is_dir():
                raise FileExistsError("Temporary mod target is not a directory")
            current = {}
            for path in target.rglob("*"):
                _plain(path)
                if path.is_file():
                    current[path.relative_to(target)] = _sha(path)
            expected = {relative: digest for _, relative, digest in source_files}
            if current != expected:
                raise FileExistsError("Temporary mod target already contains different files")
            self.files = [target / relative for _, relative, _ in source_files]
            self.manifest = {str(path): _sha(path) for path in self.files}
            return self.files

        missing = []
        parent = target
        while not parent.exists():
            missing.append(parent)
            parent = parent.parent
        _plain(parent)
        for directory in reversed(missing):
            directory.mkdir()
            self.created_dirs.append(directory)
        self.owned = True
        try:
            for source, relative, expected_hash in source_files:
                destination = _plain(target / relative)
                destination.parent.mkdir(parents=True, exist_ok=True)
                _plain(destination.parent)
                shutil.copyfile(source, destination)
                if _sha(destination) != expected_hash:
                    raise RuntimeError("Temporary mod copy hash mismatch")
                self.files.append(destination)
                self.manifest[str(destination)] = expected_hash
        except BaseException:
            self.cleanup()
            raise
        return self.files

    def cleanup(self):
        if not self.owned:
            return
        target = _plain(self.target_root)
        if not target.exists():
            self.owned = False
            return
        current_files = set()
        current_dirs = set()
        for path in target.rglob("*"):
            _plain(path)
            relative = path.relative_to(target)
            if path.is_file():
                current_files.add(relative)
            elif path.is_dir():
                current_dirs.add(relative)
        expected_files = {Path(path).relative_to(target) for path in self.manifest}
        expected_dirs = {parent for relative in expected_files for parent in relative.parents
                         if str(parent) != "."}
        if current_files != expected_files or current_dirs != expected_dirs:
            raise RuntimeError("Temporary mod contains unexpected files; preserving it for recovery")
        for path, expected_hash in self.manifest.items():
            if _sha(path) != expected_hash:
                raise RuntimeError("Temporary mod changed during the run; preserving it for recovery")
        for path in self.files:
            path.unlink()
        directories = sorted((path for path in target.rglob("*") if path.is_dir()),
                             key=lambda path: len(path.parts), reverse=True)
        for path in directories:
            path.rmdir()
        target.rmdir()
        for directory in reversed(self.created_dirs):
            if directory.exists():
                directory.rmdir()
        self.owned = False
