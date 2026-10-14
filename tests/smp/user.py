#!/usr/bin/env python3
"""Same native user ELF, fixed Linux SMP and BoarOS service-handoff profile."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/diff-abi'))
import harness
from riscv import identity
CASES=('process-affinity-overlap','pthread-tls-registers-nested-signal',
       'shared-mm-remote-permission-and-truncate','pthread-cancel-pipe-owner',
       'nonleader-exec-remote-exit','realtime-service-qualification',
       'remote-service-forward-progress')

def results(text, cpus):
    rows=[line[line.index('USER-SMP '):] for line in text.replace('\r','').splitlines() if 'USER-SMP ' in line]
    expected=[f'USER-SMP BEGIN cpus={cpus}',*[f'USER-SMP CASE {name} OK' for name in CASES],f'USER-SMP END {len(CASES)}']
    if rows!=expected: raise RuntimeError(f'incomplete or failed user SMP stream: {rows}')
    return rows

def validate(text, cpus, boaros):
    rows=results(text,cpus)
    validate_execution(text,cpus,boaros,require_parallel=True)
    return rows

def validate_execution(text,cpus,boaros,require_parallel=False):
    if boaros:
        if not harness.PROFILES['riscv'].root_success(text,42):
            raise RuntimeError('PID1/heap/root closure missing')
        samples=re.findall(r'USER SMP hart=(0x[0-9a-f]+) traps=(0x[0-9a-f]+) returns=(0x[0-9a-f]+) local-irqs=(0x[0-9a-f]+) syscalls=(0x[0-9a-f]+)',text)
        if len(samples)!=cpus or len({row[0] for row in samples})!=cpus:
            raise RuntimeError('real U-mode hart observations missing/duplicated')
        selected=samples if require_parallel else samples[:1]
        if any(int(traps,16)==0 or int(returns,16)==0 for _,traps,returns,_,_ in selected):
            raise RuntimeError('a requested CPU never entered/returned actual U-mode')
        if require_parallel and cpus==2 and not int(samples[1][3],16):
            raise RuntimeError('CPU1 local IRQ return was not exercised')
        if cpus==2 and int(samples[1][4],16):
            raise RuntimeError('CPU1 dispatched legacy kernel business')

def boot_user(qemu,firmware,kernel,disk,memory,cpus,folder,linux=False):
    command=harness.PROFILES['riscv'].boot(qemu,kernel,memory)
    command+=['-net','none','-global','virtio-mmio.force-legacy=false',
        '-object','rng-random,id=entropy,filename=/dev/urandom',
        '-device','virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.5',
        '-drive',f'file={disk},if=none,format=raw,cache=writeback,id=root',
        '-device','virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
    if linux:command+=['-append','root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
    env=dict(os.environ,BOAROS_USER_SMP='1',BOAROS_SMP_QEMU=qemu,
        BOAROS_SMP_CPUS=str(cpus),BOAROS_SMP_FIRMWARE=firmware,BOAROS_SMP_OUTPUT=str(folder))
    launch=[sys.executable,str(Path(__file__).with_name('qemu.py')),*command[1:]]
    logfile=folder/f'{disk.stem}-runner.log'
    with logfile.open('w') as stream:
        completed=subprocess.run(launch,env=env,stdin=subprocess.DEVNULL,stdout=stream,stderr=subprocess.STDOUT,timeout=185)
    text=logfile.read_text(errors='replace')
    if completed.returncode:raise RuntimeError(f'{disk.stem} {memory}/{cpus}: runner failure; {logfile}')
    run=json.loads((folder/f'{disk.stem}.json').read_text())
    return text,run,logfile

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel',type=Path,default=ROOT/'build/riscv-user-smp/kernel-rv')
    parser.add_argument('--program',type=Path,default=ROOT/'build/riscv-user-smp/user-smp-rv')
    parser.add_argument('--manifest',type=Path,help='ABI record manifest instead of the native case stream')
    parser.add_argument('--qemu',default='qemu-system-riscv64')
    parser.add_argument('--firmware',default='default')
    parser.add_argument('--memory',action='append',choices=('512M','1G'))
    parser.add_argument('--cpus',action='append',type=int,choices=(1,2))
    parser.add_argument('--output',type=Path,default=ROOT/'build/riscv-user-smp/runs')
    args=parser.parse_args()
    linux,linux_identity=harness.linux_build(Path(__file__).with_name('linux-user.config'))
    actual_config=linux.parents[3]/'.config'
    if 'CONFIG_SMP=y' not in actual_config.read_text(): raise RuntimeError('reference is not SMP-enabled')
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    # 冻结双方使用的同一个ELF及核；每次启动拥有独立盘，原fixture只作输入。
    snapshots={}
    for name,path in (('linux',linux),('boaros',args.kernel),('program',args.program)):
        snapshots[name]=output/name
        shutil.copyfile(path,snapshots[name])
    report={'status':'running','linux':linux_identity,'inputs':{name:identity(path) for name,path in snapshots.items()},'runs':[]}
    expected=None
    if args.manifest:
        shutil.copyfile(args.manifest,output/'cases.txt')
        report['inputs']['manifest']=identity(output/'cases.txt')
        expected=[line for line in (output/'cases.txt').read_text().splitlines() if line and not line.startswith('#')]
    try:
        for memory in args.memory or ('512M','1G'):
            for cpus in args.cpus or (2,1):
                folder=output/f'{memory}-{cpus}cpu';folder.mkdir(exist_ok=True)
                fixture=harness.fixture(folder,snapshots['program'])
                reference=None
                for target in ('linux','boaros'):
                    disk=folder/f'{target}.img';shutil.copyfile(fixture,disk)
                    text,run,logfile=boot_user(args.qemu,args.firmware,snapshots[target],disk,memory,cpus,folder,target=='linux')
                    report['runs'].append(dict(target=target,memory=memory,cpus=cpus,**run))
                    if expected is None:rows=validate(text,cpus,target=='boaros')
                    else:
                        rows=harness.normalize(text,expected)
                        validate_execution(text,cpus,target=='boaros')
                        (folder/f'{target}.normalized').write_text(''.join(rows))
                    if reference is None:reference=rows
                    elif rows!=reference:
                        _,delta=harness.compare([line+'\n' if not line.endswith('\n') else line for line in reference],
                            [line+'\n' if not line.endswith('\n') else line for line in rows])
                        (folder/'results.diff').write_text(delta)
                        raise RuntimeError(f'same ELF observation mismatch; {folder}/results.diff')
                    if target=='boaros':
                        subprocess.run(['e2fsck','-fn',str(disk)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,check=True)
                    print(f'user SMP {target} RAM={memory} cpus={cpus}: native cases/exit validated',flush=True)
        report['status']='passed'
    except Exception as error:
        report.update(status='failed',error=str(error));raise
    finally:(output/'report.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
