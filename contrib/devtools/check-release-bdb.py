#!/usr/bin/env python3
# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Fail-closed BDB checks for release artifacts, not ordinary developer builds.

Native release QA (also rerun against unpacked final packages)::
  python3 check-release-bdb.py --wallet bin/smartiecoin-wallet \
      --node bin/smartiecoind --node bin/smartiecoin-qt --scan-dir bin

Build/package callers additionally pass --config-header. OBJDUMP may select a
cross tool (GNU for ELF/PE, LLVM for all three formats). Only linked constants
are scanned: .rodata (ELF), __cstring (Mach-O), .rdata (PE), NEVER PE .rsrc.

--runtime=required is the default: inability to execute is a failure, not a skip.
Cross builders can explicitly use --runtime=auto: native execution is still
mandatory on matching OS/CPU; other targets receive STATIC-ONLY status and
must pass this command with --runtime=required on the target before release.
A static-only report is NOT release approval. Reports bind evidence to hashes;
a rebuild, strip, signing, or packaging change requires another artifact check.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import subprocess
import sys
import tempfile


class CheckError(RuntimeError):
    """Required release evidence is absent or violates policy."""


def run_command(argv):
    try:
        result = subprocess.run(argv, capture_output=True, text=True, encoding='utf-8',
                                errors='replace', timeout=120, env={**os.environ, 'LC_ALL': 'C'})
    except (OSError, subprocess.TimeoutExpired) as error:
        raise CheckError(f'cannot execute required check {argv[0]}: {error}') from error
    if result.returncode or result.stderr.strip() or not result.stdout.strip():
        raise CheckError(f'required check failed: {argv[0]} (exit {result.returncode}): {result.stderr.strip()}')
    return result.stdout


def check_wallet_result(output, header):
    if not {'Format: bdb', 'Descriptors: no'}.issubset(output.splitlines()):
        raise CheckError('native wallet did not create/reopen a legacy BDB wallet')
    keypool = re.search(r'^Keypool Size: ([0-9]+)$', output, re.MULTILINE)
    if not keypool or int(keypool[1]) < 1:
        raise CheckError('native wallet has no persisted keys')
    if len(header) != 20 or not any(
            int.from_bytes(header[12:16], endian) == 0x053162 and
            int.from_bytes(header[16:20], endian) == 9 for endian in ('little', 'big')):
        raise CheckError('native wallet is not Berkeley DB Btree version 9')
    return int(keypool[1])


def runtime_qa(wallet):
    # No existing datadir/wallet is accepted. The offline tool never starts a node.
    with tempfile.TemporaryDirectory(prefix='release-bdb-qa-') as directory:
        root = Path(directory).resolve()
        data = root / 'data'
        data.mkdir()
        target = root / 'wallet'
        command = [str(wallet.resolve()), '-regtest', f'-datadir={data}',
                   f'-wallet={target}', '-printtoconsole=1']
        keypools = []
        for operation in ('create', 'info'):
            output = run_command(command + [operation])
            with (target / 'wallet.dat').open('rb') as created:
                keypools.append(check_wallet_result(output, created.read(20)))
        if keypools[0] != keypools[1]:
            raise CheckError('keypool changed on native wallet reopen')
    return {'status': 'passed', 'btree_version': 9,
            'create_reopen': True, 'persisted_keypool': keypools[0]}


def check_config(text):
    for define in ('ENABLE_WALLET', 'USE_BDB'):
        if not re.search(r'^\s*#define\s+' + define + r'\s+1\s*$', text, re.MULTILINE):
            raise CheckError(f'release requires {define}=1')


def bdb_library(name):
    return re.match(r'^(?:lib)?(?:db(?:[_.0-9-]|$)|berkeley)', name.rsplit('/', 1)[-1], re.IGNORECASE)


def check_dependencies(kind, text):
    if kind == 'MACHO':
        libraries = re.findall(r'^\s+(\S+)\s+\(compatibility version ', text, re.MULTILINE)
        valid = bool(libraries)
    elif kind == 'PE':
        libraries = re.findall(r'^\s*DLL Name:\s*(\S+)', text, re.MULTILINE)
        valid = bool(libraries) and bool(re.search(r'file format (?:coff|pei)-', text))
    else:
        libraries = re.findall(r'^\s*NEEDED\s+(\S+)', text, re.MULTILINE)
        # Fully static ELF has no NEEDED entries, but must have parsed headers.
        valid = 'file format elf' in text and 'Program Header:' in text
    if not valid:
        raise CheckError(f'unrecognized or empty {kind} dependency inspection')
    for library in libraries:
        if bdb_library(library):
            raise CheckError(f'dynamic BDB dependency: {library}')
    return libraries


def decode_section(text, section):
    # GNU and LLVM objdump -s, including Mach-O's segment,section notation.
    header = re.search(r'^Contents of section (?:__TEXT,)?' + re.escape(section) + r':$', text, re.MULTILINE)
    if not header:
        raise CheckError(f'missing linked constants section {section}')
    contents = bytearray()
    next_address = None
    for line in text[header.end():].splitlines():
        if not line.strip():
            continue
        match = re.match(r'^\s*([0-9a-fA-F]+)\s+((?:[0-9a-fA-F]{2}){1,4}(?: (?:[0-9a-fA-F]{2}){1,4}){0,3})(?:\s{2,}|$)', line)
        if not match:
            raise CheckError(f'unrecognized {section} dump line')
        address = int(match[1], 16)
        chunk = bytes.fromhex(match[2])
        if next_address is not None and address != next_address:
            raise CheckError(f'non-contiguous {section} dump')
        next_address = address + len(chunk)
        contents.extend(chunk)
    if not contents:
        raise CheckError(f'empty linked constants section {section}')
    return bytes(contents)


def check_constants(contents, role):
    versions = set(re.findall(rb'Berkeley DB ([0-9]+\.[0-9]+\.[0-9]+)', contents))
    if versions != {b'4.8.30'}:
        raise CheckError(f'expected only linked Berkeley DB 4.8.30, found {sorted(versions)}')
    marker = b'-wallet=<path>\x00' if role == 'node' else b'Get wallet info\x00'
    if marker not in contents:
        raise CheckError(f'{role} wallet support missing from linked constants')


def sniff_format(header):
    """Classify a 64-byte header: (kind, arch), 'FAT' for universal Mach-O, or None
    when the file is not a final release binary (build objects, archives, stray files)."""
    if len(header) < 64:
        return ('SHORT', None)
    if header[:4] == b'\x7fELF' and header[5] in (1, 2):
        endian = 'little' if header[5] == 1 else 'big'
        if int.from_bytes(header[16:18], endian) not in (2, 3):  # ET_EXEC / ET_DYN only
            return None  # relocatable object or core, never a release artifact
        cpu = int.from_bytes(header[18:20], endian)
        return 'ELF', {3: 'i386', 40: 'arm', 62: 'x86_64', 183: 'aarch64',
                       21: 'ppc64le' if endian == 'little' else 'ppc64', 243: 'riscv64'}.get(cpu)
    if header[:4] == b'\xcf\xfa\xed\xfe':
        if int.from_bytes(header[12:16], 'little') not in (2, 6, 8):  # MH_EXECUTE / DYLIB / BUNDLE
            return None  # MH_OBJECT / MH_CORE / MH_DSYM
        return 'MACHO', {0x1000007: 'x86_64', 0x100000c: 'aarch64'}.get(int.from_bytes(header[4:8], 'little'))
    if header[:4] == b'\xca\xfe\xba\xbe':
        return 'FAT', None  # universal binary: cannot be checked in place, fail closed
    if header[:2] == b'MZ':
        return 'PE', None  # architecture resolved by binary_target from the PE header
    return None


def binary_target(path):
    # Header sniffing selects the inspector, which must then parse the real file.
    # Named artifacts must be final executables or shared libraries; build objects,
    # archives and universal binaries fail closed instead of being checked partially.
    with path.open('rb') as binary:
        header = binary.read(64)
        info = sniff_format(header)
        if info is None:
            raise CheckError(f'not a release executable (build object, archive or unknown format): {path}')
        kind, arch = info
        if kind == 'SHORT':
            raise CheckError(f'truncated executable: {path}')
        if kind == 'PE':
            binary.seek(int.from_bytes(header[60:64], 'little'))
            pe = binary.read(6)
            if pe[:4] != b'PE\x00\x00':
                raise CheckError(f'invalid PE signature: {path}')
            arch = {0x8664: 'x86_64', 0x14c: 'i386', 0xaa64: 'aarch64'}.get(int.from_bytes(pe[4:6], 'little'))
    if kind == 'FAT' or not arch:
        raise CheckError(f'unsupported executable architecture: {path}')
    return kind, arch


def is_native(target):
    system = platform.system()
    kind = 'MACHO' if system == 'Darwin' else 'ELF' if system == 'Linux' else 'PE' if system == 'Windows' else None
    machine = platform.machine().lower()
    arch = {'arm64': 'aarch64', 'amd64': 'x86_64', 'armv7l': 'arm',
            'i686': 'i386', 'powerpc64le': 'ppc64le', 'powerpc64': 'ppc64'}.get(machine, machine)
    return target == (kind, arch)


def check_runtime(mode, target, wallet):
    if mode == 'required' or is_native(target):
        return runtime_qa(wallet)
    return {'status': 'required-on-native-target',
            'instruction': 'STATIC ONLY: rerun with --runtime=required on target OS/CPU before release'}


def inspect_artifact(path, role, objdump):
    kind, arch = binary_target(path)
    if bdb_library(path.name):
        raise CheckError(f'packaged dynamic BDB library: {path}')
    options = ['--macho', '--dylibs-used'] if kind == 'MACHO' else ['-p']
    libraries = check_dependencies(kind, run_command(objdump + options + [str(path)]))
    if role != 'other':
        section = {'MACHO': '__cstring', 'PE': '.rdata', 'ELF': '.rodata'}[kind]
        contents = decode_section(run_command(objdump + ['-s', '-j', section, str(path)]), section)
        check_constants(contents, role)
    return {'file': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'format': kind, 'arch': arch, 'role': role, 'dependencies': libraries}


def bundled_binaries(directory):
    if not directory.is_dir():
        raise CheckError(f'missing artifact directory: {directory}')
    binaries = []
    for path in sorted(directory.rglob('*')):
        if not path.is_file() or path.suffix in ('.dbg', '.debug') or any(p.endswith('.dSYM') for p in path.parts):
            continue
        with path.open('rb') as candidate:
            info = sniff_format(candidate.read(64))
        if info is None or info[0] == 'SHORT':
            continue  # build objects, archives and stray files are not release artifacts
        binaries.append(path)
    if not binaries:
        raise CheckError(f'no executable artifacts in {directory}')
    return binaries


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--config-header', type=Path, help='require ENABLE_WALLET and USE_BDB in this build header')
    parser.add_argument('--wallet', type=Path, required=True)
    parser.add_argument('--node', type=Path, action='append', required=True, help='repeat for every shipped daemon/GUI')
    parser.add_argument('--other', type=Path, action='append', default=[], help='other binaries: dependency check only')
    parser.add_argument('--scan-dir', type=Path, action='append', default=[], help='also inspect all bundled executables/shared libraries')
    parser.add_argument('--runtime', choices=('required', 'auto'), default='required')
    parser.add_argument('--report', type=Path, help='JSON evidence, including explicit cross-target QA debt')
    args = parser.parse_args(argv)
    report = {'status': 'failed', 'artifacts': [], 'native_qa': {'status': 'not-run'}}
    try:
        if args.config_header:
            check_config(args.config_header.read_text(encoding='utf-8'))
        objdump = shlex.split(os.environ.get('OBJDUMP', 'objdump'))
        if not objdump:
            raise CheckError('empty OBJDUMP inspector command')
        artifacts = [(args.wallet, 'wallet')] + [(p, 'node') for p in args.node] + [(p, 'other') for p in args.other]
        named = {p.resolve() for p, _ in artifacts}
        for directory in args.scan_dir:
            for path in bundled_binaries(directory):
                if path.resolve() not in named:
                    artifacts.append((path, 'other'))
                    named.add(path.resolve())
        for path, role in artifacts:
            try:
                report['artifacts'].append(inspect_artifact(path, role, objdump))
            except CheckError as error:
                raise CheckError(f'{path}: {error}') from error
        targets = {(item['format'], item['arch']) for item in report['artifacts'] if item['role'] != 'other'}
        if len(targets) != 1:
            raise CheckError('wallet and node artifacts have different target OS/CPU')
        report['native_qa'] = check_runtime(args.runtime, targets.pop(), args.wallet)
        report['status'] = 'passed' if report['native_qa']['status'] == 'passed' else 'static-only-native-qa-required'
    except (CheckError, OSError, ValueError) as error:
        report['error'] = str(error)
    encoded = json.dumps(report, indent=2, sort_keys=True) + '\n'
    if args.report:
        args.report.write_text(encoded, encoding='utf-8')
    print(encoded, end='')
    return 1 if report['status'] == 'failed' else 0


if __name__ == '__main__':
    sys.exit(main())
