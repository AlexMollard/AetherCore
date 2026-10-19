"""Verify a meaningful shader-only edit repackages the runtime, with SDK env active."""
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[2]
shader=root/'resources/web/sprite.slang'
artifact=root/'build/webgpu-browser/web/AetherGame.data'
original=shader.read_bytes()
changed=original.replace(b'return color;',b'return color * 0.875;')
if changed==original:
    raise SystemExit('Shader fixture no longer contains the expected return expression')
def build():
    subprocess.run(['cmake','--build','--preset','webgpu-browser','-j','6'],cwd=root,check=True)
try:
    shader.write_bytes(changed)
    build()
    compiled=(root/'build/webgpu-browser/web-assets/sprite.wgsl').read_bytes()
    if compiled not in artifact.read_bytes():
        raise SystemExit('Shader was rebuilt without repackaging AetherGame.data')
    print('Meaningful shader-only edit repackaged AetherGame.data')
finally:
    shader.write_bytes(original)
    build()
