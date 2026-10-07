#!/usr/bin/env python3
"""Exercise the same raw supervisor and original BusyBox on Linux and BoarOS."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tests/loongarch'))
from arch_profiles import PROFILES
from reference import archive


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--arch', choices=tuple(PROFILES), required=True)
    args = parser.parse_args()
    arch = PROFILES[args.arch]
    facts = json.loads((ROOT / 'tests/oscomp/inputs.json').read_text())
    source = ROOT / 'references/oscomp-autotest' / facts['architectures'][args.arch]['image']
    directory = Path(tempfile.mkdtemp(prefix='oscomp-supervisor-', dir=ROOT / 'build'))
    helper = ROOT / ('build/riscv' if args.arch == 'riscv' else 'build/loongarch') / 'oscomp/case'
    run(['make', helper.relative_to(ROOT)], cwd=ROOT, stdout=subprocess.DEVNULL)
    init = directory / 'init'
    init.write_text('''#!/musl/busybox sh
set -e
BB=/musl/busybox
$BB mkdir -p /bin
$BB --install -s /bin
export PATH=/bin
echo "OSCOMP supervisor probe start"
check() {
    expected=$1; shift
    set +e
    /boaros-case 3 "$BB" "$@"
    actual=$?
    set -e
    echo "OSCOMP supervisor case expected=$expected actual=$actual"
    [ "$actual" = "$expected" ] || exit 91
}
check 7 "$BB" sh -c 'printf "native exit\\n"; exit 7'
check 143 "$BB" sh -c 'kill -TERM $$'
check 0 "$BB" setsid "$BB" true
check 7 "$BB" sh -c 'trap ":" USR1; kill -USR1 0; printf "group survived\\n"; exit 7'
set +e
/boaros-case 1 "$BB" "$BB" sh -c 'trap "" TERM; while :; do :; done'
actual=$?
set -e
[ "$actual" = 124 ] || exit 92
check 0 "$BB" true
# LTP适配器已经调用supervisor；按正式流直接调用，不能再包一层测试进程组。
set +e
"$BB" sh /boaros-ltp-case.sh /boaros-case /boaros-ltp-skips.tsv 3 "$BB" /native/cgroup_fj_proc
actual=$?
set -e
[ "$actual" = 125 ] || exit 93
set +e
"$BB" sh /boaros-ltp-case.sh /boaros-case /boaros-ltp-skips.tsv 3 "$BB" /boaros-ordinary
actual=$?
set -e
[ "$actual" = 7 ] || exit 94
echo 'OSCOMP supervisor native contracts passed'
exit 0
''')
    config = directory / 'init.json'
    ordinary = directory / 'ordinary'
    ordinary.write_text('#!/musl/busybox sh\nprintf "unlisted native forwarded\\n"\nexit 7\n')
    config.write_text(json.dumps({'path': '/musl/busybox', 'argv': ['/musl/busybox', 'sh', '/init'], 'envp': []}))
    run(['make', arch.kernel, 'INIT_CONFIG=' + str(config)], cwd=ROOT, stdout=subprocess.DEVNULL)
    kernel = directory / arch.kernel
    shutil.copyfile(ROOT / arch.kernel, kernel)
    linker = 'tests/riscv/user_elf.ld' if args.arch == 'riscv' else 'tests/loongarch/user.ld'
    linux_init = directory / 'linux-init'
    run([arch.compiler, *arch.raw_flags, '-O2', '-ffreestanding', '-fno-builtin',
         '-fno-stack-protector', '-nostdlib', '-static', '-no-pie',
         '-DROOT_CREATE_SESSION', '-Wl,--build-id=none', '-Wl,-z,max-page-size=' + str(arch.page_size),
         '-Wl,-T,' + linker, *(['-DROOT_DIRECT_FILESYSTEM'] if args.arch == 'riscv' else []),
         '-o', linux_init, 'tests/loongarch/root_linux_init.c', 'tests/common/user_start.S'], cwd=ROOT)
    ramdisk = directory / 'initramfs.gz'
    ramdisk.write_bytes(archive([('dev', 0o040755, b'', 0, 0),
        ('dev/console', 0o020600, b'', 5, 1), ('init', 0o100755, linux_init.read_bytes(), 0, 0),
        ('TRAILER!!!', 0, b'', 0, 0)]))
    observations = []
    for memory in ('512M', '1G'):
        for operating_system in ('linux', 'boaros'):
            disk = directory / (operating_system + '-' + memory + '.img')
            run(['cp', '--reflink=auto', '--sparse=always', source, disk])
            edits = ['mkdir /dev', 'mknod /dev/console c 5 1',
                     'write ' + str(helper) + ' /boaros-case', 'set_inode_field /boaros-case mode 0100755',
                     'write ' + str(init) + ' /init', 'set_inode_field /init mode 0100755']
            edits += ['write ' + str(ROOT / 'tests/oscomp/ltp-case.sh') + ' /boaros-ltp-case.sh',
                      'write ' + str(ROOT / 'tests/oscomp/ltp-skips.tsv') + ' /boaros-ltp-skips.tsv',
                      'write ' + str(ordinary) + ' /boaros-ordinary',
                      'set_inode_field /boaros-ordinary mode 0100755']
            if operating_system == 'linux' and args.arch == 'riscv':
                edits += ['write ' + str(linux_init) + ' /linux-init', 'set_inode_field /linux-init mode 0100755']
            for edit in edits:
                run(['debugfs', '-w', '-R', edit, disk], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            image = arch.linux_kernel() if operating_system == 'linux' else kernel
            command = arch.boot(arch.qemu, image, memory) + ['-net', 'none', '-drive',
                'file=' + str(disk) + ',if=none,format=raw,id=root', '-device', arch.block('modern')]
            if operating_system == 'linux':
                command += (['-append', 'console=ttyS0 root=/dev/vda rw init=/linux-init loglevel=3']
                    if args.arch == 'riscv' else ['-initrd', str(ramdisk), '-append', 'console=ttyS0 rdinit=/init loglevel=3'])
            log = directory / (operating_system + '-' + memory + '.log')
            with log.open('wb') as output:
                result = subprocess.run(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT, timeout=90)
            text = log.read_text(errors='replace')
            good = result.returncode == 0 and 'OSCOMP supervisor native contracts passed' in text
            good &= 'native exit' in text and 'group survived' in text and 'wait_status=9' in text
            good &= 'BOAROS-CASE SKIP command=/native/cgroup_fj_proc' in text
            good &= 'unlisted native forwarded' in text
            good &= ('Linux ' + ('RV' if args.arch == 'riscv' else 'LA') + ' root application passed' in text
                     if operating_system == 'linux' else arch.root_success(text, 0))
            if not good:
                raise RuntimeError('native supervisor contract failed: ' + str(log))
            observations.append({'os': operating_system, 'memory': memory,
                                 'qemu_command': command, 'log_sha256': hashlib.sha256(log.read_bytes()).hexdigest()})
            print(args.arch, operating_system, memory, 'supervisor contracts passed', flush=True)
    (directory / 'result.json').write_text(json.dumps({'helper_sha256': hashlib.sha256(helper.read_bytes()).hexdigest(),
        'kernel_sha256': hashlib.sha256(kernel.read_bytes()).hexdigest(), 'runs': observations}, indent=2) + '\n')


if __name__ == '__main__':
    main()
