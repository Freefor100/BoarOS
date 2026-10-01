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
parser.add_argument('--expect-idle-failure',action='store_true',help='counterfactual build suppresses the idle IRQ return hook')
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='journal-group.',dir=root/'build') as temporary:
    image=Path(temporary)/'root.img'
    with image.open('wb') as stream:stream.truncate(64*1024*1024)
    subprocess.run(['mkfs.ext4','-q','-F','-b','4096',str(image)],check=True)
    result=subprocess.run([args.qemu,'-machine','virt','-bios','default','-kernel',args.kernel,
        '-m','512M','-smp','1','-nographic','-no-reboot','-global',
        'virtio-mmio.force-legacy='+('true' if args.transport=='legacy' else 'false'),
        '-drive',f'file={image},if=none,format=raw,id=root,cache={args.cache}',
        '-device','virtio-blk-device,drive=root,bus=virtio-mmio-bus.0,config-wce=off,request-merging=off'],
        stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=40)
    output=result.stdout.decode(errors='replace')
    if args.expect_idle_failure:
        if result.returncode or 'journal group failed: 0xd' not in output:
            print(output[-12000:]);raise SystemExit('idle counterfactual did not fail at completion-to-run boundary')
        print('PASS: timer-free device IRQ counterfactual fails without idle return scheduling')
        raise SystemExit(0)
    if result.returncode or 'BoarOS: journal group tests passed' not in output:
        print(output[-12000:]);raise SystemExit('journal group runtime failed')
    check=subprocess.run(['e2fsck','-fn',str(image)],capture_output=True,text=True)
    if check.returncode:print(check.stdout+check.stderr);raise SystemExit('journal group filesystem damaged')
    print(f'PASS: journal group runtime {args.transport}/{args.cache}')
