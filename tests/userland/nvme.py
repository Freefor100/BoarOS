#!/usr/bin/env python3
"""Boot an original VirtIO root with an independent QEMU NVMe namespace."""
import argparse
import os
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tests'), str(ROOT / 'tests/diff-abi')]
from arch_profiles import PROFILES
import harness

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--arch', choices=PROFILES, default='riscv')
parser.add_argument('--cpus', type=int, default=1)
parser.add_argument('--kernel', type=Path, required=True)
parser.add_argument('--program', type=Path, required=True)
parser.add_argument('--qemu')
parser.add_argument('--uring', action='store_true')
parser.add_argument('--fs-batch', action='store_true',
                    help='exercise cold reads and journal writeback on the NVMe namespace')
args = parser.parse_args()
profile = PROFILES[args.arch]
qemu = args.qemu or os.environ.get(
    'QEMU_RISCV64' if args.arch == 'riscv' else 'QEMU_LOONGARCH64', profile.qemu)

(ROOT / 'build').mkdir(exist_ok=True)
work = Path(tempfile.mkdtemp(prefix='nvme-', dir=ROOT / 'build'))
root = harness.fixture(work, args.program.resolve())
disk = work / 'nvme.img'
with disk.open('wb') as stream:
    stream.truncate((32 if args.fs_batch else 8) * 1024 * 1024)
if args.fs_batch:
    harness.run_logged(['mkfs.ext4', '-qF', str(disk)], work / 'mkfs.log', timeout=30)

command = profile.boot(qemu, args.kernel.resolve(), '512M')
command[command.index('-smp') + 1] = str(args.cpus)
command += ['-net', 'none',
            '-drive', f'file={root},if=none,format=raw,id=root',
            '-device', profile.block('modern'),
            '-drive', f'file={disk},if=none,format=raw,id=nv0',
            '-device', 'nvme,drive=nv0,serial=boaros,addr=3']
if args.arch == 'riscv':
    command += ['-global', 'virtio-mmio.force-legacy=false']
if args.fs_batch:
    command += ['-trace', f'enable=pci_nvme_mmio_doorbell_sq,file={work / "doorbells.log"}']
log = work / 'guest.log'
harness.run_logged(command, log, timeout=45)
output = log.read_text()
if ('NVME ALL PASS' not in output or not profile.root_success(output, 0)
        or (args.uring and 'NVME URING PASS' not in output)):
    raise RuntimeError(f'incomplete NVMe test or root cleanup: {log}')

if args.fs_batch:
    import re
    tails = [int(value) for value in re.findall(r'sqid 1 new_tail (\d+)', (work / 'doorbells.log').read_text())]
    if not any((tail - previous) % 32 > 1 for previous, tail in zip([0] + tails, tails)):
        raise RuntimeError(f'NVMe filesystem run never batched a submission: {work / "doorbells.log"}')
    harness.run_logged(['e2fsck', '-fn', str(disk)], work / 'nvme-fsck.log', timeout=30)
else:
    with disk.open('rb') as stream:
        for i in (0, 31, 71):
            stream.seek(i * 16384)
            expected = bytes((17 * i + j // 4096 + 31 * j) & 0xff for j in range(16384))
            if stream.read(16384) != expected:
                raise RuntimeError(f'wrong persisted data after NVMe flush: {disk}')
        if args.uring:
            stream.seek(4096 * 512)
            if stream.read(1024) != b'\x2b' * 1024:
                raise RuntimeError(f'wrong persisted command data: {disk}')
harness.run_logged(['e2fsck', '-fn', str(root)], work / 'fsck.log', timeout=30)
root.unlink()
disk.unlink()
print(f'PASS NVMe {args.arch}, cpus={args.cpus}: {log}')
