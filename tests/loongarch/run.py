#!/usr/bin/env python3
"""Run the LA platform contract with two independent RAM layouts."""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--kernel', default='kernel-la')
    parser.add_argument('--qemu', default=os.environ.get(
        'QEMU_LOONGARCH64', 'build/qemu-la/qemu-system-loongarch64'))
    parser.add_argument('--stage', choices=('boot', 'user'), default='user')
    args = parser.parse_args()
    kernel = Path(args.kernel)
    if not kernel.is_file():
        raise SystemExit('LA contract failed: kernel image is missing')
    for memory in ('512M', '1G'):
        command = [args.qemu, '-machine', 'virt', '-cpu', 'la464',
                   '-smp', '1', '-m', memory, '-kernel', str(kernel),
                   '-nographic', '-no-reboot']
        try:
            result = subprocess.run(command, capture_output=True, text=True,
                                    timeout=90)
        except subprocess.TimeoutExpired as error:
            sys.stderr.write(str(error.stdout))
            raise SystemExit(f'LA contract timed out ({memory})') from error
        output = result.stdout + result.stderr
        sys.stdout.write(output)
        expected = 'LA boot contracts passed' if args.stage == 'boot' else \
                   'LA user contracts passed'
        if result.returncode or expected not in output or 'fatal' in output:
            raise SystemExit(f'LA contract failed ({memory}, exit '
                             f'{result.returncode})')
    print(f'LoongArch {args.stage} contracts passed in 512M and 1G')


if __name__ == '__main__':
    main()
