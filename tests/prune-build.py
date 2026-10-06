#!/usr/bin/env python3
"""Remove one-off build/ runs while preserving reusable build caches."""

import argparse
import shutil
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build'
KEEP_TOP = {'data-path-baselines', 'network-budget', 'io-budget', 'cost', 'riscv', 'loongarch', 'qemu-la', 'qemu-la-rtc', 'linux-la', 'linux-la-platform', 'diff-abi', 'program-environment', 'program-libc', 'host', 'tools', 'offline-c'}
KEEP_ROOT_FILES = {'elf-tail-rv', 'elf-tail-dynamic-rv', 'elf-tail-norelro-rv'}


def current_linux_keys():
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(ROOT / 'tests/diff-abi'))
    import harness
    return {harness.identity(ROOT / config)[0] for config in
            ('tests/diff-abi/linux.config', 'tests/program-inventory/linux.config',
             'tests/network-linux.config', 'tests/tty/pty-linux.config')}


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

    # Matched-kernel builds and their identities are reusable; runs/images are not.
    for family in ('network-budget', 'io-budget'):
        directory = BUILD / family
        if directory.is_dir():
            result.extend(path for path in directory.iterdir() if path.name != 'kernels')
            kernels = directory / 'kernels'
            if kernels.is_dir():
                result.extend(path for path in kernels.glob('*/build.log'))

    riscv = BUILD / 'riscv'
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
                      (path.name=='program-inventory' or path.name.startswith(('inventory-','diff-abi-','review-boundary-'))))
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
    paths = candidates()
    paths = [path for path in paths if not any(
        path.resolve() == item or item.is_relative_to(path.resolve())
        or path.resolve().is_relative_to(item) for item in kept)]
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
