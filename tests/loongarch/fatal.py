#!/usr/bin/env python3
"""Require fatal ownership diagnostics from independent guest boots."""
import argparse
from guest import run_guest

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--qemu',required=True)
args=parser.parse_args()
for memory in ('512M','1G'):
    for case in (1,2,3):
        code,output=run_guest([args.qemu,'-machine','virt','-cpu','la464','-smp','1',
                              '-m',memory,'-kernel',f'build/loongarch/kernel-fatal-{case}',
                              '-nographic','-no-reboot'],20)
        expected='physical page release fatal' if case<3 else 'fatal LA page table owner'
        if code or expected not in output or 'unexpectedly returned' in output:
            raise SystemExit(f'LA fatal contract {case}/{memory} failed:\n{output}')
        print(f'LA fatal owner {case}/{memory}: {expected}')
print('LA allocator and page-table fatal contracts passed')
