#!/usr/bin/env python3
"""PID 1 exits with independent file/MM/socket owners; shutdown must reap them."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/diff-abi"))
import harness


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--kernel", type=Path, default=ROOT / "kernel-rv")
    p.add_argument("--cases", default="daemon,stopped,threads,forking")
    args = p.parse_args()
    work = Path(tempfile.mkdtemp(prefix="root-shutdown.", dir=ROOT / "build"))
    program = work / "init"
    subprocess.run([str(ROOT / "build/riscv/musl-root/bin/musl-gcc"),
                    "-fno-link-libatomic", "-static", "-O2", "-pthread",
                    "-Wall", "-Wextra", "-Werror",
                    str(ROOT / "tests/riscv/root_shutdown.c"), "-o", str(program)], check=True)
    fixture = harness.fixture(work, program)
    for mode in args.cases.split(","):
        if mode not in ("daemon", "stopped", "threads", "forking"):
            p.error("unknown shutdown scenario")
        mode_file = work / "mode"
        mode_file.write_text(mode)
        disk = work / (mode + ".img")
        shutil.copyfile(fixture, disk)
        subprocess.run(["debugfs", "-w", "-R", f"write {mode_file} /mode", str(disk)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        log = work / (mode + ".log")
        command = [os.environ.get("QEMU_RISCV64", "qemu-system-riscv64"),
                   "-machine", "virt", "-bios", "default", "-kernel", str(args.kernel),
                   "-m", "512M", "-smp", "1", "-nographic", "-no-reboot",
                   "-drive", f"file={disk},if=none,format=raw,id=root",
                   "-device", "virtio-blk-device,drive=root,bus=virtio-mmio-bus.0"]
        with log.open("w") as stream:
            subprocess.run(command, stdin=subprocess.DEVNULL, stdout=stream,
                           stderr=subprocess.STDOUT, timeout=30, check=True)
        text = log.read_text()
        if (f"INIT SHUTDOWN READY {mode}" not in text or
                "PID 1 exited status=0x25" not in text or "heap-live=0x0; shutting down" not in text):
            raise RuntimeError(f"shutdown failed: {log}\n" + text[-1800:])
        print("PASS PID 1 shutdown:", mode, flush=True)
    print("shutdown records:", work)


if __name__ == "__main__":
    main()
