#!/usr/bin/env python3
"""Single-CPU native wait handoff, using the caller's actual build directory."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--arch', choices=('riscv', 'loongarch'), required=True)
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--kernel-dir', type=Path, required=True)
    args = parser.parse_args()
    kernel = args.kernel_dir / 'wait-native'
    if not kernel.is_file():
        raise SystemExit(f'missing wait kernel: {kernel}')
    output_dir = Path('build/sync-b2/native') / args.arch
    output_dir.mkdir(parents=True, exist_ok=True)
    for memory in ('512M', '1G'):
        command = [args.qemu, '-machine', 'virt', '-smp', '1', '-m', memory,
                   '-kernel', str(kernel), '-nographic', '-no-reboot']
        command += ['-cpu', 'la464'] if args.arch == 'loongarch' else ['-bios', 'default']
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        output = result.stdout.decode(errors='replace')
        (output_dir / f'{memory}.log').write_text(output)
        if result.returncode or 'WAIT native passed:' not in output or 'fatal' in output:
            raise SystemExit(f'{args.arch} wait {memory} failed:\n{output[-3000:]}')
        print(f'{args.arch} wait handoff RAM={memory} passed', flush=True)


if __name__ == '__main__':
    main()
