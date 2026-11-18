#!/usr/bin/env python3
"""One two-CPU RT migration: native user progress, actual IPI and root cleanup."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from user import create_run_directory, harness, validate_execution
from riscv import identity

ROOT=Path(__file__).resolve().parents[2]
MARKER='RT-BALANCE PASS source=0 destination=1 progress-before-blocker-exit children=3'

def busy_ipi_handoffs(trace,secondary,first,size,getcpu_first,getcpu_size):
    events=[(int(asynchronous),int(cause,16),int(pc,16))
        for cpu,asynchronous,cause,pc in re.findall(
            r'hart:(\d+), async:(\d+), cause:([0-9a-f]+), epc:(?:0x)?([0-9a-f]+)',trace)
        if int(cpu)==secondary]
    handoffs=[]
    for index,(asynchronous,cause,pc) in enumerate(events):
        if asynchronous!=1 or cause!=1 or not first<=pc<first+size:continue
        for next_async,next_cause,next_pc in events[index+1:]:
            if next_async and next_cause==5:break
            if not next_async and next_cause==8:
                # Reject a late IPI followed by FIFO10's exit after migration
                # already finished. Only the movable task uses this trap site.
                if getcpu_first<=next_pc<getcpu_first+getcpu_size:
                    handoffs.append({'ipi_pc':pc,'next_user_ecall_pc':next_pc})
                break
    return handoffs


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel',type=Path,required=True)
    parser.add_argument('--program',type=Path,required=True)
    parser.add_argument('--qemu',default='qemu-system-riscv64')
    parser.add_argument('--nm',default='riscv64-linux-gnu-nm')
    parser.add_argument('--firmware',default='default')
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    folder=create_run_directory(args.output,ROOT/'build/rt-balance-guest/runs')
    program=folder/'init';kernel=folder/'kernel-rv'
    shutil.copyfile(args.program,program);shutil.copyfile(args.kernel,kernel)
    symbols=subprocess.check_output([args.nm,'-S',str(program)],text=True)
    def symbol_range(name):
        match=re.search(r'^([0-9a-f]+) ([0-9a-f]+) T '+re.escape(name)+r'$',symbols,re.M)
        if not match:raise RuntimeError('required user code symbol is missing: '+name)
        return tuple(int(value,16) for value in match.groups())
    first,size=symbol_range('rt_balance_low_busy')
    getcpu_first,getcpu_size=symbol_range('rt_balance_moved_getcpu')
    disk=harness.fixture(folder,program)
    trace=folder/'interrupts.log'
    command=[sys.executable,str(ROOT/'tests/smp/qemu.py'),'-machine','virt','-bios',args.firmware,
        '-kernel',str(kernel),'-m','512M','-smp','2','-nographic','-no-reboot','-net','none',
        '-global','virtio-mmio.force-legacy=false',
        '-drive',f'file={disk},if=none,format=raw,cache=writeback,id=root',
        '-device','virtio-blk-device,drive=root,bus=virtio-mmio-bus.0',
        '-object','rng-random,id=entropy,filename=/dev/urandom',
        '-device','virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.5',
        '-d','int','-D',str(trace)]
    env=dict(os.environ,BOAROS_USER_SMP='1',BOAROS_SMP_QEMU=args.qemu,BOAROS_SMP_CPUS='2',
        BOAROS_SMP_FIRMWARE=args.firmware,BOAROS_SMP_OUTPUT=str(folder),BOAROS_SMP_TIMEOUT='30')
    report={'status':'running','program':identity(program),'kernel':identity(kernel),
        'low_loop':{'start':first,'size':size},
        'moved_getcpu':{'start':getcpu_first,'size':getcpu_size}}
    try:
        with (folder/'console.log').open('w') as stream:
            result=subprocess.run(command,env=env,stdin=subprocess.DEVNULL,stdout=stream,
                stderr=subprocess.STDOUT,timeout=40)
        text=(folder/'console.log').read_text()
        if result.returncode:raise RuntimeError('guest failed or timed out; see console.log')
        if text.replace('\r','').splitlines().count(MARKER)!=1:
            raise RuntimeError('native migration/progress/child-cleanup handshake did not complete')
        validate_execution(text,2,True,require_parallel=True)
        hart=re.search(r'SMP online logical=1 hart=0x([0-9a-f]+)',text)
        if not hart:raise RuntimeError('secondary hardware identity missing')
        secondary=int(hart[1],16)
        handoffs=busy_ipi_handoffs(trace.read_text(),secondary,first,size,getcpu_first,getcpu_size)
        if not handoffs:raise RuntimeError('no busy FIFO10 IPI followed by movable getcpu before a timer IRQ')
        report['busy_cpu_ipi_handoffs']=handoffs
        harness.run_logged([sys.executable,str(ROOT/'tests/tools/check-stack-report.py'),str(folder/'console.log')],folder/'stack.log')
        harness.run_logged(['e2fsck','-fn',str(disk)],folder/'fsck.log')
        report['status']='passed';disk.unlink()
        print(MARKER+'; real busy-loop IPI and PID1/heap/stack/fsck cleanup verified')
    except Exception as error:
        report.update(status='failed',error=str(error));raise
    finally:
        (folder/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        print('RT balance report:',folder/'report.json',flush=True)

if __name__=='__main__':main()
