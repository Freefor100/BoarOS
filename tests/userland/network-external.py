#!/usr/bin/env python3
"""Real TAP traffic in a private user/network namespace; all artifacts in build/."""
import sys
from pathlib import Path
sys.path[:0] = [str(Path(__file__).resolve().parent), str(Path(__file__).resolve().parents[1])]
import argparse
import concurrent.futures
import fcntl
import hashlib
import http.client
import http.server
import json
import os
import queue
import re
import shutil
import select
import socket
import struct
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/diff-abi'))
import harness
sys.path.insert(0,str(ROOT/"tests"))
from arch_profiles import PROFILES
from sdcard import preliminary_image

PATTERN = (bytes(range(251)) * ((16 * 1024 * 1024 + 250) // 251))[:16 * 1024 * 1024]
HOST, GUEST = '10.77.0.1', '10.77.0.2'


def tap():
    fd = os.open('/dev/net/tun', os.O_RDWR)
    fcntl.ioctl(fd, 0x400454ca, struct.pack('16sH22x', b'boar-tap', 0x1002))
    fcntl.ioctl(fd, 0x400454d0, 0)  # No host segmentation/checksum offload.
    subprocess.run(['ip', 'address', 'add', HOST + '/24', 'dev', 'boar-tap'], check=True)
    subprocess.run(['ip', 'link', 'set', 'boar-tap', 'mtu', '1500', 'up'], check=True)
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
    return fd


def connect(port):
    deadline = time.monotonic() + 10
    while True:
        try:
            s = socket.create_connection((GUEST, port), timeout=1)
            s.settimeout(180)
            return s
        except (ConnectionRefusedError, TimeoutError, OSError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(.02)


def flow(size):
    begin = time.monotonic_ns()
    with connect(18080) as s:
        s.sendall(memoryview(PATTERN)[:size])
        s.shutdown(socket.SHUT_WR)
        received = 0
        while received < size:
            data = s.recv(min(65536, size - received))
            if not data or data != PATTERN[received:received + len(data)]:
                raise RuntimeError('TCP content/length mismatch at ' + str(received))
            received += len(data)
        if s.recv(1):
            raise RuntimeError('TCP trailing data')
    return {'sent': size, 'received': received, 'begin_ns': begin,
            'end_ns': time.monotonic_ns()}


def udp():
    begin = time.monotonic_ns()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(15)
        rows = []
        for length, count in ((64, 10000), (1472, 1), (1473, 1), (65507, 1)):
            data = PATTERN[:length]
            start = time.monotonic_ns()
            latencies = []
            for _ in range(count):
                request_start = time.monotonic_ns()
                s.sendto(data, (GUEST, 18081))
                reply, peer = s.recvfrom(65536)
                if reply != data or peer != (GUEST, 18081):
                    raise RuntimeError('UDP content/peer mismatch')
                latencies.append(time.monotonic_ns() - request_start)
            latencies.sort()
            rows.append({'bytes': length, 'transactions': count,
                         'elapsed_ns': time.monotonic_ns() - start,
                         'latency_ns': {key: latencies[min(count - 1, int(count * fraction))]
                                        for key, fraction in (('p50', .5), ('p95', .95), ('p99', .99), ('max', 1))}})
    return {'begin_ns': begin, 'end_ns': time.monotonic_ns(), 'rows': rows}


def pressure():
    start = time.monotonic_ns()
    with connect(18085) as stream, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as datagrams:
        for _ in range(128):
            datagrams.sendto(PATTERN[:1472], (GUEST, 18084))
        stream.sendall(PATTERN[:8192])
        stream.shutdown(socket.SHUT_WR)
        received = 0
        while received < 8192:
            data = stream.recv(8192 - received)
            if not data or data != PATTERN[received:received + len(data)]:
                raise RuntimeError('pressure TCP content/length')
            received += len(data)
        if stream.recv(1):
            raise RuntimeError('pressure TCP trailing bytes')
    return {'udp_submitted': 128, 'udp_payload': 1472, 'tcp_sent': 8192,
            'tcp_received': received, 'elapsed_ns': time.monotonic_ns() - start}


class HostHTTP(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'
    get_bytes = post_bytes = 0
    failure = None

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path != '/data.bin':
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Length', str(len(PATTERN)))
        self.end_headers()
        self.wfile.write(PATTERN)
        type(self).get_bytes += len(PATTERN)

    def do_POST(self):
        length = int(self.headers.get('Content-Length', -1))
        data = self.rfile.read(length) if 0 <= length <= 4096 else b''
        expected = bytes(65 + i % 26 for i in range(4096))
        if self.path != '/post' or data != expected:
            type(self).failure = 'wget POST content/length mismatch'
            self.send_error(400)
            return
        type(self).post_bytes += length
        self.send_response(200)
        self.send_header('Content-Length', '8')
        self.end_headers()
        self.wfile.write(b'POST OK\n')


def http_guest():
    start = time.monotonic_ns()
    with connect(18082) as readiness:
        pass
    connection = http.client.HTTPConnection(GUEST, 18082, timeout=180)
    connection.request('GET', '/data.bin')
    response = connection.getresponse()
    received = response.read()
    if response.status != 200 or received != PATTERN:
        raise RuntimeError('guest httpd GET content/status')
    connection.close()
    connection = http.client.HTTPConnection(GUEST, 18082, timeout=180)
    connection.request('POST', '/cgi-bin/upload', body=PATTERN,
                       headers={'Content-Type': 'application/octet-stream'})
    response = connection.getresponse()
    reply = response.read()
    if response.status != 200 or reply != b'UPLOAD OK\n':
        raise RuntimeError('guest httpd CGI POST: ' + repr((response.status, reply)))
    connection.close()
    with connect(18080) as s:
        s.sendall(b'H')
        s.shutdown(socket.SHUT_WR)
    return {'get_bytes': len(received), 'post_bytes': len(PATTERN),
            'elapsed_ns': time.monotonic_ns() - start}



def ipv4_checksum(data):
    if len(data) & 1:
        data += b'\0'
    value = sum(struct.unpack('!' + 'H' * (len(data) // 2), data))
    while value >> 16:
        value = (value & 65535) + (value >> 16)
    return (~value) & 65535


def ipv4_host_phase(phase, control):
    """The guest waits for G with all endpoints installed; completion is UDP."""
    receiver = echo = None
    wire = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0800))
    wire.bind(('boar-tap', 0)); wire.setblocking(False)
    rows = []
    if phase == 'udp-out':
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(('', 18091)); receiver.setblocking(False)
    if phase == 'host-echo':
        echo = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)
        echo.settimeout(8)
    control.sendto(b'G', (GUEST, 18090))
    if phase == 'host-echo':
        payload = bytes((i + 3) % 251 for i in range(8, 64))
        request = struct.pack('!BBHHH', 8, 0, 0, 0x4b32, 3) + payload
        request = request[:2] + struct.pack('!H', ipv4_checksum(request)) + request[4:]
        echo.sendto(request, (GUEST, 0))
        while True:
            packet, peer = echo.recvfrom(65536)
            header = (packet[0] & 15) * 4
            if peer[0] == GUEST and packet[header:header + 2] == b'\0\0' and packet[header + 4:header + 8] == struct.pack('!HH', 0x4b32, 3):
                if packet[header + 8:] != payload or ipv4_checksum(packet[header:]):
                    raise RuntimeError('guest kernel Echo reply content/checksum')
                control.sendto(b'H', (GUEST, 18090))
                break
    elif phase == 'bound-ports':
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            sender.bind((HOST, 0))
            sender.sendto(b'bound-udp-eth', (GUEST, 18094))
        with socket.create_connection((GUEST, 18095), timeout=8) as stream:
            stream.settimeout(8); stream.sendall(b'bound-tcp-eth')
            reply = bytearray()
            while len(reply) < 13:
                data = stream.recv(13 - len(reply))
                if not data:
                    raise RuntimeError('bound TCP peer closed before content')
                reply += data
            if reply != b'bound-tcp-eth':
                raise RuntimeError('bound TCP remote content mismatch')
    elif phase == 'udp-in':
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            sender.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            sender.bind((HOST, 0))
            sender.sendto(b'host-broadcast', ('10.77.0.255', 18092))
    deadline = time.monotonic() + 20
    complete = False
    try:
        while not complete:
            left = deadline - time.monotonic()
            if left <= 0:
                raise RuntimeError('IPv4 phase timeout: ' + phase)
            readable, _, _ = select.select([wire, control] + ([receiver] if receiver else []), [], [], left)
            if not readable:
                raise RuntimeError('IPv4 phase timeout: ' + phase)
            if wire in readable:
                while True:
                    try:
                        frame = wire.recv(65536)
                    except BlockingIOError:
                        break
                    if len(frame) < 34 or frame[12:14] != b'\x08\x00':
                        continue
                    packet = frame[14:]
                    header = (packet[0] & 15) * 4
                    if header < 20 or len(packet) < header or packet[12:16] != socket.inet_aton(GUEST):
                        continue
                    flags_offset = struct.unpack_from('!H', packet, 6)[0]
                    rows.append({'ttl': packet[8], 'protocol': packet[9],
                        'source': socket.inet_ntoa(packet[12:16]), 'destination': socket.inet_ntoa(packet[16:20]),
                        'id': struct.unpack_from('!H', packet, 4)[0], 'fragment_offset': (flags_offset & 8191) * 8,
                        'more_fragments': bool(flags_offset & 8192), 'bytes': struct.unpack_from('!H', packet, 2)[0],
                        'payload': packet[header:struct.unpack_from('!H', packet, 2)[0]].hex()})
            if receiver and receiver in readable:
                data, peer = receiver.recvfrom(65536)
                if data != b'guest-broadcast' or peer != (GUEST, 18091):
                    raise RuntimeError('guest UDP broadcast content/source')
                control.sendto(b'U', (GUEST, 18090))
            if control in readable:
                data, peer = control.recvfrom(65536)
                if data != phase.encode() or peer != (GUEST, 18090):
                    raise RuntimeError('IPv4 completion mismatch: ' + repr((data, peer)))
                complete = True
        if phase == 'udp-out':
            packets = [r for r in rows if r['protocol'] == 17 and bytes.fromhex(r['payload'])[8:] == b'guest-broadcast']
            if not packets or any(r['ttl'] != 37 or r['destination'] != '10.77.0.255' for r in packets):
                raise RuntimeError('broadcast wire TTL/destination not verified')
        if phase == 'raw-wire':
            requests = [r for r in rows if r['protocol'] == 1 and r['fragment_offset'] == 0 and bytes.fromhex(r['payload'])[:2] == b'\x08\0']
            for sequence in (4, 5):
                first = [r for r in requests if bytes.fromhex(r['payload'])[4:8] == struct.pack('!HH', 0x4b33, sequence)]
                if len(first) != 1 or first[0]['ttl'] != 37:
                    raise RuntimeError('raw wire request/TTL not verified')
                fragments = [r for r in rows if r['protocol'] == 1 and r['id'] == first[0]['id']]
                if sequence == 5:
                    if len(fragments) < 3 or not any(r['fragment_offset'] for r in fragments):
                        raise RuntimeError('large raw packet did not produce wire fragments')
                    rebuilt = bytearray()
                    ordered = sorted(fragments, key=lambda r: r['fragment_offset'])
                    for index, fragment in enumerate(ordered):
                        if fragment['fragment_offset'] != len(rebuilt) or fragment['ttl'] != 37 or fragment['more_fragments'] != (index + 1 < len(ordered)):
                            raise RuntimeError('raw fragment offset/TTL/final flag mismatch')
                        rebuilt += bytes.fromhex(fragment['payload'])
                    expected = bytes((i + 5) % 251 for i in range(8, 4096))
                    if len(rebuilt) != 4096 or rebuilt[8:] != expected or ipv4_checksum(bytes(rebuilt)):
                        raise RuntimeError('raw wire reassembled content/checksum mismatch')
        if phase.startswith('ping-'):
            requests = [r for r in rows if r['protocol'] == 1 and r['fragment_offset'] == 0 and bytes.fromhex(r['payload'])[:2] == b'\x08\0']
            expected_ttl = 37 if phase == 'ping-ttl' else 64
            if not requests or any(r['ttl'] != expected_ttl or r['destination'] != HOST for r in requests):
                raise RuntimeError('BusyBox ping wire TTL/source not verified: ' + phase)
        return {'packets': rows, 'completed': True}
    finally:
        wire.close()
        if receiver: receiver.close()
        if echo: echo.close()


def fixture(work, workload, libc, reference, observe, arch, busybox=None):
    tree = work / 'tree'
    tree.mkdir()
    profile=PROFILES[arch]
    compiler = Path(os.environ.get('MUSL_CC') or ROOT / ('build/riscv/musl-root/bin/musl-gcc' if arch=='riscv' else 'build/loongarch/musl-root/bin/musl-gcc'))
    environment=os.environ.copy()
    flags=profile.musl_flags(compiler)
    if arch=='loongarch':environment['REALGCC']=str(ROOT/'build/loongarch/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc')
    program = work / 'init'
    subprocess.run([str(compiler), *flags, '-static', '-O2',
                    '-Wall', '-Wextra', '-Werror', str(ROOT / 'tests/workloads/network' / (workload + '.c')),
                    '-o', str(program)], env=environment,check=True)
    shutil.copyfile(program, tree / 'init')
    (tree / 'init').chmod(0o755)
    for name in ('dev', 'lib', 'proc', 'tmp'):
        (tree / name).mkdir()
    if reference:
        (tree / 'reference-network-setup').touch()
    if observe:
        (tree / 'observe').touch()
    if workload == 'ipv4_basic':
        (tree / 'external-ipv4-setup').touch()
    inputs = {'consumer_source_sha256': harness.digest(ROOT / 'tests/workloads/network' / (workload + '.c'))}
    if workload in ('external', 'ipv4_basic') and arch=='loongarch':
        if libc!='musl':raise SystemExit('LA external GNU BusyBox input must be provided by the GNU inventory stage')
        target=tree/'busybox'
        source=busybox or ROOT/'build/loongarch/busybox-source/busybox/busybox'
        headers=subprocess.check_output(['readelf','-lW',str(source)],text=True)
        if 'Requesting program interpreter:' in headers:
            raise RuntimeError('explicit LA BusyBox requires its own runtime; provide a static original ELF')
        shutil.copyfile(source,target);target.chmod(0o755)
        inputs['busybox']=harness.digest(target)
        inputs['busybox_source']=str(source.resolve())
    if workload in ('external', 'ipv4_basic') and arch=='riscv':
        original = preliminary_image('riscv')
        inputs['runtime_image'] = str(original)
        for name in ('busybox', 'lib/libc.so') if libc == 'musl' else (
                'busybox', 'lib/libc.so.6', 'lib/libm.so.6', 'lib/ld-linux-riscv64-lp64d.so.1'):
            target = tree / name
            subprocess.run(['debugfs', '-R', f'dump /{libc}/{name} {target}', str(original)],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if not target.is_file() or target.stat().st_size == 0:
                raise RuntimeError(f'original RV runtime export failed: /{libc}/{name} from {original}')
            target.chmod(0o755)
            inputs[name] = harness.digest(target)
        if libc == 'musl':
            (tree / 'lib/ld-musl-riscv64-sf.so.1').symlink_to('libc.so')
    if workload == 'external':
        http = tree / 'http'
        (http / 'cgi-bin').mkdir(parents=True)
        (http / 'data.bin').write_bytes(PATTERN)
        subprocess.run([str(compiler), *flags, '-static', '-O2', '-Wall',
                        '-Wextra', '-Werror', str(ROOT / 'tests/workloads/network/external-cgi.c'),
                        '-o', str(http / 'cgi-bin/upload')], env=environment,check=True)
    image = work / 'fixture.img'
    with image.open('wb') as stream:
        stream.truncate(128 * 1024 * 1024)
    harness.run_logged(['mkfs.ext4', '-q', '-F', '-b', '4096', '-d', str(tree), str(image)], work / 'mkfs.log')
    commands = work / 'devices.debugfs'
    commands.write_text('cd /dev\nmknod console c 5 1\nset_inode_field console mode 020600\n'
                        'mknod null c 1 3\nset_inode_field null mode 020666\n'
                        'mknod urandom c 1 9\nset_inode_field urandom mode 020666\n')
    harness.run_logged(['debugfs', '-w', '-f', str(commands), str(image)], work / 'debugfs.log')
    return image, inputs


def run_one(work, kernel, image, transport, reference, workload, tap_fd, arch, memory, cpus=1, initrd=None):
    work.mkdir()
    snapshot = work / 'kernel'
    shutil.copyfile(kernel, snapshot)
    disk = work / 'disk.img'
    shutil.copyfile(image, disk)
    profile=PROFILES[arch]
    qemu=os.environ.get('QEMU_LOONGARCH64' if arch=='loongarch' else 'QEMU_RISCV64',profile.qemu)
    command=profile.boot(qemu,snapshot,memory)
    command[command.index('-smp')+1]=str(cpus)
    if arch=='riscv':command+=['-global','virtio-mmio.force-legacy='+('true' if transport=='legacy' else 'false')]
    command+=['-drive',f'file={disk},if=none,format=raw,id=root,cache=writeback',
        '-device',profile.block(transport),'-netdev',f'tap,id=host,fd={tap_fd},vhost=off',
        '-device',('virtio-net-pci,netdev=host,mac=52:54:00:12:34:56,addr=5,disable-legacy=on,romfile=' if arch=='loongarch' else
            'virtio-net-device,netdev=host,mac=52:54:00:12:34:56,bus=virtio-mmio-bus.1'),
        '-object','rng-random,id=entropy,filename=/dev/urandom','-device',profile.rng()]
    if reference:
        command+=(['-initrd',str(initrd),'-append','console=ttyS0 rdinit=/init loglevel=3'] if arch=='loongarch' else
            ['-append','root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1'])
    row = {'command': command, 'cpus':cpus, 'kernel_sha256': harness.digest(snapshot), 'phases': {}}
    events = queue.Queue()
    raw = []
    program_finished = []
    start = time.monotonic_ns()
    with (work / 'console.log').open('w') as log:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, bufsize=1, pass_fds=(tap_fd,))
        def reader():
            for line in process.stdout:
                log.write(line)
                log.flush()
                raw.append(line)
                if 'EXTERNAL PASS all' in line:
                    program_finished.append(time.monotonic_ns())
                for prefix in ('EXTERNAL READY ', 'IPV4 READY '):
                    if prefix in line:
                        events.put(line.split(prefix, 1)[1].strip())
            events.put(None)
        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        server = None
        control = None
        try:
            if workload == 'ipv4_basic':
                control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                control.bind((HOST, 18090))
                for phase in ('host-echo', 'udp-out', 'udp-in', 'bound-ports', 'raw-wire',
                              'ping-default', 'ping-ttl', 'ping-device', 'ping-source'):
                    ready = events.get(timeout=30)
                    if ready != phase:
                        raise RuntimeError('first missing IPv4 phase ' + phase + ': ' + repr(ready))
                    row['phases'][phase] = ipv4_host_phase(phase, control)
            if workload == 'external':
                HostHTTP.get_bytes = HostHTTP.post_bytes = 0
                HostHTTP.failure = None
                server = http.server.ThreadingHTTPServer((HOST, 18083), HostHTTP)
                threading.Thread(target=server.serve_forever, daemon=True).start()
                for phase in ('tcp', 'parallel', 'udp', 'pressure', 'http'):
                    ready = events.get(timeout=180)
                    if ready != phase:
                        raise RuntimeError('first missing phase ' + phase + ': ' + repr(ready))
                    if phase == 'tcp':
                        value = flow(16 * 1024 * 1024)
                    elif phase == 'parallel':
                        gate = threading.Barrier(5)
                        def parallel(_):
                            gate.wait()
                            return flow(8 * 1024 * 1024)
                        with concurrent.futures.ThreadPoolExecutor(5) as pool:
                            value = list(pool.map(parallel, range(5)))
                    elif phase == 'udp':
                        value = udp()
                    elif phase == 'pressure':
                        value = pressure()
                    else:
                        value = http_guest()
                    row['phases'][phase] = value
            process.wait(timeout=180)
            stopped = time.monotonic_ns()
            thread.join(timeout=5)
            text = ''.join(raw)
            marker = 'EXTERNAL PASS all' if workload == 'external' else 'NETWORK PASS ' + workload
            if marker not in text or process.returncode:
                raise RuntimeError('guest marker/exit missing')
            if workload == 'ipv4_basic':
                actors = re.findall(r'^IPV4 CALLER cpu=(\d+) cpus=(\d+)$', text.replace('\r',''), re.M)
                if actors != [('1' if cpus>1 else '0',str(cpus))]:
                    raise RuntimeError('IPv4 actual caller CPU not verified: ' + repr(actors))
                names = ('raw-two-receivers-peek-trunc', 'raw-large-loopback', 'broadcast-permission-receive',
                         'host-echo-observed-and-replied', 'udp-wire-broadcast', 'bound-device-shared-port-isolation', 'raw-wire-fragmentation', 'busybox-ping')
                actual = [line for line in text.replace('\r', '').splitlines() if line.startswith('IPV4 CASE ')]
                if actual != ['IPV4 CASE ' + name + ' PASS' for name in names] or 'IPV4 FAIL' in text:
                    raise RuntimeError('IPv4 guest cases incomplete or reordered')
                for phase in ('ping-default', 'ping-ttl', 'ping-device', 'ping-source'):
                    if text.count('IPV4 BUSYBOX ' + phase + ' BEGIN') != 1 or text.count('IPV4 BUSYBOX ' + phase + ' END') != 1:
                        raise RuntimeError('BusyBox output record missing: ' + phase)

            if reference and arch=='loongarch' and 'Linux LA root application passed' not in text:
                raise RuntimeError('Linux application exit/root unmount failed')
            if not reference and not profile.root_success(text,42 if workload=='external' else 0):
                raise RuntimeError('guest teardown not complete')
            if workload == 'external':
                if HostHTTP.failure or HostHTTP.get_bytes != len(PATTERN) or HostHTTP.post_bytes != 4096:
                    raise RuntimeError('host HTTP actual transfer: ' + str(HostHTTP.failure))
                row['phases']['http']['guest_wget_get_bytes'] = HostHTTP.get_bytes
                row['phases']['http']['guest_wget_post_bytes'] = HostHTTP.post_bytes
                if len(program_finished) != 1:
                    raise RuntimeError('ambiguous guest program completion')
                row['guest_program'] = re.findall(r'EXTERNAL PROGRAM start_ns=(\d+) end_ns=(\d+)', text)
                row['guest_records'] = [line for line in text.splitlines() if 'EXTERNAL ' in line]
                row['after_program_until_shutdown_ns'] = stopped - program_finished[0]
                final = re.search(r'BoarOS: network final (.*)', text)
                row['driver_statistics'] = {key: int(value) for key, value in re.findall(r'([\w-]+)=(\d+)', final.group(1))} if final else {}
                if not reference and (row['driver_statistics'].get('loan-peak') != 32 or
                                      row['driver_statistics'].get('copy-packets', 0) == 0 or
                                      row['driver_statistics'].get('tx-sg', 0) == 0 or
                                      row['driver_statistics'].get('tx-copy', 0) >=
                                          row['driver_statistics'].get('tx-sg', 0) or
                                      row['driver_statistics'].get('errors') != 0):
                    raise RuntimeError('loan budget/fallback/device completion not verified')
                mixed = re.search(r'BoarOS: mixed IRQ rng-bytes=(0x[0-9a-f]+) rng-errors=(0x[0-9a-f]+) rng-timeouts=(0x[0-9a-f]+) block-irqs=(0x[0-9a-f]+)', text)
                if not reference:
                    if not mixed:
                        raise RuntimeError('mixed device completion evidence missing')
                    row['mixed_irq'] = dict(zip(('rng_bytes', 'rng_errors', 'rng_timeouts', 'block_irqs'),
                                                (int(value, 16) for value in mixed.groups())))
                    if row['mixed_irq']['rng_bytes'] < 32 or row['mixed_irq']['block_irqs'] < 1 or row['mixed_irq']['rng_errors'] or row['mixed_irq']['rng_timeouts']:
                        raise RuntimeError('mixed device completion failed')
                if 'COST SNAPSHOT' in text:
                    sys.path.insert(0, str(ROOT / 'tests'))
                    from cost import cost_report
                    snapshots = []
                    for name, epoch, body in re.findall(r'^COST SNAPSHOT (\S+) (\d+)\n(.*?)^COST END$', text, re.M | re.S):
                        fields = cost_report.parse(body, int(epoch))
                        snapshots.append({'name': name, 'values': fields})
                    if len(snapshots) != 5:
                        raise RuntimeError('missing observation phase')
                    row['cost'] = snapshots
                    row['protocol'] = [{'name': name, 'phase': when,
                                        'values': {key: int(value) for key, value in re.findall(r'^(\w+)=(\d+)$', body, re.M)}}
                                       for name, when, body in re.findall(r'^NETWORK PROTOCOL name=(\S+) phase=(\S+)\n(.*?)^NETWORK PROTOCOL END$', text, re.M | re.S)]
            row['status'] = 'passed'
            disk.unlink()
        except Exception as error:
            row.update(status='failed', error=str(error))
            print('\n'.join(''.join(raw).splitlines()[-12:]), flush=True)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            thread.join(timeout=5)
            if control:
                control.close()
            if server:
                server.shutdown()
                server.server_close()
            row['elapsed_ns'] = time.monotonic_ns() - start
            row['qemu_returncode'] = process.returncode
            (work / 'result.json').write_text(json.dumps(row, indent=2) + '\n')
    print(work.name, row['status'], f"{row['elapsed_ns'] / 1e9:.3f}s", flush=True)
    return row['status'] == 'passed'


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--isolated', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--prepared',type=Path,help=argparse.SUPPRESS)
    parser.add_argument('--host-probe',action='store_true',help='check private namespace and actual TAP access without starting a guest')
    parser.add_argument('--arch',choices=('riscv','loongarch'),default='riscv')
    parser.add_argument('--memory',choices=('512M','1G'),action='append')
    parser.add_argument('--cpus',type=int,choices=(1,2),default=1)
    parser.add_argument('--only', choices=('linux', 'boaros','both'))
    parser.add_argument('--workload', choices=('interface', 'external', 'ipv4_basic'), default='external')
    parser.add_argument('--libc', choices=('musl', 'glibc'), default='musl')
    parser.add_argument('--busybox', type=Path, help='explicit unmodified static LA BusyBox input; path and SHA-256 are recorded')
    parser.add_argument('--transport', choices=('legacy', 'modern', 'both'), default='modern')
    parser.add_argument('--repeat', type=int, choices=(1, 3), default=1)
    parser.add_argument('--observe', action='store_true')
    parser.add_argument('--kernel', type=Path)
    args = parser.parse_args()
    namespace = [os.environ.get('BOAROS_UNSHARE', 'unshare'), '--user', '--map-root-user', '--net']
    if args.host_probe:
        if not args.isolated:
            return subprocess.call([*namespace, sys.executable, '-B', str(Path(__file__).resolve()),
                                    '--host-probe', '--isolated'])
        fd = tap()
        os.close(fd)
        print('Private user/net namespace and TAP access PASS', flush=True)
        return 0
    if args.busybox and (args.arch!='loongarch' or args.workload=='interface'):
        parser.error('--busybox selects a static LA original-program consumer')
    profile=PROFILES[args.arch]
    args.kernel=args.kernel or ROOT/profile.kernel
    args.only=args.only or ('both' if args.arch=='loongarch' else 'boaros')
    if args.arch=='loongarch' and args.transport in ('legacy','both'):parser.error('LA profile supports modern PCI only')
    if not args.isolated:
        # Compile once, then use the identical ELF and libraries in every reference/target boot.
        prepared=ROOT/'build/network'/('external-input-'+str(time.time_ns()));prepared.mkdir(parents=True)
        image,inputs=fixture(prepared,args.workload,args.libc,False,args.observe,args.arch,args.busybox)
        inputs['init']=harness.digest(prepared/'init')
        (prepared/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
        # A new host namespace per boot also resets ARP and host TCP state.
        base = ['--isolated', '--arch',args.arch,'--workload', args.workload,
                '--libc', args.libc, '--kernel', str(args.kernel), '--repeat', '1','--prepared',str(prepared),'--cpus',str(args.cpus)]
        if args.observe:
            base.append('--observe')
        for platform in ('linux','boaros') if args.only=='both' else (args.only,):
            for memory in args.memory or (('512M','1G') if args.arch=='loongarch' else ('512M',)):
                for transport in ('legacy','modern') if args.transport=='both' else (args.transport,):
                    for repetition in range(args.repeat):
                        print(f'isolated boot {args.arch} {platform} {memory} {transport} {repetition+1}/{args.repeat}',flush=True)
                        result=subprocess.call([*namespace,sys.executable,'-B',
                            str(Path(__file__).resolve()),*base,'--only',platform,'--memory',memory,'--transport',transport])
                        if result:return result
        return 0
    work = ROOT / 'build/network' / ('external-' + str(time.time_ns()))
    work.mkdir(parents=True)
    kernel = args.kernel
    if args.only == 'linux':
        if args.arch=='loongarch':
            kernel=profile.linux_kernel();identity=json.loads((kernel.parent/'boaros-identity.json').read_text())
        else:
            config = ROOT / 'tests/userland/network-linux.config'
            if args.cpus>1:
                derived = work / 'linux-network-smp.config'
                derived.write_text(config.read_text().replace('CONFIG_SMP=n', 'CONFIG_SMP=y\nCONFIG_NR_CPUS=8'))
                config = derived
            kernel, identity = harness.linux_build(config)
        (work / 'linux-identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    if args.observe and (args.only != 'boaros' or args.repeat != 1):
        parser.error('observation is one BoarOS boot')
    if args.prepared:
        image=work/'fixture.img';shutil.copyfile(args.prepared/'fixture.img',image)
        inputs=json.loads((args.prepared/'inputs.json').read_text())
        if harness.digest(args.prepared/'init')!=inputs['init']:raise RuntimeError('prepared ELF changed')
        if args.only=='linux':
            marker=work/'reference-network-setup';marker.touch()
            harness.run_logged(['debugfs','-w','-R',f'write {marker} /reference-network-setup',str(image)],work/'reference-setup.log')
    else:image,inputs=fixture(work,args.workload,args.libc,args.only=='linux',args.observe,args.arch,args.busybox)
    metadata = {'inputs': inputs, 'fixture_sha256': harness.digest(image),
                'source': harness.output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD']),
                'source_tree': harness.output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD^{tree}']),
                'qemu': harness.output([os.environ.get('QEMU_LOONGARCH64' if args.arch=='loongarch' else 'QEMU_RISCV64',profile.qemu), '--version']),
                'qemu_sha256': harness.digest(shutil.which(os.environ.get('QEMU_LOONGARCH64' if args.arch=='loongarch' else 'QEMU_RISCV64',profile.qemu))),
                'compiler': harness.output([str(Path(os.environ.get('MUSL_CC') or ROOT / ('build/riscv/musl-root/bin/musl-gcc' if args.arch=='riscv' else 'build/loongarch/musl-root/bin/musl-gcc'))), '--version']),
                'configuration': vars(args) | {'kernel': str(kernel)}, 'namespace': harness.output(['ip', '-brief', 'link'])}
    (work / 'identity.json').write_text(json.dumps(metadata, default=str, indent=2) + '\n')
    initrd=None
    if args.arch=='loongarch' and args.only=='linux':
        sys.path.insert(0,str(ROOT/'tests/loongarch'))
        from root import execute
        from reference import archive
        supervisor=work/'supervisor'
        execute([profile.compiler,*profile.raw_flags,'-O2',f'-DEXPECTED_EXIT_STATUS={42 if args.workload=="external" else 0}',
            '-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdlib','-nostartfiles','-static','-no-pie',
            '-Wl,--build-id=none','-Wl,-z,max-page-size=16384','-T','tests/common/user.ld',
            'tests/loongarch/root_linux_init.c','tests/common/user_start.S','-o',supervisor])
        initrd=work/'initramfs.gz';initrd.write_bytes(archive([('dev',0o040755,b'',0,0),('dev/console',0o020600,b'',5,1),
            ('init',0o100755,supervisor.read_bytes(),0,0),('TRAILER!!!',0,b'',0,0)]))
    fd = tap()
    passed = True
    try:
        for transport in ('legacy', 'modern') if args.transport == 'both' else (args.transport,):
            for repetition in range(args.repeat):
                if not run_one(work / f'{args.only}-{transport}-{repetition + 1}', kernel, image,
                               transport, args.only == 'linux', args.workload, fd,args.arch,args.memory[0] if args.memory else '512M',args.cpus,initrd):
                    passed = False
                    break
            if not passed:
                break
    finally:
        os.close(fd)
    if args.prepared and harness.digest(args.prepared/'init')!=inputs['init']:raise RuntimeError('ELF changed during differential boots')
    print('artifacts:', work)
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
