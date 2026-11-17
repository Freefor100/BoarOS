#!/usr/bin/env python3
"""Focused block O_DIRECT syscall contract, using an already built static ELF/kernel."""
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
p.add_argument('--stock-la-rtc',action='store_true',help='omit optional QEMU RTC extension; this test does not check wall-clock time')
a=p.parse_args();profile=PROFILES[a.arch]
(ROOT/'build').mkdir(exist_ok=True)
work=Path(tempfile.mkdtemp(prefix='direct-block-',dir=ROOT/'build'))
disk=harness.fixture(work,a.program.resolve())
qemu=a.qemu or os.environ.get('QEMU_RISCV64' if a.arch=='riscv' else 'QEMU_LOONGARCH64',profile.qemu)
command=profile.boot(qemu,a.kernel.resolve(),'1G');command[command.index('-smp')+1]=str(a.cpus)
if a.arch=='loongarch' and a.stock_la_rtc:
    i=command.index('ls7a_rtc.toy-enabled=on');del command[i-1:i+1]
command+=['-net','none','-drive',f'file={disk},if=none,format=raw,id=root','-device',profile.block('modern')]
if a.arch=='riscv':command+=['-global','virtio-mmio.force-legacy=false']
log=work/'guest.log'
harness.run_logged(command,log,timeout=120)
text=log.read_text()
if 'DIRECT BLOCK ALL PASS' not in text or 'DIRECT BLOCK FAIL' in text or not profile.root_success(text,0):
    raise RuntimeError(f'incomplete O_DIRECT contract or root cleanup: {log}')
disk.unlink();print(f'PASS block O_DIRECT {a.arch} cpus={a.cpus}: {log}')
