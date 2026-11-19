#!/usr/bin/env python3
"""Run a supplied static iperf3 on both ends of a small guest-loopback transfer.

This validates TCP_INFO consumer compatibility, not throughput or latency.
"""
import argparse
import os
from pathlib import Path
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'tests/diff-abi')]
import harness
from arch_profiles import PROFILES
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--arch',choices=PROFILES,default='riscv')
p.add_argument('--kernel',type=Path,required=True)
p.add_argument('--program',type=Path,required=True,help='static tcp_info_iperf.c driver')
p.add_argument('--iperf',type=Path,required=True,help='actual static target iperf3 binary')
p.add_argument('--qemu')
p.add_argument('--cpus',type=int,default=2)
a=p.parse_args();profile=PROFILES[a.arch]
w=Path(tempfile.mkdtemp(prefix='tcpinfo-iperf-',dir=ROOT/'build'))
disk=harness.fixture(w,a.program.resolve())
# Copy into our known fixture path instead of interpolating a caller path into debugfs.
import shutil
shutil.copyfile(a.iperf,w/'iperf3')
commands=w/'add.debugfs'
commands.write_text(f'write {w}/iperf3 /iperf3\nset_inode_field /iperf3 mode 0100755\n'
    'mkdir /tmp\ncd /dev\nmknod urandom c 1 9\nset_inode_field urandom mode 020666\n')
harness.run_logged(['debugfs','-w','-f',str(commands),str(disk)],w/'add.log')
qemu=a.qemu or os.environ.get('QEMU_RISCV64' if a.arch=='riscv' else 'QEMU_LOONGARCH64',profile.qemu)
command=profile.boot(qemu,a.kernel.resolve(),'1G');command[command.index('-smp')+1]=str(a.cpus)
command+=['-net','none','-object','rng-random,id=entropy,filename=/dev/urandom','-device',profile.rng(),
    '-drive',f'file={disk},if=none,format=raw,id=root','-device',profile.block('modern')]
if a.arch=='riscv':command+=['-global','virtio-mmio.force-legacy=false']
harness.run_logged(command,w/'guest.log',timeout=90)
text=(w/'guest.log').read_text()
if 'IPERF TCPINFO PASS' not in text or not profile.root_poweroff_success(text) or 'getsockopt -' in text:
    raise RuntimeError(f'incomplete iperf consumer check: {w}/guest.log')
# Each endpoint prints JSON. Decode both, not a substring mistaken for success.
import json
clean=text.replace('\r','');decoder=json.JSONDecoder();reports=[];position=0
while position<len(clean):
    start=clean.find('{',position)
    if start<0:break
    try:report,length=decoder.raw_decode(clean[start:])
    except ValueError:position=start+1;continue
    if isinstance(report,dict) and 'start' in report and 'end' in report:reports.append(report)
    position=start+length
if len(reports)!=2 or any('error' in r for r in reports):
    raise RuntimeError(f'expected two successful iperf JSON reports: {w}/guest.log')
metrics=[]
for report in reports:
    if report['end']['sum_received']['bytes']!=32768 or (report['end']['sum_sent'].get('sender') and report['end']['sum_sent']['bytes']!=32768):
        raise RuntimeError(f'iperf byte accounting mismatch: {w}/guest.log')
    metrics.extend(s['sender'] for s in report['end']['streams']
                   if 'sender' in s and 'max_snd_wnd' in s['sender'])
if not metrics or any(s.get('retransmits')!=0 or s.get('max_snd_cwnd',0)<=0 or
                      s.get('max_snd_wnd',0)<=0 for s in metrics):
    raise RuntimeError(f'iperf TCP_INFO values missing or invalid: {w}/guest.log')

disk.unlink();print(f'PASS iperf TCP_INFO {a.arch} cpus={a.cpus}: {w}/guest.log')
