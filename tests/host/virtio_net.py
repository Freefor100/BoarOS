#!/usr/bin/env python3
"""Exercise the production driver against an independently decoded MMIO device."""
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command, root, timeout):
    try:
        result = subprocess.run(command, cwd=root, timeout=timeout)
    except subprocess.TimeoutExpired:
        print(f"VirtIO-net host model timed out after {timeout}s: {command[0]}", file=sys.stderr)
        raise SystemExit(1)
    if result.returncode:
        raise SystemExit(result.returncode if result.returncode > 0 else 1)


root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="boaros-virtio-net-") as work:
    exe = Path(work) / "net"
    for observe in (0, 1):
        run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             f"-DBOAROS_COST_DIAGNOSTICS={observe}",
             "-Itests/host/random", "-Iinclude", "tests/host/virtio_net_test.c",
             "arch/riscv/virtio_mmio_net.c", "-o", str(exe)], root, 30)
        run([str(exe)], root, 20)
