#!/usr/bin/env python3
"""Run pinned, unmodified RV64 glibc programs on fixed Linux and BoarOS."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
BUILD = ROOT / "build/riscv/glibc"
INTERPRETER = "/lib/ld-linux-riscv64-lp64d.so.1"
BASE_MARKERS = ["GLIBC CONSTRUCTOR", "GLIBC MAIN", "GLIBC BASE OK"]
EXTRA_MARKERS = ["GLIBC DLOPEN TLS OK", "GLIBC PTHREAD TLS OK",
                 "GLIBC SIGNAL OK"]
FINAL_MARKERS = ["GLIBC RUNTIME OK", "GLIBC EXIT"]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def logged(command, log, timeout=None):
    with log.open("w") as output:
        try:
            result = subprocess.run(command, stdin=subprocess.DEVNULL,
                                    stdout=output, stderr=subprocess.STDOUT,
                                    timeout=timeout, check=False)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f"timeout: {command[0]}; see {log}") from error
    if result.returncode != 0:
        raise RuntimeError(f"exit {result.returncode}: {command[0]}; see {log}")


def run_guest(command, log):
    with log.open("w") as output:
        try:
            result = subprocess.run(command, stdin=subprocess.DEVNULL,
                                    stdout=output, stderr=subprocess.STDOUT,
                                    timeout=30, check=False)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f"guest timeout; see {log}") from error
    return result.returncode


def checked_inputs():
    manifest = json.loads((HERE / "inputs.json").read_text())
    for role in ("tools", "runtime"):
        for name, expected in manifest[role].items():
            path = Path(name)
            if not path.is_file() or digest(path) != expected:
                raise RuntimeError(f"{role} identity mismatch: {path}")
    libc = next(Path(path) for path in manifest["runtime"]
                if path.endswith("/libc.so.6"))
    if f"stable release version {manifest['glibc_version']}".encode() not in libc.read_bytes():
        raise RuntimeError("glibc release version differs from pinned binary")
    return manifest


def build_programs(inputs):
    BUILD.mkdir(parents=True, exist_ok=True)
    compiler = next(path for path in inputs["tools"] if path.endswith("-gcc"))
    versions = {
        "static": ["-static", "-no-pie"],
        "dynamic": ["-no-pie"],
        "pie": ["-fPIE", "-pie"],
        "static-pie": ["-static-pie"],
        "pthread-pie": ["-fPIE", "-pie", "-pthread", "-DGLIBC_PROBE_THREADS"],
    }
    programs = {}
    for name, flags in versions.items():
        program = BUILD / f"{name}-rv"
        command = [compiler, "-O2", "-Wall", "-Wextra", "-Werror",
                   *flags]
        if name in ("dynamic", "pie", "pthread-pie"):
            command.append(f"-Wl,--dynamic-linker={INTERPRETER}")
        command += ["-o", str(program), str(HERE / "probe.c")]
        if name == "pthread-pie":
            command.append("-ldl")
        logged(command, BUILD / f"{name}.build.log")
        inspection = subprocess.check_output(["readelf", "-l", str(program)],
                                             text=True)
        has_interpreter = "INTERP" in inspection
        if has_interpreter != (name in ("dynamic", "pie", "pthread-pie")):
            raise RuntimeError(f"unexpected PT_INTERP: {name}")
        if has_interpreter and INTERPRETER not in inspection:
            raise RuntimeError(f"incorrect PT_INTERP: {name}")
        programs[name] = program
    library = BUILD / "libboaros-glibc-tls.so"
    logged([compiler, "-O2", "-Wall", "-Wextra", "-Werror", "-fPIC",
            "-shared", "-Wl,-soname,libboaros-glibc-tls.so",
            "-o", str(library), str(HERE / "tls_dso.c")],
           BUILD / "tls-dso.build.log")
    return programs, library


def fixture(directory, program, library, inputs):
    tree = directory / "tree"
    (tree / "lib").mkdir(parents=True)
    (tree / "dev").mkdir()
    shutil.copy2(program, tree / "init")
    shutil.copy2(library, tree / "lib/libboaros-glibc-tls.so")
    for path in inputs["runtime"]:
        shutil.copy2(path, tree / "lib" / Path(path).name)
    disk = directory / "fixture.img"
    logged(["mkfs.ext4", "-q", "-F", "-b", "4096", "-d", str(tree),
            str(disk), "32M"], directory / "mkfs.log")
    commands = directory / "devices.debugfs"
    commands.write_text("mknod /dev/console c 5 1\n"
                        "set_inode_field /dev/console mode 020600\n")
    logged(["debugfs", "-w", "-f", str(commands), str(disk)],
           directory / "debugfs.log")
    return disk


def check_observation(name, target, log, expected, returncode):
    raw = log.read_text(errors="replace").replace("\r\n", "\n")
    markers = [line for line in raw.splitlines() if line.startswith("GLIBC ")]
    if markers != expected:
        if "GLIBC MAIN" not in markers:
            stage = "loader/pre-main"
        elif "GLIBC RUNTIME OK" not in markers:
            stage = "runtime"
        else:
            stage = "exit"
        raise RuntimeError(f"{name} {target} {stage}: {markers}; see {log}")
    if returncode != 0:
        raise RuntimeError(f"{name} {target} exit: QEMU status {returncode}; see {log}")
    if target == "boaros" and not re.search(
        r"^BoarOS: PID 1 exited status=0x2a pages=0x[1-9a-f][0-9a-f]* "
        r"heap-live=0x0; shutting down$", raw, re.M
    ):
        raise RuntimeError(f"{name} boaros exit/resource mismatch; see {log}")
    if re.search(r"BoarOS: (fatal trap|root boot error)", raw):
        raise RuntimeError(f"{name} boaros kernel failure; see {log}")
    return markers


def execute(programs, library, inputs, linux, kernel, qemu):
    work = Path(tempfile.mkdtemp(prefix="run.", dir=BUILD))
    status = "failed"
    try:
        for name, program in programs.items():
            case = work / name
            case.mkdir()
            disk = fixture(case, program, library, inputs)
            expected = BASE_MARKERS + (EXTRA_MARKERS if name == "pthread-pie" else []) + FINAL_MARKERS
            for target, image in (("linux", linux), ("boaros", kernel)):
                target_disk = case / f"{target}.img"
                shutil.copyfile(disk, target_disk)
                command = [qemu, "-machine", "virt", "-bios", "default",
                           "-kernel", str(image), "-m", "512M", "-smp", "1",
                           "-nographic", "-no-reboot", "-drive",
                           f"file={target_disk},if=none,format=raw,id=root",
                           "-device", "virtio-blk-device,drive=root,bus=virtio-mmio-bus.0"]
                if target == "linux":
                    command += ["-append", "root=/dev/vda rw rootwait "
                                "console=ttyS0 init=/init loglevel=0 panic=-1"]
                log = case / f"{target}.log"
                result = run_guest(command, log)
                check_observation(name, target, log, expected, result)
            print(f"glibc 2.44 {name}: Linux/BoarOS markers and exit verified")
        status = "passed"
    finally:
        if status == "passed":
            shutil.rmtree(work)
        else:
            print(f"glibc artifacts retained: {work}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=ROOT / "kernel-rv")
    parser.add_argument("--qemu", default=os.environ.get("QEMU_RISCV64", "qemu-system-riscv64"))
    args = parser.parse_args()
    inputs = checked_inputs()
    programs, library = build_programs(inputs)
    sys.path.insert(0, str(ROOT / "tests/diff-abi"))
    from harness import linux_build
    linux, linux_identity = linux_build()
    identity = {
        "inputs": inputs,
        "sources": {path.name: digest(path) for path in
                    (HERE / "probe.c", HERE / "tls_dso.c", HERE / "run.py")},
        "programs": {name: digest(path) for name, path in programs.items()},
        "library": digest(library),
        "linux": {"image": str(linux), "sha256": digest(linux),
                  "inputs": linux_identity["inputs"]},
        "boaros": {"image": str(args.kernel), "sha256": digest(args.kernel)},
        "qemu": subprocess.check_output([args.qemu, "--version"], text=True).splitlines()[0],
    }
    (BUILD / "identity.json").write_text(json.dumps(identity, indent=2) + "\n")
    execute(programs, library, inputs, linux, args.kernel, args.qemu)
    print("rebuild: make test-glibc-riscv")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"glibc: {error}", file=sys.stderr)
        sys.exit(1)
