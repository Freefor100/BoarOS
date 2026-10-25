#!/usr/bin/env python3
"""Require fatal ownership diagnostics from independent guest boots."""
import argparse
from guest import run_guest

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--qemu',required=True)
parser.add_argument('--case',type=int,choices=range(1,6),action='append')
args=parser.parse_args()
for memory in ('512M','1G'):
    for case in args.case or (1,2,3,4,5):
        code,output=run_guest([args.qemu,'-machine','virt','-cpu','la464','-global','ls7a_rtc.toy-enabled=on','-smp','1',
                              '-m',memory,'-kernel',f'build/loongarch/kernel-fatal-{case}',
                              '-nographic','-no-reboot'],20)
        expected=('physical page release fatal' if case<3 else 'fatal LA page table owner' if case==3
                  else 'trap code=0x0000000000000001' if case==4 else 'LA exhausted kernel SP=0x0000000000001234')
        intact=True
        if case==4:
            intact=(output.count('trap code=')==1 and ' badv=0x0000000000000010 ra=' in output
                    and output.count('BoarOS: fatal LA kernel trap')==1)
        if case==5:
            intact=(output.count(expected)==1 and 'trap code=' not in output
                    and output.count('BoarOS: fatal LA kernel stack guard')==1)
        if code or expected not in output or 'unexpectedly returned' in output or not intact:
            raise SystemExit(f'LA fatal contract {case}/{memory} failed:\n{output}')
        print(f'LA fatal owner {case}/{memory}: {expected}')
print('LA allocator, page-table and held-lock fatal contracts passed')
