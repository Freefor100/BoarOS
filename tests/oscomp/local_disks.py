#!/usr/bin/env python3
"""Prepare the local two-disk profile and its fixed, real filesystem tools."""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
INPUTS = json.loads((HERE / 'inputs.json').read_text())


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


@contextmanager
def locked(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('a') as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        yield


def sync(path):
    with Path(path).open('rb') as stream:
        os.fsync(stream.fileno())


def copy_disk_image(source, destination):
    # Allocate independent host extents and complete writes before guest timing.
    subprocess.run(['cp', '--reflink=never', '--sparse=never', str(source), str(destination)], check=True)
    sync(destination)


def verify_target(path, arch):
    data = Path(path).read_bytes()
    if (len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01' or
            struct.unpack_from('<H', data, 18)[0] != INPUTS['architectures'][arch]['elf_machine']):
        raise RuntimeError('tool is not the target ELF64 ABI: ' + str(path))
    offset, size, count = struct.unpack_from('<Q', data, 32)[0], *struct.unpack_from('<HH', data, 54)
    if any(struct.unpack_from('<I', data, offset + index * size)[0] in (2, 3)
           for index in range(count)):
        raise RuntimeError('target tool must be statically linked: ' + str(path))


def fixed_source(cache):
    facts = INPUTS['e2fsprogs']
    archive = cache / Path(facts['url']).name
    source = cache / ('e2fsprogs-' + facts['version'])
    with locked(cache / 'source.lock'):
        if not archive.exists():
            temporary = archive.with_suffix('.download')
            print('Downloading pinned e2fsprogs:', facts['version'], flush=True)
            try:
                with urllib.request.urlopen(facts['url'], timeout=60) as src, temporary.open('wb') as dst:
                    shutil.copyfileobj(src, dst)
                if sha(temporary) != facts['sha256']:
                    raise RuntimeError('e2fsprogs archive checksum differs')
                temporary.replace(archive)
            finally:
                temporary.unlink(missing_ok=True)
        if archive.is_symlink() or sha(archive) != facts['sha256']:
            raise RuntimeError('e2fsprogs archive checksum differs: ' + str(archive))
        if not source.exists():
            with tarfile.open(archive) as content:
                content.extractall(cache, filter='data')
    return source


def run_logged(command, cwd, log, environment=None):
    with log.open('a') as stream:
        stream.write('\n' + repr(list(map(str, command))) + '\n')
        stream.flush()
        subprocess.run(list(map(str, command)), cwd=cwd, env=environment,
                       stdout=stream, stderr=subprocess.STDOUT, check=True)


def build_tools(cache, toolchain_root, arch):
    cache, toolchain_root = Path(cache).resolve(), Path(toolchain_root).resolve()
    source = fixed_source(cache)
    target = cache / arch
    with locked(cache / (arch + '.lock')):
        target.mkdir(parents=True, exist_ok=True)
        environment = os.environ.copy()
        for name in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS'):
            environment.pop(name, None)
        if arch == 'host':
            cc, flags = shutil.which('cc'), ['-O2', '-g0']
            host = []
            environment.pop('REALGCC', None)
        else:
            cc = toolchain_root / 'build' / arch / 'musl-root/bin/musl-gcc'
            if not cc.is_file():
                raise RuntimeError('existing musl toolchain required: ' + str(cc))
            if arch == 'riscv':
                environment['REALGCC'] = shutil.which('riscv64-linux-gnu-gcc') or 'riscv64-linux-gnu-gcc'
                flags = ['-O2', '-g0', '-march=rv64gc', '-mabi=lp64d']
                if subprocess.run([str(cc), '-fno-link-libatomic', '-E', '-x', 'c', '/dev/null'],
                                  env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                    flags.append('-fno-link-libatomic')
                uapi = Path(subprocess.check_output([str(cc), '-print-sysroot'], env=environment, text=True).strip()) / 'include'
            else:
                environment['REALGCC'] = str(toolchain_root / 'build/loongarch/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc')
                flags = ['-O2', '-g0', '-mabi=lp64s', '-msoft-float', '-mno-lsx', '-mno-lasx']
                uapi = toolchain_root / 'build/loongarch/uapi/include'
                flags += ['-Wl,-z,max-page-size=16384']
            if not (uapi / 'linux/unistd.h').is_file():
                raise RuntimeError('existing target Linux UAPI required: ' + str(uapi))
            flags += ['-idirafter', str(uapi)]
            machine = subprocess.check_output([str(cc), '-dumpmachine'], env=environment, text=True).strip()
            host = ['--host=' + machine]
        if not cc:
            raise RuntimeError('host C compiler is missing')
        cc = Path(cc).resolve()
        options = ['--enable-libuuid', '--enable-libblkid', '--disable-nls',
                   '--disable-elf-shlibs', '--disable-uuidd', '--disable-fuse2fs',
                   '--without-libarchive', '--without-pthread',
                   '--disable-rpath', *host]
        identity = {'source_sha256': INPUTS['e2fsprogs']['sha256'],
                    'cc': str(cc), 'cc_sha256': sha(cc),
                    'cc_version': subprocess.check_output([str(cc), '--version'], env=environment, text=True),
                    'flags': flags, 'link_flags': ['-static', *flags], 'configure': options}
        if arch != 'host':
            identity['scratch_source_sha256'] = sha(HERE / 'scratch.c')
            identity['musl_sha256'] = sha(toolchain_root / 'build' / arch / 'musl-root/lib/libc.a')
            identity['musl_specs_sha256'] = sha(toolchain_root / 'build' / arch / 'musl-root/lib/musl-gcc.specs')
            identity['real_cc_sha256'] = sha(environment['REALGCC'])
        stamp = target / 'identity.json'
        products = [target / 'mke2fs', target / 'mke2fs.conf'] + ([] if arch == 'host' else [target / 'scratch-check'])
        if stamp.exists():
            previous = json.loads(stamp.read_text())
            if previous.get('inputs') == identity and all(path.is_file() and previous['products'].get(path.name) == sha(path) for path in products):
                if arch != 'host':
                    for name in ('mke2fs', 'scratch-check'): verify_target(target / name, arch)
                return target
        build = target / 'build'
        if build.exists(): shutil.rmtree(build)
        build.mkdir()
        log = target / 'build.log'
        log.write_text('')
        environment.update(CC=str(cc), CFLAGS=' '.join(flags), LDFLAGS='-static ' + ' '.join(flags),
                           BUILD_CC=shutil.which('cc'), PKG_CONFIG='false')
        print('Building real e2fsprogs mke2fs:', arch, 'log:', log, flush=True)
        run_logged([source / 'configure', *options], build, log, environment)
        run_logged(['make', '-j8', 'libs'], build, log, environment)
        run_logged(['make', '-C', 'misc', '-j8', 'mke2fs'], build, log, environment)
        shutil.copyfile(build / 'misc/mke2fs', products[0])
        products[0].chmod(0o755)
        shutil.copyfile(build / 'misc/mke2fs.conf', target / 'mke2fs.conf')
        if arch != 'host':
            run_logged([cc, *flags, '-static', '-Wall', '-Wextra', '-Werror',
                        '-Wl,--build-id=none', '-o', target / 'scratch-check', HERE / 'scratch.c'], target, log, environment)
            for name in ('mke2fs', 'scratch-check'): verify_target(target / name, arch)
        stamp.write_text(json.dumps({'inputs': identity, 'products': {path.name: sha(path) for path in products}}, indent=2) + '\n')
        return target


def prepare_assets(cache, toolchain_root, arch):
    cache = Path(cache).resolve()
    host = build_tools(cache, toolchain_root, 'host')
    target = build_tools(cache, toolchain_root, arch)
    template = target / 'scratch-template.img'
    facts = INPUTS['local_disk']
    identity = {'profile': 'local-two-disk-ext4', **facts, 'mke2fs_sha256': sha(host / 'mke2fs'),
                'mke2fs_config_sha256': sha(host / 'mke2fs.conf')}
    with locked(cache / (arch + '.lock')):
        stamp = target / 'scratch-template.json'
        if stamp.exists() and json.loads(stamp.read_text()) == identity and template.is_file() and template.stat().st_size == facts['scratch_bytes']:
            return target
        temporary = template.with_suffix('.new')
        try:
            with temporary.open('wb') as stream: stream.truncate(facts['scratch_bytes'])
            environment = dict(os.environ, MKE2FS_CONFIG=str(host / 'mke2fs.conf'))
            subprocess.run([str(host / 'mke2fs'), '-q', '-F', '-t', 'ext4', '-b', str(facts['block_bytes']),
                            '-E', 'lazy_itable_init=0,lazy_journal_init=0', str(temporary)], env=environment, check=True)
            sync(temporary)
            temporary.replace(template)
            stamp.write_text(json.dumps(identity, indent=2) + '\n')
        finally:
            temporary.unlink(missing_ok=True)
    return target


def prepare_run(reference, assets, destination, arch):
    reference, assets, destination = Path(reference).resolve(), Path(assets).resolve(), Path(destination).resolve()
    root_source = reference / INPUTS['architectures'][arch]['image']
    scratch_source = assets / 'scratch-template.img'
    root, scratch = destination / 'root.img', destination / 'scratch.img'
    # Never accept an alias of the retained inputs as a writable destination.
    sources = [root_source, scratch_source]
    if root == scratch or any(path == source or (path.exists() and os.path.samefile(path, source))
            for path in (root, scratch) for source in sources):
        raise RuntimeError('run disks must be independent of retained images')
    if root.exists() or scratch.exists():
        raise RuntimeError('run disk already exists; choose a fresh directory')
    for name in ('mke2fs', 'scratch-check'): verify_target(assets / name, arch)
    destination.mkdir(parents=True, exist_ok=True)
    copy_disk_image(root_source, root)
    copy_disk_image(scratch_source, scratch)
    # Only disposable root copies receive build-owned tools; original test bytes stay intact.
    names = ('mke2fs', 'mke2fs.conf', 'scratch-check')
    def quote(path):
        return '"' + str(path).replace('\\', '\\\\').replace('"', '\\"') + '"'
    with tempfile.TemporaryDirectory(prefix='tool-readback-', dir=destination) as temporary:
        commands = ['mkdir /boaros-tools']
        for name in names:
            commands += [f'write {quote(assets / name)} /boaros-tools/{name}',
                         f'set_inode_field /boaros-tools/{name} mode ' + ('0100644' if name.endswith('.conf') else '0100755'),
                         f'dump /boaros-tools/{name} {quote(Path(temporary) / name)}']
        result = subprocess.run(['debugfs', '-w', '-f', '-', str(root)], input='\n'.join(commands) + '\n',
                                text=True, capture_output=True, check=True)
        # debugfs can exit zero after a failed command; verify the actual image bytes.
        if any(not (Path(temporary) / name).is_file() or sha(Path(temporary) / name) != sha(assets / name) for name in names):
            raise RuntimeError('tool injection failed: ' + result.stdout + result.stderr)
    sync(root)
    return root, scratch


def disk_arguments(arch, root, scratch):
    devices = []
    for index, disk in enumerate((root, scratch)):
        if arch == 'riscv':
            device = f'virtio-blk-device,drive=x{index},bus=virtio-mmio-bus.{index}'
        else:
            device = f'virtio-blk-pci,drive=x{index}'
        devices += ['-drive', f'file={disk},if=none,format=raw,id=x{index}', '-device', device]
    return devices


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--arch', choices=('riscv', 'loongarch'), required=True)
    parser.add_argument('--cache', type=Path, default=Path(os.environ.get('OSCOMP_LOCAL_CACHE', ROOT / 'build/tools/oscomp-local')))
    parser.add_argument('--toolchain-root', type=Path, default=Path(os.environ.get('OSCOMP_TOOLCHAIN_ROOT', ROOT)))
    parser.add_argument('--reference', type=Path, default=ROOT / 'references/oscomp-autotest')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--qemu')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--memory', default='1G')
    parser.add_argument('--gdb', action='store_true', help='start QEMU paused with its GDB stub')
    args = parser.parse_args()
    if bool(args.qemu) != bool(args.kernel):
        parser.error('--qemu and --kernel require each other')
    if args.gdb and not args.qemu:
        parser.error('--gdb requires --qemu and --kernel')
    assets = prepare_assets(args.cache, args.toolchain_root, args.arch)
    if args.qemu and args.output is None:
        (ROOT / 'build').mkdir(exist_ok=True)
        args.output = Path(tempfile.mkdtemp(prefix='oscomp-' + args.arch + '-run.', dir=ROOT / 'build'))
    if args.output:
        root, scratch = prepare_run(args.reference, assets, args.output, args.arch)
        if args.qemu:
            command = [args.qemu, '-machine', 'virt', '-m', args.memory, '-smp', '1',
                       '-kernel', str(args.kernel), '-nographic', '-no-reboot']
            if args.arch == 'riscv': command += ['-bios', 'default']
            else: command += ['-cpu', 'la464']
            command += disk_arguments(args.arch, root, scratch)
            if args.gdb: command += ['-S', '-s']
            print('Local two-disk profile:', ' '.join(command), flush=True)
            subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
