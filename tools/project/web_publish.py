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


def graphics_for_web(graphics):
    # Native TOML lookup folds keys; the browser graphics manifest uses these
    # case-sensitive field names (WebScene::ExtractPostSettings).
    names = (
        'autoExposure', 'gradeContrast', 'gradeSaturation', 'gradeTemperature', 'gradeTint',
        'vignetteIntensity', 'vignetteRoundness', 'chromaticAberration', 'filmGrain',
        'motionBlur', 'motionBlurMaxRadius', 'gtaoRadius', 'gtaoStrength', 'specularFilter',
        'foliageSkyLook', 'bloomStrength', 'bloomRadius', 'exposureKey', 'exposureSpeed',
        'reflectionMaxRoughness', 'reflectionIntensity', 'renderScale', 'contactShadows',
        'shadowSplitLambda',
    )
    canonical = {name.lower(): name for name in names}
    return {canonical.get(key.lower(), key.lower()): value for key, value in graphics.items()}


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
    profile = web.get('profile', PROFILE)
    if profile not in (PROFILE, 'webgpu-3d-v1'):
        reject('WEB_PROFILE_UNSUPPORTED', 'Choose webgpu-2d-v1 or webgpu-3d-v1')
    is_3d = profile == 'webgpu-3d-v1'
    paths = settings.get('paths', {})
    def local(value):
        path = (project / value).resolve()
        if not path.is_relative_to(project):
            reject('WEB_ASSET_PATH', 'Project path escapes its root: ' + value)
        return path
    catalog_path = project / 'asset-catalog.toml'
    identities = {}
    if catalog_path.is_file():
        catalog = tomllib.loads(catalog_path.read_text(encoding='utf-8-sig'))
        if catalog.get('version') != 1 or not isinstance(catalog.get('assets', []), list):
            reject('WEB_ASSET_CATALOG', 'Unsupported asset catalog')
        paths_seen = set()
        for entry in catalog.get('assets', []):
            if not isinstance(entry, dict):
                reject('WEB_ASSET_CATALOG', 'Invalid asset identity')
            uid, path = entry.get('uid'), entry.get('path')
            if (not isinstance(uid, str) or not re.fullmatch(r'[0-9a-f]{32}', uid)
                    or not isinstance(path, str) or not path or uid in identities or path in paths_seen
                    or Path(path).is_absolute() or any(part in ('.', '..') for part in path.split('/'))):
                reject('WEB_ASSET_CATALOG', 'Invalid or duplicate asset identity')
            local(path)
            identities[uid] = path
            paths_seen.add(path)
    def resolve_uri(uri):
        if uri.startswith('uid://'):
            path = identities.get(uri[6:])
            if path is None:
                reject('WEB_ASSET_MISSING', 'Unknown asset identity: ' + uri)
            return 'project://' + path
        return uri
    def scene_path(name, directory, suffix):
        if name.startswith(('uid://', 'project://')):
            uri = resolve_uri(name)
            path = local(uri.removeprefix('project://'))
            if not path.name.endswith(suffix):
                reject('WEB_SCENE_NAME', 'Invalid scene reference: ' + name)
            return path
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,128}', name):
            reject('WEB_SCENE_NAME', 'Invalid browser scene name: ' + str(name))
        return directory / (name + suffix)
    scripts = local(paths.get('scripts', 'scripts'))
    managed_project = None
    if language == 'csharp':
        projects = list(scripts.glob('*.csproj'))
        if len(projects) != 1:
            reject('WEB_MANAGED_PROJECT', 'C# Web publishing requires exactly one gameplay .csproj in the scripts directory')
        managed_project = projects[0]
    extension = '*.rs' if language == 'rust' else '*.cs'
    native_cache = (project / 'Builds/Intermediate').resolve()
    source = '\n'.join(path.read_text(encoding='utf-8-sig') for path in scripts.rglob(extension)
                       if not path.resolve().is_relative_to(native_cache)
                       and not any(part in ('obj', 'bin', 'artifacts') for part in path.relative_to(scripts).parts))
    if language == 'csharp' and re.search(r'\b(?:DllImport|LibraryImport|NativeLibrary)\b', source):
        reject('WEB_NATIVE_API_UNSUPPORTED', 'External native imports are not supported in C# Web games')
    if is_3d and language == 'csharp' and re.search(r'\b(?:Animation\s*\.\s*(?:Add|LoadExternal|Compile|SetJointOffset)|Physics\s*\.\s*DebugEnabled)\b', source):
        reject('WEB_FEATURE_UNSUPPORTED', 'Desktop-only Animation or Physics API is not available in browser gameplay')
    if language == 'csharp' and re.search(r'\b(?:Net\s*\.|System\s*\.\s*Net\b|NetRpc|IEditorWindow|EditorGui)\b', source):
        reject('WEB_CAPABILITY_UNSUPPORTED', 'Networking and editor tooling are outside the C# browser profile' if is_3d else 'Networking and editor tooling are outside the C# browser 2D profile')
    if re.search(r'\b(?:std\s*::\s*net|TcpStream|UdpSocket|network_\w*|net_connect|enet)\b', source):
        reject('WEB_NETWORK_UNSUPPORTED', 'Rust networking is outside ' + profile)
    requested = web.get('capabilities', [])
    supported = {'sprites', 'animation', 'tilemaps', 'physics_2d', 'ui', 'audio', 'input', 'saves', 'scenes', 'prefabs'}
    if is_3d:
        supported |= {'meshes_3d', 'lighting_3d', 'physics_3d', 'materials', 'effects'}
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
    if catalog_path.is_file():
        files.add(catalog_path.resolve())
    allowed_entity_tables = {'sprite', 'sprite_animator', 'rigid_body_2d', 'collider_2d', 'joint_2d', 'camera', 'scripts',
        'ui_canvas', 'ui_rect', 'ui_text', 'ui_image', 'ui_selectable', 'ui_button', 'ui_slider', 'ui_toggle', 'ui_progress_bar', 'tile_map'}
    if is_3d:
        # SceneSerializerToml::ComponentTomlKey lowercases display names and replaces
        # spaces with underscores; Particle Emitter has its explicit legacy key.
        reflected = ('Rigid Body 2D', 'Collider 2D', 'Tile Map', 'Character Controller',
            'Darkness Volume', 'Grass', 'Grass Bender', 'Point Light', 'Spot Light', 'Sky', 'Light 2D Settings',
            'UI Canvas', 'UI Rect', 'UI Text', 'UI Image', 'UI Selectable', 'UI Slider',
            'UI Toggle', 'UI Button', 'UI Progress Bar')
        allowed_entity_tables = {name.lower().replace(' ', '_') for name in reflected}
        # EntityRecord's explicitly serialized tables, including the legacy particle override.
        allowed_entity_tables |= {'sprite', 'sprite_animator', 'mesh', 'material', 'skinned',
            'physics', 'joint', 'joint_2d', 'ui_image', 'effect', 'camera', 'scripts',
            'script_joints', 'particles'}
    features_used = set()
    if is_3d and settings.get('graphics'):
        features_used.add('effects')
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
        if not isinstance(uri, str):
            reject('WEB_ASSET_PATH', 'Additional assets require a project:// or uid:// path')
        uri = resolve_uri(uri)
        if not uri.startswith('project://'):
            reject('WEB_ASSET_PATH', 'Additional assets require a project:// or uid:// path')
        path = local(uri.removeprefix('project://'))
        unsupported = {'.spv', '.dds', '.ktx', '.fontcurves'}
        if not is_3d:
            unsupported |= {'.slang', '.material', '.glb', '.gltf'}
        if path.suffix.lower() in unsupported:
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
        if is_3d and path.suffix.lower() == '.slang':
            features_used.add('custom_pass')
            sys.path.insert(0, str(REPO / 'tools/shaders'))
            try:
                from web_materials import includes_custom_pass_contract
            finally:
                sys.path.pop(0)
            if not includes_custom_pass_contract(path.read_text(encoding='utf-8-sig')):
                reject('WEB_CUSTOM_PASS_PORTABILITY', 'Custom pass must include include/CustomPass.slangh: ' + str(path))
        if is_3d and path.suffix.lower() in ('.slang', '.slangh'):
            for include in re.findall(r'^\s*#\s*include\s*"([^"]+)"', path.read_text(encoding='utf-8-sig'), re.MULTILINE):
                if include not in ('include/CustomPass.slangh', 'CustomPass.slangh'):
                    visit(path.parent / include)
        if is_3d and path.suffix.lower() == '.gltf':
            model = json.loads(path.read_text(encoding='utf-8-sig'))
            for entry in model.get('buffers', []) + model.get('images', []):
                uri = entry.get('uri', '')
                if uri and not uri.startswith('data:'):
                    visit(path.parent / uri)
        if path.name.endswith(('.scene.toml', '.prefab.toml')):
            document = tomllib.loads(path.read_text(encoding='utf-8-sig'))
            default_kind = '3d' if path.name.endswith('.scene.toml') else '2d'
            version = document.get('scene', {}).get('version', 1)
            if type(version) is not int or not 1 <= version <= 17 or (not is_3d and document.get('scene', {}).get('kind', default_kind) != '2d'):
                reject('WEB_FEATURE_UNSUPPORTED', 'Unsupported scene format or 3D scene: ' + path.name)
            if not is_3d and any(key in document for key in ('includes', 'prefab_instances', 'lights', 'environment')):
                reject('WEB_FEATURE_UNSUPPORTED', 'Linked scenes/3D environment are not supported: ' + path.name)
            if is_3d:
                for include in document.get('includes', []):
                    visit(scene_path(include['scene'], local(paths.get('scenes', 'scenes')), '.scene.toml'))
                for instance in document.get('prefab_instances', []):
                    visit(scene_path(instance['prefab'], local(paths.get('prefabs', 'assets/prefabs')), '.prefab.toml'))
                features_used.update(document.get('scene', {}).get('features', []))
                if document.get('lights') or document.get('environment'):
                    features_used.add('lighting_3d')
            for entity in document.get('entities', []):
                if is_3d:
                    features_used.update(key for key in entity if key in ('skinned', 'material', 'effect'))
                    if 'grass' in entity:
                        features_used.add('meshes_3d')
                    if 'material' in entity:
                        features_used.add('materials')
                    if 'effect' in entity:
                        features_used.add('effects')
                for key, value in entity.items():
                    if isinstance(value, (dict, list)) and (isinstance(value, dict) or value and isinstance(value[0], dict)) and key not in allowed_entity_tables:
                        reject('WEB_SHADER_UNSUPPORTED' if key in ('material','effect','ui_effect') else 'WEB_FEATURE_UNSUPPORTED', 'Unsupported component ' + key + ' in ' + path.name)
                if is_3d and 'effect' in entity and entity['effect'].get('name', '') not in ('plasma', 'molten'):
                    reject('WEB_FEATURE_UNSUPPORTED', 'Unsupported browser entity effect in ' + path.name)
                if not is_3d and 'camera' in entity and entity['camera'].get('projection', 'perspective') != 'orthographic':
                    reject('WEB_FEATURE_UNSUPPORTED', 'Only orthographic cameras are supported')
                for widget in ('ui_text', 'ui_button'):
                    value=entity.get(widget,{})
                    if value.get('font','Roboto') != 'Roboto' or any(ord(char)>255 for char in value.get('text',value.get('label',''))):
                        reject('WEB_FONT_UNSUPPORTED','Browser UI requires packaged Roboto and Latin-1 glyphs')
                if not is_3d and path.name.endswith('.prefab.toml') and entity.get('scripts'):
                    reject('WEB_FEATURE_UNSUPPORTED', 'Scripted browser prefabs are not implemented')
        elif path.suffix == '.toml':
            document = tomllib.loads(path.read_text(encoding='utf-8-sig'))
        else:
            document = None
        if document is not None:
            if is_3d and path.name.endswith('.material.toml'):
                shader = document.get('material', {}).get('shader', '')
                if shader.startswith('shaders://') and shader not in ('shaders://gltf_mesh.spv', 'shaders://plasma.spv', 'shaders://molten.spv') and 'graph' not in document:
                    visit(project / 'assets/shaders' / (Path(shader.removeprefix('shaders://')).stem + '.web.slangh'))
            for value in strings(document):
                if value.startswith(('project://', 'uid://')):
                    asset(value)
        elif path.suffix in ('.tiles', '.tilemap'):
            blob = path.read_bytes()
            if len(blob) < 12 or blob[:4] != b'ATLM':
                reject('WEB_ASSET_FORMAT', 'Invalid ATLM header: ' + path.name)
            version, length = struct.unpack_from('<II', blob, 4)
            if version not in (1, 2) or length > 4096 or len(blob) < 12 + length:
                reject('WEB_ASSET_FORMAT', 'Unsupported/truncated ATLM header: ' + path.name)
            asset(blob[12:12 + length].decode('utf-8'))
    scene_dir = local(paths.get('scenes', 'scenes'))
    for scene in scenes:
        path = scene_path(scene, scene_dir, '.scene.toml')
        if not path.is_file():
            reject('WEB_SCENE_MISSING', 'Missing declared/reachable scene: ' + scene)
        visit(path)
    prefabs = local(paths.get('prefabs', 'assets/prefabs'))
    prefab_calls = r'\.instantiate_prefab\s*\(\s*"([^"\\]+)"' if language == 'rust' else r'\bScene\s*\.\s*Instantiate\s*\(\s*"([^"\\]+)"'
    for name in re.findall(prefab_calls, source):
        visit(scene_path(name, prefabs, '.prefab.toml'))
    # Enumeration patterns and write destinations are not read dependencies.
    non_read_uri_spans = {
        match.span(1) for match in re.finditer(
            r'\bAssets\s*\.\s*(?:List|WriteText)\s*\(\s*"((?:project|uid)://[^"\\]+)"',
            source)
    } if language == 'csharp' else set()
    for match in re.finditer(r'"((?:project|uid)://[^"\\]+)"', source):
        if match.span(1) in non_read_uri_spans:
            continue
        uri = match.group(1)
        asset(uri)
    if is_3d:
        # Custom-pass APIs name shaders by stem rather than project URI.
        shader_dir = project / 'assets/shaders'
        for stem in set(re.findall(r'["\']([A-Za-z0-9_/-]+)["\']', source)):
            shader = (shader_dir / (stem + '.slang')).resolve()
            if shader.is_relative_to(shader_dir.resolve()) and shader.is_file():
                visit(shader)
        registration = (r'\.register_custom_pass\s*\(\s*"[^"]+"\s*,\s*"([^"]+)"' if language == 'rust'
                        else r'\bCustomPass\s*\.\s*Register\s*\(\s*[^,]+,\s*"([^"]+)"')
        for stem in re.findall(registration, source):
            visit(shader_dir / (stem + '.slang'))
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
                profile=profile, scenes=sorted(scenes), startup=startup,
                startup_path=scene_path(startup, scene_dir, '.scene.toml'), files=files, features_used=sorted(features_used))


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
        report['profile'] = data['profile']
        if data['profile'] == 'webgpu-3d-v1':
            ledger = json.loads((REPO / 'tools/project/web_feature_coverage.json').read_text(encoding='utf-8'))
            graphics = data['settings'].get('graphics', {})
            report['coverage'] = dict(featuresUsed=data['features_used'],
                authoredGraphicsSettings=graphics,
                unqualified=[dict(feature=name, **ledger[name]) for name in data['features_used'] if name in ledger],
                unqualifiedSettings=[dict(setting='graphics.' + name, value=value,
                    ledger=ledger['effects']['ledger'], qualification='Not individually range-qualified by this export')
                    for name, value in sorted(graphics.items())])
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
        if data['profile'] == 'webgpu-3d-v1':
            (inputs / 'assets').mkdir(exist_ok=True)
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
            '-DAETHER_WEB_STAGED_PROJECT=' + str(inputs), '-DAETHER_WEB_ASSETPACKER=' + str(packer), '-DAETHER_WEB_SLANGC=' + slang,
            '-DAETHER_WEB_PROFILE=' + data['profile']]
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
            names += ['browser-main.mjs', 'sprite.wgsl', 'mesh.wgsl', 'hdr.wgsl', 'light2d.wgsl', 'occluder2d.wgsl', 'billboard.wgsl', 'exposure.wgsl']
            names += ['post-'+name+'.wgsl' for name in ('bloom', 'fxaa', 'tonemap', 'gtao_main', 'gtao_denoise', 'ssr', 'ssr_composite', 'dof', 'volumetric_fog', 'volumetric_composite')]
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
        config = dict(schemaVersion=1, id=identifier, name=product, language=data['language'], startupScene=data['startup'], scenes=data['scenes'], profile=data['profile'],
            paths=data['settings'].get('paths', {}), graphics=graphics_for_web(data['settings'].get('graphics', {})), instructions=data['settings'].get('publish', {}).get('web', {}).get('instructions', 'Select Play to use the keyboard.'))
        (stage / 'project.js').write_text('globalThis.aetherProject=' + json.dumps(config, ensure_ascii=True) + ';\n', encoding='utf-8')
        revision = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=REPO, capture_output=True, text=True, check=True).stdout.strip()
        files = [dict(path=path.relative_to(stage).as_posix(), sizeBytes=path.stat().st_size, sha256=hashlib.sha256(path.read_bytes()).hexdigest()) for path in sorted(stage.rglob('*')) if path.is_file()]
        manifest = dict(schemaVersion=1, target='web', profile=data['profile'], language=data['language'], engineRevision=revision,
            project=dict(id=identifier, name=product, startupScene=data['startup'], scenes=data['scenes']), toolchains=pins, files=files)
        if data['profile'] == 'webgpu-3d-v1':
            requirements = (REPO / 'src/engine/webgpu/WebGpuRequirements.inc').read_text(encoding='utf-8')
            manifest['adapterRequirements'] = dict(
                limits={name: int(value) for name, value in re.findall(r'AE_WEBGPU_LIMIT\((\w+),\s*(\d+)\)', requirements)},
                features=re.findall(r'AE_WEBGPU_FEATURE\(([\w-]+)\)', requirements),
                textureFormats=re.findall(r'AE_WEBGPU_FORMAT\(([\w-]+)\)', requirements))
            scene_file = inputs / data['startup_path'].relative_to(project)
            manifest['project']['startupSceneKind'] = tomllib.loads(scene_file.read_text(encoding='utf-8-sig')).get('scene', {}).get('kind', '3d')
            manifest['cookedAssets'] = {
                'meshes': sorted(path.relative_to(inputs).as_posix() for path in inputs.rglob('*.mesh')),
                'shaders': sorted(path.relative_to(inputs).as_posix() for path in inputs.rglob('*.wshader'))}
            manifest['gameplayInputs'] = {
                path.relative_to(data['scripts']).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(data['scripts'].rglob('*'))
                if path.is_file() and path.suffix.lower() in ('.rs', '.cs', '.toml', '.csproj')
                and not any(part in ('obj', 'bin', 'artifacts') for part in path.relative_to(data['scripts']).parts)}
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
        verify_command = ['node', REPO / 'tools/browser-tests/verify-export.mjs', '--root=' + str(relocated), '--report=' + str(verification)]
        if data['profile'] == 'webgpu-3d-v1':
            verify_command.append('--zipBytes=' + str(package.stat().st_size))
        run('verify', verify_command)
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
