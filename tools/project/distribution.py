"""Bounded local creation-to-export qualification; never uploads or installs tools."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import sys
import time
import uuid
import zipfile

from gameplay_jobs import _OwnedProcess, project_tool, validate_report
from reference_fixture import ROOT, assets, config

REQUIRED = ('fresh-create', 'compiler-failure', 'repaired-build', 'fresh-native-test', 'web-publish',
            'edited-web-publish', 'rejected-content-preserves-export', 'csharp-web-rejected', 'measured-export', 'performance-budgets', 'itch-package',
            'csharp-native', 'rust-native', 'csharp-publish', 'native-gauntlet', 'browser-gauntlet')


def qualify(checks):
    return 'passed' if all(checks.get(name, {}).get('status') == 'passed' for name in REQUIRED) else 'blocked'


def native_passed(report):
    phases = {item['name']: item['status'] for item in report.get('phases', [])}
    return report.get('overall') == 'PASS' and report.get('mode') == 'full' and all(
        phases.get(name) == 'pass' for name in ('guard', 'build', 'unit', 'editor', 'runtime'))


def browser_passed(report):
    groups = [report.get('cases', []), report.get('legacy', {}).get('cases', [])]
    return report.get('status') == 'passed' and report.get('legacy', {}).get('status') == 'passed' and all(
        len(group) == count and len({item['name'] for item in group}) == count and all(item['status'] == 'passed' for item in group)
        for group, count in zip(groups, (10, 31))) and all(report.get('cleanup', {}).get(name) is True for name in ('browserClosed','serverClosed','childClosed'))


def finish_unvalidated(checks, cancelled):
    for check in checks.values():
        if check['status'] == 'running':
            check.update(status='cancelled' if cancelled else 'failed', reason='Command or result validation did not complete')


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8', newline='\r\n')


def run(args):
    started = time.monotonic()
    deadline = started + args.timeout_seconds
    destination = Path(args.report).resolve()
    directory = destination.parent / ('distribution-' + uuid.uuid4().hex)
    directory.mkdir(parents=True)
    project = ROOT / '.temp/distribution' / uuid.uuid4().hex
    report = dict(schemaVersion=1, operation='distribution-qualification', status='blocked', localAcceptance='blocked',
        project=str(project), environment=dict(os=platform.platform(), machine=platform.machine(), cpu=platform.processor(), logicalCpus=os.cpu_count(), python=platform.python_version()),
        checks={}, artifacts=[], diagnostics=[], hostQualification=dict(status='pending', url=None,
            reason='No authorized external upload destination; local iframe evidence is not host qualification'),
        browserMatrix=[dict(browser='Chrome', os=platform.platform(), status='pending'),
            dict(browser='Firefox', os='Windows', version=None, status='not-tested', reason='No Firefox run in this qualification'),
            dict(browser='Safari', os='macOS', version=None, status='not-tested', reason='No macOS executor'),
            dict(browser='Chrome/Firefox', os='macOS/Linux', version=None, status='not-tested', reason='No run on these combinations'),
            dict(browser='Any', os='Any', status='unsupported', reason='Missing hardware WebGPU; C# Web, 3D and networking remain unsupported')])

    def command(label, arguments, report_file=None, expected_code=0):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('Overall distribution deadline expired')
        log = directory / (label + '.log')
        report['phase']=label
        report['checks'][label]=dict(status='running',logPath=str(log))
        if report_file:
            report['checks'][label]['reportPath']=str(report_file)
        write_json(destination,report)
        command_line = [str(value) for value in arguments]
        command_line[0] = shutil.which(command_line[0]) or command_line[0]
        before = time.monotonic()
        with log.open('wb') as output:
            child = _OwnedProcess(command_line, ROOT, output)
            try:
                while child.poll() is None:
                    if time.monotonic() >= deadline:
                        child.kill()
                        raise TimeoutError('Distribution deadline expired during ' + label)
                    time.sleep(.025)
                code = child.poll()
            finally:
                if child.poll() is None:
                    child.kill()
                child.close()
        report['artifacts'].append(str(log))
        if code != expected_code:
            report['checks'][label]['status']='failed'
            raise RuntimeError(f'{label} exited {code}, expected {expected_code}; inspect {log}')
        value = json.loads(report_file.read_text(encoding='utf-8-sig')) if report_file else {}
        return value, int((time.monotonic() - before) * 1000)

    def operation(label, operation, selected=project, target='desktop', expected='passed', error_code='RUST_COMPILER_ERROR', **fields):
        remaining_ms = min(600000, int((deadline-time.monotonic())*1000))
        if remaining_ms < 100:
            raise TimeoutError('Overall distribution deadline expired')
        request = dict(jobId=str(uuid.uuid4()), project=str(selected), operation=operation, target=target, timeoutMs=remaining_ms, **fields)
        folder = directory / label
        folder.mkdir()
        input_file, result_file = folder/'request.json', folder/'report.json'
        write_json(input_file, request)
        result, duration = command(label, [project_tool(), '--request', input_file, '--report', result_file], result_file, 0 if expected=='passed' else 1)
        validate_report(result, request['jobId'], 0 if expected=='passed' else 1)
        if result['status'] != expected:
            raise RuntimeError(label + ' did not produce expected status ' + expected)
        if expected == 'failed' and not any(item.get('code')==error_code for item in result['diagnostics']):
            raise RuntimeError('Expected diagnostic was not returned: ' + error_code)
        report['checks'][label] = dict(status='passed', durationMs=duration, expectedStatus=expected, reportPath=str(result_file))
        write_json(destination, report)
        return result, duration

    try:
        os.environ.update(AETHER_AGENT_SESSION='1', AETHER_AGENT_BACKGROUND='1', AETHER_AGENT_NAME='Distribution qualification', AETHER_CONTROL_PORT='off')
        if args.playwright_module:
            os.environ['AETHER_PLAYWRIGHT_MODULE'] = args.playwright_module
        if args.slang_compiler:
            os.environ['AETHER_WEB_SLANGC'] = args.slang_compiler
        operation('fresh-create', 'create', name='AgentJam', language='rust', template='blank_2d')
        for relative, content in assets().items():
            path=project/relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        settings=project/'ProjectSettings.toml'
        text=settings.read_text(encoding='utf-8-sig').replace('startupScene = "default"','startupScene = "ReferenceStart"')
        text += '\n[publish.web]\nid="agentjam.'+project.name+'"\nscenes=["ReferenceStart","ReferenceResult"]\ninstructions="Hold Space to score. Your run saves automatically."\n'
        settings.write_text(text, encoding='utf-8', newline='\r\n')
        source=project/'scripts/src/lib.rs'
        good=(Path(__file__).with_name('autonomous_game.rs')).read_text(encoding='utf-8')
        source.write_text(good+'\nintentional compiler failure\n', encoding='utf-8', newline='\r\n')
        operation('compiler-failure','build', expected='failed')
        repaired=time.monotonic()
        source.write_text(good, encoding='utf-8', newline='\r\n')
        operation('repaired-build','build')
        operation('fresh-native-test','test',scene='ReferenceStart',ticks=90,launchConfig=config())
        report['nativeRepairToTestMs']=int((time.monotonic()-repaired)*1000)
        first, cold_build = operation('web-publish','publish', target='web')
        edited=time.monotonic()
        source.write_text(good.replace('Autonomous build 1','Autonomous build 2'), encoding='utf-8', newline='\r\n')
        final, warm_build = operation('edited-web-publish','publish',target='web')
        edit_to_ready=int((time.monotonic()-edited)*1000)
        package=next(Path(item['path']) for item in final['artifacts'] if item['path'].endswith('.zip'))
        previous_hash=hashlib.sha256(package.read_bytes()).hexdigest()
        current=source.read_text(encoding='utf-8')
        try:
            source.write_text(current+'\nuse std::net::TcpStream;\n',encoding='utf-8',newline='\r\n')
            operation('unsupported-network','publish',target='web',expected='failed',error_code='WEB_NETWORK_UNSUPPORTED')
        finally:
            source.write_text(current,encoding='utf-8',newline='\r\n')
        scene=project/'scenes/ReferenceStart.scene.toml'
        current=scene.read_text(encoding='utf-8')
        try:
            scene.write_text(current+'\n[entities.mesh]\npath="model.glb"\n',encoding='utf-8',newline='\r\n')
            operation('unsupported-3d','publish',target='web',expected='failed',error_code='WEB_FEATURE_UNSUPPORTED')
        finally:
            scene.write_text(current,encoding='utf-8',newline='\r\n')
        operation('csharp-web-rejected','publish',selected=ROOT/'projects/AgentPipelineCSharp',target='web',expected='failed',error_code='WEB_CSHARP_UNAVAILABLE')
        if hashlib.sha256(package.read_bytes()).hexdigest() != previous_hash:
            raise RuntimeError('Rejected content replaced a verified export')
        report['checks']['rejected-content-preserves-export']=dict(status='passed',sha256=previous_hash)
        extracted=directory/'relocated-final'
        with zipfile.ZipFile(package) as archive:
            entries=archive.infolist()
            if 'index.html' not in archive.namelist() or len(entries)>1000 or any(len(item.filename)>240 or item.file_size>200_000_000 for item in entries) or sum(item.file_size for item in entries)>500_000_000:
                raise RuntimeError('ZIP fails documented itch.io package limits')
            archive.extractall(extracted)
        report['checks']['itch-package']=dict(status='passed', files=len(entries), extractedBytes=sum(item.file_size for item in entries), package=str(package), hostVerified=False)
        measured_file=directory/'measured-export.json'
        measured, _=command('measured-export',['node',ROOT/'tools/browser-tests/verify-export.mjs','--root='+str(extracted),'--report='+str(measured_file),'--measure=true','--scenario=agent-reference-v1','--budgets='+str(Path(args.budgets).resolve()),'--zipBytes='+str(package.stat().st_size),'--editToReadyMs='+str(edit_to_ready)],measured_file)
        if measured['status'] != 'passed' or not all(measured['cleanup'].values()) or not measured.get('agentGame') or not measured.get('iframeGame'):
            raise RuntimeError('Measured browser/iframe qualification incomplete')
        if not any(item.get('message')=='Autonomous build 2' for item in measured['directState'].get('gameplayLogs',[])):
            raise RuntimeError('Edited selected-project gameplay did not run')
        metrics={**measured['measurements'], 'zipBytes':package.stat().st_size, 'editToReadyMs':edit_to_ready}
        report['measurements']=metrics
        report['measurements']['coldWebPublishMs']=cold_build
        report['measurements']['warmWebPublishMs']=warm_build
        report['browserMatrix'][0].update(status='passed', version=measured['environment']['browserVersion'],gpu=measured['environment']['gpu'])
        report['checks']['measured-export']=dict(status='passed',reportPath=str(measured_file))
        if measured.get('budgetChecks',{}).get('status') != 'passed':
            raise RuntimeError('Performance budget missing or exceeded; retain measured evidence')
        report['checks']['performance-budgets']=measured['budgetChecks']
        report['artifacts'] += [str(package),str(measured_file)]
        for language in ('CSharp','Rust'):
            selected=ROOT/'projects'/('AgentPipeline'+language)
            operation(language.lower()+'-native','test',selected=selected,scene='ReferenceStart',ticks=90,launchConfig=config())
        operation('csharp-publish','publish',selected=ROOT/'projects/AgentPipelineCSharp')
        native_file=directory/'native-gauntlet.json'
        native,_=command('native-gauntlet',['pwsh','-NoProfile','-File',ROOT/'scripts/Run-DebugGauntlet.ps1','-Fast','-RunSeconds','3','-ReportPath',native_file],native_file)
        if not native_passed(native):
            raise RuntimeError('Full native gauntlet incomplete')
        report['checks']['native-gauntlet']=dict(status='passed', reportPath=str(native_file))
        browser_file=directory/'browser/report.json'
        browser_args=['pwsh','-NoProfile','-File',ROOT/'scripts/Run-BrowserGauntlet.ps1','-TimeoutSeconds',str(min(1200,max(1,int(deadline-time.monotonic())))),'-ReportPath',browser_file]
        if args.slang_compiler:
            browser_args += ['-SlangCompiler',args.slang_compiler]
        browser,_=command('browser-gauntlet',browser_args,browser_file)
        if not browser_passed(browser):
            raise RuntimeError('41-case browser gauntlet incomplete')
        report['checks']['browser-gauntlet']=dict(status='passed',reportPath=str(browser_file))
        report['status']=report['localAcceptance']=qualify(report['checks'])
    except (Exception, KeyboardInterrupt) as error:
        report['diagnostics'].append(dict(code='DISTRIBUTION_QUALIFICATION',message=str(error)))
        report['status']='cancelled' if isinstance(error,(TimeoutError,KeyboardInterrupt)) else 'failed'
    finally:
        finish_unvalidated(report['checks'],report['status']=='cancelled')
        for name in REQUIRED:
            report['checks'].setdefault(name,dict(status='blocked',reason='Required check did not execute'))
        report['durationMs']=int((time.monotonic()-started)*1000)
        report['noEditorInteraction']=True
        write_json(destination,report)
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', default=str(ROOT/'logs/distribution-report.json'))
    parser.add_argument('--timeout-seconds', type=int, default=1200, choices=range(1,3601), metavar='1..3600')
    parser.add_argument('--playwright-module', default=os.environ.get('AETHER_PLAYWRIGHT_MODULE',''))
    parser.add_argument('--slang-compiler', default=os.environ.get('AETHER_WEB_SLANGC',''))
    parser.add_argument('--budgets', default=str(ROOT/'tools/browser-tests/distribution-budgets.json'))
    report=run(parser.parse_args())
    print(json.dumps(report,indent=2))
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':
    sys.exit(main())
