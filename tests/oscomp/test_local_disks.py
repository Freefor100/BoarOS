"""The local profile owns disposable disks and real target filesystem tools."""
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('local_disk_preparation', HERE / 'local_disks.py')
disks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(disks)


def elf(machine, program_type=None):
    data = bytearray(120)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<H', data, 18, machine)
    if program_type is not None:
        struct.pack_into('<Q', data, 32, 64)
        struct.pack_into('<HH', data, 54, 56, 1)
        struct.pack_into('<I', data, 64, program_type)
    return data


class LocalDiskTests(unittest.TestCase):
    def test_manifest_pins_upstream_source_and_disk_geometry(self):
        source = disks.INPUTS['e2fsprogs']
        self.assertEqual(source['version'], '1.47.2')
        self.assertTrue(source['url'].startswith('https://www.kernel.org/'))
        self.assertEqual(source['sha256'], '08242e64ca0e8194d9c1caad49762b19209a06318199b63ce74ae4ef2d74e63c')
        self.assertEqual(disks.INPUTS['local_disk']['scratch_bytes'], 2 * 1024**3)
        self.assertEqual(disks.INPUTS['local_disk']['block_bytes'], 4096)

    def test_static_tool_machine_and_dynamic_program_headers_are_checked(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'mke2fs'
            for machine, program in ((243, None), (258, None), (243, 2), (243, 3)):
                path.write_bytes(elf(machine, program))
                if machine == 243 and program is None:
                    disks.verify_target(path, 'riscv')
                else:
                    with self.assertRaises(RuntimeError): disks.verify_target(path, 'riscv')

    def test_bad_download_cache_fails_without_extracting_or_replacing_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            archive = cache / Path(disks.INPUTS['e2fsprogs']['url']).name
            archive.write_bytes(b'altered source archive')
            with patch.object(disks.urllib.request, 'urlopen') as download:
                with self.assertRaisesRegex(RuntimeError, 'checksum'): disks.fixed_source(cache)
                download.assert_not_called()
            self.assertEqual(archive.read_bytes(), b'altered source archive')
            self.assertFalse((cache / 'e2fsprogs-1.47.2').exists())

    def test_run_destination_cannot_alias_a_retained_image(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            reference, assets, output = (base / name for name in ('reference', 'assets', 'output'))
            for path in (reference, assets, output): path.mkdir()
            root = reference / 'sdcard-rv.img'; root.write_bytes(b'root')
            (assets / 'scratch-template.img').write_bytes(b'scratch')
            os.link(root, output / 'root.img')
            with self.assertRaisesRegex(RuntimeError, 'independent'):
                disks.prepare_run(reference, assets, output, 'riscv')
            self.assertEqual(root.read_bytes(), b'root')

    @unittest.skipUnless(shutil.which('mke2fs') and shutil.which('debugfs'), 'host ext4 tools required')
    def test_tools_are_injected_only_into_independent_synced_run_copies(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            reference, assets = base / 'reference', base / 'tool assets'
            reference.mkdir(); assets.mkdir()
            source = reference / 'sdcard-rv.img'
            with source.open('wb') as stream: stream.truncate(32 * 1024**2)
            subprocess.run(['mke2fs', '-q', '-F', '-t', 'ext4', str(source)], check=True)
            template = assets / 'scratch-template.img'
            template.write_bytes(b'original scratch template')
            for name in ('mke2fs', 'scratch-check'): (assets / name).write_bytes(elf(243))
            (assets / 'mke2fs.conf').write_text('[defaults]\nblocksize = 4096\n')
            before = disks.sha(source)
            root, scratch = disks.prepare_run(reference, assets, base / 'run', 'riscv')
            self.assertEqual(disks.sha(source), before)
            self.assertFalse(os.path.samefile(root, source))
            self.assertFalse(os.path.samefile(scratch, template))
            contents = subprocess.check_output(['debugfs', '-R', 'cat /boaros-tools/mke2fs', str(root)], stderr=subprocess.DEVNULL)
            self.assertEqual(contents, elf(243))
            listing = subprocess.check_output(['debugfs', '-R', 'stat /boaros-tools/mke2fs', str(root)], text=True, stderr=subprocess.DEVNULL)
            self.assertIn('0755', listing)
            scratch.write_bytes(b'destructive guest formatting')
            self.assertEqual(template.read_bytes(), b'original scratch template')
            with self.assertRaisesRegex(RuntimeError, 'already exists'):
                disks.prepare_run(reference, assets, base / 'run', 'riscv')

    def test_both_architectures_attach_two_distinct_physical_disks(self):
        for arch, transport in (('riscv', 'virtio-blk-device'), ('loongarch', 'virtio-blk-pci')):
            command = disks.disk_arguments(arch, '/run/root.img', '/run/scratch.img')
            self.assertEqual(command.count('-drive'), 2)
            self.assertIn('file=/run/root.img,if=none,format=raw,id=x0', command)
            self.assertIn('file=/run/scratch.img,if=none,format=raw,id=x1', command)
            devices = [command[i + 1] for i, value in enumerate(command) if value == '-device']
            self.assertTrue(all(device.startswith(transport + ',') for device in devices))
            if arch == 'riscv': self.assertIn('bus=virtio-mmio-bus.1', devices[1])

if __name__ == '__main__':
    unittest.main()
