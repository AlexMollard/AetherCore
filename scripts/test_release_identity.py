"""Regression tests for artifact hashes and retained-symbol mismatch rejection."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import uuid
import struct
import json
import sys
import contextlib
import io
from unittest import mock
import check_manual_workflows

spec = importlib.util.spec_from_file_location('release_identity', Path(__file__).with_name('Write-ReleaseIdentity.py'))
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)


def pe_fixture(guid, age=3, name='Editor.pdb'):
    data = bytearray(1024)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', data, 0x84, 0x8664, 1, 0, 0, 0, 240, 0x22)
    struct.pack_into('<H', data, 0x98, 0x20b)
    struct.pack_into('<I', data, 0x98 + 60, 512)
    struct.pack_into('<I', data, 0x98 + 108, 16)
    struct.pack_into('<II', data, 0x98 + 112 + 6 * 8, 0x1000, 28)
    data[0x188:0x190] = b'.rdata\0\0'
    struct.pack_into('<IIII', data, 0x188 + 8, 512, 0x1000, 512, 512)
    codeview = b'RSDS' + guid.bytes_le + age.to_bytes(4, 'little') + ('C:\\build\\' + name).encode() + b'\0'
    struct.pack_into('<IIHHIIII', data, 512, 0, 0, 0, 0, 2, len(codeview), 0x1040, 576)
    data[576:576 + len(codeview)] = codeview
    return data


def pdb_fixture(guid, age=3):
    block = 4096
    data = bytearray(6 * block)
    data[:32] = b'Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0'
    struct.pack_into('<IIIIII', data, 32, block, 1, 6, 16, 0, 3)
    struct.pack_into('<I', data, 3 * block, 4)
    struct.pack_into('<IIII', data, 4 * block, 2, 0, 28, 5)
    struct.pack_into('<III', data, 5 * block, 20000404, 0, age)
    data[5 * block + 12:5 * block + 28] = guid.bytes_le
    return data


class ReleaseIdentityTests(unittest.TestCase):
    def test_workflow_trigger_guard_rejects_automatic_and_ambiguous_forms(self):
        check_manual_workflows.check_triggers('on:\n  workflow_dispatch:\n    inputs:\n      version:\n        required: false\njobs:\n')
        for text in ('on:\n  push:\n', 'on:\n  workflow_dispatch:\n  schedule:\n', 'on: [push]\n', 'on:\n  workflow_dispatch:\non:\n  push:\n'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                check_manual_workflows.check_triggers(text)
    def test_symbol_guid_and_age_must_match(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            guid = uuid.uuid4()
            binary = root / 'Editor.exe'
            binary.write_bytes(pe_fixture(guid))
            pdb = root / 'Editor.pdb'
            pdb.write_bytes(pdb_fixture(guid))
            self.assertEqual(identity.windows_symbol(binary, root)['guid'], str(guid))
            pdb.write_bytes(pdb_fixture(guid, 2))
            with self.assertRaisesRegex(ValueError, 'matching PDB'):
                identity.windows_symbol(binary, root)
            pdb.write_bytes(pdb_fixture(uuid.uuid4()))
            with self.assertRaisesRegex(ValueError, 'matching PDB'):
                identity.windows_symbol(binary, root)

    def test_missing_debug_identity_is_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / 'Editor.exe'
            binary.write_bytes(b'MZ-no-debug-record')
            with self.assertRaises(ValueError):
                identity.windows_symbol(binary, root)

    def test_symbol_identity_ignores_decoy_bytes_and_rejects_non_pdb_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            guid = uuid.uuid4()
            binary = root / 'Editor.exe'
            data = pe_fixture(guid)
            decoy = b'RSDS' + uuid.uuid4().bytes_le + (9).to_bytes(4, 'little') + b'decoy.pdb\0'
            data[64:64 + len(decoy)] = decoy
            binary.write_bytes(data)
            pdb = root / 'Editor.pdb'
            pdb.write_bytes(pdb_fixture(guid))
            self.assertEqual(identity.windows_symbol(binary, root)['guid'], str(guid))
            pdb.write_bytes(b'not-a-pdb' + (3).to_bytes(4, 'little') + guid.bytes_le)
            with self.assertRaises(ValueError):
                identity.windows_symbol(binary, root)

    def test_symbol_identity_must_come_from_info_stream_and_valid_block_map(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            guid = uuid.uuid4()
            binary = root / 'Editor.exe'
            binary.write_bytes(pe_fixture(guid))
            pdb = root / 'Editor.pdb'
            wrong = pdb_fixture(uuid.uuid4())
            wrong[4096:4116] = (3).to_bytes(4, 'little') + guid.bytes_le
            pdb.write_bytes(wrong)
            with self.assertRaises(ValueError):
                identity.windows_symbol(binary, root)
            malformed = pdb_fixture(guid)
            struct.pack_into('<I', malformed, 3 * 4096, 99999)
            pdb.write_bytes(malformed)
            with self.assertRaises(ValueError):
                identity.windows_symbol(binary, root)
            pdb.write_bytes(pdb_fixture(guid)[:-1])
            with self.assertRaises(ValueError):
                identity.windows_symbol(binary, root)

    def test_source_snapshot_binds_untracked_content_tracked_content_and_deletion(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tracked = root / 'tracked.cpp'
            new = root / 'new.hpp'
            tracked.write_text('aaaa')
            new.write_text('bbbb')
            first = identity.source_snapshot(root, ['tracked.cpp'], ['new.hpp'])
            new.write_text('cccc')
            second = identity.source_snapshot(root, ['tracked.cpp'], ['new.hpp'])
            self.assertNotEqual(first['sha256'], second['sha256'])
            tracked.write_text('dddd')
            third = identity.source_snapshot(root, ['tracked.cpp'], ['new.hpp'])
            self.assertNotEqual(second['sha256'], third['sha256'])
            tracked.unlink()
            fourth = identity.source_snapshot(root, ['tracked.cpp'], ['new.hpp'])
            self.assertNotEqual(third['sha256'], fourth['sha256'])
            self.assertIn({'path': 'tracked.cpp', 'tracking': 'tracked', 'kind': 'deleted'}, fourth['files'])

    def test_inventory_excludes_report_so_rerun_cannot_hash_its_own_previous_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'product.zip').write_bytes(b'product')
            report = root / 'release-identity.json'
            report.write_text('previous report')
            first = identity.inventory(root, exclude_paths=[report])
            report.write_text('new report')
            self.assertEqual(first, identity.inventory(root, exclude_paths=[report]))
            self.assertEqual([entry['path'] for entry in first], ['product.zip'])

    def test_staged_manifest_ships_before_archives_and_final_identity_rejects_payload_drift(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payload, symbols, build, artifacts = [root / name for name in ('payload', 'symbols', 'build', 'artifacts')]
            for directory in (payload, symbols, build, artifacts):
                directory.mkdir()
            guid = uuid.uuid4()
            (payload / 'Editor.exe').write_bytes(pe_fixture(guid))
            (symbols / 'Editor.pdb').write_bytes(pdb_fixture(guid))
            compiler = root / 'compiler'
            compiler.write_bytes(b'compiler-fixture')
            (build / 'CMakeCache.txt').write_text('CMAKE_CXX_COMPILER:FILEPATH=' + str(compiler) + '\n')
            (artifacts / 'product.zip').write_bytes(b'archive-fixture')
            staged = payload / 'BUILD_PROVENANCE.json'
            final = artifacts / 'release-identity.json'

            def git_output(args, **kwargs):
                return b'global.json\0CMake/Dependencies.cmake\0' if '--cached' in args else b''

            def tool_output(args, cwd=None, **kwargs):
                output = 'a' * 40 if args[:2] == ['git', 'rev-parse'] else ('' if args[:2] == ['git', 'status'] else 'synthetic-tool-version')
                return {'command': args, 'exitCode': 0, 'output': output}

            def run(args):
                with mock.patch.object(sys, 'argv', ['Write-ReleaseIdentity.py'] + args), contextlib.redirect_stdout(io.StringIO()):
                    return identity.main()

            common = ['--payload', str(payload), '--symbols', str(symbols), '--build-dir', str(build)]
            with mock.patch.object(identity, 'command', side_effect=tool_output), \
                    mock.patch.object(identity.platform, 'platform', return_value='synthetic-host'), \
                    mock.patch.object(identity.subprocess, 'check_output', side_effect=git_output):
                self.assertEqual(run(common + ['--stage-only', '--report', str(staged)]), 0)
                manifest = json.loads(staged.read_text())
                self.assertEqual(manifest['kind'], 'staged-build-provenance')
                self.assertNotIn('BUILD_PROVENANCE.json', [entry['path'] for entry in manifest['payload']])
                self.assertEqual(run(['--payload', str(payload), '--verify-stage']), 0)
                for field in ('compiler', 'cmakeCache', 'dependencyInputs', 'tools'):
                    incomplete = dict(manifest)
                    del incomplete[field]
                    staged.write_text(json.dumps(incomplete))
                    self.assertEqual(run(['--payload', str(payload), '--verify-stage']), 1, field)
                corrupted_source = json.loads(json.dumps(manifest))
                corrupted_source['source']['files'][0]['sha256'] = '0' * 64
                staged.write_text(json.dumps(corrupted_source))
                self.assertEqual(run(['--payload', str(payload), '--verify-stage']), 1)
                wrong_symbols = json.loads(json.dumps(manifest))
                wrong_symbols['symbols'][0]['guid'] = str(uuid.uuid4())
                staged.write_text(json.dumps(wrong_symbols))
                self.assertEqual(run(['--payload', str(payload), '--verify-stage']), 1)
                staged.write_text(json.dumps(manifest))
                self.assertEqual(run(common + ['--artifacts', str(artifacts), '--report', str(final)]), 0)
                first = json.loads(final.read_text())
                self.assertEqual(first['buildProvenance']['sha256'], identity.digest(staged))
                self.assertEqual(run(common + ['--artifacts', str(artifacts), '--report', str(final)]), 0)
                self.assertEqual(first['artifacts'], json.loads(final.read_text())['artifacts'])
                changed = bytearray((payload / 'Editor.exe').read_bytes())
                changed[700] = 1
                (payload / 'Editor.exe').write_bytes(changed)
                self.assertEqual(run(['--payload', str(payload), '--verify-stage']), 1)
                self.assertEqual(run(common + ['--artifacts', str(artifacts), '--report', str(final)]), 1)
                self.assertIn('Payload changed', json.loads(final.read_text())['detail'])

    def test_stage_verification_rejects_placeholder_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'product.bin').write_bytes(b'product')
            manifest = {'schemaVersion': 1, 'kind': 'staged-build-provenance', 'status': 'pass',
                        'source': {'sourceSnapshotSha256': '0' * 64}, 'symbols': [{'fixture': 'synthetic-presence-only'}],
                        'payload': identity.inventory(root)}
            (root / 'BUILD_PROVENANCE.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, 'Incomplete staged provenance'):
                identity.verify_stage_payload(root)

    def test_pe_codeview_pointer_must_reference_valid_debug_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            guid = uuid.uuid4()
            binary = root / 'Editor.exe'
            (root / 'Editor.pdb').write_bytes(pdb_fixture(guid))
            data = pe_fixture(guid)
            struct.pack_into('<I', data, 512 + 24, len(data) + 1)
            binary.write_bytes(data)
            with self.assertRaises(ValueError):
                identity.windows_symbol(binary, root)

    def test_elf_symbol_identity_requires_exact_build_id_and_successful_reader(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'Editor.debug').write_bytes(b'symbol-fixture')
            for debug_result in ({'exitCode': 0, 'output': 'Build ID: abcdef'},
                                 {'exitCode': 1, 'output': 'Build ID: abc'}):
                with mock.patch.object(identity, 'command', side_effect=[{'exitCode': 0, 'output': 'Build ID: abc'}, debug_result]):
                    with self.assertRaises(ValueError):
                        identity.elf_symbol(root / 'Editor', root)
            with mock.patch.object(identity, 'command', return_value={'exitCode': 0, 'output': 'Build ID: abc'}):
                self.assertEqual(identity.elf_symbol(root / 'Editor', root)['buildId'], 'abc')

    def test_inventory_detects_same_size_edits(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact = root / 'product.zip'
            artifact.write_bytes(b'aaaa')
            first = identity.inventory(root)
            artifact.write_bytes(b'bbbb')
            second = identity.inventory(root)
            self.assertEqual(first[0]['bytes'], second[0]['bytes'])
            self.assertNotEqual(first[0]['sha256'], second[0]['sha256'])

    def test_inventory_order_is_independent_of_host_filesystem_case_sorting(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'a.bin').write_bytes(b'a')
            (root / 'Z.bin').write_bytes(b'z')
            self.assertEqual([entry['path'] for entry in identity.inventory(root)], ['Z.bin', 'a.bin'])


if __name__ == '__main__':
    unittest.main()
