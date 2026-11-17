#!/usr/bin/env python3
"""MLDv1 and application multicast over QEMU Ethernet, not guest loopback/TAP."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tests'), str(ROOT / 'tests/diff-abi')]
from arch_profiles import PROFILES
import harness
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--arch', choices=PROFILES, default='riscv')
p.add_argument('--kernel', type=Path, required=True)
p.add_argument('--program', type=Path, required=True)
p.add_argument('--qemu')
p.add_argument('--cpus', type=int, default=1)
a = p.parse_args(); profile = PROFILES[a.arch]
(ROOT / 'build').mkdir(exist_ok=True)
work = Path(tempfile.mkdtemp(prefix='ipv6-multicast-', dir=ROOT / 'build'))
disk = harness.fixture(work, a.program.resolve())
GROUP = socket.inet_pton(socket.AF_INET6, 'ff02::1234')
HOST = socket.inet_pton(socket.AF_INET6, 'fe80::2')
GUEST = socket.inet_pton(socket.AF_INET6, 'fe80::5054:ff:fe12:3456')
GUEST_MAC = bytes.fromhex('525400123456')
def checksum(data):
    if len(data) & 1: data += b'\0'
    value = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while value >> 16: value = (value & 65535) + (value >> 16)
    return (~value) & 65535
def pattern(stage):
    return bytes((stage * 19 + i * 31 + i // 7) & 255 for i in range(257))
def packet(dst, body, protocol=17, hop=64):
    mac = b'\x33\x33' + dst[-4:] if dst[0] == 255 else GUEST_MAC
    frame = mac + bytes.fromhex('52550000000286dd')
    frame += struct.pack('!IHBB', 6 << 28, len(body), protocol, hop) + HOST + dst + body
    return struct.pack('!I', len(frame)) + frame
def udp(dst, port, data):
    body = struct.pack('!HHHH', 19460, port, len(data) + 8, 0) + data
    value = checksum(HOST + dst + struct.pack('!I3xB', len(body), 17) + body) or 65535
    return packet(dst, body[:6] + struct.pack('!H', value) + body[8:])
def query():
    body = bytes([130, 0, 0, 0]) + struct.pack('!HH', 0, 0) + GROUP
    value = checksum(HOST + GROUP + struct.pack('!I3xB', len(body), 58) + body)
    body = body[:2] + struct.pack('!H', value) + body[4:]
    # HBH length8: ICMPv6 next-header, Router Alert(value0), PadN(0).
    return packet(GROUP, bytes([58, 0, 5, 2, 0, 0, 1, 0]) + body, 0, 1)
def peer(sock, injector):
    channel, _ = injector.accept()
    with channel:
        for stage in range(1, 7):
            data, _ = sock.recvfrom(128)
            if data != b'MLD:' + bytes([stage]):
                raise RuntimeError('unexpected guest handshake ' + repr(data))
            frames = query() if stage == 2 else b''
            frames += udp(GROUP, 19462, pattern(stage))
            frames += udp(GUEST, 19461, data)
            channel.sendall(frames)
def identity(path):
    path = Path(path).resolve()
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
def capture_records(path):
    data = path.read_bytes()
    if len(data) < 24: raise RuntimeError('missing pcap header')
    order = {'d4c3b2a1': '<', 'a1b2c3d4': '>', '4d3cb2a1': '<', 'a1b23c4d': '>'}.get(data[:4].hex())
    if not order: raise RuntimeError('unknown pcap magic')
    if struct.unpack_from(order + 'I', data, 20)[0] != 1: raise RuntimeError('pcap is not Ethernet')
    offset = 24
    while offset < len(data):
        if len(data) - offset < 16: raise RuntimeError('truncated pcap record')
        _, _, length, full = struct.unpack_from(order + 'IIII', data, offset); offset += 16
        if length != full or length > len(data) - offset: raise RuntimeError('truncated pcap frame')
        yield data[offset:offset + length]; offset += length
def verify_wire(path):
    markers = {}; reports = []; done = []; outgoing = []
    for ordinal, frame in enumerate(capture_records(path)):
        if len(frame) < 54 or frame[6:12] != GUEST_MAC or frame[12:14] != b'\x86\xdd': continue
        size, protocol, hop = struct.unpack_from('!HBB', frame, 18)
        src, dst = frame[22:38], frame[38:54]
        body = frame[54:54 + size]
        if len(body) != size: raise RuntimeError('short emitted IPv6 frame')
        alert = False
        if protocol == 0:
            if len(body) < 8: raise RuntimeError('short emitted HBH')
            hlen = (body[1] + 1) * 8
            if len(body) < hlen: raise RuntimeError('bad emitted HBH length')
            options = body[2:hlen]; at = 0
            while at < len(options):
                kind = options[at]
                if kind == 0: at += 1; continue
                if at + 2 > len(options) or at + 2 + options[at + 1] > len(options):
                    raise RuntimeError('malformed emitted HBH option')
                length = options[at + 1]
                if kind == 5 and length == 2 and options[at + 2:at + 4] == b'\0\0': alert = True
                at += 2 + length
            protocol, body = body[0], body[hlen:]
        if protocol not in (17, 58): continue
        if checksum(src + dst + struct.pack('!I3xB', len(body), protocol) + body):
            raise RuntimeError('bad emitted upper-layer checksum')
        if protocol == 17:
            sport, dport, length, value = struct.unpack_from('!HHHH', body)
            if length != len(body) or not value: raise RuntimeError('invalid emitted UDP')
            data = body[8:]
            if dport == 19460 and data[:4] == b'MLD:' and len(data) == 5:
                if data[4] in markers: raise RuntimeError('duplicate stage marker')
                markers[data[4]] = ordinal
            if dst == GROUP and dport == 19462:
                if frame[:6] != b'\x33\x33' + GROUP[-4:] or hop != 5 or src != GUEST or data != pattern(99):
                    raise RuntimeError('outbound multicast MAC/source/HL/content mismatch')
                outgoing.append(ordinal)
        elif len(body) >= 24 and body[0] in (131, 132) and body[8:24] == GROUP:
            expected = GROUP if body[0] == 131 else socket.inet_pton(socket.AF_INET6, 'ff02::2')
            if dst != expected or frame[:6] != b'\x33\x33' + expected[-4:] or hop != 1 or not alert or body[1] or len(body) != 24:
                raise RuntimeError('invalid MLDv1 destination/RouterAlert/HL/format')
            if src != GUEST and not (body[0] == 131 and src == bytes(16)):
                raise RuntimeError('invalid MLDv1 source')
            (reports if body[0] == 131 else done).append(ordinal)
    if set(markers) != set(range(1, 7)) or len(outgoing) != 1:
        raise RuntimeError('missing stage markers/outbound multicast: ' + repr((markers, outgoing)))
    if not any(n < markers[1] for n in reports): raise RuntimeError('missing initial MLD report')
    if not any(markers[2] < n < markers[3] for n in reports): raise RuntimeError('zero-delay query did not elicit report')
    if any(markers[2] < n < markers[3] for n in done): raise RuntimeError('shared membership left interface early')
    for previous, current in ((3, 4), (5, 6)):
        if not any(markers[previous] < n < markers[current] for n in done):
            raise RuntimeError('last leave/close did not emit MLD Done')
    return {'reports': len(reports), 'done': len(done), 'outbound_datagrams': len(outgoing), 'stages': 6}
qemu = a.qemu or os.environ.get('QEMU_RISCV64' if a.arch == 'riscv' else 'QEMU_LOONGARCH64', profile.qemu)
command = profile.boot(qemu, a.kernel.resolve(), '512M'); command[command.index('-smp') + 1] = str(a.cpus)
command += ['-drive', f'file={disk},if=none,format=raw,id=root', '-device', profile.block('modern')]
device = 'virtio-net-device,netdev=n0,bus=virtio-mmio-bus.1' if a.arch == 'riscv' else 'virtio-net-pci,netdev=n0,addr=2,disable-legacy=on'
command += ['-netdev', 'user,id=n0,ipv6=on,ipv6-prefix=fe80::,ipv6-prefixlen=64,ipv6-host=fe80::2', '-device', device + ',mac=52:54:00:12:34:56']
if a.arch == 'riscv': command += ['-global', 'virtio-mmio.force-legacy=false']
log = work / 'guest.log'; pcap = work / 'wire.pcap'
manifest = {'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
            'source_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True)),
            'arch': a.arch, 'cpus': a.cpus, 'kernel': identity(a.kernel), 'program': identity(a.program),
            'scope': 'QEMU Ethernet/slirp external injection; no TAP, physical NIC or performance claim'}
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as injector, socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as sock:
    injector.bind(('127.0.0.1', 0)); injector.listen(1); injector.settimeout(45)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); sock.bind(('::1', 19460)); sock.settimeout(45)
    command += ['-object', f'filter-dump,id=wire,netdev=n0,file={pcap}',
                '-chardev', f'socket,id=inject,host=127.0.0.1,port={injector.getsockname()[1]}',
                '-object', 'filter-redirector,id=inject-rx,netdev=n0,queue=tx,indev=inject']
    manifest['command'] = command
    (work / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
        job = pool.submit(peer, sock, injector)
        harness.run_logged(command, log, timeout=70)
        job.result()
for name, path in [('kernel', a.kernel), ('program', a.program)]:
    if identity(path) != manifest[name]: raise RuntimeError(name + ' changed during validation')
text = log.read_text()
if 'IPV6 MULTICAST ALL PASS' not in text or 'IPV6 MULTICAST FAIL' in text or not profile.root_poweroff_success(text):
    raise RuntimeError(f'incomplete multicast contract/cleanup: {log}')
manifest['wire_checks'] = verify_wire(pcap); manifest['passed'] = True
manifest['pcap'] = identity(pcap)
(work / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
disk.unlink(); print(f'PASS external IPv6 multicast {a.arch} cpus={a.cpus}: {log}')
