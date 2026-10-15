#!/usr/bin/env python3
"""Real worker, IRQ-backed I/O, finite sync targets and teardown."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser=argparse.ArgumentParser()
parser.add_argument('--kernel',required=True)
parser.add_argument('--qemu',default='qemu-system-riscv64')
parser.add_argument('--transport',choices=('modern','legacy'),default='modern')
parser.add_argument('--cache',choices=('writeback','writethrough'),default='writeback')
parser.add_argument('--nojournal',action='store_true')
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='journal-group.',dir=root/'build') as temporary:
    image=Path(temporary)/'root.img'
    with image.open('wb') as stream:stream.truncate(64*1024*1024)
    subprocess.run(['mkfs.ext4','-q','-F','-b','4096',*(['-O','^has_journal'] if args.nojournal else []),str(image)],check=True)
    for command in ('write /dev/null /noatime', 'set_inode_field /noatime flags 0x80080'):
        subprocess.run(['debugfs','-w','-R',command,str(image)],check=True,capture_output=True)

    if args.nojournal:
        fixture=Path(temporary)/'content'
        fixture.write_bytes(b'R')
        for entry in (f'write {fixture} /background-atime',
                      'set_inode_field /background-atime atime 0',
                      'set_inode_field /background-atime atime_extra 0'):
            subprocess.run(['debugfs','-w','-R',entry,str(image)],check=True,capture_output=True)

    command=[args.qemu,'-machine','virt','-bios','default','-kernel',args.kernel,
        '-m','512M','-smp','1','-nographic','-no-reboot','-global',
        'virtio-mmio.force-legacy='+('true' if args.transport=='legacy' else 'false'),
        '-drive',f'file={image},if=none,format=raw,id=root,cache={args.cache}',
        '-device','virtio-blk-device,drive=root,bus=virtio-mmio-bus.0,config-wce=off,request-merging=off']
    try:
        result=subprocess.run(command, stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=40)
    except subprocess.TimeoutExpired as error:
        print((error.stdout or b'').decode(errors='replace')[-12000:])
        raise SystemExit('journal group runtime timed out') from error
    output=result.stdout.decode(errors='replace')
    if result.returncode or 'BoarOS: journal group tests passed' not in output:
        print(output[-12000:]);raise SystemExit('journal group runtime failed')
    check=subprocess.run(['e2fsck','-fn',str(image)],capture_output=True,text=True)
    if check.returncode:print(check.stdout+check.stderr);raise SystemExit('journal group filesystem damaged')
    print(f'PASS: {"nojournal timestamps" if args.nojournal else "journal group"} runtime {args.transport}/{args.cache}')
