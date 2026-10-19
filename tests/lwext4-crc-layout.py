#!/usr/bin/env python3
"""Keep the RV software CRC loop within one translated code page."""
import argparse
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('kernel')
parser.add_argument('--nm', default='riscv64-elf-nm')
args = parser.parse_args()
symbols = subprocess.check_output([args.nm, '-S', '--defined-only', args.kernel], text=True)
matches = [line.split() for line in symbols.splitlines() if line.split()[-1:] == ['crc32c']]
if len(matches) != 1:
    raise SystemExit('expected one software crc32c function')
address, size = (int(value, 16) for value in matches[0][:2])
if not size or address // 4096 != (address + size - 1) // 4096:
    raise SystemExit(f'CRC32C crosses a 4 KiB code page: address={address:#x} bytes={size}')
print(f'PASS RV CRC32C stays within one code page: address={address:#x} bytes={size}')
