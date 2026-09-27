#!/usr/bin/env python3
"""Prove overlapping cold I/O and CPU/cache progress with held NBD responses."""
import argparse
from pathlib import Path
import selectors
import shutil
import subprocess
import tempfile
import time
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--kernel', required=True)
parser.add_argument('--qemu', default='qemu-system-riscv64')
parser.add_argument('--transport', choices=('all', 'legacy', 'modern'), default='all')
parser.add_argument('--write-through', action='store_true')
args = parser.parse_args()
if args.transport == 'all':
    for transport in ('legacy', 'modern'):
        for mode in ([], ['--write-through']):
            subprocess.run([sys.executable, __file__, '--kernel', args.kernel,
                            '--qemu', args.qemu, '--transport', transport] + mode, check=True)
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
work = Path(tempfile.mkdtemp(prefix='io-sleep-run.', dir=root / 'build'))
processes = []
logs = {'guest': bytearray(), 'server': bytearray()}
success = False
try:
    disk = work / 'root.img'
    with disk.open('wb') as stream:
        stream.truncate(128 * 1024 * 1024)
    subprocess.run(['mkfs.ext4', '-q', '-F', '-b', '4096', str(disk)], check=True)
    for index in range(2):
        data = work / f'cold{index}'
        data.write_bytes(bytes((i * 19 + index) % 256 for i in range(4096)))
        subprocess.run(['debugfs', '-w', '-R', f'write {data} /cold{index}', str(disk)],
                       check=True, capture_output=True)
    address = work / 'nbd.sock'
    server = subprocess.Popen([str(root / 'build/host/nbd-fault'), str(disk), str(address),
                               '--control-stdin'], stdin=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    processes.append(server)
    deadline = time.monotonic() + 10
    while not address.exists():
        if server.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError('NBD server startup failed')
        time.sleep(0.01)
    guest = subprocess.Popen([args.qemu, '-machine', 'virt', '-bios', 'default',
        '-global', 'virtio-mmio.force-legacy=' + ('true' if args.transport == 'legacy' else 'false'),
        '-kernel', args.kernel, '-m', '512M', '-smp', '1', '-nographic', '-no-reboot',
        '-drive', f'file=nbd+unix:///?socket={address},if=none,format=raw,id=root,cache={'writethrough' if args.write_through else 'writeback'}',
        '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0,config-wce=off,request-merging=off'],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    processes.append(guest)
    selector = selectors.DefaultSelector()
    selector.register(guest.stdout, selectors.EVENT_READ, 'guest')
    selector.register(server.stderr, selectors.EVENT_READ, 'server')
    pending = {'guest': bytearray(), 'server': bytearray()}
    held = []
    progress = False
    released = False
    phase = "cold"
    queued = []
    queue_verified = False
    timeout_held = []
    reset_seen = False
    deadline = time.monotonic() + 120
    while selector.get_map() and time.monotonic() < deadline:
        for key, _ in selector.select(1):
            chunk = key.fileobj.read(4096)
            if not chunk:
                selector.unregister(key.fileobj)
                continue
            label = key.data
            logs[label].extend(chunk)
            with (work / f"{label}.log").open("ab") as log:
                log.write(chunk)
            pending[label].extend(chunk)
            while b'\n' in pending[label]:
                line, _, rest = pending[label].partition(b'\n')
                pending[label] = bytearray(rest)
                line = line.rstrip(b'\r')
                if label == 'guest' and line.startswith(b'I/O counters:'):
                    print(line.decode(), flush=True)
                elif label == 'guest' and line == b'I/O handshake: ready':
                    server.stdin.write(b'hold\n')
                elif label == 'guest' and line == b'I/O handshake: queue':
                    phase = 'queue'
                    server.stdin.write(b'hold\n')
                elif label == 'guest' and line == b'I/O handshake: timeout':
                    phase = 'timeout'
                    server.stdin.write(b'hold\n')
                elif label == 'guest' and line == b'BoarOS: block timeout; resetting device':
                    assert phase == 'timeout' and len(timeout_held) == 8, timeout_held
                    reset_seen = True
                    server.stdin.write(b'drain\n')
                elif label == 'server' and line == b'control=hold':
                    guest.stdin.write(b'g')
                elif label == 'server' and line.startswith(b'held='):
                    identity = int(line.split()[0].split(b'=')[1])
                    if phase == 'cold':
                        held.append(identity)
                    elif phase == 'timeout':
                        timeout_held.append(identity)
                    else:
                        command = int(line.split()[1].split(b'=')[1])
                        queued.append((identity, command))
                        count = len(queued)
                        if count <= 8:
                            assert command == 1, queued
                            if count == 8:
                                for held_id, _ in reversed(queued):
                                    server.stdin.write(f'release {held_id}\n'.encode())
                        elif command == 3:
                            # QEMU may emulate write-through with backend
                            # FLUSH after each WRITE response; guest completion
                            # still waits for that persistence operation.
                            assert args.write_through or count == 9, queued
                            server.stdin.write(f'release {identity}\n'.encode())
                        else:
                            assert command == 0 and (args.write_through or count == 10), queued
                            server.stdin.write(f'release {identity}\ndrain\n'.encode())
                            queue_verified = True
                        assert count <= (17 if args.write_through else 10), queued
                elif label == 'guest' and line == b'I/O handshake: progress':
                    progress = True
                if len(held) >= 2 and progress and not released:
                    server.stdin.write(f'release {held[1]}\ndrain\n'.encode())
                    released = True
        if guest.poll() is not None:
            break
    guest.wait(timeout=5)
    server.wait(timeout=5)
    success = (guest.returncode == 0 and server.returncode == 0 and released and queue_verified and reset_seen and
               b'BoarOS: I/O sleep tests passed' in logs['guest'] and
               b'I/O sleep failed:' not in logs['guest'])
    if not success:
        raise RuntimeError(logs['guest'][-4000:].decode(errors='replace'))
    print(f'BoarOS: I/O sleep tests passed ({args.transport}, {'writethrough' if args.write_through else 'writeback'}); two cold reads held, CPU/cache progressed, reverse completion and FLUSH barrier and timeout/reset verified')
finally:
    for process in processes:
        if process.poll() is None:
            process.kill()
        process.wait()
    for label, data in logs.items():
        (work / f'{label}.log').write_bytes(data)
    if success:
        shutil.rmtree(work)
    else:
        print(f'I/O sleep artifacts retained: {work}')
