#!/usr/bin/env python3
"""Verify bounded readahead, cancellation and held DMA through real ext4."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import selectors
import time

parser = argparse.ArgumentParser()
parser.add_argument('--pages', nargs='+', type=int, choices=(0, 1, 2, 4, 8), default=(0, 1, 2, 4, 8))
parser.add_argument('--block-size', nargs='+', type=int, choices=(1024, 4096), default=(1024, 4096))
parser.add_argument('--transport', choices=('legacy','modern'), default='modern')
parser.add_argument('--qemu', default='qemu-system-riscv64')
parser.add_argument('--held',action='store_true',help='hold all prefetch READ replies; requires nonzero pages and 4096-byte ext4')
parser.add_argument('--jobs', type=int, default=4)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
if args.held and (0 in args.pages or args.block_size != [4096]): parser.error('--held requires --pages >0 --block-size 4096')

def held_run(command, disk, work, pages):
    """Release no READ until the entire window exists, then keep one DMA held."""
    subprocess.run(['make', 'build/host/nbd-fault'], cwd=root, check=True, stdout=subprocess.DEVNULL)
    address = work / 'nbd.sock'
    server = subprocess.Popen([str(root/'build/host/nbd-fault'), str(disk), str(address), '--control-stdin'],
                              stdin=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    guest = None
    logs = {'guest': bytearray(), 'server': bytearray()}
    try:
        deadline = time.monotonic()+10
        while not address.exists():
            if server.poll() is not None or time.monotonic()>deadline: raise RuntimeError('NBD startup failed')
            time.sleep(.01)
        command = list(command)
        drive = command.index('-drive')+1
        command[drive] = f'file=nbd+unix:///?socket={address},if=none,format=raw,id=root,cache=writeback'
        command[-1] += ',request-merging=off,config-wce=off'
        guest = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        selector = selectors.DefaultSelector()
        selector.register(guest.stdout,selectors.EVENT_READ,'guest')
        selector.register(server.stderr,selectors.EVENT_READ,'server')
        pending = {'guest':bytearray(),'server':bytearray()}
        held=[]; consumer=False; released=False; verified=False; guest_eof=False
        deadline=time.monotonic()+40
        while not guest_eof and time.monotonic()<deadline:
            for key,_ in selector.select(.5):
                chunk=key.fileobj.read(4096); label=key.data
                if not chunk:
                    selector.unregister(key.fileobj)
                    if label=='guest': guest_eof=True
                    continue
                logs[label].extend(chunk);pending[label].extend(chunk)
                while b'\n' in pending[label]:
                    line,_,rest=pending[label].partition(b'\n');pending[label]=bytearray(rest);line=bytes(line.rstrip(b'\r'))
                    if label=='guest' and line.startswith(b'readahead failed:'): raise RuntimeError(line.decode())
                    if label=='guest' and line==b'readahead: ready': server.stdin.write(b'hold\n')
                    elif label=='server' and line==b'control=hold': guest.stdin.write(b'g')
                    elif label=='server' and line.startswith(b'held='):
                        fields=dict(item.split(b'=') for item in line.split())
                        if int(fields[b'command'],0)!=0: raise RuntimeError('unexpected held command '+line.decode())
                        held.append(int(fields[b'held'],0))
                        if len(held)>pages: raise RuntimeError('prefetch exceeded window')
                    elif label=='guest' and line==b'readahead: pending': consumer=True
                    elif label=='guest' and line==b'readahead: held-owner':
                        if not released or len(held)!=pages: raise RuntimeError('missing held prefix')
                        server.stdin.write(f'release {held[0]}\ndrain\n'.encode());verified=True
                    if len(held)==pages and consumer and not released:
                        for identity in reversed(held[1:]): server.stdin.write(f'release {identity}\n'.encode())
                        released=True;guest.stdin.write(b'q')
        if not guest_eof or not verified: raise RuntimeError(f'held read deadline: reads={len(held)} consumer={consumer} owner={verified}')
        return subprocess.CompletedProcess(command,guest.wait(timeout=5),logs['guest'].decode(errors='replace'),'')
    finally:
        for name,data in logs.items(): (work/(name+'.log')).write_bytes(data)
        for process in (guest,server):
            if process is not None and process.poll() is None:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: process.kill();process.wait()

for pages in args.pages:
    build = root / 'build' / 'readahead' / (str(pages)+('-held' if args.held else ''))
    build.mkdir(parents=True, exist_ok=True)
    kernel = build / 'tests' / 'kernel-readahead-rv'
    command = ['make', '-f', 'tests/readahead.mk', f'-j{args.jobs}',
               f'BUILD_DIR={build.relative_to(root)}', f'KERNEL_RV={build}/kernel-rv',
               f'CFLAGS_EXTRA=-DBOAROS_PAGE_CACHE_READAHEAD_PAGES={pages} -DBOAROS_TEST_READAHEAD_HELD={int(args.held)}', str(kernel.relative_to(root))]
    with (build / 'build.log').open('w') as log:
        subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
    for block_size in args.block_size:
        work = Path(tempfile.mkdtemp(prefix=f'run-{block_size}.', dir=build))
        try:
            disk = work / 'root.img'
            with disk.open('wb') as stream:
                stream.truncate(64 * 1024 * 1024)
            subprocess.run(['mkfs.ext4', '-q', '-F', '-b', str(block_size), str(disk)], check=True)
            source = work / 'data'
            source.write_bytes(b''.join(bytes([i+1])*4096 for i in range(64)))
            subprocess.run(['debugfs','-w','-R',f'write {source} /data',str(disk)],check=True,capture_output=True)
            command = [args.qemu, '-machine', 'virt', '-bios', 'default', '-kernel', str(kernel),
                       '-global','virtio-mmio.force-legacy='+('true' if args.transport=='legacy' else 'false'),
                       '-m', '128M', '-smp', '1', '-nographic', '-no-reboot',
                       '-drive', f'file={disk},if=none,format=raw,id=root,cache=writeback',
                       '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
            try:
                result = held_run(command,disk,work,pages) if args.held else subprocess.run(command,
                    stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120)
            except subprocess.TimeoutExpired as error:
                def text(value):
                    return value.decode(errors='replace') if isinstance(value, bytes) else value or ''
                (work / 'boot.log').write_text(text(error.stdout) + text(error.stderr))
                raise
            (work / 'boot.log').write_text(result.stdout + result.stderr)
            if result.returncode or 'BoarOS: readahead tests passed' not in result.stdout:
                raise RuntimeError(result.stdout[-5000:] + result.stderr)
            with (work / 'fsck.log').open('w') as log:
                subprocess.run(['e2fsck', '-fn', str(disk)], stdout=log, stderr=subprocess.STDOUT, check=True)
            print(f'PASS: readahead pages={pages} ext4-block={block_size}')
            print('\n'.join(line for line in result.stdout.splitlines() if line.startswith('readahead ')))
        except BaseException:
            print(f'readahead artifacts retained: {work}')
            raise
        else:
            shutil.rmtree(work)
