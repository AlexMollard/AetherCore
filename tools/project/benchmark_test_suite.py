"""Compare three independent native test jobs with a build-once suite, for C# and Rust."""
import argparse
import copy
import json
from pathlib import Path
import platform
import statistics
import time
import uuid

from gameplay_jobs import _OwnedProcess, project_tool, validate_report
from reference_fixture import ROOT, config


def cases():
    result = []
    for name, held_ticks in (('standard', 20), ('no-input', 0), ('short-input', 5)):
        launch = copy.deepcopy(config())
        launch.pop('checkpoints', None)
        launch['input'] = ([dict(tick=0, key='space', down=True), dict(tick=held_ticks, key='space', down=False)] if held_ticks else [])
        for item in launch['assertions']:
            if item['property'] == 'score':
                item['equals'] = held_ticks
            if item['entity'] == 'Counter' and item['property'] == 'uiText':
                item['equals'] = f'Saved score {held_ticks}'
        result.append(dict(name=name, scene='ReferenceStart', ticks=90, launchConfig=launch))
    return result


def execute(request, directory, deadline):
    directory.mkdir(parents=True)
    request = dict(request, jobId=str(uuid.uuid4()), timeoutMs=min(request.get('timeoutMs', 600000), int((deadline - time.monotonic()) * 1000)))
    if request['timeoutMs'] < 100:
        raise TimeoutError('Test-suite measurement deadline expired')
    source, destination = directory / 'request.json', directory / 'report.json'
    source.write_text(json.dumps(request, indent=2), encoding='utf-8')
    started = time.monotonic()
    with (directory / 'command.log').open('wb') as log:
        child = _OwnedProcess([str(project_tool()), '--request', str(source), '--report', str(destination)], ROOT, log)
        try:
            while child.poll() is None:
                if time.monotonic() >= deadline:
                    raise TimeoutError('Test-suite measurement deadline expired')
                time.sleep(.025)
            code = child.poll()
        finally:
            child.close()
    report = json.loads(destination.read_text(encoding='utf-8-sig'))
    validate_report(report, request['jobId'], code)
    return report, round((time.monotonic() - started) * 1000, 1)


def run(args):
    destination = Path(args.report).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    folder = destination.parent / ('suite-measurement-' + uuid.uuid4().hex)
    deadline = time.monotonic() + args.timeout_seconds
    report = dict(status='running', samples=[], environment=dict(os=platform.platform()),
        method='Three input branches, each 90 real desktop ticks and explicit final assertions. '
               'Sequential fresh runtimes; saves remain project-scoped. Warm project compiler caches; no cold-cache claim.')
    try:
        for language in ('Rust', 'CSharp'):
            for mode in (('individual', 'suite') if args.mode == 'compare' else (args.mode,)):
                for repeat in range(args.repeats):
                    request = dict(project=str(ROOT / 'projects' / ('AgentPipeline' + language)), operation='test')
                    started = time.monotonic()
                    paths = []
                    for index, overrides in enumerate(cases() if mode == 'individual' else [dict(cases=cases())]):
                        directory = folder / language / mode / str(repeat) / str(index)
                        result, _ = execute(dict(request, **overrides), directory, deadline)
                        if result['status'] != 'passed':
                            raise RuntimeError('Reference test failed: ' + str(directory / 'report.json'))
                        paths.append(str(directory / 'report.json'))
                    report['samples'].append(dict(language=language.lower(), mode=mode, repeat=repeat,
                        wallMs=round((time.monotonic() - started) * 1000, 1), reports=paths))
        report['medians'] = {f'{language}-{mode}': statistics.median(item['wallMs'] for item in report['samples']
            if item['language'] == language and item['mode'] == mode)
            for language, mode in {(item['language'], item['mode']) for item in report['samples']}}
        report['status'] = 'passed'
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='cancelled' if isinstance(error, KeyboardInterrupt) else 'failed', error=str(error))
    destination.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\r\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('individual', 'suite', 'compare'), default='compare')
    parser.add_argument('--repeats', type=int, choices=range(1, 11), default=3, metavar='1..10')
    parser.add_argument('--timeout-seconds', type=int, choices=range(1, 1801), default=600, metavar='1..1800')
    parser.add_argument('--report', default=str(ROOT / 'logs/agent-test-suite/measurement.json'))
    result = run(parser.parse_args())
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['status'] == 'passed' else 130 if result['status'] == 'cancelled' else 1)
