"""Qualify a published fixture package against a bridge-enabled sibling build.

The bridge-free verifier checks relocated startup; parity checks exercise a sibling
with byte-identical paks, not the production executable itself. Supply the original
publish build and retained web-inputs directory. Run in the Emscripten environment.
"""
from __future__ import annotations
import argparse
import hashlib
import os
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compare_paks(package, sibling):
    result = {}
    for name in ('game.pak', 'engine.pak'):
        a, b = (package/name).read_bytes(), (sibling/name).read_bytes()
        offset = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), None)
        if offset is None and len(a) != len(b):
            offset = min(len(a), len(b))
        result[name] = dict(packageSha256=hashlib.sha256(a).hexdigest(), siblingSha256=hashlib.sha256(b).hexdigest(),
                            packageBytes=len(a), siblingBytes=len(b), firstDifference=offset)
    return result


def gameplay_inputs(source):
    scripts = source/'scripts'
    return {p.relative_to(scripts).as_posix(): digest(p) for p in sorted(scripts.rglob('*'))
            if p.is_file() and p.suffix.lower() in ('.rs', '.cs', '.toml', '.csproj')
            and not {'obj', 'bin', 'artifacts'}.intersection(p.relative_to(scripts).parts)}


def cache_values(path):
    result = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        if line.startswith('AETHER') and ':' in line and '=' in line:
            key, rest = line.split(':', 1)
            result[key] = rest.split('=', 1)[1]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True, help='Relocated published package root')
    parser.add_argument('--publish-build', type=Path, required=True)
    parser.add_argument('--inputs', type=Path, required=True, help='Retained publish report web-inputs')
    parser.add_argument('--sibling', type=Path, required=True, help='Fresh private sibling build directory')
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--gameplay-scenes', default='BrowserPhysics3D')
    parser.add_argument('--lighting-scenes', default='BrowserSolid')
    parser.add_argument('--graphics-json', type=Path, help='Lighting settings applied to both backends')
    parser.add_argument('--playwright', required=True)
    args = parser.parse_args()
    package, build, inputs, sibling, report_path = (p.resolve() for p in (args.package, args.publish_build, args.inputs, args.sibling, args.report))
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report = dict(schemaVersion=1, status='failed', package=str(package), sibling=str(sibling), checks=[],
                  scope='Byte-identical packaged paks and gameplay inputs, native-vs-bridge fixture parity, and separate bridge-free production capture; executable bytes differ by instrumentation.')
    try:
        manifest = json.loads((package/'manifest.json').read_text(encoding='utf-8'))
        if manifest['profile'] != 'webgpu-3d-v1':
            raise ValueError('Package parity requires webgpu-3d-v1')
        if sibling.exists():
            raise ValueError('Sibling directory must not already exist')
        cache = cache_values(build/'CMakeCache.txt')
        language = manifest['language']
        source = Path(cache['AETHER_WEB_PROJECT'])
        report['gameplayInputs'] = gameplay_inputs(source)
        if not report['gameplayInputs']:
            raise ValueError('No gameplay library source inputs found')
        if report['gameplayInputs'] != manifest.get('gameplayInputs'):
            raise ValueError('Gameplay inputs differ from publish-time manifest')
        sibling.mkdir(parents=True)
        staged = sibling/'reference-project'
        shutil.copytree(inputs, staged)
        keys = ('AETHER_WEB_PROJECT', 'AETHER_WEB_ASSETPACKER', 'AETHER_WEB_SLANGC', 'AETHER_WEB_MATERIAL_COMPILER',
                'AETHER_WEB_GAMEPLAY_MANIFEST', 'AETHER_WEB_GAMEPLAY_NAME', 'AETHER_WEB_MANAGED_PROJECT', 'AETHERCORE_WEB_MANAGED')
        options = ['-D'+key+'='+cache[key] for key in keys if cache.get(key)]
        options += ['-DAETHER_WEB_STAGED_PROJECT='+str(staged), '-DAETHER_WEB_PROFILE=webgpu-3d-v1',
                    '-DAETHERCORE_WEB_TEST_BRIDGE=ON', '-DAETHERCORE_WEB_QUALIFY_STAGED_PROJECT=ON']
        def run(name, command):
            report['checks'].append(dict(name=name, command=list(map(str, command)), status='running'))
            with (report_path.parent/(name+'.log')).open('w', encoding='utf-8') as log:
                subprocess.run(list(map(str, command)), cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True,
                               env=dict(os.environ, AETHER_PLAYWRIGHT_MODULE=args.playwright))
            report['checks'][-1]['status'] = 'passed'
        run('configure', ['cmake', '--preset', 'webgpu-browser', '-B', sibling, *options])
        run('build', ['cmake', '--build', sibling, '-j6'])
        report['paks'] = compare_paks(package, sibling/'web')
        if any(item['firstDifference'] is not None for item in report['paks'].values()):
            raise ValueError('Bridge build pak bytes differ; inspect paks.firstDifference and hashes')
        if report['gameplayInputs'] != gameplay_inputs(source):
            raise ValueError('Gameplay source inputs changed during bridge build')
        run('production-verification', ['node', ROOT/'tools/browser-tests/verify-export.mjs', '--root='+str(package), '--report='+str(report_path.parent/'production-verification.json')])
        common = ['--build', sibling, '--language', language, '--playwright', args.playwright, '--timeout', '1200']
        run('gameplay-parity', [sys.executable, ROOT/'tools/web-runtime/gameplay-parity-checks.py', *common, '--web-root', sibling/'web', '--scenes', args.gameplay_scenes, '--checkpoints', '1,10,30,60,120,150', '--report', report_path.parent/'gameplay-parity.json'])
        graphics = ['--graphics-json', args.graphics_json] if args.graphics_json else []
        run('lighting-parity', [sys.executable, ROOT/'tools/web-runtime/lighting-parity-checks.py', *common, *graphics, '--scenes', args.lighting_scenes, '--report', report_path.parent/'lighting-parity.json'])
        report['status'] = 'passed'
    except Exception as error:
        report['error'] = str(error)
        if report['checks'] and report['checks'][-1]['status'] == 'running':
            report['checks'][-1]['status'] = 'failed'
    finally:
        report_path.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
