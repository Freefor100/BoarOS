#!/usr/bin/env python3
"""Build the offline official delivery disk and independent local run copies."""
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
import zlib

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
            struct.unpack_from('<HH', data, 16) != (2, INPUTS['architectures'][arch]['elf_machine'])):
        raise RuntimeError('tool is not target ELF64 EXEC: ' + str(path))
    flags = struct.unpack_from('<I', data, 48)[0]
    if (flags & 6 != 4) if arch == 'riscv' else (flags & 7 != 3):
        raise RuntimeError('tool must use LP64D: ' + str(path))
    offset, size, count = struct.unpack_from('<Q', data, 32)[0], *struct.unpack_from('<HH', data, 54)
    if size < 56 or not count or offset + size * count > len(data):
        raise RuntimeError('invalid ELF program headers: ' + str(path))
    for index in range(count):
        entry = offset + index * size
        kind = struct.unpack_from('<I', data, entry)[0]
        if kind in (2, 3):
            raise RuntimeError('target tool must be statically linked: ' + str(path))
        if kind == 1 and arch == 'loongarch':
            file_offset, address = struct.unpack_from('<QQ', data, entry + 8)
            alignment = struct.unpack_from('<Q', data, entry + 48)[0]
            if alignment < 16384 or (address - file_offset) % 16384:
                raise RuntimeError('LA load segment must support 16 KiB pages: ' + str(path))


def fixed_source(cache):
    facts = INPUTS['e2fsprogs']
    archive = ROOT / facts['archive']
    source = cache / ('e2fsprogs-' + facts['version'])
    with locked(cache / 'source.lock'):
        if archive.is_symlink() or not archive.is_file() or sha(archive) != facts['sha256']:
            raise RuntimeError('tracked e2fsprogs archive checksum differs: ' + str(archive))
        if not source.exists():
            with tarfile.open(archive) as content:
                content.extractall(cache, filter='data')
        apply_portability_patch(source)
    return source


def apply_portability_patch(source):
    facts = INPUTS['e2fsprogs']['portability_patch']
    patch = ROOT / facts['path']
    if patch.is_symlink() or sha(patch) != facts['sha256']:
        raise RuntimeError('e2fsprogs portability patch checksum differs')
    target = source / facts['target']
    current = sha(target)
    if target.is_symlink() or current not in (facts['original_sha256'], facts['patched_sha256']):
        raise RuntimeError('e2fsprogs portability target checksum differs: ' + str(target))
    if current == facts['original_sha256']:
        subprocess.run(['patch', '--batch', '--forward', '--fuzz=0', '-p1', '-i', str(patch)],
                       cwd=source, check=True, capture_output=True, text=True)
    if sha(target) != facts['patched_sha256']:
        raise RuntimeError('e2fsprogs portability patch result checksum differs')


def run_logged(command, cwd, log, environment=None):
    with log.open('a') as stream:
        stream.write('\n' + repr(list(map(str, command))) + '\n')
        stream.flush()
        subprocess.run(list(map(str, command)), cwd=cwd, env=environment,
                       stdout=stream, stderr=subprocess.STDOUT, check=True)


def build_tools(cache, arch):
    cache = Path(cache).resolve()
    source = fixed_source(cache)
    target = cache / arch
    with locked(cache / (arch + '.lock')):
        target.mkdir(parents=True, exist_ok=True)
        environment = os.environ.copy()
        for name in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS', 'REALGCC'):
            environment.pop(name, None)
        flags = ['-O2', '-g0', '-fno-pie']
        link = ['-static', '-no-pie']
        host = []
        if arch == 'host':
            cc = Path('/usr/bin/cc')
        else:
            prefix = INPUTS['architectures'][arch]['tools_prefix']
            cc = Path(prefix + 'gcc')
            if not cc.is_file():
                raise RuntimeError('fixed official container cross compiler required: ' + str(cc))
            flags += (['-march=rv64gc', '-mabi=lp64d'] if arch == 'riscv' else
                      ['-march=loongarch64', '-mabi=lp64d', '-fno-tree-vectorize', '-fno-tree-slp-vectorize'])
            if arch == 'loongarch': link += ['-Wl,-z,max-page-size=16384']
            host = ['--host=' + subprocess.check_output([cc, '-dumpmachine'], text=True).strip()]
            environment.update({name.upper(): prefix + name for name in ('ar', 'ranlib', 'ld', 'strip', 'nm', 'objcopy', 'objdump')})
        options = ['--enable-libuuid', '--enable-libblkid', '--disable-nls',
                   '--disable-elf-shlibs', '--disable-uuidd', '--disable-fuse2fs',
                   '--without-libarchive', '--without-pthread', '--disable-rpath', *host]
        identity = {'source_sha256': INPUTS['e2fsprogs']['sha256'],
                    'portability_patch_sha256': INPUTS['e2fsprogs']['portability_patch']['sha256'],
                    'cc': str(cc), 'cc_sha256': sha(cc),
                    'cc_version': subprocess.check_output([cc, '--version'], text=True),
                    'flags': flags, 'link_flags': link, 'configure': options}
        names = ['mke2fs', 'mke2fs.conf', 'e2fsck'] if arch == 'host' else ['mke2fs', 'mke2fs.conf', 'seed']
        if arch != 'host':
            identity['seed_source_sha256'] = sha(HERE / 'seed.c')
            for library in ('libc.a', 'libgcc.a'):
                identity[library] = sha(subprocess.check_output([cc, '-print-file-name=' + library], text=True).strip())
        stamp = target / 'identity.json'
        previous = json.loads(stamp.read_text()) if stamp.exists() else {}
        if (previous.get('inputs') == identity and all((target / name).is_file() and
                previous.get('products', {}).get(name) == sha(target / name) for name in names)):
            if arch != 'host':
                for name in ('mke2fs', 'seed'): verify_target(target / name, arch)
            return target
        build = target / 'build'
        # Reuse verified filesystem tools when only the startup program changes.
        def configuration(value):
            return {key: item for key, item in value.items() if key != 'seed_source_sha256'}
        stable = [name for name in names if name != 'seed']
        reuse = (configuration(previous.get('inputs', {})) == configuration(identity) and
                 all((target / name).is_file() and previous.get('products', {}).get(name) == sha(target / name)
                     for name in stable))
        if not reuse:
            if build.exists(): shutil.rmtree(build)
            build.mkdir()
        log = target / 'build.log'; log.write_text('')
        environment.update(CC=str(cc), CFLAGS=' '.join(flags), LDFLAGS=' '.join(link + flags),
                           BUILD_CC='/usr/bin/cc', PKG_CONFIG='false')
        print('Building fixed filesystem tools:', arch, 'log:', log, flush=True)
        if not reuse:
            run_logged([source / 'configure', *options], build, log, environment)
            run_logged(['make', '-j8', 'libs'], build, log, environment)
            run_logged(['make', '-C', 'misc', '-j8', 'mke2fs'], build, log, environment)
            shutil.copy2(build / 'misc/mke2fs', target / 'mke2fs')
            shutil.copy2(build / 'misc/mke2fs.conf', target / 'mke2fs.conf')
        if arch == 'host':
            run_logged(['make', '-C', 'e2fsck', '-j8', 'e2fsck'], build, log, environment)
            shutil.copy2(build / 'e2fsck/e2fsck', target / 'e2fsck')
        else:
            run_logged([cc, *flags, *link, '-Wall', '-Wextra', '-Werror',
                        '-DSEED_ARCH=' + str(INPUTS['architectures'][arch]['elf_machine']),
                        '-Wl,--build-id=none', '-o', target / 'seed', HERE / 'seed.c'], target, log, environment)
            for name in ('mke2fs', 'seed'): verify_target(target / name, arch)
        stamp.write_text(json.dumps({'inputs': identity, 'products': {name: sha(target / name) for name in names}}, indent=2) + '\n')
        return target


def mbr():
    block = bytearray(512)
    for index, partition in enumerate(INPUTS['local_disk']['partitions']):
        struct.pack_into('<B3sB3sII', block, 446 + index * 16, 0, b'\xfe\xff\xff',
                         partition['type'], b'\xfe\xff\xff', partition['start_mib'] * 2048,
                         partition['size_mib'] * 2048)
    block[510:] = b'\x55\xaa'
    return block


def payload_files(arch):
    build = ROOT / ('build/riscv' if arch == 'riscv' else 'build/loongarch') / 'oscomp'
    result = {'case': build / 'case'}
    for name in ('ltp-hook.sh', 'libctest-hook.sh', 'ltp-case.sh', 'ltp-skips.tsv'):
        result[name] = HERE / name
    if arch == 'loongarch': result['linux-sched.so'] = build / 'linux-sched.so'
    return result


def prepare_assets(cache, arch):
    cache = Path(cache).resolve()
    compiler = Path(INPUTS['architectures'][arch]['tools_prefix'] + 'gcc')
    if not compiler.is_file() or not Path(INPUTS['dosfstools']['path']).is_file():
        # Native run/debug use the same offline container toolchain and cache as delivery.
        cache.mkdir(parents=True, exist_ok=True)
        command = ['docker', 'run', '--rm', '--pull=never', '--network', 'none',
                   '--user', str(os.getuid()) + ':' + str(os.getgid()),
                   '-v', str(ROOT) + ':' + str(ROOT), '-w', str(ROOT)]
        if not cache.is_relative_to(ROOT): command += ['-v', str(cache) + ':' + str(cache)]
        command += [INPUTS['container']['reference'], 'python3', '-B', str(HERE / 'local_disks.py'),
                    '--arch', arch, '--cache', str(cache)]
        subprocess.run(command, check=True)
        return cache / arch
    host, target = build_tools(cache, 'host'), build_tools(cache, arch)
    files = payload_files(arch)
    for name in ('mke2fs', 'mke2fs.conf', 'seed'): files[name] = target / name
    fat = Path(INPUTS['dosfstools']['path'])
    version = subprocess.run([fat, '--help'], capture_output=True, text=True).stdout
    if 'mkfs.fat ' + INPUTS['dosfstools']['version'] not in version:
        raise RuntimeError('fixed dosfstools version required: ' + version)
    facts = INPUTS['local_disk']
    identity = {'geometry': facts, 'files': {name: sha(path) for name, path in files.items()},
                'builder': sha(Path(__file__)), 'mke2fs': sha(host / 'mke2fs'), 'mkfs.fat': sha(fat)}
    template = target / 'delivery-template.img'
    with locked(cache / (arch + '.lock')):
        stamp = target / 'delivery-template.json'
        previous = json.loads(stamp.read_text()) if stamp.exists() else {}
        if (previous.get('inputs') == identity and template.is_file() and template.stat().st_size == facts['bytes']
                and previous.get('sha256') == sha(template)):
            with template.open('rb') as stream:
                stream.seek(facts['seed_offset'])
                if stream.read(facts['seed_bytes']) != bytes(facts['seed_bytes']):
                    raise RuntimeError('cached template contains a seed')
            return target
        with tempfile.TemporaryDirectory(prefix='delivery-', dir=target) as temporary:
            work = Path(temporary); tree = work / 'tools'; tree.mkdir()
            for name, path in files.items():
                shutil.copy2(path, tree / name)
                (tree / name).chmod(0o644 if name.endswith(('.conf', '.tsv')) else 0o755)
            for name in ('mkfs.ext2', 'mkfs.ext3', 'mkfs.ext4'): (tree / name).symlink_to('mke2fs')
            fresh = work / 'disk.img'
            with fresh.open('wb') as stream:
                stream.truncate(facts['bytes']); stream.write(mbr())
            environment = dict(os.environ, MKE2FS_CONFIG=str(host / 'mke2fs.conf'))
            for index, part in enumerate(facts['partitions']):
                image = work / ('part%d.img' % (index + 1))
                with image.open('wb') as stream: stream.truncate(part['size_mib'] * 1024**2)
                if part['filesystem'] == 'ext4':
                    command = [host / 'mke2fs', '-q', '-F', '-t', 'ext4', '-b', str(part['block_bytes']),
                               '-E', 'lazy_itable_init=0,lazy_journal_init=0,root_owner=0:0']
                    if index == 0: command += ['-d', tree]
                    subprocess.run([*map(str, command), str(image)], env=environment, check=True)
                    subprocess.run([host / 'e2fsck', '-fn', image], check=True, stdout=subprocess.DEVNULL)
                else:
                    subprocess.run([fat, '-F', '32', '-S', '512', '-s', '1', image], check=True)
                subprocess.run(['dd', 'if=' + str(image), 'of=' + str(fresh), 'bs=1M',
                                'seek=' + str(part['start_mib']), 'conv=notrunc,sparse', 'status=none'], check=True)
            sync(fresh); fresh.replace(template)
            stamp.write_text(json.dumps({'inputs': identity, 'sha256': sha(template)}, indent=2) + '\n')
    return target


def seed_envelope(arch):
    # getrandom waits for the host CRNG; each delivery draws its own fresh 64 bytes.
    seed = bytearray()
    while len(seed) < 64:
        chunk = os.getrandom(64 - len(seed))
        if not chunk: raise RuntimeError('host getrandom returned no bytes')
        seed.extend(chunk)
    prefix = struct.pack('<8sIII', b'BOARSEED', 1, INPUTS['architectures'][arch]['elf_machine'], 64)
    slot = prefix + struct.pack('<I', zlib.crc32(prefix + seed)) + seed
    return slot + bytes(INPUTS['local_disk']['seed_bytes'] - len(slot))


def prepare_delivery(assets, destination, arch):
    template = Path(assets) / 'delivery-template.img'
    destination = Path(destination)
    if destination.resolve() == template.resolve() or (destination.exists() and os.path.samefile(destination, template)):
        raise RuntimeError('delivery disk must be independent of template')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix=destination.name + '.', dir=destination.parent, delete=False) as stream:
        temporary = Path(stream.name)
    try:
        copy_disk_image(template, temporary)
        with temporary.open('r+b') as stream:
            stream.seek(INPUTS['local_disk']['seed_offset']); stream.write(seed_envelope(arch)); stream.flush(); os.fsync(stream.fileno())
        temporary.replace(destination)
    finally: temporary.unlink(missing_ok=True)
    return destination


def prepare_run(reference, assets, destination, arch):
    reference, assets, destination = Path(reference).resolve(), Path(assets).resolve(), Path(destination).resolve()
    root_source = reference / INPUTS['architectures'][arch]['image']
    sources = [root_source, assets / 'delivery-template.img']
    root, scratch = destination / 'root.img', destination / 'scratch.img'
    if any(path == source or (path.exists() and os.path.samefile(path, source))
           for path in (root, scratch) for source in sources):
        raise RuntimeError('run disks must be independent of retained images')
    if root.exists() or scratch.exists():
        raise RuntimeError('run disk already exists; choose a fresh directory')
    destination.mkdir(parents=True, exist_ok=True)
    copy_disk_image(root_source, root)
    prepare_delivery(assets, scratch, arch)
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
    parser.add_argument('--reference', type=Path, default=ROOT / 'references/oscomp-autotest')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--qemu')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--memory', default='1G')
    parser.add_argument('--gdb', action='store_true', help='start QEMU paused with its GDB stub')
    parser.add_argument('--delivery', type=Path, help='write a fresh seeded delivery disk')
    args = parser.parse_args()
    if bool(args.qemu) != bool(args.kernel):
        parser.error('--qemu and --kernel require each other')
    if args.gdb and not args.qemu:
        parser.error('--gdb requires --qemu and --kernel')
    assets = prepare_assets(args.cache, args.arch)
    if args.delivery:
        prepare_delivery(assets, args.delivery, args.arch)
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
