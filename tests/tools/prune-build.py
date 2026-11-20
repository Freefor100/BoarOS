#!/usr/bin/env python3
"""Keep boot disks and installed tools; remove rebuildable kernels and experiment outputs."""
import sys
from pathlib import Path
sys.path[:0] = [str(Path(__file__).resolve().parent), str(Path(__file__).resolve().parents[1])]

import argparse
import os
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build'
# Persistent boot disks and reusable installed inputs.
CACHE_TREES = (
    'rootfs', 'tools/loongarch', 'tools/oscomp-local/host', 'local-keep',
    'qemu-la-rtc/qemu-system-loongarch64', 'qemu-la-rtc/build/qemu-system-loongarch64',
    'qemu-la-rtc/boaros-identity.json',
    'loongarch/musl-root', 'loongarch/gcc-sf/root',
    'riscv/musl-root', 'riscv-user-smp4/riscv/musl-root',
)


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


def candidates():
    if not BUILD.is_dir() or BUILD.is_symlink():
        return []
    keep = {BUILD / name for name in CACHE_TREES}
    for arch in ('riscv', 'loongarch'):
        keep.update(BUILD / 'tools/oscomp-local' / arch / name for name in
                    ('mke2fs', 'mke2fs.conf', 'seed', 'identity.json'))
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
