#!/usr/bin/env python3
"""Real ext4 images and deterministic overlapping read-only verification."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path, default=ROOT/'build/lwext4-checksum-read-host')
    args = parser.parse_args()
    source = args.source_root.resolve(); out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    program = out/'probe'
    command = ['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-but-set-variable', '-Wno-stringop-truncation',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
        '-DCONFIG_USE_DEFAULT_CFG=1', '-DCONFIG_USE_USER_MALLOC=1',
        '-include', str(source/'tests/host/lwext4_memory.h'),
        '-I'+str(source/'third_party/lwext4/include'), '-idirafter', str(source/'include')]
    command += [str(p) for p in sorted((source/'third_party/lwext4/src').glob('*.c'))]
    command += [str(ROOT/'tests/host/lwext4_checksum_read.c'), str(source/'tests/host/block_fault.c'),
        str(source/'kernel/block.c'), '-Wl,--wrap=ext4_crc32c', '-o', str(program)]
    subprocess.run(command, check=True)
    with (out/'results.log').open('w') as log:
        for block in (1024, 4096):
            for inode in (128, 256):
                base = out/f'base-{block}-{inode}.img'
                with base.open('wb') as f: f.truncate(32*1024*1024)
                subprocess.run(['mkfs.ext4', '-q', '-F', '-b', str(block), '-I', str(inode), str(base)], check=True)
                subprocess.run([str(program), str(base), 'seed'], check=True, stdout=log, stderr=log)
                for mode in ('gdt', 'inode'):
                    image = out/f'{block}-{inode}-{mode}.img'
                    subprocess.run(['cp', '--sparse=always', str(base), str(image)], check=True)
                    subprocess.run([str(program), str(image), mode], check=True, stdout=log, stderr=log)
                    subprocess.run(['e2fsck', '-fn', str(image)], check=True, stdout=log, stderr=log)
    print('PASS: 8 overlapping checksum reads, immutable buffers, heap owners and independent e2fsck')

if __name__ == '__main__': main()
