#!/usr/bin/env python3
"""Build the pinned, unmodified strace release for the native RV64/LA64 musl ABIs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
from arch_profiles import PROFILES

VERSION = '7.3'
ARCHIVE = 'strace-'+VERSION+'.tar.xz'
SOURCE_KEY = 'strace/'+ARCHIVE


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def reference(key):
    return next(line.split('\t') for line in (ROOT/'references/sources.tsv').read_text().splitlines()
                if line and not line.startswith('#') and line.split('\t')[1] == key)


def capture(argv, env=None):
    return subprocess.check_output(list(map(str, argv)), env=env, text=True).strip()


def run(argv, directory, log, commands, env=None):
    argv = list(map(str, argv))
    entry = {'argv': argv, 'cwd': str(directory), 'log': str(log)}
    commands.append(entry)
    with log.open('w') as stream:
        stream.write(shlex.join(argv)+'\n');stream.flush()
        result = subprocess.run(argv, cwd=directory, env=env, stdout=stream, stderr=subprocess.STDOUT)
    entry['returncode'] = result.returncode
    if result.returncode:
        raise RuntimeError('build command failed; see '+str(log))


def source(cache, commands):
    pin = reference(SOURCE_KEY)
    archive = cache/ARCHIVE
    if not archive.exists():
        retained = ROOT/'references'/SOURCE_KEY
        if retained.exists():
            shutil.copyfile(retained, archive)
        else:
            temporary = cache/(ARCHIVE+'.part')
            run(['curl', '-fL', '--retry', '3', pin[2], '-o', temporary], ROOT,
                cache/'download.log', commands)
            if digest(temporary) != pin[4]:
                raise RuntimeError('download SHA-256 differs from references/sources.tsv')
            temporary.rename(archive)
    if digest(archive) != pin[4]:
        raise RuntimeError('archive SHA-256 differs from references/sources.tsv')
    tree = cache/('strace-'+VERSION)
    if not tree.exists():
        with tarfile.open(archive) as stream:
            stream.extractall(cache, filter='data')
    # Out-of-tree builds never need to edit the source. Check retained caches
    # against the original release rather than trusting an old success stamp.
    expected = set()
    with tarfile.open(archive) as stream:
        for item in stream:
            path = cache/item.name
            if item.isfile() or item.islnk():
                expected.add(path.relative_to(tree).as_posix())
                upstream = hashlib.sha256(stream.extractfile(item).read()).hexdigest()
                if not path.is_file() or path.is_symlink() or digest(path) != upstream:
                    raise RuntimeError('source differs from original release: '+str(path))
            elif item.issym():
                expected.add(path.relative_to(tree).as_posix())
                if not path.is_symlink() or os.readlink(path) != item.linkname:
                    raise RuntimeError('source symlink differs from original release: '+str(path))
    actual = {p.relative_to(tree).as_posix() for p in tree.rglob('*') if p.is_file() or p.is_symlink()}
    if actual != expected:
        raise RuntimeError('source tree has files absent from original release')
    return tree, {'url': pin[2], 'sha256': pin[4], 'archive': str(archive), 'tree': str(tree),
                  'unmodified': True, 'file_count': len(expected), 'license_sha256': digest(tree/'COPYING')}


def environment(arch):
    env = os.environ.copy()
    for name in ('CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH', 'LD_LIBRARY_PATH',
                 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'CC', 'CXX', 'MAKEFLAGS', 'MFLAGS', 'CONFIG_SITE'):
        env.pop(name, None)
    compiler = ROOT/('build/riscv-user-smp4/riscv/musl-root/bin/musl-gcc' if arch == 'riscv'
                    else 'build/loongarch/musl-root/bin/musl-gcc')
    real = Path('/usr/bin/riscv64-linux-gnu-gcc') if arch == 'riscv' else ROOT/'build/loongarch/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc'
    env['REALGCC'] = str(real)
    env['CC'] = str(compiler)
    env['CONFIG_SITE'] = '/dev/null'
    for name in ('ar', 'ranlib', 'strip'):
        tool = capture([real, '-print-prog-name='+name], env)
        resolved = shutil.which(tool)
        if resolved is None:
            raise RuntimeError('missing target '+name+': '+tool)
        env[name.upper()] = resolved
    return env, compiler, real


def build(arch, cache, tree, original, jobs, commands):
    env, compiler, real = environment(arch)
    linux = reference('linux')
    revision = capture(['git', '-C', ROOT/'references/linux', 'rev-parse', 'HEAD'])
    if revision != linux[4]:
        raise RuntimeError('Linux UAPI source differs from references/sources.tsv')
    headers = cache/('uapi-'+arch)
    run(['make', '-s', '-C', ROOT/'references/linux', 'O='+str(headers), 'ARCH='+arch, 'headers'],
        ROOT, cache/('headers-'+arch+'.log'), commands)
    include = headers/'usr/include'
    profile_flags = PROFILES[arch].musl_flags(compiler)
    cflags = ['-O2', '-g0', *[f for f in profile_flags if not f.startswith('-Wl,')]]
    ldflags = ['-static', *[f for f in profile_flags if f.startswith('-Wl,')]]
    env.update(CFLAGS=shlex.join(cflags), CPPFLAGS=shlex.join(['-idirafter', str(include)]),
               LDFLAGS=shlex.join(ldflags))
    header_digests = {str(p.relative_to(include)): digest(p) for p in sorted(include.rglob('*')) if p.is_file()}
    identity = {'arch': arch, 'source_sha256': original['sha256'], 'linux_uapi_revision': revision,
                'linux_uapi_tree_sha256': hashlib.sha256(json.dumps(header_digests, sort_keys=True).encode()).hexdigest(),
                'compiler': {'wrapper': str(compiler), 'wrapper_sha256': digest(compiler),
                             'real': str(real), 'sha256': digest(real), 'version': capture([real, '--version'], env)},
                'musl': {name: digest(compiler.parent.parent/'lib'/name) for name in ('libc.a', 'musl-gcc.specs')},
                'environment': {k: env[k] for k in ('CC', 'REALGCC', 'AR', 'RANLIB', 'STRIP', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS')}}
    options = ['--host='+('riscv64' if arch == 'riscv' else 'loongarch64')+'-unknown-linux-musl',
               '--enable-mpers=no', '--enable-stacktrace=no', '--with-libdw=no',
               '--with-libunwind=no', '--with-libiberty=no', '--enable-bundled=yes']
    identity['configure_options'] = options
    out = cache/arch
    out.mkdir(exist_ok=True)
    stamp = out/'configuration.json'
    if not stamp.exists() or json.loads(stamp.read_text()) != identity:
        run([tree/'configure', *options], out, out/'configure.log', commands, env)
        stamp.write_text(json.dumps(identity, indent=2)+'\n')
    run(['make', '-j'+str(jobs)], out, out/'build.log', commands, env)
    executable = out/'src/strace'
    if not executable.is_file():
        raise RuntimeError('upstream build did not produce src/strace')
    headers = capture(['readelf', '-hW', executable])
    program = capture(['readelf', '-lW', executable])
    dynamic = capture(['readelf', '-dW', executable])
    if 'INTERP' in program or '(NEEDED)' in dynamic:
        raise RuntimeError('strace output is not statically linked')
    if arch == 'loongarch' and any(int(row.split()[-1], 16) < 16384 for row in program.splitlines() if row.lstrip().startswith('LOAD')):
        raise RuntimeError('LA load segments require 16 KiB alignment')
    (out/'elf-headers.txt').write_text(headers+'\n'+program+'\n'+dynamic+'\n')
    return {'path': str(executable), 'sha256': digest(executable), 'static': True,
            'identity': str(stamp), 'elf_headers': str(out/'elf-headers.txt')}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=('all', 'riscv', 'loongarch'), default='all')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--cache-root', type=Path, default=ROOT/'build/tools'/('strace-'+VERSION))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    cache = args.cache_root.resolve();cache.mkdir(parents=True, exist_ok=True)
    report = {'version': VERSION, 'builder_sha256': digest(__file__), 'commands': [], 'binaries': {}}
    try:
        tree, original = source(cache, report['commands'])
        report['source'] = original
        for arch in (('riscv', 'loongarch') if args.arch == 'all' else (args.arch,)):
            report['binaries'][arch] = build(arch, cache, tree, original, args.jobs, report['commands'])
        report['status'] = 'built'
    except Exception as error:
        report['status'] = 'failed';report['error'] = str(error)
        raise
    finally:
        (cache/'build-report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report['binaries'], indent=2))


if __name__ == '__main__':
    main()
