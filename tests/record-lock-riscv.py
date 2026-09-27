#!/usr/bin/env python3
"""Run one unmodified musl lock lifecycle ELF on fixed Linux and BoarOS."""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def command(*arguments):
    return subprocess.run(arguments, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True)


def boot(args, kernel, disk, output, linux):
    invocation = [args.qemu, "-machine", "virt", "-bios", "default",
                  "-kernel", str(kernel), "-m", "512M", "-smp", "1",
                  "-nographic", "-no-reboot", "-drive",
                  f"file={disk},if=none,format=raw,readonly=off,id=root",
                  "-device", "virtio-blk-device,drive=root,bus=virtio-mmio-bus.0"]
    if linux:
        invocation += ["-append", "root=/dev/vda rw rootwait console=ttyS0 "
                       "init=/init loglevel=0 panic=-1"]
    with output.open("w") as log:
        result = subprocess.run(invocation, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT,
                                timeout=60)
    text = output.read_text(errors="replace")
    if result.returncode or "BoarOS: record lock lifecycle passed" not in text:
        raise AssertionError(f"{output}: {text[-4000:]}")
    if not linux and not re.search(
            r"BoarOS: PID 1 exited status=0x2a pages=0x[1-9a-f][0-9a-f]* "
            r"heap-live=0x0; shutting down", text):
        raise AssertionError(f"missing resource baseline: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-riscv64")
    args = parser.parse_args()
    sys.path.insert(0, str(ROOT / "tests/diff-abi"))
    import harness
    linux_kernel, _ = harness.linux_build()
    directory = Path(tempfile.mkdtemp(prefix="record-lock-run.",
                                      dir=ROOT / "build/riscv"))
    try:
        tree = directory / "tree"
        tree.mkdir()
        shutil.copyfile(args.program, tree / "init")
        (tree / "init").chmod(0o755)
        (tree / "dev").mkdir()
        disk = directory / "fixture.img"
        command("truncate", "-s", "32M", str(disk))
        command("mkfs.ext4", "-q", "-F", "-b", "4096", "-d", str(tree),
                str(disk))
        instructions = directory / "devices.debugfs"
        instructions.write_text("cd /dev\nmknod console c 5 1\n"
                                "set_inode_field console mode 020600\n")
        command("debugfs", "-w", "-f", str(instructions), str(disk))
        outcomes = []
        for name, kernel, linux in (("linux", linux_kernel, True),
                                    ("boaros", args.kernel, False)):
            target = directory / f"{name}.img"
            shutil.copyfile(disk, target)
            output = directory / f"{name}.log"
            boot(args, kernel, target, output, linux)
            match = re.search(r"BoarOS: traditional fd reuse result=-?\d+ "
                              r"errno=\d+", output.read_text())
            assert match, output
            outcomes.append(match.group(0))
        assert outcomes[0] == outcomes[1], outcomes
        print("record lock pthread/fork/fd reuse/forced exit matched fixed Linux")
    except Exception:
        print(f"record lock artifacts retained: {directory}", file=sys.stderr)
        raise
    else:
        shutil.rmtree(directory)


if __name__ == "__main__":
    main()
