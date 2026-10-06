#!/usr/bin/env python3
"""Run the same unmodified SQLite Unix VFS WAL workload on fixed Linux and BoarOS."""

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
sys.path.insert(0, str(ROOT / "tests/diff-abi"))
import harness
sys.path.insert(0,str(ROOT/"tests"))
from arch_profiles import PROFILES


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def boot(qemu, kernel, disk, output, linux, marker, second=None, arch="riscv",memory="512M",initrd=None,direct_init="/init"):
    profile=PROFILES[arch]
    command=profile.boot(qemu,kernel,memory)+["-net","none",
        "-object","rng-random,id=entropy,filename=/dev/urandom","-device",profile.rng(),
        "-drive",f"file={disk},if=none,format=raw,id=root","-device",profile.block("modern")]
    if arch=="riscv":command+=["-global","virtio-mmio.force-legacy=false"]
    if second is not None:
        command += ["-drive", f"file={second},if=none,format=raw,id=second",
                    "-device", "virtio-blk-device,drive=second,bus=virtio-mmio-bus.1"]
    if linux:
        root_device = "/dev/vdb" if second is not None else "/dev/vda"
        if initrd:command+=["-initrd",str(initrd),"-append","console=ttyS0 rdinit=/init loglevel=3"]
        else:command+=["-append",f"root={root_device} rw rootwait console=ttyS0 init={direct_init} loglevel=0 panic=-1"]
    with output.open("w") as log:
        result = subprocess.run(command, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT,
                                timeout=90)
    content = output.read_text(errors="replace")
    if linux and initrd and ("Linux "+("LA" if arch=="loongarch" else "RV")+" root application passed" not in content):
        raise AssertionError("SQLite real Linux exit is not42: "+str(output))
    if result.returncode or marker not in content or (
            not linux and not profile.root_success(content,42)):
        raise AssertionError(f"SQLite WAL {kernel} failed: {output}\n"
                             + content[-5000:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch",choices=tuple(PROFILES),default="riscv")
    parser.add_argument("--memory",choices=("512M","1G"),action="append")
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--qemu")
    parser.add_argument("--second-disk", action="store_true")
    args = parser.parse_args()
    profile=PROFILES[args.arch];args.qemu=args.qemu or profile.qemu
    if args.arch=="loongarch" and args.second_disk:parser.error("LA second-disk fixture is not connected by this runner")
    linux_kernel=profile.linux_kernel() if args.arch=="loongarch" else harness.linux_build()[0]
    archive=ROOT/"references/sqlite/sqlite-amalgamation-3530400.zip"
    pin=next(row.split("\t")[4] for row in (ROOT/"references/sources.tsv").read_text().splitlines() if row.startswith("file\tsqlite/sqlite-amalgamation-3530400.zip\t"))
    if sha256(archive)!=pin:raise RuntimeError("SQLite archive identity mismatch")
    identity = {
        "arch":args.arch,
        "boaros_kernel_sha256": sha256(args.kernel),
        "linux_kernel_sha256": sha256(linux_kernel),
        "workload_sha256": sha256(args.program),
        "sqlite_archive_sha256": sha256(ROOT /
            "references/sqlite/sqlite-amalgamation-3530400.zip"),
        "qemu": subprocess.check_output([args.qemu, "--version"],
            text=True).splitlines()[0],
        "qemu_sha256":sha256(shutil.which(args.qemu) or args.qemu),
        "rebuild": "make test-sqlite-second-disk-riscv" if args.second_disk else "make test-sqlite-wal-"+args.arch,
    }
    identity['runs']=[]
    directory = Path(tempfile.mkdtemp(prefix="sqlite-wal-run.",
                                      dir=ROOT / "build" / args.arch))
    try:
        disk = harness.fixture(directory, args.program)
        initrd=None
        if args.arch=="loongarch":
            sys.path.insert(0,str(ROOT/"tests/loongarch"));from reference import archive
            supervisor=directory/"supervisor"
            subprocess.run([profile.compiler,*profile.raw_flags,"-O2","-DEXPECTED_EXIT_STATUS=42",
                "-ffreestanding","-fno-builtin","-fno-stack-protector","-nostdlib","-nostartfiles",
                "-static","-no-pie","-Wl,--build-id=none","-Wl,-z,max-page-size=16384",
                "-T","tests/common/user.ld","tests/loongarch/root_linux_init.c","tests/common/user_start.S",
                "-o",str(supervisor)],check=True)
            initrd=directory/"initramfs.gz";initrd.write_bytes(archive([("dev",0o040755,b"",0,0),
                ("dev/console",0o020600,b"",5,1),("init",0o100755,supervisor.read_bytes(),0,0),("TRAILER!!!",0,b"",0,0)]))
            identity["supervisor_sha256"]=sha256(supervisor)
        frozen={}
        for name,kernel in (("linux",linux_kernel),("boaros",args.kernel.resolve())):
            frozen[name]=directory/(name+"-kernel");shutil.copy2(kernel,frozen[name])
        runs=[(name,kernel,name=="linux",memory) for name,kernel in frozen.items()
              for memory in args.memory or (["512M","1G"] if args.arch=="loongarch" else ["512M"])]
        for platform,kernel,linux,memory in runs:
            name=platform+"-"+memory
            target = directory / (name + ".img")
            shutil.copy2(disk, target)
            second = None
            if args.second_disk:
                second = directory / (name + "-second.img")
                with second.open("wb") as stream:
                    stream.truncate(64 * 1024 * 1024)
                subprocess.run(["mkfs.ext4", "-q", "-F", "-b", "4096", str(second)], check=True)
            boot(args.qemu, kernel, target, directory / (name + ".log"),
                 linux, "BoarOS: SQLite WAL multiprocess passed", second,args.arch,memory,initrd)
            boot(args.qemu, kernel, target,
                 directory / (name + "-reboot.log"), linux,
                 "BoarOS: SQLite WAL reboot verified", second,args.arch,memory,initrd)
            identity['runs'].append({'platform':platform,'memory':memory,'multiprocess':True,'reboot':True,'exit':42})
    except Exception:
        print(f"SQLite WAL artifacts retained: {directory}", file=sys.stderr)
        raise
    if args.arch=="riscv":shutil.rmtree(directory)
    else:
        (directory/"identity.json").write_text(json.dumps(identity,indent=2)+"\n")
        print("SQLite WAL native artifacts:",directory)
    print("SQLite WAL inputs:", json.dumps(identity, sort_keys=True))
    print("SQLite WAL multiprocess and reboot passed on fixed Linux and BoarOS")


if __name__ == "__main__":
    main()
