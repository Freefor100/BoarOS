#!/usr/bin/env python3
"""Build every pinned pre-2025 libc-test static/dynamic dispatch case unchanged."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
INPUTS = Path(__file__).with_name('inputs.json')
PINS = json.loads(INPUTS.read_text())
SOURCE = ROOT / PINS['object_store']
REVISION = PINS['preliminary_suite']['revision']
ORIGIN = PINS['repository']


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def capture(argv):
    return subprocess.check_output(list(map(str, argv)), text=True,
                                   stderr=subprocess.STDOUT).strip()


def build(output=None, jobs=2, uapi_include=None):
    destination = Path(output or ROOT / 'build/program-libc').resolve()
    destination.mkdir(parents=True, exist_ok=True)
    metadata = {'status': 'building', 'source': str(SOURCE), 'origin': ORIGIN,
                'revision': REVISION, 'builder_sha256': digest(__file__),
                'input_pins_sha256': digest(INPUTS),
                'selected_input': PINS['preliminary_suite'],
                'commands': [], 'cases': [], 'files': [], 'failures': []}

    def run(argv, name, timeout=300):
        argv = list(map(str, argv))
        logfile = destination / (name + '.log')
        with logfile.open('w') as stream:
            try:
                result = subprocess.run(argv, stdout=stream, stderr=subprocess.STDOUT,
                                        stdin=subprocess.DEVNULL, timeout=timeout)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
        metadata['commands'].append({'argv': argv, 'log': str(logfile), 'result': code})
        return code

    def artifact(path, guest_path):
        path = Path(path)
        item = {'source': str(path), 'destination': guest_path, 'sha256': digest(path)}
        if path.read_bytes()[:4] == b'\x7fELF':
            headers = capture(['riscv64-linux-gnu-readelf', '-lW', path])
            dynamic = capture(['riscv64-linux-gnu-readelf', '-dW', path])
            interpreter = re.search(r'Requesting program interpreter: ([^\]]+)\]', headers)
            item['interpreter'] = interpreter.group(1) if interpreter else None
            item['needed'] = re.findall(r'\(NEEDED\).*\[([^\]]+)\]', dynamic)
        metadata['files'].append(item)
        return item

    try:
        metadata['reference_head_before'] = capture(['git', '-C', SOURCE, 'rev-parse', 'HEAD'])
        if capture(['git', '-C', SOURCE, 'remote', 'get-url', 'origin']) != ORIGIN:
            raise RuntimeError('unexpected reference origin')
        if run(['git', '-C', SOURCE, 'cat-file', '-e', REVISION + '^{commit}'], 'check-revision') != 0:
            if run(['git', '-C', SOURCE, 'fetch', '--no-tags', 'origin', REVISION], 'fetch-revision') != 0:
                raise RuntimeError('cannot restore the exact selected revision')
        compiler = ROOT / 'build/riscv/musl-root/bin/musl-gcc'
        musl = compiler.parent.parent
        tools = [str(compiler), 'riscv64-linux-gnu-gcc', 'riscv64-linux-gnu-objcopy',
                 'riscv64-linux-gnu-readelf', 'riscv64-linux-gnu-ld', 'make', 'tar', 'git']
        metadata['tools'] = {}
        for tool in tools:
            resolved = shutil.which(tool)
            if resolved is None:
                raise RuntimeError('missing build tool: ' + tool)
            metadata['tools'][tool] = {'path': resolved, 'sha256': digest(resolved),
                                       'version': capture([tool, '--version'])}
        metadata['musl'] = {name: digest(musl / 'lib' / name)
                            for name in ('libc.a', 'libc.so', 'musl-gcc.specs')}
        archive = destination / 'upstream.tar'
        archive_command = ['git', '-C', str(SOURCE), 'archive', REVISION, 'libc-test',
                           'scripts/libctest', 'README.md', 'Makefile', 'Makefile.sub']
        with archive.open('wb') as stream:
            subprocess.run(archive_command, stdout=stream, check=True)
        metadata['commands'].append({'argv': archive_command, 'artifact': str(archive), 'result': 0})
        metadata['source_archive_sha256'] = digest(archive)
        source_copy = destination / 'source'
        if source_copy.exists():
            shutil.rmtree(source_copy)
        source_copy.mkdir()
        if run(['tar', '-xf', archive, '-C', source_copy], 'extract') != 0:
            raise RuntimeError('source export failed')
        source = source_copy / 'libc-test'
        metadata['inputs'] = {name: digest(source / name) for name in
                              ('Makefile', 'README.md', 'COPYRIGHT', 'config.mak.def',
                               'static.txt', 'dynamic.txt', 'entry.c')}
        metadata['upstream_recipe_sha256'] = digest(source_copy / 'Makefile.sub')
        metadata['upstream_runner_sha256'] = digest(source_copy / 'scripts/libctest/libctest_testcode.sh')
        flags = []
        if run([compiler, '-fno-link-libatomic', '-E', '-x', 'c', '/dev/null'], 'compiler-probe') == 0:
            flags.append('-fno-link-libatomic')
        if uapi_include is not None:
            include = Path(uapi_include).resolve()
            if not include.is_dir():
                raise RuntimeError('UAPI include directory missing')
            flags += ['-idirafter', str(include)]
        cc = shlex.join([str(compiler), *flags])
        ldflags = '-Os -s -lpthread -lm -lrt -Wl,--dynamic-linker=/lib/ld-musl-riscv64.so.1'
        configuration = {'CC': cc, 'PREFIX': 'riscv64-linux-gnu-', 'LDFLAGS': ldflags}
        metadata['configuration'] = configuration
        metadata['configuration_sha256'] = hashlib.sha256(json.dumps(configuration, sort_keys=True).encode()).hexdigest()
        metadata['source_changes'] = []
        metadata['adaptations'] = [
            'Use existing musl 1.2.5 wrapper and supported -fno-link-libatomic toolchain flag.',
            'Set PT_INTERP to /lib/ld-musl-riscv64.so.1 instead of the build-host install prefix.',
        ]
        command = ['make', '-C', source, '-j' + str(jobs), 'disk',
                   *[key + '=' + value for key, value in configuration.items()]]
        code = run(command, 'build', timeout=600)
        if code != 0:
            metadata['status'] = 'build-blocked'
            metadata['failures'].append({'phase': 'upstream make disk', 'result': code,
                                         'log': str(destination / 'build.log')})
            return metadata
        for kind in ('static', 'dynamic'):
            program = source / ('entry-' + kind + '.exe')
            info = artifact(program, '/' + program.name)
            if (kind == 'static' and info['interpreter'] is not None) or (
                kind == 'dynamic' and info['interpreter'] != '/lib/ld-musl-riscv64.so.1'):
                raise RuntimeError('unexpected program interpreter for ' + kind)
            entries = (source / (kind + '.txt')).read_text().splitlines()
            expected_commands = ['./runtest.exe -w ' + program.name + ' ' +
                                 Path(entry).stem.replace('-', '_') for entry in entries]
            if (source / ('run-' + kind + '.sh')).read_text().splitlines() != expected_commands:
                raise RuntimeError('upstream generated script disagrees with case dispatch table')
            for entry in entries:
                if not entry or not entry.endswith('.exe'):
                    raise RuntimeError('invalid upstream case entry: ' + entry)
                case = Path(entry).stem.replace('-', '_')
                metadata['cases'].append({'id': 'libc.' + kind + '.' + case,
                    'suite': 'libc-' + kind, 'upstream_entry': entry,
                    'argv': ['/' + program.name, case], 'expected_exit': 0,
                    'cwd': '/', 'environment': ['LD_LIBRARY_PATH=/lib:/', 'LC_ALL=C', 'PATH=/']})
        if len({case['id'] for case in metadata['cases']}) != len(metadata['cases']):
            raise RuntimeError('duplicate upstream case dispatch names')
        artifact(source / 'runtest.exe', '/runtest.exe')
        for script in ('run-static.sh', 'run-dynamic.sh', 'run-all.sh'):
            artifact(source / script, '/' + script)
        artifact(source_copy / 'scripts/libctest/libctest_testcode.sh', '/libctest_testcode.sh')
        for shared_object in sorted(source.glob('src/*/*.so')):
            artifact(shared_object, '/lib/' + shared_object.name)
            artifact(shared_object, '/' + shared_object.name)
        artifact(musl / 'lib/libc.so', '/lib/libc.so')
        artifact(musl / 'lib/libc.so', '/lib/ld-musl-riscv64.so.1')
        metadata['counts'] = {kind: sum(case['suite'] == 'libc-' + kind for case in metadata['cases'])
                              for kind in ('static', 'dynamic')}
        for kind in ('static', 'dynamic'):
            names = [case['argv'][1] for case in metadata['cases']
                     if case['suite'] == 'libc-' + kind]
            metadata['cases'].append({
                'id': 'libc.official.' + kind, 'suite': 'libc-official',
                'argv': ['/busybox', 'sh', '/run-' + kind + '.sh'],
                'expected_exit': 1, 'timeout': 90, 'cwd': '/',
                'environment': ['LD_LIBRARY_PATH=/lib:/', 'LC_ALL=C', 'PATH=/'],
                'requires': ['/busybox'],
                'output_contract': {'kind': 'libc-runtest', 'entry': 'entry-' + kind + '.exe',
                                    'cases': names, 'pass_line': 'Pass!',
                                    'reject_prefix': 'FAIL '}})
        metadata['counts']['official_wrappers'] = 2
        metadata['runtime_contract'] = {
            'direct_cases': 'Run the unchanged upstream entry ELF with its upstream table case name.',
            'official_wrapper': 'run-static.sh/run-dynamic.sh invoke runtest.exe -w entry-*.exe case; wrapper additionally requires sigtimedwait and resource limits. Pinned runtest unconditionally returns 1 even after Pass!, so unchanged shell scripts normally exit 1. Exit alone is insufficient: require every START/Pass!/END record and reject FAIL lines.',
            'working_directory': '/', 'library_path': '/lib:/',
            'fixtures': 'Both loader names and upstream DSOs are mapped explicitly; suite runner supplies /tmp and /dev.'}
        metadata['status'] = 'built'
        return metadata
    except Exception as error:
        metadata['status'] = 'build-blocked'
        metadata['failures'].append({'phase': 'environment or artifact validation', 'reason': str(error)})
        return metadata
    finally:
        metadata['reference_head_after'] = capture(['git', '-C', SOURCE, 'rev-parse', 'HEAD'])
        (destination / 'build.json').write_text(json.dumps(metadata, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--uapi-include', type=Path)
    args = parser.parse_args()
    result = build(args.output, args.jobs, args.uapi_include)
    print(json.dumps({'status': result['status'], 'counts': result.get('counts'),
                      'failures': result['failures']}, indent=2))
    raise SystemExit(0 if result['status'] == 'built' else 1)
