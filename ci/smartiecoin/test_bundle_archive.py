#!/usr/bin/env python3
"""Regression tests for the trusted, non-executing build bundle boundary."""
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
HELPER = HERE / 'bundle_archive.py'
ROOT = 'build-ci/smartiecoin-linux64'
KEY = 'build-linux64-0123abcd'


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.dest = self.work / 'destination'
        self.dest.mkdir()
        self.sentinel = self.work / 'sentinel'
        self.sentinel.write_text('unchanged')

    def run_helper(self, verb='extract', **env):
        return subprocess.run(
            ['python3', '-I', str(HELPER), verb, '--workspace', str(self.dest),
             '--archive-dir', str(self.work)],
            env={**os.environ, 'BUILD_TARGET': 'linux64', 'BUNDLE_KEY': KEY, **env},
            text=True, capture_output=True)

    def archive(self, entries):
        raw = self.work / 'input.tar'
        with tarfile.open(raw, 'w', format=tarfile.GNU_FORMAT) as out:
            for name, kind, value in entries:
                item = tarfile.TarInfo(name)
                item.mode = 0o755 if kind == tarfile.DIRTYPE else 0o644
                item.type = kind
                if kind == tarfile.REGTYPE:
                    item.size = len(value)
                    out.addfile(item, io.BytesIO(value))
                else:
                    item.linkname = value
                    out.addfile(item)
        subprocess.run(['zstd', '-q', '-f', str(raw), '-o', str(self.work / (KEY + '.tar.zst'))], check=True)

    def test_traversal_rejected_before_install(self):
        self.archive([(ROOT + '/good', tarfile.REGTYPE, b'good'),
                      ('../sentinel', tarfile.REGTYPE, b'changed')])
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('outside', result.stderr)
        self.assertEqual(self.sentinel.read_text(), 'unchanged')
        self.assertFalse((self.dest / 'build-ci').exists())

    def test_malicious_members(self):
        bad = [
            ('/sentinel', tarfile.REGTYPE, b'x'),
            (ROOT + '/../sentinel', tarfile.REGTYPE, b'x'),
            (ROOT + '//x', tarfile.REGTYPE, b'x'),
            (ROOT + '/./x', tarfile.REGTYPE, b'x'),
            (ROOT + '/x\\evil', tarfile.REGTYPE, b'x'),
            (ROOT + '/x\nname', tarfile.REGTYPE, b'x'),
            (ROOT + '/link', tarfile.SYMTYPE, '../../sentinel'),
            (ROOT + '/link', tarfile.SYMTYPE, 'good'),
            (ROOT + '/hard', tarfile.LNKTYPE, ROOT + '/good'),
            (ROOT + '/fifo', tarfile.FIFOTYPE, ''),
            (ROOT + '/dev', tarfile.CHRTYPE, ''),
            (ROOT + '/dev', tarfile.BLKTYPE, ''),
            (ROOT + '/sparse', tarfile.GNUTYPE_SPARSE, ''),
            (ROOT + '/good', tarfile.REGTYPE, b'duplicate'),
            (ROOT + '/good/child', tarfile.REGTYPE, b'x'),
        ]
        for entry in bad:
            with self.subTest(entry=entry[0:2]):
                self.archive([(ROOT + '/good', tarfile.REGTYPE, b'good'), entry])
                result = self.run_helper()
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertEqual(self.sentinel.read_text(), 'unchanged')
                self.assertFalse((self.dest / 'build-ci').exists())

    def test_identifiers_and_archive_types(self):
        for env in ({'BUILD_TARGET': '../linux64'}, {'BUILD_TARGET': 'unknown'},
                    {'BUNDLE_KEY': '../sentinel'}, {'BUNDLE_KEY': 'build-win64-0123abcd'},
                    {'BUNDLE_KEY': 'build-linux64-$(id)'}, {'BUNDLE_KEY': 'build-linux64-ABCDEF12'}):
            with self.subTest(env=env):
                result = self.run_helper(**env)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('invalid', result.stderr)
        path = self.work / (KEY + '.tar.zst')
        path.symlink_to(self.sentinel)
        self.assertNotEqual(self.run_helper().returncode, 0)
        path.unlink()
        os.mkfifo(path)
        self.assertNotEqual(self.run_helper().returncode, 0)

    def test_existing_destination_and_symlink_parent(self):
        self.archive([(ROOT + '/good', tarfile.REGTYPE, b'good')])
        (self.dest / 'build-ci').symlink_to(self.work, target_is_directory=True)
        self.assertNotEqual(self.run_helper().returncode, 0)
        (self.dest / 'build-ci').unlink()
        root = self.dest / ROOT
        root.mkdir(parents=True)
        (root / 'keep').write_text('keep')
        self.assertNotEqual(self.run_helper().returncode, 0)
        self.assertEqual((root / 'keep').read_text(), 'keep')
        self.assertEqual(self.sentinel.read_text(), 'unchanged')

    def test_trailing_payload_rejected(self):
        self.archive([(ROOT + '/good', tarfile.REGTYPE, b'good')])
        with (self.work / 'input.tar').open('ab') as stream:
            stream.write(b'not tar padding')
        subprocess.run(['zstd', '-q', '-f', str(self.work / 'input.tar'), '-o',
                        str(self.work / (KEY + '.tar.zst'))], check=True)
        self.assertNotEqual(self.run_helper().returncode, 0)
        self.assertFalse((self.dest / 'build-ci').exists())

    def test_workspace_symlink_rejected(self):
        self.archive([(ROOT + '/good', tarfile.REGTYPE, b'good')])
        self.dest.rmdir()
        self.dest.symlink_to(self.work, target_is_directory=True)
        self.assertNotEqual(self.run_helper().returncode, 0)
        self.assertFalse((self.work / 'build-ci').exists())

    def test_modes_metadata_and_truncation(self):
        variants = []
        for mode in (0o4755, 0o2755, 0o1755, 0o666):
            info = tarfile.TarInfo(ROOT + '/bad')
            info.mode = mode
            variants.append(info.tobuf(format=tarfile.GNU_FORMAT) + b'\0' * 1024)
        for kind in (tarfile.XHDTYPE, tarfile.XGLTYPE, tarfile.GNUTYPE_LONGNAME):
            info = tarfile.TarInfo(ROOT + '/bad')
            info.type = kind
            info.size = 1000000
            variants.append(info.tobuf(format=tarfile.GNU_FORMAT) + b'\0' * 1024)
        info = tarfile.TarInfo(ROOT + '/bad')
        info.size = 1000000
        variants.append(info.tobuf(format=tarfile.GNU_FORMAT) + b'\0' * 1024)
        variants.append(b'\0' * 512)
        for index, raw in enumerate(variants):
            with self.subTest(variant=index):
                (self.work / 'input.tar').write_bytes(raw)
                subprocess.run(['zstd', '-q', '-f', str(self.work / 'input.tar'), '-o',
                                str(self.work / (KEY + '.tar.zst'))], check=True)
                self.assertNotEqual(self.run_helper().returncode, 0)
                self.assertFalse((self.dest / 'build-ci').exists())
                self.assertEqual(self.sentinel.read_text(), 'unchanged')

    def test_resource_limits(self):
        import importlib.util
        from unittest.mock import patch
        spec = importlib.util.spec_from_file_location('bundle_archive', HELPER)
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.archive([(ROOT + '/one', tarfile.REGTYPE, b'12345678'),
                      (ROOT + '/two', tarfile.REGTYPE, b'12345678')])
        for limit, value in [('MAX_ARCHIVE', 1), ('MAX_TAR', 512), ('MAX_FILE', 4),
                             ('MAX_TOTAL', 12), ('MAX_ENTRIES', 1)]:
            with self.subTest(limit=limit), patch.object(module, limit, value):
                with self.assertRaises(ValueError):
                    module.unpack(self.work / (KEY + '.tar.zst'), self.dest, ROOT)
                self.assertFalse((self.dest / 'build-ci').exists())
                self.assertEqual(list(self.dest.iterdir()), [])

    def test_invalid_zstd_and_empty_tar(self):
        archive = self.work / (KEY + '.tar.zst')
        archive.write_bytes(b'not zstd')
        self.assertNotEqual(self.run_helper().returncode, 0)
        self.archive([])
        self.assertNotEqual(self.run_helper().returncode, 0)
        self.assertFalse((self.dest / 'build-ci').exists())

    def test_long_names_and_owner_modes(self):
        name = ROOT + '/' + 'a' * 90 + '/' + 'b' * 90
        self.archive([(name, tarfile.REGTYPE, b'payload')])
        result = self.run_helper()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.dest / name).read_bytes(), b'payload')
        self.assertEqual((self.dest / name).stat().st_uid, os.getuid())

    def test_create_rejects_unknown_symlink_and_no_clobber(self):
        root = self.dest / ROOT
        root.mkdir(parents=True)
        (root / 'evil').symlink_to(self.sentinel)
        self.assertNotEqual(self.run_helper('create').returncode, 0)
        (root / 'evil').unlink()
        archive = self.work / (KEY + '.tar.zst')
        archive.symlink_to(self.sentinel)
        self.assertNotEqual(self.run_helper('create').returncode, 0)
        self.assertEqual(self.sentinel.read_text(), 'unchanged')

    def test_create_rejects_reader_invalid_paths(self):
        root = self.dest / ROOT
        root.mkdir(parents=True)
        for name in ('nonascii-é', 'control\x01', 'back\\\\slash',
                     '/'.join(['d'] * 30 + ['file'])):
            with self.subTest(name=name):
                (self.work / (KEY + '.tar.zst')).unlink(missing_ok=True)
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'payload')
                result = self.run_helper('create')
                self.assertNotEqual(result.returncode, 0, result.stderr)
                self.assertFalse((self.work / (KEY + '.tar.zst')).exists())
                self.assertFalse(list(self.dest.glob('.bundle-*')))
                import shutil
                shutil.rmtree(root)
                root.mkdir()

    def test_create_resource_limits(self):
        import importlib.util
        from unittest.mock import patch
        spec = importlib.util.spec_from_file_location('bundle_archive', HELPER)
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        root = self.dest / ROOT
        root.mkdir(parents=True)
        for name in ('one', 'two'):
            (root / name).write_bytes(b'12345678')
        archive = self.work / (KEY + '.tar.zst')
        for limit, value in [('MAX_ARCHIVE', 1), ('MAX_TAR', 512),
                             ('MAX_TAR', 10239), ('MAX_FILE', 7),
                             ('MAX_TOTAL', 15), ('MAX_ENTRIES', 2)]:
            with self.subTest(limit=limit, value=value):
                archive.unlink(missing_ok=True)
                with patch.object(module, limit, value):
                    with self.assertRaises(ValueError):
                        module.create(archive, self.dest, ROOT)
                self.assertFalse(archive.exists())
                self.assertFalse(list(self.dest.glob('.bundle-*')))
        # Exactly-at-budget archives must be accepted by the same reader.
        archive.unlink(missing_ok=True)
        with patch.multiple(module, MAX_FILE=8, MAX_TOTAL=16,
                            MAX_ENTRIES=3, MAX_TAR=10240):
            module.create(archive, self.dest, ROOT)
            restored = self.work / 'restored'
            restored.mkdir()
            with patch.object(module, 'MAX_ARCHIVE', archive.stat().st_size):
                module.unpack(archive, restored, ROOT)
            self.assertEqual(sorted(p.name for p in (restored / ROOT).iterdir()),
                             ['one', 'two'])
            for name in ('one', 'two'):
                self.assertEqual((restored / ROOT / name).read_bytes(), b'12345678')

    def test_create_path_length_policy(self):
        import importlib.util
        from unittest.mock import patch
        spec = importlib.util.spec_from_file_location('bundle_archive', HELPER)
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        root = self.dest / ROOT
        root.mkdir(parents=True)
        (root / 'file').write_bytes(b'payload')
        original = tarfile.TarFile.gettarinfo
        archive = self.work / (KEY + '.tar.zst')

        def long_path(length):
            prefix = ROOT + '/' + '/'.join(['x' * 250] * 16) + '/'
            return prefix + 'y' * (length - len(prefix))

        # Real filesystems may reject these names before the archive policy can
        # see them. Substitute only metadata; write/compress real payloads.
        cases = [(ROOT + '/' + 'x' * 256, False),
                 (long_path(4097), False), (long_path(4096), False),
                 (long_path(4095), True)]
        for name, directory in cases:
            with self.subTest(length=len(name), directory=directory):
                def gettarinfo(bundle, *args, **kwargs):
                    info = original(bundle, *args, **kwargs)
                    if (directory and info.isdir()) or (not directory and info.isreg()):
                        info.name = name
                    return info
                with patch.object(tarfile.TarFile, 'gettarinfo', gettarinfo):
                    with self.assertRaises(ValueError):
                        module.create(archive, self.dest, ROOT)
                self.assertFalse(archive.exists())
                self.assertFalse(list(self.dest.glob('.bundle-*')))

    def test_create_boundary_paths_roundtrip(self):
        root = self.dest / ROOT
        names = ['x' * 255, '/'.join(['d'] * 29 + ['file'])]
        for name in names:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'boundary payload')
        result = self.run_helper('create')
        self.assertEqual(result.returncode, 0, result.stderr)
        import shutil
        shutil.rmtree(self.dest / 'build-ci')
        result = self.run_helper()
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in names:
            self.assertEqual((root / name).read_bytes(), b'boundary payload')

    def test_create_extract_roundtrip(self):
        root = self.dest / ROOT
        (root / 'src').mkdir(parents=True)
        binary = root / 'src/smartiecoind'
        binary.write_bytes(b'not executed: fixture')
        binary.chmod(0o755)
        (root / 'src/discard.o').write_bytes(b'object')
        (root / 'target').mkdir()
        (root / 'target/cache').write_bytes(b'rust build cache')
        for kind in ('functional', 'fuzz'):
            source = self.dest / ('test/' + kind)
            source.mkdir(parents=True)
            (source / 'test_runner.py').write_text('# runner fixture')
            outer = self.dest / ('build-ci/test/' + kind)
            outer.mkdir(parents=True)
            (outer / 'test_runner.py').symlink_to('../../../test/' + kind + '/test_runner.py')
            inner = root / ('test/' + kind)
            inner.mkdir(parents=True)
            (inner / 'test_runner.py').symlink_to('../../../test/' + kind + '/test_runner.py')
        result = self.run_helper('create')
        self.assertEqual(result.returncode, 0, result.stderr)
        import shutil
        shutil.rmtree(self.dest / 'build-ci')
        result = self.run_helper()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(binary.read_bytes(), b'not executed: fixture')
        self.assertEqual(binary.stat().st_mode & 0o777, 0o755)
        self.assertFalse((root / 'src/discard.o').exists())
        self.assertFalse((root / 'target').exists())
        self.assertFalse((root / 'test/functional/test_runner.py').is_symlink())
        self.assertEqual((root / 'test/functional/test_runner.py').read_text(), '# runner fixture')
        self.assertEqual(self.sentinel.read_text(), 'unchanged')


if __name__ == '__main__':
    unittest.main()
