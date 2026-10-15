#!/usr/bin/env python3
"""Check ordinary read/pread/readv batching and ownership with RA0 and real ext4."""
import argparse
from collections import Counter
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
PAGES = 16
LENGTH = 20 * 4096 + 123
PHASES = ('read', 'pread', 'hot', 'readv', 'unaligned', 'mixed', 'eof', 'at-eof',
          'efault-prefix', 'efault-first', 'eio-prefix', 'eio-first', 'eio-gap', 'retry', 'held-owner', 'hole')


def image(work, block_size):
    disk = work / 'root.img'
    with disk.open('wb') as stream:
        stream.truncate(64 * 1024 * 1024)
    subprocess.run(['mkfs.ext4', '-q', '-F', '-b', str(block_size), str(disk)], check=True)
    data = bytes((i * 17 + (i // 4096) * 31 + 7) & 255 for i in range(LENGTH))
    for name, payload in (('data', data), ('hole', data[:PAGES * 4096])):
        source = work / name
        source.write_bytes(payload)
        subprocess.run(['debugfs', '-w', '-R', f'write {source} /{name}', str(disk)], check=True, capture_output=True)
    subprocess.run(['debugfs', '-w', '-R', f'punch /hole {4 * 4096 // block_size} {12 * 4096 // block_size - 1}', str(disk)],
                   check=True, capture_output=True)
    maps = {}
    for name, size in (('data', LENGTH), ('hole', PAGES * 4096)):
        blocks = (size + block_size - 1) // block_size
        script = work / 'bmap.commands'
        script.write_text(''.join(f'bmap /{name} {i}\n' for i in range(blocks)))
        result = subprocess.run(['debugfs', '-f', str(script), str(disk)], check=True, capture_output=True, text=True)
        numbers = [int(line) for line in result.stdout.splitlines() if re.fullmatch(r'\d+', line)]
        if len(numbers) != blocks:
            raise RuntimeError(f'cannot map /{name}: {result.stdout}{result.stderr}')
        maps[name] = numbers
    if any(maps['data'][i] != maps['data'][0] + i for i in range(len(maps['data']))):
        raise RuntimeError('/data must occupy a contiguous extent for the two-request check')
    for i, block in enumerate(maps['hole']):
        if bool(block) == (4 * 4096 // block_size <= i < 12 * 4096 // block_size):
            raise RuntimeError('/hole allocation does not match its sparse range')
    return disk, maps


def sectors(mapping, block_size, pages):
    wanted = Counter()
    for page in pages:
        for logical in range(page * 4096 // block_size, (page + 1) * 4096 // block_size):
            if logical >= len(mapping):
                break
            block = mapping[logical]
            if block:
                wanted.update(range(block * block_size // 512, (block + 1) * block_size // 512))
    return wanted


def verify_requests(phase, requests, maps, block_size):
    actual = Counter()
    for request in requests:
        offset, size = request['offset'], request['size']
        if offset % 512 or size % 512 or not 0 < size <= 8 * 4096:
            raise RuntimeError(f'{phase}: invalid bounded READ {request}')
        actual.update(range(offset // 512, (offset + size) // 512))
    if phase in ('efault-prefix', 'efault-first'):
        # User access may fail after the first demand window has been loaded.
        allowed = sectors(maps['data'], block_size, range(PAGES))
        if actual - allowed or sum(actual.values()) > 8 * 4096 // 512:
            raise RuntimeError(f'{phase}: I/O continued past the first failing user-copy window: {requests}')
        return
    pages = (range(17) if phase == 'unaligned' else
             [i for i in range(PAGES) if i not in (1, 4, 7, 10, 13)] if phase == 'mixed' else
             (19, 20) if phase == 'eof' else () if phase in ('hot', 'at-eof') else
             (0, 1, 2, 3, 5, 6, 7) if phase == 'eio-gap' else
             range(8) if phase == 'eio-first' else range(PAGES))
    expected = sectors(maps['hole' if phase == 'hole' else 'data'], block_size, pages)
    if actual != expected:
        raise RuntimeError(f'{phase}: data READ coverage differs: {requests}; missing={expected - actual}, extra={actual - expected}')
    sizes = [request['size'] for request in requests]
    if phase in ('read', 'pread', 'readv', 'retry', 'eio-prefix', 'held-owner') and sizes != [32768, 32768]:
        raise RuntimeError(f'{phase}: ordinary 64KiB read did not form two 32KiB SG requests: {requests}')
    if phase == 'eio-first' and sizes != [32768]:
        raise RuntimeError(f'{phase}: I/O continued after its first failed batch: {requests}')
    if phase == 'eio-gap' and sizes != [16384, 12288]:
        raise RuntimeError(f'{phase}: a cached page did not divide the failed demand window: {requests}')
    if phase == 'unaligned' and sizes != [32768, 32768, 4096]:
        raise RuntimeError(f'{phase}: unexpected page window: {requests}')


def run(kernel, disk, work, maps, block_size, transport, qemu):
    address = work / 'nbd.sock'
    server = subprocess.Popen([str(ROOT / 'build/host/nbd-fault'), str(disk), str(address), '--control-stdin'],
                              stdin=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    guest = None
    selector = selectors.DefaultSelector()
    logs = {'guest': bytearray(), 'server': bytearray()}
    try:
        deadline = time.monotonic() + 10
        while not address.exists():
            if server.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError('NBD startup failed')
            time.sleep(.01)
        command = [qemu, '-machine', 'virt', '-bios', 'default', '-kernel', str(kernel),
                   '-global', f'virtio-mmio.force-legacy={"true" if transport == "legacy" else "false"}',
                   '-m', '128M', '-smp', '1', '-nographic', '-no-reboot',
                   '-drive', f'file=nbd+unix:///?socket={address},if=none,format=raw,id=root,cache=writeback',
                   '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0,request-merging=off,config-wce=off']
        guest = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        selector.register(guest.stdout, selectors.EVENT_READ, 'guest')
        selector.register(server.stderr, selectors.EVENT_READ, 'server')
        pending = {'guest': bytearray(), 'server': bytearray()}
        phase = None
        requests = []
        finished = []
        held = None
        owner_checked = False
        guest_eof = False
        injected = []
        deadline = time.monotonic() + 120
        while not guest_eof and time.monotonic() < deadline:
            for key, _ in selector.select(.5):
                label = key.data
                chunk = key.fileobj.read(4096)
                if not chunk:
                    selector.unregister(key.fileobj)
                    if label == 'guest':
                        guest_eof = True
                    elif not guest_eof and server.poll() not in (None, 0):
                        raise RuntimeError('NBD server failed')
                    continue
                logs[label].extend(chunk)
                pending[label].extend(chunk)
                while b'\n' in pending[label]:
                    line, _, rest = pending[label].partition(b'\n')
                    pending[label] = bytearray(rest)
                    line = bytes(line.rstrip(b'\r'))
                    if label == 'guest' and (line.startswith(b'demand-read failed:') or b'fatal supervisor' in line):
                        raise RuntimeError(line.decode(errors='replace'))
                    if label == 'guest' and line.startswith(b'demand-read begin '):
                        if phase is not None:
                            raise RuntimeError('nested demand-read phase')
                        phase = line.decode().split()[-1]
                        if phase != PHASES[len(finished)]:
                            raise RuntimeError(f'out of order phase {phase}')
                        requests = []
                        server.stdin.write(b'hold\n')
                    elif label == 'server' and line == b'control=hold':
                        guest.stdin.write(b'g')
                    elif label == 'server' and line.startswith(b'held='):
                        fields = {key.decode(): int(value, 0) for key, value in (item.split(b'=') for item in line.split())}
                        if phase is None:
                            raise RuntimeError(f'held reply outside a demand window: {line!r}')
                        identity = fields['held']
                        if fields['command'] == 0:
                            requests.append(fields)
                        if fields['command'] == 0 and phase == 'held-owner' and len(requests) == 1:
                            held = identity
                        elif fields['command'] == 0 and ((phase in ('eio-prefix', 'eio-gap') and len(requests) == 2) or phase == 'eio-first'):
                            server.stdin.write(f'read-error {identity}\n'.encode())
                            injected.append(phase)
                        else:
                            server.stdin.write(f'release {identity}\n'.encode())
                    elif label == 'guest' and line == b'demand-read held-owner':
                        if phase != 'held-owner' or held is None or owner_checked:
                            raise RuntimeError('owner check did not hold a live data READ')
                        owner_checked = True
                        server.stdin.write(f'release {held}\n'.encode())
                    elif label == 'guest' and line.startswith(b'demand-read end '):
                        if line.decode().split()[-1] != phase:
                            raise RuntimeError('mismatched demand-read phase')
                        verify_requests(phase, requests, maps, block_size)
                        print(f'PASS: demand-read {phase} ext4-block={block_size} transport={transport} '
                              f'READ-sizes={[item["size"] for item in requests]}', flush=True)
                        finished.append(phase)
                        server.stdin.write(b'drain\n')
                    elif label == 'server' and line == b'control=drain':
                        phase = None
                        guest.stdin.write(b'c')
        if not guest_eof:
            raise RuntimeError(f'demand-read deadline in {phase}; requests={requests}')
        if guest.wait(timeout=5) or list(PHASES) != finished or not owner_checked or injected != ['eio-prefix', 'eio-first', 'eio-gap']:
            raise RuntimeError(f'incomplete demand-read verification: phases={finished} owner={owner_checked} errors={injected}')
        if b'BoarOS: demand-read tests passed' not in logs['guest']:
            raise RuntimeError(logs['guest'][-5000:].decode(errors='replace'))
    finally:
        selector.close()
        for name, data in logs.items():
            (work / f'{name}.log').write_bytes(data)
        for process in (guest, server):
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, help='use this already-built RA0 kernel')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/demand-read')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--block-size', nargs='+', type=int, choices=(1024, 4096), default=(1024, 4096))
    parser.add_argument('--transport', nargs='+', choices=('legacy', 'modern'), default=('legacy', 'modern'))
    parser.add_argument('--qemu', default='qemu-system-riscv64')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--keep', action='store_true', help='retain successful run images and wire logs')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    kernel = args.kernel.resolve() if args.kernel else build / 'tests/kernel-demand-read-rv'
    if not args.kernel:
        command = ['make', '-f', 'tests/demand-read.mk', f'-j{args.jobs}',
                   f'BUILD_DIR={build}', f'KERNEL_RV={build}/kernel-rv', 'COST_DIAGNOSTICS=0',
                   'CFLAGS_EXTRA=-DBOAROS_PAGE_CACHE_READAHEAD_PAGES=0', str(kernel)]
        with (build / 'build.log').open('w') as log:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    subprocess.run(['make', 'build/host/nbd-fault'], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    if args.build_only:
        print(f'Built RA0 demand-read fixture: {kernel}')
        return
    for block_size in args.block_size:
        for transport in args.transport:
            work = Path(tempfile.mkdtemp(prefix=f'run-{block_size}-{transport}.', dir=build))
            try:
                disk, maps = image(work, block_size)
                run(kernel, disk, work, maps, block_size, transport, args.qemu)
                with (work / 'fsck.log').open('w') as log:
                    subprocess.run(['e2fsck', '-fn', str(disk)], stdout=log, stderr=subprocess.STDOUT, check=True)
            except BaseException:
                print(f'demand-read artifacts retained: {work}', flush=True)
                raise
            else:
                if args.keep:
                    print(f'demand-read artifacts retained: {work}', flush=True)
                else:
                    shutil.rmtree(work)


if __name__ == '__main__':
    main()
