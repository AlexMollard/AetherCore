"""Inspect generated compile commands: ASAN gates cannot silently omit native TUs."""
import argparse
import json
from pathlib import Path
import re


def missing_instrumentation(commands, repo, msvc_stl=False):
    repo = repo.resolve()
    missing = []
    checked = []
    for entry in commands:
        source = Path(entry['file'])
        if not source.is_absolute():
            source = Path(entry['directory']) / source
        source = source.resolve()
        if source.suffix.lower() not in ('.c', '.cc', '.cpp', '.cxx'):
            continue
        try:
            relative = source.relative_to(repo).as_posix()
        except ValueError:
            relative = source.as_posix()
        first_party = relative.startswith(('src/', 'tools/', 'tests/'))
        # MSVC STL's annotation ABI applies to compiled C++ dependencies too.
        # CPM may place sources outside the repo in a shared cache. Do not rely
        # on a particular _deps directory spelling to identify compiled C++.
        dependency_cpp = msvc_stl and source.suffix.lower() != '.c'
        if not first_party and not dependency_cpp:
            continue
        command = entry.get('command', ' '.join(entry.get('arguments', [])))
        checked.append(relative)
        if not re.search(r'[-/]fsanitize=address(?:[,\s\"]|$)', command):
            missing.append(relative)
        if '_DISABLE_STRING_ANNOTATION' in command or '_DISABLE_VECTOR_ANNOTATION' in command:
            missing.append(relative + ' (container annotations disabled)')
    if not checked:
        raise ValueError('No first-party compilation units found; coverage cannot be verified')
    return checked, missing


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compile-commands', type=Path, required=True)
    parser.add_argument('--msvc-stl', action='store_true')
    args = parser.parse_args()
    checked, missing = missing_instrumentation(json.loads(args.compile_commands.read_text(encoding='utf-8-sig')),
                                                Path(__file__).resolve().parent.parent, args.msvc_stl)
    if missing:
        print('Missing/inconsistent sanitizer instrumentation:\n' + '\n'.join(missing))
        return 1
    print(f'ASAN instrumentation verified for {len(checked)} native compilation units')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
