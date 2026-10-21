#!/usr/bin/env python3
"""Run the predeclared B3 short-window campaign under a restored CPU lease."""
import argparse
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import time

from cost_fixture import FrozenFixture, writable_root_path
from cost_match import (PROTOCOL, TARGETS, POSITIONS, build_schedule, canonical,
                        digest, loaded_segments, seal_manifest, validate_boot,
                        validate_manifest)
from cost_match_report import analyse_blocks
from cost_measurement import acquire_measurement_lock

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CACHE = ROOT / 'build/data-path-baselines/sync-b3'
PRODUCTION = ('arch', 'drivers', 'fs', 'include', 'kernel', 'lib', 'mm', 'net', 'third_party')


def save(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(canonical(value))
    temporary.replace(path)


def file_identity(path):
    path = Path(path)
    return dict(path=str(path.absolute()), target=str(path.resolve(strict=True)),
                sha256=digest(path), bytes=path.stat().st_size,
                mode=path.stat().st_mode & 0o777)


def host_state():
    state = {'policies': {}, 'unavailable': {}}
    for cpu in (2, 3):
        values = {}
        for field in ('scaling_driver', 'scaling_governor', 'scaling_min_freq',
                      'scaling_max_freq', 'energy_performance_preference'):
            try:
                values[field] = (Path(f'/sys/devices/system/cpu/cpu{cpu}/cpufreq') / field).read_text().strip()
            except OSError as error:
                state['unavailable'][f'cpu{cpu}/{field}'] = str(error)
        state['policies'][str(cpu)] = values
    for line in Path('/proc/self/status').read_text().splitlines():
        if line.startswith(('NoNewPrivs:', 'CapEff:')):
            name, value = line.split(':', 1)
            state[name] = value.strip()
    return state


class PolicyClient:
    """The root helper only owns sysfs; workload and artifact I/O stay unprivileged."""
    def __init__(self, output):
        self.output = output
        self.process = None
        self.events = []
        self.ready = False
        self.pending = b''

    def _receive(self):
        deadline = time.monotonic() + 10
        while b'\n' not in self.pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.process.stdout], [], [], remaining)[0]:
                raise RuntimeError('policy helper response timed out')
            data = os.read(self.process.stdout.fileno(), 65536)
            if not data:
                raise RuntimeError('policy helper exited before acknowledgement')
            self.pending += data
        line, self.pending = self.pending.split(b'\n', 1)
        event = json.loads(line)
        self.events.append(event)
        save(self.output / 'policy-events.json', self.events)
        return event

    def acquire(self, measurement_lease):
        if os.geteuid() == 0:
            raise RuntimeError('run the campaign as its unprivileged owner; only the policy helper uses sudo')
        command = [sys.executable, '-B', str(ROOT / 'tests/cost_policy_guard.py'),
                   '--measurement-lock-fd', str(measurement_lease.fileno())]
        self.error_stream = (self.output / 'policy-helper.stderr').open('w')
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.error_stream, bufsize=0,
                                        pass_fds=(measurement_lease.fileno(),))
        try:
            event = self._receive()
        except RuntimeError as error:
            self.error_stream.flush()
            detail = (self.output / 'policy-helper.stderr').read_text().strip()
            raise RuntimeError(str(error) + (': ' + detail if detail else '')) from error
        if event.get('event') != 'ready':
            self.error_stream.flush()
            detail = (self.output / 'policy-helper.stderr').read_text().strip()
            raise RuntimeError('policy lease refused: ' + event.get('error', str(event)) +
                               (': ' + detail if detail else ''))
        self.ready = True
        return event

    def check(self):
        if not self.ready:
            raise RuntimeError('policy lease not acquired')
        self.process.stdin.write(b'{"command":"check"}\n')
        self.process.stdin.flush()
        event = self._receive()
        if event.get('event') != 'checked':
            raise RuntimeError('policy check failed: ' + event.get('error', 'invalid acknowledgement'))
        return event['snapshot']

    def close(self):
        if self.process is None:
            return {'event': 'not-acquired', 'errors': []}
        result = None
        try:
            if self.process.poll() is None and self.ready:
                self.process.stdin.write(b'{"command":"restore"}\n')
                self.process.stdin.flush()
                result = self._receive()
                if result.get('event') != 'restored':
                    raise RuntimeError('CPU policy restoration acknowledgement missing')
            if self.process.stdin and not self.process.stdin.closed:
                self.process.stdin.close()  # EOF is also a restoration request.
            self.process.wait(timeout=10)
            # Failed acquisition may have emitted blocked+restored together.
            # Keep the actual restoration, including errors, rather than
            # treating a partially configured lease as never acquired.
            while b'\n' in self.pending or select.select([self.process.stdout], [], [], 0)[0]:
                try:
                    event = self._receive()
                except RuntimeError:
                    break
                if event.get('event') == 'restored':
                    result = event
                elif event.get('restoration'):
                    result = event['restoration']
            if self.ready and self.process.returncode and not (result and result.get('errors')):
                raise RuntimeError('policy helper failed while restoring')
            return result or {'event': 'not-acquired', 'errors': []}
        except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
            if self.process.stdin and not self.process.stdin.closed:
                self.process.stdin.close()
            if self.process.poll() is None:
                self.process.send_signal(signal.SIGTERM)
                try:
                    self.process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    pass  # Do not SIGKILL a helper still responsible for restoration.
            return {'event': 'restore-unverified', 'errors': [str(error)]}
        finally:
            self.ready = False
            self.process.stdout.close()
            self.error_stream.close()


def prepare(args):
    inputs = {}
    for role, kernel, identity_path in (
            ('baseline', args.cache / 'baseline-0.elf', args.cache / 'baseline-0.identity.json'),
            ('current', args.current_kernel, args.current_identity)):
        identity = json.loads(identity_path.read_text())
        info = file_identity(kernel)
        if info['sha256'] != identity['kernel_sha256'] or identity.get('cost_diagnostics') != 0:
            raise ValueError('frozen OFF kernel identity differs: ' + role)
        inputs[role] = dict(kernel=info, identity=file_identity(identity_path), build=identity)
    if inputs['baseline']['build']['source_commit'] != 'cbcab907a056dda5cc745ae9e940e81a283ea03c':
        raise ValueError('original B3 baseline is required')
    current = inputs['current']['build']
    diff = subprocess.check_output(['git', 'diff', current['source_commit'], '--', *PRODUCTION], cwd=ROOT)
    import hashlib
    expected_diff = current.get('source_diff_sha256', hashlib.sha256(b'').hexdigest())
    if hashlib.sha256(diff).hexdigest() != expected_diff:
        raise ValueError('current production source does not match frozen kernel provenance')
    if loaded_segments(ROOT / 'kernel-rv') != loaded_segments(args.current_kernel):
        raise ValueError('current kernel-rv load content/geometry differs from frozen current kernel')
    inputs['current']['load_segments'] = loaded_segments(args.current_kernel)
    inputs['qemu'] = file_identity(args.cache / 'qemu-system-riscv64')
    inputs['firmware'] = file_identity(args.cache / 'firmware.bin')
    for role in ('baseline', 'current'):
        tools = inputs[role]['build']['tools']
        if inputs['qemu']['sha256'] != tools['qemu-system-riscv64']['sha256']:
            raise ValueError('QEMU executable differs from frozen kernel tool profile')
        for name, identity in tools.items():
            if digest(identity['path']) != identity['sha256']:
                raise ValueError('frozen tool differs: ' + name)
    inputs['cases'] = {}
    for case in args.cases:
        elf = file_identity(args.cache / (case + '.elf'))
        fixture = FrozenFixture(args.cache / 'fixture-cache', case, elf['sha256'])
        image = fixture.load()
        if image is None:
            raise ValueError('pre-existing frozen fixture required')
        image_info = file_identity(image)
        if image_info['mode'] & 0o222:
            raise ValueError('frozen fixture is writable')
        inputs['cases'][case] = dict(elf=elf, fixture=image_info,
                                    fixture_identity=file_identity(image.parent / 'input.json'))
    scripts = ('cost-match-riscv.py', 'cost-riscv.py', 'cost_match.py', 'cost_match_report.py',
               'cost_policy.py', 'cost_measurement.py', 'cost_report.py', 'cost_fixture.py',
               'cost_seed.py', 'cost_consumer.py', 'cost_policy_guard.py', 'qemu_thread_affinity.py')
    inputs['scripts'] = {name: file_identity(ROOT / 'tests' / name) for name in scripts}
    paths = sorted(subprocess.check_output(['git', 'ls-files', '-co', '--exclude-standard'], cwd=ROOT,
                                          text=True).splitlines())
    runner_source_sha256 = hashlib.sha256(b''.join(
        path.encode() + b'\0' + digest(ROOT / path).encode() + b'\n'
        for path in paths if '__pycache__' not in Path(path).parts)).hexdigest()
    return seal_manifest(dict(protocol=PROTOCOL, cases=list(args.cases),
                              schedule=build_schedule(args.cases), inputs=inputs,
                              runner_source_sha256=runner_source_sha256,
                              parameters=dict(bootstrap_draws=10000, bootstrap_seed=0xB30020261011,
                                              cost_limit_percent=3, drift_limit_percent=2,
                                              calibration_tolerance_percent=1, timeout_seconds=180,
                                              cpu=2, siblings=[2, 3], requested_khz=2500000,
                                              qemu_clock='default TCG; no icount'),
                              host_before=host_state()))


def expected_input(manifest, entry):
    frozen = manifest['inputs']
    kernel = frozen[entry['kernel_role']]
    case = frozen['cases'][entry['case']]
    return dict(case=entry['case'], seed=entry['seed'],
                runner_source_sha256=manifest['runner_source_sha256'],
                kernel_sha256=kernel['kernel']['sha256'],
                kernel_source_commit=kernel['build']['source_commit'],
                kernel_source_diff_sha256=kernel['build'].get('source_diff_sha256'),
                firmware_sha256=frozen['firmware']['sha256'],
                qemu_sha256=frozen['qemu']['sha256'], qemu_path=frozen['qemu']['path'],
                elf_sha256=case['elf']['sha256'], fixture_sha256=case['fixture']['sha256'])


def stop_boot(process):
    # The group, not its already-reaped leader, owns any remaining QEMU.
    for signum in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(process.pid, signum)
        except ProcessLookupError:
            pass
        if signum == signal.SIGTERM:
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                pass
    process.wait()


def run_boot(manifest, entry, output, lease, policy):
    frozen = manifest['inputs']
    identity = frozen[entry['kernel_role']]
    case = entry['case']
    name = f"{entry['stage']}-{case}-{entry['block']:02d}-{entry['position']}"
    directory = output / 'boots' / name
    directory.parent.mkdir(exist_ok=True)
    before = policy.check()
    command = [sys.executable, '-B', str(ROOT / 'tests/cost-riscv.py'),
               '--case', case, '--replicas', '1', '--off', '--transport', 'modern',
               '--cache', 'writeback', '--qemu-seed', str(entry['seed']),
               '--fixture-cache', str(Path(frozen['cases'][case]['fixture']['path']).parent.parent),
               '--kernel', identity['kernel']['path'], '--kernel-identity', identity['identity']['path'],
               '--coordinator-elf', frozen['cases'][case]['elf']['path'],
               '--qemu', frozen['qemu']['path'], '--bios', frozen['firmware']['path'],
               '--measurement-lock-fd', str(lease.fileno()), '--output', str(directory)]
    item = dict(entry, directory=str(directory), command=command,
                policy_before=before, started_ns=time.monotonic_ns(), failure_stage='runner')
    metadata = output / 'boot-metadata' / (name + '.json')
    metadata.parent.mkdir(exist_ok=True)
    save(metadata, item)
    process = None
    try:
        with (output / (name + '.runner.log')).open('w') as stream:
            # Bind only the runner and its descendants; the supervisor and policy
            # helper stay off the measured physical core.
            def bind():
                os.sched_setaffinity(0, {2})
            process = subprocess.Popen(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT,
                                       pass_fds=(lease.fileno(),), start_new_session=True, preexec_fn=bind)
            process.wait(timeout=240)  # Guest budget remains the runner's 180 seconds.
        item['returncode'] = process.returncode
        item['policy_after'] = policy.check()
        if process.returncode:
            raise RuntimeError('single-boot runner failed')
        item['failure_stage'] = 'result'
        rows = json.loads((directory / 'results.json').read_text())
        if not isinstance(rows, list) or len(rows) != 1:
            raise ValueError('exactly one independent boot required')
        record = rows[0]
        boot = directory / (case + '-0')
        expected = expected_input(manifest, entry)
        item['failure_stage'] = 'guest-contract'
        item['owners'] = validate_boot(record, (boot / 'input.json').read_text(),
                                      (boot / 'boot.log').read_text(), expected,
                                      process.returncode, directory)
        for path, sha in ((directory / 'kernel', expected['kernel_sha256']),
                          (directory / 'firmware', expected['firmware_sha256']),
                          (directory / (case + '.elf'), expected['elf_sha256']),
                          (boot / 'boot.dtb', record['dtb_sha256'])):
            if digest(path) != sha:
                raise ValueError('copied execution input differs: ' + str(path))
        item['files'] = {name: file_identity(boot / name)
                         for name in ('input.json', 'boot.log', 'result.json')}
        if json.loads((boot / 'result.json').read_text()) != record:
            raise ValueError('single-boot results disagree')
        item['timings_ns'] = record['timings_ns']
        item['dtb_sha256'] = record['dtb_sha256']
        item['writable_root'] = writable_root_path(record)
        item['status'] = 'completed'
        del item['failure_stage']
    except Exception as error:
        item.update(status='timeout' if isinstance(error, subprocess.TimeoutExpired) else 'boot_failed',
                    error=str(error), error_type=type(error).__name__)
    finally:
        if process is not None:
            stop_boot(process)
        item['finished_ns'] = time.monotonic_ns()
        if process is not None:
            item['returncode'] = process.returncode
        save(metadata, item)
    return item


def stage_report(manifest, outcomes, stage, case, calibration=None):
    rows = [row for row in outcomes if row['stage'] == stage and row['case'] == case]
    expected_rows = [row for row in manifest['schedule'] if row['stage'] == stage and row['case'] == case]
    if len(rows) != 36 or any(row.get('status') != 'completed' for row in rows):
        return dict(classification='unresolved', reasons=['incomplete-or-failed-boots'], windows={})
    for row, expected in zip(rows, expected_rows):
        if any(row.get(key) != value for key, value in expected.items()):
            raise ValueError('outcome differs from predeclared schedule')
    if len({row['writable_root'] for row in rows}) != 36:
        raise ValueError('writable fixture reused')
    names = set(rows[0]['timings_ns'])
    if any(set(row['timings_ns']) != names for row in rows):
        raise ValueError('workload coverage changed between independent boots')
    for block in range(12):
        triplet = [row for row in rows if row['block'] == block]
        if (len(triplet) != 3 or [row['position'] for row in triplet] != list(POSITIONS) or
                len({row['dtb_sha256'] for row in triplet}) != 1):
            raise ValueError('pairing or DTB identity differs')
    for role in {row['kernel_role'] for row in rows}:
        pages = {row['owners']['pages'] for row in rows if row['kernel_role'] == role}
        if len(pages) != 1:
            raise ValueError('kernel resource baseline differs between boots')
    windows = {}
    for name in sorted(names):
        blocks = [{position + '_ns': next(row['timings_ns'][name] for row in rows
                                         if row['block'] == block and row['position'] == position)
                   for position in POSITIONS} for block in range(12)]
        calibrated = (calibration or {}).get('windows', {}).get(name, {}).get('classification') == 'pass'
        windows[name] = analyse_blocks(blocks, mode=stage, calibration_pass=calibrated)
    target = windows[TARGETS[case]]
    return dict(classification=target['classification'], reasons=target['reasons'],
                target=TARGETS[case], windows=windows,
                raw_boots=[{key: row[key] for key in ('block', 'position', 'seed', 'timings_ns', 'owners')}
                           for row in rows])


def aggregate_verdict(reports, cases):
    """Only the two approved targets acquire the new confidence-bound gate.

    Keep each secondary interval honest, but protect the previous point/2%
    drift contract rather than silently requiring 95% bounds for every window.
    """
    target_pass = all(reports[c]['comparison']['classification'] == 'pass' for c in cases)
    checks = {}
    for case in cases:
        for name, report in reports[case]['comparison']['windows'].items():
            if name == TARGETS[case]:
                continue
            verdict = ('unresolved' if not report['drift_pass'] else
                       'exceeds-limit' if report['estimate_percent'] > 3 else 'within-limit')
            checks[name] = dict(criterion='original-point-estimate-and-drift',
                                estimate_percent=report['estimate_percent'],
                                drift_percent=report['drift_percent'], verdict=verdict,
                                confidence_classification=report['classification'])
    failures = [name for name, check in checks.items() if check['verdict'] != 'within-limit']
    scope = 'both' if set(cases) == set(TARGETS) else 'partial'
    success = target_pass and not failures
    return dict(status=('passed' if scope == 'both' else 'partial_passed') if success else 'unresolved',
                scope=scope, other_window_failures=failures, other_window_checks=checks)


def campaign(manifest, output, lease, policy):
    outcomes, reports = [], {}
    for entry in manifest['schedule']:
        case, stage = entry['case'], entry['stage']
        if stage == 'comparison' and reports[case]['calibration']['classification'] != 'pass':
            outcomes.append(dict(entry, status='skipped_calibration'))
        else:
            print('MEASURE', stage, case, entry['block'], entry['position'], flush=True)
            outcomes.append(run_boot(manifest, entry, output, lease, policy))
        save(output / 'outcomes.json', outcomes)
        if entry['block'] == 11 and entry['position'] == 'after':
            reports.setdefault(case, {})[stage] = stage_report(
                manifest, outcomes, stage, case,
                reports.get(case, {}).get('calibration'))
            save(output / 'reports.json', reports)
    return outcomes, reports


def verify_campaign(output):
    """Recompute gates from original sealed inputs/logs, not reported scores."""
    manifest = json.loads((output / 'manifest.json').read_text())
    result = json.loads((output / 'result.json').read_text())
    validate_manifest(manifest)
    if result.get('protocol') != PROTOCOL or result.get('manifest_sha256') != manifest['manifest_sha256']:
        raise ValueError('report/manifest identity differs')
    if result.get('status') in ('passed', 'partial_passed') and result.get('boots_executed', 0) == 0:
        raise ValueError('no boots cannot prove a performance gate')
    if result.get('restoration', {}).get('errors') or result.get('status') == 'restoration_failed':
        raise ValueError('host restoration did not complete')
    if result.get('host_after', {}).get('policies') != manifest['host_before']['policies']:
        raise ValueError('saved policy values were not restored')
    inputs = manifest['inputs']
    for name, identity in inputs['scripts'].items():
        if digest(output / 'sources' / name) != identity['sha256']:
            raise ValueError('sealed measurement source differs: ' + name)
    if result['status'] == 'environment_blocked':
        if result['boots_executed'] or list((output / 'boot-metadata').glob('*.json')):
            raise ValueError('blocked environment contains unclassified boots')
        return {'status': 'environment_blocked', 'boots_verified': 0,
                'performance_verified': False}
    outcomes = json.loads((output / 'outcomes.json').read_text())
    if len(outcomes) != len(manifest['schedule']):
        raise ValueError('campaign schedule not completed; no full performance verdict')
    roots = set()
    ready = result.get('policy_acquisition', {})
    if ready.get('event') != 'ready':
        raise ValueError('measurement policy was not acquired')
    for entry, row in zip(manifest['schedule'], outcomes):
        if any(row.get(k) != v for k, v in entry.items()):
            raise ValueError('campaign schedule changed')
        if row['status'] == 'skipped_calibration':
            if row['stage'] != 'comparison':
                raise ValueError('calibration boot was skipped')
            continue
        name = f"{entry['stage']}-{entry['case']}-{entry['block']:02d}-{entry['position']}"
        if json.loads((output / 'boot-metadata' / (name + '.json')).read_text()) != row:
            raise ValueError('boot termination metadata differs')
        if row['status'] != 'completed':
            continue  # Failure remains in its exact schedule slot, never sampled.
        for snapshot_name in ('policy_before', 'policy_after'):
            policies = row[snapshot_name]['policies']
            if set(policies) != set(ready['requested']):
                raise ValueError('policy membership differs')
            for name, fields in ready['requested'].items():
                if (any(policies[name]['fields'].get(k) != v for k, v in fields.items()) or
                        policies[name]['fields'] != ready['snapshot']['policies'][name]['fields'] or
                        policies[name]['cpus'] != ready['snapshot']['policies'][name]['cpus']):
                    raise ValueError('policy drift during a boot')
        directory = Path(row['directory'])
        expected_directory = output / 'boots' / f"{entry['stage']}-{entry['case']}-{entry['block']:02d}-{entry['position']}"
        if directory != expected_directory:
            raise ValueError('boot output directory differs')
        boot = directory / (entry['case'] + '-0')
        for name, identity in row['files'].items():
            if name not in ('input.json', 'boot.log', 'result.json') or digest(boot / name) != identity['sha256']:
                raise ValueError('original boot evidence changed')
        if set(row['files']) != {'input.json', 'boot.log', 'result.json'}:
            raise ValueError('original boot evidence missing')
        record = json.loads((boot / 'result.json').read_text())
        expected = expected_input(manifest, entry)
        owners = validate_boot(record, (boot / 'input.json').read_text(),
                               (boot / 'boot.log').read_text(), expected, row['returncode'], directory)
        if owners != row['owners'] or record['timings_ns'] != row['timings_ns']:
            raise ValueError('reported time or owner evidence differs')
        for path, sha in ((directory / 'kernel', expected['kernel_sha256']),
                          (directory / 'firmware', expected['firmware_sha256']),
                          (directory / (entry['case'] + '.elf'), expected['elf_sha256']),
                          (boot / 'boot.dtb', record['dtb_sha256'])):
            if digest(path) != sha:
                raise ValueError('executed boot file differs')
        root = writable_root_path(record)
        if root in roots:
            raise ValueError('writable disk owner reused')
        roots.add(root)
    reports = {}
    for case in manifest['cases']:
        calibration = stage_report(manifest, outcomes, 'calibration', case)
        comparison = stage_report(manifest, outcomes, 'comparison', case,
                                  calibration)
        skipped = [row for row in outcomes if row['case'] == case and row['status'] == 'skipped_calibration']
        if (calibration['classification'] == 'pass' and skipped or
                calibration['classification'] != 'pass' and
                any(row['case'] == case and row['stage'] == 'comparison' and row['status'] != 'skipped_calibration'
                    for row in outcomes)):
            raise ValueError('comparison did not obey calibration decision')
        reports[case] = dict(calibration=calibration, comparison=comparison)
    if reports != result.get('reports'):
        raise ValueError('reported intervals/verdict differ from original boot data')
    aggregate = aggregate_verdict(reports, manifest['cases'])
    if any(result.get(key) != value for key, value in aggregate.items()):
        raise ValueError('aggregate scope/verdict differs')
    return {'status': aggregate['status'], 'boots_verified': len(roots),
            'performance_verified': aggregate['status'] == 'passed'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=DEFAULT_CACHE)
    parser.add_argument('--current-kernel', type=Path)
    parser.add_argument('--current-identity', type=Path)
    parser.add_argument('--cases', nargs='+', choices=tuple(TARGETS), default=list(TARGETS))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--verify', type=Path, help='recheck the original sealed campaign without running guests')
    args = parser.parse_args()
    if args.verify:
        print(canonical(verify_campaign(args.verify.resolve())), end='')
        return 0
    if not args.output:
        parser.error('--output is required for a new campaign')
    args.cache = args.cache.resolve()
    args.current_kernel = (args.current_kernel or args.cache / 'zero-0.elf').resolve()
    args.current_identity = (args.current_identity or args.cache / 'zero-0.identity.json').resolve()
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error('output must be new or empty; this protocol does not resume or mix old logs')
    output.mkdir(parents=True, exist_ok=True)
    result = dict(protocol=PROTOCOL, status='preparation_failed', boots_executed=0)
    policy = PolicyClient(output)
    measurement_lease = None
    previous = {}
    affinity = os.sched_getaffinity(0)
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'campaign interrupted by signal {signum}')
    for signum in (signal.SIGINT, signal.SIGTERM):
        previous[signum] = signal.signal(signum, interrupted)
    try:
        manifest = prepare(args)
        validate_manifest(manifest)
        save(output / 'manifest.json', manifest)
        sources = output / 'sources'
        sources.mkdir()
        for name, identity in manifest['inputs']['scripts'].items():
            (sources / name).write_bytes(Path(identity['path']).read_bytes())
        outside = affinity - {2, 3}
        if outside:
            os.sched_setaffinity(0, outside)
        (ROOT / 'build/cost').mkdir(parents=True, exist_ok=True)
        measurement_lease = acquire_measurement_lock(ROOT / 'build/cost/measurement.lock')
        try:
            result['policy_acquisition'] = policy.acquire(measurement_lease)
        except (OSError, ValueError, RuntimeError) as error:
            result.update(status='environment_blocked', error=str(error))
        else:
            result['status'] = 'measurement_incomplete'
            outcomes, reports = campaign(manifest, output, measurement_lease, policy)
            result['boots_executed'] = sum(row['status'] != 'skipped_calibration' for row in outcomes)
            result['reports'] = reports
            result.update(aggregate_verdict(reports, args.cases))
    except (Exception, KeyboardInterrupt) as error:
        result['error'] = str(error)
        if isinstance(error, KeyboardInterrupt):
            result['status'] = 'interrupted'
    finally:
        # Do not let a second interruption abandon the sysfs owner.
        for signum in previous:
            signal.signal(signum, signal.SIG_IGN)
        result['restoration'] = policy.close()
        os.sched_setaffinity(0, affinity)
        result['host_after'] = host_state()
        result['boots_executed'] = len(list((output / 'boot-metadata').glob('*.json')))
        if 'manifest' in locals():
            result['manifest_sha256'] = manifest['manifest_sha256']
        if result['restoration']['errors']:
            result['status'] = 'restoration_failed'
        elif 'manifest' in locals() and result['host_after']['policies'] != manifest['host_before']['policies']:
            result['status'] = 'restoration_failed'
            result['error'] = 'policy values do not match saved originals'
        save(output / 'result.json', result)
        if measurement_lease is not None:
            measurement_lease.close()
        for signum, handler in previous.items():
            signal.signal(signum, handler)
    print(canonical(result), end='', flush=True)
    return 0 if result['status'] == 'passed' else 2


if __name__ == '__main__':
    sys.exit(main())
