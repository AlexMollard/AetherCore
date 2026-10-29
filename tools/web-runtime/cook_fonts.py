"""Bake staged browser fonts with the same host FontProcessor as native builds."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('packer', type=Path)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    if not args.directory.exists():
        return
    for font in sorted(args.directory.rglob('*')):
        if font.is_file() and font.suffix.lower() in ('.ttf', '.otf'):
            subprocess.run([str(args.packer), 'bake-font', str(font), str(font.parent)], check=True)


if __name__ == '__main__':
    main()
