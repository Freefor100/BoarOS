#!/usr/bin/env python3
"""Run the same unmodified SQLite Unix VFS WAL workload on fixed Linux and BoarOS."""

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/diff-abi"))
import harness


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def boot(qemu, kernel, disk, output, linux, marker, second=None):
    command = [qemu, "-machine", "virt", "-bios", "default",
               "-kernel", str(kernel), "-m", "512M", "-smp", "1",
               "-nographic", "-no-reboot",
               "-drive", f"file={disk},if=none,format=raw,id=root",
               "-device", "virtio-blk-device,drive=root,bus=virtio-mmio-bus.0"]
    if second is not None:
        command += ["-drive", f"file={second},if=none,format=raw,id=second",
                    "-device", "virtio-blk-device,drive=second,bus=virtio-mmio-bus.1"]
    if linux:
        root_device = "/dev/vdb" if second is not None else "/dev/vda"
        command += ["-append", f"root={root_device} rw rootwait console=ttyS0 "
                    "init=/init loglevel=0 panic=-1"]
    with output.open("w") as log:
        result = subprocess.run(command, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT,
                                timeout=90)
    content = output.read_text(errors="replace")
    if result.returncode or marker not in content or (
            not linux and "PID 1 exited status=0x2a" not in content):
        raise AssertionError(f"SQLite WAL {kernel} failed: {output}\n"
                             + content[-5000:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-riscv64")
    parser.add_argument("--second-disk", action="store_true")
    args = parser.parse_args()
    linux_kernel, _ = harness.linux_build()
    identity = {
        "boaros_kernel_sha256": sha256(args.kernel),
        "linux_kernel_sha256": sha256(linux_kernel),
        "workload_sha256": sha256(args.program),
        "sqlite_archive_sha256": sha256(ROOT /
            "references/sqlite/sqlite-amalgamation-3530400.zip"),
        "qemu": subprocess.check_output([args.qemu, "--version"],
            text=True).splitlines()[0],
        "rebuild": "make test-sqlite-second-disk-riscv" if args.second_disk else "make test-sqlite-wal-riscv",
    }
    directory = Path(tempfile.mkdtemp(prefix="sqlite-wal-run.",
                                      dir=ROOT / "build/riscv"))
    try:
        disk = harness.fixture(directory, args.program)
        for name, kernel, linux in (("linux", linux_kernel, True),
                                    ("boaros", args.kernel.resolve(), False)):
            target = directory / (name + ".img")
            shutil.copy2(disk, target)
            second = None
            if args.second_disk:
                second = directory / (name + "-second.img")
                with second.open("wb") as stream:
                    stream.truncate(64 * 1024 * 1024)
                subprocess.run(["mkfs.ext4", "-q", "-F", "-b", "4096", str(second)], check=True)
            boot(args.qemu, kernel, target, directory / (name + ".log"),
                 linux, "BoarOS: SQLite WAL multiprocess passed", second)
            boot(args.qemu, kernel, target,
                 directory / (name + "-reboot.log"), linux,
                 "BoarOS: SQLite WAL reboot verified", second)
    except Exception:
        print(f"SQLite WAL artifacts retained: {directory}", file=sys.stderr)
        raise
    shutil.rmtree(directory)
    print("SQLite WAL inputs:", json.dumps(identity, sort_keys=True))
    print("SQLite WAL multiprocess and reboot passed on fixed Linux and BoarOS")


if __name__ == "__main__":
    main()
