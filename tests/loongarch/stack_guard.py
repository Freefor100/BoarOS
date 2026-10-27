#!/usr/bin/env python3
"""Guard, exhausted trap SP, NX and withdrawn kernel translations use real faults."""
from pathlib import Path
import subprocess
import sys
import re
import tempfile
from guest import run_guest

ROOT=Path(__file__).resolve().parents[2]
work=Path(tempfile.mkdtemp(prefix='stack-guard-run.',dir=ROOT/'build/loongarch'))
if sys.argv[1:]==['--smp']:
    command=['build/qemu-la-rtc/qemu-system-loongarch64','-machine','virt','-cpu','la464','-global','ls7a_rtc.toy-enabled=on','-smp','2','-m','512M','-kernel','build/loongarch-smp2/kernel-smp-guard','-nographic','-no-reboot','-net','none']
    code,text=run_guest(command,20);(work/'smp-guard.log').write_text(text)
    match=re.search(r'LA SMP emergency stacks distinct PASS cpu0=(0x[0-9a-f]+) cpu1=(0x[0-9a-f]+)',text)
    if code or not match or int(match[1],16)>>14==int(match[2],16)>>14 or 'fatal LA kernel stack guard' not in text:
        print(text);raise SystemExit('parallel emergency stack failure')
    print('LA two simultaneous kernel guard faults: separate emergency stacks PASS; logs',work)
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
