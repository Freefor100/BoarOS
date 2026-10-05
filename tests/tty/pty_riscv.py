#!/usr/bin/env python3
"""One PTY probe ELF on fixed Linux and BoarOS, reusing the TTY byte channel."""
import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('boaros_serial_tty', HERE / 'riscv.py')
serial_tty = importlib.util.module_from_spec(spec)
spec.loader.exec_module(serial_tty)
Serial, command, digest = serial_tty.Serial, serial_tty.command, serial_tty.digest


def build(directory, program):
    if program:
        destination = directory / 'probe-rv'
        shutil.copy2(program, destination)
        return destination
    compiler = ROOT / 'build/riscv/musl-root/bin/musl-gcc'
    destination = directory / 'probe-rv'
    command(str(compiler), '-fno-link-libatomic', '-static', '-O2', '-Wall', '-Wextra',
            '-Werror', str(HERE / 'pty_probe.c'), '-o', str(destination))
    return destination


def glibc_program(directory):
    source = ROOT / 'tests/userland/glibc/run.py'
    module_spec = importlib.util.spec_from_file_location('boaros_pinned_glibc', source)
    module = importlib.util.module_from_spec(module_spec); module_spec.loader.exec_module(module)
    inputs = module.checked_inputs()
    compiler = next(path for path in inputs['tools'] if path.endswith('-gcc'))
    program = directory / 'glibc-api-rv'
    command(compiler, '-O2', '-Wall', '-Wextra', '-Werror', '-no-pie',
            '-Wl,--dynamic-linker=' + module.INTERPRETER,
            str(HERE / 'pty_probe.c'), '-o', str(program))
    return program, inputs


def fixture(directory, program, args):
    tree = directory / 'tree'; tree.mkdir()
    for name in ('dev', 'proc', 'tmp', 'bin'):
        (tree / name).mkdir()
    shutil.copy2(program, tree / 'init')
    shutil.copy2(program, tree / 'pty-probe')
    if args.case == 'script':
        compiler = ROOT / 'build/riscv/musl-root/bin/musl-gcc'
        gate = directory / 'gate-rv'
        command(str(compiler), '-fno-link-libatomic', '-static', '-O2', '-Wall', '-Wextra',
                '-Werror', str(HERE / 'gate.c'), '-o', str(gate))
        shutil.copy2(gate, tree / 'gate')
    if args.case == 'libc':
        glibc, inputs = glibc_program(directory)
        shutil.copy2(glibc, tree / 'pty-glibc-api'); (tree / 'lib').mkdir()
        for path in inputs['runtime']:
            shutil.copy2(path, tree / 'lib' / Path(path).name)
        (directory / 'glibc-inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    (tree / 'pty-case').write_text(f'{args.case} {args.pairs} {args.bytes}\n')
    busybox = ROOT / 'build/program-environment/full-busybox/source/busybox/busybox'
    shutil.copy2(busybox, tree / 'busybox')
    for applet in ('sh', 'script', 'scriptreplay', 'stty', 'cat', 'sleep', 'printf', 'true', 'false'):
        (tree / 'bin' / applet).symlink_to('/busybox')
    image = directory / 'fixture.img'
    with image.open('wb') as stream:
        stream.truncate(64 * 1024 * 1024)
    command('mkfs.ext4', '-q', '-F', '-b', '4096', '-d', str(tree), str(image))
    nodes = directory / 'fixture.debugfs'
    nodes.write_text('cd /dev\nmknod console c 5 1\nset_inode_field console mode 020600\n')
    command('debugfs', '-w', '-f', str(nodes), str(image), stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
    return image


def script_artifacts(directory, work, disk):
    # Reuse the established journal-replay and filesystem error classification.
    source = ROOT / 'tests/offline-c-riscv.py'
    module_spec = importlib.util.spec_from_file_location('boaros_offline_fs', source)
    module = importlib.util.module_from_spec(module_spec); module_spec.loader.exec_module(module)
    module.replay_and_check(directory, work.name, disk)
    destination = work / 'script-files'; destination.mkdir()
    names = ('typescript', 'command.typescript', 'append.typescript', 'stderr.typescript',
             'megabyte.typescript', 'megabyte.replay', 'signaled.typescript', 'immediate.typescript',
             'script-default.stdout', 'script-command.stdout', 'script-quiet.stdout',
             'script-append.stdout', 'script-timing-stderr.stdout', 'script-timing-stderr.stderr',
             'script-megabyte.stdout', 'script-megabyte.timing', 'script-signaled.stdout',
             'script-immediate-tail.stdout', 'jobs.typescript', 'script-jobs.stdout',
             'script-jobs.stderr', 'script-jobs.pids')
    for name in names:
        if not module.extract(disk, '/tmp/' + name, destination / name):
            raise AssertionError('missing original script artifact: ' + name)
    def payload(size):
        return bytes((i * 37 + (i >> 8) + 23 * 53) & 255 for i in range(size))
    def equal(name, wanted):
        if (destination / name).read_bytes() != wanted:
            raise AssertionError('script byte content/tail mismatch: ' + name)
    equal('command.typescript', payload(257))
    equal('append.typescript', payload(127) + payload(131))
    equal('script-quiet.stdout', payload(127))
    equal('stderr.typescript', payload(4096))
    expected = payload(1048576)
    for name in ('megabyte.typescript', 'megabyte.replay', 'script-megabyte.stdout',
                 'immediate.typescript', 'script-immediate-tail.stdout'):
        equal(name, expected)
    if b'PTY_DEFAULT_OK' not in (destination / 'typescript').read_bytes():
        raise AssertionError('default original interactive shell did not execute input')
    jobs = (destination / 'jobs.typescript').read_bytes()
    for marker in (b'PTY_JOB_STTY_OK', b'TTY_GATE_SLEEP_READY', b'PTY_JOB_SLEEP_C_OK',
                   b'TTY_GATE_CPU', b'PTY_JOB_CPU_C_OK', b'Stopped',
                   b'TTY_GATE_BACKGROUND_RESUMED', b'TTY_GATE_FOREGROUND_RESUMED',
                   b'PTY_JOB_READER_STOPPED', b'PTY_JOB_BG_READ_STOPPED',
                   b'PTY_JOB_BG_REAPED', b'PTY_JOB_ALL_OK'):
        if marker not in jobs: raise AssertionError('missing original interactive script outcome: ' + marker.decode())
    for name, size in (('script-timing-stderr.stderr', 4096), ('script-megabyte.timing', 1048576)):
        total = 0; lines = (destination / name).read_text().splitlines()
        if not lines: raise AssertionError('empty original timing')
        for line in lines:
            fields = line.split()
            if len(fields) != 2: raise AssertionError('malformed original timing')
            delay = float(fields[0]); count = int(fields[1])
            if not math.isfinite(delay) or delay < 0 or count <= 0:
                raise AssertionError('invalid original timing value')
            total += count
        if total != size: raise AssertionError('original timing does not cover full typescript')


def recording_reboot(args, work, disk, invocation, kernel):
    # The guest selected this check; host journal tools have not touched its disk.
    reboot = work / 'recording-reboot'; reboot.mkdir()
    image = reboot / 'root.img'; original_hash = digest(disk); shutil.copy2(disk, image)
    if digest(image) != original_hash: raise AssertionError('recording reboot input copy differs')
    second = list(invocation)
    second[second.index('-drive') + 1] = f'file={image},if=none,format=raw,id=root'
    identity = {'kernel': digest(kernel), 'original_shutdown_disk': original_hash,
                'boot_disk': digest(image), 'argv': second, 'host_replay_before_boot': False,
                'status': 'prepared'}
    (reboot / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    serial = Serial(second, reboot)
    try:
        result = serial.expect(rb'(?m)^(?:PTY_PROBE_PASS records=[0-9]+|PTY_RECORD FAIL[^\r\n]*)\r?\n', args.timeout)
        if b'PTY_RECORD FAIL' in result:
            raise AssertionError('recording reboot: ' + result.decode(errors='replace').strip())
        serial.finish()
    finally:
        serial.close()
    text = bytes(serial.data)
    records = re.findall(rb'(?m)^PTY_RECORD[^\r\n]*', text)
    expected = [b'PTY_RECORD recordings-after-real-reboot-exact-content-tail 1048576']
    if records != expected or not re.search(rb'PTY_PROBE_PASS records=1\r?\n', text):
        raise AssertionError('recording reboot did not execute the complete guest check')
    if work.name.startswith('boaros-'):
        match = re.search(rb'exited status=0x([0-9a-f]+) .*heap-live=0x0; shutting down', text)
        if not match or int(match.group(1), 16) != 42:
            raise AssertionError('recording reboot actual BoarOS status/heap cleanup')
        guest_status = int(match.group(1), 16)
    else:
        match = re.search(rb'Attempted to kill init! exitcode=0x([0-9a-fA-F]+)', text)
        if not match or int(match.group(1), 16) != (42 << 8):
            raise AssertionError('recording reboot actual Linux init status unavailable/incorrect')
        guest_status = int(match.group(1), 16) >> 8
    identity.update(status='passed', qemu_status=serial.process.returncode, guest_status=guest_status)
    (reboot / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    (reboot / 'records.txt').write_bytes(b'\n'.join(records) + b'\n')
    print(work.name, 'recording reboot PASS', len(records), 'records, actual init status', guest_status, flush=True)
    return records


def linux_image(argument):
    sys.path.insert(0, str(ROOT / 'tests/diff-abi'))
    import harness
    if argument:
        image, identity = harness.fixed_linux_image(argument)
        if identity['inputs']['config'] != digest(HERE / 'pty-linux.config'):
            raise RuntimeError('fixed Linux image differs from the independent PTY profile')
        # Ordinary ABI's allnoconfig omits Unix98 PTYs; never silently use it.
        config = next((ancestor / '.config' for ancestor in image.parents
                       if (ancestor / '.config').is_file()), None)
        if config is None or 'CONFIG_UNIX98_PTYS=y' not in config.read_text():
            raise RuntimeError('fixed Linux image must enable CONFIG_UNIX98_PTYS')
        return image, identity
    return harness.linux_build(HERE / 'pty-linux.config')


def run(args):
    base = ROOT / 'build/riscv'; base.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='pty-run.', dir=base))
    print('PTY artifacts:', directory, flush=True)
    program = build(directory, args.program)
    image = fixture(directory, program, args)
    variants = []
    if args.only != 'boaros':
        reference, reference_identity = linux_image(args.linux_kernel)
        variants.append(('linux', reference, reference_identity))
    if args.only != 'linux': variants.append(('boaros', args.kernel, None))
    prepared = []
    # Freeze every kernel and input before the first startup.
    for name, kernel, reference_identity in variants:
        snapshot = directory / (name + '-kernel'); shutil.copy2(kernel, snapshot)
        prepared.append((name, snapshot, reference_identity))
    identity = {'case': args.case, 'program': digest(program), 'fixture': digest(image),
                'source': digest(HERE / 'pty_probe.c'), 'linux_config': digest(HERE / 'pty-linux.config'),
                'busybox': digest(directory / 'tree/busybox'),
                'kernels': {name: digest(kernel) for name, kernel, _ in prepared},
                'transport': args.transport, 'pairs': args.pairs, 'bytes': args.bytes,
                'replicas': args.replicas, 'status': 'prepared'}
    identity['qemu'] = command(args.qemu, '--version', capture_output=True, text=True).stdout.splitlines()[0]
    identity['qemu_sha256'] = digest(shutil.which(args.qemu))
    if args.case == 'libc': identity['glibc_api'] = digest(directory / 'glibc-api-rv')
    if args.case == 'script': identity['gate'] = digest(directory / 'gate-rv')
    (directory / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    if args.prepare_only:
        print('PTY prepared program:', program, flush=True)
        return
    observed = {}; reboot_observed = {}
    for name, kernel, _reference_identity in prepared:
        for replica in range(args.replicas):
            work = directory / f'{name}-{replica}'; work.mkdir()
            disk = work / 'root.img'; shutil.copy2(image, disk)
            invocation = [args.qemu, '-machine', 'virt', '-bios', 'default', '-kernel', str(kernel),
                          '-m', '512M', '-smp', '1', '-display', 'none', '-monitor', 'none',
                          '-serial', 'stdio', '-no-reboot', '-drive',
                          f'file={disk},if=none,format=raw,id=root', '-device',
                          'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0', '-global',
                          'virtio-mmio.force-legacy=' + ('true' if args.transport == 'legacy' else 'false')]
            if name == 'linux':
                invocation += ['-append', 'root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
            (work / 'argv.json').write_text(json.dumps(invocation, indent=2) + '\n')
            serial = Serial(invocation, work)
            try:
                result = serial.expect(rb'(?m)^(?:PTY_PROBE_PASS records=[0-9]+|PTY_RECORD FAIL[^\r\n]*)\r?\n', args.timeout)
                if b'PTY_RECORD FAIL' in result:
                    raise AssertionError(f'{name}: {result.decode(errors="replace").strip()}')
                serial.finish()
            finally:
                serial.close()
            text = bytes(serial.data)
            records = re.findall(rb'(?m)^PTY_RECORD[^\r\n]*', text)
            if not records or any(b' FAIL ' in row for row in records):
                raise AssertionError(f'{name} invalid PTY observations')
            count = int(re.search(rb'PTY_PROBE_PASS records=([0-9]+)', text).group(1))
            if len(records) != count: raise AssertionError('incomplete PTY record stream')
            if name == 'boaros' and not re.search(rb'exited status=0x2a .*heap-live=0x0; shutting down', text):
                raise AssertionError('BoarOS PTY task/heap cleanup incomplete')
            if args.case == 'script':
                if name == 'linux' and not re.search(rb'Attempted to kill init! exitcode=0x0*2a00', text):
                    raise AssertionError('original script coordinator Linux actual status is not42')
                reboot_records = recording_reboot(args, work, disk, invocation, kernel)
                if name in reboot_observed and reboot_observed[name] != reboot_records:
                    raise AssertionError('recording reboot replicas differ')
                reboot_observed[name] = reboot_records
                script_artifacts(directory, work, disk)
            timings = re.findall(rb'(?m)^(?:PTY_(?:TIMING|DURABLE|PROGRESS|CPU|MEMORY|SCRIPT_ADOPTED)|ROOT (?:DRAIN|HEAP) FIXTURE)[^\r\n]*', text)
            (work / 'records.txt').write_bytes(b'\n'.join(records) + b'\n')
            (work / 'timings.txt').write_bytes(b'\n'.join(timings) + b'\n')
            if name in observed and observed[name] != records:
                raise AssertionError('independent PTY replicas differ')
            observed[name] = records
            print(name, replica, 'PTY PASS', count, 'records', flush=True)
    if 'linux' in observed and 'boaros' in observed and observed['linux'] != observed['boaros']:
        raise AssertionError(f'PTY differential mismatch: {observed}')
    if 'linux' in reboot_observed and 'boaros' in reboot_observed and reboot_observed['linux'] != reboot_observed['boaros']:
        raise AssertionError('recording reboot Linux/BoarOS differential mismatch')
    identity['status'] = 'passed'
    (directory / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--case', choices=('red', 'core', 'libc', 'script', 'recording-check', 'performance'), default='core')
    parser.add_argument('--only', choices=('both', 'linux', 'boaros'), default='both')
    parser.add_argument('--kernel', type=Path, default=ROOT / 'kernel-rv')
    parser.add_argument('--linux-kernel', type=Path)
    parser.add_argument('--program', type=Path)
    parser.add_argument('--qemu', default='qemu-system-riscv64')
    parser.add_argument('--transport', choices=('modern', 'legacy'), default='modern')
    parser.add_argument('--replicas', type=int, default=1)
    parser.add_argument('--pairs', type=int, choices=(1, 8), default=1)
    parser.add_argument('--bytes', type=int, default=4 * 1024 * 1024)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    if args.replicas < 1 or args.bytes < 1: parser.error('positive replicas and byte count required')
    run(args)


if __name__ == '__main__': main()
