#!/usr/bin/env python3
"""Run the LA platform contract with two independent RAM layouts."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
from guest import run_guest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--kernel', default='kernel-la')
    parser.add_argument('--qemu', default=os.environ.get(
        'QEMU_LOONGARCH64', 'build/qemu-la/qemu-system-loongarch64'))
    parser.add_argument('--stage', choices=('boot', 'mmu', 'user'), default='user')
    parser.add_argument('--timeout', type=float, default=90)
    parser.add_argument('--log')
    args = parser.parse_args()
    kernel = Path(args.kernel)
    if not kernel.is_file():
        raise SystemExit('LA contract failed: kernel image is missing')
    logs = []
    for memory in ('512M', '1G'):
        command = [args.qemu, '-machine', 'virt', '-cpu', 'la464',
                   '-smp', '1', '-m', memory, '-kernel', str(kernel),
                   '-nographic', '-no-reboot']
        try:
            code, output = run_guest(command, args.timeout)
        except subprocess.TimeoutExpired as error:
            sys.stderr.write(str(error.stdout))
            raise SystemExit(f'LA contract timed out ({memory})') from error
        logs.append(output)
        sys.stdout.write(output)
        expected = f'LA {args.stage} contracts passed'
        if code or expected not in output or 'fatal' in output:
            raise SystemExit(f'LA contract failed ({memory}, exit '
                             f'{code})')
    if args.log:
        Path(args.log).write_text(''.join(logs))
    print(f'LoongArch {args.stage} contracts passed in 512M and 1G')


if __name__ == '__main__':
    main()
