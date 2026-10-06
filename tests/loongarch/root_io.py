#!/usr/bin/env python3
"""A real PCI/NBD write failure must preserve the failed mount owner and stop."""
import argparse
import os
from pathlib import Path
import selectors
import signal
import subprocess
import time
from root import disk
ROOT=Path(__file__).resolve().parents[2]

def main(args):
    directory=ROOT/'build/loongarch/root-io-run';directory.mkdir(exist_ok=True)
    for memory in ('512M','1G'):
        image=disk(directory,ROOT/'build/loongarch/root-probe',ROOT/'build/loongarch/busybox-source/busybox/busybox')
        socket=directory/'nbd.sock';socket.unlink(missing_ok=True)
        server=subprocess.Popen([ROOT/'build/host/nbd-fault',image,socket,'--arm-on-signal','--fail-write=1'],stderr=subprocess.PIPE)
        guest=None
        try:
            deadline=time.monotonic()+10
            while not socket.exists():
                if server.poll() is not None or time.monotonic()>deadline: raise RuntimeError('NBD startup failed')
                time.sleep(.01)
            command=[args.qemu,'-machine','virt','-cpu','la464','-smp','1','-m',memory,'-kernel','kernel-la',
                     '-drive',f'file=nbd+unix:///?socket={socket},if=none,format=raw,id=root',
                     '-device','virtio-blk-pci,drive=root,addr=1,disable-legacy=on',
                     '-net','none','-nographic','-no-reboot']
            guest=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
            selector=selectors.DefaultSelector();selector.register(guest.stdout,selectors.EVENT_READ,'guest');selector.register(server.stderr,selectors.EVENT_READ,'server')
            outputs={'guest':bytearray(),'server':bytearray()};armed=False;deadline=time.monotonic()+args.timeout
            while guest.poll() is None and time.monotonic()<deadline:
                for key,_ in selector.select(.1):
                    data=os.read(key.fileobj.fileno(),65536)
                    if not data: selector.unregister(key.fileobj);continue
                    outputs[key.data].extend(data)
                if not armed and b'LA root storage ready' in outputs['guest']:
                    server.send_signal(signal.SIGUSR1);armed=True
            code=guest.poll()
            if code is None: guest.kill();guest.wait()
            outputs['guest'].extend(guest.stdout.read())
            text=outputs['guest'].decode(errors='replace');(directory/f'guest-{memory}.log').write_text(text)
            (directory/f'server-{memory}.log').write_bytes(outputs['server'])
            if code!=0 or not armed or b'armed=1' not in outputs['server'] or b'type=WRITE result=5' not in outputs['server'] or 'fatal' in text or 'errno=5' not in text or \
                'LA root cleanup retained mount=0x0000000000000001' not in text or 'LA root owners released' in text or text.count('LA root cleanup errno=')!=3:
                raise SystemExit(f'LA PCI I/O ownership failed {memory}, exit={code}; see {directory}')
            print(f'LA PCI real write error {memory}: errno propagated, same failed mount/cache/device owners retained; bounded cleanup PASS')
        finally:
            if guest and guest.poll() is None: guest.kill();guest.wait()
            if server.poll() is None: server.terminate()
            server.wait(timeout=5)
            if guest: guest.stdout.close()
            server.stderr.close();socket.unlink(missing_ok=True)
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--qemu',default='build/qemu-la/qemu-system-loongarch64');parser.add_argument('--timeout',type=int,default=60);main(parser.parse_args())
