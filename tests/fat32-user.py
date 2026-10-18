#!/usr/bin/env python3
"""Exercise FAT32 through real user syscalls, then check both unmounted disks."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

from arch_profiles import PROFILES

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/loongarch'))
from guest import run_guest

MIB = 1024 * 1024


def execute(command, log):
    result = subprocess.run(list(map(str, command)), text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    log.write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f'command failed ({result.returncode}): {command}; see {log}')


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=PROFILES, default='riscv')
    parser.add_argument('--program', type=Path, required=True)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--qemu')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    profile = PROFILES[args.arch]
    kernel = (args.kernel or ROOT / profile.kernel).resolve()
    program = args.program.resolve()
    qemu = shutil.which(args.qemu or ('qemu-system-loongarch64' if args.arch == 'loongarch'
                                     else profile.qemu))
    if not qemu:
        parser.error('QEMU executable not found')
    output = (args.output or ROOT / ('build/fat32-user-' + args.arch)).resolve()
    output.mkdir(parents=True, exist_ok=True)
    root, disk, partition = [output / name for name in ('root.img', 'disk.img', 'partition2.img')]
    with root.open('wb') as stream:
        stream.truncate(32 * MIB)
    execute(['mkfs.ext4', '-q', '-F', '-b', '4096', '-O', '^metadata_csum,^64bit,^orphan_file', root],
            output / 'mkfs-root.log')
    execute(['debugfs', '-w', '-R', f'write {program} /init', root], output / 'install-init.log')
    execute(['debugfs', '-w', '-R', 'set_inode_field /init mode 0100755', root],
            output / 'chmod-init.log')
    with partition.open('wb') as stream:
        stream.truncate(64 * MIB)
    execute(['mkfs.fat', '-F', '32', '-S', '512', '-s', '1', partition], output / 'mkfs-fat.log')
    mbr = bytearray(512)
    # entry 1 保持空白，真实用户程序必须通过第二个分区节点寻址。
    struct.pack_into('<B3sB3sII', mbr, 446 + 16, 0, b'\xfe\xff\xff', 0x0c,
                     b'\xfe\xff\xff', 2048, 64 * MIB // 512)
    mbr[510:512] = b'\x55\xaa'
    with disk.open('wb') as stream, partition.open('rb') as source:
        stream.truncate(65 * MIB)
        stream.write(mbr)
        stream.seek(MIB)
        shutil.copyfileobj(source, stream)
    command = [qemu]
    if args.arch == 'riscv':
        command += ['-machine', 'virt', '-bios', 'default']
    command += ['-kernel', str(kernel), '-m', '1G', '-smp', '1', '-nographic', '-no-reboot',
                '-net', 'none']
    for index, image in enumerate((root, disk)):
        command += ['-drive', f'file={image},if=none,format=raw,id=x{index}', '-device',
                    (f'virtio-blk-device,drive=x{index},bus=virtio-mmio-bus.{index}'
                     if args.arch == 'riscv' else f'virtio-blk-pci,drive=x{index}')]
    identity = {'arch': args.arch, 'kernel': str(kernel), 'kernel_sha256': digest(kernel),
                'program': str(program), 'program_sha256': digest(program),
                'qemu': qemu, 'qemu_sha256': digest(qemu),
                'qemu_version': subprocess.check_output([qemu, '--version'], text=True).splitlines()[0],
                'command': command}
    identity_path = output / 'identity.json'
    identity_path.write_text(json.dumps(identity, indent=2) + '\n')
    try:
        code, text = run_guest(command, 120)
    except subprocess.TimeoutExpired as error:
        (output / 'serial.log').write_text(error.output)
        raise
    (output / 'serial.log').write_text(text)
    identity['qemu_returncode'] = code
    identity['guest_passed'] = (code == 0 and 'BoarOS: FAT32 user syscall checks ok' in text.splitlines() and
                              profile.root_success(text, 0) and
                              re.search(r'heap-live=(?:0x0|0)(?:[;\s]|$)', text) is not None and
                              'fat32 failure' not in text)
    identity_path.write_text(json.dumps(identity, indent=2) + '\n')
    if not identity['guest_passed']:
        sys.stdout.write(text)
        raise SystemExit(f'FAT32 user guest failed; see {output}')
    execute(['e2fsck', '-fn', root], output / 'root.fsck.log')
    with disk.open('rb') as source, partition.open('wb') as target:
        source.seek(MIB)
        shutil.copyfileobj(source, target)
    execute(['fsck.fat', '-vn', partition], output / 'fat.fsck.log')
    identity['root_fsck_passed'] = identity['fat_fsck_passed'] = True
    identity_path.write_text(json.dumps(identity, indent=2) + '\n')
    print(f'PASS FAT32 {args.arch}: user syscalls, PID 1/heap release, ext4 and FAT fsck; {output}')


if __name__ == '__main__':
    main()
