#!/usr/bin/env python3
"""Exercise real block completion diagnostics with literal split-ring faults.

Only RISC-V fence/time instructions are translated for native execution. The
driver's queue validation, reset, request owner and diagnostic code are compiled
unchanged. This checks cold diagnostics, not DMA ordering or a real device.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "arch/riscv/virtio_mmio_block.c").read_text()
translations = {
    '__asm__ volatile("fence rw, rw" ::: "memory");':
        '__atomic_thread_fence(__ATOMIC_SEQ_CST);',
    '__asm__ volatile("fence iorw, iorw" ::: "memory");':
        '__atomic_thread_fence(__ATOMIC_SEQ_CST);',
    '__asm__ volatile("csrr %0, time" : "=r"(value));':
        'value = host_block_time();',
}
for instruction, native in translations.items():
    if source.count(instruction) != 1:
        raise RuntimeError(f"native translation needs review: {instruction}")
    source = source.replace(instruction, native)
source = '#include <stdint.h>\nuint64_t host_block_time(void);\n' + source
with tempfile.TemporaryDirectory(prefix="boaros-block-diagnostics-") as work:
    work = Path(work)
    (work / "virtio_mmio_block_native.c").write_text(source)
    exe = work / "test"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-DBOAROS_PAGE_SHIFT=12",
        *shlex.split(os.environ.get("CFLAGS", "")),
        "-Itests/host/random", "-idirafter", "include", "-I", str(work),
        "tests/host/virtio_block_diagnostics.c", "-o", str(exe),
    ], cwd=root, check=True)
    subprocess.run([str(exe), *os.sys.argv[1:]], check=True)
