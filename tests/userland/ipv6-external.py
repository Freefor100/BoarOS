#!/usr/bin/env python3
"""Real Ethernet IPv6 through QEMU slirp to separate host TCP/UDP sockets."""
import argparse
import concurrent.futures
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'tests/diff-abi')]
from arch_profiles import PROFILES
import harness
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--arch',choices=PROFILES,default='riscv')
p.add_argument('--kernel',type=Path,required=True)
p.add_argument('--program',type=Path,required=True)
p.add_argument('--qemu')
p.add_argument('--cpus',type=int,default=1)
a=p.parse_args();profile=PROFILES[a.arch]
(ROOT/'build').mkdir(exist_ok=True)
work=Path(tempfile.mkdtemp(prefix='ipv6-external-',dir=ROOT/'build'))
disk=harness.fixture(work,a.program.resolve())
pattern=bytes((i*31+i//251)&255 for i in range(65536))
def checksum(data):
    if len(data)&1:data+=b'\0'
    value=sum(struct.unpack('!%dH'%(len(data)//2),data))
    while value>>16:value=(value&65535)+(value>>16)
    return (~value)&65535
def injected_udp(data,mode):
    src=socket.inet_pton(socket.AF_INET6,'fe80::2')
    dst=socket.inet_pton(socket.AF_INET6,'fe80::5054:ff:fe12:3456')
    udp=struct.pack('!HHHH',19460,19461,len(data)+8,0)+data
    value=checksum(src+dst+struct.pack('!I3xB',len(udp),17)+udp) or 65535
    if mode=='zero':value=0
    elif mode=='bad':value^=0x1234
    udp=udp[:6]+struct.pack('!H',value)+udp[8:]
    ip=struct.pack('!IHBB',6<<28,len(udp),17,64)+src+dst
    frame=bytes.fromhex('52540012345652550000000286dd')+ip+udp
    return struct.pack('!I',len(frame))+frame
def udp(s,injector):
    channel,_=injector.accept()
    with channel:
        for length in (64,1232,1452):
            data,peer=s.recvfrom(65536)
            if data!=pattern[:length]:raise RuntimeError('host UDP content mismatch')
            if length==64:
                # Same ingress channel orders both malformed packets before
                # the valid response, so the guest receive is a strict barrier.
                bad=bytes([data[0]^255])+data[1:]
                channel.sendall(injected_udp(bad,'zero')+injected_udp(bad,'bad')+injected_udp(data,'valid'))
            else:s.sendto(data,peer)
def tcp(s):
    stream,peer=s.accept()
    with stream:
        stream.settimeout(20);data=bytearray()
        while True:
            block=stream.recv(8192)
            if not block:break
            data+=block
            if len(data)>len(pattern):raise RuntimeError('host TCP excess bytes')
        if data!=pattern:raise RuntimeError('host TCP content mismatch')
        stream.sendall(data)
qemu=a.qemu or os.environ.get('QEMU_RISCV64' if a.arch=='riscv' else 'QEMU_LOONGARCH64',profile.qemu)
command=profile.boot(qemu,a.kernel.resolve(),'512M');command[command.index('-smp')+1]=str(a.cpus)
command+=['-drive',f'file={disk},if=none,format=raw,id=root','-device',profile.block('modern')]
device='virtio-net-device,netdev=n0,bus=virtio-mmio-bus.1' if a.arch=='riscv' else 'virtio-net-pci,netdev=n0,addr=2,disable-legacy=on'
command+=['-netdev','user,id=n0,ipv6=on,ipv6-prefix=fe80::,ipv6-prefixlen=64,ipv6-host=fe80::2','-device',device+',mac=52:54:00:12:34:56']
if a.arch=='riscv':command+=['-global','virtio-mmio.force-legacy=false']
log=work/'guest.log'
with socket.socket(socket.AF_INET,socket.SOCK_STREAM) as injector, socket.socket(socket.AF_INET6,socket.SOCK_DGRAM) as d, socket.socket(socket.AF_INET6,socket.SOCK_STREAM) as s:
    injector.bind(('127.0.0.1',0));injector.listen(1);injector.settimeout(45)
    command+=['-chardev',f'socket,id=inject,host=127.0.0.1,port={injector.getsockname()[1]}',
              '-object','filter-redirector,id=inject-rx,netdev=n0,queue=tx,indev=inject']
    for sock in (d,s):sock.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);sock.bind(('::1',19460));sock.settimeout(45)
    s.listen(1)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        jobs=[pool.submit(udp,d,injector),pool.submit(tcp,s)]
        harness.run_logged(command,log,timeout=70)
        for job in jobs:job.result()
text=log.read_text()
if 'IPV6 EXTERNAL ALL PASS' not in text or 'IPV6 EXTERNAL FAIL' in text or not profile.root_poweroff_success(text):
    raise RuntimeError(f'incomplete IPv6 external contract or cleanup: {log}')
disk.unlink();print(f'PASS external IPv6 {a.arch} cpus={a.cpus}: {log}')
