# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Tests for static Linux release archive/runtime verification."""
import importlib.util
import io
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / 'ci/smartiecoin/verify_linux_release_runtime.py'
SPEC = importlib.util.spec_from_file_location('verify_linux_release_runtime', SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise ImportError('cannot load Linux release runtime verifier')
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)

BINARIES = (
    'smartiecoind', 'smartiecoin-cli', 'smartiecoin-tx',
    'smartiecoin-util', 'smartiecoin-wallet', 'smartiecoin-qt',
)
BASE_NEEDED = {'libm.so.6', 'libgcc_s.so.1', 'libc.so.6'}
INTERPRETER_NEEDED = 'ld-linux-x86-64.so.2'
MANIFEST_ROWS = (
    ('libfoo1', 'libfoo.so.1'),
    ('libbar2', 'libbar.so.2'),
)
ELF_HEADER = '''\
ELF Header:
  Class:                             ELF64
  Data:                              2's complement, little endian
  Type:                              DYN (Position-Independent Executable file)
  Machine:                           Advanced Micro Devices X86-64
'''
ELF_PROGRAM_HEADERS = '''\
Program Headers:
  INTERP 0x0000000000000350 0x0000000000000350 0x0000000000000350
         0x000000000000001c 0x000000000000001c R   0x1
      [Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]
'''


def dynamic_output(needed, extra='', pie=True):
    lines = [
        f' 0x0000000000000001 (NEEDED)             Shared library: [{name}]'
        for name in sorted(needed)
    ]
    if pie:
        lines.append(' 0x000000006ffffffb (FLAGS_1)            Flags: NOW PIE')
    return '\n'.join(lines) + ('\n' + extra if extra else '')


def make_archive(path, root='smartiecoin-9.9.9-linux64', symlink=None, extra=None):
    with tarfile.open(path, 'w:gz') as archive:
        for name in (f'{root}/', f'{root}/bin/'):
            info = tarfile.TarInfo(name)
            info.type = tarfile.DIRTYPE
            info.mode = 0o755
            archive.addfile(info)
        files = [f'{root}/README.txt'] + [f'{root}/bin/{name}' for name in BINARIES]
        if extra:
            files.append(f'{root}/bin/{extra}')
        for name in files:
            if name == symlink:
                info = tarfile.TarInfo(name)
                info.type = tarfile.SYMTYPE
                info.linkname = '/bin/true'
                archive.addfile(info)
                continue
            if name.endswith('/README.txt'):
                packages = ' '.join(package for package, _ in MANIFEST_ROWS)
                data = f'Qt5 is statically linked.\nDebian/Ubuntu: sudo apt-get install {packages}\n'.encode('ascii')
            else:
                data = b'fixture-not-executed'
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o755 if '/bin/' in name else 0o644
            archive.addfile(info, io.BytesIO(data))


def write_manifest(path, rows=MANIFEST_ROWS):
    path.write_text(''.join(f'{package}\t{soname}\n' for package, soname in rows), encoding='ascii')


class LinuxReleaseRuntimeTest(unittest.TestCase):
    def test_run_command_finds_sbin_tools_with_sanitized_environment(self):
        # Remap only exec search directories to a private filesystem fixture;
        # run_command, subprocess.run, and the executable itself remain real.
        get_exec_path = os.get_exec_path
        for tool_directory in ('usr/sbin', 'sbin'):
            with self.subTest(tool_directory=tool_directory), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                tool = root / tool_directory / 'ldconfig'
                tool.parent.mkdir(parents=True)
                tool.write_text(
                    '#!/bin/sh\n'
                    'printf "%s\\n" "sbin fixture: $1" "$HOME" "$LC_ALL" '
                    '"${LD_LIBRARY_PATH-unset}" "${LD_PRELOAD-unset}"\n',
                    encoding='ascii',
                )
                tool.chmod(0o755)

                def fixture_exec_path(environment=None):
                    return [str(root / entry.lstrip('/')) for entry in get_exec_path(environment)]

                with patch.dict(os.environ, {
                    'PATH': '/untrusted', 'HOME': '/untrusted', 'LC_ALL': 'invalid',
                    'LD_LIBRARY_PATH': '/untrusted', 'LD_PRELOAD': '/untrusted.so',
                }), patch.object(os, 'get_exec_path', side_effect=fixture_exec_path):
                    try:
                        output = runtime.run_command(['ldconfig', '-p'])
                    except runtime.VerificationError as error:
                        self.fail(f'trusted {tool_directory} tool was not executed: {error}')
                self.assertEqual(output, 'sbin fixture: -p\n/nonexistent\nC\nunset\nunset\n')

    def test_manifest_parses_unique_package_soname_pairs(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / 'runtime.tsv'
            write_manifest(manifest)
            self.assertEqual(runtime.parse_runtime_manifest(manifest), dict(MANIFEST_ROWS))

    def test_manifest_rejects_duplicate_sonames_and_malformed_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / 'runtime.tsv'
            write_manifest(manifest, (('libfoo1', 'libfoo.so.1'), ('libbar2', 'libfoo.so.1')))
            with self.assertRaises(runtime.VerificationError):
                runtime.parse_runtime_manifest(manifest)
            manifest.write_text('libfoo1\tlibfoo.so.1\textra\n', encoding='ascii')
            with self.assertRaises(runtime.VerificationError):
                runtime.parse_runtime_manifest(manifest)

    def test_dynamic_section_parses_needed_and_rejects_rpath(self):
        needed = BASE_NEEDED | {soname for _, soname in MANIFEST_ROWS}
        parsed = runtime.parse_dynamic_section(dynamic_output(needed))
        self.assertEqual(parsed, needed)
        rpath = ' 0x000000000000001d (RUNPATH) Library runpath: [/build/depends/lib]'
        with self.assertRaises(runtime.VerificationError):
            runtime.parse_dynamic_section(dynamic_output(needed, rpath))

    def test_elf_header_rejects_executable_without_interpreter(self):
        def fake_command(argv):
            if argv[1] == '-h':
                return ELF_HEADER
            if argv[1] == '-l':
                return 'Program Headers:\n  LOAD ...\n'
            raise AssertionError(argv)

        with patch.object(runtime, 'run_command', side_effect=fake_command):
            with self.assertRaises(runtime.VerificationError):
                runtime._validate_elf_header(Path('smartiecoin-qt'))

    def test_archive_rejects_symlink_binaries(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / 'smartiecoin-9.9.9-linux64.tar.gz'
            stage = Path(directory) / 'stage'
            stage.mkdir()
            make_archive(archive, symlink='smartiecoin-9.9.9-linux64/bin/smartiecoin-qt')
            with patch.object(runtime, 'run_command', side_effect=AssertionError('must reject before ELF inspection')):
                with self.assertRaises(runtime.VerificationError):
                    runtime.verify_release_archive(archive, self._manifest(directory), stage)

    def test_archive_rejects_outer_archive_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'smartiecoin-9.9.9-linux64.tar.gz'
            alias_directory = root / 'alias'
            alias_directory.mkdir()
            alias = alias_directory / 'smartiecoin-9.9.9-linux64.tar.gz'
            stage = root / 'stage'
            stage.mkdir()
            make_archive(archive)
            alias.symlink_to(archive)
            with self.assertRaises(runtime.VerificationError):
                runtime._validate_archive_and_extract(alias, stage, dict(MANIFEST_ROWS))

    def test_archive_rejects_unexpected_members(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / 'smartiecoin-9.9.9-linux64.tar.gz'
            stage = Path(directory) / 'stage'
            stage.mkdir()
            make_archive(archive, extra='unexpected')
            with patch.object(runtime, 'run_command', side_effect=AssertionError('must reject unexpected member')):
                with self.assertRaises(runtime.VerificationError):
                    runtime.verify_release_archive(archive, self._manifest(directory), stage)

    def test_archive_rejects_path_traversal_members(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / 'smartiecoin-9.9.9-linux64.tar.gz'
            stage = Path(directory) / 'stage'
            stage.mkdir()
            make_archive(archive, extra='../../outside')
            with patch.object(runtime, 'run_command', side_effect=AssertionError('must reject unsafe path')):
                with self.assertRaises(runtime.VerificationError):
                    runtime.verify_release_archive(archive, self._manifest(directory), stage)

    def test_verifier_accepts_exact_direct_needed_allowlist(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'smartiecoin-9.9.9-linux64.tar.gz'
            stage = root / 'stage'
            stage.mkdir()
            manifest = self._manifest(directory)
            make_archive(archive)
            all_needed = BASE_NEEDED | {soname for _, soname in MANIFEST_ROWS}
            ldconfig_output = '\n'.join(
                f'{name} (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/{name}'
                for name in sorted(all_needed)
            )

            def fake_command(argv):
                if argv[0] == 'ldconfig':
                    return ldconfig_output
                if argv[0] == 'readelf' and argv[1] == '-h':
                    return ELF_HEADER
                if argv[0] == 'readelf' and argv[1] == '-l':
                    return ELF_PROGRAM_HEADERS
                if argv[0] == 'readelf' and argv[1] == '-d':
                    name = Path(argv[-1]).name
                    return dynamic_output(all_needed if name == 'smartiecoin-qt' else BASE_NEEDED)
                raise AssertionError(argv)

            with patch.object(runtime, 'run_command', side_effect=fake_command):
                with patch.object(runtime, 'is_system_library_path', return_value=True):
                    report = runtime.verify_release_archive(archive, manifest, stage)
            self.assertEqual(set(report), set(BINARIES))

    def test_verifier_rejects_shared_object_without_pie_flag(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'smartiecoin-9.9.9-linux64.tar.gz'
            stage = root / 'stage'
            stage.mkdir()
            manifest = self._manifest(directory)
            make_archive(archive)
            all_needed = BASE_NEEDED | {INTERPRETER_NEEDED} | {soname for _, soname in MANIFEST_ROWS}
            ldconfig_output = '\n'.join(
                f'{name} (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/{name}'
                for name in sorted(all_needed)
            )

            def fake_command(argv):
                if argv[0] == 'ldconfig':
                    return ldconfig_output
                if argv[0] == 'readelf' and argv[1] == '-h':
                    return ELF_HEADER
                if argv[0] == 'readelf' and argv[1] == '-l':
                    return ELF_PROGRAM_HEADERS
                if argv[0] == 'readelf' and argv[1] == '-d':
                    name = Path(argv[-1]).name
                    needed = all_needed if name == 'smartiecoin-qt' else BASE_NEEDED | {INTERPRETER_NEEDED}
                    return dynamic_output(needed, pie=False)
                raise AssertionError(argv)

            with patch.object(runtime, 'run_command', side_effect=fake_command):
                with patch.object(runtime, 'is_system_library_path', return_value=True):
                    with self.assertRaises(runtime.VerificationError):
                        runtime.verify_release_archive(archive, manifest, stage)

    def test_verifier_rejects_unlisted_needed_library(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'smartiecoin-9.9.9-linux64.tar.gz'
            stage = root / 'stage'
            stage.mkdir()
            manifest = self._manifest(directory)
            make_archive(archive)
            needed = BASE_NEEDED | {soname for _, soname in MANIFEST_ROWS} | {'libunexpected.so.1'}
            ldconfig_output = '\n'.join(
                f'{name} (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/{name}'
                for name in sorted(needed)
            )

            def fake_command(argv):
                if argv[0] == 'ldconfig':
                    return ldconfig_output
                if argv[1] == '-h':
                    return ELF_HEADER
                if argv[1] == '-l':
                    return ELF_PROGRAM_HEADERS
                if argv[1] == '-d':
                    return dynamic_output(needed if Path(argv[-1]).name == 'smartiecoin-qt' else BASE_NEEDED)
                raise AssertionError(argv)

            with patch.object(runtime, 'run_command', side_effect=fake_command):
                with patch.object(runtime, 'is_system_library_path', return_value=True):
                    with self.assertRaisesRegex(runtime.VerificationError, 'unexpected=.*libunexpected'):
                        runtime.verify_release_archive(archive, manifest, stage)

    def test_verifier_fails_when_system_soname_is_missing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'smartiecoin-9.9.9-linux64.tar.gz'
            stage = root / 'stage'
            stage.mkdir()
            manifest = self._manifest(directory)
            make_archive(archive)
            needed = BASE_NEEDED | {soname for _, soname in MANIFEST_ROWS}
            omitted = 'libc.so.6'
            ldconfig_output = '\n'.join(
                f'{name} (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/{name}'
                for name in sorted(needed - {omitted})
            )

            def fake_command(argv):
                if argv[0] == 'ldconfig':
                    return ldconfig_output
                if argv[1] == '-h':
                    return ELF_HEADER
                if argv[1] == '-l':
                    return ELF_PROGRAM_HEADERS
                if argv[1] == '-d':
                    return dynamic_output(needed if Path(argv[-1]).name == 'smartiecoin-qt' else BASE_NEEDED)
                raise AssertionError(argv)

            with patch.object(runtime, 'run_command', side_effect=fake_command):
                with patch.object(runtime, 'is_system_library_path', return_value=True):
                    with self.assertRaisesRegex(runtime.VerificationError, 'libc.so.6 is not resolved'):
                        runtime.verify_release_archive(archive, manifest, stage)

    @staticmethod
    def _manifest(directory):
        manifest = Path(directory) / 'runtime.tsv'
        write_manifest(manifest)
        return manifest


if __name__ == '__main__':
    unittest.main()
