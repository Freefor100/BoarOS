#!/usr/bin/env python3
"""Run static BPF syscall fixtures unchanged on fixed Linux and BoarOS."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from build import BASE, ROOT, TARGETS, LINUX_REVISION, sha
sys.path.insert(0,str(ROOT/'tests'))
from arch_profiles import PROFILES
sys.path.insert(0,str(ROOT/'tests/diff-abi'))
import harness
sys.path.insert(0,str(ROOT/'tests/loongarch'))
from guest import run_guest

WORKLOADS={'bpf_abi':'BPF ABI PASS','bpf_socket':'BPF SOCKET PASS',
           'bpf_trace':'BPF TRACE PASS','bpf_test_run':'BPF_TEST_RUN: PASS',
           'bpf_mmap':'BPF MMAP PASS'}
SUPERVISOR=r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define REQUIRE(x) do {if(!(x)){perror(#x);goto done;}}while(0)
int main(void) {
 setvbuf(stdout,NULL,_IONBF,0);int pass=0,status;
 mkdir("/proc",0755);mkdir("/sys",0755);mkdir("/tmp",01777);
 REQUIRE(!mount("proc","/proc","proc",0,NULL));
 REQUIRE(!mount("sysfs","/sys","sysfs",0,NULL));
 REQUIRE(!mount("tracefs","/sys/kernel/tracing","tracefs",0,NULL));
 int fd=socket(AF_INET,SOCK_DGRAM,0);REQUIRE(fd>=0);
 struct ifreq request={.ifr_name="lo"};REQUIRE(!ioctl(fd,SIOCGIFFLAGS,&request));
 request.ifr_flags|=IFF_UP;REQUIRE(!ioctl(fd,SIOCSIFFLAGS,&request));close(fd);
 pid_t child=fork();REQUIRE(child>=0);
 if(!child){execl("/program","/program",NULL);perror("exec program");_exit(127);}
 REQUIRE(waitpid(child,&status,0)==child);printf("BPF APPLICATION CHILD status=%d\n",status);REQUIRE(WIFEXITED(status) && !WEXITSTATUS(status));pass=1;
 done: puts(pass?"BPF APPLICATION PASS":"BPF APPLICATION FAIL");sync();if(access("/linux-reference",F_OK)==0)reboot(RB_POWER_OFF);return !pass;
}
'''

def execute(args):
 profile=PROFILES[args.arch]
 output=(args.output or BASE/args.arch/'abi').resolve()
 output.mkdir(parents=True,exist_ok=True)
 if any(output.iterdir()):raise RuntimeError('refusing to overwrite preserved BPF acceptance inputs: '+str(output))
 compiler=TARGETS[args.arch][1]+'gcc'
 flags=['-static','-O2','-pthread','-Wall','-Wextra','-Werror']
 if args.arch=='loongarch':flags+=['-Wl,-z,max-page-size=16384']
 programs={}
 reference=json.loads((args.reference/'result.json').read_text()) if args.reference else None
 if reference and (args.only!='boaros' or reference['arch']!=args.arch):raise RuntimeError('reference input requires same-architecture BoarOS-only replay')
 for name in args.workload or WORKLOADS:
  source=output/(name+'.c');shutil.copyfile(ROOT/'tests/workloads/resource'/(name+'.c'),source);program=output/name
  command=[compiler,*flags,str(source),'-o',str(program)]
  if reference:
   original=reference['programs'][name]
   if sha(source)!=original['source_sha256']:raise RuntimeError('source differs from fixed-Linux input: '+name)
   previous=Path(original['elf']);previous=previous if previous.is_absolute() else ROOT/previous
   if sha(previous)!=original['elf_sha256']:raise RuntimeError('reference ELF changed: '+name)
   for cpus in args.cpus:
    if not any(r['platform']=='linux' and r['workload']==name and r['cpus']==cpus and r['passed'] for r in reference['runs']):raise RuntimeError('reference has no passing same-ELF/CPU run: '+name+' '+str(cpus))
   shutil.copyfile(previous,program)
  else:subprocess.run(command,check=True)
  programs[name]={'elf':str(program),'elf_sha256':sha(program),'source_sha256':sha(source),'compile_command':original.get('compile_command') if reference else command}
  if reference:programs[name]['reference_elf']=str(previous)
 supervisor_source=output/'supervisor.c';supervisor_source.write_text(SUPERVISOR)
 supervisor=output/'supervisor';subprocess.run([compiler,*flags,str(supervisor_source),'-o',str(supervisor)],check=True)
 metadata={'arch':args.arch,'page_size':profile.page_size,'programs':programs,
           'compiler_sha256':sha(shutil.which(compiler)),'runner_sha256':sha(__file__),'runs':[]}
 variants=[]
 if reference:metadata['reference']={'path':str(args.reference.resolve()),'result_sha256':sha(args.reference/'result.json'),'linux':reference['linux']}
 if args.only!='boaros':
  directory=BASE/args.arch/'linux';identity=json.loads((directory/'identity.json').read_text())
  image=directory/('arch/riscv/boot/Image' if args.arch=='riscv' else 'vmlinux')
  config=(directory/'.config').read_text()
  if identity['revision']!=LINUX_REVISION:raise RuntimeError('fixed Linux source revision mismatch')
  if sha(image)!=identity['image_sha256'] or sha(directory/'.config')!=identity['configuration_sha256']:
   raise RuntimeError('fixed BPF Linux cache identity mismatch')
  if 'CONFIG_BPF_SYSCALL=y' not in config:raise RuntimeError('BPF syscall reference required')
  if max(args.cpus)>1 and 'CONFIG_SMP=y' not in config:raise RuntimeError('real SMP reference required')
  if args.arch=='loongarch' and 'CONFIG_16KB_3LEVEL=y' not in config:raise RuntimeError('real LA16KB reference required')
  metadata['linux']=identity;variants.append(('linux',image))
 if args.only!='linux':variants.append(('boaros',args.kernel or ROOT/profile.kernel))
 snapshots=[]
 for name,path in variants:
  kernel=output/(name+'-kernel');shutil.copyfile(path,kernel);metadata[name+'_kernel_sha256']=sha(kernel);snapshots.append((name,kernel))
 failures=[]
 for workload,identity in programs.items():
  for cpus in args.cpus:
   for platform,kernel in snapshots:
    case=output/(workload+'-'+platform+'-'+str(cpus)+'cpu');case.mkdir()
    image=harness.fixture(case,supervisor)
    install='write "'+str(Path(identity['elf']).resolve())+'" /program\nset_inode_field /program mode 0100755\n'
    if platform=='linux':
     marker=case/'linux-reference';marker.write_text('fixed Linux supervisor poweroff\n');install+='write "'+str(marker)+'" /linux-reference\n'
    batch=case/'program.debugfs';batch.write_text(install)
    harness.run_logged(['debugfs','-w','-f',str(batch),str(image)],case/'program-install.log')
    command=profile.boot(profile.qemu,kernel,'1G');command[command.index('-smp')+1]=str(cpus)
    command+=['-drive','file='+str(image)+',if=none,format=raw,id=root','-device',profile.block('modern'),'-net','none',
              '-object','rng-random,filename=/dev/urandom,id=entropy','-device',profile.rng()]
    if args.arch=='riscv':command+=['-global','virtio-mmio.force-legacy=false']
    if platform=='linux':command+=['-append','console=ttyS0 root=/dev/vda rw rootwait init=/init loglevel=3 panic=-1']
    try:code,raw=run_guest(command,args.timeout)
    except subprocess.TimeoutExpired as error:code=-1;raw=error.output
    if isinstance(raw,bytes):raw=raw.decode(errors='replace')
    (case/'console.log').write_text(raw)
    passed=not code and WORKLOADS[workload] in raw and 'BPF APPLICATION PASS' in raw and (platform=='linux' or profile.root_success(raw,0))
    record={'workload':workload,'platform':platform,'cpus':cpus,'memory':'1G','command':command,'returncode':code,'passed':passed,'console':str(case/'console.log')}
    metadata['runs'].append(record);(output/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print(f'{args.arch} {workload} {platform} {cpus}cpu: '+('PASS' if passed else 'FAIL'),flush=True)
    print('\n'.join(raw.splitlines()[-12:]),flush=True)
    if not passed:failures.append(record['console'])
    else:image.unlink()
 if failures:raise RuntimeError('failed BPF guests: '+', '.join(failures))

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--arch',choices=PROFILES,required=True)
 p.add_argument('--cpus',type=int,nargs='+',choices=range(1,9),default=[1,2])
 p.add_argument('--workload',choices=WORKLOADS,action='append');p.add_argument('--only',choices=('linux','boaros'))
 p.add_argument('--kernel',type=Path);p.add_argument('--reference',type=Path,help='copy verified unchanged ELF from successful same-CPU Linux result for BoarOS-only replay');p.add_argument('--output',type=Path);p.add_argument('--timeout',type=int,default=120)
 execute(p.parse_args())
