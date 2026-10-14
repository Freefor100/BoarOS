#!/usr/bin/env python3
"""Original musl consumers and five GNU forms in the actual user SMP profile."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from user import ROOT,boot_user,harness,identity,validate_execution,create_run_directory

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel',type=Path,default=ROOT/'build/riscv-user-smp/kernel-rv')
    parser.add_argument('--qemu',default='qemu-system-riscv64')
    parser.add_argument('--firmware',default='default')
    parser.add_argument('--memory',action='append',choices=('512M','1G'))
    parser.add_argument('--cpus',action='append',type=int,choices=(1,2))
    parser.add_argument('--program',type=Path,required=True)
    parser.add_argument('--pthread-program',type=Path,required=True)
    parser.add_argument('--tls',type=Path,required=True)
    parser.add_argument('--loader',type=Path,required=True)
    parser.add_argument('--output',type=Path,help='new or empty explicit run directory')
    args=parser.parse_args();output=create_run_directory(args.output,ROOT/'build/riscv-user-smp/runtime')
    snapshots={}
    for name,path in (('kernel',args.kernel),('program',args.program),('pthread-program',args.pthread_program),('tls',args.tls),('loader',args.loader)):
        snapshots[name]=output/name;shutil.copyfile(path,snapshots[name])
    report={'status':'running','inputs':{name:identity(path) for name,path in snapshots.items()},'musl':[],'glibc':[]}
    try:
        for memory in args.memory or ('512M','1G'):
            for cpus in args.cpus or (2,1):
                folder=output/f'musl-{memory}-{cpus}cpu';folder.mkdir(exist_ok=True)
                env=dict(os.environ,BOAROS_USER_SMP='1',BOAROS_SMP_QEMU=args.qemu,
                    BOAROS_SMP_CPUS=str(cpus),BOAROS_SMP_FIRMWARE=args.firmware,BOAROS_SMP_OUTPUT=str(folder),
                    QEMU_RISCV64=str(ROOT/'tests/smp/qemu.py'),KERNEL_RV=str(snapshots['kernel']),
                    REAL_USERLAND_RV=str(snapshots['program']),PTHREAD_USERLAND_RV=str(snapshots['pthread-program']),
                    PTHREAD_TLS_DSO_RV=str(snapshots['tls']),MUSL_LDSO=str(snapshots['loader']),
                    QEMU_MEMORY=memory,USERLAND_TIMEOUT='185s')
                with (folder/'consumer.log').open('w') as stream:
                    subprocess.run(['sh',str(ROOT/'tests/userland-riscv.sh')],env=env,stdout=stream,stderr=subprocess.STDOUT,check=True)
                for name in ('static-root','pthread-root'):
                    validate_execution((folder/f'{name}.log').read_text(errors='replace'),cpus,True)
                    report['musl'].append(dict(memory=memory,cpus=cpus,**json.loads((folder/f'{name}.json').read_text())))
                print(f'user SMP original musl {memory}/{cpus}: static/pthread and owners passed',flush=True)
        sys.path.insert(0,str(ROOT/'tests/userland/glibc'))
        spec=importlib.util.spec_from_file_location('glibc_smp_consumer',ROOT/'tests/userland/glibc/run.py')
        glibc=importlib.util.module_from_spec(spec);spec.loader.exec_module(glibc)
        glibc.BUILD=output/'glibc';inputs=glibc.checked_inputs('riscv')
        programs,library=glibc.build_programs(inputs)
        report['glibc_inputs']=inputs
        report['glibc_programs']={name:identity(path) for name,path in programs.items()}
        report['glibc_library']=identity(library)
        linux,linux_identity=harness.linux_build(Path(__file__).with_name('linux-user.config'))
        report['linux']=linux_identity
        for memory in args.memory or ('512M','1G'):
            for cpus in args.cpus or (2,1):
                for name,program in programs.items():
                    folder=output/f'glibc-{name}-{memory}-{cpus}cpu';folder.mkdir(exist_ok=True)
                    fixture=glibc.fixture(folder,program,library,inputs)
                    expected=glibc.BASE_MARKERS+(glibc.EXTRA_MARKERS if name=='pthread-pie' else [])+glibc.FINAL_MARKERS
                    reference=None
                    for target,kernel in (('linux',linux),('boaros',snapshots['kernel'])):
                        disk=folder/f'{target}.img';shutil.copyfile(fixture,disk)
                        text,run,log=boot_user(args.qemu,args.firmware,kernel,disk,memory,cpus,folder,target=='linux')
                        rows=glibc.check_observation(name,target,log,expected,run['returncode'])
                        validate_execution(text,cpus,target=='boaros')
                        report['glibc'].append(dict(target=target,memory=memory,cpus=cpus,form=name,**run))
                        if reference is None:reference=rows
                        elif rows!=reference:raise RuntimeError('same GNU ELF runtime observations differ')
                    print(f'user SMP glibc {inputs["glibc_version"]} {name} {memory}/{cpus}: Linux/BoarOS exit verified',flush=True)
        glibc.checked_inputs('riscv');report['status']='passed'
    except Exception as error:report.update(status='failed',error=str(error));raise
    finally:(output/'report.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
