#!/usr/bin/env python3
"""Same environment ELFs and original applets on fixed Linux then BoarOS."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from arch_profiles import PROFILES

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def quiet(argv):subprocess.run(list(map(str,argv)),check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
def fixture(work,program,busybox,apps):
    tree=work/'tree';tree.mkdir()
    for name in ('dev','bin','proc','tmp'):(tree/name).mkdir()
    shutil.copy2(program,tree/'init');shutil.copy2(busybox,tree/'busybox')
    if apps:shutil.copy2(ROOT/'tests/program-inventory/environment-check.sh',tree/'environment-check')
    for name in ('sh','dmesg','grep','hwclock','stat','df','awk','test'):(tree/'bin'/name).symlink_to('/busybox')
    disk=work/'fixture.img'
    with disk.open('wb') as stream:stream.truncate(32*1024*1024)
    quiet(['mkfs.ext4','-q','-F','-b','4096','-d',tree,disk])
    nodes=work/'nodes';nodes.write_text('cd /dev\nmknod console c 5 1\nset_inode_field console mode 020600\nmknod rtc0 c 10 135\nset_inode_field rtc0 mode 020600\n')
    quiet(['debugfs','-w','-f',nodes,disk]);return disk

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch',choices=tuple(PROFILES),default='riscv')
    parser.add_argument('--memory',choices=('512M','1G'),action='append')
    parser.add_argument('--kernel',type=Path);parser.add_argument('--linux-kernel',type=Path)
    parser.add_argument('--qemu');parser.add_argument('--rtc',default='2026-12-31T23:59:58')
    parser.add_argument('--only',choices=('both','linux','boaros'),default='both')
    parser.add_argument('--loglevel',type=int,default=0)
    args=parser.parse_args();profile=PROFILES[args.arch]
    base=ROOT/'build'/args.arch;base.mkdir(exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix='environment-run.',dir=base));print(work,flush=True)
    compiler=ROOT/('build/riscv/musl-root/bin/musl-gcc' if args.arch=='riscv' else 'build/loongarch/musl-root/bin/musl-gcc')
    environment=os.environ.copy()
    flags=['-fno-link-libatomic'] if args.arch=='riscv' else [*profile.raw_flags,'-Wl,-z,max-page-size=16384']
    if args.arch=='loongarch':environment['REALGCC']=str(ROOT/'build/loongarch/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc')
    busybox=ROOT/('build/program-environment/full-busybox/source/busybox/busybox' if args.arch=='riscv' else 'build/loongarch/busybox-source/busybox/busybox')
    cases={}
    # Both tests clear the log. Independent boots preserve each original precondition.
    for name,apps in (('probe',False),('apps',True)):
        case=work/name;case.mkdir();program=case/'init'
        subprocess.run([str(compiler),*flags,'-static','-O2','-Wall','-Wextra','-Werror',
            f'-DENVIRONMENT_APPS_ONLY={int(apps)}',str(ROOT/'tests/workloads/environment.c'),'-o',str(program)],env=environment,check=True)
        cases[name]={'program_sha256':sha(program),'fixture':fixture(case,program,busybox,apps)}
    linux=args.linux_kernel or profile.linux_kernel()
    if args.arch=='riscv':
        sys.path.insert(0,str(ROOT/'tests/diff-abi'));import harness
        linux,_=harness.fixed_linux_image(args.linux_kernel)
    qemu=args.qemu or profile.qemu
    identity={'arch':args.arch,'cases':{name:{'program':value['program_sha256'],'fixture':sha(value['fixture'])} for name,value in cases.items()},
        'busybox':sha(busybox),'script':sha(ROOT/'tests/program-inventory/environment-check.sh'),
        'rtc':args.rtc,'qemu_sha256':sha(shutil.which(qemu) or qemu),'runs':[],'status':'prepared'}
    kernels={}
    for name,kernel in [('linux',linux),('boaros',args.kernel or ROOT/profile.kernel)]:
        if args.only!='both' and args.only!=name:continue
        snapshot=work/(name+'-kernel');shutil.copy2(kernel,snapshot);kernels[name]=snapshot
    for name,snapshot in kernels.items():
        for memory in args.memory or (['512M','1G'] if args.arch=='loongarch' else ['512M']):
            for case,value in cases.items():
                run=work/(name+'-'+memory+'-'+case);run.mkdir()
                image=run/'root.img';shutil.copy2(value['fixture'],image)
                argv=profile.boot(qemu,snapshot,memory)+['-net','none','-rtc',f'base={args.rtc},clock=vm',
                    '-drive',f'file={image},if=none,format=raw,id=root','-device',profile.block('modern')]
                if args.arch=='riscv':argv+=['-global','virtio-mmio.force-legacy=false']
                if name=='linux':argv+=['-append',f'root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel={args.loglevel} panic=-1']
                with (run/'console.log').open('wb') as log:
                    result=subprocess.run(argv,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,timeout=120)
                output=(run/'console.log').read_text(errors='replace')
                good=result.returncode==0 and 'ENV PASS all' in output
                if case=='apps':good &= 'ENVIRONMENT CONTENT PASS' in output
                else:good &= 'ENV PASS log:' in output and 'ENV PASS rtc:' in output
                if name=='boaros':good &= profile.root_success(output,0)
                else:good &= bool(re.search(r'Attempted to kill init! exitcode=0x0+\b',output))
                identity['runs'].append({'name':name,'memory':memory,'case':case,'kernel':sha(snapshot),'argv':argv,'qemu_status':result.returncode,'passed':good})
                (work/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
                if not good:print(output);raise SystemExit('environment failed: '+str(run))
                print(name,memory,case,'real ELF/RTC/log/original apps/exit PASS',flush=True)
    identity['status']='passed';(work/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
if __name__=='__main__':main()
