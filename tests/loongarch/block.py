#!/usr/bin/env python3
"""Real PCI block data/flush/IRQ acceptance with two independent RAM layouts."""
import argparse
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time


def run_block(command, image, directory):
    address=Path(directory).resolve()/'nbd.sock'
    server=subprocess.Popen(['build/host/nbd-fault',str(image),str(address),'--control-stdin'],
                            stdin=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
    guest=None
    output=bytearray();backend=bytearray()
    held=None;ready=False;released=False
    try:
        deadline=time.monotonic()+60
        while not address.exists():
            if server.poll() is not None or time.monotonic()>deadline:
                raise RuntimeError('PCI NBD backend did not start')
            time.sleep(0.01)
        command+=['-drive',f'file=nbd+unix:///?socket={address},format=raw,if=none,id=d0,cache=writeback',
                  '-device','virtio-blk-pci,drive=d0,disable-legacy=on,addr=1.0']
        guest=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT,bufsize=0)
        with selectors.DefaultSelector() as selector:
            selector.register(guest.stdout,selectors.EVENT_READ,'guest')
            selector.register(server.stderr,selectors.EVENT_READ,'server')
            pending={'guest':bytearray(),'server':bytearray()}
            while selector.get_map():
                if time.monotonic()>deadline:raise RuntimeError('PCI IRQ handshake timed out')
                for key,_ in selector.select(0.1):
                    chunk=os.read(key.fd,4096)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    label=key.data
                    (output if label=='guest' else backend).extend(chunk)
                    pending[label].extend(chunk)
                    while b'\n' in pending[label]:
                        line,_,tail=pending[label].partition(b'\n');pending[label]=bytearray(tail)
                        line=bytes(line).rstrip(b'\r')
                        if label=='guest' and line==b'LA PCI IRQ hold':
                            server.stdin.write(b'hold\n')
                        elif label=='server' and line==b'control=hold':
                            guest.stdin.write(b'g')
                        elif label=='guest' and line==b'LA PCI IRQ release':
                            ready=True
                        elif label=='server' and line.startswith(b'held='):
                            fields=dict(part.split(b'=',1) for part in line.split())
                            if held is not None or (int(fields[b'command']),int(fields[b'offset']),int(fields[b'size']))!=(0,16384,4096):
                                raise RuntimeError('PCI SG request was not one held 4 KiB read')
                            held=int(fields[b'held'])
                        if ready and held is not None and not released:
                            server.stdin.write(b'drain\n');released=True
                if guest.poll() is not None:
                    break
        code=guest.wait(timeout=5)
        if not released:raise RuntimeError('PCI IRQ test did not release an owned response')
        return code,output.decode(errors='replace')
    except Exception:
        if backend:sys.stderr.write(backend.decode(errors='replace'))
        raise
    finally:
        for process in (guest,server):
            if process is not None:
                if process.poll() is None:process.kill()
                process.wait()
        if output:print(output.decode(errors='replace'),end='')

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
        for index,image in enumerate(images[1:],1):
            command+=['-drive',f'file={image},format=raw,if=none,id=d{index},readonly={"on" if index else "off"}',
                      '-device',f'virtio-blk-pci,drive=d{index},disable-legacy=on,addr={1 if index==0 else 5}.0']
        code,text=run_block(command,images[0],directory)
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
