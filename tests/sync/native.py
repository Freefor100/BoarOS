#!/usr/bin/env python3
"""Exercise real CPU/atomic instructions and forbidden scheduler entrypoints."""
import argparse
from pathlib import Path
import subprocess

def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--arch', choices=('riscv', 'loongarch'), required=True)
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--kernel-dir', type=Path, required=True)
    args = parser.parse_args()
    directory = Path('build/sync-b1/native') / args.arch
    directory.mkdir(parents=True, exist_ok=True)
    for memory in ('512M', '1G'):
        for case in range(5):
            kernel = args.kernel_dir / f'sync-{case}'
            if not kernel.is_file(): raise SystemExit(f'missing native sync kernel: {kernel}')
            command = [args.qemu, '-machine', 'virt', '-smp', '1', '-m', memory,
                       '-kernel', str(kernel), '-nographic', '-no-reboot']
            if args.arch == 'loongarch': command += ['-cpu', 'la464']
            else: command += ['-bios', 'default']
            result = subprocess.run(command, input=b'', stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=30)
            output = result.stdout.decode(errors='replace')
            (directory / f'{memory}-{case}.log').write_text(output)
            valid = result.returncode == 0 and 'SYNC native entered' in output
            if case == 0: valid &= 'SYNC native passed' in output and 'fatal' not in output
            else:
                # 固定RV GCC的__builtin_trap生成ebreak；LA生成break并归入kernel trap。
                marker = 'fatal LA kernel trap' if args.arch == 'loongarch' else 'fatal trap scause=0x3'
                valid &= marker in output and 'SYNC forbidden entry returned' not in output
            if not valid: raise SystemExit(f'{args.arch} sync case {case} {memory} failed:\n{output[-3000:]}')
            print(f'{args.arch} sync case={case} RAM={memory} passed', flush=True)

if __name__ == '__main__': main()
