#!/usr/bin/env python3
"""Real U-mode network contracts against the fixed Linux reference."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests/diff-abi'))
import harness
from arch_profiles import PROFILES

def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()

def run_loongarch(args):
    import tempfile
    from arch_profiles import PROFILES
    profile=PROFILES['loongarch']
    sys.path.insert(0,str(ROOT/'tests/loongarch'))
    from guest import run_guest
    from root import disk,execute
    from reference import archive
    work=Path(tempfile.mkdtemp(prefix='network-run.',dir=ROOT/'build/loongarch'))
    program=work/'init'
    environment=os.environ.copy();environment['REALGCC']=str(ROOT/'build/loongarch/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc')
    compiler=ROOT/'build/loongarch/musl-root/bin/musl-gcc'
    subprocess.run([str(compiler),*profile.raw_flags,*(['-DPIPE_SMOKE=1'] if args.workload=='pipe' else ['-DEVENT_SMOKE=1'] if args.workload=='event' else []),'-static','-O2','-pthread','-Wall','-Wextra','-Werror',
        '-Wl,-z,max-page-size=16384',str(ROOT/('tests/workloads/signal/queued.c' if args.workload=='queued' else 'tests/workloads/signal/altstack.c' if args.workload=='altstack' else 'tests/workloads/event/caller.c' if args.workload=='event' else 'tests/workloads/pipe/caller.c' if args.workload=='pipe' else 'tests/workloads/epoll/delivery.c' if args.workload=='epoll' else f'tests/workloads/network/{args.workload}.c')),'-o',str(program)],env=environment,check=True)
    supervisor=work/'supervisor'
    execute([profile.compiler,*profile.raw_flags,'-O2','-DROOT_NETWORK_SETUP=1','-ffreestanding',
        '-fno-builtin','-fno-stack-protector','-nostdlib','-nostartfiles','-static','-no-pie',
        '-Wl,--build-id=none','-Wl,-z,max-page-size=16384','-T','tests/common/user.ld',
        'tests/loongarch/root_linux_init.c','tests/common/user_start.S','-o',supervisor])
    initrd=work/'initramfs.gz';initrd.write_bytes(archive([('dev',0o040755,b'',0,0),('dev/console',0o020600,b'',5,1),
        ('init',0o100755,supervisor.read_bytes(),0,0),('TRAILER!!!',0,b'',0,0)]))
    metadata={'arch':profile.name,'program_sha256':digest(program),'runs':{}}
    qemu=os.environ.get('QEMU_LOONGARCH64',profile.qemu)
    for platform in ('linux','boaros'):
        if args.only and args.only!=platform:continue
        for memory in args.memory or ('512M','1G'):
            case=work/f'{platform}-{memory}';case.mkdir()
            image=harness.fixture(case,program) if args.workload in ('pipe','event','altstack','queued') else disk(case,program)
            kernel=profile.linux_kernel() if platform=='linux' else args.kernel or ROOT/profile.kernel
            snapshot=case/'kernel';shutil.copyfile(kernel,snapshot)
            command=profile.boot(qemu,snapshot,memory)+['-drive',f'file={image},if=none,format=raw,id=root',
                '-device',profile.block('modern'),'-net','none']
            if args.platform_config=='official':
                command=command[:-2]+['-device','virtio-net-pci,netdev=net,addr=5,disable-legacy=on','-netdev','user,id=net','-rtc','base=utc']
            if platform=='linux':command+=['-initrd',str(initrd),'-append','console=ttyS0 rdinit=/init loglevel=3']
            try:code,raw=run_guest(command,120)
            except subprocess.TimeoutExpired as error:code=-1;raw=error.output
            (case/'console.log').write_text(raw)
            passed=not code and ('QUEUED END failures=0' if args.workload=='queued' else 'ALTSTACK END 6' if args.workload=='altstack' else 'EPOLL PASS delivery' if args.workload=='epoll' else 'PIPE END 7' if args.workload=='pipe' else 'EVENT END 8' if args.workload=='event' else f'NETWORK PASS {args.workload}') in raw and 'fatal' not in raw and (
                'Linux LA root application passed' in raw if platform=='linux' else profile.root_success(raw,0))
            if digest(program)!=metadata['program_sha256']:raise RuntimeError('ELF changed during comparison')
            row={'passed':passed,'qemu_returncode':code,'kernel_sha256':digest(snapshot),'command':command,
                'owner_baseline_checked':platform=='boaros'}
            metadata['runs'][f'{platform}-{memory}']=row
            (work/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
            print(platform,memory,'PASS' if passed else 'FAIL',work,flush=True)
            if not passed:print('\n'.join(raw.splitlines()[-20:]));return 1
            with (case/'fsck.log').open('w') as log:subprocess.run(['e2fsck','-fn',str(image)],stdout=log,stderr=subprocess.STDOUT,check=True)
            image.unlink();snapshot.unlink()
    return 0

def run_smp(args):
    sys.path.insert(0,str(ROOT/'tests/smp'))
    from user import boot_user,identity
    output=args.output or ROOT/'build/network'/('socket-smp-'+str(time.time_ns()))
    output.mkdir(parents=True,exist_ok=False)
    compiler=ROOT/'build/riscv/musl-root/bin/musl-gcc'
    program=output/'program'
    subprocess.run([str(compiler),*PROFILES['riscv'].musl_flags(compiler),
        *(['-Wl,--dynamic-linker=/lib/ld-musl-riscv64.so.1'] if args.dynamic else ['-static']),*(['-DEVENT_SMOKE=1'] if args.workload=='event' and not args.observe else []),'-O2','-pthread','-Wall','-Wextra','-Werror',
        str(ROOT/('tests/workloads/signal/queued.c' if args.workload=='queued' else 'tests/workloads/signal/altstack.c' if args.workload=='altstack' else 'tests/workloads/event/caller.c' if args.workload=='event' else 'tests/workloads/pipe/caller.c' if args.workload=='pipe' else 'tests/workloads/network/smp.c')),'-o',str(program)],check=True)
    variants=[]
    if args.only!='boaros':
        linux,linux_identity=harness.linux_build(ROOT/'tests/smp/linux-user.config')
        config=(linux.parents[3]/'.config').read_text()
        if 'CONFIG_NR_CPUS=4' not in config:raise RuntimeError('four-CPU reference required')
        variants.append(('linux',linux))
    if args.only!='linux':variants.append(('boaros',args.kernel or ROOT/'kernel-rv'))
    originals=variants;variants=[]
    for name,kernel in originals:
        snapshot=output/(name+'-kernel');shutil.copyfile(kernel,snapshot);variants.append((name,snapshot))
    report={'program':identity(program),'dynamic':args.dynamic,'runs':[],'status':'running'}
    (output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    expected=[f'SOCKET-SMP CASE {name} OK' for name in ('ipv4-tcp','ipv6-tcp','ipv4-udp','ipv6-udp','unix-stream','unix-dgram','blocking-close-reuse-cancel','fault-empty-trunc-timeout-sendfile','shared-readers-writers','remote-epoll-oneshot-nested')]+['SOCKET-SMP END 10']
    pipe=args.workload=='pipe';event=args.workload=='event';altstack=args.workload=='altstack'
    queued=args.workload=='queued'
    if queued and args.observe:raise ValueError('queued observation profile is not configured')
    if altstack:expected=[f'ALTSTACK CASE {name} OK' for name in ('abi','frames','bad-stacks','lifecycle','threads')]+['ALTSTACK CASE copy-races SKIP','ALTSTACK END 6']
    if event:expected=[f'EVENT CASE {name} OK' for name in ('counter-flags-limit','vectors-fault-alias','timer-clocks-periodic','timer-fault-commit','eventfd-direct','eventfd-epoll','timerfd-remote','lifecycle-close-reuse-signal-exec')]+['EVENT END 8']
    if pipe:expected=[f'PIPE CASE {name} OK' for name in ('stream-one','stream-four','ppoll-four','pselect-four','epoll-four','close-reuse-eof-signal','vectors-fault-fifo-sendfile-mixed-poll','epoll-modes-fault-rearm')]+['PIPE END 8']
    try:
        for cpus in args.cpus or [4]:
            for memory in args.memory or ['512M']:
                if queued:expected=[f'QUEUED THREADS mode={mode} cpus={cpus} OK' for mode in range(4)]+['QUEUED END failures=0']
                folder=output/f'{cpus}cpu-{memory}';folder.mkdir()
                fixture=harness.fixture(folder,program)
                if args.dynamic:
                    loader=ROOT/'build/riscv/musl-root/lib/libc.so'
                    batch=folder/'loader.debugfs';batch.write_text(f'mkdir /lib\nwrite "{loader}" /lib/ld-musl-riscv64.so.1\nset_inode_field /lib/ld-musl-riscv64.so.1 mode 0100755\n')
                    harness.run_logged(['debugfs','-w','-f',str(batch),str(fixture)],folder/'loader.log')
                for target,kernel in variants:
                    disk=folder/(target+'.img');shutil.copyfile(fixture,disk)
                    if target=='boaros' and args.observe:
                        marker=folder/'socket-observer';marker.write_text('private correctness observations\n')
                        batch=folder/'observer.debugfs';batch.write_text(f'write "{marker.resolve()}" /'+('altstack-observer' if altstack else 'event-observer' if event else 'pipe-observer' if pipe else 'socket-observer')+'\n')
                        harness.run_logged(['debugfs','-w','-f',str(batch),str(disk)],folder/'observer-install.log')
                    raw,record,_=boot_user('qemu-system-riscv64',args.firmware,str(kernel.resolve()),disk,memory,cpus,folder,target=='linux')
                    lines=[line[line.index('SOCKET-SMP '):] for line in raw.replace('\r','').splitlines() if 'SOCKET-SMP ' in line]
                    if queued:lines=[line for line in raw.replace('\r','').splitlines() if line.startswith('QUEUED ')]
                    if pipe:lines=[line for line in raw.replace('\r','').splitlines() if line.startswith('PIPE CASE ') or line.startswith('PIPE END ')]
                    if event:lines=[line for line in raw.replace('\r','').splitlines() if line.startswith('EVENT CASE ') or line.startswith('EVENT END ')]
                    if altstack:
                        lines=[line for line in raw.replace('\r','').splitlines() if line.startswith('ALTSTACK CASE ') or line.startswith('ALTSTACK END ')]
                        expected[-2]='ALTSTACK CASE copy-races '+('OK' if target=='boaros' and args.observe and cpus>1 else 'SKIP')
                    passed=lines==expected and (target=='linux' or PROFILES['riscv'].root_success(raw,0))
                    observations=None
                    if target=='boaros' and args.observe and not pipe and not event and not altstack:
                        matches=re.findall(r'SOCKET caller cpu=(0x[0-9a-f]+) operations=(0x[0-9a-f]+) copy-bytes=(0x[0-9a-f]+) pins=(0x[0-9a-f]+) waits=(0x[0-9a-f]+)',raw)
                        observations=[[int(value,16) for value in row] for row in matches]
                        active=[row for row in observations if row[0]<cpus]
                        holds=re.findall(r'SOCKET controlled rx=(0x[0-9a-f]+) tx-pin=(0x[0-9a-f]+) callback=(0x[0-9a-f]+)',raw)
                        controlled=[line for line in raw.replace('\r','').splitlines() if line.startswith('SOCKET-CONTROL ')]
                        passed=passed and len(active)==cpus and {row[0] for row in active}==set(range(cpus)) and all(all(row[1:]) for row in active)
                        passed=passed and len(holds)==1 and all(int(value,16)==1 for value in holds[0]) and controlled==[
                            'SOCKET-CONTROL reservation-append-last-close OK','SOCKET-CONTROL tx-pin-remote-mprotect OK','SOCKET-CONTROL callback-borrow-del-close OK']
                        passed=passed and bool(re.search(r'SOCKET CPU0 protocol-calls=0x[1-9a-f][0-9a-f]*',raw))
                    if target=='boaros' and args.observe and pipe:
                        observations=[[int(v,16) for v in row] for row in re.findall(r'PIPE caller cpu=(0x[0-9a-f]+) reads=(0x[0-9a-f]+) writes=(0x[0-9a-f]+) queries=(0x[0-9a-f]+) epoll-queries=(0x[0-9a-f]+) event-bytes=(0x[0-9a-f]+)',raw)]
                        active=[row for row in observations if row[0]<cpus]
                        passed=passed and len(active)==cpus and {row[0] for row in active}==set(range(cpus)) and all(all(row[1:]) for row in active)
                    if target=='boaros' and args.observe and event:
                        observations=[[int(v,16) for v in row] for row in re.findall(r'EVENT caller cpu=(0x[0-9a-f]+) io=(0x[0-9a-f]+) completed-bytes=(0x[0-9a-f]+) pins=(0x[0-9a-f]+) timers=(0x[0-9a-f]+)',raw)]
                        active=[row for row in observations if row[0]<cpus]
                        passed=passed and len(active)==cpus and {row[0] for row in active}==set(range(cpus)) and all(all(row[1:]) for row in active) and bool(re.search(r'EVENT CPU0 expiry=0x[1-9a-f][0-9a-f]*',raw))
                    if target=='boaros' and args.observe and altstack:
                        observations=[[int(v,16) for v in row] for row in re.findall(r'ALTSTACK caller cpu=(0x[0-9a-f]+) frames=(0x[0-9a-f]+) returns=(0x[0-9a-f]+)',raw)]
                        active=[row for row in observations if row[0]<cpus]
                        passed=passed and len(active)==cpus and {row[0] for row in active}==set(range(cpus)) and all(all(row[1:]) for row in active) and all(not any(row[1:]) for row in observations if row[0]>=cpus)
                    row=dict(target=target,cpus=cpus,memory=memory,passed=passed,observations=observations,**record);report['runs'].append(row)
                    print(target,cpus,memory,'PASS' if passed else 'FAIL',output,flush=True)
                    if not passed:raise RuntimeError('socket SMP stream or owner cleanup failed: '+str(folder))
                    with (folder/(target+'-fsck.log')).open('w') as log:subprocess.run(['e2fsck','-fn',str(disk)],stdout=log,stderr=subprocess.STDOUT,check=True)
                    disk.unlink()
                fixture.unlink()
        report['status']='passed'
    except Exception as error:
        report.update(status='failed',error=str(error));raise
    finally:(output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    return 0

def main(default_arch="riscv"):
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--only',choices=('linux','boaros'))
    parser.add_argument('--workload',choices=('contract','content','timer','budget','interface','sendfile','admission','unix_sender','smp','epoll','pipe','event','altstack','queued'),default='contract')
    parser.add_argument('--platform-config',choices=('fixture','official'),default='fixture')
    parser.add_argument('--arch',choices=('riscv','loongarch'),default=default_arch)
    parser.add_argument('--memory',action='append',choices=('512M','1G'))
    parser.add_argument('--kernel',type=Path)
    parser.add_argument('--cpus',action='append',type=int,choices=(1,2,4))
    parser.add_argument('--dynamic',action='store_true')
    parser.add_argument('--observe',action='store_true')
    parser.add_argument('--output',type=Path)
    parser.add_argument('--firmware',default='default')
    args=parser.parse_args()
    if args.workload in ('smp','pipe','event','altstack','queued') and args.arch=='riscv':return run_smp(args)
    if args.arch=='riscv' and args.platform_config=='official' and args.memory and args.memory!=['1G']:
        parser.error('official profile requires 1G')
    if args.arch=='loongarch':return run_loongarch(args)
    args.kernel=args.kernel or ROOT/'kernel-rv'
    work=ROOT/'build/network'/('contract-'+str(time.time_ns()))
    work.mkdir(parents=True)
    program=work/'init'
    compiler=ROOT/'build/riscv/musl-root/bin/musl-gcc'
    subprocess.run([str(compiler),*PROFILES['riscv'].musl_flags(compiler),'-static','-O2','-pthread','-Wall','-Wextra','-Werror',
                    str(ROOT/('tests/workloads/signal/queued.c' if args.workload=='queued' else 'tests/workloads/signal/altstack.c' if args.workload=='altstack' else 'tests/workloads/event/caller.c' if args.workload=='event' else 'tests/workloads/pipe/caller.c' if args.workload=='pipe' else 'tests/workloads/epoll/delivery.c' if args.workload=='epoll' else f'tests/workloads/network/{args.workload}.c')),'-o',str(program)],check=True)
    image=harness.fixture(work,program)
    metadata={'program_sha256':digest(program),'fixture_sha256':digest(image),'runs':{}}
    variants=[]
    if args.only!='boaros':
        linux,identity=harness.fixed_linux_image(None)
        metadata['linux']=identity;variants.append(('linux',linux))
    if args.only!='linux':variants.append(('boaros',args.kernel))
    qemu=os.environ.get('QEMU_RISCV64','qemu-system-riscv64')
    metadata['qemu']=subprocess.check_output([qemu,'--version'],text=True)
    for name,kernel in variants:
        snapshot=work/(name+'-kernel');shutil.copyfile(kernel,snapshot)
        memories=args.memory or (('1G',) if args.platform_config=='official' else ('512M',))
        for memory in memories:
            label=name if len(memories)==1 else name+'-'+memory
            disk=work/(label+'.img');shutil.copyfile(image,disk)
            command=[qemu,'-machine','virt','-bios','default','-kernel',str(snapshot),'-m',memory,'-smp','1',
                     '-nographic','-no-reboot','-drive',f'file={disk},if=none,format=raw,id=root',
                     '-device','virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
            if args.platform_config=='official':
                command+=['-device','virtio-net-device,netdev=net','-netdev','user,id=net','-rtc','base=utc']
            if name=='linux':command+=['-append','root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
            log=work/(label+'.log')
            try:harness.run_logged(command,log,60)
            except (RuntimeError,TimeoutError):pass
            raw=log.read_text(errors='replace')
            passed=('QUEUED END failures=0' if args.workload=='queued' else 'ALTSTACK END 6' if args.workload=='altstack' else 'EPOLL PASS delivery' if args.workload=='epoll' else 'PIPE END 8' if args.workload=='pipe' else f'NETWORK PASS {args.workload}') in raw
            if name=='boaros':passed=passed and 'heap-live=0x0; shutting down' in raw and 'exited status=0x0 ' in raw
            metadata['runs'][label]={'passed':passed,'memory':memory,'kernel_sha256':digest(snapshot),'command':command}
            print(name,memory,'PASS' if passed else 'FAIL',work)
            if not passed:print('\n'.join(raw.splitlines()[-8:]))
    (work/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
    if not all(row['passed'] for row in metadata['runs'].values()):return 1
    for path in work.glob('*.img'):path.unlink()
    return 0

if __name__=='__main__':sys.exit(main())
