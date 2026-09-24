#!/usr/bin/env python3
"""Remove one-off build/ runs while preserving reusable build caches."""

import argparse
import shutil
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build'
KEEP_TOP = {'riscv', 'diff-abi', 'program-environment', 'program-libc', 'host', 'tools'}
KEEP_ROOT_FILES = {'elf-tail-rv', 'elf-tail-dynamic-rv', 'elf-tail-norelro-rv'}


def current_linux_keys():
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(ROOT / 'tests/diff-abi'))
    import harness
    return {harness.identity(ROOT / config)[0] for config in
            ('tests/diff-abi/linux.config', 'tests/program-inventory/linux.config')}


def candidates():
    if not BUILD.is_dir() or BUILD.is_symlink():
        return []
    keys = current_linux_keys()  # Fail before deleting anything if identity is unavailable.
    result = []
    for path in BUILD.iterdir():
        if path.is_dir() and not path.is_symlink() and path.name in KEEP_TOP:
            continue
        if path.is_file() and path.name in KEEP_ROOT_FILES:
            continue
        result.append(path)

    riscv = BUILD / 'riscv'
    if riscv.is_dir():
        result.extend(path for path in riscv.iterdir()
                      if path.name in {'artifacts', 'musl-src', 'truncate-probe'}
                      or path.name.startswith('userland-run.'))

    diff = BUILD / 'diff-abi'
    if diff.is_dir():
        result.extend(path for path in diff.iterdir()
                      if path.name not in {'linux', 'tools', 'cases-rv'})
        linux = diff / 'linux'
        if linux.is_dir():
            for path in linux.iterdir():
                if path.name not in keys:
                    result.append(path)
                elif path.is_dir():
                    result.extend(path / name for name in ('configure.log', 'build.log')
                                  if (path / name).exists())

    libc = BUILD / 'program-libc'
    if libc.is_dir():
        result.extend(path for path in libc.iterdir()
                      if path.name == 'upstream.tar' or path.suffix == '.log')

    environment = BUILD / 'program-environment'
    if environment.is_dir():
        stale = {'linux-6.6', 'linux-6.6.tar.xz', 'linux-build',
                 'extract-linux-6.6.log', 'headers-install.log', 'headers-probe.log',
                 'restore-linux-6.6.log', 'busybox-uapi-7.3-failure.log',
                 'kernel-sha256sums.asc', 'full-build-result.json',
                 'prepare-result.json', 'reproducibility.json'}
        result.extend(path for path in environment.iterdir() if path.name in stale)
        busybox_uapi = environment / 'busybox-environment'
        if busybox_uapi.is_dir():
            result.extend(path for path in busybox_uapi.iterdir()
                          if path.name in {'linux-build', 'headers-install.log',
                                           'headers-probe.log', 'extract-linux-6.6.log'})
        busybox = environment / 'full-busybox'
        if busybox.is_dir():
            result.extend(path for path in busybox.iterdir()
                          if path.name in {'source.tar', 'extract.log',
                                           'configure.log', 'build.log'})
    return sorted(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', help='delete listed artifacts')
    args = parser.parse_args()
    paths = candidates()
    for path in paths:
        print(path.relative_to(ROOT))
        if args.apply:
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            else:
                path.unlink()
    print(f'{"Removed" if args.apply else "Would remove"}: {len(paths)} paths')


if __name__ == '__main__':
    main()
