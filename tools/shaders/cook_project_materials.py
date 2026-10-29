"""Cook custom material shaders in a disposable staged Web project before packing."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import shutil
import tomllib
from web_materials import cook, cook_custom_pass, cook_ui_shader, custom_pass_stem, resolve_compiler, shader_output_name


BUILT_INS = {'', 'shaders://gltf_mesh.spv', 'shaders://plasma.spv', 'shaders://molten.spv'}


def custom_materials(project):
    """Find explicit custom shaders; duplicate output ownership is an error."""
    claims = {}
    result = []
    for material in sorted((project/'assets').rglob('*.toml')):
        if material.stat().st_size > 256*1024:
            if material.name.endswith('.material.toml'):
                raise ValueError('WEB_SHADER_SOURCE_BUDGET: '+str(material))
            continue
        document = tomllib.loads(material.read_text(encoding='utf-8-sig'))
        properties = document.get('material', document)
        uri = properties.get('shader', '')
        if uri in BUILT_INS:
            if 'graph' in document:
                raise ValueError('WEB_SHADER_GRAPH_URI: graph must declare an explicit custom shader URI: '+str(material))
            continue
        if not isinstance(uri, str):
            raise ValueError('WEB_SHADER_URI: material shader must be a string: '+str(material))
        if uri in claims:
            raise ValueError('WEB_SHADER_DUPLICATE: '+uri+' is claimed by '+str(claims[uri])+' and '+str(material))
        claims[uri] = material
        result.append(material)
    return result


EXCLUDED_DIRECTORIES = {'build', 'builds', 'obj', 'bin', 'out', 'intermediate', 'intermediates', 'cache'}


def ui_shader_sources(project):
    """Find explicitly authored UI shader roles in scenes and prefabs."""
    shaders = (project/'assets/shaders').resolve()
    claims = set()
    for root in (project/'scenes', project/'assets'):
        if not root.is_dir():
            continue
        for path in root.rglob('*.toml'):
            if not path.name.endswith(('.scene.toml', '.prefab.toml')):
                continue
            document = tomllib.loads(path.read_text(encoding='utf-8-sig'))
            for entity in document.get('entities', []):
                for table, role in (('ui_material', 'material'), ('ui_effect', 'effect')):
                    name = entity.get(table, {}).get('shader', '')
                    if not name:
                        continue
                    if not isinstance(name, str):
                        raise ValueError('WEB_UI_SOURCE: shader must be a string in '+str(path))
                    source = (shaders/(name+'.slang')).resolve()
                    if not source.is_relative_to(shaders) or not source.is_file():
                        raise ValueError('WEB_UI_SOURCE: missing or invalid shader '+name+' in '+str(path))
                    claims.add((source, role))
    manifest = project/'.web-ui-roles.json'
    if manifest.is_file():
        for item in json.loads(manifest.read_text(encoding='utf-8')):
            source = (project/item['source']).resolve()
            role = item['role']
            if role not in ('material', 'effect') or not source.is_relative_to(shaders) or not source.is_file():
                raise ValueError('WEB_UI_SOURCE: invalid staged UI shader role')
            claims.add((source, role))
    return sorted(claims)


def custom_pass_sources(project, materials, ui_sources=()):
    """Find assets/shaders/**/*.slang that no custom material claims as its native source.

    Every one must be a portable custom pass: a source without the CustomPass.slangh include is
    reported by cook_custom_pass as WEB_CUSTOM_PASS_PORTABILITY instead of silently skipped."""
    shaders = project/'assets/shaders'
    if not shaders.is_dir():
        return []
    claimed = set()
    for material in materials:
        document = tomllib.loads(material.read_text(encoding='utf-8-sig'))
        uri = document.get('material', document).get('shader', '')
        claimed.add((shaders/(shader_output_name(uri)[:-len('.wshader')]+'.slang')).resolve())
    claimed.update(source for source, _ in ui_sources)
    result, stems = [], {}
    for source in sorted(shaders.rglob('*.slang')):
        parts = source.relative_to(shaders).parts[:-1]
        if any(part.startswith('.') or part.lower() in EXCLUDED_DIRECTORIES for part in parts) or source.resolve() in claimed:
            continue
        key = custom_pass_stem(project, source).lower()
        if key in stems:
            raise ValueError('WEB_CUSTOM_PASS_DUPLICATE: '+str(source)+' and '+str(stems[key])+' cook to the same shader stem')
        stems[key] = source
        result.append(source)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', required=True, type=Path)
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--slangc', type=Path, default=Path(shutil.which('slangc') or 'slangc'))
    parser.add_argument('--timeout', type=int, default=120, help='Deadline in seconds per material')
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    results, custom_passes, ui_shaders = [], [], []
    try:
        project = args.project.resolve()
        if not (project/'assets').is_dir():
            raise ValueError('WEB_SHADER_PROJECT: staged project has no assets directory')
        materials = custom_materials(project)
        ui_sources = ui_shader_sources(project)
        passes = custom_pass_sources(project, materials, ui_sources)
        compiler = resolve_compiler(args.compiler) if materials else None
        for material in materials:
            results.append(cook(project, material, project/'assets/shaders', compiler, args.slangc, args.timeout))
        for source in passes:
            custom_passes.append(cook_custom_pass(project, source, project/'assets/shaders', args.slangc, args.timeout))
        for source, role in ui_sources:
            ui_shaders.append(cook_ui_shader(project, source, project/'assets/shaders', args.slangc, role, args.timeout))
            if role == 'material':
                ui_shaders.append(cook_ui_shader(project, source, project/'assets/shaders', args.slangc, role, args.timeout, glyph=True))
        report = dict(schemaVersion=1, status='passed', project=str(project), materials=results, customPasses=custom_passes, uiShaders=ui_shaders,
                      qualification='Cook compilation only; browser execution and native visual parity are separate checks')
    except Exception as error:
        report = dict(schemaVersion=1, status='failed', project=str(args.project), materials=results, customPasses=custom_passes, uiShaders=ui_shaders,
                      diagnostics=[dict(code='WEB_MATERIAL_PROJECT_COOK', message=str(error))])
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(status=report['status'], report=str(args.report))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
