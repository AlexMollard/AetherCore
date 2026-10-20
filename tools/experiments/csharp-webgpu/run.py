"""Isolated .NET Wasm ABI qualification; does not enable C# Web publishing."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'tools/project'))
from gameplay_jobs import _OwnedProcess


def validate_probe(report):
    if report.get('profile') != 'csharp-wasm-interop-v1' or report.get('status') != 'passed' or report.get('runtime') != '10.0.9':
        raise ValueError('Probe identity, runtime pin or status did not match')
    for name in ('scalar', 'utf8', 'structure', 'callbacks', 'reflection'):
        if report.get(name) is not True:
            raise ValueError('Required observed ABI check did not pass: ' + name)


def qualify(args):
    started = time.monotonic()
    deadline = started + args.timeout_seconds
    output = ROOT / '.temp/task14' / ('probe-' + args.mode)
    logs = Path(args.report).resolve().parent
    logs.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    report = dict(schemaVersion=1, profile='csharp-wasm-interop-v1', mode=args.mode, status='failed',
                  gameplayQualified=False, checks=[], diagnostics=[], artifacts=[])

    def command(name, arguments):
        log = logs / (args.mode + '-' + name + '.log')
        report['phase'] = name
        report['artifacts'].append(str(log))
        with log.open('wb') as stream:
            owned = _OwnedProcess([str(arg) for arg in arguments], HERE, stream)
            try:
                while owned.poll() is None:
                    if time.monotonic() >= deadline:
                        raise TimeoutError('Probe exceeded its shared deadline')
                    time.sleep(.03)
                code = owned.poll()
            finally:
                owned.close()
        if code != 0:
            raise RuntimeError(f'{name} exited {code}; inspect {log}')
        return log

    try:
        dotnet = shutil.which('dotnet')
        if not dotnet:
            raise RuntimeError('Install the pinned .NET SDK and Wasm workload explicitly')
        dotnet_root = Path(dotnet).resolve().parent
        if not (dotnet_root / 'sdk/10.0.303').is_dir():
            raise RuntimeError('Pinned .NET SDK 10.0.303 is missing')
        if not (dotnet_root / 'packs/Microsoft.NETCore.App.Runtime.Mono.browser-wasm/10.0.9').is_dir():
            raise RuntimeError('Pinned Mono browser-wasm pack 10.0.9 is missing')
        # Select existing matching manifests only for this process's children.
        # No system SDK/manifest files or workload installations are changed.
        overlay = output / 'manifests'
        for name in ('microsoft.net.workload.mono.toolchain.current', 'microsoft.net.workload.emscripten.current'):
            candidates = sorted((dotnet_root / 'sdk-manifests').glob('*/' + name + '/10.0.109'))
            if not candidates:
                raise RuntimeError('Pinned installed workload manifest 10.0.109 is missing: ' + name)
            shutil.copytree(candidates[-1], overlay / '10.0.300' / name / '10.0.109', dirs_exist_ok=True)
        os.environ['DOTNETSDK_WORKLOAD_MANIFEST_ROOTS'] = str(overlay)
        report['toolchains'] = dict(dotnetSdk='10.0.303', mono='10.0.9', manifest='10.0.109',
                                    emscripten='6.0.12' if args.mode == 'engine' else '3.1.56')
        arguments = [dotnet, 'publish', HERE / 'InteropProbe.csproj', '-c', 'Release', '-o', output / 'publish',
                     '-p:ArtifactsPath=' + str(output / 'managed')]
        if args.mode == 'engine':
            sdk = Path(args.emsdk).resolve()
            if not (sdk / 'upstream/emscripten/emcc.py').is_file():
                raise RuntimeError('Pinned engine Emscripten SDK is missing')
            os.environ['EMSDK'] = str(sdk)
            os.environ['EM_CONFIG'] = str(sdk / '.emscripten')
            compat = HERE / 'emscripten6-compat.js'
            stamp = hashlib.sha256(compat.read_bytes()).hexdigest()[:16]
            arguments += [
                '-p:AetherEngineToolchain=true',
                '-p:EmscriptenSdkToolsPath=' + str(sdk / 'upstream') + '/',
                '-p:EmscriptenUpstreamEmscriptenPath=' + str(sdk / 'upstream/emscripten') + '/',
                '-p:EmscriptenUpstreamBinPath=' + str(sdk / 'upstream/bin') + '/',
                '-p:WasmCachePath=' + str(sdk / 'upstream/emscripten/cache'),
                '-p:EmccExtraLDFlags=-sDEFAULT_TO_CXX=1 -fexceptions -g0 --pre-js "' + compat.as_posix() + '" -sASSERTIONS=2 -DAETHER_COMPAT_' + stamp,
                '-p:WasmEnableExceptionHandling=false', '-p:WasmNativeStrip=false']
        command('publish', arguments)
        observed = command('execute', [shutil.which('node') or 'node', output / 'publish/wwwroot/main.mjs'])
        records = [json.loads(line) for line in observed.read_text(encoding='utf-8', errors='replace').splitlines() if line.startswith('{')]
        if len(records) != 1:
            raise ValueError('Expected exactly one structured interop result')
        validate_probe(records[0])
        report['checks'].append(records[0])
        report['status'] = 'passed'
    except (Exception, KeyboardInterrupt) as error:
        report['status'] = 'cancelled' if isinstance(error, (TimeoutError, KeyboardInterrupt)) else 'failed'
        report['diagnostics'].append(dict(code='CSHARP_WASM_INTEROP', message=str(error)))
    finally:
        report['durationMs'] = int((time.monotonic() - started) * 1000)
        Path(args.report).resolve().write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\r\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('stock', 'engine'), default='stock')
    parser.add_argument('--emsdk', default=str(ROOT / '.temp/toolchains/emsdk-main'))
    parser.add_argument('--timeout-seconds', type=int, choices=range(1, 1201), default=300, metavar='1..1200')
    parser.add_argument('--report', default=str(ROOT / 'logs/task14/interop.json'))
    report = qualify(parser.parse_args())
    print(json.dumps(report, indent=2))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
