#!/usr/bin/env python3
"""Focused MAP_GROWSDOWN syscall contract, using an already built static ELF/kernel."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'tests/diff-abi')]
from arch_profiles import PROFILES
import harness
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--arch',choices=PROFILES,default='riscv')
p.add_argument('--kernel',type=Path,required=True)
p.add_argument('--program',type=Path,required=True)
p.add_argument('--qemu')
p.add_argument('--cpus',type=int,default=1)
p.add_argument('--stock-la-rtc',action='store_true',help='Use stock QEMU without the optional TOY RTC property; this test does not check wall-clock time')
a=p.parse_args();profile=PROFILES[a.arch]
(ROOT/'build').mkdir(exist_ok=True)
work=Path(tempfile.mkdtemp(prefix='growdown-',dir=ROOT/'build'))
disk=harness.fixture(work,a.program.resolve())
qemu=a.qemu or os.environ.get('QEMU_RISCV64' if a.arch=='riscv' else 'QEMU_LOONGARCH64',profile.qemu)
command=profile.boot(qemu,a.kernel.resolve(),'1G');command[command.index('-smp')+1]=str(a.cpus)
if a.arch=='loongarch' and a.stock_la_rtc:
    i=command.index('-global');del command[i:i+2]
command+=['-net','none','-drive',f'file={disk},if=none,format=raw,id=root','-device',profile.block('modern')]
if a.arch=='riscv':command+=['-global','virtio-mmio.force-legacy=false']
log=work/'guest.log'
harness.run_logged(command,log,timeout=120)
text=log.read_text()
if 'GROWDOWN ALL PASS' not in text or 'GROWDOWN FAIL' in text or not profile.root_poweroff_success(text):
    raise RuntimeError(f'incomplete GROWDOWN contract or root cleanup: {log}')
harness.run_logged(['e2fsck','-fn',str(disk)],work/'fsck.log',timeout=30)
disk.unlink();print(f'PASS MAP_GROWSDOWN {a.arch} cpus={a.cpus}: {log}')
