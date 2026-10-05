#!/usr/bin/env python3
"""Exercise bounded I/O and MM costs using the real kernel modules."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--kernel', required=True)
parser.add_argument('--qemu', default='qemu-system-riscv64')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
work = Path(tempfile.mkdtemp(prefix='scale-run.', dir=root / 'build'))
try:
    disk = work / 'root.img'
    with disk.open('wb') as stream:
        stream.truncate(128 * 1024 * 1024)
    subprocess.run(['mkfs.ext4', '-q', '-F', '-b', '4096', str(disk)], check=True)
    result = subprocess.run([args.qemu, '-machine', 'virt', '-bios', 'default',
        '-kernel', args.kernel, '-m', '512M', '-smp', '1', '-nographic', '-no-reboot',
        '-drive', f'file={disk},if=none,format=raw,id=root',
        '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0'],
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120)
    (work / 'boot.log').write_text(result.stdout + result.stderr)
    if result.returncode or 'BoarOS: scale tests passed' not in result.stdout or 'scale failed:' in result.stdout:
        raise RuntimeError(result.stdout[-4000:] + result.stderr)
    labels = ('growth ', 'file ', 'TCP ', 'mapped bytes:', 'resident probes:',
              'protect visits:', 'address flushes:', 'global flushes:', 'BoarOS: scale')
    print('\n'.join(line for line in result.stdout.splitlines()
                    if line.startswith(labels)))
except BaseException:
    print(f'scale artifacts retained: {work}')
    raise
else:
    shutil.rmtree(work)
