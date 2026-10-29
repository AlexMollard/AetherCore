"""Build the C# (managed) browser runtime with the automation bridge and assemble a served web root.

The root holds the reference/fixture game.pak (every maintained scene, 3D included) and the
projects/AgentPipelineCSharp scripts. It is the --web-root for
`gameplay-parity-checks.py --language csharp`. Private qualification only: published exports
never carry the bridge (web_publish.py configures it OFF).
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ['sprite', 'mesh', 'tiled_light_cull', 'hdr', 'light2d', 'occluder2d', 'billboard', 'ui_shapes_web', 'exposure']
POST = ['bloom', 'fxaa', 'tonemap', 'gtao_main', 'gtao_denoise', 'ssr', 'ssr_composite', 'dof', 'volumetric_fog', 'volumetric_composite']


def run(command, log):
    command = [str(part) for part in command]
    command[0] = shutil.which(command[0]) or command[0]
    with log.open('wb') as stream:
        result = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        raise SystemExit('FAILED (' + str(result.returncode) + '): ' + ' '.join(command[:3]) + ' ... see ' + str(log))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/webgpu-managed')
    parser.add_argument('--project', type=Path, help='C# gameplay csproj; defaults to a project staged by gameplay-parity-checks.py --stage-project (includes the 3D fixture scripts)')
    parser.add_argument('--root', type=Path, help='Assembled web root; defaults to <build>/parity-root')
    parser.add_argument('--packer', type=Path, default=Path(os.environ.get('AETHER_WEB_ASSETPACKER', ROOT / ('build/default/tools/RelWithDebInfo/AssetPacker.exe' if os.name == 'nt' else 'build/linux-clang/tools/AssetPacker'))))
    parser.add_argument('--slangc', default=os.environ.get('AETHER_WEB_SLANGC') or shutil.which('slangc'))
    parser.add_argument('--jobs', type=int, default=6)
    args = parser.parse_args()
    build = args.build.resolve()
    root = (args.root or build / 'parity-root').resolve()
    sdk = Path(os.environ.get('EMSDK', ROOT / '.temp/toolchains/emsdk-main')).resolve()
    if not (sdk / 'upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake').is_file():
        raise SystemExit('Pinned Emscripten SDK is missing: ' + str(sdk))
    if not args.packer.is_file() or not args.slangc or not Path(args.slangc).is_file():
        raise SystemExit('Need a built AssetPacker (--packer) and slangc (--slangc)')
    if args.project is None:
        staged = subprocess.run([sys.executable, str(ROOT / 'tools/web-runtime/gameplay-parity-checks.py'), '--language', 'csharp', '--stage-project'], cwd=ROOT, capture_output=True, text=True, check=True)
        args.project = Path(json.loads(staged.stdout)['project']) / 'scripts/AetherGame.csproj'
    if not args.project.is_file():
        raise SystemExit('C# project not found: ' + str(args.project))
    os.environ.update(EMSDK=str(sdk), EM_CONFIG=str(sdk / '.emscripten'))
    os.environ['PATH'] = str(sdk / 'upstream/emscripten') + os.pathsep + os.environ['PATH']
    build.mkdir(parents=True, exist_ok=True)
    run(['cmake', '--preset', 'webgpu-browser', '-B', build, '-DAETHERCORE_WEB_MANAGED=ON', '-DAETHERCORE_WEB_TEST_BRIDGE=ON',
         '-DAETHER_WEB_MANAGED_PROJECT=' + args.project.resolve().as_posix(), '-DAETHER_WEB_ASSETPACKER=' + args.packer.resolve().as_posix(),
         '-DAETHER_WEB_SLANGC=' + str(args.slangc)], build / 'configure.log')
    run(['cmake', '--build', build, '-j' + str(args.jobs)], build / 'build.log')
    web = build / 'web'
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    names = ['index.html', 'platform.js', 'aethercore-icon.png', 'game.pak', 'engine.pak', 'test-bridge.js', 'browser-main.mjs']
    names += [name + '.wgsl' for name in SHADERS] + ['post-' + name + '.wgsl' for name in POST]
    for name in names:
        shutil.copy2(web / name, root / name)
    shutil.copytree(web / '_framework', root / '_framework')
    (root / 'project.js').write_text('globalThis.aetherProject=null;\n', encoding='utf-8')
    print(root)
    return 0


if __name__ == '__main__':
    sys.exit(main())
