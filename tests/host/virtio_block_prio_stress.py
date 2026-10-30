#!/usr/bin/env python3
"""Adversarial stress test runner for VirtIO block priority scheduler and descriptor invariants."""
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

with tempfile.TemporaryDirectory(prefix="boaros-prio-stress-") as work:
    work = Path(work)
    (work / "virtio_mmio_block_native.c").write_text(source)
    exe = work / "test_prio_stress"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-DBOAROS_PAGE_SHIFT=12", "-DBOAROS_MAX_CPUS=2",
        *(["-DHOST_ALTERNATE_TRANSPORT"] if os.environ.get("HOST_ALTERNATE_TRANSPORT") else []),
        *shlex.split(os.environ.get("CFLAGS", "")),
        "-Itests/host/random", "-idirafter", "include", "-I", str(work),
        "tests/host/virtio_block_prio_stress.c", "kernel/block.c",
        "tests/host/block_runtime.c", "-pthread",
        "drivers/virtio/transport.c", "drivers/virtio/split_queue.c",
        "kernel/raw_lock.c", "tests/host/sync/notifications.c",
        "-o", str(exe),
    ], cwd=root, check=True)
    subprocess.run([str(exe)], check=True)
