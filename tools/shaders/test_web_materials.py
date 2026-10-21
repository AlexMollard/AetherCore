import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
import json
from web_materials import encode_shader, validate_surface, validate_graph, validate_bindings, shader_output_name
import web_materials


class WebMaterialCookTests(unittest.TestCase):
    def test_host_compiler_discovery_supports_native_single_config_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root/'build/linux-clang/tools/shaders'/('WebMaterialCompiler.exe' if web_materials.os.name == 'nt' else 'WebMaterialCompiler')
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b'\x7fELF'+bytes(32))
            with patch.dict(web_materials.os.environ, {}, clear=True):
                self.assertEqual(web_materials.resolve_compiler(repo=root), executable.resolve())
            executable.write_bytes(b'\0asm'+bytes(32))
            with patch.dict(web_materials.os.environ, {}, clear=True), self.assertRaisesRegex(ValueError, 'native WebMaterialCompiler'):
                web_materials.resolve_compiler(repo=root)

    def test_compile_failure_preserves_previous_working_container(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            material = root/'fixture.material.toml'
            material.write_text('[material]\nshader="shaders://Fixture.spv"\n[graph.node.0]\nid=1\ntype=0\n')
            output = root/'cooked/Fixture.wshader'
            output.parent.mkdir()
            output.write_bytes(b'previous verified shader')
            pin = json.loads((web_materials.ROOT/'tools/experiments/rust-webgpu/toolchain.json').read_text())['slang']
            def failing_run(command, log, deadline):
                if '-version' in command:
                    log.write_text(pin)
                elif '-target' not in command:
                    Path(command[-1]).write_text('GraphSurface EvaluateGraphSurface(GraphInput input) { return GraphSurface(); }')
                else:
                    raise RuntimeError('shader compile failed')
            with patch.object(web_materials, 'run', failing_run), self.assertRaisesRegex(RuntimeError, 'compile failed'):
                web_materials.cook(root, material, output.parent, Path(__file__), Path(__file__))
            self.assertEqual(output.read_bytes(), b'previous verified shader')

    def test_compiled_bindings_cannot_extend_runtime_layout(self):
        validate_bindings('@binding(5) @group(1) var x: texture_2d<f32>;')
        for source in ('@binding(6) @group(1) var x: texture_2d<f32>;', '@binding(0) @group(3) var x: sampler;', '@group(0) @binding(0) var<storage, read_write> x: array<f32>;'):
            with self.assertRaisesRegex(ValueError, 'WEB_SHADER_BINDINGS'):
                validate_bindings(source)

    def test_invalid_serialized_nodes_are_not_silently_dropped_or_clamped(self):
        for key, value in (('type', 99), ('slot', 8), ('channel', -1), ('value', [float('nan'), 0, 0, 1])):
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'WEB_SHADER_GRAPH_NODE'):
                validate_graph({'node': {'0': {key: value}}})

    def test_native_addresses_and_extra_resources_require_an_actionable_port(self):
        for source in ('DevicePtr<float4> data;', '[[vk::binding(4,3)]] Texture2D extra;'):
            with self.assertRaisesRegex(ValueError, 'WEB_SHADER_PORTABILITY'):
                validate_surface(source)

    def test_shader_paths_cannot_escape_the_project(self):
        self.assertEqual(shader_output_name('shaders://effects/Pulse.spv'), 'effects/Pulse.wshader')
        for path in ('shaders://../outside.spv', 'shaders:///absolute.spv', 'file://shader.spv'):
            with self.assertRaises(ValueError):
                shader_output_name(path)

    def test_cooked_shader_is_versioned_bounded_and_rejects_non_wgsl(self):
        with self.assertRaisesRegex(ValueError, 'WGSL'):
            encode_shader(b'not a shader', bytes(32))
        value = encode_shader(b'@vertex fn vertexMain() {}\n@fragment fn fragmentMain() {}', bytes(32))
        self.assertEqual(value[:4], b'AEWG')
        self.assertEqual(int.from_bytes(value[4:8], 'little'), 1)
        self.assertEqual(int.from_bytes(value[8:12], 'little'), len(value)-88)


if __name__ == '__main__':
    unittest.main()
