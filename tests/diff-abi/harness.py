#!/usr/bin/env python3
"""Run one ELF against two system kernels; reject incomplete observations."""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from timestamps import normalize_timestamps, normalize_utimens

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BUILD = ROOT / 'build' / 'diff-abi'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def output(command):
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT).strip()


def run_logged(command, logfile, timeout=None):
    with Path(logfile).open('w') as stream:
        try:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise TimeoutError(f'timeout after {timeout}s: {command[0]}; see {logfile}') from error
    if result.returncode:
        raise RuntimeError(f'{command[0]} returned {result.returncode}; see {logfile}')


def normalize(text, expected):
    lines = [line for line in text.replace('\r\n', '\n').splitlines() if line.startswith('ABI ')]
    if not lines or lines[0] != 'ABI BEGIN 1' or lines[-1] != f'ABI END {len(expected)}':
        raise ValueError('missing, malformed, or incomplete ABI stream')
    records = []
    for line in lines[1:-1]:
        match = re.fullmatch(r'ABI ([a-zA-Z0-9_.-]+) (-?\d+) (\d+) (-?\d+) (-?\d+) (\d+) (-|(?:[0-9a-f]{2})+)', line)
        if not match:
            raise ValueError(f'malformed ABI record: {line}')
        name, ret, errno, size, offset, status, data = match.groups()
        ret, errno, size, offset, status = map(int, (ret, errno, size, offset, status))
        if errno != (-ret if -4095 <= ret < 0 else 0):
            raise ValueError(f'inconsistent return/errno: {name}')
        if name.startswith('time.'):
            data = normalize_timestamps(data)
        elif name.startswith('utime.'):
            data = normalize_utimens(data, ret)
        records.append((name, ret, errno, size, offset, status, data))
    if [record[0] for record in records] != expected:
        raise ValueError('missing, duplicated, unknown, or reordered case result')
    return [' '.join(map(str, record)) + '\n' for record in records]


def compare(reference, actual):
    delta = ''.join(difflib.unified_diff(reference, actual, fromfile='linux', tofile='boaros'))
    return not delta, delta


def source_info():
    rows = [line.split('\t') for line in (ROOT / 'references/sources.tsv').read_text().splitlines()
            if line.startswith('snapshot\tlinux\t')]
    if len(rows) != 1:
        raise RuntimeError('Linux reference must have one manifest entry')
    return rows[0][2], rows[0][4]


def identity(config=None):
    config = Path(config) if config is not None else HERE / 'linux.config'
    compiler = os.environ.get('LINUX_CC', 'riscv64-linux-gnu-gcc')
    prefix = compiler.removesuffix('gcc')
    identities = {}
    for tool in (compiler, prefix + 'ld', prefix + 'as', 'gcc', 'make', 'flex', 'bison', 'bc'):
        path = shutil.which(tool)
        if not path:
            raise RuntimeError(f'missing build tool: {tool}')
        identities[tool] = {'version': output([tool, '--version']), 'sha256': digest(path)}
    data = {'source': list(source_info()), 'config': digest(config),
            'builder': digest(__file__), 'tools': identities}
    key = hashlib.sha256(json.dumps(data, sort_keys=True).encode()).hexdigest()
    return key, data, prefix


def linux_build(config=None):
    config = Path(config).resolve() if config is not None else HERE / 'linux.config'
    key, data, prefix = identity(config)
    destination = BUILD / 'linux' / key
    image = destination / 'arch/riscv/boot/Image'
    metadata = destination / 'identity.json'
    if image.exists() and metadata.exists():
        saved = json.loads(metadata.read_text())
        if saved['inputs'] == data and saved['image_sha256'] == digest(image) and saved['config_sha256'] == digest(destination / '.config'):
            return image, saved
        raise RuntimeError(f'Linux cache integrity failure: {destination}')
    source = ROOT / 'references/linux'
    url, revision = source_info()
    if not source.exists():
        source.mkdir(parents=True)
        subprocess.run(['git', 'init', '-q', str(source)], check=True)
        subprocess.run(['git', '-C', str(source), 'remote', 'add', 'origin', url], check=True)
        subprocess.run(['git', '-C', str(source), 'fetch', '--depth=1', 'origin', revision], check=True)
        subprocess.run(['git', '-C', str(source), 'checkout', '--detach', revision], check=True)
    if output(['git', '-C', str(source), 'rev-parse', 'HEAD']) != revision or output(['git', '-C', str(source), 'status', '--porcelain']):
        raise RuntimeError('Linux reference revision differs or contains local modifications')
    if output(['git', '-C', str(source), 'remote', 'get-url', 'origin']) != url:
        raise RuntimeError('Linux reference origin differs from manifest')
    subprocess.run(['git', '-C', str(source), 'sparse-checkout', 'disable'], check=True)
    destination.mkdir(parents=True, exist_ok=True)
    command = ['make', '-C', str(source), 'O=' + str(destination), 'ARCH=riscv',
               'CROSS_COMPILE=' + prefix, 'HOSTCC=gcc', 'KBUILD_BUILD_USER=boaros',
               'KBUILD_BUILD_HOST=diff-abi', 'KBUILD_BUILD_TIMESTAMP=2026-01-01 00:00:00 UTC']
    env = os.environ.copy()
    env['KCONFIG_ALLCONFIG'] = str(config)
    with (destination / 'configure.log').open('w') as stream:
        subprocess.run(command + ['allnoconfig'], env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)
    run_logged(command + ['-j' + os.environ.get('DIFF_JOBS', str(min(os.cpu_count() or 2, 8))), 'Image'], destination / 'build.log')
    saved = {'inputs': data, 'image_sha256': digest(image), 'config_sha256': digest(destination / '.config')}
    metadata.write_text(json.dumps(saved, indent=2) + '\n')
    return image, saved


def fixture(directory, program):
    tree = directory / 'fixture'
    tree.mkdir()
    shutil.copy2(program, tree / 'init')
    (tree / 'init').chmod(0o755)
    (tree / 'data').write_bytes(bytes(65 + i % 26 for i in range(9000)))
    (tree / 'dev').mkdir()
    disk = directory / 'fixture.img'
    with disk.open('wb') as stream:
        stream.truncate(32 * 1024 * 1024)
    run_logged(['mkfs.ext4', '-q', '-F', '-b', '4096', '-d', str(tree), str(disk)], directory / 'mkfs.log')
    # debugfs works without host root privileges; Linux opens this console as init stdio.
    commands = directory / 'fixture.debugfs'
    commands.write_text('cd /dev\n'
                        'mknod console c 5 1\nset_inode_field console mode 020600\n'
                        'mknod null c 1 3\nset_inode_field null mode 020666\n'
                        'mknod zero c 1 5\nset_inode_field zero mode 020666\n')
    run_logged(['debugfs', '-w', '-f', str(commands), str(disk)], directory / 'debugfs.log')
    return disk


def run(args):
    directory = BUILD / 'run'
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    metadata = {'status': 'preparing', 'timestamp_normalizer_sha256': digest(HERE / 'timestamps.py')}
    metadata_path = directory / 'metadata.json'
    try:
        kernel_snapshot = directory / 'boaros-kernel'
        program_snapshot = directory / 'cases-rv'
        shutil.copyfile(args.kernel, kernel_snapshot)
        shutil.copyfile(args.program, program_snapshot)
        manifest_snapshot = directory / 'cases.txt'
        shutil.copyfile(HERE / 'cases.txt', manifest_snapshot)
        metadata.update(boaros_sha256=digest(kernel_snapshot),
                        program_sha256=digest(program_snapshot),
                        case_manifest_sha256=digest(manifest_snapshot))
        image, linux_metadata = linux_build()
        metadata['linux'] = linux_metadata
        qemu = os.environ.get('QEMU_RISCV64', 'qemu-system-riscv64')
        metadata['qemu'] = output([qemu, '--version'])
        metadata['mkfs'] = output(['mkfs.ext4', '-V'])
        disk = fixture(directory, program_snapshot)
        metadata['fixture_sha256'] = digest(disk)
        expected = [line for line in manifest_snapshot.read_text().splitlines() if line and not line.startswith('#')]
        normalized = []
        failures = []
        for name, kernel in [('linux', image), ('boaros', kernel_snapshot)]:
            target = directory / (name + '.img')
            shutil.copyfile(disk, target)
            command = [qemu, '-machine', 'virt', '-bios', 'default', '-kernel', str(kernel),
                       '-m', '512M', '-smp', '1', '-nographic', '-no-reboot',
                       '-drive', f'file={target},if=none,format=raw,id=root',
                       '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
            if name == 'linux':
                # init_mount uses namespace.c's default MNT_RELATIME. relatime
                # is a VFS flag, not an ext4 rootflags= filesystem parameter.
                command += ['-append', 'root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
            metadata[name + '_command'] = command
            logfile = directory / (name + '.log')
            try:
                run_logged(command, logfile, args.timeout)
                raw = logfile.read_text(errors='replace')
                result = normalize(raw, expected)
                if name == 'boaros' and not re.search(r'^BoarOS: PID 1 exited status=0x2a pages=0x[1-9a-f][0-9a-f]* heap-live=0x0; shutting down$', raw, re.M):
                    raise RuntimeError('BoarOS did not report successful full resource cleanup')
                (directory / (name + '.normalized')).write_text(''.join(result))
                normalized.append(result)
            except (RuntimeError, TimeoutError, ValueError) as error:
                failures.append(f'{name}: {error}')
        if failures:
            raise RuntimeError('; '.join(failures))
        same, delta = compare(*normalized)
        (directory / 'results.diff').write_text(delta)
        if not same:
            raise RuntimeError('ABI mismatch:\n' + delta)
        metadata['status'] = 'passed'
        for path in directory.glob('*.img'):
            path.unlink()
        print(f'{len(expected)} differential ABI records match; artifacts: {directory}')
    except Exception as error:
        metadata.update(status='failed', error=str(error))
        raise
    finally:
        metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache-key', action='store_true')
    parser.add_argument('--build-linux', action='store_true')
    parser.add_argument('--linux-config', type=Path,
                        help='Linux configuration for cache-key/build-linux')
    parser.add_argument('--kernel', type=Path, default=ROOT / 'kernel-rv')
    parser.add_argument('--program', type=Path, default=BUILD / 'cases-rv')
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    if args.cache_key:
        print(identity(args.linux_config)[0])
    elif args.build_linux:
        print(linux_build(args.linux_config)[0])
    else:
        run(args)

if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(f'diff-abi: {error}', file=sys.stderr)
        sys.exit(1)
