#!/usr/bin/env python3
"""VirtIO RNG real transport/lifecycle; delayed EGD is not an entropy-quality test."""
import argparse
from pathlib import Path
import selectors
import shutil
import socket
import subprocess
import tempfile
import time
ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--kernel', default=str(ROOT/'kernel-rv'))
p.add_argument('--program', default=str(ROOT/'build/riscv/tests/user/rng-rv'))
p.add_argument('--qemu', default='qemu-system-riscv64')
p.add_argument('--transport', choices=['all','legacy','modern'], default='all')
a = p.parse_args()

def run(transport, mode):
    work=Path(tempfile.mkdtemp(prefix='rng.', dir=ROOT/'build'))
    guest=connection=server=None
    good=False
    try:
        shutil.copyfile(a.kernel,work/'kernel')
        disk=work/'root.img'
        with disk.open('wb') as f: f.truncate(32*1024*1024)
        subprocess.run(['mkfs.ext4','-q','-F',str(disk)],check=True)
        (work/'mode').write_text(mode)
        for command in [f'write {a.program} /init','set_inode_field /init mode 0100755',f'write {work}/mode /mode']:
            subprocess.run(['debugfs','-w','-R',command,str(disk)],capture_output=True,check=True)
        command=[a.qemu,'-machine','virt','-bios','default','-kernel',str(work/'kernel'),
                 '-m','512M','-smp','1','-nographic','-no-reboot',
                 '-global','virtio-mmio.force-legacy='+('true' if transport=='legacy' else 'false'),
                 '-drive',f'file={disk},if=none,format=raw,id=root,readonly=on',
                 '-device','virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
        if mode=='n': command+=['-object','rng-random,id=entropy,filename=/dev/urandom']
        elif mode in ('d','s'):
            server=socket.socket(socket.AF_UNIX);server.bind(str(work/'egd.sock'));server.listen(1)
            command+=['-chardev',f'socket,id=egd,path={work}/egd.sock',
                      '-object','rng-egd,id=entropy,chardev=egd']
        if mode!='a': command+=['-device','virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.5']
        guest=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,bufsize=0)
        select=selectors.DefaultSelector();select.register(guest.stdout,selectors.EVENT_READ,'guest')
        if server:
            server.settimeout(10);connection,_=server.accept();connection.setblocking(False)
            select.register(connection,selectors.EVENT_READ,'egd')
        log=bytearray();requests=bytearray();released=False;total=0;end=time.monotonic()+30
        while select.get_map() and time.monotonic()<end:
            for key,_ in select.select(.1):
                data=key.fileobj.recv(4096) if key.data=='egd' else key.fileobj.read(4096)
                if not data: select.unregister(key.fileobj);continue
                if key.data=='guest':log.extend(data);(work/'guest.log').write_bytes(log)
                else:
                    requests.extend(data)
                    while len(requests)>=2:
                        assert requests[0]==2 and 0<requests[1]<=64,bytes(requests)
                        total+=requests[1];del requests[:2]
                if mode=='d' and total and b'rng: computation progressed' in log and not released:
                    assert b'rng: unready' in log and b'rng: waiter ready' not in log
                    connection.sendall(bytes(i%251 for i in range(total)));released=True
            if guest.poll() is not None: break
        guest.wait(timeout=2)
        output=log.decode(errors='replace')
        assert guest.returncode==0 and 'PID 1 exited status=0x2a ' in output and 'heap-live=0x0' in output,output
        assert 'root finish failure' not in output,output
        if mode=='d': assert released and 'rng: delayed ready' in output,output
        elif mode=='s': assert total and 'rng: stop pending' in output,output
        elif mode=='a': assert 'rng: absent boot ok' in output and 'rng: unready wait interrupted' in output,output
        else: assert 'rng: ready' in output,output
        subprocess.run(['python3',str(ROOT/'tests/check-stack-report.py'),str(work/'guest.log')],check=True)
        good=True;print(f'RNG {transport} mode={mode}: PASS',flush=True)
    finally:
        if guest and guest.poll() is None: guest.kill();guest.wait()
        if connection:connection.close()
        if server:server.close()
        if good:shutil.rmtree(work)
        else:print(f'RNG failure artifacts: {work}',flush=True)
for transport in (['legacy','modern'] if a.transport=='all' else [a.transport]):
    for mode in ('n','a','d','s'):run(transport,mode)
