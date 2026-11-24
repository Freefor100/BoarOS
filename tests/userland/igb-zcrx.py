#!/usr/bin/env python3
"""Exercise io_uring receive from an isolated 82576 queue and check DMA addresses."""
import argparse
import os
from pathlib import Path
import re
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

PEER_MAC = bytes.fromhex('525400010203')
GUEST_MAC = bytes.fromhex('5254001234d6')
PEER_IP = socket.inet_aton('10.77.0.1')
GUEST_IP = socket.inet_aton('10.77.0.3')
TOTAL = 128 * 1024
CHUNK = 1024


def checksum(data):
    data += b'\0' * (len(data) % 2)
    value = sum(struct.unpack(f'!{len(data) // 2}H', data))
    while value >> 16:
        value = (value & 0xffff) + (value >> 16)
    return value ^ 0xffff


def ethernet(destination, protocol, payload):
    data = destination + PEER_MAC + struct.pack('!H', protocol) + payload
    return struct.pack('!I', len(data)) + data


def ipv4(protocol, payload):
    header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload),
                         1, 0, 64, protocol, 0, PEER_IP, GUEST_IP)
    header = header[:10] + struct.pack('!H', checksum(header)) + header[12:]
    return ethernet(GUEST_MAC, 0x0800, header + payload)


def tcp(sequence, acknowledgment, flags, payload=b''):
    data = struct.pack('!HHIIBBHHH', 5051, 18080, sequence, acknowledgment,
                       0x50, flags, 65535, 0, 0) + payload
    pseudo = PEER_IP + GUEST_IP + struct.pack('!BBH', 0, 6, len(data))
    return ipv4(6, data[:16] + struct.pack('!H', checksum(pseudo + data)) + data[18:])


def receive_exact(peer, count):
    data = bytearray()
    while len(data) < count:
        part = peer.recv(count - len(data))
        if not part:
            raise RuntimeError('guest Ethernet connection closed')
        data.extend(part)
    return bytes(data)


def packet(peer, matches):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        size, = struct.unpack('!I', receive_exact(peer, 4))
        data = receive_exact(peer, size)
        if matches(data):
            return data
    raise RuntimeError('timed out waiting for matching guest Ethernet frame')


def is_ipv4(data, protocol):
    return (len(data) >= 42 and data[:12] == PEER_MAC + GUEST_MAC and
            data[12:14] == b'\x08\x00' and data[23] == protocol and
            data[26:34] == GUEST_IP + PEER_IP)


class TcpPeer:
    def __init__(self, peer):
        self.peer = peer
        self.sequence = 1000
        self.expected = 0
        self.incoming = bytearray()
        self.send(2)
        frame = packet(peer, lambda p: is_ipv4(p, 6) and p[47] & 18 == 18)
        self.expected = struct.unpack('!I', frame[38:42])[0] + 1
        self.send(16)

    def send(self, flags, payload=b''):
        self.peer.sendall(tcp(self.sequence, self.expected, flags, payload))
        self.sequence += len(payload) + bool(flags & (1 | 2))

    def receive(self, length):
        while len(self.incoming) < length:
            frame = packet(self.peer, lambda p: (is_ipv4(p, 6) and
                len(p) > 34 + (p[46] >> 4) * 4))
            sequence, = struct.unpack('!I', frame[38:42])
            if sequence != self.expected:
                raise RuntimeError('unexpected guest TCP sequence')
            payload = frame[34 + (frame[46] >> 4) * 4:]
            self.incoming.extend(payload)
            self.expected += len(payload)
            self.send(16)
        result = self.incoming[:length]
        del self.incoming[:length]
        return result

    def close(self):
        self.send(17)
        frame = packet(self.peer, lambda p: is_ipv4(p, 6) and p[47] & 1)
        self.expected = struct.unpack('!I', frame[38:42])[0] + 1
        self.send(16)


def transfer(peer):
    request = struct.pack('!HHBBH', 1, 0x0800, 6, 4, 1)
    request += PEER_MAC + PEER_IP + bytes(6) + GUEST_IP
    for _ in range(10):
        peer.settimeout(0.25)
        peer.sendall(ethernet(b'\xff' * 6, 0x0806, request))
        try:
            packet(peer, lambda p: (p[:12] == PEER_MAC + GUEST_MAC and
                p[12:14] == b'\x08\x06' and p[20:22] == b'\0\x02'))
            break
        except TimeoutError:
            continue
    else:
        raise RuntimeError('secondary interface did not answer ARP')
    peer.settimeout(10)
    body = bytes(13 * i & 255 for i in range(1472))
    peer.sendall(ipv4(17, struct.pack('!HHHH', 5050, 18081, len(body) + 8, 0) + body))
    if packet(peer, lambda p: is_ipv4(p, 17))[42:] != body:
        raise RuntimeError('UDP fallback payload differs')
    tcp_peer = TcpPeer(peer)
    for start in range(0, TOTAL, CHUNK):
        body = bytes(17 * (start + i) & 255 for i in range(CHUNK))
        tcp_peer.send(24, body)
        if tcp_peer.receive(len(body)) != body:
            raise RuntimeError(f'wrong TCP echo at byte {start}')
    if tcp_peer.receive(1) != b'\xab':
        raise RuntimeError('missing cancellation handshake')
    tcp_peer.close()


def run(args):
    profile = PROFILES[args.arch]
    work = Path(tempfile.mkdtemp(prefix='igb-zcrx-', dir=ROOT / 'build'))
    root = harness.fixture(work, args.program.resolve())
    log, trace = work / 'guest.log', work / 'dma.trace'
    with socket.socket() as listener, log.open('wb') as output:
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(5)
        qemu = args.qemu or os.environ.get(
            'QEMU_RISCV64' if args.arch == 'riscv' else 'QEMU_LOONGARCH64', profile.qemu)
        command = profile.boot(qemu, args.kernel.resolve(), '512M')
        command[command.index('-smp') + 1] = str(args.cpus)
        if args.arch == 'riscv':
            command += ['-global', 'virtio-mmio.force-legacy=false']
        command += ['-drive', f'file={root},if=none,format=raw,id=root',
                    '-device', profile.block('modern'), '-netdev',
                    f'socket,id=peer,connect=127.0.0.1:{listener.getsockname()[1]}',
                    '-device', 'igb,addr=4,netdev=peer,mac=52:54:00:12:34:56',
                    '-trace', f'enable=igb_rx_desc_buff_write,file={trace}']
        with subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT) as guest:
            try:
                peer, _ = listener.accept()
                with peer:
                    deadline = time.monotonic() + 15
                    while 'IGB NET READY' not in log.read_text():
                        if guest.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError('guest did not enable isolated receive')
                        time.sleep(0.01)
                    transfer(peer)
                if guest.wait(timeout=10):
                    raise RuntimeError('QEMU failed')
            except (OSError, RuntimeError) as error:
                raise RuntimeError(f'{error}: {log}') from error
            finally:
                if guest.poll() is None:
                    guest.kill()
    output = log.read_text()
    if 'ZCRX NETWORK PASS' not in output or not profile.root_success(output, 0):
        raise RuntimeError(f'guest did not complete resource cleanup: {log}')
    physical = {int(p, 16) for p in re.findall(r'^ZCRX PAGE ([0-9a-f]+)', output, re.M)}
    if len(physical) != 32:
        raise RuntimeError(f'guest did not report the registered user pages: {log}')
    direct = 0
    for address, size in re.findall(r'buffer 1, addr: (0x[0-9a-f]+).*length: (\d+)', trace.read_text()):
        if int(address, 16) in physical and int(size) == CHUNK:
            direct += 1
    if direct != TOTAL // CHUNK:
        raise RuntimeError(f'only {direct} TCP payloads directly reached registered pages: {trace}')
    harness.run_logged(['e2fsck', '-fn', str(root)], work / 'fsck.log', timeout=30)
    root.unlink()
    print(f'PASS ZCRX {args.arch} cpus={args.cpus} direct_payloads={direct}: {log}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=PROFILES, default='riscv')
    parser.add_argument('--cpus', type=int, default=1)
    parser.add_argument('--qemu')
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--program', type=Path, required=True)
    run(parser.parse_args())
