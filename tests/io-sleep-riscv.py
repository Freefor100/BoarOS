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
import hashlib
import json
import fcntl
from cost_report import parse

parser = argparse.ArgumentParser()
parser.add_argument('--kernel', required=True)
parser.add_argument('--qemu', default='qemu-system-riscv64')
parser.add_argument('--transport', choices=('all', 'legacy', 'modern'), default='all')
parser.add_argument('--write-through', action='store_true')
parser.add_argument('--cost-output', type=Path, help='cost records destination; requires one transport, diagnostic fixture')
args = parser.parse_args()
if args.cost_output and args.transport=='all': parser.error('cost fixture needs explicit transport')
if args.transport == 'all':
    for transport in ('legacy', 'modern'):
        for mode in ([], ['--write-through']):
            subprocess.run([sys.executable, __file__, '--kernel', args.kernel,
                            '--qemu', args.qemu, '--transport', transport] + mode, check=True)
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
if args.cost_output:
    (root/'build/cost').mkdir(parents=True,exist_ok=True)
    measurement_lock=(root/'build/cost/measurement.lock').open('w')
    fcntl.flock(measurement_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
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
    fixture_sha256=hashlib.sha256(disk.read_bytes()).hexdigest() if args.cost_output else None
    kernel_snapshot=work/'kernel'
    if args.cost_output:
        shutil.copyfile(args.kernel,kernel_snapshot)
        kernel_path=str(kernel_snapshot)
        source_tree=subprocess.check_output(['git','write-tree'],cwd=root,text=True).strip()
        source_sha256=hashlib.sha256(b''.join(p.encode()+b'\0'+hashlib.sha256((root/p).read_bytes()).hexdigest().encode()+b'\n' for p in sorted(subprocess.check_output(['git','ls-files','-co','--exclude-standard'],cwd=root,text=True).splitlines()) if '__pycache__' not in Path(p).parts)).hexdigest()
        bios=work/'firmware';shutil.copyfile('/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin',bios)
        dtb=work/'boot.dtb'
        subprocess.run([args.qemu,'-machine','virt,dumpdtb='+str(dtb),'-bios',str(bios),'-kernel',kernel_path,'-m','512M','-smp','1','-nographic','-global','virtio-mmio.force-legacy='+('true' if args.transport=='legacy' else 'false'),'-drive',f'file={disk},if=none,format=raw,id=root','-device','virtio-blk-device,drive=root,bus=virtio-mmio-bus.0,config-wce=off,request-merging=off'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        firmware_sha256=hashlib.sha256(bios.read_bytes()).hexdigest();dtb_sha256=hashlib.sha256(dtb.read_bytes()).hexdigest()
    else: kernel_path=args.kernel
    address = work / 'nbd.sock'
    server = subprocess.Popen([str(root / 'build/host/nbd-fault'), str(disk), str(address),
                               '--control-stdin', '--arm-on-signal', '--fail-write=8'],
                              stdin=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    processes.append(server)
    deadline = time.monotonic() + 10
    while not address.exists():
        if server.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError('NBD server startup failed')
        time.sleep(0.01)
    guest_command=[args.qemu, '-machine', 'virt', '-bios', str(bios) if args.cost_output else 'default',
        '-global', 'virtio-mmio.force-legacy=' + ('true' if args.transport == 'legacy' else 'false'),
        '-kernel', kernel_path, '-m', '512M', '-smp', '1', '-nographic', '-no-reboot',
        '-drive', f'file=nbd+unix:///?socket={address},if=none,format=raw,id=root,cache={'writethrough' if args.write_through else 'writeback'}',
        '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0,config-wce=off,request-merging=off']
    if args.cost_output:guest_command+=['-dtb',str(dtb)]
    if args.cost_output:
        input_record={'transport':args.transport,'cache':'writethrough' if args.write_through else 'writeback',
            'kernel_sha256':hashlib.sha256(Path(kernel_path).read_bytes()).hexdigest(),
            'fixture_sha256':fixture_sha256,'source_tree':source_tree,'source_sha256':source_sha256,
            'firmware_sha256':firmware_sha256,'dtb_sha256':dtb_sha256,'argv':guest_command,
            'qemu_version':subprocess.check_output([args.qemu,'--version'],text=True).splitlines()[0],
            'qemu_sha256':hashlib.sha256(Path(shutil.which(args.qemu)).read_bytes()).hexdigest(),
            'nbd_sha256':hashlib.sha256((root/'build/host/nbd-fault').read_bytes()).hexdigest()}
        frozen=json.dumps(input_record,sort_keys=True,separators=(',',':'))+'\n'
        (work/'input.json').write_text(frozen)
        input_record['input_keys']=list(input_record);input_record['input_sha256']=hashlib.sha256(frozen.encode()).hexdigest()
    guest = subprocess.Popen(guest_command,
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
    batch_writes = []
    batch_pending = False
    batch_released = 0
    batch_verified = set()
    batch_deadline = None
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
                if label == 'guest' and line.startswith(b'I/O sleep failed:'):
                    raise RuntimeError(line.decode())
                if label == 'guest' and line.startswith(b'I/O counters:'):
                    print(line.decode(), flush=True)
                elif label == 'guest' and line == b'I/O handshake: ready':
                    server.stdin.write(b'hold\n')
                elif label == 'guest' and line == b'I/O handshake: queue':
                    phase = 'queue'
                    server.stdin.write(b'hold\n')
                elif label == 'guest' and line == b'I/O handshake: timeout':
                    phase = 'timeout'
                    batch_deadline = None
                    server.stdin.write(b'hold\n')
                elif label == 'guest' and line in (b'I/O handshake: batch', b'I/O handshake: batch-partial',
                                                   b'I/O handshake: batch-error'):
                    phase = line.decode().split(': ')[1]
                    batch_writes = []
                    batch_pending = False
                    batch_released = 0
                    batch_deadline = time.monotonic() + 15
                    if phase == 'batch-error': server.stdin.write(b'arm\n')
                    server.stdin.write(b'hold\n')
                elif label == 'guest' and line == b'I/O handshake: batch-pending':
                    assert phase.startswith('batch')
                    batch_pending = True
                    if phase == 'batch-error':
                        # An error response cannot release the batch's other
                        # outstanding DMA or let its unpublished spans proceed.
                        assert len(batch_writes) == 8, batch_writes
                        server.stdin.write(b'drain\n')
                        batch_verified.add(phase)
                        batch_deadline = None
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
                    elif phase.startswith('batch'):
                        command = int(line.split()[1].split(b'=')[1])
                        expected_writes = 10 if phase == 'batch-partial' else 8
                        if command == 1:
                            batch_writes.append(identity)
                            assert len(batch_writes) <= expected_writes, batch_writes
                            if phase == 'batch-error' and len(batch_writes) == 8:
                                # Six outside writers leave two slots for the
                                # batch. The second batch write fails first.
                                server.stdin.write(f'release {identity}\n'.encode())
                        elif command == 3:
                            assert args.write_through or (batch_pending and
                                len(batch_writes) == expected_writes and batch_released == expected_writes), batch_writes
                            server.stdin.write(f'release {identity}\n'.encode())
                        else:
                            assert command == 0 and batch_pending and len(batch_writes) == expected_writes, batch_writes
                            server.stdin.write(f'release {identity}\ndrain\n'.encode())
                            batch_verified.add(phase)
                            batch_deadline = None
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
                if phase in ('batch', 'batch-partial') and batch_pending:
                    if phase == 'batch-partial' and args.write_through:
                        # Without NBD FUA, WRITE replies still require backend
                        # FLUSH. Release the first wave, then each new WRITE.
                        threshold = 8 if batch_released == 0 else batch_released + 1
                    else:
                        threshold = 8 if batch_released == 0 else 9 if batch_released == 2 else 10
                    if len(batch_writes) >= threshold and batch_released < threshold:
                        limit = threshold
                        if phase == 'batch-partial' and not args.write_through and batch_released == 0:
                            # In writeback, require an outside-owner refill
                            # while all six initial batch replies remain held.
                            limit = 2
                        for held_id in reversed(batch_writes[batch_released:limit]):
                            server.stdin.write(f'release {held_id}\n'.encode())
                        batch_released = limit
        if batch_deadline and time.monotonic() > batch_deadline:
            raise RuntimeError(f'{phase}: batch publication or barrier stalled; writes={batch_writes}')
        if guest.poll() is not None:
            break
    guest.wait(timeout=5)
    server.wait(timeout=5)
    success = (guest.returncode == 0 and server.returncode == 0 and released and queue_verified and reset_seen and
               batch_verified == {'batch', 'batch-partial', 'batch-error'} and
               b'BoarOS: I/O sleep tests passed' in logs['guest'] and
               b'I/O sleep failed:' not in logs['guest'])
    if success and args.cost_output:
        snapshots=[]; current=None; body=[]
        for line in logs['guest'].decode().splitlines():
            if line.startswith('COST SNAPSHOT '):
                if current: raise ValueError('nested fixture snapshot')
                _,_,name,epoch=line.split(); current=(name,int(epoch)); body=[]
            elif line=='COST END':
                if not current: raise ValueError('extra fixture end')
                name,epoch=current; snapshot=parse('\n'.join(body),epoch)
                if snapshot['mode']!='fixture': raise ValueError('unmarked fixture')
                snapshots.append({'name':name,'values':snapshot}); current=None
            elif current: body.append(line)
        if current or {s['name'] for s in snapshots}!={'pressure-io','timeout-cancel'}: raise ValueError('missing fixture window')
        args.cost_output.parent.mkdir(parents=True,exist_ok=True)
        args.cost_output.write_text(json.dumps({**input_record,'snapshots':snapshots},indent=2)+'\n')
    if not success:
        raise RuntimeError(logs['guest'][-4000:].decode(errors='replace'))
    print(f'BoarOS: I/O sleep tests passed ({args.transport}, {'writethrough' if args.write_through else 'writeback'}); two cold reads held, CPU/cache progressed, eight-span batch and partial-slot refill and error drain and FLUSH barrier and timeout/reset verified')
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
