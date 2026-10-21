#!/usr/bin/env python3
"""Persistent, editor-independent gameplay jobs. Uses only the Python standard library."""

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading
import time
import uuid

from toolchain_environment import child_environment

TERMINAL = {'passed', 'failed', 'cancelled', 'skipped'}
EXIT_CODES = {'passed': 0, 'failed': 1, 'cancelled': 130, 'skipped': 3}
OPERATIONS = ('create', 'inspect', 'build', 'run', 'relaunch', 'test', 'publish')
REPO = Path(__file__).resolve().parents[2]
CANCEL_GRACE_SECONDS = 2


def project_tool(repo=None):
    """Resolve lazily so a tool built after MCP startup is immediately available."""
    if os.environ.get('AETHER_PROJECT_TOOL'):
        return Path(os.environ['AETHER_PROJECT_TOOL']).resolve()
    repo = Path(repo or os.environ.get('AETHER_REPO', REPO)).resolve()
    bases = [Path(os.environ['AETHER_BUILD_DIR'])] if os.environ.get('AETHER_BUILD_DIR') else []
    bases += [repo / name for name in ('build/default', 'build/vs2022-msvc', 'build/ninja-clang', 'build/linux-clang')]
    exe = 'aether-project.exe' if os.name == 'nt' else 'aether-project'
    candidates = [(base if base.is_absolute() else repo / base) / 'src/app' / config / exe
                  for base in bases for config in ('RelWithDebInfo', 'Debug', 'Release', '')]
    existing = [path for path in candidates if path.is_file()]
    return max(existing, key=lambda path: path.stat().st_mtime) if existing else candidates[0]


def _atomic_json(path, value):
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _job_dir(job_id, repo=None):
    if not isinstance(job_id, str) or str(uuid.UUID(job_id)) != job_id:
        raise ValueError('jobId must be a canonical UUID')
    return Path(repo or os.environ.get('AETHER_REPO', REPO)).resolve() / 'logs/gameplay-jobs' / job_id


@contextmanager
def _lock(directory):
    with (directory / 'state.lock').open('a+b') as stream:
        if os.name == 'nt':
            import msvcrt
            stream.seek(0, 2)
            if stream.tell() == 0:
                stream.write(b'0')
                stream.flush()
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            if os.name == 'nt':
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def _report(request, status, phase, code=None, message=None):
    return dict(schemaVersion=1, jobId=request['jobId'], status=status, phase=phase,
                target=request.get('target', 'desktop'), language=(request.get('language') if isinstance(request.get('language'), str) and request['language'].strip() else 'unknown'),
                profile='default', diagnostics=([dict(code=code, severity='error', message=message)] if code else []),
                toolchains=[], artifacts=[], requiredChecks=[], durationMs=0, exitCode=EXIT_CODES.get(status))


def _validate(value, schema, root):
    """Validate the checked-in report schema's vocabulary, including nested records."""
    if '$ref' in schema:
        referenced = root
        for part in schema['$ref'].split('/')[1:]:
            referenced = referenced[part]
        _validate(value, referenced, root)
    def matches(candidate):
        try:
            _validate(value, candidate, root)
            return True
        except ValueError:
            return False
    for constraint in schema.get('allOf', []):
        _validate(value, constraint, root)
    if 'if' in schema and matches(schema['if']):
        _validate(value, schema.get('then', {}), root)
    if 'not' in schema and matches(schema['not']):
        raise ValueError('forbidden report value')
    types = schema.get('type', [])
    types = [types] if isinstance(types, str) else types
    kinds = {'object': dict, 'array': list, 'string': str, 'integer': int, 'null': type(None)}
    if types and not any(type(value) is kinds[kind] for kind in types):
        raise ValueError('invalid report value type')
    if 'const' in schema and (type(value) is not type(schema['const']) or value != schema['const']):
        raise ValueError('invalid report constant')
    if 'enum' in schema and not any(type(value) is type(item) and value == item for item in schema['enum']):
        raise ValueError('invalid report enum')
    if isinstance(value, dict):
        if not set(schema.get('required', [])).issubset(value):
            raise ValueError('missing report fields')
        properties = schema.get('properties', {})
        if schema.get('additionalProperties') is False and set(value) - set(properties):
            raise ValueError('unexpected report fields')
        for key, item in value.items():
            if key in properties:
                _validate(item, properties[key], root)
        for key, required in schema.get('dependentRequired', {}).items():
            if key in value and not set(required).issubset(value):
                raise ValueError('missing dependent report fields')
    if isinstance(value, list):
        for item in value:
            _validate(item, schema.get('items', {}), root)
        if schema.get('uniqueItems') and len({json.dumps(item, sort_keys=True) for item in value}) != len(value):
            raise ValueError('duplicate report entries')
        if 'contains' in schema:
            if not any(_schema_matches(item, schema['contains'], root) for item in value):
                raise ValueError('required report entry missing')
    if isinstance(value, str):
        if len(value) < schema.get('minLength', 0) or ('pattern' in schema and not re.search(schema['pattern'], value)):
            raise ValueError('empty report text')
    if type(value) is int and not schema.get('minimum', value) <= value <= schema.get('maximum', value):
        raise ValueError('report integer outside range')


def _schema_matches(value, schema, root):
    try:
        _validate(value, schema, root)
        return True
    except ValueError:
        return False


def validate_report(report, job_id, return_code):
    schema = json.loads((REPO / 'docs/schemas/agent-report-v1.schema.json').read_text(encoding='utf-8'))
    _validate(report, schema, schema)
    if report['jobId'] != job_id or report['status'] not in TERMINAL or report['exitCode'] != return_code:
        raise ValueError('worker report identity, terminal status, or process exit code disagrees')
    names = [check['name'] for check in report['requiredChecks']]
    if len(set(names)) != len(names):
        raise ValueError('duplicate required check names')


class _OwnedProcess:
    """Own one process tree; Windows attaches the suspended child before it can run."""
    def __init__(self, command, cwd, output):
        self.process = None
        self.job = None
        environment = child_environment()
        if os.name != 'nt':
            self.process = subprocess.Popen(command, cwd=cwd, stdin=subprocess.DEVNULL,
                                            stdout=output, stderr=subprocess.STDOUT, start_new_session=True, env=environment)
            return
        import ctypes as c
        from ctypes import wintypes as w
        import msvcrt
        import _winapi

        class Basic(c.Structure):
            _fields_ = [('processTime', c.c_int64), ('jobTime', c.c_int64), ('flags', w.DWORD),
                        ('minimum', c.c_size_t), ('maximum', c.c_size_t), ('active', w.DWORD),
                        ('affinity', c.c_size_t), ('priority', w.DWORD), ('scheduling', w.DWORD)]
        class Counters(c.Structure):
            _fields_ = [(name, c.c_uint64) for name in ('reads', 'writes', 'other', 'readBytes', 'writeBytes', 'otherBytes')]
        class Limits(c.Structure):
            _fields_ = [('basic', Basic), ('io', Counters), ('processMemory', c.c_size_t),
                        ('jobMemory', c.c_size_t), ('peakProcess', c.c_size_t), ('peakJob', c.c_size_t)]
        self.kernel = c.WinDLL('kernel32', use_last_error=True)
        self.kernel.CreateJobObjectW.argtypes = [c.c_void_p, w.LPCWSTR]
        self.kernel.CreateJobObjectW.restype = w.HANDLE
        self.kernel.SetInformationJobObject.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD]
        self.kernel.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
        self.kernel.TerminateJobObject.argtypes = [w.HANDLE, w.UINT]
        self.kernel.CloseHandle.argtypes = [w.HANDLE]
        self.kernel.ResumeThread.argtypes = [w.HANDLE]
        self.kernel.ResumeThread.restype = w.DWORD
        self.job = self.kernel.CreateJobObjectW(None, None)
        limits = Limits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.job or not self.kernel.SetInformationJobObject(self.job, 9, c.byref(limits), c.sizeof(limits)):
            self.close()
            raise c.WinError(c.get_last_error())
        process = thread = None
        try:
            with open(os.devnull, 'rb') as null:
                out_handle = msvcrt.get_osfhandle(output.fileno())
                in_handle = msvcrt.get_osfhandle(null.fileno())
                os.set_handle_inheritable(out_handle, True)
                os.set_handle_inheritable(in_handle, True)
                startup = subprocess.STARTUPINFO()
                startup.dwFlags = subprocess.STARTF_USESTDHANDLES
                startup.hStdInput, startup.hStdOutput, startup.hStdError = in_handle, out_handle, out_handle
                try:
                    process, thread, _, _ = _winapi.CreateProcess(command[0], subprocess.list2cmdline(command),
                        None, None, True, subprocess.CREATE_NO_WINDOW | 4, environment, str(cwd), startup)
                finally:
                    os.set_handle_inheritable(out_handle, False)
                    os.set_handle_inheritable(in_handle, False)
            self.process = process
            if not self.kernel.AssignProcessToJobObject(self.job, process):
                raise c.WinError(c.get_last_error())
            if self.kernel.ResumeThread(thread) == 0xffffffff:
                raise OSError('could not resume gameplay worker')
        except BaseException:
            if process:
                _winapi.TerminateProcess(process, 1)
                _winapi.WaitForSingleObject(process, _winapi.INFINITE)
            self.close()
            raise
        finally:
            if thread:
                _winapi.CloseHandle(thread)

    def poll(self):
        if os.name != 'nt':
            return self.process.poll()
        import _winapi
        return _winapi.GetExitCodeProcess(self.process) if _winapi.WaitForSingleObject(self.process, 0) == 0 else None

    def kill(self):
        if os.name == 'nt':
            if self.job:
                self.kernel.TerminateJobObject(self.job, 1)
        elif self.process:
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    def close(self):
        if os.name == 'nt':
            if self.job:
                self.kernel.CloseHandle(self.job)
                self.job = None
            if self.process:
                import _winapi
                _winapi.WaitForSingleObject(self.process, _winapi.INFINITE)
                _winapi.CloseHandle(self.process)
                self.process = None
        elif self.process:
            self.kill()
            self.process.wait()
            self.process = None


def _snapshot(job_id, directory):
    report = json.loads((directory / 'report.json').read_text(encoding='utf-8'))
    return dict(jobId=job_id, report=report, reportPath=str(directory / 'report.json'),
                cancellationRequested=(directory / 'cancel').exists())


def status_job(job_id, *, repo=None):
    directory = _job_dir(job_id, repo)
    with _lock(directory):
        return _snapshot(job_id, directory)


def cancel_job(job_id, *, repo=None):
    directory = _job_dir(job_id, repo)
    with _lock(directory):
        result = _snapshot(job_id, directory)
        if result['report']['status'] not in TERMINAL:
            (directory / 'cancel').touch()
        return _snapshot(job_id, directory)


def start_job(request, *, repo=None, _worker_command=None):
    if not isinstance(request, dict) or not isinstance(request.get('project'), str) or not request['project'].strip():
        raise ValueError('project must be a nonempty path')
    if request.get('operation') not in OPERATIONS or request.get('target', 'desktop') not in ('desktop', 'web'):
        raise ValueError('operation or target is unsupported')
    timeout = request.get('timeoutMs', 600000 if request.get('target') == 'web' else 180000)
    if type(timeout) is not int or not 100 <= timeout <= 600000:
        raise ValueError('timeoutMs must be an integer from 100 through 600000')
    repo = Path(repo or os.environ.get('AETHER_REPO', REPO)).resolve()
    request = dict(request, jobId=str(uuid.uuid4()), target=request.get('target', 'desktop'), timeoutMs=timeout)
    if not Path(request['project']).is_absolute():
        request['project'] = str((repo / request['project']).resolve())
    directory = _job_dir(request['jobId'], repo)
    directory.mkdir(parents=True)
    tool = project_tool(repo)
    command = list(_worker_command) if _worker_command is not None else [str(tool)]
    cwd = next((parent for parent in tool.parents if (parent / 'CMakeCache.txt').exists()), tool.parent)
    if _worker_command is not None:
        cwd = repo
    _atomic_json(directory / 'request.json', request)
    _atomic_json(directory / 'worker.json', dict(command=command, cwd=str(cwd), createdAt=time.time()))
    _atomic_json(directory / 'report.json', _report(request, 'queued', 'queued'))
    queued = status_job(request['jobId'], repo=repo)
    args = [sys.executable, str(Path(__file__).resolve()), '--repo', str(repo), '_supervise', request['jobId']]
    options = dict(cwd=str(repo), stdin=subprocess.DEVNULL)
    if os.name == 'nt':
        options['creationflags'] = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
    else:
        options['start_new_session'] = True
    try:
        with (directory / 'supervisor.log').open('ab') as output:
            child = subprocess.Popen(args, stdout=output, stderr=subprocess.STDOUT, **options)
        # Do not wait: the detached supervisor owns its worker and all cleanup.
        threading.Thread(target=child.wait, daemon=True).start()
        return queued
    except OSError as exc:
        _atomic_json(directory / 'report.json', _report(request, 'failed', 'launch', 'gameplay.supervisor_launch', str(exc)))
        return status_job(request['jobId'], repo=repo)


def _supervise(job_id, repo):
    directory = _job_dir(job_id, repo)
    request = json.loads((directory / 'request.json').read_text(encoding='utf-8'))
    metadata = json.loads((directory / 'worker.json').read_text(encoding='utf-8'))
    started = time.monotonic()
    worker = None
    reason = None
    report = None
    try:
        with _lock(directory):
            _atomic_json(directory / 'report.json', _report(request, 'running', request['operation']))
        if (directory / 'cancel').exists():
            reason = 'cancelled'
        else:
            command = metadata['command'] + ['--request', str(directory / 'request.json'),
                '--report', str(directory / 'worker-report.json'), '--cancel-file', str(directory / 'cancel')]
            with (directory / 'worker.log').open('ab') as output:
                worker = _OwnedProcess(command, metadata['cwd'], output)
                deadline = started + max(0, request['timeoutMs'] / 1000 - (time.time() - metadata['createdAt']))
                while worker.poll() is None:
                    if (directory / 'cancel').exists():
                        reason = 'cancelled'
                        break
                    if time.monotonic() >= deadline:
                        reason = 'timeout'
                        # The native worker shares the same cooperative cancellation path.
                        (directory / 'cancel').touch()
                        break
                    time.sleep(.02)
                if reason is None and time.monotonic() >= deadline:
                    reason = 'timeout'
                    (directory / 'cancel').touch()
                if reason:
                    grace = time.monotonic() + CANCEL_GRACE_SECONDS
                    while worker.poll() is None and time.monotonic() < grace:
                        time.sleep(.02)
                    if worker.poll() is None:
                        worker.kill()
                        while worker.poll() is None:
                            time.sleep(.01)
                code = worker.poll()
                try:
                    report = json.loads((directory / 'worker-report.json').read_text(encoding='utf-8'))
                    validate_report(report, job_id, code)
                except (OSError, ValueError, KeyError, TypeError) as exc:
                    report = _report(request, 'failed', 'worker', 'gameplay.worker_failed', f'Worker exited {code}; invalid or missing terminal report: {exc}')
    except Exception as exc:
        report = _report(request, 'failed', 'supervisor', 'gameplay.supervisor_failed', str(exc))
    finally:
        if worker:
            worker.close()
        with _lock(directory):
            # Cancel and final publication use the same lock. Once cancellation wins,
            # an otherwise valid late worker pass can never become the public result.
            if reason != 'timeout' and (directory / 'cancel').exists():
                report = _report(request, 'cancelled', 'cancelled')
            elif reason == 'timeout':
                report = _report(request, 'failed', 'timeout', 'gameplay.timeout', 'Gameplay job exceeded timeoutMs')
            if report is None:
                report = _report(request, 'failed', 'supervisor', 'gameplay.missing_report', 'No terminal report was produced')
            report['durationMs'] = max(report['durationMs'], int((time.monotonic() - started) * 1000))
            _atomic_json(directory / 'report.json', report)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', default=os.environ.get('AETHER_REPO', str(REPO)))
    commands = parser.add_subparsers(dest='action', required=True)
    start = commands.add_parser('start')
    start.add_argument('--request', required=True)
    start.add_argument('--wait', action='store_true')
    start.add_argument('--report')
    for action in ('status', 'cancel', '_supervise'):
        sub = commands.add_parser(action)
        sub.add_argument('job_id')
        if action != '_supervise':
            sub.add_argument('--report')
    args = parser.parse_args(argv)
    try:
        if args.action == '_supervise':
            _supervise(args.job_id, Path(args.repo))
            return 0
        if args.action == 'start':
            request = json.loads(Path(args.request).read_text(encoding='utf-8-sig'))
            result = start_job(request, repo=args.repo)
            if args.wait:
                while result['report']['status'] not in TERMINAL:
                    time.sleep(.05)
                    result = status_job(result['jobId'], repo=args.repo)
        elif args.action == 'cancel':
            result = cancel_job(args.job_id, repo=args.repo)
        else:
            result = status_job(args.job_id, repo=args.repo)
        if args.report:
            destination = Path(args.report).resolve()
            destination.parent.mkdir(parents=True, exist_ok=True)
            # A status export must never replace a newer live snapshot in place.
            if destination != Path(result['reportPath']).resolve():
                _atomic_json(destination, result['report'])
        print(json.dumps(result, ensure_ascii=False))
        return result['report']['exitCode'] or 0
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(json.dumps({'error': str(exc)}, ensure_ascii=False))
        return 1


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.exit(main())
