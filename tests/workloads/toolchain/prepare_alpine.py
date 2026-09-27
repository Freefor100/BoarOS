#!/usr/bin/env python3
"""Verify pinned Alpine APKs and assemble the native RV64 compiler root."""

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
RELATIVE = Path("alpine/v3.22/main/riscv64")
URL_PREFIX = "https://dl-cdn.alpinelinux.org/alpine/v3.22/main/riscv64/"
DESTINATION = ROOT / "build/offline-c/alpine-tree"
IDENTITY = ROOT / "build/offline-c/alpine-tree.identity.json"

# Exact .PKGINFO identities, including the upstream license expressions.
PACKAGES = {
    "binutils-2.44-r3.apk": ("binutils", "2.44-r3", "GPL-2.0-or-later AND LGPL-2.1-or-later AND BSD-3-Clause"),
    "gcc-14.2.0-r6.apk": ("gcc", "14.2.0-r6", "GPL-2.0-or-later AND LGPL-2.1-or-later"),
    "gmp-6.3.0-r3.apk": ("gmp", "6.3.0-r3", "LGPL-3.0-or-later OR GPL-2.0-or-later"),
    "isl26-0.26-r1.apk": ("isl26", "0.26-r1", "MIT"),
    "jansson-2.14.1-r0.apk": ("jansson", "2.14.1-r0", "MIT"),
    "libatomic-14.2.0-r6.apk": ("libatomic", "14.2.0-r6", "GPL-2.0-or-later AND LGPL-2.1-or-later"),
    "libgcc-14.2.0-r6.apk": ("libgcc", "14.2.0-r6", "GPL-2.0-or-later AND LGPL-2.1-or-later"),
    "libgomp-14.2.0-r6.apk": ("libgomp", "14.2.0-r6", "GPL-2.0-or-later AND LGPL-2.1-or-later"),
    "libstdc++-14.2.0-r6.apk": ("libstdc++", "14.2.0-r6", "GPL-2.0-or-later AND LGPL-2.1-or-later"),
    "mpc1-1.3.1-r1.apk": ("mpc1", "1.3.1-r1", "LGPL-3.0-or-later"),
    "mpfr4-4.2.1_p1-r0.apk": ("mpfr4", "4.2.1_p1-r0", "LGPL-3.0-or-later"),
    "musl-1.2.5-r12.apk": ("musl", "1.2.5-r12", "MIT"),
    "musl-dev-1.2.5-r12.apk": ("musl-dev", "1.2.5-r12", "MIT"),
    "zlib-1.3.2-r0.apk": ("zlib", "1.3.2-r0", "Zlib"),
    "zstd-libs-1.5.7-r0.apk": ("zstd-libs", "1.5.7-r0", "BSD-3-Clause OR GPL-2.0-or-later"),
}


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def sources():
    selected = {}
    for row in (ROOT / "references/sources.tsv").read_text().splitlines():
        if not row or row.startswith("#"):
            continue
        kind, path, url, revision, sha256 = row.split("\t")
        if not path.startswith(RELATIVE.as_posix() + "/"):
            continue
        filename = Path(path).name
        if kind != "file" or revision != "-" or filename not in PACKAGES or \
                url != URL_PREFIX + filename or len(sha256) != 64:
            raise ValueError(f"unexpected Alpine source row: {row}")
        if filename in selected:
            raise ValueError(f"duplicate Alpine source: {filename}")
        selected[filename] = sha256
    if selected.keys() != PACKAGES.keys():
        raise ValueError(f"Alpine closure differs: {selected.keys() ^ PACKAGES.keys()}")
    return selected


def verify_apk(filename, sha256):
    path = ROOT / "references" / RELATIVE / filename
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".apk-", delete=False) as temporary:
            pending = Path(temporary.name)
            try:
                with urllib.request.urlopen(URL_PREFIX + filename, timeout=120) as response:
                    shutil.copyfileobj(response, temporary)
            except BaseException:
                pending.unlink(missing_ok=True)
                raise
        if digest(pending) != sha256:
            pending.unlink()
            raise ValueError(f"SHA-256 mismatch: {filename}")
        pending.replace(path)
    if digest(path) != sha256:
        raise ValueError(f"SHA-256 mismatch: {filename}")
    text = subprocess.run(("bsdtar", "-xOf", str(path), ".PKGINFO"),
                          check=True, capture_output=True, text=True).stdout
    metadata = dict(line.split(" = ", 1) for line in text.splitlines()
                    if " = " in line and line.split(" = ", 1)[0] in
                    ("pkgname", "pkgver", "license", "arch"))
    name, version, license_expression = PACKAGES[filename]
    expected = {"pkgname": name, "pkgver": version,
                "license": license_expression, "arch": "riscv64"}
    if metadata != expected:
        raise ValueError(f"APK metadata mismatch: {filename}: {metadata}")
    listing = subprocess.run(("bsdtar", "-tf", str(path)), check=True,
                             capture_output=True, text=True).stdout
    for member in listing.splitlines():
        if member.startswith("/") or ".." in Path(member).parts:
            raise ValueError(f"unsafe APK member: {filename}: {member}")
    return path


def tree_digest(tree):
    value = hashlib.sha256()
    for entry in sorted(tree.rglob("*")):
        relative = entry.relative_to(tree).as_posix()
        value.update(relative.encode() + b"\0")
        value.update(f"{entry.lstat().st_mode & 0o7777:o}".encode() + b"\0")
        if entry.is_symlink():
            value.update(b"link\0" + str(entry.readlink()).encode() + b"\0")
        elif entry.is_file():
            value.update(b"file\0" + digest(entry).encode() + b"\0")
        elif entry.is_dir():
            value.update(b"dir\0")
        else:
            raise ValueError(f"unsupported compiler tree entry: {relative}")
    return value.hexdigest()


def main():
    selected = sources()
    packages = {name: verify_apk(name, selected[name]) for name in sorted(selected)}
    manifest = hashlib.sha256(json.dumps(selected, sort_keys=True).encode()).hexdigest()
    if DESTINATION.exists():
        if not IDENTITY.is_file():
            raise ValueError("compiler tree exists without identity; remove stale ignored tree")
        recorded = json.loads(IDENTITY.read_text())
        if recorded != {"manifest_sha256": manifest,
                        "tree_sha256": tree_digest(DESTINATION)}:
            raise ValueError("compiler tree differs from its pinned identity")
    else:
        DESTINATION.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="alpine-tree.", dir=DESTINATION.parent) as temporary:
            tree = Path(temporary)
            for archive in packages.values():
                subprocess.run(("bsdtar", "-xf", str(archive), "-C", str(tree),
                                "--exclude", ".PKGINFO", "--exclude", ".SIGN.*"),
                               check=True)
            for executable in ("usr/bin/gcc", "usr/bin/as", "lib/ld-musl-riscv64.so.1"):
                if not (tree / executable).is_file():
                    raise ValueError(f"compiler payload lacks {executable}")
            identity = {"manifest_sha256": manifest,
                        "tree_sha256": tree_digest(tree)}
            os.rename(tree, DESTINATION)
        IDENTITY.write_text(json.dumps(identity, sort_keys=True, indent=2) + "\n")
    print(f"Alpine native compiler: {len(packages)} verified APKs, "
          f"tree SHA-256 {tree_digest(DESTINATION)}")


if __name__ == "__main__":
    main()
