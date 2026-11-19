#!/usr/bin/env python3
"""Build the pinned host BTF encoder from repository archives, without network access."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
ARCHIVES = ('dwarves-1.28.tar.xz', 'elfutils-0.192.tar.bz2')
ELFUTILS_PATCH = ROOT / 'tools/pahole-elfutils-const.patch'


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(data)
    return value.hexdigest()


def sources():
    pins = {row[1]: row[4] for line in (ROOT / 'references/sources.tsv').read_text().splitlines()
            if line and not line.startswith('#') for row in [line.split('\t')]}
    result = {}
    for name in ARCHIVES:
        archive = ROOT / 'third_party/sources' / name
        expected = pins['pahole/' + name]
        if archive.is_symlink() or not archive.is_file() or digest(archive) != expected:
            raise RuntimeError('pahole source SHA-256 differs: ' + str(archive))
        result[name] = expected
    return result


def run(command, directory, environment, log):
    with log.open('a') as stream:
        stream.write('\n' + repr(list(map(str, command))) + '\n')
        stream.flush()
        result = subprocess.run(list(map(str, command)), cwd=directory, env=environment,
                                stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError('pahole build failed; see ' + str(log))


def build(output, compiler, jobs):
    output.mkdir(parents=True, exist_ok=True)
    # RV and LA, including independent make processes, share one host tool.
    with (output / 'build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        inputs = sources()
        cc = Path(shutil.which(compiler) or compiler).resolve(strict=True)
        identity = {'sources': inputs, 'builder': digest(__file__), 'elfutils_patch': digest(ELFUTILS_PATCH),
                    'cc': str(cc),
                    'cc_sha256': digest(cc),
                    'cc_version': subprocess.check_output([cc, '--version'], text=True)}
        executable, stamp = output / 'pahole', output / 'identity.json'
        if executable.is_file() and stamp.is_file():
            try:
                saved = json.loads(stamp.read_text())
            except json.JSONDecodeError:
                saved = None
            if isinstance(saved, dict) and saved.get('inputs') == identity and saved.get('sha256') == digest(executable):
                print('Using cached host pahole:', executable)
                return

        work = output / 'work'
        if work.exists():
            shutil.rmtree(work)
        work.mkdir()
        for name in ARCHIVES:
            # sources() verified the repository archives before extraction.
            with tarfile.open(ROOT / 'third_party/sources' / name) as archive:
                archive.extractall(work)
        elf, dwarves = work / 'elfutils-0.192', work / 'dwarves-1.28'
        environment = os.environ.copy()
        # Parent make may be a cross build. These libraries execute on the host.
        for name in ('CC', 'CXX', 'AR', 'RANLIB', 'LD', 'STRIP', 'CFLAGS', 'CXXFLAGS',
                     'CPPFLAGS', 'LDFLAGS', 'LIBS', 'MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES',
                     'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH',
                     'LD_LIBRARY_PATH', 'PKG_CONFIG_PATH', 'PKG_CONFIG_LIBDIR',
                     'CMAKE_PREFIX_PATH', 'CMAKE_TOOLCHAIN_FILE', 'PKG_CONFIG', 'CONFIG_SITE'):
            environment.pop(name, None)
        environment.update(CC=str(cc), CFLAGS='-std=gnu11 -O2 -g0', CONFIG_SITE='/dev/null')
        log = output / 'build.log'
        log.write_text('')
        # New glibc preserves input constness in bsearch/memchr results. These
        # lookups only read the result; retain that type and compiler checks.
        run(['patch', '--batch', '--forward', '--fuzz=0', '-p1', '-i', ELFUTILS_PATCH],
            elf, environment, log)
        elf_build = work / 'elf-build'
        elf_build.mkdir()
        run([elf / 'configure', '--disable-debuginfod', '--disable-libdebuginfod',
             '--disable-nls', '--without-bzlib', '--without-lzma',
             '--without-zstd'], elf_build, environment, log)
        libraries = [('lib', 'libeu.a'), ('libelf', 'libelf.a'),
                     ('backends', 'libebl_backends.a'), ('libcpu', 'libcpu.a'),
                     ('libebl', 'libebl.a'), ('libdwelf', 'libdwelf.a'),
                     ('libdwfl', 'libdwfl.a'), ('libdw', 'libdw.a')]
        for directory, target in libraries:
            run(['make', '-j' + str(jobs), '-C', elf_build / directory, target],
                work, environment, log)

        include = work / 'include'
        (include / 'elfutils').mkdir(parents=True)
        for name in ('libelf.h', 'gelf.h', 'nlist.h'):
            shutil.copyfile(elf / 'libelf' / name, include / name)
        shutil.copyfile(elf / 'libdw/dwarf.h', include / 'dwarf.h')
        for directory, name in (('libdw', 'libdw.h'), ('libdw', 'known-dwarf.h'),
                                ('libdwfl', 'libdwfl.h'), ('libdwelf', 'libdwelf.h')):
            shutil.copyfile(elf / directory / name, include / 'elfutils' / name)
        shutil.copyfile(elf_build / 'version.h', include / 'elfutils/version.h')
        host_libraries = {}
        for name in ('libz.a', 'libdl.a'):
            path = subprocess.check_output([cc, '-print-file-name=' + name], text=True).strip()
            if not Path(path).is_file():
                raise RuntimeError('host static development library is required: ' + name)
            host_libraries[name] = str(Path(path).resolve())
        pahole_build = work / 'pahole-build'
        # Static third-party libraries keep the result independent of installed
        # libdw/libelf/libbpf and of the temporary build directory's location.
        elf_libraries = ';'.join([str(elf_build / 'libelf/libelf.a'),
                                  str(elf_build / 'lib/libeu.a'),
                                  host_libraries['libz.a'], host_libraries['libdl.a']])
        run(['cmake', '-G', 'Unix Makefiles', '-S', dwarves, '-B', pahole_build,
             '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=OFF',
             '-DCMAKE_POLICY_VERSION_MINIMUM=3.10',
             '-DLIBBPF_EMBEDDED=ON', '-DGIT_SUBMODULE=OFF',
             '-DCMAKE_C_COMPILER=' + str(cc), '-DCMAKE_C_STANDARD=11',
             '-DCMAKE_C_STANDARD_REQUIRED=ON', '-DCMAKE_EXE_LINKER_FLAGS=-static',
             '-DDWARF_INCLUDE_DIR=' + str(include), '-DLIBDW_INCLUDE_DIR=' + str(include),
             '-DDWARF_LIBRARY=' + str(elf_build / 'libdw/libdw.a'),
             '-DELF_LIBRARY=' + elf_libraries, '-DZLIB_LIBRARY=' + host_libraries['libz.a']],
            work, environment, log)
        run(['cmake', '--build', pahole_build, '--target', 'pahole', '-j', str(jobs)],
            work, environment, log)
        binary = pahole_build / 'pahole'
        version = subprocess.check_output([binary, '--version'], text=True).strip()
        if version != 'v1.28':
            raise RuntimeError('unexpected pahole version: ' + version)
        headers = subprocess.check_output(['readelf', '-lW', '-dW', binary], text=True)
        if 'INTERP' in headers or '(NEEDED)' in headers:
            raise RuntimeError('host pahole must not depend on private shared libraries')
        temporary = output / 'pahole.new'
        shutil.copy2(binary, temporary)
        temporary.chmod(0o755)
        temporary.replace(executable)
        temporary_stamp = output / 'identity.json.new'
        temporary_stamp.write_text(json.dumps({'inputs': identity, 'sha256': digest(executable),
                                               'version': version}, indent=2) + '\n')
        temporary_stamp.replace(stamp)
        print('Built offline host pahole:', executable)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tools/pahole')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    build(args.output.resolve(), args.cc, args.jobs)


if __name__ == '__main__':
    main()
