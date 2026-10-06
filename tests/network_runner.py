#!/usr/bin/env python3
"""Real U-mode network contracts against the fixed Linux reference."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests/diff-abi'))
import harness

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
    subprocess.run([str(compiler),*profile.raw_flags,'-static','-O2','-Wall','-Wextra','-Werror',
        '-Wl,-z,max-page-size=16384',str(ROOT/f'tests/workloads/network/{args.workload}.c'),'-o',str(program)],env=environment,check=True)
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
            image=disk(case,program)
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
            passed=not code and f'NETWORK PASS {args.workload}' in raw and 'fatal' not in raw and (
                'Linux LA root application passed' in raw if platform=='linux' else profile.root_success(raw,0))
            if digest(program)!=metadata['program_sha256']:raise RuntimeError('ELF changed during comparison')
            row={'passed':passed,'qemu_returncode':code,'kernel_sha256':digest(snapshot),'command':command,
                'owner_baseline_checked':platform=='boaros'}
            metadata['runs'][f'{platform}-{memory}']=row
            (work/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
            print(platform,memory,'PASS' if passed else 'FAIL',work,flush=True)
            if not passed:print('\n'.join(raw.splitlines()[-20:]));return 1
            image.unlink();snapshot.unlink()
    return 0

def main(default_arch="riscv"):
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--only',choices=('linux','boaros'))
    parser.add_argument('--workload',choices=('contract','content','timer','budget','interface','sendfile','admission'),default='contract')
    parser.add_argument('--platform-config',choices=('fixture','official'),default='fixture')
    parser.add_argument('--arch',choices=('riscv','loongarch'),default=default_arch)
    parser.add_argument('--memory',action='append',choices=('512M','1G'))
    parser.add_argument('--kernel',type=Path)
    args=parser.parse_args()
    if args.arch=='loongarch':return run_loongarch(args)
    args.kernel=args.kernel or ROOT/'kernel-rv'
    if args.workload=='budget' and args.only!='boaros':
        parser.error('UNIX receiver-budget contract requires --only boaros')
    work=ROOT/'build/network'/('contract-'+str(time.time_ns()))
    work.mkdir(parents=True)
    program=work/'init'
    subprocess.run([str(ROOT/'build/riscv/musl-root/bin/musl-gcc'),'-fno-link-libatomic','-static','-O2','-Wall','-Wextra','-Werror',
                    str(ROOT/f'tests/workloads/network/{args.workload}.c'),'-o',str(program)],check=True)
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
        disk=work/(name+'.img');shutil.copyfile(image,disk)
        command=[qemu,'-machine','virt','-bios','default','-kernel',str(snapshot),'-m','512M','-smp','1',
                 '-nographic','-no-reboot','-drive',f'file={disk},if=none,format=raw,id=root',
                 '-device','virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
        if args.platform_config=='official':
            command[command.index('-m')+1]='1G'
            command+=['-device','virtio-net-device,netdev=net','-netdev','user,id=net','-rtc','base=utc']
        if name=='linux':command+=['-append','root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
        log=work/(name+'.log')
        try:harness.run_logged(command,log,60)
        except (RuntimeError,TimeoutError):pass
        raw=log.read_text(errors='replace')
        passed=f'NETWORK PASS {args.workload}' in raw
        if name=='boaros':passed=passed and 'heap-live=0x0; shutting down' in raw and 'exited status=0x0 ' in raw
        metadata['runs'][name]={'passed':passed,'kernel_sha256':digest(snapshot),'command':command}
        print(name, 'PASS' if passed else 'FAIL',work)
        if not passed:print('\n'.join(raw.splitlines()[-8:]))
    (work/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
    if not all(row['passed'] for row in metadata['runs'].values()):return 1
    for path in work.glob('*.img'):path.unlink()
    return 0

if __name__=='__main__':sys.exit(main())
