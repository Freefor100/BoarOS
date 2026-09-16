#!/usr/bin/env python3
"""Isolated QEMU-system real-program cases with a shared reference fixture.

No emulator is started at import time. Call run_suite explicitly; guests run
serially, and every case starts from the same fixture on both kernels.
"""
import hashlib
import inspect
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
ID = re.compile(r'[A-Za-z0-9._-]+')
DEFAULT_ENV = {'PATH': '/bin:/usr/bin:/', 'LC_ALL': 'C',
               'LD_LIBRARY_PATH': '/lib:/', 'TMPDIR': '/tmp', 'TERM': 'linux'}


def reference_environment_ready(observation):
    mounts = {'proc', 'sysfs', 'shm', 'mqueue'}
    for name in mounts | {'shm-dir', 'mqueue-dir', 'lo'}:
        value = observation['environment'].get(name, {})
        if value.get('result') == 0:
            continue
        if name in mounts and value.get('result') == -1 and value.get('errno') == 16:
            continue  # An already mounted facility is available.
        return False
    return True


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def save_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')
    temporary.replace(path)


def parse_observation(raw, case_id):
    result = {'complete': False, 'stdout_hex': '', 'stderr_hex': '',
              'wait_status': None, 'exit_code': None, 'signal': None,
              'exec_errno': None, 'setup_errno': None, 'timed_out': False, 'environment': {}, 'errors': []}
    lines = [line for line in raw.replace('\r\n', '\n').splitlines()
             if line == 'SUITE' or line.startswith('SUITE ')]
    if not lines or lines[0] != f'SUITE BEGIN 1 {case_id}':
        result['errors'].append('missing or malformed begin record')
    if not lines or lines[-1] != f'SUITE END {case_id}':
        result['errors'].append('missing or malformed end record')
    phase = 'data'
    for line in lines[1:-1]:
        data = re.fullmatch(r'SUITE DATA ([OE]) ((?:[0-9a-f]{2})*)\.', line)
        env = re.fullmatch(r'SUITE ENV ([a-z0-9-]+) (-?\d+) (\d+)', line)
        if data and phase == 'data':
            key = 'stdout_hex' if data[1] == 'O' else 'stderr_hex'
            result[key] += data[2]
        elif env and phase == 'data' and env[1] not in result['environment']:
            result['environment'][env[1]] = {'result': int(env[2]), 'errno': int(env[3])}
        elif re.fullmatch(r'SUITE (EXEC|SETUP) \d+', line) and phase == 'data':
            key = 'exec_errno' if line.startswith('SUITE EXEC ') else 'setup_errno'
            result[key] = int(line.split()[-1])
            phase = 'exec'
        elif re.fullmatch(r'SUITE WAIT \d+', line) and phase in {'data', 'exec'}:
            result['wait_status'] = int(line.split()[-1])
            phase = 'wait'
        elif re.fullmatch(r'SUITE TIMEOUT [01]', line) and phase == 'wait':
            result['timed_out'] = line.endswith('1')
            phase = 'timeout'
        else:
            result['errors'].append('malformed, incomplete, duplicate or out-of-order record: ' + line)
    if phase != 'timeout':
        result['errors'].append('missing wait/timeout records')
    status = result['wait_status']
    if status is not None:
        signal = status & 127
        if signal == 0:
            result['exit_code'] = (status >> 8) & 255
        elif signal != 127:
            result['signal'] = signal
        else:
            result['errors'].append('child stopped without a terminal status')
    result['complete'] = not result['errors']
    return result


def observation_status(observed, expected_exit=0):
    if observed.get('guest_status') not in {None, 'complete'}:
        return observed['guest_status']
    if observed.get('timed_out'):
        return 'timeout'
    if not observed.get('complete'):
        return 'protocol-error'
    if observed.get('setup_errno') is not None:
        return 'setup-error'
    if observed.get('exec_errno') is not None:
        return 'exec-error'
    if observed.get('signal') is not None:
        return 'signal'
    if observed.get('exit_code') != expected_exit:
        return 'nonzero-exit'
    if observed.get('contract', {}).get('status', 'pass') != 'pass':
        return observed['contract']['status']
    return 'pass'


def compare_observations(reference, observed, expected_exit=0):
    linux_status = observation_status(reference, expected_exit)
    boaros_status = observation_status(observed, expected_exit)
    if linux_status != 'pass':
        status = 'reference-not-pass'
    elif boaros_status != 'pass':
        status = boaros_status
    elif any(reference[key] != observed[key] for key in ('stdout_hex', 'stderr_hex', 'wait_status')):
        status = 'output-mismatch'
    else:
        status = 'pass'
    return {'status': status, 'linux_status': linux_status, 'boaros_status': boaros_status}


def case_configuration(case, manifest, default_timeout=10):
    case_id = case['id']
    if not ID.fullmatch(case_id) or case_id in {'.', '..'}:
        raise ValueError('unsafe case id: ' + repr(case_id))
    arguments = case['argv']
    if not arguments or len(arguments) > 128 or not arguments[0].startswith('/'):
        raise ValueError('argv must contain an absolute executable and at most 128 arguments')
    env = {**DEFAULT_ENV, **manifest.get('env', {}), **case.get('env', {})}
    if len(env) > 128 or any(not key or '=' in key for key in env):
        raise ValueError('invalid environment vector')
    timeout = float(case.get('timeout', default_timeout))
    if not 0 < timeout <= 3600:
        raise ValueError('case timeout must be in (0, 3600] seconds')
    cwd = case.get('cwd', manifest.get('cwd', '/'))
    if not cwd.startswith('/'):
        raise ValueError('cwd must be absolute')
    fields = ['SUITE1', case_id, str(max(1, int(timeout * 1000))), cwd, str(len(env)),
              *[key + '=' + str(value) for key, value in sorted(env.items())],
              str(len(arguments)), *arguments]
    if any(not isinstance(value, str) or '\0' in value for value in fields):
        raise ValueError('configuration values must be NUL-free strings')
    encoded = b'\0'.join(value.encode() for value in fields) + b'\0'
    if len(encoded) >= 65536:
        raise ValueError('case configuration exceeds driver bound')
    return encoded, timeout


def guest_path(tree, value):
    path = PurePosixPath(value)
    if not path.is_absolute() or '..' in path.parts or str(path) == '/':
        raise ValueError('fixture destination must be an absolute non-root guest path')
    destination = tree.joinpath(*path.parts[1:])
    # Earlier manifest symlinks must not redirect a later host write.
    for parent in (destination, *destination.parents):
        if parent == tree:
            break
        if parent.is_symlink():
            raise ValueError('fixture path traverses an existing symlink: ' + value)
    return destination


def logged(argv, log, timeout=300):
    with Path(log).open('wb') as output:
        try:
            result = subprocess.run(list(map(str, argv)), stdin=subprocess.DEVNULL,
                                    stdout=output, stderr=subprocess.STDOUT, timeout=timeout)
            return result.returncode
        except subprocess.TimeoutExpired:
            return 'timeout'


def sparse_copy(source, destination):
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', '--', str(source), str(destination)], check=True)


def build_fixture(manifest, destination, driver):
    tree = destination / 'fixture-tree'
    if tree.exists():
        tree.rename(destination / ('fixture-tree.incomplete-' + str(time.time_ns())))
    tree.mkdir()
    directories = ['/tmp', '/proc', '/sys', '/dev', '/dev/shm', '/dev/mqueue', '/lib', '/bin', '/usr/bin']
    directories += manifest.get('directories', [])
    for entry in directories:
        entry = {'path': entry} if isinstance(entry, str) else entry
        name = entry.get('path', entry.get('destination'))
        path = guest_path(tree, name)
        path.mkdir(parents=True, exist_ok=True)
        path.chmod(int(str(entry.get('mode', '1777' if name in {'/tmp', '/dev/shm'} else '755')), 8))
    identities = []
    entries = list(manifest.get('files', []))
    entries += [{'destination': link['destination'], 'symlink': link['target']} for link in manifest.get('symlinks', [])]
    for entry in entries:
        if entry['destination'] in {'/init', '/case'}:
            raise ValueError('reserved driver fixture path: ' + entry['destination'])
        path = guest_path(tree, entry['destination'])
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists():
            raise ValueError('duplicate fixture destination: ' + entry['destination'])
        if 'symlink' in entry:
            path.symlink_to(entry['symlink'])
            identities.append({'destination': entry['destination'], 'symlink': entry['symlink']})
            continue
        source = Path(entry['source'])
        actual = digest(source)
        if entry.get('sha256', actual) != actual:
            raise ValueError('fixture input hash changed: ' + str(source))
        shutil.copyfile(source, path)
        path.chmod(int(str(entry.get('mode', '755')), 8))
        identities.append({'source': str(source), 'destination': entry['destination'], 'sha256': actual})
    defaults = {'/etc/passwd': 'root:x:0:0:root:/root:/bin/sh\nnobody:x:65534:65534:nobody:/:/bin/false\n',
                '/etc/group': 'root:x:0:\nnobody:x:65534:\n',
                '/etc/hosts': '127.0.0.1 localhost\n::1 localhost\n'}
    for name, contents in defaults.items():
        target = guest_path(tree, name)
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            target.write_text(contents)
            target.chmod(0o644)
    (tree / 'root').mkdir(exist_ok=True)
    shutil.copyfile(driver, tree / 'init')
    (tree / 'init').chmod(0o755)
    # Leave room for ext4 metadata and case-created data, beyond all input ELF/DSOs.
    total = sum(path.stat().st_size for path in tree.rglob('*') if path.is_file() and not path.is_symlink())
    requested = int(manifest.get('fixture_bytes', 0))
    size = max(64 * 1024 * 1024, total * 2 + 32 * 1024 * 1024, requested)
    size = (size + 4095) // 4096 * 4096
    disk = destination / 'fixture.img'
    with disk.open('wb') as stream:
        stream.truncate(size)
    argv = ['mkfs.ext4', '-q', '-F', '-b', '4096', '-d', tree, disk]
    if logged(argv, destination / 'mkfs.log') != 0:
        raise RuntimeError('mkfs failed; see ' + str(destination / 'mkfs.log'))
    commands = destination / 'devices.debugfs'
    devices = [('console', 5, 1), ('tty', 5, 0), ('null', 1, 3), ('zero', 1, 5),
               ('random', 1, 8), ('urandom', 1, 9)]
    commands.write_text('cd /dev\n' + ''.join(
        f'mknod {name} c {major} {minor}\nset_inode_field {name} mode 020666\n'
        for name, major, minor in devices) + ''.join(f'stat {name}\n' for name, _, _ in devices))
    if logged(['debugfs', '-w', '-f', commands, disk], destination / 'devices.log') != 0:
        raise RuntimeError('device fixture creation failed')
    if (destination / 'devices.log').read_text().count('Type: character special') != len(devices):
        raise RuntimeError('debugfs did not create all requested character devices')
    return {'path': str(disk), 'sha256': digest(disk), 'bytes': size, 'files': identities}


def run_suite(manifest, output_dir, driver_elf, linux_kernel, boaros_kernel, *,
              case_ids=None, resume=True, default_timeout=10, boot_timeout=15,
              qemu='qemu-system-riscv64', output_validator=None):
    """Run all selected cases serially; persist each result before continuing.

    A driver or guest failure affects its case only. No result is called pass
    unless the Linux reference completed successfully with the expected exit.
    An interruption leaves remaining records explicitly not-run; repeat the
    same invocation to resume, or set case_ids to select independent cases.
    """
    manifest = json.loads(Path(manifest).read_text()) if not isinstance(manifest, dict) else manifest
    destination = Path(output_dir).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    driver = Path(driver_elf).resolve()
    kernels = {'linux': Path(linux_kernel).resolve(), 'boaros': Path(boaros_kernel).resolve()}
    cases = manifest['cases']
    if len({case['id'] for case in cases}) != len(cases):
        raise ValueError('duplicate case id')
    configurations = {case['id']: case_configuration(case, manifest, default_timeout) for case in cases}
    selected = set(configurations) if case_ids is None else set(case_ids)
    if not selected <= set(configurations):
        raise ValueError('unknown requested case id')
    identity = {'manifest': manifest, 'driver_sha256': digest(driver),
                'driver_source_sha256': digest(Path(__file__).with_name('suite_driver.c')),
                'runner_sha256': digest(__file__),
                'kernels': {name: {'path': str(path), 'sha256': digest(path)} for name, path in kernels.items()},
                'default_timeout': default_timeout, 'boot_timeout': boot_timeout,
                'qemu': str(qemu), 'memory': '512M', 'smp': 1}
    identity['tools'] = {}
    for tool, flag in ((qemu, '--version'), ('mkfs.ext4', '-V'), ('debugfs', '-V')):
        executable = shutil.which(str(tool))
        if executable is None:
            raise RuntimeError('missing execution tool: ' + str(tool))
        version = subprocess.check_output([executable, flag], stderr=subprocess.STDOUT,
                                          text=True).strip()
        identity['tools'][str(tool)] = {'path': executable, 'sha256': digest(executable),
                                      'version': version}
    # Validate source bytes on every resume, even when manifest omitted hashes.
    identity['inputs'] = {entry['source']: digest(entry['source']) for entry in manifest.get('files', []) if 'source' in entry}
    if output_validator is not None:
        validator_source = inspect.getsourcefile(output_validator)
        identity['output_validator'] = {
            'module': output_validator.__module__, 'name': output_validator.__qualname__,
            'source_sha256': digest(validator_source) if validator_source else None,
        }
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    path = destination / 'suite.json'
    if path.exists():
        state = json.loads(path.read_text())
        if state['identity_sha256'] != key:
            raise ValueError('suite inputs changed; use a new output directory to preserve earlier artifacts')
    else:
        state = {'identity_sha256': key, 'identity': identity, 'status': 'preparing',
                 'results': {case['id']: {'case': case, 'status': 'not-run'} for case in cases}}
    save_json(destination / 'manifest.json', manifest)
    save_json(path, state)
    try:
        if 'fixture' not in state:
            state['fixture'] = build_fixture(manifest, destination, driver)
            save_json(path, state)
        base = Path(state['fixture']['path'])
        if digest(base) != state['fixture']['sha256']:
            raise ValueError('base fixture changed since preparation')
        state['status'] = 'running'
        save_json(path, state)
        for case in cases:
            case_id = case['id']
            if case_id not in selected:
                continue
            previous = state['results'][case_id]
            if resume and previous.get('completed'):
                continue
            directory = destination / 'cases' / case_id
            directory.mkdir(parents=True, exist_ok=True)
            encoded, timeout = configurations[case_id]
            config = directory / 'case'
            config.write_bytes(encoded)
            save_json(directory / 'case.json', case)
            fixture = directory / 'fixture.img'
            sparse_copy(base, fixture)
            debugfs = directory / 'case.debugfs'
            # Paths are quoted for debugfs, not interpreted by a host shell.
            escaped = str(config).replace('\\', '\\\\').replace('"', '\\"')
            debugfs.write_text(f'write "{escaped}" /case\n')
            if logged(['debugfs', '-w', '-f', debugfs, fixture], directory / 'fixture.log') != 0:
                raise RuntimeError('could not install case configuration')
            result = {'case': case, 'status': 'running', 'completed': False,
                      'fixture_sha256': digest(fixture), 'fixture': str(fixture), 'guests': {}}
            state['results'][case_id] = result
            save_json(path, state)
            for name, kernel in kernels.items():
                disk = directory / (name + '.img')
                sparse_copy(fixture, disk)
                argv = [qemu, '-machine', 'virt', '-bios', 'default', '-kernel', kernel,
                        '-m', '512M', '-smp', '1', '-nographic', '-no-reboot',
                        '-drive', f'file={disk},if=none,format=raw,id=root',
                        '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
                if name == 'linux':
                    argv += ['-append', 'root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
                log = directory / (name + '.log')
                begun = time.monotonic()
                code = logged(argv, log, timeout + boot_timeout)
                raw = log.read_text(errors='replace').replace('\r\n', '\n')
                observed = parse_observation(raw, case_id)
                observed['guest_status'] = 'complete'
                if code == 'timeout':
                    observed['guest_status'] = 'timeout'
                elif code != 0:
                    observed['guest_status'] = 'guest-crash'
                elif name == 'boaros' and not re.search(
                    r'^BoarOS: PID 1 exited status=0x2a pages=0x[1-9a-f][0-9a-f]* heap-live=0x0; shutting down$', raw, re.M):
                    observed['guest_status'] = 'guest-incomplete'
                elif name == 'linux' and not reference_environment_ready(observed):
                    observed['guest_status'] = 'reference-environment-error'
                for channel in ('stdout', 'stderr'):
                    (directory / (name + '.' + channel)).write_bytes(bytes.fromhex(observed[channel + '_hex']))
                if case.get('output_contract'):
                    if output_validator is None:
                        observed['contract'] = {'status': 'contract-unchecked', 'errors': ['no output validator supplied']}
                    else:
                        observed['contract'] = output_validator(case, observed)
                        if not isinstance(observed['contract'], dict) or 'status' not in observed['contract']:
                            raise ValueError('output validator must return a dict with status')
                observed.update({'command': list(map(str, argv)), 'qemu_returncode': code,
                                 'log': str(log), 'disk': str(disk), 'elapsed_seconds': time.monotonic() - begun})
                result['guests'][name] = observed
                save_json(directory / 'result.json', result)
                save_json(path, state)
            result.update(compare_observations(result['guests']['linux'], result['guests']['boaros'], case.get('expected_exit', 0)))
            result['completed'] = True
            save_json(directory / 'result.json', result)
            save_json(path, state)
        state['status'] = 'complete' if all(row.get('completed') for row in state['results'].values()) else 'partial'
        state['counts'] = {status: sum(row['status'] == status for row in state['results'].values())
                           for status in sorted({row['status'] for row in state['results'].values()})}
        return state
    except BaseException as error:
        state['status'] = 'interrupted' if isinstance(error, (KeyboardInterrupt, SystemExit)) else 'runner-error'
        state['error'] = str(error)
        for row in state['results'].values():
            if row['status'] == 'running':
                row['status'] = state['status']
        raise
    finally:
        state['counts'] = {status: sum(row['status'] == status for row in state['results'].values())
                           for status in sorted({row['status'] for row in state['results'].values()})}
        save_json(path, state)
