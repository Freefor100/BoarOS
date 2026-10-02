#!/usr/bin/env python3
"""Real FIFO and pipe contracts with the same ELF on fixed Linux and BoarOS."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests/diff-abi'))
import harness

def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--only',choices=('linux','boaros'))
    parser.add_argument('--kernel',type=Path,default=ROOT/'kernel-rv')
    args=parser.parse_args()
    work=Path(tempfile.mkdtemp(prefix='fifo-run.',dir=ROOT/'build'))
    program=work/'init'
    subprocess.run([str(ROOT/'build/riscv/musl-root/bin/musl-gcc'),'-fno-link-libatomic','-static','-O2','-Wall','-Wextra','-Werror',str(ROOT/'tests/workloads/fifo/contract.c'),'-o',str(program)],check=True)
    image=harness.fixture(work,program)
    variants=[]
    if args.only!='boaros':
        linux,_=harness.fixed_linux_image(None);variants.append(('linux',linux))
    if args.only!='linux':variants.append(('boaros',args.kernel))
    passed=True
    for name,kernel in variants:
        snapshot=work/(name+'-kernel');shutil.copyfile(kernel,snapshot)
        disk=work/(name+'.img');shutil.copyfile(image,disk)
        command=[os.environ.get('QEMU_RISCV64','qemu-system-riscv64'),'-machine','virt','-bios','default','-kernel',str(snapshot),'-m','512M','-smp','1','-nographic','-no-reboot','-drive',f'file={disk},if=none,format=raw,id=root','-device','virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
        if name=='linux':command+=['-append','root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
        log=work/(name+'.log')
        try:harness.run_logged(command,log,90)
        except (RuntimeError,TimeoutError):pass
        raw=log.read_text(errors='replace')
        ok='FIFO PASS all' in raw
        if name=='boaros':ok=ok and 'exited status=0x0 ' in raw and 'heap-live=0x0; shutting down' in raw
        print(name,'PASS' if ok else 'FAIL',work)
        if not ok:print('\n'.join(raw.splitlines()[-10:]))
        passed=passed and ok
    if passed:shutil.rmtree(work)
    return 0 if passed else 1
if __name__=='__main__':sys.exit(main())
