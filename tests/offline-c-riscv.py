#!/usr/bin/env python3
"""Run one native C compilation pipeline on fixed Linux and BoarOS."""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests/workloads/toolchain/program.c"
STAGES = ("preprocess", "compile", "assemble", "link", "run")
ARTIFACTS = ("program.i", "program.s", "program.o", "program",
             "output.txt", "stages.tsv")
EXPECTED_OUTPUT = b"BoarOS offline C result=1522623313\n"
LINUX_REVISION = "f4cdf7ca9a1fdcca413157df19753f388a5a224e"


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command(*arguments):
    return subprocess.run(arguments, check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def linux_image(argument):
    if argument is None:
        sys.path.insert(0, str(ROOT / "tests/diff-abi"))
        import harness
        image, _ = harness.linux_build()
        return image
    image = argument.resolve(strict=True)
    for ancestor in image.parents:
        metadata = ancestor / "identity.json"
        if metadata.is_file():
            saved = json.loads(metadata.read_text())
            if saved["inputs"]["source"][1] != LINUX_REVISION or \
                    saved["image_sha256"] != sha256(image):
                raise ValueError("Linux image is not the verified fixed build")
            return image
    raise ValueError("Linux image has no fixed-build identity.json")


def tree_identity(tree):
    if tree is None:
        return None
    digest = hashlib.sha256()
    for entry in sorted(tree.rglob("*")):
        relative = entry.relative_to(tree).as_posix()
        if relative.split("/")[0] in ("init", "work", "dev"):
            raise ValueError(f"toolchain tree overlaps probe fixture: {relative}")
        digest.update(relative.encode() + b"\0")
        digest.update(f"{entry.lstat().st_mode & 0o7777:o}".encode() + b"\0")
        if entry.is_symlink():
            digest.update(b"link\0" + str(entry.readlink()).encode() + b"\0")
        elif entry.is_file():
            digest.update(b"file\0" + sha256(entry).encode() + b"\0")
        elif entry.is_dir():
            digest.update(b"dir\0")
        else:
            raise ValueError(f"unsupported toolchain tree entry: {relative}")
    return digest.hexdigest()


def fixture(directory, program, args):
    tree = directory / "tree"
    if args.toolchain_tree is not None:
        shutil.copytree(args.toolchain_tree, tree, symlinks=True)
    else:
        tree.mkdir()
    (tree / "work").mkdir()
    shutil.copy2(program, tree / "init")
    (tree / "init").chmod(0o755)
    shutil.copy2(SOURCE, tree / "work/program.c")
    (tree / "work/tools.conf").write_text(args.compiler + "\n" +
                                           args.assembler + "\n")
    (tree / "dev").mkdir()
    (tree / "tmp").mkdir(exist_ok=True)
    disk = directory / "fixture.img"
    contents_size = sum(entry.stat().st_size for entry in tree.rglob("*")
                        if entry.is_file() and not entry.is_symlink())
    disk_size = max(64 * 1024 * 1024, contents_size * 2 + 64 * 1024 * 1024)
    with disk.open("wb") as stream:
        stream.truncate(disk_size)
    command("mkfs.ext4", "-q", "-F", "-b", "4096", "-d", str(tree), str(disk))
    instructions = directory / "devices.debugfs"
    instructions.write_text("cd /dev\nmknod console c 5 1\n"
                            "set_inode_field console mode 020600\n"
                            "mknod null c 1 3\n"
                            "set_inode_field null mode 020666\n"
                            "mknod zero c 1 5\n"
                            "set_inode_field zero mode 020666\n")
    command("debugfs", "-w", "-f", str(instructions), str(disk))
    return disk


def boot(args, kernel, disk, output, linux):
    invocation = [args.qemu, "-machine", "virt", "-bios", "default",
                  "-kernel", str(kernel), "-m", "768M", "-smp", "1",
                  "-nographic", "-no-reboot", "-drive",
                  f"file={disk},if=none,format=raw,id=root",
                  "-device", "virtio-blk-device,drive=root,bus=virtio-mmio-bus.0"]
    if linux:
        invocation += ["-append", "root=/dev/vda rw rootwait console=ttyS0 "
                       "init=/init loglevel=0 panic=-1"]
    with output.open("w") as log:
        result = subprocess.run(invocation, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT,
                                timeout=args.timeout)
    transcript = output.read_text(errors="replace")
    if result.returncode or transcript.count(
            "BoarOS: offline compiler probe finished") != 1:
        raise AssertionError(f"guest did not finish: {output}\n{transcript[-4000:]}")
    if not linux and not re.search(
            r"BoarOS: PID 1 exited status=0x2a pages=0x[1-9a-f][0-9a-f]* "
            r"heap-live=0x0; shutting down", transcript):
        raise AssertionError(f"guest resource cleanup incomplete: {output}")


def orphan_cleanup_only(output, repaired):
    """Accept only ext4 crash-orphan cleanup, never unrelated fsck repairs."""
    answer = "yes" if repaired else "no"
    seen_marker = seen_orphan = seen_verdict = seen_summary = False
    for line in output.splitlines():
        if not line:
            continue
        if line.startswith("e2fsck ") or re.fullmatch(r"Pass [1-5](?:A)?: .*", line):
            continue
        if line == ("Inodes that were part of a corrupted orphan linked list "
                    f"found.  Fix? {answer}"):
            seen_marker = True
        elif re.fullmatch(r"Inode \d+ was part of the orphaned inode list.  "
                          + (r"FIXED\." if repaired else r"IGNORED\."), line):
            seen_orphan = True
        elif line == f"Fix? {answer}" or re.fullmatch(
                r"Free (?:blocks|inodes) count wrong \(\d+, counted=\d+\)\.", line):
            continue
        elif (repaired and "***** FILE SYSTEM WAS MODIFIED *****" in line) or \
                (not repaired and "********** WARNING: Filesystem still has errors **********" in line):
            seen_verdict = True
        elif re.fullmatch(r".+: \d+/\d+ files .* \d+/\d+ blocks", line):
            seen_summary = True
        else:
            return False
    return seen_marker and seen_orphan and seen_verdict and seen_summary


def replay_and_check(directory, name, disk):
    # Linux PID 1 exit stops QEMU without unmounting ext4. A new boot would
    # replay the journal before seeing the fsynced stage files; mirror that
    # step before inspecting the image with debugfs.
    replay = subprocess.run(["e2fsck", "-E", "journal_only", "-y", str(disk)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    (directory / (name + "-replay.log")).write_text(replay.stdout)
    if replay.returncode not in (0, 1):
        raise AssertionError(f"{name}: ext4 journal replay failed: {replay.stdout}")
    check = subprocess.run(["e2fsck", "-fn", str(disk)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True)
    (directory / (name + "-fsck-before.log")).write_text(check.stdout)
    if check.returncode:
        if not orphan_cleanup_only(check.stdout, repaired=False):
            raise AssertionError(f"{name}: ext4 check failed: {check.stdout}")
        repair = subprocess.run(["e2fsck", "-fy", str(disk)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True)
        (directory / (name + "-orphan-repair.log")).write_text(repair.stdout)
        if repair.returncode not in (0, 1) or not orphan_cleanup_only(
                repair.stdout, repaired=True):
            raise AssertionError(f"{name}: unexpected ext4 repair: {repair.stdout}")
        check = subprocess.run(["e2fsck", "-fn", str(disk)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True)
    (directory / (name + "-fsck.log")).write_text(check.stdout)
    if check.returncode:
        raise AssertionError(f"{name}: ext4 remains inconsistent: {check.stdout}")


def extract(disk, guest, destination):
    probe = subprocess.run(["debugfs", "-R", f"stat {guest}", str(disk)],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           text=True)
    if probe.returncode or not re.search(r"^Inode: \d+", probe.stdout, re.M):
        return False
    command("debugfs", "-R", f"dump {guest} {destination}", str(disk))
    return destination.is_file()


def evidence(directory, name, disk):
    extracted = directory / (name + "-files")
    extracted.mkdir()
    hashes = {}
    for filename in ARTIFACTS:
        if extract(disk, "/work/" + filename, extracted / filename):
            hashes[filename] = sha256(extracted / filename)
    if "stages.tsv" not in hashes:
        raise AssertionError(f"{name}: no stage record")
    lines = (extracted / "stages.tsv").read_text().splitlines()
    rows = [line.split("\t") for line in lines]
    if len(rows) != len(STAGES) or [row[0] for row in rows] != list(STAGES) or \
            any(len(row) != 3 or not row[2].isdigit() for row in rows):
        raise AssertionError(f"{name}: malformed stage record: {lines}")
    first_failure = next((row for row in rows if row[1] != "exit" or row[2] != "0"), None)
    if first_failure and any(row[1] != "skipped" for row in rows[rows.index(first_failure)+1:]):
        raise AssertionError(f"{name}: later stage ran after failure: {lines}")
    return rows, hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--linux-kernel", type=Path)
    parser.add_argument("--toolchain-tree", type=Path)
    parser.add_argument("--compiler", default="/usr/bin/gcc")
    parser.add_argument("--assembler", default="/usr/bin/as")
    parser.add_argument("--expect-first-failure",
                        help="harness diagnostic mode, e.g. preprocess:exec:2")
    parser.add_argument("--qemu", default="qemu-system-riscv64")
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    for path in (args.compiler, args.assembler):
        if not path.startswith("/") or "\n" in path or "\r" in path:
            parser.error("tool paths must be absolute and single-line")
    linux_kernel = linux_image(args.linux_kernel)
    identity = {
        "boaros_kernel_sha256": sha256(args.kernel),
        "linux_kernel_sha256": sha256(linux_kernel),
        "driver_sha256": sha256(args.program),
        "source_sha256": sha256(SOURCE),
        "toolchain_tree_sha256": tree_identity(args.toolchain_tree),
        "compiler": args.compiler,
        "assembler": args.assembler,
        "qemu": command(args.qemu, "--version").stdout.splitlines()[0],
        "rebuild": "make test-offline-c-riscv",
    }
    directory = Path(tempfile.mkdtemp(prefix="offline-c-run.",
                                      dir=ROOT / "build/riscv"))
    try:
        fixture_disk = fixture(directory, args.program, args)
        observations = {}
        for name, kernel, linux in (("linux", linux_kernel, True),
                                    ("boaros", args.kernel.resolve(), False)):
            disk = directory / (name + ".img")
            shutil.copyfile(fixture_disk, disk)
            boot(args, kernel, disk, directory / (name + ".log"), linux)
            replay_and_check(directory, name, disk)
            observations[name] = evidence(directory, name, disk)
        linux_rows, linux_hashes = observations["linux"]
        boaros_rows, boaros_hashes = observations["boaros"]
        if linux_rows != boaros_rows:
            raise AssertionError(f"stage mismatch: linux={linux_rows}, "
                                 f"boaros={boaros_rows}")
        first_failure = next((row for row in linux_rows
                              if row[1] != "exit" or row[2] != "0"), None)
        if args.expect_first_failure:
            expected = args.expect_first_failure.split(":")
            if first_failure != expected:
                raise AssertionError(f"expected {expected}, saw {first_failure}")
            if linux_hashes != boaros_hashes:
                raise AssertionError("failure artifacts differ between kernels")
            verdict = "diagnostic first failure=" + ":".join(first_failure)
        else:
            if first_failure:
                raise AssertionError("first compilation failure: " +
                                     ":".join(first_failure))
            if set(linux_hashes) != set(ARTIFACTS) or \
                    set(boaros_hashes) != set(ARTIFACTS):
                raise AssertionError("successful stages left missing artifacts")
            for name in ("linux", "boaros"):
                if (directory / (name + "-files/output.txt")).read_bytes() != \
                        EXPECTED_OUTPUT:
                    raise AssertionError(f"{name}: compiled output mismatch")
                elf = directory / (name + "-files/program")
                if elf.read_bytes()[:4] != b"\x7fELF":
                    raise AssertionError(f"{name}: linked file is not ELF")
            if linux_hashes != boaros_hashes:
                raise AssertionError("stage artifact hashes differ between kernels")
            verdict = "all five native stages and exact output passed"
        print("Offline C inputs:", json.dumps(identity, sort_keys=True))
        print("Offline C stage rows:", json.dumps(linux_rows))
        print("Offline C artifact SHA-256:", json.dumps(linux_hashes, sort_keys=True))
        print("Offline C fixed Linux/BoarOS:", verdict)
    except Exception:
        print(f"offline C artifacts retained: {directory}", file=sys.stderr)
        raise
    else:
        shutil.rmtree(directory)


if __name__ == "__main__":
    main()
