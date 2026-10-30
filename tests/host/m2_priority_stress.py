#!/usr/bin/env python3
"""Milestone 2 Empirical VirtIO Priority & Slot Quota Stress Harness Runner."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/virtio/block.c").read_text() + '\n' + (root / "arch/riscv/virtio_mmio_block.c").read_text() + '\n' + (root / "drivers/virtio/mmio.c").read_text()
translations = {
    'arch_dma_barrier();': '__atomic_thread_fence(__ATOMIC_SEQ_CST);',
    'arch_io_barrier();': '__atomic_thread_fence(__ATOMIC_SEQ_CST);',
    'return arch_time_read();': 'return host_block_time();',
}
for instruction, native in translations.items():
    if source.count(instruction) != 1:
        raise RuntimeError(f"native translation needs review: {instruction}")
    source = source.replace(instruction, native)
source = '#include <stdint.h>\nuint64_t host_block_time(void);\n' + source

build_dir = root / "build/host"
build_dir.mkdir(parents=True, exist_ok=True)
(build_dir / "virtio_mmio_block_native.c").write_text(source)

exe = build_dir / "m2_priority_stress"

cmd = [
    os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-DBOAROS_PAGE_SHIFT=12", "-DBOAROS_MAX_CPUS=2",
    "-Itests/host/random", "-idirafter", "include", "-I", str(build_dir),
    "tests/host/m2_priority_stress.c", "kernel/block.c",
    "tests/host/block_runtime.c", "-pthread",
    "drivers/virtio/transport.c", "drivers/virtio/split_queue.c",
    "kernel/raw_lock.c", "tests/host/sync/notifications.c",
    "-Wl,--wrap=virtio_split_take,--wrap=kernel_raw_lock_acquire", "-o", str(exe),
]

print(f"Compiling test harness: {' '.join(cmd)}")
subprocess.run(cmd, cwd=root, check=True)

print(f"Running test harness: {exe}")
subprocess.run([str(exe)], cwd=root, check=True)
