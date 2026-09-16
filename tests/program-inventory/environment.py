#!/usr/bin/env python3
"""Prepare pinned RISC-V UAPI headers and build the full competition BusyBox."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
LINUX_REVISION = 'f4cdf7ca9a1fdcca413157df19753f388a5a224e'
INPUTS = Path(__file__).with_name('inputs.json')
MANIFEST = ROOT / 'references/sources.tsv'
DEFAULT = ROOT / 'build/program-environment'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def tree_sha(directory):
    digest = hashlib.sha256()
    for path in sorted(Path(directory).rglob('*')):
        if path.is_file():
            digest.update(str(path.relative_to(directory)).encode() + b'\0')
            digest.update(bytes.fromhex(sha(path)))
    return digest.hexdigest()


def build_environment():
    result = os.environ.copy()
    for name in ('CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'OBJC_INCLUDE_PATH',
                 'LIBRARY_PATH', 'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'MAKEFLAGS', 'MFLAGS'):
        result.pop(name, None)
    result.update(REALGCC='riscv64-linux-gnu-gcc', KCONFIG_NOTIMESTAMP='1',
                  LC_ALL='C', TZ='UTC')
    return result


def reference(name, kind=None):
    rows = [line.split('\t') for line in MANIFEST.read_text().splitlines()
            if line and not line.startswith('#')]
    matches = [row for row in rows if len(row) == 5 and row[1] == name]
    if len(matches) != 1 or (kind and matches[0][0] != kind):
        raise RuntimeError('missing, duplicate, or wrong-kind reference: ' + name)
    return matches[0]


def capture(argv):
    return subprocess.check_output(list(map(str, argv)), text=True, env=build_environment()).strip()


def pinned(name, revision):
    row = reference(name)
    if row[0] not in ('full', 'snapshot') or row[4] != revision:
        raise RuntimeError(name + ': revision differs from references/sources.tsv')
    source = ROOT / 'references' / name
    actual = capture(['git', '-C', source, 'rev-parse', 'HEAD'])
    if actual != revision:
        raise RuntimeError(f'{name}: expected {revision}, found {actual}')
    subprocess.run(['git', '-C', source, 'diff', '--quiet', revision, '--'], check=True)
    return source


def run(argv, log, **kwargs):
    kwargs.setdefault('env', build_environment())
    with Path(log).open('w') as stream:
        subprocess.run(list(map(str, argv)), stdout=stream, stderr=subprocess.STDOUT,
                       check=True, **kwargs)


def prepare(output=DEFAULT, uapi="runtime"):
    """Return reproducible environment identity; never add host libc headers."""
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if uapi == 'runtime':
        linux = pinned('linux', LINUX_REVISION)
        revision = LINUX_REVISION
        source_identity = {'revision': revision}
    elif uapi == 'busybox':
        source_identity = json.loads(INPUTS.read_text())['busybox_uapi']
        revision = source_identity['revision']
        row = reference(source_identity['archive_reference'], 'file')
        source_identity = dict(source_identity, archive_url=row[2], archive_sha256=row[4])
        # Reuse the reference restorer's checksum and atomic-download contract.
        selected = output / 'uapi-source.tsv'
        selected.write_text('\t'.join(row) + '\n')
        run([ROOT / 'references/fetch.sh', '--manifest', selected],
            output / 'restore-linux-6.6.log')
        archive = ROOT / 'references' / row[1]
        linux = output / 'linux-6.6'
        output = output / 'busybox-environment'
        output.mkdir(exist_ok=True)
    else:
        raise ValueError('unknown UAPI profile: ' + uapi)
    compiler = ROOT / 'build/riscv/musl-root/bin/musl-gcc'
    if not compiler.is_file():
        raise RuntimeError('run make musl-toolchain before preparing the environment')
    tools = {}
    for tool in ('make', 'gcc', 'riscv64-linux-gnu-gcc', 'riscv64-linux-gnu-ld',
                 'riscv64-linux-gnu-ar', 'rsync'):
        executable = shutil.which(tool)
        if executable is None:
            raise RuntimeError('missing build tool: ' + tool)
        tools[tool] = {'path': executable, 'sha256': sha(executable),
                       'version': capture([executable, '--version'])}
    flags = []
    if subprocess.run([compiler, '-fno-link-libatomic', '-E', '-x', 'c', '/dev/null'],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                      env=build_environment()).returncode == 0:
        flags.append('-fno-link-libatomic')
    identity = {'compiler_flags': flags, 'linux_revision': revision, 'source': source_identity, 'arch': 'riscv',
                'compiler': str(compiler), 'compiler_sha256': sha(compiler),
                'musl_specs_sha256': sha(compiler.parent.parent / 'lib/musl-gcc.specs'),
                'musl_libc_sha256': sha(compiler.parent.parent / 'lib/libc.a'),
                'musl_headers_sha256': tree_sha(compiler.parent.parent / 'include'),
                'musl_libraries_sha256': tree_sha(compiler.parent.parent / 'lib'),
                'build_environment': {'REALGCC': 'riscv64-linux-gnu-gcc',
                                      'KCONFIG_NOTIMESTAMP': '1', 'LC_ALL': 'C', 'TZ': 'UTC'},
                'builder_sha256': sha(__file__), 'tools': tools}
    include = output / 'linux-uapi/include'
    stamp = output / 'environment.json'
    if stamp.exists() and include.is_dir():
        previous = json.loads(stamp.read_text())
        if (previous.get('identity') == identity and
                previous.get('headers_sha256') == tree_sha(include) and
                (output / 'headers-probe').is_file() and
                previous.get('probe_sha256') == sha(output / 'headers-probe')):
            return previous
    stamp.unlink(missing_ok=True)
    shutil.rmtree(output / 'linux-build', ignore_errors=True)
    if uapi == 'busybox':
        # Rebuild only from the verified archive, never a modified extraction.
        shutil.rmtree(linux, ignore_errors=True)
        run(['tar', '-xf', archive, '-C', linux.parent],
            output / 'extract-linux-6.6.log')
    # Replace only generated headers so obsolete files cannot survive a rebuild.
    shutil.rmtree(output / 'linux-uapi', ignore_errors=True)
    argv = ['make', '-C', linux, 'O=' + str(output / 'linux-build'), 'ARCH=riscv',
            'INSTALL_HDR_PATH=' + str(include.parent), 'headers_install']
    run(argv, output / 'headers-install.log')
    if not (include / 'asm/unistd.h').is_file() or not (include / 'linux/kd.h').is_file():
        raise RuntimeError('incomplete RISC-V UAPI export')
    probe = output / 'headers-probe.c'
    probe.write_text('#include <stdio.h>\n#include <linux/kd.h>\n#include <asm/unistd.h>\n'
                     '#if !defined(__riscv) || __riscv_xlen != 64\n#error wrong target\n#endif\n'
                     'int main(void) { return __NR_read != 63; }\n')
    run([compiler, *flags, '-static', '-idirafter', include, probe, '-o', output / 'headers-probe'],
        output / 'headers-probe.log')
    result = {'identity': identity, 'include': str(include),
              'cflags': [*flags, '-idirafter', str(include)],
              'headers_sha256': tree_sha(include), 'command': list(map(str, argv)),
              'probe_sha256': sha(output / 'headers-probe')}
    stamp.write_text(json.dumps(result, indent=2) + '\n')
    return result


def config_settings(path):
    return [line for line in Path(path).read_text().splitlines()
            if line.startswith('CONFIG_') or
            (line.startswith('# CONFIG_') and line.endswith(' is not set'))]


def build_busybox(output=DEFAULT, jobs=2):
    environment = prepare(output, uapi='busybox')
    output = Path(output).resolve() / 'full-busybox'
    output.mkdir(parents=True, exist_ok=True)
    (output / 'build.json').unlink(missing_ok=True)
    inputs = json.loads(INPUTS.read_text())
    revision = inputs['busybox']['revision']
    config_path = inputs['busybox']['config']
    source = pinned('oscomp-testsuits', revision)
    archive = output / 'source.tar'
    with archive.open('wb') as stream:
        subprocess.run(['git', '-C', str(source), 'archive', revision,
                        'busybox', config_path], stdout=stream, check=True)
    destination = output / 'source'
    shutil.rmtree(destination, ignore_errors=True)
    destination.mkdir()
    run(['tar', '-xf', archive, '-C', destination], output / 'extract.log')
    busybox = destination / 'busybox'
    config = destination / config_path
    shutil.copy2(config, busybox / '.config')
    compiler = environment['identity']['compiler']
    # Static linking is a build-environment choice. Keep every upstream applet.
    cc = compiler + ' -static ' + ' '.join(environment['cflags'])
    make = ['make', '-C', busybox, 'CC=' + cc,
            'CROSS_COMPILE=riscv64-linux-gnu-', 'HOSTCC=gcc']
    run(make + ['oldconfig'], output / 'configure.log', input='\n' * 4096, text=True)
    if config_settings(config) != config_settings(busybox / '.config'):
        raise RuntimeError('upstream BusyBox feature configuration changed')
    run(make + ['-j' + str(jobs)], output / 'build.log')
    shutil.copy2(busybox / '.config', output / 'competition.config')
    result = {'revision': revision, 'inputs_sha256': sha(INPUTS), 'source_archive_sha256': sha(archive),
              'upstream_config_sha256': sha(config),
              'resolved_config_sha256': sha(output / 'competition.config'),
              'binary': str(busybox / 'busybox'), 'binary_sha256': sha(busybox / 'busybox'),
              'license_sha256': sha(busybox / 'LICENSE'), 'environment': environment,
              'command': list(map(str, make + ['-j' + str(jobs)]))}
    (output / 'build.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=DEFAULT)
    parser.add_argument('--busybox', action='store_true')
    parser.add_argument('--uapi', choices=('runtime', 'busybox'), default='runtime')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    print(json.dumps(build_busybox(args.output, args.jobs) if args.busybox
                     else prepare(args.output, args.uapi), indent=2))
