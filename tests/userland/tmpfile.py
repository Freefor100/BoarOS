#!/usr/bin/env python3
"""Run the O_TMPFILE or ext4 namespace consumer on a fresh filesystem."""
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
p.add_argument('--namespace', choices=('journal', 'nojournal'))
p.add_argument('--linux', action='store_true', help='run the namespace consumer on Linux')
p.add_argument('--block-size', type=int, choices=(1024, 4096), default=4096)
a=p.parse_args();profile=PROFILES[a.arch]
if a.linux and not a.namespace:
    p.error('--linux requires the namespace consumer')
(ROOT/'build').mkdir(exist_ok=True)
work=Path(tempfile.mkdtemp(prefix='tmpfile-',dir=ROOT/'build'))
features = '^has_journal' if a.namespace == 'nojournal' else None
disk=harness.fixture(work,a.program.resolve(),block_size=a.block_size,features=features)
qemu=a.qemu or os.environ.get('QEMU_RISCV64' if a.arch=='riscv' else 'QEMU_LOONGARCH64',profile.qemu)
command=profile.boot(qemu,a.kernel.resolve(),'1G');command[command.index('-smp')+1]=str(a.cpus)
# This contract does not assert RTC timestamps.
if a.arch=='loongarch': command=command[:-2]
command+=['-net','none','-drive',f'file={disk},if=none,format=raw,id=root','-device',profile.block('modern')]
if a.arch=='riscv':command+=['-global','virtio-mmio.force-legacy=false']
if a.linux:command+=['-append','root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=4 panic=-1']
log=work/'guest.log'
harness.run_logged(command,log,timeout=120)
text=log.read_text()
marker = 'EXT4 NAMESPACE' if a.namespace else 'TMPFILE'
shutdown = ('Power down' in text if a.linux else
            profile.root_poweroff_success(text) if a.namespace else profile.root_success(text,0))
if f'{marker} ALL PASS' not in text or f'{marker} FAIL' in text or not shutdown:
    raise RuntimeError(f'incomplete {marker} contract or root cleanup: {log}')
harness.run_logged(['e2fsck','-fn',str(disk)],work/'fsck.log',timeout=30)
disk.unlink();print(f'PASS {marker} {a.arch} cpus={a.cpus}: {log}')
