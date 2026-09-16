#!/usr/bin/env python3
"""Inventory a small, unmodified fixed-upstream BusyBox workload on two kernels."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
REVISION = 'b5ec6ef8497e1818cbdec3b54bb722f036e57972'
SOURCE = ROOT / 'references/oscomp-testsuits'
CASES = ('true', 'false', 'echo', 'cat', 'ls', 'shell')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def capture(command):
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT).strip()


def command(argv, log, timeout=300, input=None):
    with log.open('w') as stream:
        try:
            result = subprocess.run(argv, input=input, text=True, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=timeout)
            return result.returncode
        except subprocess.TimeoutExpired:
            return 'timeout'


def observations(raw):
    results = {}
    for name in CASES:
        output = re.findall(r'^INVENTORY OUTPUT ' + name + r' ((?:[0-9a-f]{2})*)\.$', raw, re.M)
        status = re.findall(r'^INVENTORY STATUS ' + name + r' (\d+)$', raw, re.M)
        if len(output) == len(status) == 1:
            results[name] = {'wait_status': int(status[0]), 'output_hex': output[0]}
    return results


def run_status(raw, returncode, case_statuses=(), kernel=None):
    """Completed children remain observations; a broken guest is never a pass."""
    lines = [line for line in raw.replace('\r\n', '\n').splitlines()
             if line == 'INVENTORY' or line.startswith('INVENTORY ')]
    patterns = [r'INVENTORY BEGIN 1']
    for name in CASES:
        patterns += [r'INVENTORY OUTPUT ' + name + r' (?:[0-9a-f]{2})*\.',
                     r'INVENTORY STATUS ' + name + r' \d+']
    patterns.append(r'INVENTORY END ' + str(len(CASES)))
    errors = []
    if len(lines) != len(patterns):
        errors.append('missing or duplicated protocol records')
    for index, (line, pattern) in enumerate(zip(lines, patterns)):
        if re.fullmatch(pattern, line) is None:
            errors.append(f'malformed, unknown, or reordered record {index + 1}: {line}')
    protocol_complete = not errors
    cleanup_ok = (kernel != 'boaros' or re.search(
        r'^BoarOS: PID 1 exited status=0x2a pages=0x[1-9a-f][0-9a-f]* heap-live=0x0; shutting down$',
        raw, re.M) is not None)
    if not cleanup_ok:
        errors.append('BoarOS did not report successful resource cleanup')
    if returncode == 'timeout':
        status = 'timeout'
    elif returncode != 0 or not cleanup_ok:
        status = 'crash'
    elif errors:
        status = 'protocol-error'
    elif any(case != 'pass' for case in case_statuses):
        status = 'semantic-mismatch'
    else:
        status = 'pass'
    return {'status': status, 'returncode': returncode,
            'protocol_complete': protocol_complete, 'errors': errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/program-inventory')
    parser.add_argument('--timeout', type=int, default=45)
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    destination = args.output.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    metadata = {'source': str(SOURCE.relative_to(ROOT)), 'revision': REVISION,
                'runner_sha256': sha(__file__), 'driver_sha256': sha(HERE / 'init.c'),
                'commands': [], 'results': [], 'guest_runs': {}, 'runtime_status': 'not-run'}
    record_path = destination / 'inventory.json'

    def run(argv, name, **kwargs):
        log = destination / (name + '.log')
        code = command([str(value) for value in argv], log, **kwargs)
        metadata['commands'].append({'argv': list(map(str, argv)), 'log': str(log), 'result': code})
        return code

    def blocked(program, reason):
        metadata['results'].append({'program': program, 'status': 'build-blocked', 'reason': reason})

    try:
        if capture(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD']) != REVISION:
            raise RuntimeError('upstream reference HEAD does not match the pinned revision')
        manifest = (ROOT / 'references/sources.tsv').read_text()
        if not any(row.split('\t')[1:2] == ['oscomp-testsuits'] and row.split('\t')[-1] == REVISION
                   for row in manifest.splitlines() if not row.startswith('#')):
            raise RuntimeError('inventory revision differs from references/sources.tsv')
        tree = capture(['git', '-C', str(SOURCE), 'ls-tree', '--name-only', REVISION])
        (destination / 'source-tree.log').write_text(tree + '\n')
        if 'libc-test' not in tree.splitlines():
            blocked('libc-test', 'pinned source tree has no libc-test directory; see source-tree.log')
        else:
            blocked('libc-test', 'libc-test build recipe requires review for this pinned tree')
        compiler = ROOT / 'build/riscv/musl-root/bin/musl-gcc'
        if not compiler.exists():
            blocked('busybox', 'existing musl toolchain absent; run make musl-toolchain first')
            return 1
        tools = [str(compiler), 'riscv64-linux-gnu-gcc', 'riscv64-linux-gnu-ld',
                 'riscv64-linux-gnu-ar', 'gcc', 'make', 'qemu-system-riscv64', 'mkfs.ext4']
        metadata['tools'] = {}
        for tool in tools:
            path = shutil.which(tool)
            if not path:
                blocked('busybox', 'missing tool: ' + tool)
                return 1
            version = capture([tool, '-V' if tool == 'mkfs.ext4' else '--version'])
            metadata['tools'][tool] = {'path': path, 'sha256': sha(path), 'version': version}
        metadata['musl_specs_sha256'] = sha(ROOT / 'build/riscv/musl-root/lib/musl-gcc.specs')
        metadata['musl_libc_sha256'] = sha(ROOT / 'build/riscv/musl-root/lib/libc.a')
        # A git archive reads the exact tree, independent of worktree content.
        archive = destination / 'upstream.tar'
        with archive.open('wb') as stream:
            subprocess.run(['git', '-C', str(SOURCE), 'archive', REVISION, 'busybox',
                            'config/busybox-config-riscv64'], stdout=stream, check=True)
        metadata['source_archive_sha256'] = sha(archive)
        source_copy = destination / 'source'
        if source_copy.exists():
            shutil.rmtree(source_copy)
        source_copy.mkdir()
        if run(['tar', '-xf', archive, '-C', source_copy], 'extract') != 0:
            raise RuntimeError('archive extraction failed')
        source = source_copy / 'busybox'
        config = source_copy / 'config/busybox-config-riscv64'
        metadata['upstream_config_sha256'] = sha(config)
        metadata['license_sha256'] = sha(source / 'LICENSE')
        shutil.copy2(config, source / '.config')
        flags = []
        if run([compiler, '-fno-link-libatomic', '-E', '-x', 'c', '/dev/null'],
               'compiler-flags') == 0:
            flags.append('-fno-link-libatomic')
        metadata['compiler_flags'] = flags
        make = ['make', '-C', source, 'CC=' + str(compiler) + ' -static ' + ' '.join(flags),
                'CROSS_COMPILE=riscv64-linux-gnu-', 'HOSTCC=gcc']
        result = run(make + ['oldconfig'], 'busybox-configure', input='\n' * 4096)
        if result == 0:
            result = run(make + ['-j' + str(args.jobs)], 'busybox-build')
        metadata['resolved_config_sha256'] = sha(source / '.config')
        shutil.copy2(source / '.config', destination / 'competition.config')
        metadata['busybox_profile'] = 'competition'
        if result != 0:
            blocked('busybox-competition', 'upstream config/build failed; see busybox-configure.log and busybox-build.log')
            metadata['busybox_profile'] = 'smoke'
            metadata['smoke_config_sha256'] = sha(HERE / 'busybox-smoke.config')
            run(make + ['distclean'], 'busybox-smoke-clean')
            result = run(make + ['allnoconfig'], 'busybox-smoke-defaults')
            if result == 0:
                resolved = (source / '.config').read_text()
                for setting in (HERE / 'busybox-smoke.config').read_text().splitlines():
                    key = setting.split('=', 1)[0]
                    resolved, count = re.subn(r'^(?:' + key + r'=.*|# ' + key +
                                             r' is not set)$', setting, resolved, flags=re.M)
                    if count != 1:
                        raise RuntimeError('unknown or duplicate smoke config key: ' + key)
                (source / '.config').write_text(resolved)
                result = run(make + ['oldconfig'], 'busybox-smoke-configure', input='\n' * 4096)
            if result == 0:
                result = run(make + ['-j' + str(args.jobs)], 'busybox-smoke-build')
            metadata['smoke_resolved_config_sha256'] = sha(source / '.config')
            shutil.copy2(source / '.config', destination / 'smoke.config')
            if result != 0:
                blocked('busybox-smoke', 'minimal config/build failed; see busybox-smoke-build.log')
                return 0
        program = source / 'busybox'
        metadata['busybox_sha256'] = sha(program)
        driver = destination / 'init-rv'
        if run([compiler, *flags, '-static', '-O2', '-Wall', '-Wextra', HERE / 'init.c', '-o', driver], 'driver-build') != 0:
            blocked('inventory-driver', 'see driver-build.log')
            return 1
        metadata['driver_elf_sha256'] = sha(driver)
        specification = importlib.util.spec_from_file_location('diff_harness', ROOT / 'tests/diff-abi/harness.py')
        harness = importlib.util.module_from_spec(specification)
        sys.path.insert(0, str(ROOT / 'tests/diff-abi'))
        try:
            specification.loader.exec_module(harness)
        finally:
            sys.path.pop(0)
        image, identity = harness.linux_build()
        metadata['linux'] = identity
        kernel = ROOT / 'kernel-rv'
        metadata['boaros_sha256'] = sha(kernel)
        fixture_directory = destination / 'fixture-build'
        if fixture_directory.exists():
            shutil.rmtree(fixture_directory)
        fixture_directory.mkdir()
        disk = harness.fixture(fixture_directory, driver)
        commands = fixture_directory / 'programs.debugfs'
        data = fixture_directory / 'inventory-data'
        data.write_bytes(b'inventory data\n')
        commands.write_text(f'write {program} /busybox\nset_inode_field /busybox mode 0100755\n'
                            f'write {data} /inventory-data\nmkdir /inventory-dir\n'
                            f'write {data} /inventory-dir/entry\n')
        if run(['debugfs', '-w', '-f', commands, disk], 'fixture-programs') != 0:
            raise RuntimeError('fixture population failed')
        metadata['fixture_sha256'] = sha(disk)
        collected = {}
        for name, boot_kernel in [('linux', image), ('boaros', kernel)]:
            target = destination / (name + '.img')
            shutil.copyfile(disk, target)
            argv = ['qemu-system-riscv64', '-machine', 'virt', '-bios', 'default',
                    '-kernel', boot_kernel, '-m', '512M', '-smp', '1', '-nographic', '-no-reboot',
                    '-drive', f'file={target},if=none,format=raw,id=root',
                    '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
            if name == 'linux':
                argv += ['-append', 'root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
            code = run(argv, name, timeout=args.timeout)
            raw = (destination / (name + '.log')).read_text(errors='replace').replace('\r\n', '\n')
            results = observations(raw)
            collected[name] = results
            case_statuses = []
            for case in CASES:
                observed = results.get(case)
                reference = collected.get('linux', {}).get(case)
                if observed is None:
                    status = 'timeout' if code == 'timeout' else 'crash'
                elif observed['wait_status'] & 127:
                    status = 'crash'
                elif name == 'boaros' and (reference is None or reference['wait_status'] != (256 if case == 'false' else 0)):
                    status = 'semantic-mismatch'
                elif name == 'boaros' and observed != reference:
                    output_bytes = bytes.fromhex(observed['output_hex'])
                    status = ('missing-capability' if b'execve errno=38' in output_bytes
                              else 'semantic-mismatch')
                elif name == 'linux' and observed['wait_status'] != (256 if case == 'false' else 0):
                    status = 'semantic-mismatch'
                else:
                    status = 'pass'
                case_statuses.append(status)
                metadata['results'].append({'program': 'busybox-' + metadata['busybox_profile'], 'case': case,
                    'kernel': name, 'status': status, 'observed': observed,
                    'log': str(destination / (name + '.log'))})
            state = run_status(raw, code, case_statuses, kernel=name)
            metadata['guest_runs'][name] = state
            metadata[name + '_complete'] = state['protocol_complete']
        metadata['runtime_status'] = ('passed' if all(guest['status'] == 'pass'
            for guest in metadata['guest_runs'].values()) else 'failed')
        print(json.dumps({'runtime_status': metadata['runtime_status'],
                          'guest_runs': metadata['guest_runs'],
                          'results': metadata['results']}, indent=2))
        return 0
    except Exception as error:
        metadata['runner_error'] = str(error)
        metadata['runtime_status'] = 'failed'
        print(f'program-inventory: {error}', file=sys.stderr)
        return 1
    finally:
        record_path.write_text(json.dumps(metadata, indent=2) + '\n')
        print(f'Inventory artifacts: {record_path}')


if __name__ == '__main__':
    sys.exit(main())
