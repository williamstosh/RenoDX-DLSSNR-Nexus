"""Bounded, read-only snapshots for the protected game-test attempt."""
import hashlib
from itertools import islice
import json
import os
from pathlib import Path
import subprocess
from datetime import datetime, timezone

LIMIT = 128 * 1024 * 1024
MAX_FILES = 64
MAX_BYTES = 512 * 1024 * 1024
SCAN_LIMIT = 4096


def copy_snapshot(source, destination):
    size = source.stat().st_size
    if size > LIMIT:
        raise ValueError(f'file exceeds {LIMIT} bytes: {source}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Never replace earlier evidence, including a partial copy.
    with source.open('rb') as src, destination.open('xb') as dst:
        remaining = size
        while remaining:
            chunk = src.read(min(1024 * 1024, remaining))
            if not chunk:
                raise OSError('source shortened during copy')
            dst.write(chunk)
            remaining -= len(chunk)
        if src.read(1):
            raise OSError('source grew during copy')
    return dict(source=str(source), bytes=size)


class Budget:
    def __init__(self):
        self.files = 0
        self.bytes = 0

    def copy(self, source, destination):
        size = source.stat().st_size
        if self.files >= MAX_FILES or self.bytes + size > MAX_BYTES or size > LIMIT:
            return dict(source=str(source), status='skipped', reason='copy_budget_or_file_size_limit', bytes=size)
        # Reserve even on failure: a partial destination must count against the budget.
        self.files += 1
        self.bytes += size
        return copy_snapshot(source, destination)


def write_new(path, content):
    with path.open('x', encoding='utf-8') as stream:
        stream.write(content)


def configurations(cfg, out):
    out.mkdir(parents=True, exist_ok=True)
    records = []
    budget = Budget()
    install = Path(cfg['executable']).parent
    reshade = install / 'ReShade.ini'
    if not reshade.is_file():
        records.append(dict(source=str(reshade), error='not found'))
    groups = [(install, '*.ini', out)]
    if cfg.get('profile'):
        groups.append((Path(cfg['profile']), '*.settings*', out / 'profile'))
    for root, pattern, dest in groups:
        sources = list(islice(root.glob(pattern), MAX_FILES + 1))
        if len(sources) > MAX_FILES:
            records.append(dict(source=str(root / pattern), status='skipped', reason='enumeration_limit'))
        sources = sources[:MAX_FILES]
        if root == install and reshade.is_file() and reshade not in sources:
            sources.insert(0, reshade)
        for source in sources:
            try:
                records.append(budget.copy(source, dest / source.name))
            except Exception as error:
                records.append(dict(source=str(source), error=str(error)))
    write_new(out / 'manifest.json', json.dumps(records, indent=2))
    return records


def modules(process, enumerate_modules, required_module=None):
    result = dict(process=process, status='unavailable', modules=[])
    if not process:
        result['error'] = 'owned process not observed'
        return result
    try:
        paths = enumerate_modules(process)
        result['status'] = 'enumerated'
        result['total'] = len(paths)
        result['paths_truncated'] = len(paths) > 4096
        entries = [dict(path=name) for name in paths[:4096]]
        result['modules'] = entries
        required = (required_module or '').lower()

        def priority(entry):
            name = Path(entry['path']).name.lower()
            if required and name == required:
                return 0
            if any(token in name for token in ('nvngx', 'dlss', 'streamline', 'optiscaler', 'reshade', 'renodx')) or name.startswith('sl.') or name.startswith('sl_'):
                return 1
            return 2

        used = 0
        for entry in sorted(entries, key=priority):
            name = entry['path']
            try:
                size = Path(name).stat().st_size
                if size > LIMIT or used + size > MAX_BYTES:
                    entry.update(hash_status='budget_skipped', disk_bytes=size)
                    continue
                used += size
                digest = hashlib.sha256()
                with open(name, 'rb') as stream:
                    remaining = size
                    while remaining:
                        chunk = stream.read(min(1024 * 1024, remaining))
                        if not chunk:
                            raise OSError('module shortened during hash')
                        digest.update(chunk)
                        remaining -= len(chunk)
                    if stream.read(1):
                        raise OSError('module grew during hash')
                entry.update(hash_status='hashed', disk_sha256=digest.hexdigest(), disk_bytes=size)
            except (OSError, ValueError) as error:
                entry.update(hash_status='error', error=str(error))
        result['hash_budget_bytes'] = MAX_BYTES
        result['hash_bytes_reserved'] = used
    except Exception as error:
        result['error'] = str(error)
    return result


def owned_process_dumps(records, process):
    if not process:
        return []
    executable = Path(str(process["path"]).replace("\\", "/")).name
    expected = f"{executable}.{process['pid']}.dmp".casefold()
    return [record for record in records
            if Path(str(record.get("source", "")).replace("\\", "/")).name.casefold() == expected
            and "bytes" in record and "error" not in record
            and (not record.get("status")
                 or record.get("reason") == "copy_budget_or_file_size_limit")]


def collect(cfg, out, started, phase, run=subprocess.run):
    destination = out / 'crash-evidence' / phase
    destination.mkdir(parents=True, exist_ok=False)
    records = []
    budget = Budget()
    start_time = datetime.fromisoformat(started).timestamp() - 5
    # Filesystem timestamps may be rounded slightly ahead of the wall clock.
    end_time = datetime.now(timezone.utc).timestamp() + 1
    # XML retains all event fields. A filename match identifies candidates, not ownership.
    command = ("$ErrorActionPreference='Stop'; $p=[Console]::In.ReadToEnd() | ConvertFrom-Json; "
               "$s=[DateTime]::Parse($p.started).ToUniversalTime(); "
               "try { $events=@(Get-WinEvent -FilterHashtable @{LogName='Application';Id=1000,1001;StartTime=$s} "
               "-MaxEvents 100 -ErrorAction Stop) } catch { "
               "if ($_.FullyQualifiedErrorId -like 'NoMatchingEventsFound*') { $events=@() } else { throw } }; "
               "$events | ForEach-Object { $xml=$_.ToXml(); "
               "if ($xml.IndexOf($p.executable,[StringComparison]::OrdinalIgnoreCase) -ge 0) { $xml } }")
    try:
        result = run(['pwsh.exe', '-NoProfile', '-NonInteractive', '-Command', command],
                     input=json.dumps(dict(started=started, executable=Path(cfg['executable']).name)),
                     capture_output=True, encoding='utf-8', timeout=20, check=True)
        write_new(destination / 'application-events.xml', '<Events>\n' + result.stdout + '\n</Events>')
        records.append(dict(kind='events', status='collected' if result.stdout.strip() else 'zero_events', limit=100))
    except Exception as error:
        records.append(dict(kind='events', error=str(error)))
    roots = [('install-dumps', Path(cfg['executable']).parent, 'install')]
    if os.environ.get('LOCALAPPDATA'):
        local = Path(os.environ['LOCALAPPDATA'])
        roots += [('local-dumps', local / 'CrashDumps', 'dumps'),
                  ('local-wer-queue', local / 'Microsoft/Windows/WER/ReportQueue', 'wer'),
                  ('local-wer-archive', local / 'Microsoft/Windows/WER/ReportArchive', 'wer')]
    if os.environ.get('PROGRAMDATA'):
        shared = Path(os.environ['PROGRAMDATA'])
        roots += [('shared-wer-queue', shared / 'Microsoft/Windows/WER/ReportQueue', 'wer'),
                  ('shared-wer-archive', shared / 'Microsoft/Windows/WER/ReportArchive', 'wer'),
                  ('streamline', shared / 'NVIDIA/Streamline' / Path(cfg['executable']).stem, 'streamline')]
    stem = Path(cfg['executable']).stem.lower()
    for label, root, kind in roots:
        try:
            if kind in ('install', 'dumps'):
                candidates = (p for p in root.glob('*.dmp') if p.name.lower().startswith(stem + '.') or p.name.lower().startswith(stem + '_'))
            else:
                entries = (p for p in root.iterdir() if p.is_dir() and (kind == 'streamline' or p.name.lower().startswith('appcrash_' + stem + '.exe_')))
                def files():
                    for entry_index, entry in enumerate(islice(entries, SCAN_LIMIT + 1)):
                        if entry_index == SCAN_LIMIT:
                            records.append(dict(source=str(root), status='skipped', reason='report_enumeration_limit'))
                            break
                        for item_index, item in enumerate(islice(entry.glob('*.dmp' if kind == 'streamline' else '*'), SCAN_LIMIT + 1)):
                            if item_index == SCAN_LIMIT:
                                records.append(dict(source=str(entry), status='skipped', reason='file_enumeration_limit'))
                                break
                            if item.is_file():
                                yield item
                candidates = files() if root.is_dir() else ()
            def current_candidates():
                for index, item in enumerate(islice(candidates, SCAN_LIMIT + 1)):
                    if index == SCAN_LIMIT:
                        records.append(dict(source=str(root), status='skipped', reason='scan_limit'))
                        break
                    try:
                        modified = item.stat().st_mtime
                        if start_time <= modified <= end_time:
                            yield item
                        else:
                            records.append(dict(source=str(item), status='skipped', reason='outside_attempt_time'))
                    except OSError as error:
                        records.append(dict(source=str(item), error=str(error)))
            found = list(islice(current_candidates(), MAX_FILES + 1))
            if len(found) > MAX_FILES:
                records.append(dict(source=str(root), status='skipped', reason='enumeration_limit'))
            for item in found[:MAX_FILES]:
                try:
                    relative = item.relative_to(root)
                    records.append(budget.copy(item, destination / label / relative))
                except Exception as error:
                    records.append(dict(source=str(item), error=str(error)))
        except Exception as error:
            records.append(dict(source=str(root), error=str(error)))
    write_new(destination / 'manifest.json', json.dumps(records, indent=2))
    return records
