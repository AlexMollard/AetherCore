"""Create content-addressed browser streaming assets from preflight's dependency graph."""
import hashlib
import json
import shutil
import struct
import tomllib
from pathlib import Path


def emit(data, inputs, stage, packer, run):
    project = data['project']
    bootstrap = stage.parent / (stage.name + '-bootstrap')
    # FontRegistry resolves source font references to the native cooked sidecar.
    for original in list(data['files']):
        if original.suffix.lower() not in ('.ttf', '.otf'):
            continue
        cooked = original.with_suffix('.fontcurves')
        if not (inputs / cooked.relative_to(project)).is_file():
            raise ValueError('Missing cooked browser font: ' + str(cooked))
        data['files'].add(cooked)
        data['dependencies'].setdefault(original, set()).add(cooked)
        if original in data['bootstrap_files']:
            data['bootstrap_files'].add(cooked)
    # The browser loads baked meshes by replacing the source model extension.
    for original in list(data['files']):
        if original.suffix.lower() not in ('.gltf', '.glb'):
            continue
        cooked = original.with_suffix('.mesh')
        if not (inputs / cooked.relative_to(project)).is_file():
            raise ValueError('Missing cooked browser mesh: ' + str(cooked))
        data['files'].add(cooked)
        data['dependencies'].setdefault(original, set()).add(cooked)
        if original in data['bootstrap_files']:
            data['bootstrap_files'].add(cooked)
        blob = (inputs / cooked.relative_to(project)).read_bytes()
        if len(blob) < 72 or blob[:4] != b'MESH':
            raise ValueError('Invalid cooked browser mesh: ' + str(cooked))
        version, vertices, indices, skin_len, material_count, submeshes = struct.unpack_from('<6I', blob, 4)
        if version not in (3, 4) or blob[68] not in (0, 1):
            raise ValueError('Invalid cooked browser mesh header: ' + str(cooked))
        cursor = 72 + vertices * 96 + indices * (2 if blob[68] == 0 else 4) + submeshes * 12
        def read_string():
            nonlocal cursor
            if cursor + 2 > len(blob):
                raise ValueError('Truncated cooked mesh strings: ' + str(cooked))
            length = struct.unpack_from('<H', blob, cursor)[0]
            cursor += 2
            if cursor + length > len(blob):
                raise ValueError('Truncated cooked mesh string: ' + str(cooked))
            value = blob[cursor:cursor + length].decode('utf-8')
            cursor += length
            return value
        if version >= 4:
            for _ in range(submeshes):
                read_string()
        if cursor + skin_len > len(blob):
            raise ValueError('Truncated cooked mesh skin: ' + str(cooked))
        skin = blob[cursor:cursor + skin_len].decode('utf-8')
        cursor += skin_len
        paths = [skin] if skin else []
        paths.extend(read_string() for _ in range(material_count))
        def add_cooked(path):
            path = path.resolve()
            if not path.is_relative_to(project) or not (inputs / path.relative_to(project)).is_file():
                raise ValueError('Missing cooked mesh dependency: ' + str(path))
            data['files'].add(path)
            data['dependencies'].setdefault(cooked, set()).add(path)
            if original in data['bootstrap_files']:
                data['bootstrap_files'].add(path)
        for reference in paths:
            if reference.startswith('project://'):
                relative = reference.removeprefix('project://')
                path = project / relative
            elif '://' in reference:
                raise ValueError('Unsupported cooked mesh dependency: ' + reference)
            elif '..' in reference:
                path = cooked.parent / reference
            else:
                path = project / reference
            add_cooked(path)
        animset = cooked.with_suffix('.animset')
        animset_input = inputs / animset.relative_to(project)
        if animset_input.is_file():
            add_cooked(animset)
            content = animset_input.read_bytes()
            if len(content) < 24 or content[:4] != b'ASET' or struct.unpack_from('<I', content, 4)[0] != 1:
                raise ValueError('Invalid cooked animation set: ' + str(animset))
            count = struct.unpack_from('<I', content, 8)[0]
            position = 24
            for _ in range(count):
                if position + 2 > len(content):
                    raise ValueError('Truncated cooked animation set: ' + str(animset))
                length = struct.unpack_from('<H', content, position)[0]
                position += 2
                if position + length > len(content):
                    raise ValueError('Truncated cooked animation path: ' + str(animset))
                add_cooked(project / content[position:position + length].decode('utf-8'))
                position += length
    for original, roles in data.get('ui_shader_sources', {}).items():
        variants = {'.ui-' + role + '.wshader' for role in roles}
        if 'material' in roles:
            variants.add('.ui-material-glyph.wshader')
        for suffix in variants:
            cooked = original.with_suffix(suffix)
            if not (inputs / cooked.relative_to(project)).is_file():
                raise ValueError('Missing cooked browser UI shader: ' + str(cooked))
            data['files'].add(cooked)
            data['dependencies'].setdefault(original, set()).add(cooked)
            if original in data['bootstrap_files']:
                data['bootstrap_files'].add(cooked)
    for original in list(data['files']):
        cooked = None
        if original.name.endswith('.material.toml'):
            document = tomllib.loads(original.read_text(encoding='utf-8-sig'))
            shader = document.get('material', document).get('shader', '')
            if shader.startswith('shaders://') and shader not in ('shaders://gltf_mesh.spv', 'shaders://plasma.spv', 'shaders://molten.spv'):
                cooked = project / 'assets/shaders' / (shader.removeprefix('shaders://').removesuffix('.spv') + '.wshader')
        elif original.suffix == '.slang' and original not in data.get('ui_shader_sources', {}):
            cooked = original.with_suffix('.wshader')
        if cooked is not None:
            if not (inputs / cooked.relative_to(project)).is_file():
                raise ValueError('Missing cooked browser shader: ' + str(cooked))
            data['files'].add(cooked)
            data['dependencies'].setdefault(original, set()).add(cooked)
            if original in data['bootstrap_files']:
                data['bootstrap_files'].add(cooked)
    bootstrap.mkdir()
    if data['profile'] == 'webgpu-3d-v1':
        (bootstrap / 'assets').mkdir(exist_ok=True)
    try:
        for original in data['bootstrap_files'] | {project / 'ProjectSettings.toml'}:
            relative = original.relative_to(project)
            source = inputs / relative
            if source.is_file():
                target = bootstrap / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        run('stream-bootstrap', [packer, '--project', '--target', data['profile'], bootstrap, stage / 'game.pak'])
    finally:
        shutil.rmtree(bootstrap)
    assets = {}
    for original in sorted(data['files']):
        relative = original.relative_to(project).as_posix()
        source = inputs / relative
        content = source.read_bytes()
        if len(content) > 64 * 1024 * 1024:
            raise ValueError('Streaming asset exceeds 64 MiB: ' + relative)
        digest = hashlib.sha256(content).hexdigest()
        blob = 'streaming/' + digest
        target = stage / blob
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            target.write_bytes(content)
        dependencies = ['project://' + child.relative_to(project).as_posix()
                        for child in sorted(data['dependencies'].get(original, set()))]
        assets['project://' + relative] = dict(url=blob, size=len(content), sha256=digest, dependencies=dependencies)
    aliases = {'uid://' + uid: 'project://' + path for uid, path in data['identities'].items()}
    for reference in data['region_references']:
        if reference.startswith('uid://'):
            path = aliases.get(reference)
        elif reference.startswith('project://'):
            path = reference
        else:
            path = 'project://' + (data['scene_directory'] / (reference + '.scene.toml')).relative_to(project).as_posix()
        if path not in assets or project / path.removeprefix('project://') in data['bootstrap_files']:
            raise ValueError('Streaming region must not be part of bootstrap: ' + reference)
        aliases[reference] = path
    (stage / 'streaming-manifest.json').write_text(json.dumps(dict(schemaVersion=1, assets=assets, aliases=aliases), separators=(',', ':')), encoding='utf-8')
