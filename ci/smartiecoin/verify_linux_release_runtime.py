# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Verify Linux archive safety and direct DT_NEEDED allowlisting/system paths.

This does not verify ABI compatibility or the transitive dependency closure.
"""

import argparse
import os
import re
import subprocess
import sys
import tarfile
from pathlib import Path

BINARIES = (
    'smartiecoind', 'smartiecoin-cli', 'smartiecoin-tx',
    'smartiecoin-util', 'smartiecoin-wallet', 'smartiecoin-qt',
)
BASE_SONAMES = frozenset({'libm.so.6', 'libgcc_s.so.1', 'libc.so.6'})
INTERPRETER_SONAME = 'ld-linux-x86-64.so.2'
PACKAGE_PATTERN = re.compile(r'^[a-z0-9][a-z0-9.+-]*$')
SONAME_PATTERN = re.compile(r'^lib[a-zA-Z0-9][a-zA-Z0-9.+_-]*\.so(?:\.\d+)*$')
PACKAGE_ROOT_PATTERN = re.compile(r'^smartiecoin-\d+\.\d+\.\d+-linux64$')
MAX_ARCHIVE_ENTRIES = 9
MAX_ARCHIVE_BYTES = 512 * 1024 * 1024
MAX_TOTAL_BYTES = 512 * 1024 * 1024
MAX_BINARY_BYTES = 256 * 1024 * 1024
MAX_README_BYTES = 1024 * 1024
COPY_CHUNK_BYTES = 1024 * 1024


class VerificationError(RuntimeError):
    """Raised when an archive, manifest, or direct dependency check fails."""


def parse_runtime_manifest(path):
    """Return ordered apt-package -> SONAME mappings from a strict TSV manifest."""
    try:
        lines = Path(path).read_text(encoding='ascii').splitlines()
    except (OSError, UnicodeError) as error:
        raise VerificationError(f'cannot read runtime manifest: {error}') from error
    if not lines:
        raise VerificationError('runtime manifest is empty')

    result = {}
    sonames = set()
    for line_number, line in enumerate(lines, start=1):
        fields = line.split('\t')
        if len(fields) != 2:
            raise VerificationError(f'malformed runtime manifest line {line_number}')
        package, soname = fields
        if not PACKAGE_PATTERN.fullmatch(package) or not SONAME_PATTERN.fullmatch(soname):
            raise VerificationError(f'invalid package/SONAME on manifest line {line_number}')
        if package in result or soname in sonames:
            raise VerificationError(f'duplicate package or SONAME on manifest line {line_number}')
        result[package] = soname
        sonames.add(soname)
    return result


def parse_dynamic_section(output):
    """Read DT_NEEDED names and reject runtime search paths from an ELF dump."""
    needed = set(re.findall(r'\(NEEDED\).*?Shared library: \[([^\]]+)\]', output))
    if not needed:
        raise VerificationError('ELF dynamic section contains no DT_NEEDED entries')
    if re.search(r'\((?:RPATH|RUNPATH)\)', output):
        raise VerificationError('release ELF contains an RPATH/RUNPATH')
    if any(not re.fullmatch(r'[A-Za-z0-9._+-]+', name) for name in needed):
        raise VerificationError('ELF contains an invalid DT_NEEDED name')
    return needed


def parse_ldconfig_cache(output):
    """Map x86-64 SONAMEs to their system-library paths."""
    result = {}
    pattern = re.compile(r'^\s*(\S+)\s+\(([^)]*)\)\s+=>\s+(\S+)\s*$')
    for line in output.splitlines():
        match = pattern.match(line)
        if not match:
            continue
        soname, abi, path = match.groups()
        if 'x86-64' not in abi and 'x86_64' not in abi:
            continue
        result.setdefault(soname, []).append(path)
    if not result:
        raise VerificationError('ldconfig returned no x86-64 libraries')
    return result


def is_system_library_path(path):
    """Accept only existing libraries in the standard /lib or /usr/lib trees."""
    try:
        resolved = Path(path).resolve(strict=True)
    except (OSError, RuntimeError):
        return False
    if not resolved.is_file():
        return False
    roots = (Path('/lib').resolve(), Path('/usr/lib').resolve())
    return any(resolved == root or root in resolved.parents for root in roots)


def run_command(arguments):
    """Run a trusted inspection tool with loader injection variables cleared."""
    environment = {'PATH': '/usr/bin:/bin:/usr/sbin:/sbin', 'HOME': '/nonexistent', 'LC_ALL': 'C'}
    try:
        result = subprocess.run(
            arguments, check=False, capture_output=True, text=True,
            timeout=30, env=environment,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise VerificationError(f'cannot run {arguments[0]}: {error}') from error
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise VerificationError(f'{arguments[0]} failed ({result.returncode}): {detail}')
    if result.stderr.strip():
        raise VerificationError(f'{arguments[0]} emitted stderr: {result.stderr.strip()}')
    return result.stdout


def _validate_elf_header(binary_path):
    output = run_command(['readelf', '-h', str(binary_path)])
    if not re.search(r'^\s*Class:\s+ELF64\s*$', output, re.MULTILINE):
        raise VerificationError(f'{binary_path.name} is not ELF64')
    if not re.search(r"^\s*Data:\s+2's complement, little endian\s*$", output, re.MULTILINE):
        raise VerificationError(f'{binary_path.name} is not little-endian ELF')
    if not re.search(r'^\s*Machine:\s+Advanced Micro Devices X86-64\s*$', output, re.MULTILINE):
        raise VerificationError(f'{binary_path.name} is not x86-64')
    type_match = re.search(r'^\s*Type:\s+(DYN|EXEC)\b', output, re.MULTILINE)
    if type_match is None:
        raise VerificationError(f'{binary_path.name} is not an executable ELF')

    program_headers = run_command(['readelf', '-l', str(binary_path)])
    interpreters = re.findall(
        r'\[Requesting program interpreter:\s*([^\]]+)\]', program_headers,
    )
    if len(interpreters) != 1:
        raise VerificationError(f'{binary_path.name} must have exactly one PT_INTERP')
    interpreter = interpreters[0].strip()
    if Path(interpreter).name != 'ld-linux-x86-64.so.2':
        raise VerificationError(f'{binary_path.name} has an unexpected ELF interpreter: {interpreter}')
    return type_match.group(1), interpreter


def _validate_archive_and_extract(archive_path, staging_dir, packages):
    """Reject unsafe tar members, then copy only the six regular ELF files."""
    archive_path = Path(archive_path)
    if archive_path.is_symlink() or not archive_path.is_file():
        raise VerificationError('release archive must be a regular file, not a symlink')
    try:
        archive_size = archive_path.stat().st_size
    except OSError as error:
        raise VerificationError(f'cannot stat release archive: {error}') from error
    if archive_size <= 0 or archive_size > MAX_ARCHIVE_BYTES:
        raise VerificationError('release archive is empty or exceeds the compressed size limit')
    suffix = '.tar.gz'
    if not archive_path.name.endswith(suffix):
        raise VerificationError('release archive must end in .tar.gz')
    package_root = archive_path.name[:-len(suffix)]
    if not PACKAGE_ROOT_PATTERN.fullmatch(package_root):
        raise VerificationError(f'unexpected package root: {package_root}')

    root_dir = package_root
    bin_dir = f'{package_root}/bin'
    readme_name = f'{package_root}/README.txt'
    binary_paths = {f'{bin_dir}/{name}': name for name in BINARIES}
    expected_dirs = {root_dir, bin_dir}
    expected_files = {readme_name, *binary_paths}
    expected_names = expected_dirs | expected_files

    staging_dir = Path(staging_dir)
    staging_dir.mkdir(parents=True, exist_ok=True)
    if any(staging_dir.iterdir()):
        raise VerificationError('runtime staging directory must be empty')

    seen = set()
    total_size = 0
    readme_text = None
    extracted = {}
    try:
        with tarfile.open(archive_path, mode='r|gz') as archive:
            for member in archive:
                name = member.name
                if len(seen) >= MAX_ARCHIVE_ENTRIES:
                    raise VerificationError('release archive contains too many entries')
                if not name or name.startswith('/') or '\\' in name:
                    raise VerificationError(f'unsafe archive path: {name!r}')
                if any(part in ('', '.', '..') for part in name.split('/')):
                    raise VerificationError(f'unsafe archive path: {name!r}')
                if name in seen:
                    raise VerificationError(f'duplicate archive member: {name}')
                seen.add(name)
                if name not in expected_names:
                    raise VerificationError(f'unexpected archive member: {name}')

                if name in expected_dirs:
                    if not member.isdir():
                        raise VerificationError(f'expected directory member: {name}')
                    continue
                if not member.isreg():
                    raise VerificationError(f'archive member is not a regular file: {name}')
                if member.size <= 0:
                    raise VerificationError(f'empty archive member: {name}')
                size_limit = MAX_README_BYTES if name == readme_name else MAX_BINARY_BYTES
                if member.size > size_limit:
                    raise VerificationError(f'oversized archive member: {name}')
                total_size += member.size
                if total_size > MAX_TOTAL_BYTES:
                    raise VerificationError('release archive expands beyond size limit')

                source = archive.extractfile(member)
                if source is None:
                    raise VerificationError(f'cannot read regular archive member: {name}')
                if name == readme_name:
                    contents = source.read(MAX_README_BYTES + 1)
                    if len(contents) != member.size:
                        raise VerificationError('README length does not match archive metadata')
                    try:
                        readme_text = contents.decode('utf-8')
                    except UnicodeDecodeError as error:
                        raise VerificationError('README is not valid UTF-8') from error
                    continue

                binary_name = binary_paths[name]
                if not member.mode & 0o111:
                    raise VerificationError(f'release binary is not executable in the archive: {name}')
                destination = staging_dir / binary_name
                copied = 0
                with destination.open('xb') as output:
                    while True:
                        chunk = source.read(COPY_CHUNK_BYTES)
                        if not chunk:
                            break
                        copied += len(chunk)
                        if copied > size_limit:
                            raise VerificationError(f'archive member exceeds size limit: {name}')
                        output.write(chunk)
                if copied != member.size:
                    raise VerificationError(f'archive member length mismatch: {name}')
                extracted[binary_name] = destination
    except (OSError, tarfile.TarError, EOFError) as error:
        raise VerificationError(f'cannot safely read release archive: {error}') from error

    if seen != expected_names:
        missing = sorted(expected_names - seen)
        raise VerificationError(f'release archive is missing required members: {missing}')
    expected_readme_line = f"Debian/Ubuntu: sudo apt-get install {' '.join(packages)}"
    if readme_text is None or 'Qt5 is statically linked' not in readme_text or expected_readme_line not in readme_text:
        raise VerificationError('release README does not document the runtime package manifest')
    return extracted


def verify_release_archive(archive_path, manifest_path, staging_dir):
    """Verify exact archive contents, direct DT_NEEDED names, and system paths."""
    packages = parse_runtime_manifest(manifest_path)
    extracted = _validate_archive_and_extract(archive_path, staging_dir, packages)
    expected_manifest_sonames = set(packages.values())
    cache = parse_ldconfig_cache(run_command(['ldconfig', '-p']))
    report = {}

    for name in BINARIES:
        binary_path = extracted[name]
        elf_type, interpreter = _validate_elf_header(binary_path)
        if not is_system_library_path(interpreter):
            raise VerificationError(f'{name} PT_INTERP is not a system loader: {interpreter}')
        dynamic_output = run_command(['readelf', '-d', str(binary_path)])
        if elf_type == 'DYN' and not re.search(r'\(FLAGS_1\).*?\bPIE\b', dynamic_output):
            raise VerificationError(f'{name} is ET_DYN but lacks the PIE executable flag')
        needed = parse_dynamic_section(dynamic_output)
        expected = BASE_SONAMES | (expected_manifest_sonames if name == 'smartiecoin-qt' else set())
        missing = sorted(expected - needed)
        unexpected = sorted((needed - expected) - {INTERPRETER_SONAME})
        if missing or unexpected:
            raise VerificationError(f'{name} DT_NEEDED mismatch; missing={missing}, unexpected={unexpected}')
        for soname in sorted(needed - {INTERPRETER_SONAME}):
            paths = cache.get(soname, [])
            if not any(is_system_library_path(path) for path in paths):
                raise VerificationError(f'{soname} is not resolved by installed system runtime packages')
        report[name] = needed
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--staging-dir', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        report = verify_release_archive(args.archive, args.manifest, args.staging_dir)
    except VerificationError as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1
    for name in BINARIES:
        print(f'Runtime SONAMEs for {name}: {", ".join(sorted(report[name]))}')
    print(f'Linux release direct DT_NEEDED allowlisting/system-path checks passed: {args.archive.name}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
