#!/usr/bin/env python3
"""Portable split-ring lifecycle simulation, not entropy-quality validation."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='boaros-virtio-rng-') as work:
    exe = str(Path(work)/'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-DBOAROS_PAGE_SHIFT=12',
                    '-Itests/host/random', '-Iinclude', 'tests/host/virtio_rng_test.c',
                    'arch/riscv/virtio_mmio_rng.c', 'drivers/virtio/rng.c',
                    'drivers/virtio/mmio.c', 'drivers/virtio/transport.c',
                    'drivers/virtio/split_queue.c', '-o', exe], cwd=root, check=True)
    subprocess.run([exe], check=True)
    subprocess.run([exe,'--generic'],check=True)
