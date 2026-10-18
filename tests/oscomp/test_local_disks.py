"""Official and local launches share immutable inputs and freshly seeded delivery disks."""
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('local_disk_preparation', HERE / 'local_disks.py')
disks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(disks)


def elf(machine, program=1):
    data = bytearray(120)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HH', data, 16, 2, machine)
    struct.pack_into('<Q', data, 32, 64)
    struct.pack_into('<I', data, 48, 4 if machine == 243 else 3)
    struct.pack_into('<HH', data, 54, 56, 1)
    struct.pack_into('<I', data, 64, program)
    struct.pack_into('<Q', data, 112, 16384)
    return data


class LocalDiskTests(unittest.TestCase):
    def test_manifest_and_actual_mbr_match_approved_geometry(self):
        facts = disks.INPUTS['local_disk']
        self.assertEqual(facts['bytes'], 2304 * 1024**2)
        mbr = disks.mbr()
        self.assertEqual(mbr[510:], b'\x55\xaa')
        for index, expected in enumerate(((131, 2048, 128*2048), (12, 129*2048, 64*2048), (131, 193*2048, 2048*2048))):
            self.assertEqual((mbr[450+index*16], *struct.unpack_from('<II', mbr, 454+index*16)), expected)
        self.assertEqual(mbr[494:510], bytes(16))
        self.assertNotIn('rng', disks.INPUTS['local_profile'])

    def test_static_exec_lp64d_and_la_alignment_are_enforced(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'tool'
            for arch, machine in [('riscv',243), ('loongarch',258)]:
                path.write_bytes(elf(machine)); disks.verify_target(path, arch)
                for offset, value in ((16, 3), (18, 62), (48, 0), (64, 2), (64, 3)):
                    image = elf(machine); image[offset] = value; path.write_bytes(image)
                    with self.assertRaises(RuntimeError): disks.verify_target(path, arch)
            image = elf(258); struct.pack_into('<Q', image, 112, 4096); path.write_bytes(image)
            with self.assertRaisesRegex(RuntimeError, '16 KiB'): disks.verify_target(path, 'loongarch')

    def test_bad_tracked_archive_is_rejected_without_extraction(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); archive = root / disks.INPUTS['e2fsprogs']['archive']
            archive.parent.mkdir(parents=True); archive.write_bytes(b'corruption')
            with patch.object(disks, 'ROOT', root):
                with self.assertRaisesRegex(RuntimeError, 'checksum'): disks.fixed_source(root / 'cache')
            self.assertFalse((root / 'cache/e2fsprogs-1.47.2').exists())

    def test_independent_fresh_seeds_have_valid_checksums_and_no_template_secret(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); reference = root / 'reference'; reference.mkdir()
            original = reference / 'sdcard-rv.img'; original.write_bytes(b'original published root')
            template = root / 'delivery-template.img'; template.write_bytes(disks.mbr() + bytes(8192-512))
            before = template.read_bytes()
            one, disk_one = disks.prepare_run(reference, root, root / 'one', 'riscv')
            two, disk_two = disks.prepare_run(reference, root, root / 'two', 'riscv')
            self.assertEqual(one.read_bytes(), original.read_bytes()); self.assertEqual(two.read_bytes(), original.read_bytes())
            self.assertEqual(template.read_bytes(), before)
            slot1 = disk_one.read_bytes()[4096:8192]; slot2 = disk_two.read_bytes()[4096:8192]
            self.assertNotEqual(slot1[24:88], slot2[24:88])
            self.assertEqual(struct.unpack_from('<8sIII',slot1), (b'BOARSEED',1,243,64))
            self.assertEqual(struct.unpack_from('<I',slot1,20)[0],zlib.crc32(slot1[:20]+slot1[24:88]))
            self.assertEqual(slot1[88:],bytes(4096-88))
            with self.assertRaisesRegex(RuntimeError, 'already exists'): disks.prepare_run(reference, root, root/'one','riscv')
            with self.assertRaisesRegex(RuntimeError, 'independent'): disks.prepare_delivery(root, template, 'riscv')
            disk_one.write_bytes(b'destructive formatting')
            self.assertEqual(template.read_bytes(), before)

    def test_run_destination_cannot_alias_original_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            reference, assets, output = (base / name for name in ('reference', 'assets', 'run'))
            for path in (reference, assets, output): path.mkdir()
            original = reference / 'sdcard-rv.img'; original.write_bytes(b'original root')
            (assets / 'delivery-template.img').write_bytes(bytes(8192))
            os.link(original, output / 'root.img')
            with self.assertRaisesRegex(RuntimeError, 'independent'):
                disks.prepare_run(reference, assets, output, 'riscv')
            self.assertEqual(original.read_bytes(), b'original root')

    def test_partial_getrandom_is_completed_and_architectures_are_distinct(self):
        with patch.object(os, 'getrandom', side_effect=[b'a'*7,b'b'*57,b'c'*64]) as random:
            rv, la = disks.seed_envelope('riscv'), disks.seed_envelope('loongarch')
        self.assertEqual([call.args for call in random.call_args_list], [(64,), (57,), (64,)])
        self.assertEqual(struct.unpack_from('<I', la, 12)[0],258)
        self.assertNotEqual(rv[24:88],la[24:88])

    def test_failure_does_not_replace_previous_delivery(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); (root/'delivery-template.img').write_bytes(bytes(8192))
            output=root/'disk.img'; output.write_bytes(b'previous')
            with patch.object(os, 'getrandom', side_effect=OSError(5,'I/O error')):
                with self.assertRaises(OSError): disks.prepare_delivery(root,output,'riscv')
            self.assertEqual(output.read_bytes(),b'previous')

    def test_both_architectures_use_two_disks_without_rng(self):
        for arch in ('riscv','loongarch'):
            with patch.object(sys,'argv',['local_disks.py','--arch',arch,'--qemu','qemu','--kernel','kernel','--output','run','--gdb']), \
                    patch.object(disks,'prepare_assets',return_value=Path('assets')), \
                    patch.object(disks,'prepare_run',return_value=(Path('root.img'),Path('scratch.img'))), \
                    patch.object(disks.subprocess,'run') as launch, patch('builtins.print'):
                disks.main()
                command=launch.call_args.args[0]
                self.assertEqual(command.count('-drive'),2)
                self.assertNotIn('-object',command)
                self.assertIn('-S',command)

    def test_missing_native_toolchain_uses_same_offline_container_and_host_owner(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(Path, 'is_file', return_value=False), \
                patch.object(disks.subprocess, 'run') as run:
            cache = Path(temporary) / 'cache'
            self.assertEqual(disks.prepare_assets(cache, 'riscv'), cache / 'riscv')
            command = run.call_args.args[0]
            self.assertIn('--pull=never', command)
            self.assertEqual(command[command.index('--network') + 1], 'none')
            self.assertEqual(command[command.index('--user') + 1], f'{os.getuid()}:{os.getgid()}')
            self.assertIn(disks.INPUTS['container']['reference'], command)
            self.assertIn(str(HERE / 'local_disks.py'), command)
            self.assertNotIn('--delivery', command)

    def test_real_guest_seed_program_failure_ordering(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary=Path(temporary)/'seed-test'
            subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',str(HERE/'seed_test.c'),'-o',str(binary)],check=True)
            subprocess.run([binary],check=True,capture_output=True)


class PreparedDeliveryTests(unittest.TestCase):
    def test_real_deliveries_have_exact_filesystems_tools_and_seed_slots(self):
        root = HERE.parents[1]
        if not all((root / name).is_file() for name in ('disk.img', 'disk-la.img')):
            self.skipTest('build the two official delivery disks first')
        for arch, name in [('riscv', 'disk.img'), ('loongarch', 'disk-la.img')]:
            disk = root / name
            assets = root / 'build/tools/oscomp-local' / arch
            with disk.open('rb') as stream:
                self.assertEqual(stream.read(512), disks.mbr())
                stream.seek(4096); slot = stream.read(4096)
                self.assertEqual(struct.unpack_from('<8sIII', slot),
                                 (b'BOARSEED', 1, disks.INPUTS['architectures'][arch]['elf_machine'], 64))
                self.assertEqual(struct.unpack_from('<I', slot, 20)[0], zlib.crc32(slot[:20] + slot[24:88]))
                stream.seek(1024**2 + 1024); sb1 = stream.read(1024)
                stream.seek(129 * 1024**2); fat = stream.read(512)
                stream.seek(193 * 1024**2 + 1024); sb3 = stream.read(1024)
            for sb, exponent in ((sb1, 0), (sb3, 2)):
                self.assertEqual(sb[56:58], b'\x53\xef')
                self.assertEqual(struct.unpack_from('<I', sb, 24)[0], exponent)
            self.assertEqual(struct.unpack_from('<H', fat, 11)[0], 512)
            self.assertEqual(fat[13], 1)
            self.assertEqual(fat[82:90], b'FAT32   ')
            with tempfile.TemporaryDirectory() as temporary:
                partition = Path(temporary) / 'p1.img'
                subprocess.run(['dd', 'if=' + str(disk), 'of=' + str(partition), 'bs=1M',
                                'skip=1', 'count=128', 'conv=sparse', 'status=none'], check=True)
                files = disks.payload_files(arch)
                for name in ('mke2fs', 'mke2fs.conf', 'seed'): files[name] = assets / name
                for name, original in files.items():
                    actual = subprocess.check_output(['debugfs', '-R', 'cat /' + name, str(partition)],
                                                     stderr=subprocess.DEVNULL)
                    self.assertEqual(actual, original.read_bytes(), name)
                for name in ('mkfs.ext2', 'mkfs.ext3', 'mkfs.ext4'):
                    actual = subprocess.check_output(['debugfs', '-R', 'stat /' + name, str(partition)],
                                                     stderr=subprocess.DEVNULL, text=True)
                    self.assertIn('symlink', actual)
                    self.assertIn('mke2fs', actual)
            for name in ('mke2fs', 'seed'): disks.verify_target(assets / name, arch)
            with (assets / 'delivery-template.img').open('rb') as stream:
                stream.seek(4096)
                self.assertEqual(stream.read(4096), bytes(4096))


if __name__ == '__main__': unittest.main()
