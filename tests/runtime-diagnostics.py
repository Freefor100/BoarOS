#!/usr/bin/env python3
"""Same fixed original RV ELF inputs on Linux/BoarOS; diagnostic, never a score."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import shlex
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/diff-abi'))
import harness


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compat-release', choices=['4.15.0'], help='isolated uname variant for the fixed old glibc; never edits main')
    parser.add_argument('--kernel', type=Path, default=ROOT / 'kernel-rv')
    args = parser.parse_args()
    out = args.output.resolve()
    if not out.is_relative_to(ROOT / 'build') or out.exists():
        parser.error('output must be a new directory under build/')
    out.mkdir(parents=True)
    original = ROOT / 'references/oscomp-autotest/sdcard-rv.img'
    identity = digest(original)
    if identity != 'f419468678d342133546add2f8459ea09aeba987ba968e28753d6ee656996b8b':
        raise RuntimeError('fixed RV image identity mismatch')
    program = out / 'init'
    compiler = str(ROOT / 'build/riscv/musl-root/bin/musl-gcc')
    flags = []
    if subprocess.run([compiler, '-fno-link-libatomic', '-E', '-x', 'c', '/dev/null'],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
        flags.append('-fno-link-libatomic')
    subprocess.run([compiler, *flags, '-static', '-O2',
                    '-Wall', '-Wextra', '-Werror', str(ROOT / 'tests/workloads/diagnostics/runtime.c'),
                    '-o', str(program)], check=True)
    original_files = {}
    for name, source in [('libc.so.6', '/glibc/lib/libc.so.6'),
                         ('loader', '/glibc/lib/ld-linux-riscv64-lp64d.so.1'),
                         ('entry-static.exe', '/glibc/entry-static.exe'),
                         ('entry-dynamic.exe', '/glibc/entry-dynamic.exe'),
                         ('cgroup_fj_proc', '/glibc/ltp/testcases/bin/cgroup_fj_proc')]:
        target = out / name
        subprocess.run(['debugfs', '-R', f'dump {source} {target}', str(original)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        original_files[source] = digest(target)
    cross = 'riscv64-linux-gnu-gcc'
    def crt(name):
        return subprocess.check_output([cross, '-print-file-name=' + name], text=True).strip()
    time_probe = out / 'time-probe'
    subprocess.run([cross, *flags, '-nostdlib', '-no-pie', '-O2', '-Wall', '-Wextra', '-Werror',
                    crt('crt1.o'), crt('crti.o'), crt('crtbegin.o'),
                    str(ROOT / 'tests/workloads/diagnostics/time.c'),
                    str(out / 'libc.so.6'), str(out / 'loader'), '-lgcc',
                    crt('crtend.o'), crt('crtn.o'),
                    '-Wl,--dynamic-linker=/lib/ld-linux-riscv64-lp64d.so.1',
                    '-o', str(time_probe)], check=True)
    kernel_snapshot = out / 'boaros-kernel'
    shutil.copyfile(args.kernel, kernel_snapshot)
    if args.compat_release:
        if args.kernel.resolve() != ROOT / 'kernel-rv':
            parser.error('compat variant requires the current default build')
        source = (ROOT / 'kernel/syscall/dispatch.c').read_text()
        old = '.release = "0.1.0-boaros-dev"'
        if source.count(old) != 1:
            raise RuntimeError('unexpected uname source; inspect before adapting diagnostic')
        variant_source = out / 'dispatch.c'
        variant_source.write_text(source.replace(old, '.release = "4.15.0"'))
        commands = subprocess.check_output(['make', '-s', '-n', '-W', 'kernel/syscall/dispatch.c', 'all'], cwd=ROOT, text=True)
        commands = commands.replace(chr(92) + chr(10), '')
        compile_command = link_command = None
        for line in commands.splitlines():
            parts = shlex.split(line)
            if '-c' in parts and 'kernel/syscall/dispatch.c' in parts:
                compile_command = [str(variant_source) if p == 'kernel/syscall/dispatch.c' else
                    str(out / 'dispatch.o') if p == 'build/riscv/kernel/syscall/dispatch.o' else p for p in parts]
                compile_command += ['-I' + str(ROOT / 'kernel/syscall')]
            if '-o' in parts and 'kernel-rv' in parts:
                link_command = [str(kernel_snapshot) if p == 'kernel-rv' else
                    str(out / 'dispatch.o') if p == 'build/riscv/kernel/syscall/dispatch.o' else
                    '-Wl,-Map,' + str(out / 'kernel.map') if p.startswith('-Wl,-Map,') else p for p in parts]
        if not compile_command or not link_command:
            raise RuntimeError('could not derive isolated diagnostic build commands')
        with (out / 'variant-build.log').open('w') as log:
            for command in [compile_command, link_command]:
                subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    linux, linux_identity = harness.fixed_linux_image(None)
    fixture = out / 'fixture.img'
    subprocess.run(['cp', '--sparse=always', '--reflink=auto', str(original), str(fixture)], check=True)
    commands = out / 'fixture.commands'
    commands.write_text(f'write {program} /init\nset_inode_field /init mode 0100755\n'
                        f'write {time_probe} /time-probe\nset_inode_field /time-probe mode 0100755\n')
    subprocess.run(['debugfs', '-w', '-f', str(commands), str(fixture)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with fixture.open('rb') as stream:
        os.fsync(stream.fileno())
    metadata = {'original_sha256': identity, 'original_files': original_files,
                'time_probe_sha256': digest(time_probe), 'program_sha256': digest(program),
                'linux': linux_identity, 'boaros_sha256': digest(kernel_snapshot), 'compat_release': args.compat_release, 'runs': {}}
    qemu = os.environ.get('QEMU_RISCV64', 'qemu-system-riscv64')
    metadata['qemu'] = subprocess.check_output([qemu, '--version'], text=True)
    for name, kernel in [('linux', linux), ('boaros', kernel_snapshot)]:
        disk = out / (name + '.img')
        subprocess.run(['cp', '--sparse=always', '--reflink=auto', str(fixture), str(disk)], check=True)
        with disk.open('rb') as stream:
            os.fsync(stream.fileno())
        command = [qemu, '-machine', 'virt', '-bios', 'default',
                   '-object', 'rng-random,id=entropy,filename=/dev/urandom',
                   '-device', 'virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.7', '-kernel', str(kernel),
                   '-m', '512M', '-smp', '1', '-nographic', '-no-reboot',
                   '-drive', f'file={disk},if=none,format=raw,id=root',
                   '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
        if name == 'linux':
            command += ['-append', 'root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
        log = out / (name + '.log')
        try:
            harness.run_logged(command, log, 180)
            status = 'finished'
        except (RuntimeError, TimeoutError) as error:
            status = str(error)
        raw = log.read_text(errors='replace')
        records = [line for line in raw.splitlines() if line.startswith('PROBE ')]
        metadata['runs'][name] = {'status': status, 'command': command, 'records': records}
        (out / 'report.json').write_text(json.dumps(metadata, indent=2) + '\n')
        print(name, status, '\n' + '\n'.join(records), flush=True)
        if 'PROBE complete' not in raw:
            raise RuntimeError(f'{name} diagnostic did not complete: {log}')


if __name__ == '__main__':
    main()
