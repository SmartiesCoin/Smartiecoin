#!/usr/bin/env python3
# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Unit tests for the release gate (not evidence of a target-platform build)."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / 'contrib/devtools/check-release-bdb.py'


class ReleaseBDBArtifactsTest(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.is_file(), 'common release artifact gate is missing')
        spec = importlib.util.spec_from_file_location('release_bdb', SCRIPT)
        assert spec is not None and spec.loader is not None
        self.gate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.gate)

    def test_linked_constants_require_exact_pinned_version_and_wallet(self):
        gate = self.gate
        self.assertTrue(hasattr(gate, 'check_constants'), 'constant checks are missing')
        valid = b'Berkeley DB 4.8.30: (April 9, 2010)\x00-wallet=<path>\x00'
        gate.check_constants(valid, 'node')
        gate.check_constants(b'Get wallet info\x00' + valid, 'wallet')
        for contents in (b'', b'-wallet=<path>\x00',
                         valid.replace(b'4.8.30', b'18.1.40'),
                         valid.replace(b'4.8.30', b'4.8.31'),
                         valid + b'Berkeley DB 6.2.32\x00',
                         valid.replace(b'-wallet=<path>', b'-nowallet')):
            with self.subTest(contents=contents), self.assertRaises(gate.CheckError):
                gate.check_constants(contents, 'node')
        with self.assertRaises(gate.CheckError):
            gate.check_constants(valid, 'wallet')

    def test_dependency_inspection_is_fail_closed(self):
        gate = self.gate
        self.assertTrue(hasattr(gate, 'check_dependencies'), 'dependency checks missing')
        templates = {
            'PE': 'x: file format coff-x86-64\n    DLL Name: {}\n',
            'ELF': 'x: file format elf64-x86-64\nProgram Header:\nDynamic Section:\n NEEDED {}\n',
            'MACHO': 'x:\n\t{} (compatibility version 1.0.0, current version 1.0.0)\n',
        }
        for kind, template in templates.items():
            with self.subTest(kind=kind):
                gate.check_dependencies(kind, template.format('libc.dylib'))
                for name in ('libdb-4.8.so', 'libdb_cxx-18.1.dylib', 'DB48.DLL',
                             '@rpath/libdb_cxx.4.8.dylib', 'libberkeleydb.so'):
                    with self.assertRaisesRegex(gate.CheckError, 'dynamic BDB'):
                        gate.check_dependencies(kind, template.format(name))
                for output in ('', 'tool failed', 'x: file format unknown'):
                    with self.assertRaises(gate.CheckError):
                        gate.check_dependencies(kind, output)
        gate.check_dependencies('ELF', 'x: file format elf64-x86-64\nProgram Header:\n LOAD off 0x00\n')

    def test_section_decode_reconstructs_split_strings(self):
        gate = self.gate
        self.assertTrue(hasattr(gate, 'decode_section'), 'section extraction missing')
        # Unit fixture, deliberately split across objdump lines; never scan .rsrc.
        dump = ('x: file format coff-x86-64\nContents of section .rdata:\n'
                ' 1000 4265726b 656c6579 20444220 342e382e  Berkeley DB 4.8.\n'
                ' 1010 333000                               30.\n')
        self.assertEqual(gate.decode_section(dump, '.rdata'), b'Berkeley DB 4.8.30\x00')
        for output in ('', dump.replace('.rdata', '.rsrc'),
                       dump.replace('1010', '1020'), dump.split(' 1000')[0]):
            with self.subTest(output=output), self.assertRaises(gate.CheckError):
                gate.decode_section(output, '.rdata')

    def test_inspection_tool_errors_and_empty_output_fail(self):
        gate = self.gate
        self.assertTrue(hasattr(gate, 'run_command'), 'checked tool execution missing')
        for error in (OSError('missing'), subprocess.TimeoutExpired('objdump', 1)):
            with patch.object(subprocess, 'run', side_effect=error), self.assertRaises(gate.CheckError):
                gate.run_command(['objdump', '-p', 'test'])
        for result in (subprocess.CompletedProcess([], 1, '', 'bad input'),
                       subprocess.CompletedProcess([], 0, '', ''),
                       subprocess.CompletedProcess([], 0, 'partial', 'warning')):
            with patch.object(subprocess, 'run', return_value=result), self.assertRaises(gate.CheckError):
                gate.run_command(['objdump', '-p', 'test'])

    def test_runtime_requires_real_legacy_bdb9(self):
        gate = self.gate
        self.assertTrue(hasattr(gate, 'check_wallet_result'), 'runtime assertions missing')
        # The actual offline wallet tool does not initialize the debug logger.
        # Backend version is checked in linked constants, never assumed from help.
        output = 'Format: bdb\nDescriptors: no\nKeypool Size: 2000\n'
        header = bytes(12) + (0x053162).to_bytes(4, 'little') + (9).to_bytes(4, 'little')
        self.assertEqual(gate.check_wallet_result(output, header), 2000)
        for bad_output, bad_header in ((output, b''), (output, bytes(20)),
                                      (output, header[:16] + (10).to_bytes(4, 'little')),
                                      (output.replace('bdb', 'sqlite'), header),
                                      (output.replace('2000', '0'), header),
                                      (output.replace('Descriptors: no', 'Descriptors: yes'), header)):
            with self.subTest(output=bad_output, header=bad_header), self.assertRaises(gate.CheckError):
                gate.check_wallet_result(bad_output, bad_header)

    def test_runtime_must_reopen_and_fails_instead_of_skipping(self):
        gate = self.gate
        self.assertTrue(hasattr(gate, 'runtime_qa'), 'native QA missing')
        with patch.object(gate, 'run_command', side_effect=gate.CheckError('cannot execute')):
            with self.assertRaisesRegex(gate.CheckError, 'cannot execute'):
                gate.runtime_qa(Path('/nonexistent-wallet'))
        calls = []
        def fake_wallet_command(argv):
            # Unit-test double: no fabricated artifact/build evidence.
            calls.append(argv)
            wallet = Path(next(x.split('=', 1)[1] for x in argv if x.startswith('-wallet=')))
            if argv[-1] == 'create':
                self.assertFalse(wallet.exists())
                wallet.mkdir()
                (wallet / 'wallet.dat').write_bytes(bytes(12) + (0x053162).to_bytes(4, 'little') + (9).to_bytes(4, 'little'))
            else:
                self.assertEqual(argv[-1], 'info')
                self.assertTrue((wallet / 'wallet.dat').exists())
            return 'Using BerkeleyDB version Berkeley DB 4.8.30\nFormat: bdb\nDescriptors: no\nKeypool Size: 2\n'
        with patch.object(gate, 'run_command', side_effect=fake_wallet_command):
            gate.runtime_qa(Path('/unit-test-wallet'))
        self.assertEqual([c[-1] for c in calls], ['create', 'info'])
        wallet = Path(next(x.split('=', 1)[1] for x in calls[0] if x.startswith('-wallet=')))
        self.assertFalse(wallet.exists(), 'disposable QA wallet must be removed')

    def test_object_and_universal_binaries_fail_closed(self):
        gate = self.gate
        self.assertTrue(hasattr(gate, 'sniff_format'), 'format sniffing missing')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rel_obj = root / 'unit-rel.o'
            body = bytearray(64)
            body[:6] = b'\x7fELF\x02\x01'
            body[16] = 1  # ET_REL
            rel_obj.write_bytes(body)
            macho_obj = root / 'unit-mach.o'
            macho_obj.write_bytes(bytes.fromhex('cffaedfe070000010000000001000000') + bytes(48))  # MH_OBJECT
            fat = root / 'unit-fat'
            fat.write_bytes(bytes.fromhex('cafebabe00000002') + bytes(56))
            for path in (rel_obj, macho_obj, fat):
                with self.subTest(path=path.name), self.assertRaises(gate.CheckError):
                    gate.binary_target(path)
            self.assertIsNone(gate.sniff_format(rel_obj.read_bytes()))
            self.assertIsNone(gate.sniff_format(macho_obj.read_bytes()))
            exe = root / 'unit-exe'
            elf = bytearray(64)
            elf[:6] = b'\x7fELF\x02\x01'
            elf[16:18] = (2).to_bytes(2, 'little')
            elf[18:20] = (62).to_bytes(2, 'little')
            exe.write_bytes(elf)
            self.assertEqual([p.name for p in gate.bundled_binaries(root)], ['unit-exe', 'unit-fat'])

    def test_artifact_identification_and_native_qa_mode(self):
        gate = self.gate
        self.assertTrue(hasattr(gate, 'binary_target'), 'artifact format detection missing')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'unit-header'
            elf = bytearray(64)
            elf[:6] = b'\x7fELF\x02\x01'
            elf[16:18] = (2).to_bytes(2, 'little')  # ET_EXEC
            elf[18:20] = (62).to_bytes(2, 'little')
            path.write_bytes(elf)
            self.assertEqual(gate.binary_target(path), ('ELF', 'x86_64'))
            path.write_bytes(bytes.fromhex('cffaedfe070000010000000002000000') + bytes(48))
            self.assertEqual(gate.binary_target(path), ('MACHO', 'x86_64'))
            path.write_bytes(b'not a binary')
            with self.assertRaises(gate.CheckError):
                gate.binary_target(path)
        with patch.object(gate.platform, 'system', return_value='Darwin'), \
                patch.object(gate.platform, 'machine', return_value='arm64'):
            self.assertTrue(gate.is_native(('MACHO', 'aarch64')))
            self.assertFalse(gate.is_native(('PE', 'x86_64')))
            self.assertFalse(gate.is_native(('ELF', 'x86_64')))

    def test_cli_rejects_missing_artifacts(self):
        result = subprocess.run([__import__('sys').executable, str(SCRIPT)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0, 'empty release check must not pass')

    def test_auto_mode_never_skips_native_execution_failure(self):
        gate = self.gate
        self.assertTrue(hasattr(gate, 'check_runtime'), 'cross/native boundary missing')
        with patch.object(gate, 'is_native', return_value=True), \
                patch.object(gate, 'runtime_qa', side_effect=gate.CheckError('cannot execute')):
            with self.assertRaisesRegex(gate.CheckError, 'cannot execute'):
                gate.check_runtime('auto', ('MACHO', 'aarch64'), Path('wallet'))
        with patch.object(gate, 'is_native', return_value=False), \
                patch.object(gate, 'runtime_qa', side_effect=gate.CheckError('cannot execute')):
            report = gate.check_runtime('auto', ('PE', 'x86_64'), Path('wallet'))
            self.assertEqual(report['status'], 'required-on-native-target')
            with self.assertRaisesRegex(gate.CheckError, 'cannot execute'):
                gate.check_runtime('required', ('PE', 'x86_64'), Path('wallet'))

    def test_packaging_calls_gate_without_gating_ordinary_builds(self):
        root = SCRIPT.parents[2]
        makefile = (root / 'Makefile.am').read_text()
        guix = (root / 'contrib/guix/libexec/build.sh').read_text()
        debian = (root / 'contrib/debian/rules').read_text()
        self.assertIn('check-release-bdb: all-recursive', makefile)
        self.assertNotIn('all-local: check-release-bdb', makefile)
        self.assertIn('contrib/devtools/check-release-bdb.py', makefile)
        self.assertIn('test/lint/test_release_bdb_artifacts.py', makefile)
        native = makefile.split('if BUILD_DARWIN\n', 1)[1].split('else !BUILD_DARWIN', 1)[0]
        self.assertIn('check-release-bdb', native.split('$(OSX_DEPLOY_SCRIPT)', 1)[0])
        deployed = native.split('$(OSX_DEPLOY_SCRIPT)', 1)[1]
        self.assertLess(deployed.index('check-release-bdb'), deployed.index('make_archive'))
        self.assertIn('RELEASE_BDB_SCAN_DIR=', deployed)
        self.assertIn('check-release-bdb RELEASE_BDB_RUNTIME=auto', guix)
        staged = guix.split('# Check the final stripped', 1)[1]
        self.assertIn('BITCOIN_WALLET_BIN=', staged.split('# Finally,', 1)[0])
        self.assertIn('override_dh_builddeb:', debian)
        self.assertLess(debian.index('check-release-bdb.py'), debian.index('\n\tdh_builddeb'))
        self.assertIn('--runtime required', debian)

    def test_release_requires_wallet_and_bdb_defines(self):
        gate = self.gate
        gate.check_config('#define ENABLE_WALLET 1\n#define USE_BDB 1\n')
        for text in ('', '#define ENABLE_WALLET 1\n',
                     '#define USE_BDB 1\n',
                     '#define ENABLE_WALLET 1\n#define USE_BDB 0\n'):
            with self.subTest(text=text), self.assertRaises(gate.CheckError):
                gate.check_config(text)


if __name__ == '__main__':
    unittest.main()
