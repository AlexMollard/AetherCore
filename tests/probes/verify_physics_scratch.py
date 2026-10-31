"""Run the opt-in EngineTests scratch policy probe in bounded child processes."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


TEST_CASE = 'Physics scratch exhaustion child probe'
SURVIVED = 'PHYSICS_SCRATCH_PROBE_SURVIVED'
ASSERTION = 'Physics scratch limit exceeded; increase physics scratch budgets or reduce scene complexity'
DIAGNOSTIC = re.compile(
    r'Physics scratch exhausted: request=(\d+) arenaUsage=(\d+)/1024 '
    r'fallbackUsage=0/0 highWater=\d+\. Terminating: '
    r'Jolt cannot roll back a partially executed step\.')
SANITIZER_DIAGNOSTIC = re.compile(
    r'\b(?:Address|Leak|Memory|Thread|UndefinedBehavior)Sanitizer\b|runtime error:',
    re.IGNORECASE)


def validate_result(returncode, output, mode):
    output = re.sub(r'\x1b\[[0-9;]*m', '', output)
    if f'PHYSICS_SCRATCH_PROBE_READY {mode}' not in output:
        raise ValueError('Child did not enter the requested scratch probe')
    if SANITIZER_DIAGNOSTIC.search(output):
        raise ValueError('Sanitizer failure is not evidence of the scratch policy')
    if mode == 'control':
        if returncode != 0 or SURVIVED not in output or 'Physics scratch exhausted:' in output:
            raise ValueError('Normal-budget control did not complete successfully')
    else:
        match = DIAGNOSTIC.search(output)
        if returncode == 0 or SURVIVED in output or ASSERTION not in output or match is None:
            raise ValueError('Exhaustion did not terminate with the exact scratch policy diagnostics')
        requested, used = map(int, match.groups())
        if used > 1024 or requested <= 1024 - used:
            raise ValueError('Reported allocation does not exceed the configured scratch budgets')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine-tests', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=30, choices=range(1, 121), metavar='1..120')
    args = parser.parse_args()
    executable = args.engine_tests.resolve(strict=True)
    report = dict(schemaVersion=1, status='fail', executable=str(executable), checks=[])
    try:
        for mode in ('control', 'exhaust'):
            environment = dict(os.environ)
            environment['AETHER_TEST_PHYSICS_SCRATCH_CHILD'] = mode
            result = subprocess.run(
                [str(executable), f'--test-case={TEST_CASE}', '--no-colors=true'],
                capture_output=True, text=True, encoding='utf-8', errors='replace', env=environment,
                timeout=args.timeout,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            output = result.stdout + result.stderr
            report['checks'].append(dict(name=mode, exitCode=result.returncode, output=output))
            validate_result(result.returncode, output, mode)
        report['status'] = 'pass'
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        report['detail'] = str(error)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))
    return 0 if report['status'] == 'pass' else 1


if __name__ == '__main__':
    raise SystemExit(main())
