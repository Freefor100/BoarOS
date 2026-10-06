#!/usr/bin/env python3
"""Exercise real block completion diagnostics with literal split-ring faults.

Only RISC-V fence/time instructions are translated for native execution. The
driver's queue validation, reset, request owner and diagnostic code are compiled
unchanged. This checks cold diagnostics, not DMA ordering or a real device.
"""
from pathlib import Path
import os
import shlex
import shutil
import re
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
    # 宿主 x86 可把 packed 半字当成一次访问；必须核对实际 RV64 DMA 访问宽度。
    cross = next((p for p in ("riscv64-elf-", "riscv64-unknown-elf-")
                  if shutil.which(p + "gcc")), None)
    if cross is None:
        raise RuntimeError("RV64 compiler required to verify shared ring index accesses")
    access = work / "ring-access.c"
    access.write_text('#include "' + str(root / "arch/riscv/virtio_mmio_block.c") + '"\n' + r'''
uint16_t wire_available_read(volatile struct virtq_available *ring) { return ring->index; }
void wire_available_write(volatile struct virtq_available *ring, uint16_t value) { ring->index = value; }
uint16_t wire_used_read(volatile struct virtq_used *ring) { return ring->index; }
''')
    obj = work / "ring-access.o"
    subprocess.run([cross + "gcc", "-O2", "-ffreestanding", "-fno-builtin",
        "-march=rv64imac_zicsr_zifencei", "-mabi=lp64", "-mcmodel=medany",
        "-DBOAROS_PAGE_SHIFT=12", "-Iinclude", "-c", str(access), "-o", str(obj)],
        cwd=root, check=True)
    for function, instruction in (("wire_available_read", "lhu"),
                                  ("wire_available_write", "sh"),
                                  ("wire_used_read", "lhu")):
        assembly = subprocess.check_output([cross + "objdump", "-d",
            "--disassemble=" + function, str(obj)], text=True)
        if re.search(r"\b(?:sb|lbu|lb)\s", assembly) or not re.search(r"\b" + instruction + r"\s", assembly):
            raise AssertionError(f"{function}: split-ring index needs one aligned halfword access; "
                "bytewise 0x00ff -> 0x0100 can expose 0x0000 to the device\n" + assembly)
    print("PASS RV64 split-ring index access width: aligned halfword publication and snapshots", flush=True)
    (work / "virtio_mmio_block_native.c").write_text(source)
    exe = work / "test"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-DBOAROS_PAGE_SHIFT=12",
        *shlex.split(os.environ.get("CFLAGS", "")),
        "-Itests/host/random", "-idirafter", "include", "-I", str(work),
        "tests/host/virtio_block_diagnostics.c", "kernel/block.c", "-o", str(exe),
    ], cwd=root, check=True)
    subprocess.run([str(exe), *os.sys.argv[1:]], check=True)
