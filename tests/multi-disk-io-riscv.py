#!/usr/bin/env python3
"""Two independent NBD disks: held B, live A, real B EIO, cold reboot."""
import argparse
import json
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--kernel', default=str(ROOT / 'kernel-rv'))
parser.add_argument('--program', default=str(ROOT / 'build/riscv/tests/user/multi-disk-io-rv'))
parser.add_argument('--qemu', default='qemu-system-riscv64')
parser.add_argument('--transport', choices=('all', 'legacy', 'modern'), default='all')
parser.add_argument('--cache', choices=('writeback', 'writethrough'), default='writeback')
parser.add_argument('--fault', choices=('write', 'flush'), default='write')
parser.add_argument('--rt-load', action='store_true', help='independent no-fault FIFO/RR storage progress mode')
args = parser.parse_args()


def send_token(guest, tokens, label, boundary):
    guest.stdin.write(b'g')
    tokens.append({'index': len(tokens) + 1, 'time': time.monotonic(),
                   'after': label + ':' + boundary.decode(errors='replace')})


def check_progress_timeout(guest, work, phase, logs, tokens, state, deadline):
    if guest.poll() is not None or time.monotonic() < deadline:
        return
    snapshot = {'phase': phase, 'tokens': tokens, 'state': state,
                'guest_pid': guest.pid,
                'tails': {name: data.decode(errors='replace').splitlines()[-20:]
                          for name, data in logs.items()}}
    path = work / (str(phase) + '-timeout.json')
    path.write_text(json.dumps(snapshot, indent=2) + '\n')
    raise TimeoutError(f'multi-disk progress timeout: {state}; tokens={len(tokens)}; {path}')


def run(transport, cache, fault):
    work = Path(tempfile.mkdtemp(prefix='multi-disk-io.', dir=ROOT / 'build'))
    processes = []
    success = False
    try:
        kernel = work / 'kernel'
        shutil.copyfile(args.kernel, kernel)
        disks = [work / 'a.img', work / 'b.img']
        for name, disk in zip(('a', 'b'), disks):
            with disk.open('wb') as stream:
                stream.truncate(32 * 1024 * 1024)
            subprocess.run(['mkfs.ext4', '-q', '-F', str(disk)], check=True)
            data = work / f'{name}-data'
            data.write_bytes((f'{name.upper()}-COLD'.encode() + bytes(4090)))
            subprocess.run(['debugfs', '-w', '-R', f'write {data} /disk-{name}-data', str(disk)],
                           check=True, capture_output=True)
        subprocess.run(['debugfs', '-w', '-R', f'write {args.program} /init', str(disks[0])],
                       check=True, capture_output=True)
        subprocess.run(['debugfs', '-w', '-R', 'set_inode_field /init mode 0100755', str(disks[0])],
                       check=True, capture_output=True)
        for reboot in (False, True):
            servers = []
            addresses = []
            logs = {k: bytearray() for k in ('guest', 'a', 'b')}
            tokens = []
            for name, disk in zip(('a', 'b'), disks):
                address = work / f'{name}-{int(reboot)}.sock'
                command = [str(ROOT / 'build/host/nbd-fault'), str(disk), str(address),
                           '--control-stdin', '--arm-on-signal']
                if name == 'b' and not reboot:
                    command.append(f'--fail-{fault}=1')
                server = subprocess.Popen(command, stdin=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
                processes.append(server)
                servers.append(server)
                addresses.append(address)
            deadline = time.monotonic() + 10
            while not all(p.exists() for p in addresses):
                if any(s.poll() is not None for s in servers) or time.monotonic() > deadline:
                    raise RuntimeError('NBD startup failed')
                time.sleep(.01)
            command = [args.qemu, '-machine', 'virt', '-bios', 'default',
                       '-object', 'rng-random,id=entropy,filename=/dev/urandom',
                       '-device', 'virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.7', '-kernel', str(kernel),
                       '-global', 'virtio-mmio.force-legacy=' + ('true' if transport == 'legacy' else 'false'),
                       '-m', '512M', '-smp', '1', '-nographic', '-no-reboot']
            for index, (name, address) in enumerate(zip(('a', 'b'), addresses)):
                command += ['-drive', f'file=nbd+unix:///?socket={address},if=none,format=raw,id={name},cache={cache}',
                            '-device', f'virtio-blk-device,drive={name},bus=virtio-mmio-bus.{index},config-wce=off,request-merging=off']
            guest = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, bufsize=0)
            processes.append(guest)
            selector = selectors.DefaultSelector()
            for label, stream in [('guest', guest.stdout), ('a', servers[0].stderr), ('b', servers[1].stderr)]:
                selector.register(stream, selectors.EVENT_READ, label)
            pending = {k: bytearray() for k in logs}
            held = released = progressed = actual_fault = isolated = False
            a_write = a_flush = False
            cutting = False
            cut_acks = set()
            verified_text = None
            deadline = time.monotonic() + 90
            # 进程退出后管道仍可能留有串口和 NBD 尾部，必须读到 EOF。
            while selector.get_map() and time.monotonic() < deadline:
                for key, _ in selector.select(.2):
                    chunk = key.fileobj.read(4096)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    label = key.data
                    logs[label].extend(chunk)
                    pending[label].extend(chunk)
                    (work / f'{int(reboot)}-{label}.log').write_bytes(logs[label])
                    while b'\n' in pending[label]:
                        line, _, rest = pending[label].partition(b'\n')
                        pending[label] = bytearray(rest)
                        line = line.rstrip(b'\r')
                        if label == 'guest' and line == b'multi-disk: hold ready':
                            servers[1].stdin.write(b'hold\n')
                        elif label == 'b' and line == b'control=hold':
                            send_token(guest, tokens, label, line)
                        elif label == 'b' and line.startswith(b'held=') and not held:
                            if b'command=0 ' in line:
                                held = True
                                send_token(guest, tokens, label, line)
                            else:
                                identity = int(line.split()[0].split(b'=')[1])
                                servers[1].stdin.write(f'release {identity}\n'.encode())
                        elif label == 'a' and held and not released and line.startswith(b'event='):
                            assert b'result=0 ' in line, line
                            a_write |= b'type=WRITE ' in line
                            a_flush |= b'type=FLUSH ' in line
                        elif label == 'guest' and line == b'multi-disk: A progressed while B held':
                            assert held and not released
                            progressed = True
                        elif label == 'guest' and line == b'multi-disk: fault ready':
                            servers[1].stdin.write(b'arm\n')
                        elif label == 'guest' and line == b'multi-disk: B isolated owner retained':
                            assert not reboot and progressed
                            isolated = True
                        elif label == 'b' and line == b'control=arm':
                            send_token(guest, tokens, label, line)
                        elif label == 'b' and line.startswith(b'event=') and b'result=5 ' in line:
                            assert f'type={fault.upper()} '.encode() in line, line
                            actual_fault = True
                        if label in ('a', 'b') and line.startswith(b'cut=') and b'cause=control' in line:
                            assert cutting
                            cut_acks.add(label)
                        # Pipe readiness across processes does not imply a log
                        # ordering. Require all evidence before releasing B.
                        if progressed and a_write and a_flush and not released:
                            servers[1].stdin.write(b'drain\n')
                            released = True
                        if isolated and actual_fault and not cutting:
                            # Stop each NBD endpoint at its explicit power-cut
                            # boundary before killing QEMU: killing first can
                            # interrupt an otherwise healthy response writer.
                            verified_text = logs['guest'].decode(errors='replace')
                            cutting = True
                            for server in servers:
                                server.stdin.write(b'cut\n')
                        if cut_acks == {'a', 'b'} and guest.poll() is None:
                            guest.kill()
            check_progress_timeout(guest, work, int(reboot), logs, tokens,
                {'held': held, 'released': released, 'progressed': progressed,
                 'actual_fault': actual_fault, 'isolated': isolated,
                 'a_write': a_write, 'a_flush': a_flush,
                 'cut_requested': cutting, 'cut_acks': sorted(cut_acks)}, deadline)
            guest.wait(timeout=3)
            for server in servers:
                server.wait(timeout=3)
            # The induced disconnect is teardown, after the verified guest
            # checkpoint. Keep its complete raw log, but judge progress before it.
            text = verified_text if cutting else logs['guest'].decode(errors='replace')
            exit_state = {'guest': guest.returncode, 'servers': [s.returncode for s in servers],
                          'isolated': isolated, 'cut_requested': cutting, 'cut_acks': sorted(cut_acks)}
            (work / f'{int(reboot)}-exit.json').write_text(json.dumps(exit_state, indent=2) + '\n')
            assert not cutting or cut_acks == {'a', 'b'}, exit_state
            marker = 'multi-disk: persistent readback ok' if reboot else 'multi-disk: isolation ok'
            assert (guest.returncode == 0 or isolated) and all(s.returncode == 0 for s in servers), (exit_state, text[-5000:])
            if not isolated:
                assert marker in text and re.search(r'PID 1 exited status=0x2a .*heap-live=0x0;', text), text[-5000:]
            assert not re.search(r'multi-disk failure|fatal trap|root finish failure|block timeout', text), text[-5000:]
            assert not re.search(rb'event=.*result=[1-9]', logs['a']), logs['a'][-1000:]
            if not reboot:
                assert held and released and progressed and actual_fault
            detail = 'cold persistent reboot' if reboot else 'held B, live A, B EIO; ' + ('sticky owner isolated until reset' if isolated else 'owner retry succeeded')
            print(f'PASS multi-disk {transport}/{cache}/{fault}: {detail}', flush=True)
        for disk in disks:
            checked = subprocess.run(['e2fsck', '-fn', str(disk)], capture_output=True)
            (work / f'{disk.stem}-fsck.log').write_bytes(checked.stdout + checked.stderr)
            assert checked.returncode == 0, checked.stdout + checked.stderr
        success = True
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait()
        if success:
            shutil.rmtree(work)
        else:
            print(f'multi-disk artifacts retained: {work}', flush=True)


def run_rt(transport, cache):
    work = Path(tempfile.mkdtemp(prefix='multi-disk-rt.', dir=ROOT / 'build'))
    processes = []
    success = False
    try:
        shutil.copyfile(args.kernel, work / 'kernel')
        (work / 'mode').write_text('rt')
        disks, addresses, servers = [], [], []
        logs = {key: bytearray() for key in ('guest', 'a', 'b')}
        tokens = []
        for name in ('a', 'b'):
            disk = work / (name + '.img')
            with disk.open('wb') as stream: stream.truncate(32 * 1024 * 1024)
            subprocess.run(['mkfs.ext4', '-q', '-F', str(disk)], check=True)
            content = bytearray(131072)
            for offset in (0, 65536): content[offset:offset+6] = (name.upper() + '-COLD').encode()
            data = work / (name + '-data'); data.write_bytes(content)
            commands = [f'write {data} /disk-{name}-data']
            if name == 'a':
                commands += [f'write {args.program} /init', 'set_inode_field /init mode 0100755',
                             f'write {work}/mode /rt-load']
            for command in commands:
                subprocess.run(['debugfs', '-w', '-R', command, str(disk)], check=True, capture_output=True)
            address = work / (name + '.sock')
            server = subprocess.Popen([str(ROOT / 'build/host/nbd-fault'), str(disk), str(address),
                                       '--control-stdin'], stdin=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
            processes.append(server);servers.append(server);disks.append(disk);addresses.append(address)
        deadline = time.monotonic() + 10
        while not all(address.exists() for address in addresses):
            if any(server.poll() is not None for server in servers) or time.monotonic() > deadline:
                raise RuntimeError('RT NBD startup failed')
            time.sleep(.01)
        command = [args.qemu, '-machine', 'virt', '-bios', 'default', '-kernel', str(work / 'kernel'),
                   '-m', '512M', '-smp', '1', '-nographic', '-no-reboot',
                   '-object', 'rng-random,id=entropy,filename=/dev/urandom',
                   '-device', 'virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.7',
                   '-global', 'virtio-mmio.force-legacy=' + ('true' if transport == 'legacy' else 'false')]
        for index, (name, address) in enumerate(zip(('a', 'b'), addresses)):
            command += ['-drive', f'file=nbd+unix:///?socket={address},if=none,format=raw,id={name},cache={cache}',
                        '-device', f'virtio-blk-device,drive={name},bus=virtio-mmio-bus.{index},config-wce=off,request-merging=off']
        guest = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, bufsize=0)
        processes.append(guest)
        selector = selectors.DefaultSelector()
        for label, stream in [('guest', guest.stdout), ('a', servers[0].stderr), ('b', servers[1].stderr)]:
            selector.register(stream, selectors.EVENT_READ, label)
        pending = {key: bytearray() for key in logs}
        active = None; progressed = False; observed = set(); completed = set()
        hold_acks = set()
        deadline = time.monotonic() + 120
        # 回收记录也属于验收输入，进程退出不能代替管道 EOF。
        while selector.get_map() and time.monotonic() < deadline:
            for key, _ in selector.select(.1):
                chunk = key.fileobj.read(4096)
                if not chunk: selector.unregister(key.fileobj);continue
                label = key.data;logs[label].extend(chunk);pending[label].extend(chunk)
                (work / (label + '.log')).write_bytes(logs[label])
                while b'\n' in pending[label]:
                    line, _, rest = pending[label].partition(b'\n');pending[label] = bytearray(rest)
                    line = line.rstrip(b'\r')
                    if label == 'guest' and line.startswith(b'multi-disk-rt: load ready policy='):
                        active = int(line.rsplit(b'=', 1)[1]);progressed = False;observed = set()
                        hold_acks = set()
                        for server in servers: server.stdin.write(b'hold\n')
                    elif label in ('a', 'b') and line == b'control=hold' and active is not None:
                        hold_acks.add(label)
                        if hold_acks == {'a', 'b'}: send_token(guest, tokens, label, line)
                    elif label in ('a', 'b') and line.startswith(b'held=') and active is not None:
                        server = servers[0 if label == 'a' else 1]
                        if b'command=0 ' in line:
                            observed.add((label, 'READ'));server.stdin.write(b'drain\n')
                        else:
                            identity = int(line.split()[0].split(b'=')[1])
                            server.stdin.write(f'release {identity}\n'.encode())
                    elif label in ('a', 'b') and line.startswith(b'event='):
                        assert b'result=0 ' in line, line
                        if active is not None:
                            event = line.split(b'type=')[1].split()[0].decode()
                            observed.add((label, event))
                    elif label == 'guest' and line.startswith(b'multi-disk-rt: I/O progressed policy='):
                        assert active == int(line.rsplit(b'=', 1)[1]);progressed = True
                    if progressed and all((disk, event) in observed for disk in ('a', 'b') for event in ('READ', 'WRITE', 'FLUSH')):
                        completed.add(active);active = None;progressed = False;send_token(guest, tokens, label, line)
        check_progress_timeout(guest, work, 'rt', logs, tokens,
            {'active_policy': active, 'completed_policies': sorted(completed),
             'hold_acks': sorted(hold_acks), 'observed': sorted(observed),
             'progressed': progressed}, deadline)
        guest.wait(timeout=3)
        for server in servers: server.wait(timeout=3)
        text = logs['guest'].decode(errors='replace')
        assert guest.returncode == 0 and all(server.returncode == 0 for server in servers), text[-5000:]
        assert completed == {1, 2} and 'multi-disk-rt: cleanup ok' in text, text[-5000:]
        assert re.search(r'PID 1 exited status=0x2a .*heap-live=0x0;', text), text[-5000:]
        assert not re.search(r'multi-disk failure|fatal trap|root finish failure|block timeout', text), text[-5000:]
        subprocess.run(['python3', str(ROOT / 'tests/check-stack-report.py'), str(work / 'guest.log')], check=True)
        for name, disk in zip(('a', 'b'), disks):
            checked = subprocess.run(['e2fsck', '-fn', str(disk)], capture_output=True)
            assert checked.returncode == 0, checked.stdout + checked.stderr
            output = work / (name + '-persistent')
            subprocess.run(['debugfs', '-R', f'dump /disk-{name}-data {output}', str(disk)], check=True, capture_output=True)
            data = output.read_bytes()
            for round, offset in enumerate((0, 65536)):
                assert data[offset:offset+8192] == bytes([ord(name)+round])*8192
        success = True
        print(f'PASS multi-disk RT {transport}/{cache}: FIFO+RR, normal task and both disks progressed; persistent data and teardown baseline', flush=True)
    finally:
        for process in processes:
            if process.poll() is None: process.kill()
            process.wait()
        if success: shutil.rmtree(work)
        else: print(f'multi-disk RT artifacts retained: {work}', flush=True)


if args.rt_load:
    for transport, cache in ([('legacy', 'writeback'), ('modern', 'writethrough'),
                              ('modern', 'writeback'), ('legacy', 'writethrough')]
                             if args.transport == 'all' else [(args.transport, args.cache)]):
        run_rt(transport, cache)
elif args.transport == 'all':
    for transport, cache, fault in [('legacy', 'writeback', 'write'),
                                     ('modern', 'writethrough', 'flush'),
                                     ('modern', 'writeback', 'flush'),
                                     ('legacy', 'writethrough', 'write')]:
        run(transport, cache, fault)
else:
    run(args.transport, args.cache, args.fault)
