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
PROFILES = serial_tty.PROFILES


def compile_static(args, source, destination):
    compiler = ROOT / ('build/riscv/musl-root/bin/musl-gcc' if args.arch == 'riscv'
                       else 'build/loongarch/musl-root/bin/musl-gcc')
    environment = os.environ.copy()
    flags = PROFILES[args.arch].musl_flags(compiler)
    if args.arch == 'loongarch':
        environment['REALGCC'] = str(ROOT / 'build/loongarch/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc')
    command(str(compiler), *flags, '-static', '-O2', '-Wall', '-Wextra', '-Werror',
            str(source), '-o', str(destination), env=environment)


def build(directory, args):
    destination = directory / 'probe'
    if args.program: shutil.copy2(args.program, destination)
    else: compile_static(args, HERE / 'pty_probe.c', destination)
    return destination


def glibc_program(directory, args):
    sys.path.insert(0, str(ROOT / 'tests/userland/glibc'))
    from profiles import checked_inputs
    inputs = checked_inputs(args.arch)
    compiler = next(path for path in inputs['tools'] if path.endswith('-gcc'))
    program = directory / 'glibc-api'
    interpreter = inputs.get('interpreter', '/lib/ld-linux-riscv64-lp64d.so.1')
    command(compiler, *inputs.get('compiler_flags', []), '-O2', '-Wall', '-Wextra',
            '-Werror', '-no-pie', '-Wl,--dynamic-linker=' + interpreter,
            str(HERE / 'pty_probe.c'), '-o', str(program))
    return program, inputs


def fixture(directory, program, args):
    tree = directory / 'tree'; tree.mkdir()
    for name in ('dev', 'proc', 'tmp', 'bin'):
        (tree / name).mkdir()
    shutil.copy2(program, tree / 'init')
    shutil.copy2(program, tree / 'pty-probe')
    if args.case == 'script':
        gate = directory / 'gate'
        compile_static(args, HERE / 'gate.c', gate)
        shutil.copy2(gate, tree / 'gate')
    if args.case == 'libc':
        glibc, inputs = glibc_program(directory, args)
        shutil.copy2(glibc, tree / 'pty-glibc-api')
        library = tree / inputs.get('library_directory', '/lib').lstrip('/')
        library.mkdir(parents=True)
        for path in inputs['runtime']:
            shutil.copy2(path, library / Path(path).name)
        (directory / 'glibc-inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    (tree / 'pty-case').write_text(f'{args.case} {args.pairs} {args.bytes}\n')
    busybox = ROOT / ('build/program-environment/full-busybox/source/busybox/busybox' if args.arch == 'riscv' else 'build/loongarch/busybox-source/busybox/busybox')
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
        if not PROFILES[args.arch].root_success(text.decode(errors='replace'), 42):
            raise AssertionError('recording reboot actual BoarOS status/heap cleanup')
        guest_status = 42
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
    base = ROOT / ('build/riscv' if args.arch == 'riscv' else 'build/loongarch'); base.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='pty-run.', dir=base))
    print('PTY artifacts:', directory, flush=True)
    program = build(directory, args)
    image = fixture(directory, program, args)
    variants = []
    if args.only != 'boaros':
        reference, reference_identity = (linux_image(args.linux_kernel) if args.arch == 'riscv' else
            (args.linux_kernel or PROFILES[args.arch].linux_kernel(), json.loads((PROFILES[args.arch].linux_kernel().parent / 'boaros-identity.json').read_text())))
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
                'arch': args.arch, 'memory': args.memory, 'replicas': args.replicas, 'status': 'prepared'}
    identity['qemu'] = command(args.qemu, '--version', capture_output=True, text=True).stdout.splitlines()[0]
    identity['qemu_sha256'] = digest(shutil.which(args.qemu))
    if args.case == 'libc': identity['glibc_api'] = digest(directory / 'glibc-api')
    if args.case == 'script': identity['gate'] = digest(directory / 'gate')
    (directory / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    if args.prepare_only:
        print('PTY prepared program:', program, flush=True)
        return
    observed = {}; reboot_observed = {}
    for name, kernel, _reference_identity in prepared:
        for memory, replica in ((memory, replica) for memory in args.memory for replica in range(args.replicas)):
            work = directory / f'{name}-{memory}-{replica}'; work.mkdir()
            disk = work / 'root.img'; shutil.copy2(image, disk)
            invocation = [args.qemu, '-machine', 'virt', '-kernel', str(kernel),
                          '-m', memory, '-smp', '1', '-display', 'none', '-monitor', 'none',
                          '-serial', 'stdio', '-net', 'none', '-no-reboot', '-drive',
                          f'file={disk},if=none,format=raw,id=root', '-device',
                          PROFILES[args.arch].block(args.transport)]
            if args.arch == 'loongarch': invocation += ['-cpu', 'la464']
            else: invocation += ['-bios', 'default', '-global',
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
            if name == 'boaros' and not PROFILES[args.arch].root_success(text.decode(errors='replace'), 42):
                raise AssertionError('BoarOS PTY task/heap cleanup incomplete')
            if args.case == 'script':
                if name == 'linux' and not re.search(rb'Attempted to kill init! exitcode=0x0*2a00', text):
                    raise AssertionError('original script coordinator Linux actual status is not42')
                reboot_records = recording_reboot(args, work, disk, invocation, kernel)
                if name in reboot_observed and reboot_observed[name] != reboot_records:
                    raise AssertionError('recording reboot replicas differ')
                reboot_observed[name] = reboot_records
                script_artifacts(directory, work, disk)
            if args.case == 'performance':
                timing = re.findall(rb'(?m)^PTY_TIMING pairs=([0-9]+) bytes_each_direction=([0-9]+) elapsed_ns=([0-9]+)\r?$', text)
                if len(timing) != 1 or tuple(map(int, timing[0][:2])) != (args.pairs, args.bytes) or int(timing[0][2]) <= 0:
                    raise AssertionError('missing or interleaved PTY payload timing')
                progress = re.findall(rb'(?m)^PTY_PROGRESS pair=([0-9]+) direction=([01]) bytes=([0-9]+) complete_ns=([0-9]+)\r?$', text)
                if len(progress) != args.pairs * 2 or {(int(p), int(d), int(n)) for p, d, n, _ in progress} != {
                        (p, d, args.bytes) for p in range(args.pairs) for d in (0, 1)}:
                    raise AssertionError('missing per-direction PTY completion')
                if len(re.findall(rb'(?m)^PTY_DURABLE scope=sync elapsed_ns=[0-9]+\r?$', text)) != 1:
                    raise AssertionError('missing explicit PTY sync boundary')
                drains = re.findall(rb'(?m)^ROOT DRAIN FIXTURE ticks=0x[0-9a-f]+ status=0x0\r?$', text)
                if b'ROOT DRAIN FIXTURE' in text and len(drains) != 1:
                    raise AssertionError('root fixture did not identify a single successful disk drain')
            timings = re.findall(rb'(?m)^(?:PTY_(?:TIMING|DURABLE|PROGRESS|CPU|MEMORY|SCRIPT_ADOPTED)|ROOT (?:DRAIN|HEAP) FIXTURE)[^\r\n]*', text)
            (work / 'records.txt').write_bytes(b'\n'.join(records) + b'\n')
            (work / 'timings.txt').write_bytes(b'\n'.join(timings) + b'\n')
            if name in observed and observed[name] != records:
                raise AssertionError('independent PTY replicas differ')
            observed[name] = records
            print(name, memory, replica, 'PTY PASS', count, 'records', flush=True)
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
    parser.add_argument('--arch', choices=tuple(PROFILES), default='riscv')
    parser.add_argument('--memory', choices=('512M', '1G'), action='append')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--linux-kernel', type=Path)
    parser.add_argument('--program', type=Path)
    parser.add_argument('--qemu')
    parser.add_argument('--transport', choices=('modern', 'legacy'), default='modern')
    parser.add_argument('--replicas', type=int, default=1)
    parser.add_argument('--pairs', type=int, choices=(1, 8), default=1)
    parser.add_argument('--bytes', type=int, default=4 * 1024 * 1024)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    profile = PROFILES[args.arch]
    args.kernel = args.kernel or ROOT / profile.kernel; args.qemu = args.qemu or profile.qemu
    args.memory = args.memory or (['512M', '1G'] if args.arch == 'loongarch' else ['512M'])
    if args.arch == 'loongarch' and args.transport == 'legacy': parser.error('LA uses modern PCI')
    if args.replicas < 1 or args.bytes < 1: parser.error('positive replicas and byte count required')
    run(args)


if __name__ == '__main__': main()
