"""Persistent gameplay job behavior with real, isolated worker processes."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

import gameplay_jobs as jobs


WORKER = r'''
import argparse, json, pathlib, subprocess, sys, time
p = argparse.ArgumentParser()
p.add_argument('--request'); p.add_argument('--report'); p.add_argument('--cancel-file')
a = p.parse_args()
r = json.loads(pathlib.Path(a.request).read_text(encoding='utf-8'))
mode = r.get('name', 'passed')
pathlib.Path(a.report).with_name('started.json').write_text(json.dumps(r), encoding='utf-8')
if mode == 'crash': sys.exit(17)
if mode == 'hang': time.sleep(30)
if mode == 'tree':
    child = "import pathlib,sys,time; p=pathlib.Path(sys.argv[1]); (p/'descendant-started').touch(); time.sleep(3); (p/'descendant-survived').touch()"
    subprocess.Popen([sys.executable, '-c', child, str(pathlib.Path(a.report).parent)])
    time.sleep(30)
if mode in ('cancel', 'cancel-evidence', 'late-pass'):
    while not pathlib.Path(a.cancel_file).exists(): time.sleep(.01)
status = 'cancelled' if mode in ('cancel', 'cancel-evidence') else ('failed' if mode == 'failed' else 'passed')
code = {'passed': 0, 'failed': 1, 'cancelled': 130}[status]
out = dict(schemaVersion=1, jobId=r['jobId'], status=status, phase='complete', target=r.get('target','desktop'),
           language='csharp', profile='default', diagnostics=[], toolchains=[], artifacts=[],
           requiredChecks=[dict(name='worker', status='passed')], durationMs=1, exitCode=code)
if status == 'failed': out['diagnostics'] = [dict(code='fixture.failure',severity='error',message='fixture failed')]
if mode in ('cancel-evidence', 'late-pass'):
    evidence = pathlib.Path(a.report).parent / 'partial-summary.json'
    evidence.write_text('{}', encoding='utf-8')
    out['artifacts'] = [dict(path=str(evidence), bytes=evidence.stat().st_size)]
if mode == 'bad-check': out['requiredChecks'][0]['status'] = 'failed'
if mode == 'wrong-id': out['jobId'] = 'wrong'
pathlib.Path(a.report).write_text(json.dumps(out), encoding='utf-8')
sys.exit(9 if mode == 'contradiction' else code)
'''


class GameplayJobTests(unittest.TestCase):
    def test_cancel_preserves_valid_worker_partial_evidence(self):
        job_id = self.start('cancel-evidence')['jobId']
        self.started(job_id)
        jobs.cancel_job(job_id, repo=self.repo)
        result = self.wait(job_id)['report']
        self.assertEqual(result['status'], 'cancelled')
        self.assertEqual(len(result['artifacts']), 1)
        self.assertTrue(Path(result['artifacts'][0]['path']).is_file())

    def test_timeout_keeps_worker_evidence_but_cannot_become_late_success(self):
        result = self.wait(self.start('late-pass', timeout=500)['jobId'])['report']
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(result['exitCode'], 1)
        self.assertTrue(any(item['code'] == 'gameplay.timeout' for item in result['diagnostics']))
        self.assertEqual(len(result['artifacts']), 1)

    def test_owned_child_receives_repaired_toolchain_environment_without_mutating_parent(self):
        environment = dict(os.environ)
        environment.pop('RUSTUP_HOME', None)
        repaired = dict(environment, RUSTUP_HOME=str(self.repo / 'Rust home & \u4e16\u754c'))
        output = self.repo / 'environment.json'
        with mock.patch.dict(os.environ, environment, clear=True), \
                mock.patch.object(jobs, 'child_environment', return_value=repaired, create=True), output.open('wb') as log:
            child = jobs._OwnedProcess([sys.executable, '-c', 'import json,os; print(json.dumps(os.environ.get("RUSTUP_HOME")))'], self.repo, log)
            try:
                deadline = time.monotonic() + 5
                while child.poll() is None and time.monotonic() < deadline:
                    time.sleep(.01)
                self.assertEqual(child.poll(), 0)
            finally:
                child.close()
            self.assertNotIn('RUSTUP_HOME', os.environ)
        self.assertEqual(json.loads(output.read_text(encoding='utf-8')), repaired['RUSTUP_HOME'])

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='gameplay jobs 世界 & ')
        self.addCleanup(self.cleanup)
        self.repo = Path(self.temp.name)
        self.worker = self.repo / 'worker & literal.py'
        self.worker.write_text(WORKER, encoding='utf-8')

    def cleanup(self):
        # Terminal publication precedes the detached supervisor's final stdio close.
        for attempt in range(100):
            try:
                self.temp.cleanup()
                return
            except PermissionError:
                if attempt == 99:
                    raise
                time.sleep(.02)

    def test_web_target_survives_start_and_status(self):
        started=jobs.start_job({'project':str(self.repo/'project'), 'operation':'publish', 'target':'web'},repo=self.repo,_worker_command=[sys.executable,str(self.worker)])
        self.assertEqual(started['report']['target'],'web')
        terminal=self.wait(started['jobId'])
        self.assertEqual(terminal['report']['target'],'web')
        self.assertEqual(terminal['report']['status'],'passed')

    def start(self, mode='passed', timeout=5000):
        return jobs.start_job({'project': str(self.repo / 'project & 世界'), 'operation': 'inspect',
                               'name': mode, 'timeoutMs': timeout}, repo=self.repo,
                              _worker_command=[sys.executable, str(self.worker)])

    def wait(self, job_id):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            result = jobs.status_job(job_id, repo=self.repo)
            if result['report']['status'] in jobs.TERMINAL:
                return result
            time.sleep(.02)
        directory = self.repo / 'logs/gameplay-jobs' / job_id
        logs = '\n'.join(path.read_text(encoding='utf-8') for path in directory.glob('*.log'))
        self.fail(f'supervisor did not publish a terminal report: {result}; {logs}')

    def started(self, job_id):
        path = self.repo / 'logs/gameplay-jobs' / job_id / 'started.json'
        deadline = time.monotonic() + 5
        while not path.exists() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertTrue(path.exists())
        return json.loads(path.read_text(encoding='utf-8'))

    def test_start_returns_queued_and_separate_cli_reads_persisted_pass(self):
        start = self.start()
        self.assertEqual(start['report']['status'], 'queued')
        self.assertIsNone(start['report']['exitCode'])
        self.assertEqual(self.wait(start['jobId'])['report']['status'], 'passed')
        process = subprocess.run([sys.executable, str(Path(jobs.__file__)), '--repo', str(self.repo),
                                  'status', start['jobId']], capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(json.loads(process.stdout)['report']['status'], 'passed')
        self.assertEqual(self.started(start['jobId'])['project'], str(self.repo / 'project & 世界'))

    def test_relative_project_paths_are_repo_absolute_for_inspect_and_create(self):
        for operation in ('inspect', 'create'):
            with self.subTest(operation=operation):
                result = jobs.start_job({'project': 'projects/new & 世界', 'operation': operation},
                                        repo=self.repo, _worker_command=[sys.executable, str(self.worker)])
                self.assertEqual(self.wait(result['jobId'])['report']['status'], 'passed')
                directory = self.repo / 'logs/gameplay-jobs' / result['jobId']
                persisted = json.loads((directory / 'request.json').read_text(encoding='utf-8'))
                expected = str(self.repo.resolve() / 'projects' / 'new & 世界')
                self.assertEqual(persisted['project'], expected)
                self.assertEqual(self.started(result['jobId'])['project'], expected)

    def test_worker_failure_and_crash_cannot_be_success(self):
        for mode in ('failed', 'crash', 'contradiction', 'bad-check', 'wrong-id'):
            with self.subTest(mode=mode):
                final = self.wait(self.start(mode)['jobId'])['report']
                self.assertEqual(final['status'], 'failed')
                self.assertEqual(final['exitCode'], 1)
                self.assertTrue(any(d['severity'] == 'error' for d in final['diagnostics']))

    def test_cancel_is_persistent_and_preserves_a_terminal_cancel(self):
        job_id = self.start('cancel')['jobId']
        self.started(job_id)
        jobs.cancel_job(job_id, repo=self.repo)
        self.assertEqual(self.wait(job_id)['report']['status'], 'cancelled')
        self.assertEqual(jobs.cancel_job(job_id, repo=self.repo)['report']['status'], 'cancelled')

    def test_uncooperative_worker_is_killed_on_timeout(self):
        report = self.wait(self.start('hang', timeout=150)['jobId'])['report']
        self.assertEqual(report['status'], 'failed')
        self.assertTrue(any(d['code'] == 'gameplay.timeout' for d in report['diagnostics']))

    def test_cancellation_cannot_be_overwritten_by_late_worker_success(self):
        job_id = self.start('late-pass')['jobId']
        self.started(job_id)
        jobs.cancel_job(job_id, repo=self.repo)
        self.assertEqual(self.wait(job_id)['report']['status'], 'cancelled')

    def test_forced_cancellation_kills_the_owned_descendant_tree(self):
        job_id = self.start('tree')['jobId']
        directory = self.repo / 'logs/gameplay-jobs' / job_id
        deadline = time.monotonic() + 5
        while not (directory / 'descendant-started').exists() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertTrue((directory / 'descendant-started').exists())
        jobs.cancel_job(job_id, repo=self.repo)
        self.assertEqual(self.wait(job_id)['report']['status'], 'cancelled')
        time.sleep(1.2)
        self.assertFalse((directory / 'descendant-survived').exists())

    def test_job_ids_cannot_escape_the_job_directory(self):
        for job_id in ('../elsewhere', 'not-a-uuid', '', '00000000-0000-0000-0000-000000000000/../'):
            with self.subTest(job_id=job_id), self.assertRaises(ValueError):
                jobs.status_job(job_id, repo=self.repo)


if __name__ == '__main__':
    unittest.main()
