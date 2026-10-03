#!/usr/bin/env python3
"""Trusted build-bundle reader. Never execute or use tar.extract on archive data.

Run with Python isolated mode (-I), from a trusted checkout, as an unprivileged
user in a private workspace. Existing destinations are deliberately not replaced.

Format v2: only build-ci/smartiecoin-TARGET, regular files/directories, ASCII
canonical paths, GNU long names <=4096 bytes. No PAX, sparse files, symlinks,
hardlinks, special files, special mode bits or group/world-writable entries.
Ownership/timestamps are not restored; modes become 0644/0755. Limits: 8 GiB
compressed, 12 GiB tar, 10 GiB total payload, 2 GiB/file, 100000 members.
Validation and decompression finish before writing the staged tree; install is
one rename after all payloads have been read. Budget disk for both tar and tree.

The inspected linux64 CI bundle contained outer configure outputs, Cargo cache
hardlinks, and two target runner symlinks via build-ci/test into workspace/test.
Creation omits outer outputs and Cargo target/, and materializes only those two
observed runner links. All other links fail closed. Old whole-build-ci bundles
must be rebuilt, not silently filtered. No downloaded code is executed here.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile

TARGETS = frozenset(('arm-linux', 'linux64', 'linux64_asan', 'linux64_fuzz',
                    'linux64_multiprocess', 'linux64_nowallet', 'linux64_sqlite',
                    'linux64_tsan', 'linux64_ubsan', 'linux64_valgrind', 'mac',
                    's390x', 'win64'))
MAX_ARCHIVE = 8 * 1024**3
MAX_TAR = 12 * 1024**3
MAX_FILE = 2 * 1024**3
MAX_TOTAL = 10 * 1024**3
MAX_ENTRIES = 100000


def identifiers():
    target = os.environ.get('BUILD_TARGET', '')
    key = os.environ.get('BUNDLE_KEY', '')
    if target not in TARGETS:
        raise ValueError('invalid BUILD_TARGET')
    if not re.fullmatch(r'build-' + re.escape(target) + r'-[0-9a-f]{8,40}', key):
        raise ValueError('invalid BUNDLE_KEY')
    return target, key


def regular_open(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    stream = os.fdopen(fd, 'rb')
    if not stat.S_ISREG(os.fstat(fd).st_mode):
        stream.close()
        raise ValueError('archive/input must be a regular file')
    return stream


def check_tar_framing(raw):
    """Bound metadata before tarfile can allocate/recursively parse extensions.

    Only plain GNU/ustar records and a single bounded GNU long-name record are
    supported. PAX, sparse, links, concatenated archives and truncated EOF fail.
    """
    with raw.open('rb') as stream:
        length = raw.stat().st_size
        pending_name = False
        records = 0
        while True:
            header = stream.read(512)
            if header == b'\0' * 512:
                remaining = length - stream.tell()
                if remaining < 512 or remaining % 512 or pending_name:
                    raise ValueError('truncated tar terminator')
                while True:
                    chunk = stream.read(1024 * 1024)
                    if not chunk:
                        return
                    if any(chunk):
                        raise ValueError('nonzero trailing tar payload')
            if len(header) != 512:
                raise ValueError('truncated tar header')
            member = tarfile.TarInfo.frombuf(header, 'utf-8', 'strict')
            records += 1
            if records > MAX_ENTRIES * 2:
                raise ValueError('too many tar records')
            if member.type == tarfile.GNUTYPE_LONGNAME:
                if pending_name or not 0 < member.size <= 4096:
                    raise ValueError('invalid long-name metadata')
                pending_name = True
            else:
                if member.type not in (tarfile.REGTYPE, tarfile.AREGTYPE, tarfile.DIRTYPE):
                    raise ValueError('unsupported archive type/link/metadata')
                pending_name = False
                if member.isdir() and member.size:
                    raise ValueError('directory with payload')
                if not 0 <= member.size <= MAX_FILE:
                    raise ValueError('archive member too large')
            skip = ((member.size + 511) // 512) * 512
            if stream.tell() + skip > length:
                raise ValueError('truncated archive member')
            stream.seek(skip, 1)


def validate_path(member, root):
    name = member.name
    parts = name.split('/')
    if (len(name) > 4096 or len(parts) > 32 or
            any(len(part) > 255 for part in parts) or
            any(ord(char) < 32 or ord(char) > 126 for char in name)):
        raise ValueError('invalid archive path encoding/length')
    if name == root and not member.isdir():
        raise ValueError('subtree root must be a directory')
    if (not name.startswith(root + '/') and name != root) or any(
            part in ('', '.', '..') for part in parts) or '\\' in name:
        raise ValueError('archive path outside intended subtree')


def validate_budget(member, total, count):
    if member.size < 0 or member.size > MAX_FILE:
        raise ValueError('archive member too large')
    total += member.size
    if total > MAX_TOTAL or count >= MAX_ENTRIES:
        raise ValueError('archive resource limit exceeded')
    return total


def unpack(archive, workspace, root):
    # workspace must be private to this user: other processes with the same UID
    # are outside this boundary. No PR code may run concurrently with extraction.
    if workspace.is_symlink() or not workspace.is_dir():
        raise ValueError('unsafe workspace directory')
    with regular_open(archive) as source, tempfile.TemporaryDirectory(
            prefix='.bundle-', dir=workspace) as temporary:
        if os.fstat(source.fileno()).st_size > MAX_ARCHIVE:
            raise ValueError('compressed archive too large')
        stage = Path(temporary)
        raw = stage / 'archive.tar'
        with raw.open('wb') as output:
            proc = subprocess.Popen(['zstd', '-q', '-dc'], stdin=source, stdout=subprocess.PIPE)
            assert proc.stdout is not None
            try:
                size = 0
                while True:
                    chunk = proc.stdout.read(1024 * 1024)
                    if not chunk:
                        break
                    size += len(chunk)
                    if size > MAX_TAR:
                        raise ValueError('uncompressed archive too large')
                    output.write(chunk)
                if proc.wait() != 0:
                    raise ValueError('invalid zstd archive')
            finally:
                proc.stdout.close()
                if proc.poll() is None:
                    proc.kill()
                proc.wait()
        check_tar_framing(raw)
        with tarfile.open(raw, 'r:') as bundle:
            entries = {}
            total = 0
            for member in bundle:
                validate_path(member, root)
                name = member.name
                if name in entries:
                    raise ValueError('duplicate archive path')
                if not (member.isdir() or member.isreg()):
                    raise ValueError('unsupported archive type/link')
                if member.mode & ~0o777 or member.mode & 0o022:
                    raise ValueError('unsafe archive mode')
                total = validate_budget(member, total, len(entries))
                entries[name] = member
            for name in entries:
                for parent in Path(name).parents:
                    if str(parent) in entries and not entries[str(parent)].isdir():
                        raise ValueError('non-directory archive ancestor')
            if not entries:
                raise ValueError('empty archive')
            tree = stage / 'tree'
            tree.mkdir()
            for name, member in entries.items():
                path = tree / name
                path.parent.mkdir(parents=True, exist_ok=True)
                if member.isdir():
                    path.mkdir(exist_ok=True)
                else:
                    src = bundle.extractfile(member)
                    if src is None:
                        raise ValueError('missing regular member payload')
                    with src, path.open('xb') as dst:
                        shutil.copyfileobj(src, dst)
                    path.chmod(0o755 if member.mode & 0o111 else 0o644)
            for directory, _, _ in os.walk(tree):
                Path(directory).chmod(0o755)
            build = workspace / 'build-ci'
            if build.is_symlink() or (build.exists() and not build.is_dir()):
                raise ValueError('unsafe build-ci destination')
            destination = workspace / root
            if destination.exists() or destination.is_symlink():
                raise ValueError('destination already exists')
            build.mkdir(exist_ok=True)
            os.rename(tree / root, destination)


def create(archive, workspace, root):
    """Bundle only the test tree; materialize the two observed runner links.

    Rust's target/ is compilation cache, not functional-test input. Omitting it
    also removes Cargo hardlinks. Unknown symlinks fail closed, not dereferenced.
    """
    base = workspace / root
    if base.is_symlink() or not base.is_dir() or (workspace / 'build-ci').is_symlink():
        raise ValueError('unsafe or missing source directory')
    with tempfile.TemporaryDirectory(prefix='.bundle-', dir=workspace) as temporary:
        raw = Path(temporary) / 'archive.tar'
        total = count = 0
        with tarfile.open(raw, 'w', format=tarfile.GNU_FORMAT, dereference=True) as bundle:
            for directory, dirs, files in os.walk(base, followlinks=False):
                dirs[:] = sorted(d for d in dirs if d not in ('.deps', '.libs') and
                                 not (Path(directory) == base and d == 'target'))
                paths = [Path(directory)] + [Path(directory) / f for f in sorted(files)]
                if any((Path(directory) / d).is_symlink() for d in dirs):
                    raise ValueError('unsupported source directory symlink')
                for path in paths:
                    if path.suffix in ('.a', '.o'):
                        continue
                    relative = path.relative_to(base).as_posix()
                    source = path
                    if path.is_symlink():
                        if relative not in ('test/functional/test_runner.py', 'test/fuzz/test_runner.py'):
                            raise ValueError('unsupported source symlink')
                        expected = workspace / relative
                        if os.readlink(path) != '../../../' + relative or path.resolve() != expected.resolve():
                            raise ValueError('unexpected runner symlink')
                        source = expected
                    info = bundle.gettarinfo(str(source), arcname=path.relative_to(workspace).as_posix())
                    validate_path(info, root)
                    total = validate_budget(info, total, count)
                    count += 1
                    info.uid = info.gid = 0
                    info.uname = info.gname = ''
                    info.mode = 0o755 if info.isdir() or info.mode & 0o111 else 0o644
                    if info.isdir():
                        bundle.addfile(info)
                    elif info.isreg():
                        with regular_open(source) as stream:
                            bundle.addfile(info, stream)
                    else:
                        raise ValueError('unsupported source type')
                    if bundle.offset > MAX_TAR:
                        raise ValueError('uncompressed archive too large')
        # Include tar EOF/record padding and validate GNU long-name framing
        # (its terminating NUL, and directory slash, also consume the budget).
        if raw.stat().st_size > MAX_TAR:
            raise ValueError('uncompressed archive too large')
        check_tar_framing(raw)
        compressed = Path(temporary) / 'archive.tar.zst'
        subprocess.run(['zstd', '-q', '-T0', '-5', str(raw), '-o', str(compressed)], check=True)
        if compressed.stat().st_size > MAX_ARCHIVE:
            raise ValueError('compressed archive too large')
        # Hardlink installation is no-clobber, including dangling symlinks.
        os.link(compressed, archive)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('verb', choices=('create', 'extract'))
    parser.add_argument('--workspace', type=Path, default=Path.cwd())
    parser.add_argument('--archive-dir', type=Path, default=Path.cwd())
    args = parser.parse_args()
    try:
        target, key = identifiers()
        action = create if args.verb == 'create' else unpack
        action(args.archive_dir / (key + '.tar.zst'), args.workspace,
               'build-ci/smartiecoin-' + target)
    except (ValueError, OSError, tarfile.TarError, subprocess.CalledProcessError) as error:
        parser.exit(1, 'bundle: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
