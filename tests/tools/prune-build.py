#!/usr/bin/env python3
"""Remove one-off build/ runs while preserving reusable build caches."""
import sys
from pathlib import Path
sys.path[:0] = [str(Path(__file__).resolve().parent), str(Path(__file__).resolve().parents[1])]

import argparse
import os
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build'
KEEP_TOP = {'data-path-baselines', 'cost', 'riscv', 'riscv-smp', 'riscv-user-smp', 'riscv-user-smp-composition', 'riscv-smp-test', 'riscv-mm-smp-test', 'riscv-mm-shared-smp-test', 'riscv-mm-file-smp-test', 'riscv-mm-disk-smp-test', 'riscv-files-smp-test', 'riscv-file-io-smp-test', 'loongarch', 'qemu-la', 'qemu-la-rtc', 'linux-la', 'linux-la-platform', 'diff-abi', 'program-environment', 'program-libc', 'tools', 'offline-c'}
KEEP_ROOT_FILES = {'elf-tail-rv', 'elf-tail-dynamic-rv', 'elf-tail-norelro-rv'}
KEEP_TOP.update({'local-keep', 'linux-la-platform-noswap', 'loongarch-smp2',
                 'loongarch-smp4', 'loongarch-smp8', 'riscv-user-smp4', 'riscv-user-smp8'})


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
    keys = current_linux_keys()  # Fail before deleting anything if identity is unavailable.
    result = []
    for path in BUILD.iterdir():
        if path.is_dir() and not path.is_symlink() and path.name in KEEP_TOP:
            continue
        if path.is_file() and path.name in KEEP_ROOT_FILES:
            continue
        result.append(path)

    cost = BUILD / 'cost'
    if cost.is_dir():
        result.extend(path for path in cost.iterdir()
                      if path.name not in {'riscv', 'host', 'kernel-rv', 'measurement.lock'})

    # These runners restore their pinned images from references/ on demand.
    tools = BUILD / 'tools'
    result.extend(path for path in (tools / 'network-rv', tools / 'oscomp')
                  if path.exists() or path.is_symlink())

    for family in ('riscv-mm-smp-test', 'riscv-mm-shared-smp-test', 'riscv-mm-file-smp-test', 'riscv-mm-disk-smp-test', 'riscv-files-smp-test', 'riscv-file-io-smp-test'):
        directory = BUILD / family
        if directory.is_dir():
            result.extend(p for p in directory.iterdir() if p.is_dir() and p.name.startswith('runs'))
            if family in ('riscv-files-smp-test','riscv-file-io-smp-test'):
                # 自定义CI构建保留编译缓存，嵌套运行镜像仍按同一规则清理。
                result.extend(p for p in directory.glob('*/runs') if p.is_dir())

    for family in ('riscv-user-smp', 'riscv-user-smp4', 'riscv-user-smp8'):
        user_smp = BUILD / family
        if user_smp.is_dir():
            result.extend(path for path in user_smp.iterdir()
                          if path.name not in {'riscv','kernel-rv','user-smp-rv','user-smp-dynamic-rv'})

    composition = BUILD / 'riscv-user-smp-composition'
    if composition.is_dir():
        result.extend(path for path in composition.iterdir()
                      if path.is_dir() and path.name.startswith('runs'))

    riscv = BUILD / 'riscv'
    smp_runs = BUILD / 'riscv-smp' / 'runs'
    if smp_runs.is_dir():
        result.append(smp_runs)
    if riscv.is_dir():
        result.extend(path for path in riscv.iterdir()
                      if path.name in {'artifacts', 'musl-src', 'truncate-probe'}
                      or path.name.startswith(('userland-run.', 'record-lock-run.',
                                                'offline-c-run.', 'offline-project-run.', 'tty-run.', 'pty-run.', 'sched-bandwidth.',
                                                'sqlite-run.',
                                                'sqlite-nbd-run.',
                                                'sqlite-recovery-run.','sqlite-wal-run.','rng-run.','environment-run.')))

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

    la = BUILD / 'loongarch'
    if la.is_dir():
        result.extend(la.glob('*.log'))
        result.extend(la.glob('userland-identity-v1.json'))
        result.extend(la.glob('userland-identity-v2-pre-mode.json'))
        result.extend(la.glob('userland-identity-v2-pre-wrapper.json'))
        result.extend(path for path in la.iterdir() if path.name in {'root-run','root-io-run','block-run','toolchain-check','dynamic','dynamic-dp','exec-error-inputs'}
                      or path.name.startswith(('root-run.','root-io-run.','userland-run.','exec-fail-run.','stack-guard-run.','rng-run.','network-run.','net-failure-run.','uart-failure-run.','tty-run.','pty-run.','environment-run.','rtc-model.','rtc-alarm.','sqlite-wal-run.','sqlite-run.')))
        result.extend(path for path in la.iterdir() if path.is_dir() and
                      (path.name=='program-inventory' or path.name.startswith(('program-inventory-','inventory-','diff-abi-','review-boundary-'))))
        la_libc=la/'program-libc'
        if la_libc.is_dir():
            result.extend(path for path in la_libc.iterdir() if path.name=='upstream.tar' or path.suffix=='.log')
        gcc_cache=la/'gcc-sf'
        if gcc_cache.is_dir():
            result.extend(path for path in gcc_cache.iterdir() if path.name in {'build','build-sf'})
        reference = la / 'reference'
        if reference.is_dir():
            result.extend(path for path in reference.iterdir()
                          if path.suffix in {'.log','.img','.gz'})

    libc = BUILD / 'program-libc'
    if libc.is_dir():
        result.extend(path for path in libc.iterdir()
                      if path.name == 'upstream.tar' or path.suffix == '.log')

    for architecture in ('riscv','loongarch'):
        glibc=BUILD/architecture/'glibc'
        if glibc.is_dir():
            result.extend(path for path in glibc.iterdir() if path.suffix in ('.img','.log') or path.name.startswith('run.'))

    offline_c = BUILD / 'offline-c'
    if offline_c.is_dir():
        result.extend(path for path in offline_c.iterdir()
                      if path.name in {'trace.img', 'apks'} or path.suffix == '.log')

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
