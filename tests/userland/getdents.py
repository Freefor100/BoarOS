#!/usr/bin/env python3
"""Run directory contracts through real syscalls and the normal kernel boot."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tests'), str(ROOT / 'tests/diff-abi')]
import harness
from arch_profiles import PROFILES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=PROFILES, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--program', type=Path, required=True)
    parser.add_argument('--qemu')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    profile = PROFILES[args.arch]
    folder = args.output.resolve()
    folder.mkdir(parents=True)
    kernel, program = folder / 'kernel', folder / 'init'
    shutil.copyfile(args.kernel, kernel)
    shutil.copyfile(args.program, program)
    disk = harness.fixture(folder, program)
    qemu = args.qemu or os.environ.get('QEMU_RISCV64' if args.arch == 'riscv' else 'QEMU_LOONGARCH64', profile.qemu)
    command = [qemu, '-machine', 'virt', '-smp', '1', '-m', '512M', '-kernel', str(kernel),
               '-nographic', '-no-reboot', '-net', 'none']
    # This contract makes no timestamp assertions and needs no patched RTC model.
    command += ['-bios', 'default'] if args.arch == 'riscv' else ['-cpu', 'la464']
    command += ['-drive', f'file={disk},if=none,format=raw,id=root', '-device', profile.block('modern'),
                '-object', 'rng-random,id=entropy,filename=/dev/urandom', '-device', profile.rng()]
    if args.arch == 'riscv': command += ['-global', 'virtio-mmio.force-legacy=false']
    record = {'arch': args.arch, 'kernel_sha256': harness.digest(kernel), 'program_sha256': harness.digest(program),
              'command': command, 'qemu': harness.output([qemu, '--version']), 'passed': False}
    with (folder / 'boot.log').open('w') as stream:
        try:
            result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT, timeout=90)
            record['returncode'] = result.returncode
        except subprocess.TimeoutExpired:
            record['returncode'] = None
    raw = (folder / 'boot.log').read_text(errors='replace')
    rows = [line for line in raw.replace('\r', '').splitlines() if line.startswith('GETDENTS ')]
    cases = ('batch-long-name-eof', 'short-buffer-cookie', 'efault-complete-prefix', 'dup-fork-independent-offset')
    expected = [f'GETDENTS {backend} {case} PASS' for backend in ('ext4', 'tmpfs') for case in cases]
    expected += ['GETDENTS CONTRACT PASS']
    record['rows'] = rows
    record['passed'] = record['returncode'] == 0 and rows == expected and profile.root_success(raw, 0)
    if record['passed']:
        with (folder / 'fsck.log').open('w') as stream:
            record['fsck'] = subprocess.run(['e2fsck', '-fn', str(disk)], stdout=stream, stderr=subprocess.STDOUT).returncode
        record['passed'] &= record['fsck'] == 0
    (folder / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print('\n'.join(rows) if rows else raw[-4000:])
    if record['passed']: disk.unlink()
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
