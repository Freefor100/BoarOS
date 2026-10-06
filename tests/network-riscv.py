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

def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--only',choices=('linux','boaros'))
    parser.add_argument('--workload',choices=('contract','content','timer','budget','interface','sendfile','admission'),default='contract')
    parser.add_argument('--platform-config',choices=('fixture','official'),default='fixture')
    parser.add_argument('--kernel',type=Path,default=ROOT/'kernel-rv')
    args=parser.parse_args()
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
