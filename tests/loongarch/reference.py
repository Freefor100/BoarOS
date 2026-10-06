#!/usr/bin/env python3
"""Boot the same immutable ELF under the fixed 16 KiB Linux baseline."""
import argparse
import gzip
import hashlib
from pathlib import Path
import re
import subprocess
import sys
from guest import run_guest

ROOT = Path(__file__).resolve().parents[2]
REVISION = 'f4cdf7ca9a1fdcca413157df19753f388a5a224e'


def archive(entries):
    output = bytearray()
    for inode, (name, mode, data, major, minor) in enumerate(entries, 1):
        encoded = name.encode() + b'\0'
        values = (inode, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(encoded), 0)
        output.extend(b'070701' + ''.join(f'{v:08x}' for v in values).encode())
        output.extend(encoded)
        output.extend(b'\0' * (-len(output) % 4))
        output.extend(data)
        output.extend(b'\0' * (-len(output) % 4))
    return gzip.compress(bytes(output), mtime=0)


def records(output):
    return {name: (int(reason, 16), int(status, 16)) for name, reason, status in
            re.findall(r'LA case (\w+) reason=(0x[0-9a-f]+) status=(0x[0-9a-f]+)', output)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--qemu', default='build/qemu-la/qemu-system-loongarch64')
    parser.add_argument('--kernel', default='build/linux-la/vmlinux')
    parser.add_argument('--program', default='build/loongarch/user-probe')
    parser.add_argument('--cc', default='loongarch64-unknown-linux-gnu-gcc')
    parser.add_argument('--boaros-log')
    args = parser.parse_args()
    actual = subprocess.check_output(['git', '-C', 'references/linux', 'rev-parse', 'HEAD'], text=True).strip()
    if actual != REVISION:
        raise SystemExit('Linux revision differs from fixed LA baseline')
    configuration = Path(args.kernel).parent.joinpath('.config').read_text()
    if 'CONFIG_16KB_3LEVEL=y' not in configuration:
        raise SystemExit('Linux LA baseline must use 16 KiB, three levels')
    output = ROOT / 'build/loongarch/reference'
    output.mkdir(parents=True, exist_ok=True)
    init = output / 'init'
    subprocess.run([args.cc, '-march=loongarch64', '-mabi=lp64s', '-msoft-float',
                    '-mno-lsx', '-mno-lasx', '-O2', '-ffreestanding', '-fno-builtin',
                    '-fno-stack-protector', '-nostdlib', '-nostartfiles', '-static',
                    '-no-pie', '-Wl,--build-id=none', '-Wl,-z,max-page-size=16384',
                    '-T', 'tests/loongarch/user.ld', '-o', str(init),
                    'tests/loongarch/linux_init.c', 'tests/loongarch/user_start.S', '-lgcc'], check=True)
    program = Path(args.program).read_bytes()
    ramdisk = output / 'initramfs.gz'
    ramdisk.write_bytes(archive([
        ('dev', 0o040755, b'', 0, 0), ('dev/console', 0o020600, b'', 5, 1),
        ('init', 0o100755, init.read_bytes(), 0, 0),
        ('la-probe', 0o100755, program, 0, 0), ('TRAILER!!!', 0, b'', 0, 0)]))
    expected = records(Path(args.boaros_log).read_text()) if args.boaros_log else None
    for memory in ('512M', '1G'):
        code, text = run_guest([args.qemu, '-machine', 'virt', '-cpu', 'la464',
                                 '-smp', '1', '-m', memory, '-kernel', args.kernel,
                                 '-initrd', str(ramdisk), '-append',
                                 'console=ttyS0 rdinit=/init loglevel=3', '-nographic',
                                 '-no-reboot'], timeout=120)
        (output / f'linux-{memory}.log').write_text(text)
        observed = records(text)
        print(f'Linux {memory}: {observed}')
        if code or 'Linux LA user contracts passed' not in text or len(observed) != 9:
            sys.stdout.write(text)
            raise SystemExit('Linux LA contract failed')
        if expected is not None and observed != expected:
            raise SystemExit(f'LA differential mismatch: BoarOS {expected}, Linux {observed}')
    print('Same LA ELF contracts match; SHA-256 ' + hashlib.sha256(program).hexdigest())


if __name__ == '__main__':
    main()
