"""Run the authored material controls on native Vulkan and hardware WebGPU."""
from pathlib import Path
import json
import runpy
import sys
import tempfile

from material_effect_oracles import GRAPHICS, SCENES


def main():
    root = Path(__file__).resolve().parents[2]
    scratch = root / '.temp'
    scratch.mkdir(exist_ok=True)
    original = sys.argv
    try:
        with tempfile.TemporaryDirectory(prefix='material-parity-', dir=scratch) as directory:
            graphics = Path(directory) / 'graphics.json'
            graphics.write_text(json.dumps(GRAPHICS), encoding='utf-8')
            helper = Path(__file__).with_name('lighting-parity-checks.py')
            sys.argv = [str(helper), '--scenes', ','.join(SCENES), '--graphics-json', str(graphics)] + original[1:]
            runpy.run_path(str(helper), run_name='__main__')
    finally:
        sys.argv = original


if __name__ == '__main__':
    main()
