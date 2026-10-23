#!/usr/bin/env python3
"""Keep hot RISC-V kernel paths within a translated code page."""
import argparse
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('kernel')
parser.add_argument('--nm', default='riscv64-elf-nm')
args = parser.parse_args()
symbols = subprocess.check_output([args.nm, '-S', '--defined-only', args.kernel], text=True)
for name, alignment in [('crc32c', 2048), ('put_buffer', 1024), ('memcpy', 1024), ('memmove', 2048),
                        ('kernel_syscall_dispatch', 4096)]:
    matches = [line.split() for line in symbols.splitlines() if line.split()[-1:] == [name]]
    if len(matches) != 1:
        raise SystemExit(f'expected one {name} function')
    address, size = (int(value, 16) for value in matches[0][:2])
    if not 0 < size <= alignment or address % alignment or address // 4096 != (address + size - 1) // 4096:
        raise SystemExit(f'{name} exceeds its aligned code slot: address={address:#x} bytes={size} slot={alignment}')
    print(f'PASS {name} stays within one code page: address={address:#x} bytes={size}')

rows = [line.split() for line in symbols.splitlines() if line.split()[-1:] == ['crc32c_slices']]
if len(rows) != 1:
    raise SystemExit('expected one immutable CRC32C slice table')
address, size = (int(value, 16) for value in rows[0][:2])
if address % 4096 or size != 8192:
    raise SystemExit('CRC32C slices must occupy exactly two aligned 4KiB regions')
print(f'PASS CRC32C slices occupy two aligned 4KiB regions: address={address:#x} bytes={size}')
