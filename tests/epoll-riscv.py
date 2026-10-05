#!/usr/bin/env python3
"""Run the same epoll delivery ELF on BoarOS and the pinned Linux reference."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/diff-abi'))
import harness

parser = argparse.ArgumentParser(__doc__)
parser.add_argument('--kernel', type=Path, default=ROOT / 'kernel-rv')
parser.add_argument('--linux', type=Path)
parser.add_argument('--only', choices=('boaros', 'linux'))
parser.add_argument('--qemu', default='qemu-system-riscv64')
args = parser.parse_args()
work = Path(tempfile.mkdtemp(prefix='epoll-delivery.', dir=ROOT / 'build'))
program = work / 'init'
subprocess.run([str(ROOT / 'build/riscv/musl-root/bin/musl-gcc'),
                '-fno-link-libatomic', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                str(ROOT / 'tests/workloads/epoll/delivery.c'), '-o', str(program)], check=True)
fixture = harness.fixture(work, program)
variants = []
metadata = {'program_sha256': harness.digest(program), 'runs': {},
            'qemu': subprocess.check_output([args.qemu, '--version'], text=True)}
if args.only != 'boaros':
    linux, metadata['linux'] = harness.fixed_linux_image(args.linux)
    variants.append(('linux', linux))
if args.only != 'linux':
    variants.append(('boaros', args.kernel))
for name, kernel in variants:
    disk = work / (name + '.img')
    shutil.copyfile(fixture, disk)
    command = [args.qemu, '-machine', 'virt', '-bios', 'default', '-kernel', str(kernel),
               '-m', '512M', '-smp', '1', '-nographic', '-no-reboot',
               '-drive', f'file={disk},if=none,format=raw,id=root',
               '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
    if name == 'linux':
        command += ['-append', 'root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
    log = work / (name + '.log')
    try:
        harness.run_logged(command, log, 60)
    except (RuntimeError, TimeoutError):
        pass  # Linux PID 1 exits after its final contract marker.
    raw = log.read_text(errors='replace')
    passed = 'EPOLL PASS delivery' in raw and 'epoll failure' not in raw
    if name == 'boaros':
        passed &= 'heap-live=0x0; shutting down' in raw and 'exited status=0x0 ' in raw
    metadata['runs'][name] = {'passed': passed, 'kernel_sha256': harness.digest(kernel), 'command': command}
    print(name, 'PASS' if passed else 'FAIL', work)
    if not passed:
        print('\n'.join(raw.splitlines()[-12:]))
(work / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
if not all(run['passed'] for run in metadata['runs'].values()):
    raise SystemExit(1)
for disk in work.glob('*.img'):
    disk.unlink()
