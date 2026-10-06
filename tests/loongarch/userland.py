#!/usr/bin/env python3
"""Run one LA ELF and supplied libraries unchanged on both root baselines."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
from guest import run_guest
from reference import archive,REVISION
from root import disk,execute,ROOT

def add_file(image,guest,source,mode=0o100755):
    from pathlib import PurePosixPath
    target=PurePosixPath(guest)
    if not target.is_absolute() or '..' in target.parts or len(target.parts)<2 or not source.is_file():
        raise SystemExit('invalid guest file mapping: '+guest)
    for parent in reversed(target.parents):
        if str(parent)!='/':execute(['debugfs','-w','-R',f'mkdir {parent}',image])
    execute(['debugfs','-w','-R',f'write {source} {target}',image])
    execute(['debugfs','-w','-R',f'set_inode_field {target} mode 0{mode:o}',image])

def main(args):
    directory=Path(tempfile.mkdtemp(prefix='userland-run.',dir=ROOT/'build/loongarch'))
    if subprocess.check_output(['git','-C','references/linux','rev-parse','HEAD'],text=True).strip()!=REVISION:
        raise SystemExit('Linux LA revision changed')
    configuration=(ROOT/'build/linux-la/.config').read_text()
    if 'CONFIG_16KB_3LEVEL=y' not in configuration: raise SystemExit('Linux LA page configuration changed')
    supervisor=directory/'linux-init'
    execute([args.cc,'-mabi=lp64s','-msoft-float','-mno-lsx','-mno-lasx','-O2',f'-DEXPECTED_EXIT_STATUS={args.exit_status}','-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdlib','-nostartfiles','-static','-no-pie','-Wl,--build-id=none','-Wl,-z,max-page-size=16384','-T','tests/loongarch/user.ld','tests/loongarch/root_linux_init.c','tests/loongarch/user_start.S','-o',supervisor])
    initrd=directory/'initramfs.gz';initrd.write_bytes(archive([('dev',0o040755,b'',0,0),('dev/console',0o020600,b'',5,1),('init',0o100755,supervisor.read_bytes(),0,0),('TRAILER!!!',0,b'',0,0)]))
    digest=hashlib.sha256(args.program.read_bytes()).hexdigest()
    files=[]
    for mapping in args.file:
        guest,separator,host=mapping.partition('=')
        if not separator:raise SystemExit('--file requires /guest/path=host/path')
        source=Path(host);files.append((guest,source,hashlib.sha256(source.read_bytes()).hexdigest()))
    for platform in args.platform or ('Linux','BoarOS'):
        for memory in ('512M','1G'):
            image=disk(directory,args.program,ROOT/'build/loongarch/busybox-source/busybox/busybox')
            for guest,source,_ in files:add_file(image,guest,source)
            command=[args.qemu,'-machine','virt','-cpu',args.cpu,'-smp','1','-m',memory,'-kernel','build/linux-la/vmlinux' if platform=='Linux' else args.kernel,
                     '-drive',f'file={image},format=raw,if=none,id=root','-device','virtio-blk-pci,drive=root,addr=1,disable-legacy=on','-net','none','-nographic','-no-reboot']
            if platform=='Linux': command+=['-initrd',str(initrd),'-append','console=ttyS0 rdinit=/init loglevel=3']
            try: code,text=run_guest(command,args.timeout)
            except subprocess.TimeoutExpired as error:
                (directory/f'{platform}-{memory}.log').write_text(error.output);raise
            (directory/f'{platform}-{memory}.log').write_text(text)
            good=code==0 and all(marker in text for marker in args.marker) and 'fatal' not in text
            if platform=='Linux': good &= 'Linux LA root application passed' in text
            else: good &= f'LA PID 1 exited reason=0x0000000000000001 status=0x{args.exit_status:016x}' in text and 'LA root owners released' in text
            if not good: sys.stdout.write(text);raise SystemExit(f'LA userland failed {platform}/{memory}; {directory}')
            if hashlib.sha256(args.program.read_bytes()).hexdigest()!=digest: raise SystemExit('ELF changed during differential run')
            if any(hashlib.sha256(source.read_bytes()).hexdigest()!=value for _,source,value in files):raise SystemExit('library changed during differential run')
            print(f'{platform}/{memory}: actual ELF, markers, exit and root lifecycle PASS')
    print(f'LA ELF SHA-256 {digest}; platforms {args.platform or ["Linux","BoarOS"]}; logs {directory}')
    return directory
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--program',type=Path,required=True)
    parser.add_argument('--marker',action='append',required=True);parser.add_argument('--timeout',type=int,default=120)
    parser.add_argument('--platform',choices=['Linux','BoarOS'],action='append',help='default both; injected-failure fixtures select BoarOS explicitly')
    parser.add_argument('--file',action='append',default=[],help='/guest/path=host/path; copy original loader/library inputs')
    parser.add_argument('--exit-status',type=int,choices=range(256),default=0,metavar='0..255')
    parser.add_argument('--cpu',default='la464')
    parser.add_argument('--cc',default='loongarch64-unknown-linux-gnu-gcc');parser.add_argument('--qemu',default='build/qemu-la/qemu-system-loongarch64');parser.add_argument('--kernel',default='kernel-la');main(parser.parse_args())
