#!/usr/bin/env python3
"""Run real BusyBox logging or queue tools against Linux and BoarOS SysV IPC."""
import argparse, hashlib, json, re, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'tests/program-inventory'),str(ROOT/'tests/loongarch')]
from arch_profiles import PROFILES
from suites import build_fixture, digest
from guest import run_guest

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--arch',choices=PROFILES,default='riscv');p.add_argument('--kernel',type=Path)
    p.add_argument('--only',choices=('linux','boaros'));p.add_argument('--output',type=Path,required=True)
    p.add_argument('--workload',choices=('log','messages'),default='log')
    p.add_argument('--linux-kernel',type=Path)
    a=p.parse_args();profile=PROFILES[a.arch];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    if a.arch=='riscv':
        built=json.loads((ROOT/'build/program-environment/full-busybox/build.json').read_text());busybox=Path(built['binary'])
        assert digest(busybox)==built['binary_sha256']
    else:busybox=ROOT/'build/loongarch/busybox-source/busybox/busybox'
    program=out/'program';compiler=ROOT/'build'/a.arch/'musl-root/bin/musl-gcc'
    source='busybox_msg.c' if a.workload=='messages' else 'busybox_log.c'
    command=[str(compiler),*profile.musl_flags(compiler),'-static','-O2','-Wall','-Wextra','-Werror',str(ROOT/'tests/workloads/ipc'/source),'-o',str(program)]
    with (out/'compile.log').open('w') as log:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
    rows=[];identity=dict(busybox=dict(path=str(busybox),sha256=digest(busybox)),program=dict(path=str(program),sha256=digest(program)),compile=command,rows=rows)
    for target in ([a.only] if a.only else ['linux','boaros']):
        directory=out/target;directory.mkdir();disk=Path(build_fixture(dict(directories=['/var/run','/var/log'],files=[dict(source=str(busybox),destination='/busybox')]),directory,program)['path'])
        kernel=(a.linux_kernel or profile.linux_kernel()) if target=='linux' else (a.kernel or ROOT/profile.kernel).resolve()
        boot=profile.boot(profile.qemu,kernel,'512M')+['-net','none','-drive',f'file={disk},if=none,format=raw,id=root','-device',profile.block('modern'),'-object','rng-random,id=entropy,filename=/dev/urandom','-device',profile.rng()]
        if a.arch=='riscv':boot+=['-global','virtio-mmio.force-legacy=false']
        if target=='linux':boot+=['-append','root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
        try:code,raw=run_guest(boot,45)
        except subprocess.TimeoutExpired as error:
            code=-1;raw=error.output or ''
            if isinstance(raw,bytes):raw=raw.decode(errors='replace')
        (directory/'console.log').write_text(raw)
        prefix='MSGTOOLS' if a.workload=='messages' else 'SEMLOG'
        names=['real-ipcs-queue-content','real-ipcrm-wakes-blocked-receiver'] if a.workload=='messages' else ['daemon-unix-socket-and-ipc-ready','real-logger-concurrent-logread-content','reader-exit-undo-unblocks-real-daemon','daemon-exit-ipc-removed']
        actual=[line for line in raw.replace('\r','').splitlines() if prefix+' ' in line]
        expected=[prefix+' CASE '+x+' PASS' for x in names]+[prefix+' END '+str(len(names))]
        passed=code==0 and actual==expected and (target=='linux' or profile.root_success(raw,0))
        stack=re.findall(r'min-free=(0x[0-9a-f]+)',raw)
        if target=='boaros' and a.arch=='riscv':passed=passed and len(stack)==1 and int(stack[0],16)>=1024
        row=dict(target=target,passed=passed,kernel_sha256=digest(kernel),command=[str(x) for x in boot],returncode=code,stack_free=int(stack[0],16) if stack else None);rows.append(row)
        if passed:
            with (directory/'fsck.log').open('w') as log:row['fsck']=subprocess.run(['e2fsck','-fn',str(disk)],stdout=log,stderr=subprocess.STDOUT).returncode
            row['passed']=row['fsck']==0
            if row['passed']:disk.unlink()
        (out/'result.json').write_text(json.dumps(identity,indent=2)+'\n');print(target,'PASS' if row['passed'] else 'FAIL',flush=True)
        if not row['passed']:raise RuntimeError('logging contract or owner check failed: '+str(directory))
    return 0
if __name__=='__main__':raise SystemExit(main())
