#!/usr/bin/env python3
"""Exercise the production driver against an independently decoded MMIO device."""
import argparse
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


parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sanitize",action="store_true")
args=parser.parse_args()
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="boaros-virtio-net-") as work:
    exe = Path(work) / "net"
    for page_shift, observe in ((12, 0), (12, 1), (14, 0), (14, 1)):
        command=["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             f"-DBOAROS_COST_DIAGNOSTICS={observe}", f"-DBOAROS_PAGE_SHIFT={page_shift}",
             "-Itests/host/random", "-Iinclude", "tests/host/virtio_net_test.c",
             "arch/riscv/virtio_mmio_net.c", "drivers/virtio/net.c", "drivers/virtio/transport.c",
             "drivers/virtio/split_queue.c", "drivers/virtio/mmio.c", "-o", str(exe)]
        if args.sanitize:command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        run(command,root,30)
        run([str(exe)], root, 20)
