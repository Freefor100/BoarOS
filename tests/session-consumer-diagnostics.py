#!/usr/bin/env python3
"""Same fixed original RV ELF inputs on Linux/BoarOS; diagnostic, never a score."""
import argparse
import hashlib
import json
import os
import re
from collections import Counter
from pathlib import Path
import subprocess
import shutil
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
    parser.add_argument('--linux-kernel', type=Path)
    parser.add_argument('--only', choices=['linux', 'boaros'])
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
                    '-Wall', '-Wextra', '-Werror', str(ROOT / 'tests/workloads/diagnostics/session-consumers.c'),
                    '-o', str(program)], check=True)
    original_files = {}
    for source in ['/musl/iperf3', '/musl/cyclictest']:
        target = out / Path(source).name
        subprocess.run(['debugfs', '-R', f'dump {source} {target}', str(original)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        original_files[source] = digest(target)
    manifest = json.loads((ROOT / 'build/program-environment/full-busybox/build.json').read_text())
    busybox = Path(manifest['binary'])
    if digest(busybox) != manifest['binary_sha256'] or manifest['binary_sha256'] != 'f2cda5fcdff6d41c8a553ac658e8aa55b6a48aa40898cb123a19f7865f3773ac':
        raise RuntimeError('fixed full BusyBox binary mismatch')
    original_files['/busybox'] = digest(busybox)
    plugin = out / 'syscall-entry.so'
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True).split()
    subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
        str(ROOT / 'tests/workloads/diagnostics/syscall-entry-plugin.c'), '-o', str(plugin), *flags], check=True)
    kernel_snapshot = out / 'boaros-kernel'
    shutil.copyfile(args.kernel, kernel_snapshot)
    linux, linux_identity = harness.fixed_linux_image(args.linux_kernel)
    fixture = out / 'fixture.img'
    subprocess.run(['cp', '--sparse=always', '--reflink=auto', str(original), str(fixture)], check=True)
    commands = out / 'fixture.commands'
    commands.write_text(f'write {program} /init\nset_inode_field /init mode 0100755\n'
                        f'write {busybox} /busybox\nset_inode_field /busybox mode 0100755\n')
    subprocess.run(['debugfs', '-w', '-f', str(commands), str(fixture)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with fixture.open('rb') as stream:
        os.fsync(stream.fileno())
    metadata = {'original_sha256': identity, 'original_files': original_files,
                'plugin_sha256': digest(plugin), 'program_sha256': digest(program),
                'sources': {str(p.relative_to(ROOT)): digest(p) for p in [Path(__file__), ROOT / 'tests/workloads/diagnostics/session-consumers.c', ROOT / 'tests/workloads/diagnostics/syscall-entry-plugin.c']},
                'busybox_build': manifest, 'linux': linux_identity, 'boaros_sha256': digest(kernel_snapshot), 'runs': {}}
    qemu = os.environ.get('QEMU_RISCV64', 'qemu-system-riscv64')
    metadata['qemu'] = subprocess.check_output([qemu, '--version'], text=True)
    for name, kernel in [('linux', linux), ('boaros', kernel_snapshot)]:
        if args.only and name != args.only:
            continue
        disk = out / (name + '.img')
        subprocess.run(['cp', '--sparse=always', '--reflink=auto', str(fixture), str(disk)], check=True)
        with disk.open('rb') as stream:
            os.fsync(stream.fileno())
        command = [qemu, '-machine', 'virt', '-bios', 'default',
                   '-object', 'rng-random,id=entropy,filename=/dev/urandom',
                   '-device', 'virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.7', '-kernel', str(kernel),
                   '-plugin', str(plugin) + ',trace=' + str(out / (name + '.ecall')), '-m', '512M', '-smp', '1', '-nographic', '-no-reboot',
                   '-drive', f'file={disk},if=none,format=raw,id=root',
                   '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
        if name == 'linux':
            command += ['-append', 'root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
        log = out / (name + '.log')
        try:
            harness.run_logged(command, log, 40)
            status = 'finished'
        except (RuntimeError, TimeoutError) as error:
            status = str(error)
        raw = log.read_text(errors='replace')
        records = [line for line in raw.splitlines() if line.startswith('PROBE ')]
        groups = {}; active = None
        for line in (out / (name + '.ecall')).read_text().splitlines():
            match = re.fullmatch(r'ECALL pc=([0-9a-f]+) nr=(\d+) a0=([0-9a-f]+) a1=([0-9a-f]+) a2=([0-9a-f]+)', line)
            if not match:
                raise RuntimeError('malformed syscall trace')
            nr, a0 = int(match[2]), int(match[3], 16)
            signed = a0 if a0 < 2**63 else a0 - 2**64
            if nr == 155 and -10099 <= signed <= -10001:
                active = str(-10000 - signed); groups[active] = Counter()
            elif nr == 155 and -20099 <= signed <= -20001:
                active = None
            elif active:
                groups[active][nr] += 1
        metadata['runs'][name] = {'status': status, 'command': command, 'records': records,
                                  'syscall_entry_counts_by_case_id': groups}
        (out / 'report.json').write_text(json.dumps(metadata, indent=2) + '\n')
        print(name, status, '\n' + '\n'.join(records), flush=True)
        if 'PROBE complete' not in raw:
            print(f'{name} incomplete diagnostic: {log}', flush=True)


if __name__ == '__main__':
    main()
