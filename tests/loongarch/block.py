#!/usr/bin/env python3
"""Real PCI block data/flush/IRQ acceptance with two independent RAM layouts."""
import argparse
from pathlib import Path
import tempfile
from guest import run_guest

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernel',default='build/loongarch/kernel-block-la')
parser.add_argument('--qemu',default='build/qemu-la-rtc/qemu-system-loongarch64')
args=parser.parse_args()
for memory in ('512M','1G'):
    with tempfile.TemporaryDirectory(prefix='pci-block-',dir='build/loongarch') as directory:
        images=[]
        for name in ('rw','ro'):
            image=Path(directory)/f'{name}.raw'
            with image.open('wb') as stream:
                stream.truncate(32*1024*1024)
                stream.seek(512);stream.write(b'BoarOS PCI original sector')
                stream.seek(16384);stream.write(bytes(i%251 for i in range(4096)))
            images.append(image)
        command=[args.qemu,'-machine','virt','-cpu','la464','-global','ls7a_rtc.toy-enabled=on','-smp','1','-m',memory,
                 '-kernel',args.kernel,'-nographic','-no-reboot','-nodefaults','-serial','stdio']
        for index,image in enumerate(images):
            command+=['-drive',f'file={image},format=raw,if=none,id=d{index},readonly={"on" if index else "off"}',
                      '-device',f'virtio-blk-pci,drive=d{index},disable-legacy=on,addr={1 if index==0 else 5}.0']
        code,text=run_guest(command,60)
        print(text,end='')
        if code or 'LA PCI block contracts passed' not in text or 'fatal' in text:
            raise SystemExit(f'LA PCI block failed ({memory})')
        with images[0].open('rb') as stream:
            stream.seek(32768);observed=stream.read(512)
        if observed!=bytes((i*13+7)%256 for i in range(512)):
            raise SystemExit('PCI block FLUSH persistence mismatch')
        with images[1].open('rb') as stream:
            stream.seek(32768)
            if stream.read(512)!=bytes(512):raise SystemExit('PCI readonly disk changed')
print('LA real PCI block/IRQ/persistence passed in 512M and 1G')
