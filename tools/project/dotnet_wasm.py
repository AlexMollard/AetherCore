"""CMake-owned C# browser build using the qualified, existing .NET/engine toolchains."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]


def installed_environment(dotnet_root, scratch):
    if not (dotnet_root / 'sdk/10.0.303').is_dir():
        raise RuntimeError('WEB_MANAGED_TOOLCHAIN: install .NET SDK 10.0.303 explicitly')
    for pack in ('Microsoft.NETCore.App.Runtime.Mono.browser-wasm', 'Microsoft.NET.Runtime.WebAssembly.Sdk'):
        if not (dotnet_root / 'packs' / pack / '10.0.9').is_dir():
            raise RuntimeError('WEB_MANAGED_TOOLCHAIN: install the matching Wasm workload/10.0.9 packs explicitly')
    overlay = scratch / 'manifests'
    for name in ('microsoft.net.workload.mono.toolchain.current', 'microsoft.net.workload.emscripten.current'):
        candidates = sorted((dotnet_root / 'sdk-manifests').glob('*/' + name + '/10.0.109'))
        if not candidates:
            raise RuntimeError('WEB_MANAGED_TOOLCHAIN: installed manifest 10.0.109 is missing: ' + name)
        shutil.copytree(candidates[-1], overlay / '10.0.300' / name / '10.0.109', dirs_exist_ok=True)
    return dict(DOTNETSDK_WORKLOAD_MANIFEST_ROOTS=str(overlay))


def engine_arguments(sdk, compat):
    stamp = hashlib.sha256(compat.read_bytes()).hexdigest()[:16] if compat.is_file() else 'missing'
    return [
        '-p:EmscriptenSdkToolsPath=' + (sdk / 'upstream').as_posix() + '/',
        '-p:EmscriptenUpstreamEmscriptenPath=' + (sdk / 'upstream/emscripten').as_posix() + '/',
        '-p:EmscriptenUpstreamBinPath=' + (sdk / 'upstream/bin').as_posix() + '/',
        '-p:WasmCachePath=' + (sdk / 'upstream/emscripten/cache').as_posix(),
        '-p:EmccExtraLDFlags=-sDEFAULT_TO_CXX=1 -fexceptions -g0 --use-port=emdawnwebgpu -sFETCH=1 -sASSERTIONS=2 --pre-js "' + compat.as_posix() + '" -DAETHER_COMPAT_' + stamp,
        '-p:WasmEnableExceptionHandling=false', '-p:WasmNativeStrip=false']


def publish(project, build):
    from gameplay_jobs import _OwnedProcess
    dotnet = shutil.which('dotnet')
    if not dotnet:
        raise RuntimeError('WEB_MANAGED_TOOLCHAIN: dotnet is missing')
    sdk = Path(os.environ.get('EMSDK', ROOT / '.temp/toolchains/emsdk-main')).resolve()
    scratch = build / 'managed'
    scratch.mkdir(parents=True, exist_ok=True)
    os.environ.update(installed_environment(Path(dotnet).resolve().parent, scratch))
    os.environ.update(EMSDK=str(sdk), EM_CONFIG=str(sdk / '.emscripten'))
    output = scratch / ('publish-' + uuid.uuid4().hex)
    compat = ROOT / 'tools/experiments/csharp-webgpu/emscripten6-compat.js'
    command = [dotnet, 'publish', ROOT / 'managed/AetherCore.Interop.Browser/AetherCore.Interop.Browser.csproj',
               '-c', 'Release', '-o', output, '-p:ArtifactsPath=' + str(scratch / 'artifacts'),
               '-p:AetherBrowser=true', '-p:AetherGameProject=' + str(project),
               '-p:AetherManagedLibraries=' + str(build / 'web-managed-libraries.txt')] + engine_arguments(sdk, compat)
    log = build / 'managed-publish.log'
    with log.open('wb') as stream:
        worker = _OwnedProcess([str(value) for value in command], ROOT / 'managed/AetherCore.Interop.Browser', stream)
        try:
            deadline = time.monotonic() + 300
            while worker.poll() is None:
                if time.monotonic() >= deadline:
                    raise TimeoutError('C# browser build exceeded 300 seconds')
                time.sleep(.03)
            if worker.poll() != 0:
                raise RuntimeError('WEB_MANAGED_BUILD: inspect ' + str(log))
        finally:
            worker.close()
    web = build / 'web'
    framework = web / '_framework'
    if framework.exists():
        if framework.is_symlink() or framework.resolve().parent != web.resolve():
            raise RuntimeError('Managed output directory escapes the build tree')
        shutil.rmtree(framework)
    for source in (output / 'wwwroot').rglob('*'):
        if source.is_file() and source.suffix not in ('.br', '.gz'):
            target = web / source.relative_to(output / 'wwwroot')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    for shader in ('sprite', 'mesh', 'hdr', 'light2d', 'occluder2d'):
        shutil.copy2(build / ('web-assets/' + shader + '.wgsl'), web / (shader + '.wgsl'))
    sys.path.insert(0, str(ROOT / 'tools/web-runtime'))
    import audit
    native = list(framework.glob('dotnet.native.*.wasm'))
    if len(native) != 1:
        raise RuntimeError('Expected exactly one managed native Wasm module')
    imports = audit.wasm_imports(native[0].read_bytes())
    audit.audit_imports(imports)
    report = dict(schemaVersion=1, status='passed', language='csharp', mode='trimmed-interpreter',
                  sdk='10.0.303', runtime='10.0.9', manifest='10.0.109', emscripten='6.0.12',
                  nativeModule=native[0].relative_to(web).as_posix(), imports=imports, sharedMemory=False)
    (build / 'web-managed-build.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\r\n')
    print('Managed Wasm build and native-import/shared-memory audit passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    publish(args.project.resolve(), args.build.resolve())
