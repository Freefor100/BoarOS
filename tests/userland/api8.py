#!/usr/bin/env python3
"""Run selected user ABI callers on Linux and BoarOS using identical ELF files."""
import sys
from pathlib import Path
sys.path[:0] = [str(Path(__file__).resolve().parent), str(Path(__file__).resolve().parents[1])]
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'tests/diff-abi'),str(ROOT/'tests/loongarch')]
import harness
from arch_profiles import PROFILES

SOURCES={'init':'resource/api8.c','fd_events':'resource/fd_events.c',
         'flock':'locks/flock.c','mmsg':'network/mmsg.c','copy_range':'io/copy_range.c',
         'icache':'resource/icache.c','sched_query':'resource/sched_query.c'}
LEGACY=tuple(name for name in SOURCES if name!='init')
SOURCES['vectors_v2']='io/vectors_v2.c'
SOURCES['file_advice']='io/file_advice.c'
SOURCES['splice']='io/splice.c'
SOURCES['memory_advice']='resource/madvise.c'
SOURCES['atime']='resource/mount_attributes.c'
SOURCES['propagation']='resource/mount_namespace.c'
SOURCES['mount_api']='resource/mount_namespace.c'
SOURCES['mount_context']='resource/mount_namespace.c'
SOURCES['system_control']='resource/system_control.c'
SOURCES['sched_control']='resource/sched_query.c'
SOURCES['async_io']='io/async_io.c'
SOURCES['tty_async']='tty/pty_probe.c'
SOURCES['uts_namespace']='resource/uts_namespace.c'
SOURCES['ipc_namespace']='resource/ipc_namespace.c'
SOURCES['pid_namespace']='resource/pid_namespace.c'
SOURCES['nsfs_pid']='resource/nsfs_pid.c'
SOURCES['unix_control']='network/unix_control.c'
SOURCES['pid_namespace_misc']='resource/pid_namespace_misc.c'
SOURCES['pid_namespace_creation']='resource/pid_namespace_creation.c'
SOURCES['pid_namespace_reboot']='resource/pid_namespace_reboot.c'
SOURCES['pid_namespace_clocks']='resource/pid_namespace_clocks.c'
SOURCES['ioprio']='resource/ioprio.c'
SOURCES['pid_namespace_priority']='resource/pid_namespace_priority.c'
SOURCES['cgroup']='resource/cgroup.c'
SOURCES['pid_namespace_cgroup']='resource/pid_namespace_cgroup.c'
SOURCES['pid_namespace_groups']='resource/pid_namespace_groups.c'
SOURCES['pid_namespace_ipc']='resource/pid_namespace_ipc.c'
SOURCES['pid_namespace_socket']='resource/pid_namespace_socket.c'
SOURCES['pi']='futex/pi.c'
SOURCES['robust_exec']='futex/robust_exec.c'
SOURCES['process_memory']='resource/process_memory.c'
SOURCES['personality']='resource/personality.c'
SOURCES['ptrace']='resource/ptrace.c'
SOURCES['ptrace_tools']='resource/ptrace_tools.c'
SOURCES['ptrace_signal']='resource/ptrace_signal.c'
SOURCES['process_control']='resource/process_control.c'
SOURCES['file_flags']='resource/file_flags.c'
SOURCES['file_ioctl']='resource/fd_events.c'
SOURCES['ipv4_abi']='network/ipv4_abi.c'

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def build(args,work):
    profile=PROFILES[args.arch]
    musl=ROOT/('build/riscv-user-smp4/riscv/musl-root' if args.arch=='riscv'
               else 'build/loongarch/musl-root')
    compiler=(Path('/usr/bin/riscv64-linux-gnu-gcc') if args.arch=='riscv'
              else ROOT/'build/tools/loongarch/bin/loongarch64-unknown-linux-gnu-gcc')
    if args.libc=='musl':compiler=musl/'bin/musl-gcc'
    env=os.environ.copy()
    if args.arch=='loongarch' and args.libc=='musl':
        env['REALGCC']=str(ROOT/'build/loongarch/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc')
    flags=profile.musl_flags(compiler) if args.libc=='musl' else []
    if args.arch=='loongarch' and args.libc=='glibc':flags+=['-Wl,-z,max-page-size=16384']
    if args.dynamic:
        loader=re.search(r'-dynamic-linker\s+(\S+)',(musl/'lib/musl-gcc.specs').read_text())[1]
        flags += ['-Wl,--dynamic-linker=/lib/'+Path(loader).name]
    programs={}
    for name,source in SOURCES.items():
        if name!='init' and name not in args.programs:continue
        if name=='icache' and args.arch!='riscv':continue
        destination=work/name
        command=[str(compiler),*flags,*([] if args.dynamic else ['-static']),
                 '-O2','-pthread','-Wall','-Wextra','-Werror',
                 *(['-DSCHED_CONTROL_ONLY=1'] if name=='sched_control' else []),
                 *(['-DFIONBIO_ONLY=1'] if name=='file_ioctl' else []),
                 *(['-DTTY_ASYNC_ONLY=1'] if name=='tty_async' else []),
                 *(['-DMADVISE_POPULATION_ONLY=1'] if name=='memory_advice' else []),
                 *(['-DMOUNT_ATIME_ONLY=1'] if name=='atime' else []),
                 *(['-DMOUNT_PROPAGATION_ONLY=1'] if name=='propagation' else []),
                 *(['-DMOUNT_API_ONLY=1'] if name=='mount_api' else []),
                 *(['-DMOUNT_CONTEXT_ONLY=1'] if name=='mount_context' else []),
                 str(ROOT/'tests/workloads'/source),'-lrt','-o',str(destination)]
        with (work/(name+'-compile.log')).open('w') as log:
            subprocess.run(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
        programs[name]={'path':str(destination),'sha256':digest(destination),'command':command}
    image=harness.fixture(work,work/'init')
    commands=['mkdir /proc','mkdir /tmp','mkdir /dev/mqueue']
    for name in programs:
        if name!='init':commands += [f'write "{work/name}" /{name}',f'set_inode_field /{name} mode 0100755']
    if 'ptrace_tools' in programs:
        harness.run_logged([sys.executable,str(ROOT/'tools/build-strace.py'),'--arch',args.arch],work/'strace-build.log')
        tool=ROOT/'build/tools/strace-7.3'/args.arch/'src/strace'
        programs['strace']={'path':str(tool),'sha256':digest(tool),'upstream_version':'7.3'}
        commands += [f'write "{tool}" /strace','set_inode_field /strace mode 0100755']
    if 'vectors_v2' in programs:
        empty=work/'append-only';empty.write_bytes(b'')
        commands += [f'write "{empty}" /vectors-append-only',
                     'set_inode_field /vectors-append-only mode 0100600',
                     'set_inode_field /vectors-append-only flags 0x80020']
    selection=work/'api8-programs'
    selection.write_text(''.join('/'+name+'\n' for name in args.programs))
    commands += [f'write \"{selection}\" /api8-programs']
    if args.dynamic:
        headers=subprocess.check_output(['readelf','-lW',str(work/'init')],text=True)
        interpreter=re.findall(r'Requesting program interpreter: ([^\]]+)',headers)
        if len(interpreter)!=1 or not interpreter[0].startswith('/lib/'):
            raise RuntimeError('unexpected musl interpreter')
        commands += ['mkdir /lib',f'write "{musl/"lib/libc.so"}" {interpreter[0]}',
                     f'set_inode_field {interpreter[0]} mode 0100755',
                     f'write "{musl/"lib/libc.so"}" /lib/libc.so']
    supervisor=work/'supervisor'
    command=[profile.compiler,*profile.raw_flags,'-O2','-ffreestanding','-fno-builtin',
             '-fno-stack-protector','-nostdlib','-nostartfiles','-static','-no-pie',
             '-Wl,--build-id=none','-T',str(ROOT/'tests/common/user.ld'),
             str(ROOT/'tests/loongarch/root_linux_init.c'),str(ROOT/'tests/common/user_start.S'),'-o',str(supervisor)]
    command+=['-DROOT_DIRECT_FILESYSTEM=1']
    if args.arch=='loongarch':command+=['-Wl,-z,max-page-size=16384']
    harness.run_logged(command,work/'supervisor-compile.log')
    commands += [f'write "{supervisor}" /supervisor','set_inode_field /supervisor mode 0100755']
    batch=work/'payload.debugfs';batch.write_text('\n'.join(commands)+'\n')
    harness.run_logged(['debugfs','-w','-f',str(batch),str(image)],work/'payload.log')
    harness.run_logged(['e2fsck','-fn',str(image)],work/'fixture-fsck.log')
    return image,programs

def accepted(raw,arch,cpus,target,selected=None):
    programs=[name for name in (selected or LEGACY) if name!='icache' or arch=='riscv']
    if target=='boaros' and arch=='riscv' and ('mount_api' in programs or 'mount_context' in programs or 'system_control' in programs or 'sched_control' in programs or 'async_io' in programs or 'tty_async' in programs or 'uts_namespace' in programs or 'ipc_namespace' in programs or 'pid_namespace' in programs or 'nsfs_pid' in programs or any(p in programs for p in ('ptrace','ptrace_signal','ptrace_tools','process_memory','personality','process_control','file_flags','file_ioctl'))):
        free=re.search(r'task stacks released=0x[0-9a-f]+ min-free=0x([0-9a-f]+)',raw)
        if not free or int(free[1],16)<1024:return False
    ends=[line for line in raw.replace('\r','').splitlines() if line.startswith('API8 END ')]
    expected=[f'API8 END /{name} status=0' for name in programs]
    known={'nsfs_pid':['NSFS PID PASS scalar TID TGID reap and errno'],
           'pid_namespace_creation':['PIDNS DRAIN PASS external tracer owns member wait before namespace init completion','PIDNS CREATE PASS unshare stable caller, three-level set_tid, pidfd wait, dead child namespace'],
           'pid_namespace_misc':['PIDNS MISC PASS netlink inherited port and TSYNC failed TID'],'pid_namespace_reboot':['PIDNS REBOOT PASS invalid inputs and permission ordering'],'pid_namespace_clocks':['PIDNS CLOCK PASS process thread strict clocks and timer recipient'],'ioprio':['IOPRIO PASS'],'pid_namespace_priority':['PIDNS PRIORITY PASS process and inherited group'],'cgroup':['CGROUP END 5'],'pid_namespace_cgroup':['PIDNS CGROUP PASS current-reader inherited FD and migration'],'pid_namespace_groups':['PIDNS GROUP PASS inherited session tty and async owners'],'pid_namespace_ipc':['PIDNS IPC PASS sem msg shm retained identity exit'],'unix_control':['UNIX-CONTROL CASE credentials-real-ids-peer-snapshot-explicit-permissions PASS','UNIX-CONTROL END 23'],'pid_namespace_socket':['PIDNS SOCKET PASS peercred SCM_CREDENTIALS SCM_RIGHTS exited-owner'],'robust_exec':['ROBUST EXEC PASS nonleader old-TID cleanup'],'pi':['PI END 15'],'ipv4_abi':[*(f'IPV4 ABI PASS {name}' for name in ('scalar-options','device-options','multicast-interface','raw-boundaries','permissions')),'IPV4 ABI PASS'],'file_ioctl':['FD-IOCTL PASS'],'process_control':['PROCESS_CONTROL PASS'],'file_flags':['FILE_FLAGS PASS'],'ptrace_signal':['PTRACE SIGNAL suppression reset-mask blocked-replacement siginfo ignored continue-cancels-stop PASS'],'process_memory':['PROCESS_MEMORY PASS'],'personality':['PERSONALITY PASS'],'ptrace':['PTRACE traced-stop memory syscall-info exit PASS'],'ptrace_tools':['PTRACE_TOOLS PASS'],'uts_namespace':['UTS NAMESPACE PASS cases=8'],'ipc_namespace':['IPCNS PASS cases=3'],'pid_namespace':['PIDNS FD PASS initial-view stable-inode setns inherited-owner'],'async_io':['ASYNC PASS cases=4'],'tty_async':['PTY_RECORD sigio-vhangup-generation 1'],'sched_control':['SCHED-CONTROL END cases=6'],'system_control':['SYSTEM CONTROL PASS cases=5'],'mount_context':['FS-CONTEXT END 9'],'mount_api':['MOUNT-API END 5'],'propagation':['PROP END 9'],'atime':['MOUNT-ATTR END 4'],'memory_advice':['MADVISE END 5'],'fd_events':['FD-EVENTS PASS'],'flock':['FLOCK-PASS'],
           'splice':['SPLICE PASS cases=9'],'file_advice':['ADVICE RESULT cases=3 failures=0'],'copy_range':['COPY_RANGE PASS'],'sched_query':['SCHED-QUERY END cases=7'],
           'icache':[f'ICACHE PASS cpus={cpus}'],
           'vectors_v2':[*(f'VECTORS2 CASE {name} PASS' for name in (
               'positions-libc-native-width','per-call-append-sync','vectors-errors-prefix',
               'signal-targets-seals','caller-cpus-atomic-append','local-copy-fault-prefix-cow-string')),'VECTORS2 PASS cases=6']}
    markers=[f'API8 PASS programs={len(programs)}']
    for name in programs:markers+=known.get(name,[])
    lines=raw.replace('\r','').splitlines()
    success=(f'Linux {"RV" if arch=="riscv" else "LA"} root application passed' in raw
             if target=='linux' else PROFILES[arch].root_success(raw,0))
    mmsg=[line for line in lines if re.fullmatch(r'MMSG PASS cases=[1-9][0-9]*',line)]
    limited=('vectors_v2' not in programs or target!='boaros' or all(
        lines.count(f'VECTORS2 CAP flag={flag} result=-95')==1 for flag in (8,64,128,2147483648)))
    return limited and ends==expected and ('mmsg' not in programs or len(mmsg)==1) and all(lines.count(marker)==1 for marker in markers) and success and 'API8 FAIL' not in raw

def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--arch',choices=PROFILES,default='riscv')
    parser.add_argument('--program',action='append',choices=[name for name in SOURCES if name!='init'],
                        help='run only selected existing callers (repeatable)')
    parser.add_argument('--libc',choices=('musl','glibc'),default='musl')
    parser.add_argument('--dynamic',action='store_true')
    parser.add_argument('--kernel',type=Path)
    parser.add_argument('--cpus',type=int,choices=range(1,9),default=1)
    parser.add_argument('--memory',default='512M')
    parser.add_argument('--vfat',action='store_true',help='also check copy_file_range on a BoarOS VFAT disk')
    parser.add_argument('--only',choices=('linux','boaros'))
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if args.dynamic and args.libc!='musl':parser.error('--dynamic requires musl')
    args.programs=tuple(name for name in dict.fromkeys(args.program or LEGACY) if name!='icache' or args.arch=='riscv')
    profile=PROFILES[args.arch]
    work=(args.output or ROOT/'build/api8'/str(time.time_ns())).resolve()
    work.mkdir(parents=True,exist_ok=False)
    report={'arch':args.arch,'libc':args.libc,'dynamic':args.dynamic,'cpus':args.cpus,'runs':[], 'status':'preparing'}
    try:
        fixture,programs=build(args,work)
        report.update(programs=programs,fixture_sha256=digest(fixture))
        kernels=[]
        if args.only!='boaros':
            if args.arch=='riscv':
                linux,identity=harness.linux_build(ROOT/'tests/smp/linux-user.config')
                report['linux_identity']=identity
            else:linux=profile.linux_kernel()
            kernels.append(('linux',linux))
        if args.only!='linux':kernels.append(('boaros',args.kernel or ROOT/profile.kernel))
        qemu=os.environ.get('QEMU_RISCV64' if args.arch=='riscv' else 'QEMU_LOONGARCH64',profile.qemu)
        report['qemu']=subprocess.check_output([qemu,'--version'],text=True)
        for target,kernel in kernels:
            snapshot=work/(target+'-kernel');shutil.copyfile(kernel,snapshot)
            disk=work/(target+'.img');shutil.copyfile(fixture,disk)
            command=profile.boot(qemu,snapshot,args.memory)+['-drive',f'file={disk},if=none,format=raw,id=root',
                      '-device',profile.block('modern'),'-net','none']
            command[command.index('-smp')+1]=str(args.cpus)
            fat=None
            if args.vfat and target=='boaros':
                marker=work/'with-vfat';marker.write_text('VFAT copy consumer\n')
                batch=work/'vfat.debugfs';batch.write_text(f'write "{marker}" /with-vfat\n')
                harness.run_logged(['debugfs','-w','-f',str(batch),str(disk)],work/'vfat-install.log')
                fat=work/'fat.img'
                with fat.open('wb') as stream:stream.truncate(64*1024*1024)
                harness.run_logged(['mkfs.fat','-F','32','-s','1',str(fat)],work/'mkfs-fat.log')
                device=('virtio-blk-device,drive=fat,bus=virtio-mmio-bus.1' if args.arch=='riscv'
                        else 'virtio-blk-pci,drive=fat,addr=2,disable-legacy=on')
                command+=['-drive',f'file={fat},if=none,format=raw,id=fat','-device',device]
            if target=='linux':
                # setns installs the namespace root. Use a real root mount on
                # both architectures, rather than a LA-only chroot in initrd.
                command+=['-append','root=/dev/vda rw rootwait console=ttyS0 init=/supervisor loglevel=3 panic=-1']
            record={'target':target,'kernel_sha256':digest(snapshot),'command':command,'returncode':0}
            log=work/(target+'.log')
            try:harness.run_logged(command,log,180)
            except (RuntimeError,TimeoutError) as error:record.update(returncode=-1,error=str(error))
            raw=log.read_text(errors='replace')
            record['passed']=not record['returncode'] and accepted(raw,args.arch,args.cpus,target,args.programs)
            report['runs'].append(record)
            if not record['passed']:raise RuntimeError(f'{target} caller/exit/owner failure: {log}')
            harness.run_logged(['e2fsck','-fn',str(disk)],work/(target+'-fsck.log'))
            if fat:
                harness.run_logged(['fsck.fat','-n',str(fat)],work/'fat-fsck.log');fat.unlink()
            if any(digest(row['path'])!=row['sha256'] for row in programs.values()):
                raise RuntimeError('caller ELF changed during comparison')
            disk.unlink();print(target,'PASS',work,flush=True)
        fixture.unlink();report['status']='passed'
    except Exception as error:
        report.update(status='failed',error=str(error));raise
    finally:(work/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    return 0

if __name__=='__main__':sys.exit(main())
