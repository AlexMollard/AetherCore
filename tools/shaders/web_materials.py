"""Cook native material graphs or explicit portable surface adapters to WebGPU.

The production 3D export gate is unchanged. Outputs use the bounded AEWG ABI 1
container, and failed compilation never replaces the previous working shader.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import time
import tomllib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wgsl_checks import validate_negated_vector_sums

ROOT = Path(__file__).resolve().parents[2]
MAX_SHADER_BYTES = 1024 * 1024


def resolve_compiler(explicit=None, repo=ROOT):
    """Resolve the host tool lazily, including multi- and single-config builds."""
    sys.path.insert(0, str(ROOT/'tools/web-runtime'))
    from audit import validate_host_binary
    override = explicit or os.environ.get('AETHER_WEB_MATERIAL_COMPILER')
    if override:
        path = Path(override).resolve()
        validate_host_binary(path)
        return path
    repo = Path(repo).resolve()
    bases = [Path(os.environ['AETHER_BUILD_DIR'])] if os.environ.get('AETHER_BUILD_DIR') else []
    bases += [repo/name for name in ('build/default', 'build/vs2022-msvc', 'build/ninja-clang', 'build/linux-clang')]
    bases += sorted((repo/'build').glob('*'))
    executable = 'WebMaterialCompiler.exe' if os.name == 'nt' else 'WebMaterialCompiler'
    candidates = [(base if base.is_absolute() else repo/base)/'tools/shaders'/configuration/executable
                  for base in bases for configuration in ('RelWithDebInfo', 'Debug', 'Release', '')]
    for path in dict.fromkeys(candidates):
        if path.is_file():
            try:
                validate_host_binary(path)
                return path
            except ValueError:
                continue
    raise ValueError('WEB_SHADER_TOOLCHAIN: build native WebMaterialCompiler with CMake or pass --compiler; no native WebMaterialCompiler was found')


def shader_output_name(uri):
    if not isinstance(uri, str) or not uri.startswith('shaders://') or not uri.endswith('.spv'):
        raise ValueError('WEB_SHADER_PATH: expected an authored shaders://relative/name.spv URI')
    name = uri[len('shaders://'):]
    if any(part in ('', '.', '..') for part in name.split('/')) or any(c in name for c in '\\:\x00'):
        raise ValueError('WEB_SHADER_PATH: shader name escapes the project shader directory')
    return name[:-4]+'.wshader'


def validate_surface(source):
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    forbidden = r'\b(?:DevicePtr|uint64_t|uint64|ByteAddressBuffer|RWByteAddressBuffer|StructuredBuffer|RWStructuredBuffer|Texture\w*|RWTexture\w*|SamplerState|SamplerComparisonState|discard|import)\b|\[\[\s*vk\s*::\s*(?:binding|push_constant)|\bregister\s*\('
    match = re.search(forbidden, source)
    if match:
        line = source[:match.start()].count('\n')+1
        raise ValueError('WEB_SHADER_PORTABILITY: unsupported address/resource/control feature '+match.group()+
                         ' at adapter line '+str(line)+'; port to GraphSurface EvaluateGraphSurface(GraphInput) using the five GraphSampleTexture slots and frame inputs')


def validate_graph(graph):
    nodes, links = graph.get('node', {}), graph.get('link', {})
    if not isinstance(nodes, dict) or not isinstance(links, dict) or len(nodes) > 256 or len(links) > 1024:
        raise ValueError('WEB_SHADER_GRAPH_BUDGET: expected indexed graph tables within 256 nodes/1024 links')
    import math
    for name, node in nodes.items():
        for key, maximum in (('type', 26), ('slot', 4), ('channel', 3)):
            value = node.get(key, 0)
            if type(value) is not int or not 0 <= value <= maximum:
                raise ValueError('WEB_SHADER_GRAPH_NODE: node '+str(name)+' has unsupported '+key)
        values = node.get('value', [0, 0, 0, 1])
        if not isinstance(values, list) or len(values) != 4 or any(type(v) not in (int, float) or not math.isfinite(v) for v in values):
            raise ValueError('WEB_SHADER_GRAPH_NODE: node '+str(name)+' needs four finite values')


def encode_shader(wgsl, source_hash):
    if not 0 < len(wgsl) <= MAX_SHADER_BYTES or len(source_hash) != 32 or b'\0' in wgsl:
        raise ValueError('WEB_SHADER_BUDGET: shader exceeds the 1 MiB source budget or has invalid bytes')
    text = wgsl.decode('utf-8')
    if '@vertex' not in text or '@fragment' not in text or 'vertexMain' not in text or 'fragmentMain' not in text:
        raise ValueError('WEB_SHADER_WGSL: compiler did not produce the vertex/fragment WGSL contract')
    checksum = 1469598103934665603
    for byte in wgsl:
        checksum = ((checksum ^ byte)*1099511628211) & ((1<<64)-1)
    return struct.pack('<4sIIIQ', b'AEWG', 1, len(wgsl), 0, checksum)+hashlib.sha256(wgsl).digest()+source_hash+wgsl


def validate_bindings(wgsl):
    # Check the compiler output, not merely source spelling: adapters may not
    # extend the renderer's fixed read-only resource layout through aliases.
    for declaration in re.findall(r'(?:@(?:binding|group)\(\d+\)\s*){2}var[^;]*;', wgsl):
        group = int(re.search(r'@group\((\d+)\)', declaration).group(1))
        binding = int(re.search(r'@binding\((\d+)\)', declaration).group(1))
        if group not in (0, 1, 2) or binding > ({0: 6, 1: 5, 2: 4}.get(group, -1)) or 'read_write' in declaration:
            raise ValueError('WEB_SHADER_BINDINGS: compiled shader extends the fixed read-only material resource ABI')


def run(command, log, deadline):
    sys.path.insert(0, str(ROOT/'tools/project'))
    from gameplay_jobs import _OwnedProcess
    with log.open('wb') as stream:
        worker = _OwnedProcess([str(part) for part in command], ROOT, stream)
        try:
            while worker.poll() is None:
                if time.monotonic() >= deadline:
                    raise TimeoutError('WEB_SHADER_TIMEOUT: bounded shader compilation exceeded its deadline')
                time.sleep(.025)
            if worker.poll() != 0:
                raise RuntimeError('WEB_SHADER_COMPILE: '+log.read_text(encoding='utf-8', errors='replace')[:16000])
        finally:
            worker.close()


def cook(project, material, output_dir, compiler, slang, timeout=120):
    project, material, output_dir = project.resolve(), material.resolve(), output_dir.resolve()
    if not material.is_relative_to(project) or not material.is_file() or material.stat().st_size > 256*1024:
        raise ValueError('WEB_SHADER_SOURCE: material must be a bounded source file within the selected project')
    if not compiler.is_file() or not slang.is_file():
        raise ValueError('WEB_SHADER_TOOLCHAIN: build WebMaterialCompiler and install the pinned Slang compiler explicitly')
    if not 1 <= timeout <= 600:
        raise ValueError('WEB_SHADER_TIMEOUT: timeout must be 1..600 seconds')
    source = tomllib.loads(material.read_text(encoding='utf-8-sig'))
    properties = source.get('material', source)
    uri = properties.get('shader') or 'shaders://'+material.name.removesuffix('.material.toml')+'.spv'
    output = output_dir/shader_output_name(uri)
    output.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic()+timeout
    with tempfile.TemporaryDirectory(prefix='.web-shader-', dir=output.parent) as directory:
        scratch = Path(directory)
        version_log = scratch/'slang-version.log'
        run([slang, '-version'], version_log, deadline)
        pin = json.loads((ROOT/'tools/experiments/rust-webgpu/toolchain.json').read_text())['slang']
        version = version_log.read_text(encoding='utf-8', errors='replace').strip()
        if pin not in version:
            raise ValueError('WEB_SHADER_TOOLCHAIN: expected Slang '+pin+'; got '+version)
        header = scratch/'aether-web-material.slangh'
        dependencies = [material]
        if 'graph' in source:
            validate_graph(source['graph'])
            run([compiler, material, header], scratch/'graph.log', deadline)
        else:
            adapter = project/'assets/shaders'/shader_output_name(uri).replace('.wshader', '.web.slangh')
            if not adapter.is_file() or adapter.stat().st_size > 256*1024:
                raise ValueError('WEB_SHADER_PORTABILITY: '+uri+' needs an explicit '+str(adapter)+
                                 ' surface adapter; native BDA/SPIR-V shaders are never substituted with the default material')
            text = adapter.read_text(encoding='utf-8-sig')
            validate_surface(text)
            if '#include' in text:
                raise ValueError('WEB_SHADER_PORTABILITY: adapter includes require an explicit reviewed portable dependency closure; put the surface math in the adapter')
            if not re.search(r'\bGraphSurface\s+EvaluateGraphSurface\s*\(', text):
                raise ValueError('WEB_SHADER_CONTRACT: adapter must define GraphSurface EvaluateGraphSurface(GraphInput input)')
            header.write_text('#include "material-graph-contract.slangh"\n'+text, encoding='utf-8')
            dependencies.append(adapter)
        with header.open('a', encoding='utf-8') as stream:
            stream.write('\n#include "material-graph-lighting.slangh"\n')
        wrapper = scratch/'material.slang'
        wrapper.write_text('#define AE_WEB_CUSTOM_MATERIAL 1\n#include "mesh.slang"\n', encoding='utf-8')
        dependencies.extend(sorted((ROOT/'resources/web').glob('*.slang*')))
        dependencies.extend(sorted((ROOT/'src/shaders/include').rglob('*.slangh')))
        input_hash = hashlib.sha256()
        hashes = []
        for path in sorted(set(dependencies)):
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            name = path.relative_to(project).as_posix() if path.is_relative_to(project) else 'engine/'+path.relative_to(ROOT).as_posix()
            input_hash.update(name.encode('utf-8')+b'\0'+bytes.fromhex(digest))
            hashes.append(dict(path=name, sha256=digest))
        input_hash.update(hashlib.sha256(compiler.read_bytes()).digest())
        wgsl = scratch/'material.wgsl'
        reflection = scratch/'reflection.json'
        run([slang, wrapper, '-target', 'wgsl', '-entry', 'vertexMain', '-entry', 'fragmentMain', '-entry', 'shadowMain', '-entry', 'auxiliaryMain',
             '-I', scratch, '-I', ROOT/'resources/web', '-reflection-json', reflection, '-o', wgsl], scratch/'slang.log', deadline)
        validate_bindings(wgsl.read_text(encoding='utf-8'))
        validate_negated_vector_sums(wgsl.read_text(encoding='utf-8'), str(material))
        encoded = encode_shader(wgsl.read_bytes(), input_hash.digest())
        metadata = dict(schemaVersion=1, abi=1, status='passed', material=material.relative_to(project).as_posix(),
                        shader=uri, sourceSha256=input_hash.hexdigest(), wgslSha256=hashlib.sha256(wgsl.read_bytes()).hexdigest(),
                        containerSha256=hashlib.sha256(encoded).hexdigest(), bytes=len(encoded), slang=version,
                        compilerSha256=hashlib.sha256(compiler.read_bytes()).hexdigest(), dependencies=hashes,
                        qualification='Compiled portable material; GPU execution and native visual comparison are separate required checks')
        staged = scratch/'shader.wshader'
        staged.write_bytes(encoded)
        # Commit only a complete successfully compiled container. The previous one
        # remains intact on graph/compiler/Slang failures.
        os.replace(staged, output)
        output.with_suffix(output.suffix+'.json').write_text(json.dumps(metadata, indent=2)+'\n', encoding='utf-8')
        return dict(**metadata, output=str(output))


def cook_native(project, material, output_dir, compiler, slang, timeout=120):
    """Stage the same graph through the native compiler for visual qualification."""
    project, material, output_dir = project.resolve(), material.resolve(), output_dir.resolve()
    if not material.is_relative_to(project) or material.stat().st_size > 256*1024 or not 1 <= timeout <= 600:
        raise ValueError('NATIVE_GRAPH_SOURCE: expected a bounded project material and timeout')
    source = tomllib.loads(material.read_text(encoding='utf-8-sig'))
    validate_graph(source.get('graph', {}))
    uri = source.get('material', source).get('shader')
    output = output_dir/shader_output_name(uri).replace('.wshader', '.spv')
    output.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic()+timeout
    with tempfile.TemporaryDirectory(prefix='.native-graph-', dir=output.parent) as directory:
        scratch = Path(directory)
        version_log = scratch/'slang-version.log'
        run([slang, '-version'], version_log, deadline)
        version = version_log.read_text(encoding='utf-8', errors='replace').strip()
        pin = json.loads((ROOT/'tools/experiments/rust-webgpu/toolchain.json').read_text())['slang']
        if pin not in version:
            raise ValueError('NATIVE_GRAPH_TOOLCHAIN: expected pinned Slang '+pin+'; got '+version)
        generated = scratch/'material.slang'
        run([compiler, '--native', material, generated], scratch/'graph.log', deadline)
        binary = scratch/'material.spv'
        # Exact default AETHERCORE_SLANG_SHADER_ARGS from CMake/SlangShaders.cmake.
        run([slang, generated, '-target', 'spirv', '-emit-spirv-directly', '-fvk-use-scalar-layout',
             '-matrix-layout-column-major', '-I', ROOT/'src/shaders', '-o', binary], scratch/'slang.log', deadline)
        data = binary.read_bytes()
        if len(data) < 20 or data[:4] != bytes.fromhex('03022307'):
            raise ValueError('NATIVE_GRAPH_SPIRV: compiler did not produce a SPIR-V module')
        inspection = scratch/'entry-points.json'
        run([compiler, '--inspect', binary], inspection, deadline)
        entries = json.loads(inspection.read_text(encoding='utf-8-sig'))
        if entries.get('graphPrepassMain') is not True or entries.get('fragmentMain') is not True:
            raise ValueError('NATIVE_GRAPH_PREPASS: compiled graph module must expose both forward and graph prepass fragment entries')
        metadata = dict(output=str(output), sha256=hashlib.sha256(data).hexdigest(),
                        materialSha256=hashlib.sha256(material.read_bytes()).hexdigest(),
                        generatedSourceSha256=hashlib.sha256(generated.read_bytes()).hexdigest(),
                        compilerSha256=hashlib.sha256(compiler.read_bytes()).hexdigest(), slang=version,
                        slangSha256=hashlib.sha256(slang.read_bytes()).hexdigest(),
                        fragmentEntries=entries,
                        nativeIncludeSha256={path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                                             for path in sorted((ROOT/'src/shaders/include').rglob('*.slangh'))})
        os.replace(binary, output)
        return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--material', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, help='Native WebMaterialCompiler; discovered from configured host build trees when omitted')
    parser.add_argument('--slangc', type=Path, default=Path(shutil.which('slangc') or 'slangc'))
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--native-output-dir', type=Path, help='Also stage native SPIR-V from the same graph for private parity qualification')
    args = parser.parse_args()
    try:
        args.compiler = resolve_compiler(args.compiler)
        report = cook(args.project, args.material, args.output_dir, args.compiler, args.slangc, args.timeout)
        if args.native_output_dir:
            report['nativeQualificationShader'] = cook_native(args.project, args.material, args.native_output_dir, args.compiler, args.slangc, args.timeout)
    except Exception as error:
        report = dict(schemaVersion=1, status='failed', material=str(args.material),
                      diagnostics=[dict(code='WEB_SHADER_COOK', file=str(args.material), message=str(error))])
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(status=report['status'], report=str(args.report))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
