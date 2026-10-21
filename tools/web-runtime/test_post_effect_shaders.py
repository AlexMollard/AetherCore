"""Compile the actual native effect bodies through their portable resource adapter."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
EFFECTS = {
    'bloom': ('downsampleMain', 'upsampleMain'),
    'fxaa': ('fragmentMain',),
    'tonemap': ('fragmentMain',),
    'gtao_main': ('fragmentMain',),
    'gtao_denoise': ('fragmentMain',),
    'ssr': ('fragmentMain',),
    'ssr_composite': ('fragmentMain',),
    'dof': ('fragmentMain',),
    'volumetric_fog': ('fragmentMain',),
    'volumetric_composite': ('fragmentMain',),
}


class PostEffectShaders(unittest.TestCase):
    def test_native_effect_bodies_compile_with_explicit_web_bindings(self):
        compiler = os.environ.get('AETHER_WEB_SLANGC') or shutil.which('slangc')
        if not compiler:
            self.skipTest('Set AETHER_WEB_SLANGC to run the pinned Slang compiler')
        with tempfile.TemporaryDirectory() as directory:
            for name, entries in EFFECTS.items():
                with self.subTest(effect=name):
                    output = Path(directory)/(name+'.wgsl')
                    command = [compiler, str(ROOT/'src/shaders'/(name+'.slang')),
                               '-D', 'AE_WEB_POST=1', '-target', 'wgsl', '-matrix-layout-column-major',
                               '-entry', 'vertexMain', '-stage', 'vertex']
                    for entry in entries:
                        command += ['-entry', entry, '-stage', 'fragment']
                    result = subprocess.run(command+['-o', str(output)], capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                    code = output.read_text(encoding='utf-8')
                    self.assertIn('@fragment', code)
                    # Slang can emit unsupported WGSL while still returning zero.
                    # These native BDA/bindless constructs are not WebGPU bindings.
                    for forbidden in ('uint64_t', 'ptr<,', 'array<texture_', 'ptr<physical'):
                        self.assertFalse(forbidden in code, name+' emitted unsupported WGSL: '+forbidden)
                    native = [compiler, str(ROOT/'src/shaders'/(name+'.slang')),
                              '-target', 'spirv', '-profile', 'glsl_460', '-entry', 'vertexMain']
                    for entry in entries:
                        native += ['-entry', entry]
                    spirv = output.with_suffix('.spv')
                    result = subprocess.run(native+['-o', str(spirv)], capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                    self.assertEqual(spirv.read_bytes()[:4], b'\x03\x02\x23\x07')


if __name__ == '__main__':
    unittest.main()
