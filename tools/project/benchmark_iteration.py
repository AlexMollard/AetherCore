"""Measure the Rust/C# reference CLI build, test and fresh relaunch paths; never publish or open an editor."""
import argparse
import json
import os
from pathlib import Path
import platform
import statistics
import time
import uuid

from gameplay_jobs import _OwnedProcess, project_tool, validate_report
from reference_fixture import ROOT, config


def run(args):
    destination = Path(args.report).resolve()
    folder = destination.parent / ('iteration-' + uuid.uuid4().hex)
    folder.mkdir(parents=True)
    deadline = time.monotonic() + args.timeout_seconds
    report = dict(schemaVersion=1, operation='agent-iteration-benchmark', status='running', samples=[],
        environment=dict(os=platform.platform(), python=platform.python_version()),
        method='Sequential real CLI build/test/relaunch, both reference projects, 90 desktop ticks with assertions. '
            'Job-private runnable outputs; first sample is not an OS/compiler-cache-cold claim. '
            'Test/relaunch include intentional simulation, renderer and audio startup. No arbitrary runtime-state migration.')

    def save():
        destination.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\r\n')

    environment = dict(os.environ)
    os.environ.update(AETHER_AGENT_SESSION='1', AETHER_AGENT_BACKGROUND='1',
        AETHER_AGENT_NAME='Agent iteration measurement', AETHER_CONTROL_PORT='off')
    try:
        for language in ('Rust', 'CSharp'):
            for operation in ('build', 'test', 'relaunch'):
                for index in range(args.repeats):
                    remaining = min(60000, int((deadline - time.monotonic()) * 1000))
                    if remaining < 100:
                        raise TimeoutError('Iteration measurement deadline expired')
                    directory = folder / f'{language}-{operation}-{index}'
                    directory.mkdir()
                    request = dict(jobId=str(uuid.uuid4()), project=str(ROOT / 'projects' / ('AgentPipeline' + language)),
                        operation=operation, target='desktop', scene='ReferenceStart', ticks=90, step=1/60,
                        timeoutMs=remaining, launchConfig=config())
                    request_file, result_file = directory / 'request.json', directory / 'report.json'
                    request_file.write_text(json.dumps(request), encoding='utf-8')
                    report['phase'] = directory.name
                    save()
                    before = time.monotonic()
                    with (directory / 'command.log').open('wb') as log:
                        child = _OwnedProcess([str(project_tool()), '--request', str(request_file), '--report', str(result_file)], ROOT, log)
                        try:
                            while child.poll() is None:
                                if time.monotonic() >= deadline:
                                    raise TimeoutError('Iteration measurement deadline expired')
                                time.sleep(.025)
                            code = child.poll()
                        finally:
                            child.close()
                    value = json.loads(result_file.read_text(encoding='utf-8-sig'))
                    validate_report(value, request['jobId'], code)
                    if code or value['status'] != 'passed' or value['language'] != language.lower():
                        raise RuntimeError('Reference operation failed; inspect ' + str(result_file))
                    report['samples'].append(dict(language=language.lower(), operation=operation, index=index,
                        wallMs=round((time.monotonic() - before) * 1000, 1), reportedMs=value['durationMs'],
                        report=str(result_file)))
        report['medians'] = {f'{language}-{operation}': statistics.median(sample['wallMs'] for sample in report['samples']
            if sample['language'] == language and sample['operation'] == operation)
            for language in ('rust', 'csharp') for operation in ('build', 'test', 'relaunch')}
        report['status'] = 'passed'
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='cancelled' if isinstance(error, KeyboardInterrupt) else 'failed', error=str(error))
    finally:
        os.environ.clear()
        os.environ.update(environment)
        save()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', default=str(ROOT / 'logs/agent-iteration/report.json'))
    parser.add_argument('--repeats', type=int, choices=range(1, 21), default=3, metavar='1..20')
    parser.add_argument('--timeout-seconds', type=int, choices=range(1, 1801), default=600, metavar='1..1800')
    report = run(parser.parse_args())
    print(json.dumps(report, indent=2))
    return 0 if report['status'] == 'passed' else 130 if report['status'] == 'cancelled' else 1


if __name__ == '__main__':
    raise SystemExit(main())
