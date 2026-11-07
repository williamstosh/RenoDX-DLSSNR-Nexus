"""Explicit migration of completed legacy game runs to the shared output lifecycle."""

import argparse
import json
from pathlib import Path

import runner


def adopt_completed(root):
    root = runner.local_output_root(root)
    lifecycle = runner.output_lifecycle
    report = dict(adopted=[], skipped=[], errors=[])
    if lifecycle.os.environ.get('NR_NEXUS_TEST_KEEP_RAW') == '1':
        return report
    for complete_path in root.rglob('guard-complete.json'):
        directory = complete_path.parent
        if (directory / 'artifact-lifecycle.json').exists():
            continue
        try:
            lifecycle._relative_file(root, complete_path)
            ancestors = [parent for parent in directory.parents if parent.is_relative_to(runner.ROOT)]
            if any((parent / 'artifact-lifecycle.json').is_file() and
                   (lifecycle._read(parent).get('keepRawArtifacts') or
                    lifecycle._owner_active(int(lifecycle._read(parent).get('ownerPid', 0))))
                   for parent in ancestors):
                report['skipped'].append(str(directory))
                continue
            complete = json.loads(complete_path.read_text(encoding='utf-8-sig'))
            final = json.loads((directory / 'result.json').read_text(encoding='utf-8-sig'))
            if (complete.get('verified') is not True or complete.get('owned_process_exited') is not True
                    or final.get('guard') != 'verified_original_hashes_and_released'
                    or not final.get('finished')):
                report['skipped'].append(str(directory))
                continue
            # Historical manifests can refer to another checkout. Never follow them.
            entries = json.loads((directory / 'protected-files.json').read_text(encoding='utf-8-sig'))
            for entry in entries:
                lifecycle._relative_file(directory, Path(entry['backup_path']))
            state = dict(schema=lifecycle.SCHEMA, runId=lifecycle.os.urandom(16).hex(),
                ownerPid=lifecycle.os.getpid(), mode='Standard', status='Running',
                startedUtc=final.get('started', final['finished']), keepRawArtifacts=False,
                budgetBytes=lifecycle.budget_bytes(), disposables=[], retainedPaths=[],
                cleanup=[], cleanupErrors=[], adoptedCompletedGameRun=True)
            lifecycle._write(directory, state)
            runner.finish_artifacts(directory, final, True)
            report['adopted'].append(str(directory))
        except (OSError, ValueError, KeyError) as error:
            report['errors'].append(dict(directory=str(directory), error=str(error)))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    with runner.runner_lock():
        if runner.process_inventory('witcher3.exe') or runner.process_inventory('CrashReporter.exe'):
            raise RuntimeError('Game or crash reporter is still running; preserve recovery files')
        report = adopt_completed(args.root)
    receipt = runner.ROOT / '.local/game-artifact-cleanup.json'
    receipt.write_text(json.dumps(report, ensure_ascii=False, indent=2) + chr(10), encoding='utf-8')
    print(json.dumps(dict(adopted=len(report['adopted']), skipped=len(report['skipped']),
                         errors=report['errors'], receipt=str(receipt))))
    return 1 if report['errors'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
