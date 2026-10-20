"""Host-side Web publishing shared by the editor and project worker."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import struct
import sys
import tempfile
import time
import tomllib
import uuid
import zipfile

REPO = Path(__file__).resolve().parents[2]
PROFILE = 'webgpu-2d-v1'


class PreflightError(Exception):
    def __init__(self, code, message):
        self.code = code
        super().__init__(message)


def reject(code, message):
    raise PreflightError(code, message)


def preflight(project, startup_override=None):
    project = Path(project).resolve()
    def settings_keys(value):
        # Native Settings/Config lookup folds keys, including newly authored startupScene.
        return {key.lower(): settings_keys(child) for key, child in value.items()} if isinstance(value, dict) else value
    settings = settings_keys(tomllib.loads((project / 'ProjectSettings.toml').read_text(encoding='utf-8-sig')))
    language = settings.get('scripting', {}).get('language', 'csharp').lower()
    if language not in ('rust', 'csharp'):
        reject('WEB_LANGUAGE_UNSUPPORTED', 'Web gameplay requires Rust or C#')
    web = settings.get('publish', {}).get('web', {})
    if web.get('profile', PROFILE) != PROFILE:
        reject('WEB_PROFILE_UNSUPPORTED', 'Only webgpu-2d-v1 is supported')
    paths = settings.get('paths', {})
    def local(value):
        path = (project / value).resolve()
        if not path.is_relative_to(project):
            reject('WEB_ASSET_PATH', 'Project path escapes its root: ' + value)
        return path
    scripts = local(paths.get('scripts', 'scripts'))
    managed_project = None
    if language == 'csharp':
        projects = list(scripts.glob('*.csproj'))
        if len(projects) != 1:
            reject('WEB_MANAGED_PROJECT', 'C# Web publishing requires exactly one gameplay .csproj in the scripts directory')
        managed_project = projects[0]
    extension = '*.rs' if language == 'rust' else '*.cs'
    source = '\n'.join(path.read_text(encoding='utf-8-sig') for path in scripts.rglob(extension)
                       if not any(part in ('obj', 'bin', 'artifacts') for part in path.relative_to(scripts).parts))
    if language == 'csharp' and re.search(r'\b(?:DllImport|LibraryImport|NativeLibrary)\b', source):
        reject('WEB_NATIVE_API_UNSUPPORTED', 'External native imports are not supported in C# Web games')
    if language == 'csharp' and re.search(r'\b(?:Net\s*\.|System\s*\.\s*Net\b|NetRpc|IEditorWindow|EditorGui)\b', source):
        reject('WEB_CAPABILITY_UNSUPPORTED', 'Networking and editor tooling are outside the C# browser 2D profile')
    if re.search(r'\b(?:std\s*::\s*net|TcpStream|UdpSocket|network_\w*|net_connect|enet)\b', source):
        reject('WEB_NETWORK_UNSUPPORTED', 'Rust networking is outside webgpu-2d-v1')
    requested = web.get('capabilities', [])
    supported = {'sprites', 'animation', 'tilemaps', 'physics_2d', 'ui', 'audio', 'input', 'saves', 'scenes', 'prefabs'}
    if not isinstance(requested, list) or any(item not in supported for item in requested):
        reject('WEB_CAPABILITY_UNSUPPORTED', 'Unsupported declared module capability')
    declared = web.get('scenes', [])
    if not isinstance(declared, list) or any(not isinstance(item, str) for item in declared):
        reject('WEB_SCENE_DECLARATION', 'publish.web.scenes must be a list of scene names')
    scenes = set(declared)
    startup = startup_override or settings.get('app', {}).get('startupscene', 'default')
    scenes.add(startup)
    scene_calls = r'\.load_scene\s*\(([^)]*)\)' if language == 'rust' else r'\bScene\s*\.\s*Load\s*\(([^)]*)\)'
    for call in re.findall(scene_calls, source):
        literal = re.fullmatch(r'\s*"([^"\\]+)"\s*', call)
        if literal:
            scenes.add(literal[1])
        elif not declared:
            reject('WEB_DYNAMIC_SCENE_UNDECLARED', 'Dynamic scene loads require publish.web.scenes')
    files = set()
    allowed_entity_tables = {'sprite', 'sprite_animator', 'rigid_body_2d', 'collider_2d', 'joint_2d', 'camera', 'scripts',
        'ui_canvas', 'ui_rect', 'ui_text', 'ui_image', 'ui_selectable', 'ui_button', 'ui_slider', 'ui_toggle', 'ui_progress_bar', 'tile_map'}
    def strings(value):
        if isinstance(value, str):
            yield value
        elif isinstance(value, dict):
            for child in value.values():
                yield from strings(child)
        elif isinstance(value, list):
            for child in value:
                yield from strings(child)
    def asset(uri):
        if not isinstance(uri, str) or not uri.startswith('project://'):
            reject('WEB_ASSET_PATH', 'Additional assets require a project:// path')
        path = local(uri.removeprefix('project://'))
        if path.suffix.lower() in {'.slang', '.spv', '.material', '.glb', '.gltf', '.dds', '.ktx', '.fontcurves'}:
            reject('WEB_SHADER_UNSUPPORTED' if path.suffix.lower() in {'.slang', '.spv', '.material'} else 'WEB_FEATURE_UNSUPPORTED', 'Unsupported browser asset: ' + uri)
        visit(path)
    def visit(path):
        path = path.resolve()
        if not path.is_relative_to(project):
            reject('WEB_ASSET_PATH', 'Reachable content escapes project root')
        if path in files:
            return
        if not path.is_file():
            reject('WEB_ASSET_MISSING', 'Missing reachable asset: ' + str(path))
        files.add(path)
        if path.name.endswith(('.scene.toml', '.prefab.toml')):
            document = tomllib.loads(path.read_text(encoding='utf-8-sig'))
            default_kind = '3d' if path.name.endswith('.scene.toml') else '2d'
            if document.get('scene', {}).get('version', 0) > 16 or document.get('scene', {}).get('kind', default_kind) != '2d':
                reject('WEB_FEATURE_UNSUPPORTED', 'Unsupported scene format or 3D scene: ' + path.name)
            if any(key in document for key in ('includes', 'prefab_instances', 'lights', 'environment')):
                reject('WEB_FEATURE_UNSUPPORTED', 'Linked scenes/3D environment are not supported: ' + path.name)
            for entity in document.get('entities', []):
                for key, value in entity.items():
                    if isinstance(value, (dict, list)) and (isinstance(value, dict) or value and isinstance(value[0], dict)) and key not in allowed_entity_tables:
                        reject('WEB_SHADER_UNSUPPORTED' if key in ('material','effect','ui_effect') else 'WEB_FEATURE_UNSUPPORTED', 'Unsupported component ' + key + ' in ' + path.name)
                if 'camera' in entity and entity['camera'].get('projection', 'perspective') != 'orthographic':
                    reject('WEB_FEATURE_UNSUPPORTED', 'Only orthographic cameras are supported')
                for widget in ('ui_text', 'ui_button'):
                    value=entity.get(widget,{})
                    if value.get('font','Roboto') != 'Roboto' or any(ord(char)>255 for char in value.get('text',value.get('label',''))):
                        reject('WEB_FONT_UNSUPPORTED','Browser UI requires packaged Roboto and Latin-1 glyphs')
                if path.name.endswith('.prefab.toml') and entity.get('scripts'):
                    reject('WEB_FEATURE_UNSUPPORTED', 'Scripted browser prefabs are not implemented')
        elif path.suffix == '.toml':
            document = tomllib.loads(path.read_text(encoding='utf-8-sig'))
        else:
            document = None
        if document is not None:
            for value in strings(document):
                if value.startswith('project://'):
                    asset(value)
        elif path.suffix == '.tilemap':
            blob = path.read_bytes()
            if len(blob) < 12 or blob[:4] != b'ATLM':
                reject('WEB_ASSET_FORMAT', 'Invalid ATLM header: ' + path.name)
            version, length = struct.unpack_from('<II', blob, 4)
            if version != 1 or length > 4096 or len(blob) < 12 + length:
                reject('WEB_ASSET_FORMAT', 'Unsupported/truncated ATLM header: ' + path.name)
            asset(blob[12:12 + length].decode('utf-8'))
    scene_dir = local(paths.get('scenes', 'scenes'))
    for scene in scenes:
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,128}', scene):
            reject('WEB_SCENE_NAME', 'Invalid browser scene name: ' + str(scene))
        path = scene_dir / (scene + '.scene.toml')
        if not path.is_file():
            reject('WEB_SCENE_MISSING', 'Missing declared/reachable scene: ' + scene)
        visit(path)
    prefabs = local(paths.get('prefabs', 'assets/prefabs'))
    prefab_calls = r'\.instantiate_prefab\s*\(\s*"([^"\\]+)"' if language == 'rust' else r'\bScene\s*\.\s*Instantiate\s*\(\s*"([^"\\]+)"'
    for name in re.findall(prefab_calls, source):
        visit(prefabs / (name + '.prefab.toml'))
    for uri in re.findall(r'"(project://[^"\\]+)"', source):
        asset(uri)
    additional = web.get('additional_assets', [])
    if not isinstance(additional, list):
        reject('WEB_ASSET_PATH', 'additional_assets must be a list')
    asset_calls = r'\.(?:play_audio|instantiate_prefab)\s*\(([^)]*)\)' if language == 'rust' else r'\b(?:Audio\s*\.\s*Play|Scene\s*\.\s*Instantiate)\s*\(([^)]*)\)'
    for call in re.findall(asset_calls, source):
        if not re.match(r'^\s*"[^"\\]+"\s*(?:,|$)', call) and not additional:
            reject('WEB_DYNAMIC_ASSET_UNDECLARED', 'Dynamic asset loads require publish.web.additional_assets')
    for uri in additional:
        asset(uri)
    return dict(project=project, settings=settings, scripts=scripts, language=language, managed_project=managed_project,
                profile=PROFILE, scenes=sorted(scenes), startup=startup, files=files)


def publish(request, directory, output=None):
    from gameplay_jobs import _OwnedProcess, _lock
    started = time.monotonic()
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    report = dict(schemaVersion=1, jobId=request.get('jobId', str(uuid.uuid4())), status='running', phase='preflight',
        target='web', language=request.get('language', 'unknown'), profile=PROFILE, diagnostics=[], toolchains=[], artifacts=[], requiredChecks=[], durationMs=0, exitCode=None)
    deadline = started + request.get('timeoutMs', 600000) / 1000
    stage = None
    project_lock = None
    def run(name, arguments, cwd=REPO):
        report['phase'] = name
        log = directory / (name + '.log')
        with log.open('ab') as stream:
            command = [str(arg) for arg in arguments]
            command[0] = shutil.which(command[0]) or command[0]
            owned = _OwnedProcess(command, cwd, stream)
            try:
                while owned.poll() is None:
                    if time.monotonic() >= deadline:
                        owned.kill()
                        reject('WEB_TIMEOUT', 'Web publish exceeded its shared deadline')
                    time.sleep(.025)
                if owned.poll() != 0:
                    reject('WEB_' + name.upper().replace('-', '_'), 'Command failed; inspect ' + str(log))
            finally:
                owned.close()
        report['artifacts'].append(dict(path=str(log), bytes=log.stat().st_size))
    try:
        data = preflight(request['project'], request.get('scene'))
        report['language'] = data['language']
        report['requiredChecks'].append(dict(name='web-preflight', status='passed'))
        pins = json.loads((REPO / 'tools/experiments/rust-webgpu/toolchain.json').read_text(encoding='utf-8'))
        sdk = Path(os.environ.get('EMSDK', REPO / '.temp/toolchains/emsdk-main')).resolve()
        if not (sdk / 'upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake').is_file():
            reject('WEB_TOOLCHAIN_MISSING', 'Install the pinned Emscripten SDK; no tools are installed automatically')
        os.environ['EMSDK'] = str(sdk)
        os.environ['EM_CONFIG'] = str(sdk / '.emscripten')
        os.environ['PATH'] = str(sdk / 'upstream/emscripten') + os.pathsep + os.environ['PATH']
        packer = Path(os.environ.get('AETHER_WEB_ASSETPACKER', REPO / ('build/default/tools/RelWithDebInfo/AssetPacker.exe' if os.name == 'nt' else 'build/linux-clang/tools/AssetPacker')))
        slang = os.environ.get('AETHER_WEB_SLANGC') or shutil.which('slangc')
        if not slang and os.environ.get('VULKAN_SDK'):
            slang = str(Path(os.environ['VULKAN_SDK']) / 'Bin/slangc.exe')
        if not packer.is_file() or not slang or not Path(slang).is_file():
            reject('WEB_TOOLCHAIN_MISSING', 'Build native AssetPacker and install pinned Slang; set AETHER_WEB_ASSETPACKER/AETHER_WEB_SLANGC')
        for tool in ('cmake', 'node') + (('cargo',) if data['language'] == 'rust' else ()):
            if not shutil.which(tool):
                reject('WEB_TOOLCHAIN_MISSING', tool + ' is unavailable')
        for name in ('emscripten', 'slang', 'emdawnwebgpu') + (('rustWasm',) if data['language'] == 'rust' else ()):
            report['toolchains'].append(dict(name=name, version=pins[name], target='web'))
        project = data['project']
        identity = hashlib.sha256(str(project).encode('utf-8')).hexdigest()[:16]
        lock_directory = REPO / 'logs/web-publish-locks' / identity
        lock_directory.mkdir(parents=True, exist_ok=True)
        project_lock = _lock(lock_directory)
        project_lock.__enter__()
        product = re.sub(r'[\x00-\x1f<>:"/\\|?*]', '_', data['settings'].get('project', {}).get('name', project.name)).rstrip(' .')
        if not product or not product.strip('_'):
            product = 'AetherGame'
        parent = project / 'Builds/Web'
        if not parent.resolve().is_relative_to(project):
            reject('WEB_OUTPUT_PATH', 'Builds/Web escapes the project root')
        parent.mkdir(parents=True, exist_ok=True)
        destination = Path(output) if output else parent / product
        if destination.is_symlink() or destination.is_junction() or destination.parent.resolve() != parent.resolve():
            reject('WEB_OUTPUT_PATH', 'Export destination must remain in the project Builds/Web folder')
        stage = Path(tempfile.mkdtemp(prefix='.web-stage-', dir=parent))
        inputs = directory / 'web-inputs'
        inputs.mkdir(exist_ok=True)
        shutil.copy2(project / 'ProjectSettings.toml', inputs / 'ProjectSettings.toml')
        for path in data['files']:
            target = inputs / path.relative_to(project)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        identity = hashlib.sha256(str(project).encode('utf-8')).hexdigest()[:16]
        build = REPO / 'build' / ('web-publish-' + identity)
        # Reuse only project-qualified build products. Configure always enforces bridge OFF.
        configure = ['cmake', '--preset', 'webgpu-browser', '-B', build,
            '-DAETHERCORE_WEB_TEST_BRIDGE=OFF', '-DAETHER_WEB_PROJECT=' + str(project),
            '-DAETHER_WEB_STAGED_PROJECT=' + str(inputs), '-DAETHER_WEB_ASSETPACKER=' + str(packer), '-DAETHER_WEB_SLANGC=' + slang]
        if data['language'] == 'rust':
            manifest_path = data['scripts'] / 'Cargo.toml'
            cargo = tomllib.loads(manifest_path.read_text(encoding='utf-8'))
            library = cargo.get('lib', {}).get('name', cargo['package']['name']).replace('-', '_')
            configure += ['-DAETHERCORE_WEB_MANAGED=OFF', '-DAETHER_WEB_GAMEPLAY_MANIFEST=' + str(manifest_path), '-DAETHER_WEB_GAMEPLAY_NAME=' + library]
        else:
            configure += ['-DAETHERCORE_WEB_MANAGED=ON', '-DAETHER_WEB_MANAGED_PROJECT=' + str(data['managed_project'])]
        run('configure', configure)
        run('build', ['cmake', '--build', build, '-j6'])
        report['requiredChecks'].append(dict(name='web-compilation-and-import-audit', status='passed'))
        names = ['index.html', 'platform.js', 'aethercore-icon.png', 'game.pak', 'engine.pak']
        if data['language'] == 'rust':
            names += ['AetherGame.js', 'AetherGame.wasm', 'AetherGame.data']
        else:
            names += ['browser-main.mjs', 'sprite.wgsl']
            shutil.copytree(build / 'web/_framework', stage / '_framework')
            managed = json.loads((build / 'web-managed-build.json').read_text(encoding='utf-8'))
            pins = {key: value for key, value in pins.items() if not key.startswith('rust') and key != 'panicStrategy'}
            pins['cppExceptions'] = 'managed-adapter-only'
            pins['managed'] = {key: managed[key] for key in ('sdk', 'runtime', 'manifest', 'mode', 'nativeModule')}
            report['toolchains'].extend(dict(name='managed-' + key, version=managed[key], target='web')
                for key in ('sdk', 'runtime', 'manifest', 'mode'))
        for name in names:
            shutil.copy2(build / 'web' / name, stage / name)
        identifier = data['settings'].get('publish', {}).get('web', {}).get('id', product)
        config = dict(schemaVersion=1, id=identifier, name=product, language=data['language'], startupScene=data['startup'], scenes=data['scenes'], profile=PROFILE,
            paths=data['settings'].get('paths', {}), instructions=data['settings'].get('publish', {}).get('web', {}).get('instructions', 'Select Play to use the keyboard.'))
        (stage / 'project.js').write_text('globalThis.aetherProject=' + json.dumps(config, ensure_ascii=True) + ';\n', encoding='utf-8')
        revision = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=REPO, capture_output=True, text=True, check=True).stdout.strip()
        files = [dict(path=path.relative_to(stage).as_posix(), sizeBytes=path.stat().st_size, sha256=hashlib.sha256(path.read_bytes()).hexdigest()) for path in sorted(stage.rglob('*')) if path.is_file()]
        manifest = dict(schemaVersion=1, target='web', profile=PROFILE, language=data['language'], engineRevision=revision,
            project=dict(id=identifier, name=product, startupScene=data['startup'], scenes=data['scenes']), toolchains=pins, files=files)
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
        package = stage / (product + '.zip')
        with zipfile.ZipFile(package, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(stage.rglob('*')):
                if path.is_file() and path != package:
                    archive.write(path, path.relative_to(stage).as_posix())
        relocated = directory / ('relocated-' + uuid.uuid4().hex)
        relocated.mkdir()
        with zipfile.ZipFile(package) as archive:
            archive.extractall(relocated)
        verification = directory / 'web-verification.json'
        run('verify', ['node', REPO / 'tools/browser-tests/verify-export.mjs', '--root=' + str(relocated), '--report=' + str(verification)])
        evidence = json.loads(verification.read_text(encoding='utf-8'))
        if evidence.get('status') != 'passed':
            reject('WEB_VERIFICATION', 'Relocated Web export did not pass browser/iframe verification')
        report['artifacts'].append(dict(path=str(verification), bytes=verification.stat().st_size))
        report['requiredChecks'].append(dict(name='relocated-zip-and-iframe', status='passed'))
        report['phase'] = 'commit'
        backup = parent / ('.previous-' + uuid.uuid4().hex)
        had_previous = destination.exists()
        if had_previous:
            destination.rename(backup)
        try:
            stage.rename(destination)
            stage = None
        except BaseException:
            if had_previous:
                backup.rename(destination)
            raise
        if had_previous:
            try:
                shutil.rmtree(backup)
            except OSError as error:
                report['diagnostics'].append(dict(code='WEB_BACKUP_CLEANUP', severity='warning', message=str(error)))
        report['artifacts'].extend(dict(path=str(destination / name), bytes=(destination / name).stat().st_size) for name in ('manifest.json', product + '.zip'))
        report['status'] = 'passed'
    except Exception as error:
        report['status'] = 'failed'
        report['diagnostics'].append(dict(code=getattr(error, 'code', 'WEB_PUBLISH'), severity='error', message=str(error)))
    finally:
        if stage is not None:
            try:
                shutil.rmtree(stage)
            except OSError as error:
                report['diagnostics'].append(dict(code='WEB_STAGE_CLEANUP', severity='warning', message=str(error)))
        if project_lock is not None:
            project_lock.__exit__(None, None, None)
        report['durationMs'] = int((time.monotonic() - started) * 1000)
        report['exitCode'] = 0 if report['status'] == 'passed' else 1
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--request', required=True)
    parser.add_argument('--report', required=True)
    parser.add_argument('--directory', required=True)
    args = parser.parse_args()
    result = publish(json.loads(Path(args.request).read_text(encoding='utf-8-sig')), args.directory)
    Path(args.report).write_text(json.dumps(result, indent=2), encoding='utf-8')
    sys.exit(result['exitCode'])
