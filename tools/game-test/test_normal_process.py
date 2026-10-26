"""Real synthetic filesystem/process tests; no game, UI, or user clipboard."""
import ctypes
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
import unittest
from unittest import mock
import uuid

import normal_process as normal


@unittest.skipUnless(os.name == 'nt', 'Windows shell parent required')
class NormalProcessTests(unittest.TestCase):
    def setUp(self):
        self.scratch = normal.JOBS / uuid.uuid4().hex
        self.scratch.mkdir(parents=True)
        self.addCleanup(shutil.rmtree, self.scratch)
        self.helper = str(Path(normal.__file__).resolve())

    def invoke(self, *args, cwd=None):
        return subprocess.run([sys.executable, '-B', self.helper, *args], cwd=cwd or normal.ROOT,
                              capture_output=True, encoding='utf-8', timeout=30)

    def test_probe_uses_real_unique_file_and_cleans_it(self):
        result = normal.probe()
        self.assertIn('redirected', result)
        self.assertFalse(Path(result['expected']).exists())
        self.assertEqual(result['redirected'], os.path.normcase(result['actual']) != os.path.normcase(result['expected']))
        cli = self.invoke('--probe')
        receipt = json.loads(cli.stdout)
        self.assertEqual(cli.returncode, 2 if receipt['redirected'] else 0)
        self.assertFalse(Path(receipt['expected']).exists())

    def test_redirected_parent_to_normal_shell_worker_and_descendant(self):
        parent = normal.probe()
        if not parent['redirected']:
            self.skipTest('Current process already has the normal filesystem view.')
        code = ("import json,os,sys; sys.path.insert(0," + repr(str(Path(self.helper).parent)) + "); "
                "import normal_process; print(json.dumps(dict(context=normal_process.require_normal_context(), "
                "marker=os.environ.get(normal_process.MARKER), argv=sys.argv[1:], cwd=os.getcwd()),ensure_ascii=False))")
        arguments = ['中文 空格', 'quote" and trailing\\', "literal ' $(never-execute)"]
        result = self.invoke('--run', sys.executable, '-B', '-X', 'utf8', '-c', code, *arguments, cwd=self.scratch)
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = json.loads(result.stdout)
        self.assertFalse(receipt['context']['redirected'])
        self.assertEqual(receipt['marker'], '1')
        self.assertEqual(receipt['argv'], arguments)
        self.assertEqual(Path(receipt['cwd']), self.scratch)
        self.assertFalse(Path(receipt['context']['expected']).exists())

    def test_run_forwards_exit_seven_and_both_logs(self):
        code = "import sys; print('中文 stdout'); print('中文 stderr',file=sys.stderr); sys.exit(7)"
        result = self.invoke('--run', sys.executable, '-B', '-c', code)
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(result.stdout, '中文 stdout\n')
        self.assertEqual(result.stderr, '中文 stderr\n')

    def test_direct_worker_refuses_redirected_context_even_with_marker(self):
        if not normal.probe()['redirected']:
            self.skipTest('Requires the redirected parent view.')
        sentinel = self.scratch / 'must-not-run.txt'
        job = self.scratch / 'job.json'
        code = 'from pathlib import Path; Path(' + repr(str(sentinel)) + ").write_text('unexpected')"
        job.write_text(json.dumps(dict(argv=[sys.executable, '-c', code], cwd=str(self.scratch))), encoding='utf-8')
        environment = os.environ.copy()
        environment[normal.MARKER] = '1'
        child = subprocess.run([sys.executable, '-B', self.helper, '--worker', str(job)],
                               env=environment, capture_output=True, encoding='utf-8', timeout=15)
        self.assertEqual(child.returncode, 1)
        self.assertFalse(sentinel.exists())
        result = json.loads((self.scratch / 'result.json').read_text(encoding='utf-8'))
        self.assertIn('normal-context protected entry point', result['error'])
        self.assertEqual(result['exitcode'], 1)

    def test_streams_capture_ready_before_command_can_exit_and_decodes_split_utf8(self):
        acknowledgment = self.scratch / 'ack.txt'
        code = ("import sys,time; from pathlib import Path; "
                "sys.stdout.buffer.write(b'CAPTURE_READY='); sys.stdout.buffer.flush(); "
                "data='中文'.encode('utf-8'); sys.stdout.buffer.write(data[:1]); sys.stdout.buffer.flush(); "
                "time.sleep(.3); sys.stdout.buffer.write(data[1:]+b'\\n'); sys.stdout.buffer.flush(); "
                "ack=Path(" + repr(str(acknowledgment)) + "); deadline=time.monotonic()+8; "
                "exec('while not ack.exists() and time.monotonic()<deadline: time.sleep(.05)'); "
                "sys.stdout.buffer.write('尾部😀'.encode('utf-8')); sys.stdout.buffer.flush(); "
                "sys.exit(0 if ack.exists() else 9)")
        child = subprocess.Popen([sys.executable, '-B', self.helper, '--run', sys.executable, '-B', '-c', code],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding='utf-8')
        try:
            line = child.stdout.readline()
            self.assertEqual(line, 'CAPTURE_READY=中文\n')
            self.assertIsNone(child.poll(), 'Capture-ready must be forwarded while command is still alive')
            acknowledgment.write_text('synthetic acknowledgment', encoding='utf-8')
            stdout, stderr = child.communicate(timeout=20)
            self.assertEqual(child.returncode, 0, stderr)
            self.assertEqual(stdout, '尾部😀')
        finally:
            acknowledgment.touch(exist_ok=True)
            child.communicate(timeout=20)
            child.stdout.close()
            child.stderr.close()

    def test_entry_quotes_paths_and_splats_json_without_execution(self):
        entry = self.scratch / "入口 ' with space.ps1"
        parameters = self.scratch / "参数 ' input.json"
        (self.scratch / 'relative.txt').write_text('relative-data', encoding='utf-8')
        entry.write_text("param([string]$Text,[switch]$Enabled,[string]$Relative)\n"
                         "[ordered]@{text=$Text;enabled=[bool]$Enabled;"
                         "relative=(Get-Content -LiteralPath $Relative -Raw);"
                         "cwd=(Get-Location).Path;marker=$env:NR_NEXUS_NORMAL_USER_CONTEXT} | ConvertTo-Json -Compress\n",
                         encoding='utf-8')
        payload = "中文 ' ; $(throw 'must remain data')"
        parameters.write_text(json.dumps(dict(Text=payload, Enabled=True, Relative='relative.txt'), ensure_ascii=False), encoding='utf-8')
        result = self.invoke('--entry', str(entry), '--parameters', str(parameters), cwd=self.scratch)
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = json.loads(result.stdout)
        self.assertEqual(receipt['text'], payload)
        self.assertTrue(receipt['enabled'])
        self.assertEqual(receipt['relative'], 'relative-data')
        self.assertEqual(Path(receipt['cwd']), self.scratch)
        self.assertEqual(receipt['marker'], '1')
        parameters.write_text(json.dumps(dict(Text=payload, Enabled=False, Relative='relative.txt'), ensure_ascii=False), encoding='utf-8')
        with entry.open('a', encoding='utf-8') as stream:
            stream.write('\nexit 7\n')
        result = self.invoke('--entry', str(entry), '--parameters', str(parameters), cwd=self.scratch)
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertFalse(json.loads(result.stdout)['enabled'])

    def test_entry_streams_out_host_marker_before_external_signal(self):
        entry = self.scratch / '实时入口.ps1'
        parameters = self.scratch / 'parameters.json'
        acknowledgment = self.scratch / 'continue.signal'
        entry.write_text("param([string]$Signal,[switch]$Enabled,[string]$Text)\n"
                         "'CAPTURE_READY=' + $Text | Out-Host\n"
                         "$deadline=[DateTime]::UtcNow.AddSeconds(8)\n"
                         "while (!(Test-Path -LiteralPath $Signal) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50 }\n"
                         "if (!(Test-Path -LiteralPath $Signal)) { exit 9 }\n"
                         "if (!$Enabled) { exit 8 }\n"
                         "'结束中文' | Out-Host\n", encoding='utf-8')
        parameters.write_text(json.dumps(dict(Signal=str(acknowledgment), Enabled=True, Text='中文 空格'), ensure_ascii=False), encoding='utf-8')
        child = subprocess.Popen([sys.executable, '-B', self.helper, '--entry', str(entry), '--parameters', str(parameters)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding='utf-8')
        try:
            self.assertEqual(child.stdout.readline(), 'CAPTURE_READY=中文 空格\n')
            self.assertIsNone(child.poll())
            acknowledgment.write_text('synthetic signal', encoding='utf-8')
            stdout, stderr = child.communicate(timeout=20)
            self.assertEqual(child.returncode, 0, stderr)
            self.assertEqual(stdout, '结束中文\n')
        finally:
            acknowledgment.touch(exist_ok=True)
            child.communicate(timeout=20)
            child.stdout.close()
            child.stderr.close()

    def test_creation_failure_releases_parent_and_attributes_without_fallback(self):
        def fail(*args):
            ctypes.set_last_error(5)
            return 0
        with mock.patch.object(normal.kernel, 'CreateProcessW', side_effect=fail) as create, \
                mock.patch.object(normal.kernel, 'CloseHandle', wraps=normal.kernel.CloseHandle) as close, \
                mock.patch.object(normal.kernel, 'DeleteProcThreadAttributeList', wraps=normal.kernel.DeleteProcThreadAttributeList) as delete:
            with self.assertRaises(OSError):
                normal.launch([sys.executable, '-c', 'pass'])
            self.assertEqual(create.call_count, 1)
            self.assertEqual(close.call_count, 1)
            self.assertEqual(delete.call_count, 1)

    def test_launch_failure_restores_ctrl_c_handler(self):
        before = signal.getsignal(signal.SIGINT)
        observed = []
        def fail(*args, **kwargs):
            observed.append(signal.getsignal(signal.SIGINT))
            raise OSError('synthetic launch failure')
        with mock.patch.object(normal, 'launch', side_effect=fail):
            with self.assertRaisesRegex(OSError, 'synthetic launch failure'):
                normal.run([sys.executable, '-c', 'pass'])
        self.assertEqual(observed, [signal.SIG_IGN])
        self.assertEqual(signal.getsignal(signal.SIGINT), before)


if __name__ == '__main__':
    unittest.main()
