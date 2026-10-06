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
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from arch_profiles import PROFILES

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
    # Idle IRQs may insert complete user records after a partial boot banner.
    # Keep the entire reserved ABI suffix; payload/identity/order stay strict.
    lines = [line[line.index('ABI '):]
             for line in text.replace('\r\n', '\n').splitlines() if 'ABI ' in line]
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


def fixed_linux_image(argument):
    if argument is None:
        return linux_build()
    image = Path(argument).resolve(strict=True)
    for ancestor in image.parents:
        metadata = ancestor / 'identity.json'
        if metadata.is_file():
            saved = json.loads(metadata.read_text())
            if (saved['inputs']['source'] != list(source_info()) or
                    saved['image_sha256'] != digest(image)):
                raise RuntimeError('Linux image does not match fixed build identity')
            return image, saved
    raise RuntimeError('Linux image has no fixed-build identity.json')


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
                        'mknod zero c 1 5\nset_inode_field zero mode 020666\n'
                        'mknod rtc0 c 10 135\nset_inode_field rtc0 mode 020600\n')
    run_logged(['debugfs', '-w', '-f', str(commands), str(disk)], directory / 'debugfs.log')
    return disk


def run(args):
    directory = getattr(args, 'output', None) or BUILD / 'run'
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    profile=PROFILES[getattr(args,'arch','riscv')]
    memories=getattr(args,'memory',None) or (['512M','1G'] if profile.name=='loongarch' else ['512M'])
    metadata = {'status': 'preparing', 'arch':profile.name,'memory':memories,'timestamp_normalizer_sha256': digest(HERE / 'timestamps.py')}
    metadata_path = directory / 'metadata.json'
    try:
        kernel_snapshot = directory / 'boaros-kernel'
        program_snapshot = directory / ('cases-rv' if profile.name=='riscv' else 'cases-la')
        shutil.copyfile(args.kernel, kernel_snapshot)
        shutil.copyfile(args.program, program_snapshot)
        manifest_snapshot = directory / 'cases.txt'
        shutil.copyfile(getattr(args, 'case_manifest', None) or HERE / 'cases.txt', manifest_snapshot)
        metadata.update(boaros_sha256=digest(kernel_snapshot),
                        program_sha256=digest(program_snapshot),
                        case_manifest_sha256=digest(manifest_snapshot))
        if profile.name=='riscv':image, linux_metadata = fixed_linux_image(getattr(args, 'linux_kernel', None))
        else:
            image=Path(getattr(args,'linux_kernel',None) or profile.linux_kernel())
            linux_metadata=json.loads((image.parent/'boaros-identity.json').read_text())
            if linux_metadata['revision']!=source_info()[1] or 'CONFIG_16KB_3LEVEL=y' not in (image.parent/'.config').read_text():
                raise RuntimeError('LA fixed Linux/page identity mismatch')
            if linux_metadata.get('image_sha256')!=digest(image):raise RuntimeError('LA Linux image integrity mismatch; run prepare-la-linux-platform')
        metadata['linux'] = linux_metadata
        linux_snapshot=directory/'linux-kernel'
        shutil.copyfile(image,linux_snapshot)
        metadata['linux_image_sha256']=digest(linux_snapshot)
        image=linux_snapshot
        qemu = getattr(args,'qemu',None) or (os.environ.get('QEMU_RISCV64',profile.qemu) if profile.name=='riscv' else profile.qemu)
        metadata['qemu'] = output([qemu, '--version'])
        executable=Path(shutil.which(qemu) or qemu)
        metadata['qemu_sha256']=digest(executable)
        metadata['qemu_mode']=executable.stat().st_mode & 0o777
        metadata['qemu_link']=__import__('os').readlink(executable) if executable.is_symlink() else None
        metadata['mkfs'] = output(['mkfs.ext4', '-V'])
        disk = fixture(directory, program_snapshot)
        metadata['fixture_sha256'] = digest(disk)
        expected = [line for line in manifest_snapshot.read_text().splitlines() if line and not line.startswith('#')]
        normalized = []
        failures = []
        runs=[(name,kernel,memory) for name,kernel in [('linux',image),('boaros',kernel_snapshot)] for memory in memories]
        for platform, kernel,memory in runs:
            name=platform if len(memories)==1 else platform+'-'+memory
            target = directory / (name + '.img')
            shutil.copyfile(disk, target)
            command=profile.boot(qemu,kernel,memory)+['-net','none','-object','rng-random,id=entropy,filename=/dev/urandom',
                '-device',profile.rng(),'-drive',f'file={target},if=none,format=raw,id=root','-device',profile.block('modern')]
            if profile.name=='riscv':command+=['-global','virtio-mmio.force-legacy=false']
            if platform == 'linux':
                # init_mount uses namespace.c's default MNT_RELATIME. relatime
                # is a VFS flag, not an ext4 rootflags= filesystem parameter.
                command += ['-append', 'root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
            metadata[name + '_command'] = command
            logfile = directory / (name + '.log')
            try:
                run_logged(command, logfile, args.timeout)
                raw = logfile.read_text(errors='replace')
                result = normalize(raw, expected)
                if platform == 'boaros' and not profile.root_success(raw,42):
                    raise RuntimeError('BoarOS did not report successful full resource cleanup')
                (directory / (name + '.normalized')).write_text(''.join(result))
                normalized.append(result)
            except (RuntimeError, TimeoutError, ValueError) as error:
                failures.append(f'{name}: {error}')
        if failures:
            raise RuntimeError('; '.join(failures))
        same, delta=True, ""
        for actual in normalized[1:]:
            matched,difference=compare(normalized[0],actual);same &= matched;delta+=difference
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
    parser.add_argument('--arch',choices=tuple(PROFILES),default='riscv')
    parser.add_argument('--memory',choices=('512M','1G'),action='append')
    parser.add_argument('--qemu')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--linux-kernel', type=Path,
                        help='verified fixed Linux Image cache')
    parser.add_argument('--program', type=Path)
    parser.add_argument('--case-manifest', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    profile=PROFILES[args.arch]
    args.kernel=args.kernel or ROOT/profile.kernel
    args.program=args.program or (BUILD/'cases-rv' if args.arch=='riscv' else ROOT/'build/loongarch/diff-abi-cases')
    if args.arch=='loongarch' and not args.output:args.output=ROOT/'build/loongarch/diff-abi-run'
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
