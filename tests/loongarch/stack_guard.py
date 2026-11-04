#!/usr/bin/env python3
"""Guard, exhausted trap SP, NX and withdrawn kernel translations use real faults."""
from pathlib import Path
import subprocess
import sys
import re
import tempfile
import argparse
from guest import run_guest

ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--smp',type=int,nargs='?',const=2,choices=(2,4,8));p.add_argument('--kernel',type=Path);p.add_argument('--output',type=Path);args=p.parse_args()
if args.output:
    work=args.output.resolve();work.mkdir()
else:
    work=Path(tempfile.mkdtemp(prefix='stack-guard-run.',dir=ROOT/'build/loongarch'))
if args.smp:
    kernel=args.kernel or Path(f'build/loongarch-smp{args.smp}/kernel-smp-guard')
    command=['build/qemu-la-rtc/qemu-system-loongarch64','-machine','virt','-cpu','la464','-global','ls7a_rtc.toy-enabled=on','-smp',str(args.smp),'-m','512M','-kernel',str(kernel),'-nographic','-no-reboot','-net','none']
    code,text=run_guest(command,20);(work/'smp-guard.log').write_text(text)
    match=re.search(r'LA SMP emergency stacks distinct PASS cpus=(0x[0-9a-f]+)((?: CPU=0x[0-9a-f]+ SP=0x[0-9a-f]+)+)',text)
    stacks=[(int(cpu,16),int(sp,16)>>14) for cpu,sp in re.findall(r' CPU=(0x[0-9a-f]+) SP=(0x[0-9a-f]+)',match[2])] if match else []
    if code or not match or int(match[1],16)!=args.smp or [cpu for cpu,sp in stacks]!=list(range(args.smp)) or len({sp for cpu,sp in stacks})!=args.smp or 'fatal LA kernel stack guard' not in text:
        print(text);raise SystemExit('parallel emergency stack failure')
    print(f'LA {args.smp} simultaneous kernel guard faults: separate emergency stacks PASS; logs',work)
    raise SystemExit(0)
markers=['guard write','guard SP overflow','stack NX','withdrawn translation']
for case,marker in enumerate(markers):
    for memory in ('512M','1G'):
        command=['build/qemu-la-rtc/qemu-system-loongarch64','-machine','virt','-cpu','la464','-global','ls7a_rtc.toy-enabled=on','-smp','1','-m',memory,'-kernel',f'build/loongarch/kernel-stack-guard-{case}','-nographic','-no-reboot','-net','none']
        code,text=run_guest(command,20);(work/f'{case}-{memory}.log').write_text(text)
        fatal='kernel stack guard' if case<2 else 'kernel trap'
        if code or f'LA actual {marker} ready' not in text or f'BoarOS: fatal LA {fatal}' not in text or 'guard missed' in text:
            print(text);raise SystemExit(f'LA stack guard failed {case}/{memory}; {work}')
        print(f'LA stack {marker}/{memory}: real fault PASS')
