"""Reject workflow trigger changes that could spend hosted minutes automatically."""
from pathlib import Path
import re


def check_triggers(text):
    lines = text.splitlines()
    blocks = [index for index, line in enumerate(lines)
              if re.match(r'^(?:on|"on"|\'on\'):', line)]
    if len(blocks) != 1 or not re.fullmatch(r'(?:on|"on"|\'on\'):\s*(?:#.*)?', lines[blocks[0]]):
        raise ValueError('Require one explicit block-style on: mapping')
    children = []
    for line in lines[blocks[0] + 1:]:
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        if not line[0].isspace():
            break
        if line.startswith('  ') and not line.startswith('   '):
            children.append(line.strip().split(':', 1)[0])
    if children != ['workflow_dispatch']:
        raise ValueError(f'Only workflow_dispatch is permitted, found {children}')


def main():
    root = Path(__file__).resolve().parent.parent
    files = sorted((root / '.github/workflows').glob('*.y*ml'))
    if not files:
        raise ValueError('Workflow inventory missing')
    for path in files:
        try:
            check_triggers(path.read_text(encoding='utf-8-sig'))
        except ValueError as error:
            raise ValueError(f'{path.name}: {error}') from error
    print(f'Manual-only triggers verified: {len(files)} workflows')


if __name__ == '__main__':
    main()
