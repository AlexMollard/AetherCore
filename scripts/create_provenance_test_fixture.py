"""Synthetic package-verifier fixture only; generates no runnable engine bytes."""
import contextlib
import io
from pathlib import Path
import sys
import uuid
from unittest import mock

import test_release_identity as fixtures


def main():
    payload, scratch = map(Path, sys.argv[1:])
    symbols = scratch / 'fixture-symbols'
    build = scratch / 'fixture-build'
    symbols.mkdir()
    build.mkdir()
    for name in ('Editor', 'Launcher', 'AetherGame'):
        guid = uuid.uuid4()
        (payload / (name + '.exe')).write_bytes(fixtures.pe_fixture(guid, name=name + '.pdb'))
        (symbols / (name + '.pdb')).write_bytes(fixtures.pdb_fixture(guid))
    compiler = build / 'synthetic-compiler'
    compiler.write_bytes(b'synthetic compiler identity - not executable')
    (build / 'CMakeCache.txt').write_text('CMAKE_CXX_COMPILER:FILEPATH=' + str(compiler) + '\n')

    def command(arguments, cwd=None):
        output = 'a' * 40 if arguments[:2] == ['git', 'rev-parse'] else '' if arguments[:2] == ['git', 'status'] else 'synthetic tool version'
        return dict(command=arguments, exitCode=0, output=output)

    def git_output(arguments, **kwargs):
        return b'global.json\0CMake/Dependencies.cmake\0' if '--cached' in arguments else b''

    arguments = ['Write-ReleaseIdentity.py', '--payload', str(payload), '--symbols', str(symbols),
                 '--build-dir', str(build), '--stage-only', '--report', str(payload / 'BUILD_PROVENANCE.json')]
    with mock.patch.object(sys, 'argv', arguments), mock.patch.object(fixtures.identity.platform, 'platform', return_value='synthetic-test-host'), mock.patch.object(fixtures.identity, 'command', side_effect=command), \
            mock.patch.object(fixtures.identity.subprocess, 'check_output', side_effect=git_output), contextlib.redirect_stdout(io.StringIO()):
        return fixtures.identity.main()


if __name__ == '__main__':
    raise SystemExit(main())
