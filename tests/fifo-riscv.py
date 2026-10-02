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
    readonly=work/'readonly.img'
    subprocess.run(['truncate','-s','32M',str(readonly)],check=True)
    subprocess.run(['mkfs.ext4','-q','-F','-b','4096',str(readonly)],check=True)
    script=work/'readonly.debugfs';script.write_text('cd /\nmknod fifo p\nset_inode_field fifo mode 010640\n')
    subprocess.run(['debugfs','-w','-f',str(script),str(readonly)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    variants=[]
    if args.only!='boaros':
        linux,_=harness.fixed_linux_image(None);variants.append(('linux',linux))
    if args.only!='linux':variants.append(('boaros',args.kernel))
    passed=True
    for name,kernel in variants:
        snapshot=work/(name+'-kernel');shutil.copyfile(kernel,snapshot)
        disk=work/(name+'.img');shutil.copyfile(image,disk)
        command=[os.environ.get('QEMU_RISCV64','qemu-system-riscv64'),'-machine','virt','-bios','default','-kernel',str(snapshot),'-m','512M','-smp','1','-nographic','-no-reboot','-drive',f'file={disk},if=none,format=raw,id=root','-device','virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
        command+=['-drive',f'file={readonly},if=none,format=raw,id=second,readonly=on','-device','virtio-blk-device,drive=second,bus=virtio-mmio-bus.1']
        if name=='linux':command+=['-append','root=/dev/vdb rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
        log=work/(name+'.log')
        try:harness.run_logged(command,log,90)
        except (RuntimeError,TimeoutError):pass
        raw=log.read_text(errors='replace')
        ok='FIFO PASS all' in raw
        if name=='boaros':ok=ok and 'exited status=0x0 ' in raw and 'heap-live=0x0; shutting down' in raw
        if ok:
            reboot=work/(name+'-reboot.log')
            try:harness.run_logged(command,reboot,90)
            except (RuntimeError,TimeoutError):pass
            restarted=reboot.read_text(errors='replace')
            ok='FIFO PASS reboot' in restarted and 'FIFO PASS all' in restarted
            if name=='boaros':ok=ok and 'exited status=0x0 ' in restarted and 'heap-live=0x0; shutting down' in restarted
            if not ok:raw=restarted
        print(name,'PASS' if ok else 'FAIL',work)
        if not ok:print('\n'.join(raw.splitlines()[-10:]))
        passed=passed and ok
    if passed:shutil.rmtree(work)
    return 0 if passed else 1
if __name__=='__main__':sys.exit(main())
