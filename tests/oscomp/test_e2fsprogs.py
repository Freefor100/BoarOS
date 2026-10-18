"""Real filesystem feature selection follows OS identity and preserves Linux behavior."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location('e2fsprogs_tools', HERE / 'local_disks.py')
disks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(disks)
CACHE = Path(os.environ.get('OSCOMP_LOCAL_CACHE', ROOT / 'build/tools/oscomp-local'))


class PatchTests(unittest.TestCase):
    def test_patch_identity_is_pinned_with_unmodified_archive(self):
        facts = disks.INPUTS['e2fsprogs']
        self.assertEqual(facts['sha256'], '08242e64ca0e8194d9c1caad49762b19209a06318199b63ce74ae4ef2d74e63c')
        self.assertEqual(disks.sha(ROOT / facts['portability_patch']['path']), facts['portability_patch']['sha256'])

    def test_patch_is_idempotent_and_rejects_changed_source(self):
        facts = disks.INPUTS['e2fsprogs']
        archive = CACHE / Path(facts['url']).name
        if not archive.is_file(): self.skipTest('prepare the pinned e2fsprogs archive first')
        self.assertEqual(disks.sha(archive), facts['sha256'])
        port = facts['portability_patch']
        with tarfile.open(archive) as content:
            original = content.extractfile('e2fsprogs-' + facts['version'] + '/' + port['target']).read()
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            target = source / port['target']; target.parent.mkdir(parents=True)
            target.write_bytes(original)
            self.assertEqual(disks.sha(target), port['original_sha256'])
            disks.apply_portability_patch(source)
            self.assertEqual(disks.sha(target), port['patched_sha256'])
            modified = target.stat().st_mtime_ns
            disks.apply_portability_patch(source)
            self.assertEqual(target.stat().st_mtime_ns, modified)
            target.write_bytes(target.read_bytes() + b'\n/* unexpected local change */\n')
            with self.assertRaisesRegex(RuntimeError, 'target checksum'):
                disks.apply_portability_patch(source)


class NativeMke2fsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        host = CACHE / 'host'
        if not shutil.which('cc') or not (host / 'identity.json').is_file():
            raise unittest.SkipTest('prepare the fixed host e2fsprogs tools first')
        identity = json.loads((host / 'identity.json').read_text())
        if identity['inputs'].get('portability_patch_sha256') != disks.INPUTS['e2fsprogs']['portability_patch']['sha256']:
            raise unittest.SkipTest('rebuild the fixed host e2fsprogs portability input first')
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.base = Path(cls.temporary.name)
        cls.executable = cls.base / 'mke2fs'
        build = host / 'build'
        objects = [build / 'misc' / (name + '.o') for name in
                   ('mke2fs', 'util', 'default_profile', 'mk_hugefiles', 'create_inode', 'create_inode_libarchive')]
        libraries = [build / 'lib' / ('lib' + name + '.a') for name in
                     ('ext2fs', 'com_err', 'support', 'blkid', 'uuid', 'ext2fs', 'e2p')]
        subprocess.run(['cc', '-O2', '-static', '-Wall', '-Wextra', '-Werror',
                        '-Wl,--wrap=uname', '-o', str(cls.executable), str(HERE / 'e2fsprogs_uname.c'),
                        *map(str, objects), *map(str, libraries)], check=True, capture_output=True)
        cls.configuration = host / 'mke2fs.conf'

    def filesystem(self, label, release, version):
        image = self.base / (label + '.img')
        with image.open('wb') as stream: stream.truncate(64 * 1024**2)
        environment = dict(os.environ, MKE2FS_CONFIG=str(self.configuration),
                           OSCOMP_TEST_UTS_RELEASE=release, OSCOMP_TEST_UTS_VERSION=version)
        subprocess.run([str(self.executable), '-q', '-F', '-t', 'ext4', '-b', '4096',
                        '-E', 'lazy_itable_init=0,lazy_journal_init=0', str(image)],
                       env=environment, check=True, capture_output=True)
        with image.open('rb') as stream: stream.seek(1024); superblock = stream.read(1024)
        revision = struct.unpack_from('<I', superblock, 76)[0]
        features = struct.unpack_from('<III', superblock, 92)
        return revision, features

    def test_boaros_and_modern_linux_keep_default_ext4_features(self):
        linux = self.filesystem('linux6', '6.12.0', '#1 SMP Linux')
        boaros = self.filesystem('boaros', '0.1.0-boaros-dev', '#1 BoarOS')
        self.assertEqual(boaros, linux)
        self.assertEqual(boaros[0], 1)
        compatible, incompatible, readonly = boaros[1]
        self.assertTrue(compatible & 0x4, 'has_journal missing')
        self.assertTrue(incompatible & 0x40, 'extents missing')
        self.assertTrue(incompatible & 0x80, '64bit missing')
        self.assertTrue(readonly & 0x400, 'metadata_csum missing')

    def test_actual_old_linux_keeps_revision_zero_behavior(self):
        revision, features = self.filesystem('linux2', '2.0.40', '#1 Linux')
        self.assertEqual(revision, 0)
        self.assertEqual(features, (0, 0, 0))


if __name__ == '__main__':
    unittest.main()
