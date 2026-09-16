#!/usr/bin/env python3
"""Build complete pinned programs, then inventory isolated RISC-V executions."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def busybox_manifest(build, directory, inputs):
    """Use unchanged upstream script and command list with the full binary."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    revision = inputs['preliminary_suite']['revision']
    source = ROOT / inputs['object_store']
    files = [{'source': build['binary'], 'destination': '/busybox',
              'sha256': build['binary_sha256'], 'mode': '755'}]
    for name in ('busybox_testcode.sh', 'busybox_cmd.txt'):
        data = subprocess.check_output(['git', '-C', str(source), 'show',
                                       revision + ':scripts/busybox/' + name])
        path = directory / name
        path.write_bytes(data)
        files.append({'source': str(path), 'destination': '/' + name,
                      'sha256': sha(path), 'mode': '755' if name.endswith('.sh') else '644'})
    data = directory / 'inventory-data'
    data.write_bytes(b'inventory data\n')
    files += [{'source': str(data), 'destination': name, 'sha256': sha(data)}
              for name in ('/inventory-data', '/inventory-dir/entry')]
    table = Path(build['binary']).parent / 'include/applet_tables.h'
    names = re.findall(r'^"([^"/]+)" "\\0"$', table.read_text(), re.M)
    if len(names) != int(re.search(r'#define NUM_APPLETS (\d+)', table.read_text())[1]):
        raise ValueError('incomplete BusyBox applet installation list')
    commands = [
        ('true', ['true'], 0), ('false', ['false'], 1),
        ('echo', ['echo', 'inventory'], 0), ('cat', ['cat', '/inventory-data'], 0),
        ('ls', ['ls', '/inventory-dir'], 0),
        ('shell', ['sh', '-c', "printf 'shell\\n'; /busybox cat /inventory-data"], 0),
        ('od', ['od', '-An', '-tx1', '/inventory-data'], 0),
        ('hexdump', ['hexdump', '-C', '/inventory-data'], 0)]
    cases = [{'id': 'busybox.' + name, 'argv': ['/busybox', *argv],
              'expected_exit': expected} for name, argv, expected in commands]
    commands_data = (directory / 'busybox_cmd.txt').read_bytes()
    if not commands_data.endswith(b'\n'):
        raise ValueError('upstream command list lacks final newline; review shell read semantics')
    cases.append({'id': 'busybox.official',
        'argv': ['/busybox', 'sh', '/busybox_testcode.sh'], 'expected_exit': 0,
        'timeout': 90, 'output_contract': {'kind': 'busybox-script',
            'expected_records': len(commands_data.splitlines()),
            'commands': [re.sub(r'\\(.)', r'\1', line.strip(' \t'))
                         for line in commands_data.decode().splitlines()]}})
    return {'files': files, 'cases': cases,
            'symlinks': [{'target': '/busybox', 'destination': '/bin/' + name} for name in names],
            'directories': ['/tmp', '/proc', '/sys', '/dev', '/bin', '/etc', '/inventory-dir'],
            'env': {'PATH': '/bin:/', 'LC_ALL': 'C', 'TERM': 'linux'},
            'sources': {'busybox': build, 'script_revision': revision,
                        'command_list_sha256': sha(directory / 'busybox_cmd.txt')}}


def validate_build(metadata, expected_revision):
    if metadata.get('revision') != expected_revision:
        raise ValueError('build revision differs from selected input')
    files = metadata.get('files', [])
    if 'binary' in metadata:
        files = [{'source': metadata['binary'], 'sha256': metadata['binary_sha256']}]
    if not files or any(sha(item['source']) != item['sha256'] for item in files):
        raise ValueError('missing or modified build artifact')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/program-inventory-full')
    parser.add_argument('--suite', choices=('all', 'busybox', 'libc'), default='all')
    parser.add_argument('--case', action='append', dest='case_ids')
    parser.add_argument('--reuse-builds', action='store_true', help='verify and reuse existing program builds')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--timeout', type=float, default=10)
    parser.add_argument('--require-pass', action='store_true', help='also fail for program incompatibilities')
    args = parser.parse_args()
    destination = args.output.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    # Local unpacked tools are optional; all executable identities are recorded.
    local_tools = ROOT / 'build/diff-abi/tools/usr/bin'
    if local_tools.is_dir():
        os.environ['PATH'] = str(local_tools) + os.pathsep + os.environ['PATH']
    import environment
    import libc_build
    import suites
    from reports import validate_output
    sys.path.insert(0, str(ROOT / 'tests/diff-abi'))
    import harness
    inputs = json.loads((HERE / 'inputs.json').read_text())
    metadata = {'inputs': inputs, 'inputs_sha256': sha(HERE / 'inputs.json'),
                'runner_sha256': sha(__file__), 'status': 'preparing'}
    try:
        if args.reuse_builds:
            busybox = json.loads((ROOT / 'build/program-environment/full-busybox/build.json').read_text())
        else:
            busybox = environment.build_busybox(jobs=args.jobs)
        validate_build(busybox, inputs['busybox']['revision'])
        manifest = busybox_manifest(busybox, destination / 'upstream-scripts', inputs)
        if args.suite == 'libc':
            manifest['cases'] = []
        if args.suite in ('all', 'libc'):
            if args.reuse_builds:
                libc = json.loads((ROOT / 'build/program-libc/build.json').read_text())
            else:
                libc = libc_build.build(jobs=args.jobs)
            if libc['status'] != 'built':
                raise RuntimeError('libc-test build incomplete: ' + json.dumps(libc['failures']))
            validate_build(libc, inputs['preliminary_suite']['revision'])
            manifest['files'].extend(libc['files'])
            for case in libc['cases']:
                case = dict(case)
                case['env'] = dict(value.split('=', 1) for value in case.pop('environment', []))
                # Keep an installed applet PATH for the unchanged official wrappers.
                case['env']['PATH'] = '/bin:/'
                manifest['cases'].append(case)
            manifest['sources']['libc'] = libc
        manifest_file = destination / 'manifest.json'
        manifest_file.write_text(json.dumps(manifest, indent=2) + '\n')
        metadata['case_count'] = len(manifest['cases'])
        metadata['manifest_sha256'] = sha(manifest_file)
        if args.build_only:
            metadata['status'] = 'built'
            return 0
        compiler = ROOT / 'build/riscv/musl-root/bin/musl-gcc'
        driver = destination / 'suite-driver-rv'
        command = [str(compiler), *busybox['environment']['identity']['compiler_flags'],
                   '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                   str(HERE / 'suite_driver.c'), '-o', str(driver)]
        with (destination / 'driver-build.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        image, linux_identity = harness.linux_build(HERE / 'linux.config')
        metadata.update(linux=linux_identity, driver_command=command,
                        driver_sha256=sha(driver), boaros_sha256=sha(ROOT / 'kernel-rv'))
        result = suites.run_suite(manifest, destination / 'runs', driver, image, ROOT / 'kernel-rv',
                                 case_ids=args.case_ids, default_timeout=args.timeout,
                                 output_validator=validate_output)
        metadata['status'] = 'inventoried'
        metadata['execution'] = result
        print(json.dumps(result, indent=2))
        if args.require_pass and (result.get('status') != 'complete' or any(status != 'pass' for status in result.get('counts', {}))):
            return 1
        return 0
    except Exception as error:
        metadata.update(status='environment-or-runner-error', error=str(error))
        print(str(error), file=sys.stderr)
        return 1
    finally:
        (destination / 'inventory.json').write_text(json.dumps(metadata, indent=2) + '\n')
        print('Program inventory: ' + str(destination / 'inventory.json'))


if __name__ == '__main__':
    raise SystemExit(main())
