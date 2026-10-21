"""Cook custom material shaders in a disposable staged Web project before packing."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import shutil
import tomllib
from web_materials import cook, resolve_compiler


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', required=True, type=Path)
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--slangc', type=Path, default=Path(shutil.which('slangc') or 'slangc'))
    parser.add_argument('--timeout', type=int, default=120, help='Deadline in seconds per material')
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    results = []
    try:
        project = args.project.resolve()
        if not (project/'assets').is_dir():
            raise ValueError('WEB_SHADER_PROJECT: staged project has no assets directory')
        materials = custom_materials(project)
        compiler = resolve_compiler(args.compiler) if materials else None
        for material in materials:
            results.append(cook(project, material, project/'assets/shaders', compiler, args.slangc, args.timeout))
        report = dict(schemaVersion=1, status='passed', project=str(project), materials=results,
                      qualification='Cook compilation only; browser execution and native visual parity are separate checks')
    except Exception as error:
        report = dict(schemaVersion=1, status='failed', project=str(args.project), materials=results,
                      diagnostics=[dict(code='WEB_MATERIAL_PROJECT_COOK', message=str(error))])
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(status=report['status'], report=str(args.report))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
