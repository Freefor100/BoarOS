#!/usr/bin/env python3
"""Verify 82576 hardware header splitting and DMA return in QEMU."""
import argparse
import os
from pathlib import Path
import selectors
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tests'), str(ROOT / 'tests/diff-abi')]
from arch_profiles import PROFILES
import harness


def frame(index, destination='525400123456'):
    payload = bytes((index + 5 * offset) & 0xff for offset in range(512))
    ethernet = bytes.fromhex(destination + '5254000102030800')
    header = bytes.fromhex('4500021c00010000401100000a0000010a000002')
    checksum = sum(struct.unpack('!10H', header))
    checksum = (checksum & 0xffff) + (checksum >> 16)
    header = header[:10] + struct.pack('!H', (checksum ^ 0xffff) & 0xffff) + header[12:]
    udp = struct.pack('!HHHH', 5050, 5051, len(payload) + 8, 0)
    data = ethernet + header + udp + payload
    return struct.pack('!I', len(data)) + data


def receive_exact(peer, count):
    data = bytearray()
    while len(data) < count:
        part = peer.recv(count - len(data))
        if not part:
            raise RuntimeError('82576 socket closed during transmit')
        data.extend(part)
    return data


def run_dma(args, profile, qemu):
    work = Path(tempfile.mkdtemp(prefix='igb-', dir=ROOT / 'build'))
    root = harness.fixture(work, args.program.resolve())
    log = work / 'guest.log'
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(5)
        port = listener.getsockname()[1]
        command = profile.boot(qemu, args.kernel.resolve(), '512M')
        command += ['-drive', f'file={root},if=none,format=raw,id=root',
                    '-device', profile.block('modern'),
                    '-global', 'virtio-mmio.force-legacy=false',
                    '-netdev', f'socket,id=fixture,connect=127.0.0.1:{port}',
                    '-device', 'igb,addr=4,netdev=fixture,mac=52:54:00:12:34:56']
        if args.trace:
            events_file = work / 'trace-events'
            events_file.write_text('e1000e_rx_*\ne1000x_rx_*\nigb_rx_*\n')
            command += ['-trace', f'events={events_file},file={work / "qemu.trace"}']
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as guest:
            try:
                peer, _ = listener.accept()
                with peer, log.open('wb') as output, selectors.DefaultSelector() as events:
                    peer.settimeout(2)
                    index = 0
                    pending = b''
                    events.register(guest.stdout, selectors.EVENT_READ)
                    stop_at = time.monotonic() + 30
                    while events.select(max(0, stop_at - time.monotonic())):
                        data = os.read(guest.stdout.fileno(), 8192)
                        if not data:
                            break
                        output.write(data)
                        output.flush()
                        pending += data.replace(b'\r', b'')
                        lines = pending.split(b'\n')
                        pending = lines.pop()
                        for line in lines:
                            if line == b'IGB SEND':
                                peer.sendall(frame(index))
                                index += 1
                            if line == b'IGB TRANSMIT':
                                for value in (0xab, 0xcd):
                                    size, = struct.unpack('!I', receive_exact(peer, 4))
                                    expected = bytes.fromhex('52540001020352540012345688b5')
                                    if size != 64 or receive_exact(peer, size) != expected + bytes([value]) * 50:
                                        raise RuntimeError(f'82576 transmit bytes differ: {log}')
                            if line == b'IGB SELECT':
                                for value, destination in enumerate((
                                    '525400123456', 'ffffffffffff', '525400123458', '525400123457'
                                ), 40):
                                    peer.sendall(frame(value, destination))
                        if time.monotonic() > stop_at:
                            break
                if guest.wait(timeout=2) != 0:
                    raise RuntimeError(f'QEMU failed: {log}')
            finally:
                if guest.poll() is None:
                    guest.kill()
    text = log.read_text()
    if (index != 40 or 'IGB DMA PASS' not in text or 'IGB ISOLATION PASS' not in text
            or not profile.root_success(text, 0)):
        raise RuntimeError(f'82576 receive incomplete: {log}')
    harness.run_logged(['e2fsck', '-fn', str(root)], work / 'fsck.log', timeout=30)
    root.unlink()
    print(f'PASS 82576 DMA: {log}')


def run_network(args, profile, qemu):
    work = Path(tempfile.mkdtemp(prefix='igb-net-', dir=ROOT / 'build'))
    root = harness.fixture(work, args.program.resolve())
    log = work / 'guest.log'
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp_port, socket.socket() as tcp_port:
        udp_port.bind(('127.0.0.1', 0))
        tcp_port.bind(('127.0.0.1', 0))
        udp = udp_port.getsockname()[1]
        tcp = tcp_port.getsockname()[1]
    command = profile.boot(qemu, args.kernel.resolve(), '512M')
    command[command.index('-smp') + 1] = str(args.cpus)
    if args.arch == 'riscv':
        command += ['-global', 'virtio-mmio.force-legacy=false']
    if args.virtio:
        network = ('virtio-net-pci,netdev=host,addr=5,disable-legacy=on,romfile='
                   if args.arch == 'loongarch' else 'virtio-net-device,netdev=host,bus=virtio-mmio-bus.1')
    else:
        network = 'igb,addr=4,netdev=host'
    command += ['-drive', f'file={root},if=none,format=raw,id=root',
                '-device', profile.block('modern'),
                '-netdev', f'user,id=host,net=10.77.0.0/24,host=10.77.0.1,'
                f'hostfwd=udp:127.0.0.1:{udp}-{args.guest_address}:18081,'
                f'hostfwd=tcp:127.0.0.1:{tcp}-{args.guest_address}:18080',
                '-device', network + ',mac=52:54:00:12:34:56']
    if args.trace:
        command += ['-object', f'filter-dump,id=wire,netdev=host,file={work / "wire.pcap"}']
    with log.open('wb') as output, subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT) as guest:
        try:
            deadline = time.monotonic() + 15
            while 'IGB NET READY' not in log.read_text():
                if guest.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError(f'guest did not listen: {log}')
                time.sleep(0.01)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
                peer.settimeout(0.2)
                payload = bytes(13 * offset & 0xff for offset in range(1472))
                # The userspace backend may discard the datagram that starts
                # ARP discovery before the link finishes negotiation.
                deadline = time.monotonic() + 5
                while True:
                    peer.sendto(payload, ('127.0.0.1', udp))
                    try:
                        reply = peer.recvfrom(1500)[0]
                        break
                    except TimeoutError:
                        if time.monotonic() >= deadline:
                            raise RuntimeError(f'UDP reply timed out: {log}')
                if reply != payload:
                    raise RuntimeError(f'UDP payload mismatch: {log}')
            with socket.create_connection(('127.0.0.1', tcp), timeout=10) as peer:
                payload = bytes(17 * offset & 0xff for offset in range(128 * 1024))
                peer.sendall(payload)
                peer.shutdown(socket.SHUT_WR)
                if receive_exact(peer, len(payload)) != payload or peer.recv(1):
                    raise RuntimeError(f'TCP payload mismatch: {log}')
            if guest.wait(timeout=10):
                raise RuntimeError(f'QEMU failed: {log}')
        finally:
            if guest.poll() is None:
                guest.kill()
    output = log.read_text()
    transport = 'modern' if args.virtio else 'igb'
    if ('IGB NETWORK PASS' not in output or f'eth0 ready transport={transport}' not in output
            or not profile.root_success(output, 0)):
        raise RuntimeError(f'incomplete socket or root cleanup: {log}')
    harness.run_logged(['e2fsck', '-fn', str(root)], work / 'fsck.log', timeout=30)
    root.unlink()
    print(f'PASS {transport} socket {args.arch}, cpus={args.cpus}: {log}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=PROFILES, default='riscv')
    parser.add_argument('--qemu')
    parser.add_argument('--cpus', type=int, default=1)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--program', type=Path, required=True)
    parser.add_argument('--trace', action='store_true')
    parser.add_argument('--network', action='store_true')
    parser.add_argument('--guest-address', default='10.77.0.2')
    parser.add_argument('--virtio', action='store_true', help='exercise the original socket transport')
    args = parser.parse_args()
    if not args.network and (args.arch != 'riscv' or args.cpus != 1 or args.virtio):
        parser.error('DMA fixture requires RISC-V and one CPU')
    profile = PROFILES[args.arch]
    qemu = args.qemu or profile.qemu
    (run_network if args.network else run_dma)(args, profile, qemu)
