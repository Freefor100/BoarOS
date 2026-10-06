#!/usr/bin/env python3
"""Real TAP traffic in a private user/network namespace; all artifacts in build/."""
import argparse
import concurrent.futures
import fcntl
import hashlib
import http.client
import http.server
import json
import os
from pathlib import Path
import queue
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/diff-abi'))
import harness
sys.path.insert(0,str(ROOT/"tests"))
from arch_profiles import PROFILES

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


def fixture(work, workload, libc, reference, observe, arch):
    tree = work / 'tree'
    tree.mkdir()
    profile=PROFILES[arch]
    compiler = ROOT / ('build/riscv/musl-root/bin/musl-gcc' if arch=='riscv' else 'build/loongarch/musl-root/bin/musl-gcc')
    environment=os.environ.copy()
    flags=['-fno-link-libatomic'] if arch=='riscv' else [*profile.raw_flags,'-Wl,-z,max-page-size=16384']
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
    inputs = {}
    if workload == 'external' and arch=='loongarch':
        if libc!='musl':raise SystemExit('LA external GNU BusyBox input must be provided by the GNU inventory stage')
        target=tree/'busybox'
        shutil.copyfile(ROOT/'build/loongarch/busybox-source/busybox/busybox',target);target.chmod(0o755)
        inputs['busybox']=harness.digest(target)
    if workload == 'external' and arch=='riscv':
        original = ROOT / 'references/oscomp-autotest/sdcard-rv.img'
        for name in ('busybox', 'lib/libc.so') if libc == 'musl' else (
                'busybox', 'lib/libc.so.6', 'lib/libm.so.6', 'lib/ld-linux-riscv64-lp64d.so.1'):
            target = tree / name
            subprocess.run(['debugfs', '-R', f'dump /{libc}/{name} {target}', str(original)],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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


def run_one(work, kernel, image, transport, reference, workload, tap_fd, arch, memory, initrd=None):
    work.mkdir()
    snapshot = work / 'kernel'
    shutil.copyfile(kernel, snapshot)
    disk = work / 'disk.img'
    shutil.copyfile(image, disk)
    profile=PROFILES[arch]
    qemu=os.environ.get('QEMU_LOONGARCH64' if arch=='loongarch' else 'QEMU_RISCV64',profile.qemu)
    command=profile.boot(qemu,snapshot,memory)
    if arch=='riscv':command+=['-global','virtio-mmio.force-legacy='+('true' if transport=='legacy' else 'false')]
    command+=['-drive',f'file={disk},if=none,format=raw,id=root,cache=writeback',
        '-device',profile.block(transport),'-netdev',f'tap,id=host,fd={tap_fd},vhost=off',
        '-device',('virtio-net-pci,netdev=host,mac=52:54:00:12:34:56,addr=5,disable-legacy=on' if arch=='loongarch' else
            'virtio-net-device,netdev=host,mac=52:54:00:12:34:56,bus=virtio-mmio-bus.1'),
        '-object','rng-random,id=entropy,filename=/dev/urandom','-device',profile.rng()]
    if reference:
        command+=(['-initrd',str(initrd),'-append','console=ttyS0 rdinit=/init loglevel=3'] if arch=='loongarch' else
            ['-append','root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1'])
    row = {'command': command, 'kernel_sha256': harness.digest(snapshot), 'phases': {}}
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
                if 'EXTERNAL READY ' in line:
                    events.put(line.split('EXTERNAL READY ', 1)[1].strip())
            events.put(None)
        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        server = None
        try:
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
            marker = 'EXTERNAL PASS all' if workload == 'external' else 'NETWORK PASS interface'
            if marker not in text or process.returncode:
                raise RuntimeError('guest marker/exit missing')
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
                    import cost_report
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
    parser.add_argument('--arch',choices=('riscv','loongarch'),default='riscv')
    parser.add_argument('--memory',choices=('512M','1G'),action='append')
    parser.add_argument('--only', choices=('linux', 'boaros','both'))
    parser.add_argument('--workload', choices=('interface', 'external'), default='external')
    parser.add_argument('--libc', choices=('musl', 'glibc'), default='musl')
    parser.add_argument('--transport', choices=('legacy', 'modern', 'both'), default='modern')
    parser.add_argument('--repeat', type=int, choices=(1, 3), default=1)
    parser.add_argument('--observe', action='store_true')
    parser.add_argument('--kernel', type=Path)
    args = parser.parse_args()
    profile=PROFILES[args.arch]
    args.kernel=args.kernel or ROOT/profile.kernel
    args.only=args.only or ('both' if args.arch=='loongarch' else 'boaros')
    if args.arch=='loongarch' and args.transport in ('legacy','both'):parser.error('LA profile supports modern PCI only')
    if not args.isolated:
        # Compile once, then use the identical ELF and libraries in every reference/target boot.
        prepared=ROOT/'build/network'/('external-input-'+str(time.time_ns()));prepared.mkdir(parents=True)
        image,inputs=fixture(prepared,args.workload,args.libc,False,args.observe,args.arch)
        inputs['init']=harness.digest(prepared/'init')
        (prepared/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
        # A new host namespace per boot also resets ARP and host TCP state.
        base = ['--isolated', '--arch',args.arch,'--workload', args.workload,
                '--libc', args.libc, '--kernel', str(args.kernel), '--repeat', '1','--prepared',str(prepared)]
        if args.observe:
            base.append('--observe')
        for platform in ('linux','boaros') if args.only=='both' else (args.only,):
            for memory in args.memory or (('512M','1G') if args.arch=='loongarch' else ('512M',)):
                for transport in ('legacy','modern') if args.transport=='both' else (args.transport,):
                    for repetition in range(args.repeat):
                        print(f'isolated boot {args.arch} {platform} {memory} {transport} {repetition+1}/{args.repeat}',flush=True)
                        result=subprocess.call(['unshare','--user','--map-root-user','--net',sys.executable,'-B',
                            str(Path(__file__).resolve()),*base,'--only',platform,'--memory',memory,'--transport',transport])
                        if result:return result
        return 0
    work = ROOT / 'build/network' / ('external-' + str(time.time_ns()))
    work.mkdir(parents=True)
    kernel = args.kernel
    if args.only == 'linux':
        if args.arch=='loongarch':
            kernel=profile.linux_kernel();identity=json.loads((kernel.parent/'boaros-identity.json').read_text())
        else:kernel, identity = harness.linux_build(ROOT / 'tests/network-linux.config')
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
    else:image,inputs=fixture(work,args.workload,args.libc,args.only=='linux',args.observe,args.arch)
    metadata = {'inputs': inputs, 'fixture_sha256': harness.digest(image),
                'source': harness.output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD']),
                'source_tree': harness.output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD^{tree}']),
                'qemu': harness.output([os.environ.get('QEMU_LOONGARCH64' if args.arch=='loongarch' else 'QEMU_RISCV64',profile.qemu), '--version']),
                'qemu_sha256': harness.digest(shutil.which(os.environ.get('QEMU_LOONGARCH64' if args.arch=='loongarch' else 'QEMU_RISCV64',profile.qemu))),
                'compiler': harness.output([str(ROOT / ('build/riscv/musl-root/bin/musl-gcc' if args.arch=='riscv' else 'build/loongarch/musl-root/bin/musl-gcc')), '--version']),
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
                               transport, args.only == 'linux', args.workload, fd,args.arch,args.memory[0] if args.memory else '512M',initrd):
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
