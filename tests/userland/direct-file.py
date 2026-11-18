#!/usr/bin/env python3
"""Focused ext4 O_DIRECT syscall contract, using an already built static ELF/kernel."""
import argparse
import os
from pathlib import Path
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
p.add_argument('--nojournal',action='store_true',help='use preseeded regular files on ext4 without a journal')
p.add_argument('--cpus',type=int,default=1)
a=p.parse_args();profile=PROFILES[a.arch]
(ROOT/'build').mkdir(exist_ok=True)
work=Path(tempfile.mkdtemp(prefix='direct-file-',dir=ROOT/'build'))
disk=harness.fixture(work,a.program.resolve())
if a.nojournal:
    harness.run_logged(['tune2fs','-O','^has_journal,^orphan_file',str(disk)],work/'nojournal.log')
    empty=work/'empty';empty.write_bytes(b'')
    for name in ('direct-input','direct-output'):
        harness.run_logged(['debugfs','-w','-R',f'write {empty} /{name}',str(disk)],work/f'{name}.log')
qemu=a.qemu or os.environ.get('QEMU_RISCV64' if a.arch=='riscv' else 'QEMU_LOONGARCH64',profile.qemu)
command=profile.boot(qemu,a.kernel.resolve(),'1G');command[command.index('-smp')+1]=str(a.cpus)
# This contract does not assert RTC timestamps.
if a.arch=='loongarch': command=command[:-2]
command+=['-net','none','-drive',f'file={disk},if=none,format=raw,id=root','-device',profile.block('modern')]
if a.arch=='riscv':command+=['-global','virtio-mmio.force-legacy=false']
log=work/'guest.log'
harness.run_logged(command,log,timeout=120)
text=log.read_text()
if ('DIRECT FILE ALL PASS' not in text or 'DIRECT FILE FAIL' in text or
    not profile.root_success(text,0) or
    (a.arch=='loongarch' and not any(line.endswith(' heap-live=0') for line in text.splitlines()))):
    raise RuntimeError(f'incomplete ext4 O_DIRECT contract or root cleanup: {log}')
harness.run_logged(['e2fsck','-fn',str(disk)],work/'fsck.log',timeout=30)
if a.nojournal:
    page=16384 if a.arch=='loongarch' else 4096
    prefix=b'z'+b'A'*122+bytes(page-123)
    expected={'direct-input':prefix+bytes(3*page)+b'q'*page,
              'direct-output':prefix*2}
    for name,wanted in expected.items():
        output=work/f'{name}.bytes'
        harness.run_logged(['debugfs','-R',f'dump /{name} {output}',str(disk)],work/f'{name}-dump.log')
        if output.read_bytes()!=wanted:
            raise RuntimeError(f'persisted nojournal data/EOF mismatch: {output}')
disk.unlink();print(f'PASS ext4 O_DIRECT {a.arch} cpus={a.cpus}: {log}')
