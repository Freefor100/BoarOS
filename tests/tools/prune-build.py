#!/usr/bin/env python3
"""Keep current compiler inputs and kernel caches; remove historical build outputs."""
import sys
from pathlib import Path
sys.path[:0] = [str(Path(__file__).resolve().parent), str(Path(__file__).resolve().parents[1])]

import argparse
import os
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build'
# Whole installed inputs, not every historical profile that once built them.
CACHE_TREES = (
    'tools/loongarch', 'tools/oscomp-local/host', 'tools/bc',
    'tools/oscomp-analysis', 'qemu-la-rtc', 'program-environment',
    'offline-c/alpine-tree', 'local-keep',
    'loongarch/musl-root', 'loongarch/gcc-sf/root',
    'loongarch/busybox-source/busybox', 'loongarch/uapi',
    'riscv/musl-root', 'riscv-user-smp4/riscv/musl-root',
    'bpf/downloads', 'bpf/riscv/root', 'bpf/loongarch/root',
)
KERNEL_CACHES = ('riscv', 'loongarch', 'riscv-user-smp8/riscv', 'loongarch-smp8')
KERNEL_PARTS = ('arch', 'drivers', 'fs', 'generated', 'kernel', 'lib', 'mm',
                'net', 'platform', 'third_party', 'vdso', 'btf')
REFERENCE_FILES = ('.config', 'identity.json', 'boaros-identity.json')


def protected_paths():
    """Build can contain a live Git worktree, including uncommitted changes."""
    listing = subprocess.check_output(['git', 'worktree', 'list', '--porcelain', '-z'], cwd=ROOT)
    paths = [Path(os.fsdecode(item[len(b'worktree '):])).resolve()
             for item in listing.split(b'\0') if item.startswith(b'worktree ')]
    tracked = subprocess.check_output(['git', 'ls-files', '-z', '--', 'build'], cwd=ROOT)
    paths.extend(ROOT / os.fsdecode(item) for item in tracked.split(b'\0') if item)
    return [path for path in paths if path.is_relative_to(BUILD.resolve())]


def deletion_paths(paths, protected):
    root = BUILD.resolve()
    result = []
    for path in paths:
        # Resolve the parent, not the leaf: an eligible symlink is only unlinked.
        entry = path.parent.resolve() / path.name
        if entry.is_relative_to(root) and entry != root and not any(
            item.is_relative_to(entry) or entry.is_relative_to(item) for item in protected
        ):
            result.append(path)
    return result


def current_linux_keys():
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(ROOT / 'tests/diff-abi'))
    import harness
    return {harness.identity(ROOT / config)[0] for config in
            ('tests/diff-abi/linux.config', 'tests/userland/inventory/linux.config',
             'tests/userland/network-linux.config', 'tests/userland/tty/pty-linux.config', 'tests/smp/linux-user.config')}


def candidates():
    if not BUILD.is_dir() or BUILD.is_symlink():
        return []
    keys = current_linux_keys()  # Invalid reference inputs must stop cleanup.
    keep = {BUILD / name for name in CACHE_TREES}
    for cache in KERNEL_CACHES:
        keep.update(BUILD / cache / name for name in KERNEL_PARTS)
        keep.update(BUILD / cache / name for name in
                    ('kernel-rv', 'kernel-la', 'kernel-rv.map', 'kernel.map'))
    keep.add(BUILD / 'riscv-user-smp8/kernel-rv')
    for arch in ('riscv', 'loongarch'):
        keep.update(BUILD / 'tools/oscomp-local' / arch / name for name in
                    ('mke2fs', 'mke2fs.conf', 'seed', 'identity.json'))
    for cache in ('linux-la', 'linux-la-platform', 'linux-la-platform-noswap'):
        keep.update(BUILD / cache / name for name in (*REFERENCE_FILES, 'vmlinux'))
    for key in keys:
        keep.update(BUILD / 'diff-abi/linux' / key / name for name in
                    (*REFERENCE_FILES, 'arch/riscv/boot/Image'))
    for arch in ('riscv', 'loongarch'):
        base = BUILD / 'bpf' / arch
        keep.update(base / 'linux' / name for name in
                    (*REFERENCE_FILES, 'arch/riscv/boot/Image' if arch == 'riscv' else 'vmlinux'))
        # baseline.py installs these libraries; compiler object trees are disposable.
        keep.update((base / 'llvm/lib').glob('*.so*'))

    result = []
    def select(directory):
        for path in directory.iterdir():
            if path in keep:
                continue
            if path.is_dir() and not path.is_symlink() and any(
                    item.is_relative_to(path) for item in keep):
                select(path)
            else:
                result.append(path)
    select(BUILD)
    return sorted(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', help='delete listed artifacts')
    parser.add_argument('--keep', action='append', default=[], type=Path,
                        help='preserve a selected build path and any containing candidate')
    args = parser.parse_args()
    kept = [path.resolve() for path in args.keep]
    if any(not path.is_relative_to(BUILD.resolve()) for path in kept):
        parser.error('--keep paths must be inside build/')
    paths = deletion_paths(candidates(), protected_paths() + kept)
    removed = failed = 0
    for path in paths:
        print(path.relative_to(ROOT))
        if args.apply:
            try:
                if path.is_dir() and not path.is_symlink():
                    shutil.rmtree(path)
                else:
                    path.unlink()
                removed += 1
            except OSError as error:
                failed += 1
                print(f'Could not remove {path.relative_to(ROOT)}: {error}', file=sys.stderr)
    if args.apply:
        print(f'Removed: {removed} paths; failed: {failed}')
    else:
        print(f'Would remove: {len(paths)} paths')
    return int(failed != 0)


if __name__ == '__main__':
    sys.exit(main())
