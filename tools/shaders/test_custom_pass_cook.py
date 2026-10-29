import json
import shutil
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
import cook_project_materials
import web_materials

PIN = json.loads((web_materials.ROOT/'tools/experiments/rust-webgpu/toolchain.json').read_text())['slang']
REAL_SLANGC = Path('C:/VulkanSDK/1.4.350.0/Bin/slangc.exe')
GOOD_WGSL = ('@binding(0) @group(0) var<uniform> g_customPassFrame_0 : Frame;\n'
             '@binding(1) @group(0) var<storage, read> g_customPassData_0 : array<vec4<f32>>;\n'
             '@vertex fn vertexMain() {}\n@fragment fn fragmentMain() {}\n')
PORTABLE = '''#include "include/CustomPass.slangh"
struct VSOutput { float4 position : SV_Position; };
[shader("vertex")]
VSOutput vertexMain(uint id : SV_VertexID)
{
    VSOutput o;
    const float2 c = float2(id & 1u, (id >> 1u) & 1u);
    o.position = mul(CustomPassViewProj(), float4(lerp(CustomPassParams().xy, CustomPassParams().zw, c), 0.0, 1.0));
    return o;
}
[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    float4 d = CustomPassDataCount() > 0u ? CustomPassData(0u) : float4(0, 0, 0, 0);
    if (d.x > 2.0) { discard; }
    return d + CustomPassColor0() * CustomPassElapsedTime();
}
'''


def fake_run(wgsl=GOOD_WGSL, fail=False):
    def run(command, log, deadline):
        if '-version' in command:
            log.write_text(PIN)
        elif fail:
            raise RuntimeError('shader compile failed')
        else:
            Path(command[command.index('-o')+1]).write_text(wgsl)
    return run


class CustomPassCookTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.project = Path(directory.name).resolve()
        self.shaders = self.project/'assets/shaders'
        self.shaders.mkdir(parents=True)

    def source(self, name, text):
        path = self.shaders/name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')
        return path

    def test_include_spellings_and_comments(self):
        for spelling in ('#include "include/CustomPass.slangh"', '#include <include/CustomPass.slangh>', '#include "CustomPass.slangh"', '  #  include "include/CustomPass.slangh"'):
            self.assertTrue(web_materials.includes_custom_pass_contract(spelling+'\n'), spelling)
        for text in ('// #include "include/CustomPass.slangh"\n', '/* \n#include "include/CustomPass.slangh"\n*/', 'float x; // #include "include/CustomPass.slangh"'):
            self.assertFalse(web_materials.includes_custom_pass_contract(text), text)

    def test_non_portable_shader_is_rejected_with_actionable_port_diagnostic(self):
        source = self.source('native.slang', '[[vk::push_constant]] struct P { DevicePtr<float4> d; } pc;\n')
        with self.assertRaises(ValueError) as error:
            web_materials.cook_custom_pass(self.project, source, self.shaders, Path(__file__))
        message = str(error.exception)
        self.assertTrue(message.startswith('WEB_CUSTOM_PASS_PORTABILITY:'))
        for text in ('native.slang', 'include/CustomPass.slangh', 'CustomPassViewProj', 'DevicePtr'):
            self.assertIn(text, message)

    def test_commented_include_does_not_make_a_shader_portable(self):
        source = self.source('commented.slang', '// #include "include/CustomPass.slangh"\n')
        with self.assertRaisesRegex(ValueError, 'WEB_CUSTOM_PASS_PORTABILITY'):
            web_materials.cook_custom_pass(self.project, source, self.shaders, Path(__file__))

    def test_discovery_skips_material_sources_build_directories_and_non_slang(self):
        (self.project/'assets/materials').mkdir()
        material = self.project/'assets/materials/m.material.toml'
        material.write_text('[material]\nshader="shaders://effects/Claimed.spv"\n', encoding='utf-8')
        claimed = self.source('effects/Claimed.slang', 'native material source')
        self.source('build/generated.slang', 'x')
        self.source('.cache/hidden.slang', 'x')
        self.source('notes.txt', 'x')
        wanted = self.source('fx/ink.slang', PORTABLE)
        materials = cook_project_materials.custom_materials(self.project)
        self.assertEqual(materials, [material])
        self.assertEqual(cook_project_materials.custom_pass_sources(self.project, materials), [wanted])
        self.assertNotIn(claimed, cook_project_materials.custom_pass_sources(self.project, materials))

    def test_duplicate_stems_are_rejected(self):
        self.source('a/ink.slang', PORTABLE)
        self.source('b/ink.slang', PORTABLE)
        with patch.object(cook_project_materials, 'custom_pass_stem', lambda project, source: 'ink'), \
                self.assertRaisesRegex(ValueError, 'WEB_CUSTOM_PASS_DUPLICATE'):
            cook_project_materials.custom_pass_sources(self.project, [])

    def test_project_without_shaders_needs_no_tools(self):
        shutil.rmtree(self.shaders)
        self.assertEqual(cook_project_materials.custom_pass_sources(self.project, []), [])

    def test_extra_bindings_are_rejected(self):
        web_materials.validate_custom_pass_bindings(GOOD_WGSL)
        bad = (GOOD_WGSL+'@binding(2) @group(0) var tex : texture_2d<f32>;\n',
               GOOD_WGSL+'@binding(0) @group(1) var<uniform> extra : vec4<f32>;\n',
               '@binding(1) @group(0) var<storage, read_write> d : array<vec4<f32>>;\n',
               '@binding(0) @group(0) var<storage, read> d : array<vec4<f32>>;\n',
               'var<push_constant> pc : vec4<f32>;\n')
        for wgsl in bad:
            with self.subTest(wgsl=wgsl), self.assertRaisesRegex(ValueError, 'WEB_CUSTOM_PASS_BINDINGS'):
                web_materials.validate_custom_pass_bindings(wgsl)

    def test_cook_writes_container_and_metadata_for_nested_stem(self):
        source = self.source('fx/ink.slang', PORTABLE)
        with patch.object(web_materials, 'run', fake_run()):
            result = web_materials.cook_custom_pass(self.project, source, self.shaders, Path(__file__))
        output = self.shaders/'fx/ink.wshader'
        self.assertEqual(Path(result['output']), output)
        self.assertEqual(output.read_bytes()[:4], b'AEWG')
        sidecar = json.loads(output.with_suffix('.wshader.json').read_text())
        self.assertEqual((sidecar['shader'], sidecar['source'], sidecar['slang']), ('fx/ink', 'assets/shaders/fx/ink.slang', PIN))
        self.assertIn('Compile-only', sidecar['qualification'])
        self.assertTrue(any(item['path'].startswith('engine/src/shaders/include/CustomPass.slangh') for item in sidecar['dependencies']))
        self.assertEqual(list(self.shaders.glob('.web-shader-*')), [])

    def test_failures_preserve_previous_artifact(self):
        source = self.source('ink.slang', PORTABLE)
        output = self.shaders/'ink.wshader'
        output.write_bytes(b'previous verified shader')
        with patch.object(web_materials, 'run', fake_run(fail=True)), self.assertRaisesRegex(RuntimeError, 'compile failed'):
            web_materials.cook_custom_pass(self.project, source, self.shaders, Path(__file__))
        extra = GOOD_WGSL+'@binding(2) @group(0) var tex : texture_2d<f32>;\n'
        with patch.object(web_materials, 'run', fake_run(extra)), self.assertRaisesRegex(ValueError, 'WEB_CUSTOM_PASS_BINDINGS'):
            web_materials.cook_custom_pass(self.project, source, self.shaders, Path(__file__))
        self.assertEqual(output.read_bytes(), b'previous verified shader')

    def test_wrong_slang_version_and_oversized_source_fail(self):
        source = self.source('ink.slang', PORTABLE)
        def old(command, log, deadline):
            log.write_text('0.0.0')
        with patch.object(web_materials, 'run', old), self.assertRaisesRegex(ValueError, 'WEB_SHADER_TOOLCHAIN'):
            web_materials.cook_custom_pass(self.project, source, self.shaders, Path(__file__))
        source.write_text(PORTABLE+'//'+'x'*web_materials.MAX_CUSTOM_PASS_SOURCE_BYTES)
        with self.assertRaisesRegex(ValueError, 'WEB_CUSTOM_PASS_SOURCE'):
            web_materials.cook_custom_pass(self.project, source, self.shaders, Path(__file__))

    @unittest.skipUnless(REAL_SLANGC.is_file(), 'pinned slangc is not installed')
    def test_real_slang_cooks_portable_shader_and_rejects_native_extras(self):
        source = self.source('fx/ink.slang', PORTABLE)
        result = web_materials.cook_custom_pass(self.project, source, self.shaders, REAL_SLANGC)
        wgsl = (self.shaders/'fx/ink.wshader').read_bytes()[88:].decode('utf-8')
        self.assertIn('discard', wgsl)
        self.assertEqual(result['shader'], 'fx/ink')
        text = PORTABLE.replace('#include "include/CustomPass.slangh"\n', '#include "include/CustomPass.slangh"\n[[vk::binding(2, 0)]] Texture2D g_extra;\n')
        extra = self.source('extra.slang', text.replace('return d +', 'return g_extra.Load(int3(0, 0, 0)) + d +'))
        with self.assertRaisesRegex(ValueError, 'WEB_CUSTOM_PASS_BINDINGS'):
            web_materials.cook_custom_pass(self.project, extra, self.shaders, REAL_SLANGC)
        self.assertFalse((self.shaders/'extra.wshader').exists())

    @unittest.skipUnless(REAL_SLANGC.is_file(), 'pinned slangc is not installed')
    def test_original_ui_sources_cook_in_distinct_roles(self):
        examples = web_materials.ROOT/'projects/INKBOUND/assets/shaders'
        for name, role in (('ui_ink_ui', 'material'), ('ui_glitch_text', 'material'), ('ui_void_bg', 'effect')):
            source = self.source(name+'.slang', (examples/(name+'.slang')).read_text(encoding='utf-8'))
            result = web_materials.cook_ui_shader(self.project, source, self.shaders, REAL_SLANGC, role)
            output = self.shaders/(name+'.ui-'+role+'.wshader')
            self.assertEqual(Path(result['output']), output)
            self.assertEqual(output.read_bytes()[:4], b'AEWG')
            if role == 'material':
                glyph = web_materials.cook_ui_shader(self.project, source, self.shaders, REAL_SLANGC, role, glyph=True)
                self.assertEqual(Path(glyph['output']), self.shaders/(name+'.ui-material-glyph.wshader'))
                self.assertEqual(Path(glyph['output']).read_bytes()[:4], b'AEWG')
                if name == 'ui_glitch_text':
                    wgsl = Path(glyph['output']).read_bytes()[88:].decode('utf-8')
                    for line in wgsl.splitlines():
                        if '@interpolate(' in line:
                            self.assertIn('@location(', line)

    def test_ui_shader_rejects_used_native_bindless_resource(self):
        source = self.source('native.slang', '''
[[vk::binding(0, 0)]] Texture2D g_textures[];
struct P { float4 screenSize; float4 rect; float4 params; float4 color0; float4 color1; };
[[vk::push_constant]] P pc;
float4 fragmentMain() : SV_Target0 { return g_textures[0].Load(int3(0, 0, 0)); }
''')
        with self.assertRaisesRegex(ValueError, 'WEB_UI_PORTABILITY.*native bindless resource'):
            web_materials.cook_ui_shader(self.project, source, self.shaders, Path(__file__), 'effect')
        self.assertFalse((self.shaders/'native.ui-effect.wshader').exists())

    def test_ui_bad_compiled_resource_preserves_previous_artifact(self):
        examples = web_materials.ROOT/'projects/INKBOUND/assets/shaders'
        source = self.source('ui_ink_ui.slang', (examples/'ui_ink_ui.slang').read_text(encoding='utf-8'))
        output = self.shaders/'ui_ink_ui.ui-material.wshader'
        output.write_bytes(b'previous')
        bad = '@binding(5) @group(0) var<storage, read_write> extra : array<vec4<f32>>;\n'
        with patch.object(web_materials, 'run', fake_run(bad)), self.assertRaisesRegex(ValueError, 'WEB_UI_BINDINGS'):
            web_materials.cook_ui_shader(self.project, source, self.shaders, Path(__file__), 'material')
        self.assertEqual(output.read_bytes(), b'previous')

    def test_ui_discovery_rejects_missing_shader_and_excludes_claimed_sources(self):
        scenes = self.project/'scenes'
        scenes.mkdir()
        scene = scenes/'Start.scene.toml'
        scene.write_text('[[entities]]\n[entities.ui_material]\nshader="ink"\n', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'WEB_UI_SOURCE'):
            cook_project_materials.ui_shader_sources(self.project)
        source = self.source('ink.slang', 'source')
        claims = cook_project_materials.ui_shader_sources(self.project)
        self.assertEqual(claims, [(source.resolve(), 'material')])
        self.assertEqual(cook_project_materials.custom_pass_sources(self.project, [], claims), [])


if __name__ == '__main__':
    unittest.main()
