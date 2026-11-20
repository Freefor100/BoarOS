#!/usr/bin/env python3
"""Create a minimal ext4 root disk for a BoarOS serial shell."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile


REPOSITORY = Path(__file__).resolve().parents[1]
PACKAGE = "busybox-static-1.37.0-r30.apk"


def build_image(arch, output):
    if output.is_file():
        return
    key = f"alpine/v3.23/main/{arch}/{PACKAGE}"
    manifest = REPOSITORY / "references/sources.tsv"
    for line in manifest.read_text().splitlines():
        if line.startswith(f"file\t{key}\t"):
            _, _, url, _, checksum = line.split("\t")
            break
    else:
        raise ValueError(f"missing package in {manifest}: {key}")

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".rootfs-", dir=output.parent) as temporary:
        work = Path(temporary)
        package = work / PACKAGE
        subprocess.run(["curl", "-fsSL", url, "-o", package], check=True)
        if hashlib.sha256(package.read_bytes()).hexdigest() != checksum:
            raise ValueError(f"package checksum mismatch: {url}")

        root = work / "root"
        for directory in ("bin", "dev", "etc", "proc"):
            (root / directory).mkdir(parents=True)
        init = root / "init"
        # APK v2 concatenates tar streams for the signature, metadata and files.
        with tarfile.open(package, "r:gz", ignore_zeros=True) as archive:
            with archive.extractfile("bin/busybox.static") as binary, init.open("wb") as dest:
                shutil.copyfileobj(binary, dest)
        init.chmod(0o755)
        (root / "bin/busybox").symlink_to("../init")
        for applet in ("sh", "cat", "ls", "uname", "mount", "umount", "ps", "poweroff"):
            (root / "bin" / applet).symlink_to("busybox")
        (root / "etc/inittab").write_text("ttyS0::askfirst:-/bin/sh\n")

        image = work / "root.img"
        with image.open("wb") as disk:
            disk.truncate(64 * 1024 * 1024)
        subprocess.run(["mkfs.ext4", "-q", "-F", "-b", "4096",
                        "-O", "^metadata_csum,^64bit,^orphan_file",
                        "-d", root, image], check=True)
        subprocess.run(["debugfs", "-w", "-f", "-", image], check=True, text=True,
                       input="cd /dev\nmknod console c 5 1\nmknod null c 1 3\n"
                             "mknod tty c 5 0\nmknod ttyS0 c 4 64\n",
                       stdout=subprocess.DEVNULL)

        # Publish a complete disk without replacing one created concurrently.
        try:
            os.link(image, output)
        except FileExistsError:
            if not output.is_file():
                raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("arch", choices=("riscv64", "loongarch64"))
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        build_image(args.arch, args.output)
    except (OSError, ValueError, KeyError, tarfile.TarError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"mkrootfs: {error}\n")


if __name__ == "__main__":
    main()
