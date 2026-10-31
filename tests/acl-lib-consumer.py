#!/usr/bin/env python3
"""Run the upstream acl1 and real libacl consumer twice on a persistent disk."""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tests'), str(ROOT / 'tests/diff-abi'),
                str(ROOT / 'tests/smp'), str(ROOT / 'tests/loongarch')]
import harness
from arch_profiles import PROFILES
from guest import run_guest
from reference import archive
from user import boot_user


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=('riscv', 'loongarch'), required=True)
    parser.add_argument('--libacl-build', type=Path, required=True)
    parser.add_argument('--libattr-build', type=Path, required=True)
    parser.add_argument('--acl1-source', type=Path, required=True)
    parser.add_argument('--busybox', type=Path, required=True,
                        help='static target BusyBox with sh and chmod')
    parser.add_argument('--linux-kernel', type=Path, required=True,
                        help='fixed Linux image with ext4 POSIX ACL enabled')
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    profile = PROFILES[args.arch]
    compiler = ('riscv64-linux-gnu-gcc' if args.arch == 'riscv' else
                str(ROOT / 'build/tools/loongarch/bin/loongarch64-unknown-linux-gnu-gcc'))
    page = [] if args.arch == 'riscv' else ['-Wl,-z,max-page-size=16384']
    include = ['-DEXPORT=extern', '-I' + str((args.libacl_build / 'include').resolve())]
    libs = ['-L' + str((args.libacl_build / '.libs').resolve()),
            '-L' + str((args.libattr_build / '.libs').resolve()), '-lacl', '-lattr']
    config = output / 'ltp-include'; config.mkdir()
    (config / 'config.h').write_text('#define HAVE_LIBACL 1\n')
    (config / 'tst_res_flags.h').write_text('#define TCONF 32\n')
    acl1, program = output / 'acl1', output / 'program'
    subprocess.run([compiler, '-std=gnu89', '-static', '-O2', '-w', *page,
                    '-I' + str(config), *include, str(args.acl1_source.resolve()),
                    *libs, '-o', str(acl1)], cwd=ROOT, check=True)
    subprocess.run([compiler, '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                    *page, *include, str(ROOT / 'tests/workloads/resource/acl_lib.c'),
                    *libs, '-o', str(program)], cwd=ROOT, check=True)

    fixture = harness.fixture(output, program)
    passwd, group = output / 'passwd', output / 'group'
    passwd.write_text('root:x:0:0:root:/:/bin/sh\n' + ''.join(
        f'user{i}:x:{600+i}:{600+i}:test:/:/bin/sh\n' for i in range(1, 6)))
    group.write_text('root:x:0:\n' + ''.join(f'grp{i}:x:{600+i}:\n' for i in range(1, 6)))
    busybox = args.busybox.resolve()
    supervisor = output / 'supervisor'
    rawcc = [profile.compiler, *profile.raw_flags]
    subprocess.run([*rawcc, '-O2', '-ffreestanding', '-fno-builtin',
                    '-fno-stack-protector', '-nostdlib', '-nostartfiles', '-static',
                    '-no-pie', '-Wl,--build-id=none', *page,
                    *(['-DROOT_DIRECT_FILESYSTEM=1'] if args.arch == 'riscv' else []),
                    '-T', 'tests/common/user.ld', 'tests/loongarch/root_linux_init.c',
                    'tests/common/user_start.S', '-o', str(supervisor)], cwd=ROOT, check=True)
    batch = output / 'install.debugfs'
    batch.write_text('\n'.join(['mkdir /etc', f'write {passwd} /etc/passwd',
        f'write {group} /etc/group', 'mkdir /bin', f'write {busybox} /bin/busybox',
        'set_inode_field /bin/busybox mode 0100755', 'symlink /bin/sh busybox',
        'symlink /bin/chmod busybox', f'write {acl1} /acl1',
        'set_inode_field /acl1 mode 0100755', f'write {supervisor} /supervisor',
        'set_inode_field /supervisor mode 0100755']) + '\n')
    harness.run_logged(['debugfs', '-w', '-f', str(batch), str(fixture)], output / 'install.log')
    initrd = output / 'initramfs.gz'
    if args.arch == 'loongarch':
        initrd.write_bytes(archive([('dev', 0o040755, b'', 0, 0),
            ('dev/console', 0o020600, b'', 5, 1),
            ('init', 0o100755, supervisor.read_bytes(), 0, 0), ('TRAILER!!!', 0, b'', 0, 0)]))
    for target, kernel in (('linux', args.linux_kernel), ('boaros', args.kernel)):
        disk = output / (target + '.img'); shutil.copyfile(fixture, disk)
        for boot in (1, 2):
            if args.arch == 'riscv':
                text, record, _ = boot_user('qemu-system-riscv64', 'default',
                    str(kernel.resolve()), disk, '512M', 1, output, target == 'linux',
                    direct_init='/supervisor')
                code = record['returncode']
            else:
                command = profile.boot(str(ROOT / profile.qemu), str(kernel.resolve()), '512M')
                command += ['-drive', f'file={disk},if=none,format=raw,id=root',
                            '-device', profile.block('modern'), '-net', 'none']
                if target == 'linux':
                    command += ['-initrd', str(initrd), '-append', 'console=ttyS0 rdinit=/init loglevel=3']
                code, text = run_guest(command, 120)
            (output / f'{target}-{boot}.log').write_text(text)
            expected = (('ACL-LIB ACCESS-MASK-CHMOD PASS', 'ACL-LIB DEFAULT-INHERIT PASS',
                'ACL TESTS RESULTS: 15 passed, 0 failed', 'ACL-LIB INITIAL PASS') if boot == 1
                else ('ACL-LIB PERSIST PASS',))
            closure = (profile.root_success(text, 0) if target == 'boaros' else
                       ('Linux RV root application passed' if args.arch == 'riscv' else
                        'Linux LA root application passed') in text)
            if code or not closure or not all(line in text for line in expected):
                raise RuntimeError(f'{target} boot {boot} failed; see {output / f"{target}-{boot}.log"}')
            print(target, 'boot', boot, 'PASS', flush=True)
        harness.run_logged(['e2fsck', '-fn', str(disk)], output / (target + '-fsck.log'))
        disk.unlink()
    harness.run_logged(['e2fsck', '-fn', str(fixture)], output / 'fixture-fsck.log')
    fixture.unlink()


if __name__ == '__main__':
    main()
