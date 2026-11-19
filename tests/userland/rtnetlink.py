#!/usr/bin/env python3
"""Focused read-only RTNETLINK syscall contract, using an already built static ELF/kernel."""
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
p.add_argument('--ip',type=Path,help='optional static iproute2 ip ELF')
p.add_argument('--network',action='store_true',help='include the real virtio Ethernet interface and default route')
p.add_argument('--cpus',type=int,default=1)
a=p.parse_args();profile=PROFILES[a.arch]
(ROOT/'build').mkdir(exist_ok=True)
work=Path(tempfile.mkdtemp(prefix='rtnetlink-',dir=ROOT/'build'))
disk=harness.fixture(work,a.program.resolve())
if a.ip:
    harness.run_logged(['debugfs','-w','-R',f'write {a.ip.resolve()} /ip',str(disk)],work/'ip-install.log')
    harness.run_logged(['debugfs','-w','-R','set_inode_field /ip mode 0100755',str(disk)],work/'ip-mode.log')
qemu=a.qemu or os.environ.get('QEMU_RISCV64' if a.arch=='riscv' else 'QEMU_LOONGARCH64',profile.qemu)
command=profile.boot(qemu,a.kernel.resolve(),'1G');command[command.index('-smp')+1]=str(a.cpus)
command+=['-drive',f'file={disk},if=none,format=raw,id=root','-device',profile.block('modern')]
if a.network:
    device='virtio-net-device,netdev=n0,bus=virtio-mmio-bus.1' if a.arch=='riscv' else 'virtio-net-pci,netdev=n0,addr=2,disable-legacy=on'
    command+=['-netdev','user,id=n0','-device',device]
else:command+=['-net','none']
if a.arch=='riscv':command+=['-global','virtio-mmio.force-legacy=false']
log=work/'guest.log'
harness.run_logged(command,log,timeout=120)
text=log.read_text()
if (a.ip and 'RTNETLINK iproute2 PASS' not in text) or 'RTNETLINK ALL PASS' not in text or 'RTNETLINK FAIL' in text or 'RTNETLINK answers:' in text or 'ioctl(' in text or not profile.root_poweroff_success(text):
    raise RuntimeError(f'incomplete RTNETLINK contract or root cleanup: {log}')
if a.network and a.ip and ('eth0' not in text or 'default via' not in text):
    raise RuntimeError(f'missing real Ethernet/default route: {log}')
disk.unlink();print(f'PASS read-only RTNETLINK {a.arch} cpus={a.cpus}: {log}')
