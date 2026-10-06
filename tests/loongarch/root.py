#!/usr/bin/env python3
"""Boot immutable static user programs from PCI ext4 and verify persistent data."""
import argparse
import gzip
import hashlib
from pathlib import Path
import re
import subprocess
import sys
from guest import run_guest
from reference import archive
ROOT=Path(__file__).resolve().parents[2]

def execute(command):
    subprocess.run(list(map(str,command)),check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)

def disk(directory,program,busybox=None,case='normal'):
    image=directory/(case+'.img')
    with image.open('wb') as output: output.truncate(64*1024*1024)
    execute(['mkfs.ext4','-q','-F','-b','4096','-O','^metadata_csum,^64bit,^orphan_file',image])
    def edit(text): execute(['debugfs','-w','-R',text,image])
    if case!='missing':
        executable=program
        if case=='wrong-arch':
            executable=directory/'wrong-init';data=bytearray(program.read_bytes());data[18:20]=(243).to_bytes(2,'little');executable.write_bytes(data)
        edit(f'write {executable} /init')
        edit('set_inode_field /init mode '+('0100644' if case=='nonexec' else '0100755'))
    data=directory/'original';data.write_bytes(bytes(i%251 for i in range(49169)))
    edit(f'write {data} /original')
    if busybox:
        edit(f'write {busybox} /busybox');edit('set_inode_field /busybox mode 0100755')
    if case=='corrupt':
        with image.open('r+b') as output: output.seek(1024);output.write(bytes(1024))
    return image

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu',default='build/qemu-la/qemu-system-loongarch64')
    parser.add_argument('--cc',default='loongarch64-unknown-linux-gnu-gcc');parser.add_argument('--kernel',default='kernel-la');parser.add_argument('--program',type=Path,default=Path('build/loongarch/root-probe'))
    parser.add_argument('--busybox',type=Path,default=Path('build/loongarch/busybox-source/busybox/busybox'))
    parser.add_argument('--linux',action='store_true');parser.add_argument('--smoke',action='store_true')
    parser.add_argument('--fault-program',type=Path);parser.add_argument('--oom-kernel');args=parser.parse_args()
    directory=ROOT/'build/loongarch/root-run';directory.mkdir(exist_ok=True)
    cases=['normal'] if args.linux or args.smoke else ['normal','readonly','missing','nonexec','wrong-arch','corrupt','legacy']
    initrd=None
    if args.linux:
        if subprocess.check_output(['git','-C','references/linux','rev-parse','HEAD'],text=True).strip()!='f4cdf7ca9a1fdcca413157df19753f388a5a224e': raise SystemExit('Linux revision mismatch')
        config=(ROOT/'build/linux-la/.config').read_text()
        for item in ('16KB_3LEVEL','EXT4_FS','VIRTIO_BLK','VIRTIO_PCI','SYSFS'):
            if f'CONFIG_{item}=y' not in config: raise SystemExit('Linux root baseline missing '+item)
        supervisor=directory/'linux-init'
        execute([args.cc,'-mabi=lp64s','-msoft-float','-mno-lsx','-mno-lasx','-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdlib','-nostartfiles','-static','-no-pie','-Wl,--build-id=none','-Wl,-z,max-page-size=16384','-T','tests/loongarch/user.ld','tests/loongarch/root_linux_init.c','tests/loongarch/user_start.S','-o',supervisor])
        initrd=directory/'initramfs.gz';initrd.write_bytes(archive([('dev',0o040755,b'',0,0),('dev/console',0o020600,b'',5,1),('init',0o100755,supervisor.read_bytes(),0,0),('TRAILER!!!',0,b'',0,0)]))
    expected={'missing':-2,'nonexec':-13,'wrong-arch':-8,'corrupt':-117,'legacy':-95}
    for memory in ('512M','1G'):
        for case in cases+(['userfault'] if args.fault_program and not args.linux else [])+(['oom'] if args.oom_kernel and not args.linux else []):
            image=disk(directory,args.fault_program if case=='userfault' else args.program,args.busybox if args.busybox.exists() else None,case)
            readonly_hash=hashlib.sha256(image.read_bytes()).hexdigest() if case=='readonly' else None
            kernel='build/linux-la/vmlinux' if args.linux else args.oom_kernel if case=='oom' else args.kernel
            command=[args.qemu,'-machine','virt','-cpu','la464','-smp','1','-m',memory,'-kernel',kernel,'-drive',f'file={image},format=raw,if=none,id=root'+(',readonly=on' if case=='readonly' else ''),'-device','virtio-blk-pci,drive=root,addr=1,disable-legacy=on','-net','none','-nographic','-no-reboot']
            if case=='legacy': command=[entry.replace(',disable-legacy=on','') for entry in command]
            if initrd: command+=['-initrd',str(initrd),'-append','console=ttyS0 rdinit=/init loglevel=3']
            try: code,text=run_guest(command,120)
            except subprocess.TimeoutExpired as error:
                (directory/f'{case}-{memory}.log').write_text(error.output);raise
            (directory/f'{case}-{memory}.log').write_text(text)
            passed=not code and 'fatal' not in text
            if args.linux: passed &= 'Linux LA root application passed' in text
            elif case in expected or case=='oom':
                error=expected.get(case,-12)&((1<<64)-1)
                passed &= f'LA root boot errno=0x{error:016x}' in text and 'LA root owners released' in text and 'LA PID 1' not in text
            elif case=='userfault': passed &= 'LA PID 1 exited reason=0x0000000000000004 status=0x000000000000000b' in text and 'LA root owners released' in text
            else: passed &= 'LA PID 1 exited reason=0x0000000000000001 status=0x0000000000000000' in text and 'LA root owners released' in text
            if not passed: sys.stdout.write(text);raise SystemExit(f'LA root failed {case} {memory}')
            if readonly_hash and hashlib.sha256(image.read_bytes()).hexdigest()!=readonly_hash: raise SystemExit('readonly ext4 image changed')
            if case=='normal' and not args.smoke:
                if 'LA musl root contracts passed' not in text: raise SystemExit('missing actual musl results')
                recovered=directory/'persisted';execute(['debugfs','-R',f'dump /persisted {recovered}',image])
                if recovered.read_bytes()!=bytes((i*7+3)%256 for i in range(32791)): raise SystemExit('persisted file differs after shutdown')
            print(f'{"Linux" if args.linux else "BoarOS"} LA root {case} {memory}: PASS')
    print('Root ELF SHA-256 '+hashlib.sha256(args.program.read_bytes()).hexdigest())
if __name__=='__main__': main()
