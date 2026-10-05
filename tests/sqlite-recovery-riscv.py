#!/usr/bin/env python3
"""Recover SQLite rollback transactions from NBD's stable image."""

import argparse
from datetime import datetime, timedelta
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]


def command(*arguments):
    return subprocess.run(arguments, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def put_file(image, source, target, executable=False):
    command("debugfs", "-w", "-R", f"write {source} {target}", str(image))
    if executable:
        command("debugfs", "-w", "-R",
                f"set_inode_field {target} mode 0100755", str(image))


def block_number(image, path):
    result = command("debugfs", "-R", f"bmap {path} 0", str(image))
    match = re.search(r"(?m)^([0-9]+)\s*$", result.stdout)
    if not match:
        raise AssertionError(f"no allocated block for {path}: {result.stdout}")
    return int(match.group(1))


def set_control(image, block, value):
    with open(image, "r+b", buffering=0) as file:
        file.seek(block * 4096)
        file.write(value.encode())
        os.fsync(file.fileno())


def qemu_command(args, image, socket=None, linux=False):
    # 每次重启的 RTC 明确晚于前一轮 inode 时间，避免宿主时间成为隐式 fixture 输入。
    boot = getattr(args, "rtc_boot", 0)
    args.rtc_boot = boot + 1
    rtc = datetime(2030, 1, 1) + timedelta(hours=boot)
    drive = (f"nbd:unix:{socket}" if socket else str(image))
    result = [args.qemu, "-machine", "virt", "-bios", "default",
                       "-object", "rng-random,id=entropy,filename=/dev/urandom",
                       "-device", "virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.7",
            "-kernel", str(args.linux_kernel if linux else args.kernel),
            "-m", "512M", "-smp", "1",
            "-rtc", f"base={rtc.isoformat()},clock=vm",
            "-nographic", "-no-reboot", "-drive",
            f"file={drive},if=none,format=raw,readonly=off,id=root,cache=writeback",
            "-device", "virtio-blk-device,drive=root,bus=virtio-mmio-bus.0"]
    if linux:
        result += ["-append", "root=/dev/vda rw rootwait console=ttyS0 "
                   "init=/init loglevel=0 panic=-1"]
    return result


def direct_boot(args, image, output, marker, linux=False):
    with open(output, "w") as log:
        result = subprocess.run(qemu_command(args, image, linux=linux),
                                stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT, timeout=60)
    content = output.read_text(errors="replace")
    if (result.returncode or marker not in content or
            (not linux and "PID 1 exited status=0x2a" not in content)):
        raise AssertionError(f"direct boot failed: {output}\n{content[-5000:]}")
    if args.journal == "wal" and "BoarOS: SQLite journal=wal" not in content:
        raise AssertionError(f"WAL mode not confirmed: {output}\n{content[-5000:]}")
    return content


def wait_for_marker(process, output, marker, timeout=60):
    start = time.monotonic()
    while time.monotonic() - start < timeout:
        if output.exists() and marker in output.read_text(errors="replace"):
            return
        if process.poll() is not None:
            break
        time.sleep(0.02)
    raise AssertionError(f"guest did not reach {marker}: {output}")


def nbd_boot(args, image, directory, name, *, options=(), marker=None,
             cut=False, expect_failure=False, gate=False):
    socket = directory / f"{name}.sock"
    backend_log = directory / f"{name}.nbd.log"
    guest_log = directory / f"{name}.guest.log"
    with open(backend_log, "w") as backend_output, \
         open(guest_log, "w") as guest_output:
        backend = subprocess.Popen([str(args.server), str(image), str(socket),
                                    *options, "--control-stdin"],
                                   stdin=subprocess.PIPE,
                                   stdout=backend_output, stderr=subprocess.STDOUT)
        guest = None
        controlled_cut = False
        def request_cut():
            nonlocal controlled_cut
            # Killing the client can interrupt a valid reply or WRITE payload.
            # Cut the backend at its request boundary and require its explicit
            # acknowledgement before terminating QEMU.
            backend.stdin.write(b"cut\n")
            backend.stdin.flush()
            if backend.wait(timeout=10) or "cause=control" not in backend_log.read_text(errors="replace"):
                raise AssertionError("NBD controlled cut not acknowledged")
            controlled_cut = True
        try:
            for _ in range(200):
                if socket.exists():
                    break
                if backend.poll() is not None:
                    raise AssertionError("NBD backend exited before listen")
                time.sleep(0.01)
            else:
                raise AssertionError("NBD backend did not listen")
            guest = subprocess.Popen(qemu_command(args, image, socket),
                                     stdin=subprocess.PIPE if gate else
                                           subprocess.DEVNULL,
                                     stdout=guest_output,
                                     stderr=subprocess.STDOUT)
            if gate:
                wait_for_marker(guest, guest_log,
                                "BoarOS: SQLite mutation armed")
                backend.stdin.write(b"arm\n")
                backend.stdin.flush()
                wait_for_marker(backend, backend_log, "control=arm")
                # Console input is canonical on both ordinary Linux and the
                # serial TTY; a complete line releases the one-byte guest read.
                guest.stdin.write(b"g\n")
                guest.stdin.flush()
            if marker:
                wait_for_marker(guest, guest_log, marker)
                request_cut()
                guest.kill()
                guest.wait(timeout=5)
            elif cut or (gate and expect_failure):
                start = time.monotonic()
                quiet_since = start
                previous = None
                while backend.poll() is None and guest.poll() is None:
                    text = backend_log.read_text(errors="replace")
                    if cut and re.search(r"(?m)^cut=", text):
                        guest.kill()
                        guest.wait(timeout=5)
                        break
                    if text != previous:
                        previous, quiet_since = text, time.monotonic()
                    committed = "BoarOS: SQLite commit confirmed" in guest_log.read_text(errors="replace")
                    faulted = "result=5" in text
                    # Commit can return before checkpoint, and group merging
                    # changes later event ordinals. An unreachable ordinal is
                    # a distinct post-commit power cut, never a claimed fault.
                    if (committed or faulted) and time.monotonic() - quiet_since >= 0.2:
                        request_cut()
                        break
                    if time.monotonic() - start >= 60:
                        raise AssertionError("mutation neither progressed nor reached commit/fault")
                    time.sleep(0.02)
                if guest.poll() is None:
                    guest.kill()
                guest.wait(timeout=5)
            else:
                code = guest.wait(timeout=90)
                if code:
                    raise AssertionError("QEMU exited unsuccessfully")
            if backend.wait(timeout=10):
                raise AssertionError(f"NBD backend exited unsuccessfully: {backend.returncode}; {backend_log}")
        finally:
            if guest and guest.poll() is None:
                guest.kill()
                guest.wait()
            if backend.poll() is None:
                backend.kill()
                backend.wait()
            (directory / f"{name}.exit.json").write_text(json.dumps({
                "guest": guest.returncode if guest else None,
                "backend": backend.returncode, "controlled_cut": controlled_cut,
                "marker": marker, "ordinal_cut": cut, "expect_failure": expect_failure,
            }, indent=2) + "\n")
    guest_text = guest_log.read_text(errors="replace")
    backend_text = backend_log.read_text(errors="replace")
    if args.journal == "wal" and "BoarOS: SQLite journal=wal" not in guest_text:
        raise AssertionError(f"WAL mode not confirmed: {guest_log}\n"
                             + guest_text[-3000:])
    if cut:
        assert "cut=" in backend_text, backend_text[-3000:]
    elif marker is None:
        if expect_failure:
            assert "result=5" in backend_text or (
                "BoarOS: SQLite commit confirmed" in guest_text and
                "cause=control" in backend_text), backend_text[-3000:]
        else:
            assert "PID 1 exited status=0x2a" in guest_text, guest_text[-3000:]
    return guest_text, backend_text


def recover_twice(args, image, directory, phase_block, name, expected=None):
    set_control(image, phase_block, "R")
    for run in (1, 2):
        content = direct_boot(args, image, directory / f"{name}.recover{run}.log",
                              "BoarOS: SQLite recovery state=")
        state = re.search(r"BoarOS: SQLite recovery state=(old|new)", content)
        assert state, content[-3000:]
        if expected:
            assert state.group(1) == expected, content[-3000:]
        command("e2fsck", "-fn", str(image))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=ROOT / "kernel-rv")
    parser.add_argument("--program", type=Path,
                        default=ROOT / "build/riscv/tests/user/sqlite-recovery-rv")
    parser.add_argument("--server", type=Path,
                        default=ROOT / "build/host/nbd-fault")
    parser.add_argument("--qemu", default="qemu-system-riscv64")
    parser.add_argument("--matrix", choices=("sample", "full"))
    parser.add_argument("--journal", choices=("delete", "wal"),
                        default="delete")
    parser.add_argument("--linux", action="store_true",
                        help="run the same ELF under fixed Linux too")
    args = parser.parse_args()
    args.kernel = args.kernel.resolve()
    args.program = args.program.resolve()
    args.server = args.server.resolve()
    if args.linux:
        sys.path.insert(0, str(ROOT / "tests/diff-abi"))
        import harness
        args.linux_kernel, _ = harness.linux_build()
    directory = Path(tempfile.mkdtemp(prefix="sqlite-recovery-run.",
                                      dir=ROOT / "build/riscv"))
    try:
        original = (args.kernel, args.program, args.server)
        snapshots = (directory / "kernel-rv", directory / "sqlite-recovery-rv",
                     directory / "nbd-fault")
        for source, destination in zip(original, snapshots):
            shutil.copy2(source, destination)
        args.kernel, args.program, args.server = snapshots
        identity = {
            "inputs": {str(source): sha256(destination)
                       for source, destination in zip(original, snapshots)},
            "qemu": command(args.qemu, "--version").stdout.splitlines()[0],
            "sqlite_archive_sha256": sha256(ROOT /
                "references/sqlite/sqlite-amalgamation-3530400.zip"),
            "journal": args.journal,
            "rtc": {"start": "2030-01-01T00:00:00", "step_hours": 1, "clock": "vm"},
            "runner_sha256": sha256(Path(__file__)),
            "rebuild": ("make test-sqlite-wal-recovery-matrix-riscv"
                        if args.journal == "wal" and args.matrix else
                        "make test-sqlite-wal-recovery-riscv"
                        if args.journal == "wal" else
                        "make test-sqlite-recovery-matrix-riscv"
                        if args.matrix else "make test-sqlite-recovery-riscv"),
        }
        if args.linux:
            identity["linux_kernel_sha256"] = sha256(args.linux_kernel)
        (directory / "identity.json").write_text(
            json.dumps(identity, indent=2) + "\n")
        print("SQLite recovery inputs:", json.dumps(identity), flush=True)
        base = directory / "baseline.img"
        command("truncate", "-s", "64M", str(base))
        command("mkfs.ext4", "-q", "-F", "-b", "4096", str(base))
        put_file(base, args.program, "/init", executable=True)
        commands = directory / "devices.debugfs"
        commands.write_text("mkdir /dev\ncd /dev\n"
                            "mknod console c 5 1\n"
                            "set_inode_field console mode 020600\n")
        command("debugfs", "-w", "-f", str(commands), str(base))
        for name, value in (("phase", "S"), ("sync", "E"),
                            ("size", "2"),
                            ("journal", "W" if args.journal == "wal" else "D")):
            source = directory / name
            source.write_text(value)
            put_file(base, source, "/" + name)
        phase_block = block_number(base, "/phase")
        sync_block = block_number(base, "/sync")
        if args.linux:
            linux_image = directory / "linux.img"
            shutil.copyfile(base, linux_image)
            direct_boot(args, linux_image, directory / "linux-setup.log",
                        "BoarOS: SQLite recovery setup complete", linux=True)
            set_control(linux_image, phase_block, "M")
            direct_boot(args, linux_image, directory / "linux-mutate.log",
                        "BoarOS: SQLite commit confirmed", linux=True)
            set_control(linux_image, phase_block, "R")
            linux_result = direct_boot(args, linux_image,
                                       directory / "linux-recover.log",
                                       "BoarOS: SQLite recovery state=new",
                                       linux=True)
            assert "BoarOS: SQLite recovery state=new" in linux_result
            print("same SQLite ELF passed fixed Linux setup/mutate/recover",
                  flush=True)
        direct_boot(args, base, directory / "setup.log",
                    "BoarOS: SQLite recovery setup complete")
        for name, phase, sync, stop, expected in (
            ("extra-normal", "M", "E", None, "new"),
            ("full-normal", "M", "F", None, "new"),
            ("hot", "H", "E", "BoarOS: SQLite hot transaction ready", "old"),
            ("confirmed", "C", "E", "BoarOS: SQLite commit confirmed", "new"),
        ):
            image = directory / f"{name}.img"
            shutil.copyfile(base, image)
            set_control(image, phase_block, phase)
            set_control(image, sync_block, sync)
            guest, events = nbd_boot(args, image, directory, name, marker=stop)
            if not stop:
                assert "BoarOS: SQLite commit confirmed" in guest
            recover_twice(args, image, directory, phase_block, name, expected)
            print(f"{name}: {events.count('type=WRITE')} writes, "
                  f"{events.count('type=FLUSH')} flushes, {expected}", flush=True)
        for fault in ("write", "flush"):
            name = f"full-fail-{fault}"
            image = directory / f"{name}.img"
            shutil.copyfile(base, image)
            set_control(image, phase_block, "G")
            set_control(image, sync_block, "F")
            guest, _ = nbd_boot(args, image, directory, name, gate=True,
                                expect_failure=True,
                                options=(f"--fail-{fault}=1",
                                         "--arm-on-signal"))
            assert "SQLite recovery" in guest and \
                   "BoarOS: SQLite commit confirmed" not in guest, guest[-3000:]
            recover_twice(args, image, directory, phase_block, name)
            print(f"{name}: error propagated and recovery passed", flush=True)
        if args.matrix:
            small = directory / "small-baseline.img"
            command("truncate", "-s", "64M", str(small))
            command("mkfs.ext4", "-q", "-F", "-b", "4096", str(small))
            put_file(small, args.program, "/init", executable=True)
            for control, value in (("phase", "S"), ("sync", "E"),
                                   ("size", "1"),
                                   ("journal", "W" if args.journal == "wal" else "D")):
                source = directory / control
                source.write_text(value)
                put_file(small, source, "/" + control)
            small_phase = block_number(small, "/phase")
            direct_boot(args, small, directory / "small-setup.log",
                        "BoarOS: SQLite recovery setup complete")
            probe = directory / "small-probe.img"
            shutil.copyfile(small, probe)
            set_control(probe, small_phase, "G")
            _, event_log = nbd_boot(args, probe, directory, "small-probe",
                                    options=("--arm-on-signal",), gate=True,
                                    marker="BoarOS: SQLite commit confirmed")
            assert "armed=1\n" in event_log
            event_log = event_log.split("armed=1\n", 1)[1]
            events = event_log.count("type=WRITE") + event_log.count("type=FLUSH")
            writes = event_log.count("type=WRITE")
            flushes = event_log.count("type=FLUSH")
            print(f"small transaction: {events} events ({writes} writes, "
                  f"{flushes} flushes)", flush=True)
            # 同一持久化策略决定稳定盘初态，切点上限不能借用默认探针。
            for policy in ("none", "odd", "reverse"):
                policy_probe = directory / f"small-probe-{policy}.img"
                shutil.copyfile(small, policy_probe)
                set_control(policy_probe, small_phase, "G")
                _, policy_log = nbd_boot(
                    args, policy_probe, directory, f"small-probe-{policy}",
                    options=(f"--persist={policy}", "--arm-on-signal"),
                    gate=True, marker="BoarOS: SQLite commit confirmed")
                policy_probe.unlink()
                assert "armed=1\n" in policy_log
                policy_log = policy_log.split("armed=1\n", 1)[1]
                policy_events = (policy_log.count("type=WRITE") +
                                 policy_log.count("type=FLUSH"))
                writes = max(writes, policy_log.count("type=WRITE"))
                flushes = max(flushes, policy_log.count("type=FLUSH"))
                print(f"{policy} cut transaction: {policy_events} events",
                      flush=True)
                if args.matrix == "full":
                    cuts = range(1, policy_events + 1)
                else:
                    cuts = sorted({1, 2, policy_events // 4,
                                   policy_events // 2,
                                   3 * policy_events // 4, policy_events})
                for position in cuts:
                    name = f"cut-{position}-{policy}"
                    image = directory / f"{name}.img"
                    shutil.copyfile(small, image)
                    set_control(image, small_phase, "G")
                    guest, _ = nbd_boot(args, image, directory, name, cut=True,
                             gate=True,
                             options=(f"--cut-after={position}",
                                      f"--persist={policy}",
                                      "--arm-on-signal"))
                    committed = "BoarOS: SQLite commit confirmed" in guest
                    recover_twice(args, image, directory, small_phase, name,
                                  "new" if committed else None)
                    image.unlink()
                    print(name + (": post-commit" if committed else ": in-flight"), flush=True)
            if args.matrix == "full":
                write_faults = range(1, writes + 1)
                flush_faults = range(1, flushes + 1)
            else:
                write_faults = sorted({1, max(1, writes // 2), writes})
                flush_faults = sorted({1, max(1, flushes // 2), flushes})
            print(f"fault envelope: {writes} WRITE/{flushes} FLUSH ordinals", flush=True)
            for kind, fault_positions in (("write", write_faults),
                                          ("flush", flush_faults)):
                for position in fault_positions:
                    name = f"{kind}-fault-{position}"
                    image = directory / f"{name}.img"
                    shutil.copyfile(small, image)
                    set_control(image, small_phase, "G")
                    guest, events = nbd_boot(args, image, directory, name,
                             options=(f"--fail-{kind}={position}",
                                      "--arm-on-signal"),
                             gate=True, expect_failure=True)
                    committed = "BoarOS: SQLite commit confirmed" in guest
                    recover_twice(args, image, directory, small_phase, name,
                                  "new" if committed and "result=5" not in events else None)
                    image.unlink()
                    print(name + (": injected" if "result=5" in events else
                          ": ordinal not reached, post-commit cut"), flush=True)
        print("SQLite EXTRA/FULL, hot journal, confirmed commit recovery passed")
    except Exception:
        print(f"SQLite recovery artifacts retained: {directory}", flush=True)
        raise
    else:
        shutil.rmtree(directory)


if __name__ == "__main__":
    main()
