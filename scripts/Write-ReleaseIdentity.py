#!/usr/bin/env python3
"""Private artifact inventory. No uploads, signing, or source/build mutations."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import struct
import uuid


def command(args, cwd=None):
    try:
        result = subprocess.run(args, cwd=cwd, text=True, capture_output=True, timeout=30)
        return {"command": args, "exitCode": result.returncode,
                "output": (result.stdout + result.stderr).strip()}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"command": args, "exitCode": None, "output": str(error)}


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def inventory(root, exclude_paths=()):
    excluded = {path.resolve() for path in exclude_paths}
    paths = sorted(root.rglob('*'), key=lambda path: path.relative_to(root).as_posix())
    return [{"path": p.relative_to(root).as_posix(), "bytes": p.stat().st_size,
             "sha256": digest(p)} for p in paths if p.is_file() and p.resolve() not in excluded]


def source_snapshot(repo, tracked, untracked):
    files = []
    for tracking, names in (('tracked', tracked), ('untracked', untracked)):
        for name in sorted(set(names)):
            if not name:
                continue
            path = repo / name
            entry = {'path': name, 'tracking': tracking}
            if path.is_symlink():
                entry.update(kind='symlink', target=os.readlink(path))
            elif path.is_file():
                entry.update(kind='file', bytes=path.stat().st_size, sha256=digest(path))
            elif not path.exists():
                entry['kind'] = 'deleted'
            else:
                raise ValueError(f'Unsupported source entry (submodules require separate provenance): {name}')
            files.append(entry)
    files.sort(key=lambda item: (item['path'], item['tracking']))
    return {'sha256': canonical_sha256(files), 'files': files}


def canonical_sha256(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode('utf-8')).hexdigest()


def pe_codeview_identity(data):
    # PE/COFF optional-header debug directory, never a raw RSDS byte search.
    # https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
    def unpack(format_string, offset):
        size = struct.calcsize(format_string)
        if offset < 0 or offset + size > len(data):
            raise ValueError('Truncated PE metadata')
        return struct.unpack_from(format_string, data, offset)

    if data[:2] != b'MZ':
        raise ValueError('DOS/PE header absent')
    pe, = unpack('<I', 0x3c)
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('PE signature absent')
    sections, = unpack('<H', pe + 6)
    optional_size, = unpack('<H', pe + 20)
    optional = pe + 24
    magic, = unpack('<H', optional)
    if magic not in (0x10b, 0x20b):
        raise ValueError('Unsupported PE optional header')
    directories = 96 if magic == 0x10b else 112
    if optional_size < directories + 7 * 8 or optional + optional_size > len(data):
        raise ValueError('PE debug data directory absent')
    directory_count, = unpack('<I', optional + directories - 4)
    if directory_count <= 6:
        raise ValueError('PE debug data directory absent')
    section_table = optional + optional_size
    if sections == 0 or section_table + sections * 40 > len(data):
        raise ValueError('Invalid PE section table')

    def rva_offset(rva, size):
        header_size, = unpack('<I', optional + 60)
        if rva < header_size and rva + size <= min(header_size, len(data)):
            return rva
        for section in range(sections):
            virtual_size, virtual_address, raw_size, raw_pointer = unpack('<IIII', section_table + section * 40 + 8)
            offset = rva - virtual_address
            if 0 <= offset and offset + size <= raw_size and offset < max(virtual_size, raw_size):
                if raw_pointer + offset + size > len(data):
                    raise ValueError('Truncated PE section data')
                return raw_pointer + offset
        raise ValueError('PE debug RVA has no file-backed section')

    debug_rva, debug_size = unpack('<II', optional + directories + 6 * 8)
    if not debug_rva or not debug_size or debug_size % 28:
        raise ValueError('Invalid PE debug directory')
    debug = rva_offset(debug_rva, debug_size)
    identities = []
    for offset in range(debug, debug + debug_size, 28):
        kind, size, address, pointer = unpack('<IIII', offset + 12)
        if kind != 2:
            continue
        if pointer + size > len(data) or size < 25:
            raise ValueError('Truncated PE CodeView record')
        if address and rva_offset(address, size) != pointer:
            raise ValueError('PE CodeView file pointer/RVA disagree')
        record = data[pointer:pointer + size]
        if record[:4] != b'RSDS':
            continue
        if b'\0' not in record[24:]:
            raise ValueError('Unterminated CodeView PDB path')
        name = record[24:].split(b'\0', 1)[0].decode('utf-8').replace('\\', '/').split('/')[-1]
        if not name or name in ('.', '..'):
            raise ValueError('CodeView PDB name absent')
        identities.append((record[4:20], int.from_bytes(record[20:24], 'little'), name))
    if len(identities) != 1:
        raise ValueError(f'Expected one PE CodeView RSDS identity; found {len(identities)}')
    return identities[0]


def pdb_info_identity(path):
    # MSF7 stream directory -> information stream 1's header (GUID + age).
    # https://llvm.org/docs/PDB/MsfFile.html
    # https://llvm.org/docs/PDB/PdbStream.html
    with path.open('rb') as stream:
        file_size = path.stat().st_size

        def read_at(offset, size):
            if offset < 0 or size < 0 or offset + size > file_size:
                raise ValueError('PDB block points outside file')
            stream.seek(offset)
            data = stream.read(size)
            if len(data) != size:
                raise ValueError('Truncated PDB block')
            return data

        header = read_at(0, 56)
        if header[:32] != b'Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0':
            raise ValueError('MSF7 PDB signature absent')
        block_size, free_map, blocks, directory_size, _, block_map = struct.unpack_from('<IIIIII', header, 32)
        if block_size not in (512, 1024, 2048, 4096) or free_map not in (1, 2) or blocks * block_size != file_size:
            raise ValueError('Invalid MSF7 PDB superblock')
        if directory_size < 12 or directory_size > file_size:
            raise ValueError('Invalid PDB stream-directory size')
        directory_blocks = (directory_size + block_size - 1) // block_size
        if directory_blocks * 4 > block_size or block_map >= blocks:
            raise ValueError('Invalid PDB directory block map')
        indexes = struct.unpack('<' + 'I' * directory_blocks, read_at(block_map * block_size, directory_blocks * 4))
        if any(index >= blocks for index in indexes):
            raise ValueError('PDB directory block outside file')
        directory = b''.join(read_at(index * block_size, block_size) for index in indexes)[:directory_size]
        count, = struct.unpack_from('<I', directory)
        if count < 2 or 4 + count * 4 > len(directory):
            raise ValueError('PDB information stream absent')
        sizes = struct.unpack_from('<' + 'I' * count, directory, 4)
        cursor = 4 + count * 4
        info_blocks = None
        for index, size in enumerate(sizes):
            block_count = 0 if size == 0xffffffff else (size + block_size - 1) // block_size
            if cursor + block_count * 4 > len(directory):
                raise ValueError('Truncated PDB stream block list')
            block_indexes = struct.unpack_from('<' + 'I' * block_count, directory, cursor)
            if any(block >= blocks for block in block_indexes):
                raise ValueError('PDB stream block outside file')
            if index == 1:
                info_blocks = block_indexes
            cursor += block_count * 4
        if sizes[1] == 0xffffffff or sizes[1] < 28 or not info_blocks:
            raise ValueError('PDB information stream header absent')
        info = read_at(info_blocks[0] * block_size, 28)
        version, _, age = struct.unpack_from('<III', info)
        if version not in (19990604, 20000404, 20030901, 20091201, 20140508):
            raise ValueError('Unsupported PDB information-stream version')
        return info[12:28], age


def windows_symbol(binary, symbols):
    guid, age, pdb_name = pe_codeview_identity(binary.read_bytes())
    matches = []
    for path in symbols.rglob('*'):
        if path.is_file() and path.name.casefold() == pdb_name.casefold():
            try:
                if pdb_info_identity(path) == (guid, age):
                    matches.append(path)
            except (ValueError, OSError, struct.error):
                continue
    if len(matches) != 1:
        raise ValueError(f'Expected one retained matching PDB for {binary.name}; found {len(matches)}')
    return {"binary": binary.name, "guid": str(uuid.UUID(bytes_le=guid)), "age": age,
            "symbol": str(matches[0].resolve()), "sha256": digest(matches[0])}


def elf_build_id(binary):
    result = command(['readelf', '-n', str(binary)])
    match = re.search(r'^\s*Build ID:\s+([0-9a-fA-F]+)\s*$', result['output'], re.M)
    if result['exitCode'] != 0 or not match:
        raise ValueError('Valid ELF build ID missing (readelf must be available for ELF provenance)')
    return match[1].lower()


def elf_symbol(binary, symbols):
    identifier = elf_build_id(binary)
    debug = symbols / (binary.name + '.debug')
    if elf_build_id(debug) != identifier:
        raise ValueError(f'Matching retained ELF debug symbols missing: {debug}')
    return {'binary': binary.name, 'buildId': identifier, 'symbol': str(debug), 'sha256': digest(debug)}


def verify_stage_payload(payload):
    path = payload / 'BUILD_PROVENANCE.json'
    staged = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(staged, dict) or staged.get('schemaVersion') != 1 or staged.get('kind') != 'staged-build-provenance' or staged.get('status') != 'pass':
        raise ValueError('Successful staged BUILD_PROVENANCE.json is required')
    if staged.get('payload') != inventory(payload, exclude_paths=[path]):
        raise ValueError('Payload changed after staged build provenance')
    def require(condition, message):
        if not condition:
            raise ValueError('Incomplete staged provenance: ' + message)

    def sha256(value):
        return isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value) is not None

    def command_record(value):
        return (isinstance(value, dict) and isinstance(value.get('command'), list) and bool(value['command'])
                and all(isinstance(argument, str) for argument in value['command'])
                and 'exitCode' in value and (value['exitCode'] is None or type(value['exitCode']) is int)
                and isinstance(value.get('output'), str))

    require(staged.get('distribution') == 'private' and staged.get('signing') == 'unsigned-not-verified', 'explicit private/unsigned verification status required')
    source = staged.get('source')
    require(isinstance(source, dict), 'source record absent')
    require(command_record(source.get('revision')) and source['revision']['exitCode'] == 0
            and re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}', source['revision']['output']), 'valid source revision required')
    require(command_record(source.get('workingTree')) and source['workingTree']['exitCode'] == 0 and sha256(source.get('patchSha256')), 'working tree/patch identity required')
    files = source.get('files')
    require(isinstance(files, list) and bool(files), 'source file inventory absent')
    paths = set()
    for entry in files:
        require(isinstance(entry, dict) and isinstance(entry.get('path'), str) and bool(entry['path']), 'source path missing')
        name = PurePosixPath(entry['path'])
        require(not name.is_absolute() and '..' not in name.parts and entry['path'] not in paths, 'source path duplicated or unsafe')
        paths.add(entry['path'])
        require(entry.get('tracking') in ('tracked', 'untracked'), 'source tracking status absent')
        if entry.get('kind') == 'file':
            require(type(entry.get('bytes')) is int and entry['bytes'] >= 0 and sha256(entry.get('sha256')), 'source file identity absent')
        elif entry.get('kind') == 'symlink':
            require(isinstance(entry.get('target'), str) and bool(entry['target']), 'source link target absent')
        else:
            require(entry.get('kind') == 'deleted', 'source kind absent')
    require(sha256(source.get('sourceSnapshotSha256')) and canonical_sha256(files) == source['sourceSnapshotSha256'], 'source snapshot aggregate differs from included inventory')
    compiler = staged.get('compiler')
    require(isinstance(compiler, dict) and isinstance(compiler.get('path'), str) and bool(compiler['path'])
            and sha256(compiler.get('sha256')) and command_record(compiler.get('version')), 'compiler path/bytes/version identity required')
    cache = staged.get('cmakeCache')
    require(isinstance(cache, dict) and sha256(cache.get('sha256')) and isinstance(cache.get('settings'), list)
            and bool(cache['settings']) and all(isinstance(line, str) for line in cache['settings']), 'CMake cache identity required')
    tools = staged.get('tools')
    require(isinstance(tools, dict) and all(command_record(tools.get(name)) for name in ('cmake', 'ctest', 'dotnet', 'rust', 'vulkan', 'slang', 'innoSetup')), 'tool version/status records required')
    dependencies = staged.get('dependencyInputs')
    require(isinstance(dependencies, list) and bool(dependencies), 'dependency input identities required')
    by_path = {entry['path']: entry for entry in files}
    dependency_names = set()
    for entry in dependencies:
        require(isinstance(entry, dict) and isinstance(entry.get('path'), str) and sha256(entry.get('sha256'))
                and isinstance(entry.get('content'), str), 'dependency identity malformed')
        require(entry['path'] in by_path and by_path[entry['path']].get('sha256') == entry['sha256'], 'dependency differs from source snapshot')
        dependency_names.add(entry['path'])
    require({'CMake/Dependencies.cmake', 'global.json'} <= dependency_names, 'required dependency inputs absent')
    symbols = staged.get('symbols')
    require(isinstance(symbols, list) and bool(symbols), 'symbol identities required')
    declared = {}
    for entry in symbols:
        require(isinstance(entry, dict) and isinstance(entry.get('binary'), str) and bool(entry['binary'])
                and '/' not in entry['binary'] and '\\' not in entry['binary'] and entry['binary'] not in declared,
                'symbol binary identity missing/duplicated')
        require(isinstance(entry.get('symbol'), str) and bool(entry['symbol']) and sha256(entry.get('sha256')), 'retained symbol path/hash required')
        declared[entry['binary']] = entry
    binaries = list(payload.glob('*.exe'))
    if binaries:
        require(set(declared) == {binary.name for binary in binaries}, 'payload executable/symbol inventory differs')
        for binary in binaries:
            guid, age, _ = pe_codeview_identity(binary.read_bytes())
            entry = declared[binary.name]
            require(isinstance(entry.get('guid'), str) and type(entry.get('age')) is int
                    and entry['age'] == age and uuid.UUID(entry['guid']).bytes_le == guid, 'payload PE identity differs from retained symbol identity')
    else:
        binaries = [payload / name for name in ('Editor', 'Launcher', 'AetherGame') if (payload / name).is_file()]
        require(set(declared) == {binary.name for binary in binaries} and bool(binaries), 'payload ELF/symbol inventory differs')
        for binary in binaries:
            require(declared[binary.name].get('buildId') == elf_build_id(binary), 'payload ELF identity differs from retained symbol identity')
    return staged


def verify_build_provenance(path, source, payload, symbols):
    staged = verify_stage_payload(path.parent)
    if staged.get('source', {}).get('sourceSnapshotSha256') != source['sourceSnapshotSha256']:
        raise ValueError('Source snapshot changed after staged build provenance')
    if staged.get('payload') != payload:
        raise ValueError('Payload changed after staged build provenance')
    def symbol_ids(entries):
        return [{key: entry[key] for key in ('binary', 'guid', 'age', 'buildId', 'sha256') if key in entry}
                for entry in entries]
    if symbol_ids(staged.get('symbols', [])) != symbol_ids(symbols):
        raise ValueError('Retained symbol identity changed after staged build provenance')
    return {'path': path.name, 'sha256': digest(path)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload', required=True, type=Path)
    parser.add_argument('--artifacts', type=Path)
    parser.add_argument('--symbols', type=Path)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--stage-only', action='store_true', help='Write payload BUILD_PROVENANCE.json before creating archives/installer')
    parser.add_argument('--verify-stage', action='store_true', help='Verify delivered payload hashes against its staged manifest without developer tools')
    args = parser.parse_args()
    if args.verify_stage:
        try:
            verify_stage_payload(args.payload)
            print('Staged build provenance matches every delivered payload file.')
            return 0
        except (ValueError, OSError, TypeError) as error:
            print(f'Staged build provenance verification failed: {error}')
            return 1
    if not args.symbols or not args.build_dir or not args.report:
        parser.error('--symbols, --build-dir and --report are required for provenance generation')
    build_provenance = args.payload / 'BUILD_PROVENANCE.json'
    if not args.stage_only and not args.artifacts:
        parser.error('--artifacts is required unless --stage-only is used')
    if args.stage_only and args.report.resolve() != build_provenance.resolve():
        parser.error('--stage-only report must be <payload>/BUILD_PROVENANCE.json')
    if not args.stage_only and args.report.resolve() == build_provenance.resolve():
        parser.error('Final artifact identity must not overwrite staged BUILD_PROVENANCE.json')
    if not args.stage_only and args.report.resolve().is_relative_to(args.payload.resolve()):
        parser.error('Final artifact identity must be outside the already staged payload')
    repo = Path(__file__).resolve().parent.parent
    report = {"schemaVersion": 1, "status": "fail", "distribution": "private",
              "kind": "staged-build-provenance" if args.stage_only else "final-artifact-identity",
              "signing": "unsigned-not-verified", "host": platform.platform(),
              "cleanMachineQualification": "blocked-requires-standard-user-clean-VM"}
    try:
        for path in (args.payload, args.symbols, args.build_dir) + (() if args.stage_only else (args.artifacts,)):
            if not path.is_dir():
                raise ValueError(f'Missing directory: {path}')
        report['source'] = {"revision": command(['git', 'rev-parse', 'HEAD'], repo),
                            "workingTree": command(['git', 'status', '--porcelain=v1'], repo),
                            "patchSha256": hashlib.sha256(subprocess.check_output(['git', 'diff', '--binary', 'HEAD'], cwd=repo)).hexdigest()}
        if report['source']['revision']['exitCode'] != 0 or report['source']['workingTree']['exitCode'] != 0:
            raise ValueError('Git source revision/working-tree provenance unavailable')
        untracked = subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard', '-z'], cwd=repo).decode('utf-8').split('\0')
        tracked = subprocess.check_output(['git', 'ls-files', '--cached', '-z'], cwd=repo).decode('utf-8').split('\0')
        snapshot = source_snapshot(repo, tracked, untracked)
        report['source']['sourceSnapshotSha256'] = snapshot['sha256']
        report['source']['files'] = snapshot['files']
        report['source']['untrackedFiles'] = [{"path": name, "sha256": digest(repo / name), "bytes": (repo / name).stat().st_size}
                                                for name in untracked if name and (repo / name).is_file()]
        inno = shutil.which('ISCC')
        if not inno and os.name == 'nt':
            candidate = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Inno Setup 6/ISCC.exe'
            if candidate.is_file():
                inno = str(candidate)
        report['tools'] = {name: command(cmd) for name, cmd in {
            'cmake': ['cmake', '--version'], 'ctest': ['ctest', '--version'],
            'dotnet': ['dotnet', '--info'], 'rust': ['rustc', '-Vv'],
            'vulkan': ['vulkaninfo', '--summary'], 'slang': ['slangc', '-version'],
            'innoSetup': [inno or 'ISCC', '/?']}.items()}
        report['vulkanSdkRoot'] = os.environ.get('VULKAN_SDK')
        cache = args.build_dir / 'CMakeCache.txt'
        if not cache.is_file():
            raise ValueError('CMakeCache.txt required for build provenance')
        report['cmakeCache'] = {"sha256": digest(cache), "settings": [line for line in cache.read_text(errors='replace').splitlines()
                                if re.match(r'(AETHER|CMAKE_(BUILD_TYPE|CONFIGURATION_TYPES|CXX_COMPILER|C_COMPILER|GENERATOR))', line)]}
        compiler = re.search(r'^CMAKE_CXX_COMPILER:FILEPATH=(.*)$', cache.read_text(errors='replace'), re.M)
        if not compiler:
            compiler = re.search(r'^CMAKE_CXX_COMPILER:STRING=(.*)$', cache.read_text(errors='replace'), re.M)
        compiler_description = None
        if not compiler:
            # Visual Studio generators keep the actual compiler in CMakeFiles,
            # rather than CMakeCache. Preserve CMake's detected version as well.
            descriptions = sorted((args.build_dir / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'), reverse=True)
            if descriptions:
                compiler_description = descriptions[0].read_text(errors='replace')
                compiler = re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+)"\)', compiler_description)
        if compiler:
            compiler_path = Path(compiler.group(1).strip().replace(chr(92), '/'))
            report['compiler'] = {"path": str(compiler_path), "sha256": digest(compiler_path),
                                  "version": command([str(compiler_path), '--version']),
                                  "cmakeDetected": compiler_description}
        else:
            raise ValueError('Compiler identity missing from CMake cache')
        report['dependencyInputs'] = [{"path": p.relative_to(repo).as_posix(), "sha256": digest(p), "content": p.read_text(errors='replace')}
                                     for p in (repo / 'CMake/Dependencies.cmake', repo / 'global.json')]
        report['payload'] = inventory(args.payload, exclude_paths=[args.report, build_provenance])
        if not args.stage_only:
            report['artifacts'] = inventory(args.artifacts, exclude_paths=[args.report])
            if not report['artifacts']:
                raise ValueError('Final artifact inventory must be nonempty')
        if not report['payload']:
            raise ValueError('Payload inventory must be nonempty')
        report['bundledRuntimes'] = [p.name for p in (args.payload / 'dotnet/shared/Microsoft.NETCore.App').glob('*') if p.is_dir()]
        report['symbols'] = []
        binaries = list(args.payload.glob('*.exe'))
        for binary in binaries:
            report['symbols'].append(windows_symbol(binary, args.symbols))
        if not binaries:
            for name in ('Editor', 'Launcher', 'AetherGame'):
                binary = args.payload / name
                if not binary.is_file():
                    continue
                report['symbols'].append(elf_symbol(binary, args.symbols))
        if not report['symbols']:
            raise ValueError('No product executable symbol identities found')
        if not args.stage_only:
            report['buildProvenance'] = verify_build_provenance(build_provenance, report['source'], report['payload'], report['symbols'])
        report['status'] = 'pass'
    except (ValueError, OSError, subprocess.SubprocessError, struct.error, UnicodeError) as error:
        report['detail'] = str(error)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2), encoding='utf-8')
    return 0 if report['status'] == 'pass' else 1


if __name__ == '__main__':
    raise SystemExit(main())
