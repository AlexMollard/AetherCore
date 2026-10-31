"""Prove engine-linked global new is ASAN-owned using a bounded crashing child."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def validate_result(returncode, output, fault):
    if 'ASAN engine allocation probe ready' not in output:
        raise ValueError('Probe did not confirm instrumentation and engine allocator policy')
    if fault:
        if returncode == 0 or 'AddressSanitizer: heap-buffer-overflow' not in output:
            raise ValueError('Intentional heap OOB did not produce a nonzero ASAN heap-buffer-overflow diagnostic')
    elif returncode != 0 or 'ERROR: AddressSanitizer' in output:
        raise ValueError('In-bounds control allocation failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = dict(schemaVersion=1, status='fail', checks=[])
    environment = dict(os.environ)
    # Abort on the first report and avoid a GUI crash dialog. Do not accept a
    # pre-existing suppression or disabled allocator/interceptor environment.
    environment['ASAN_OPTIONS'] = 'halt_on_error=1:abort_on_error=1:detect_leaks=0'
    environment.pop('ASAN_SUPPRESSIONS', None)
    try:
        for fault in (False, True):
            arguments = [str(args.probe.resolve())] + ([] if fault else ['--valid'])
            result = subprocess.run(arguments, capture_output=True, text=True, env=environment,
                                    timeout=30, creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            output = result.stdout + result.stderr
            report['checks'].append(dict(name='heap-oob' if fault else 'in-bounds-control',
                                          exitCode=result.returncode, output=output))
            validate_result(result.returncode, output, fault)
        report['status'] = 'pass'
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        report['detail'] = str(error)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))
    return 0 if report['status'] == 'pass' else 1


if __name__ == '__main__':
    raise SystemExit(main())
