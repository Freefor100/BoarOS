#!/usr/bin/env python3
"""Run bounded snapshot writeback against real ext4 and the cache worker."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--pages', nargs='+', type=int, choices=(1, 2, 4, 8), default=(1, 2, 4, 8))
parser.add_argument('--block-size', nargs='+', type=int, choices=(1024, 4096), default=(1024, 4096))
parser.add_argument('--qemu', default='qemu-system-riscv64')
parser.add_argument('--transport', choices=('legacy', 'modern'), default='modern')
parser.add_argument('--jobs', type=int, default=4)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
for pages in args.pages:
    build = root / 'build' / 'writeback-batch' / str(pages)
    build.mkdir(parents=True, exist_ok=True)
    kernel = build / 'tests' / 'kernel-writeback-batch-rv'
    command = ['make', '-f', 'tests/writeback-batch.mk', f'-j{args.jobs}',
               f'BUILD_DIR={build.relative_to(root)}', f'KERNEL_RV={build}/kernel-rv',
               f'CFLAGS_EXTRA=-DBOAROS_PAGE_CACHE_WRITEBACK_PAGES={pages}', str(kernel.relative_to(root))]
    with (build / 'build.log').open('w') as log:
        subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
    for block_size in args.block_size:
        work = Path(tempfile.mkdtemp(prefix=f'run-{block_size}.', dir=build))
        try:
            disk = work / 'root.img'
            with disk.open('wb') as stream:
                stream.truncate(64 * 1024 * 1024)
            subprocess.run(['mkfs.ext4', '-q', '-F', '-b', str(block_size), str(disk)], check=True)
            command = [args.qemu, '-machine', 'virt', '-bios', 'default', '-kernel', str(kernel),
                       '-m', '128M', '-smp', '1', '-nographic', '-no-reboot',
                       '-global', f'virtio-mmio.force-legacy={"true" if args.transport == "legacy" else "false"}',
                       '-drive', f'file={disk},if=none,format=raw,id=root,cache=writeback',
                       '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
            try:
                result = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True,
                                        text=True, timeout=120)
            except subprocess.TimeoutExpired as error:
                def text(value):
                    return value.decode(errors='replace') if isinstance(value, bytes) else value or ''
                (work / 'boot.log').write_text(text(error.stdout) + text(error.stderr))
                raise
            (work / 'boot.log').write_text(result.stdout + result.stderr)
            if result.returncode or 'BoarOS: writeback batch tests passed' not in result.stdout:
                raise RuntimeError(result.stdout[-5000:] + result.stderr)
            with (work / 'fsck.log').open('w') as log:
                subprocess.run(['e2fsck', '-fn', str(disk)], stdout=log, stderr=subprocess.STDOUT, check=True)
            print(f'PASS: writeback pages={pages} ext4-block={block_size} transport={args.transport}')
            print('\n'.join(line for line in result.stdout.splitlines() if line.startswith('writeback ')))
        except BaseException:
            print(f'writeback artifacts retained: {work}')
            raise
        else:
            shutil.rmtree(work)
