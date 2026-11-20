#!/usr/bin/env python3
"""Run static feature probes from a generated ext4 fixture."""
import argparse
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tests"), str(ROOT / "tests" / "diff-abi")]
from arch_profiles import PROFILES
import harness


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=PROFILES, default="riscv")
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--program", required=True, type=Path)
    parser.add_argument("--cpus", type=int, default=1)
    parser.add_argument("--qemu")
    parser.add_argument("--firmware", type=Path)
    parser.add_argument("--unpatched-la-qemu", action="store_true")
    parser.add_argument("--linux", action="store_true")
    parser.add_argument("--linux-root", default="/dev/vda")
    parser.add_argument("--expect", action="append", default=[])
    parser.add_argument("--reject", action="append", default=[])
    scratch_input = parser.add_mutually_exclusive_group()
    scratch_input.add_argument("--scratch-mib", type=int, default=0)
    scratch_input.add_argument("--scratch-image", type=Path)
    parser.add_argument("--host-cpu", type=int)
    parser.add_argument("--file", action="append", default=[], metavar="/GUEST/PATH=HOST_PATH")
    args = parser.parse_args()
    profile = PROFILES[args.arch]
    building = ROOT / "build"
    building.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="linux-abi-wave1-", dir=building))
    disk = harness.fixture(work, args.program.resolve())
    directories = {"/", "/dev"}
    for mapping in args.file:
        guest, separator, source = mapping.partition("=")
        guest = Path(guest)
        if not separator or not guest.is_absolute() or ".." in guest.parts:
            parser.error("--file requires /GUEST/PATH=HOST_PATH")
        for parent in reversed(guest.parents):
            if str(parent) in directories:
                continue
            harness.run_logged(["debugfs", "-w", "-R", f'mkdir "{parent}"', str(disk)],
                               work / "fixture-directory.log")
            directories.add(str(parent))
        host = Path(source).resolve(strict=True)
        if any(char in str(host) or char in str(guest) for char in ('"', "\n", "\r")):
            parser.error("unsupported quote or newline in fixture file path")
        harness.run_logged(["debugfs", "-w", "-R", f'write "{host}" "{guest}"', str(disk)],
                           work / "fixture-file.log")
    qemu_var = "QEMU_RISCV64" if args.arch == "riscv" else "QEMU_LOONGARCH64"
    qemu = args.qemu or os.environ.get(qemu_var, profile.qemu)
    command = profile.boot(qemu, args.kernel.resolve(), "512M")
    command[command.index("-smp") + 1] = str(args.cpus)
    if args.firmware:
        command[command.index("-bios") + 1] = str(args.firmware.resolve())
    if args.arch == "loongarch" and args.unpatched_la_qemu:
        index = command.index("-global")
        del command[index:index + 2]
    command += ["-net", "none", "-drive", f"file={disk},if=none,format=raw,id=root",
                "-device", profile.block("modern")]
    if args.scratch_mib or args.scratch_image:
        scratch = work / "scratch.img"
        if args.scratch_image:
            shutil.copyfile(args.scratch_image, scratch)
        else:
            with scratch.open("wb") as image:
                image.truncate(args.scratch_mib * 1024 * 1024)
            harness.run_logged(["mkfs.ext4", "-q", "-F", "-b", "4096", str(scratch)],
                               work / "scratch-mkfs.log")
        block = ("virtio-blk-device,drive=scratch,bus=virtio-mmio-bus.1"
                 if args.arch == "riscv"
                 else "virtio-blk-pci,drive=scratch,addr=2,disable-legacy=on")
        command += ["-drive", f"file={scratch},if=none,format=raw,id=scratch",
                    "-device", block]
    if args.arch == "riscv":
        command += ["-global", "virtio-mmio.force-legacy=false"]
    if args.linux:
        command += ["-append", f"root={args.linux_root} rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1"]
    if args.host_cpu is not None:
        command = ["taskset", "-c", str(args.host_cpu), *command]
    identity = {
        "arch": args.arch,
        "command": command,
        "qemu_version": harness.output([qemu, "--version"]).splitlines()[0],
        "kernel_sha256": harness.digest(args.kernel),
        "program_sha256": harness.digest(args.program),
        "fixture_sha256": harness.digest(disk),
    }
    if args.scratch_mib or args.scratch_image:
        identity["scratch_sha256"] = harness.digest(scratch)
    (work / "identity.json").write_text(json.dumps(identity, indent=2) + "\n")
    logfile = work / "guest.log"
    print(logfile, flush=True)
    harness.run_logged(command, logfile, timeout=90)
    log = logfile.read_text(errors="replace")
    missing = [marker for marker in args.expect if marker not in log]
    rejected = [marker for marker in args.reject if marker in log]
    if missing or rejected:
        raise SystemExit(f"missing {missing}, rejected {rejected}: {logfile}")
    if not args.linux and not (profile.root_poweroff_success(log) or profile.root_success(log, 0)):
        raise SystemExit(f"incomplete BoarOS shutdown: {logfile}")
    harness.run_logged(["e2fsck", "-fn", str(disk)], work / "fsck.log")
    disk.unlink()
    if args.scratch_mib or args.scratch_image:
        harness.run_logged(["e2fsck", "-fn", str(scratch)], work / "scratch-fsck.log")
        scratch.unlink()


if __name__ == "__main__":
    main()
