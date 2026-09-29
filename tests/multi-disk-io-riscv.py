#!/usr/bin/env python3
"""Two independent NBD disks: held B, live A, real B EIO, cold reboot."""
import argparse
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
args = parser.parse_args()


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
            command = [args.qemu, '-machine', 'virt', '-bios', 'default', '-kernel', str(kernel),
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
            deadline = time.monotonic() + 90
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
                            guest.stdin.write(b'g')
                        elif label == 'b' and line.startswith(b'held=') and not held:
                            if b'command=0 ' in line:
                                held = True
                                guest.stdin.write(b'g')
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
                            guest.stdin.write(b'g')
                        elif label == 'b' and line.startswith(b'event=') and b'result=5 ' in line:
                            assert f'type={fault.upper()} '.encode() in line, line
                            actual_fault = True
                        # Pipe readiness across processes does not imply a log
                        # ordering. Require all evidence before releasing B.
                        if progressed and a_write and a_flush and not released:
                            servers[1].stdin.write(b'drain\n')
                            released = True
                        if isolated and actual_fault and guest.poll() is None:
                            guest.kill()
                if guest.poll() is not None:
                    break
            guest.wait(timeout=3)
            for server in servers:
                server.wait(timeout=3)
            text = logs['guest'].decode(errors='replace')
            marker = 'multi-disk: persistent readback ok' if reboot else 'multi-disk: isolation ok'
            assert (guest.returncode == 0 or isolated) and all(s.returncode == 0 for s in servers), text[-5000:]
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


if args.transport == 'all':
    for transport, cache, fault in [('legacy', 'writeback', 'write'),
                                     ('modern', 'writethrough', 'flush'),
                                     ('modern', 'writeback', 'flush'),
                                     ('legacy', 'writethrough', 'write')]:
        run(transport, cache, fault)
else:
    run(args.transport, args.cache, args.fault)
