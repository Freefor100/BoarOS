#!/usr/bin/env python3
"""Prepare/run one twelve-attempt attribution batch; no performance acceptance."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys

from cost_diagnostic import (PROFILE, EVENT_NAMES, EVENT_SYMBOLS, WINDOW_NAMES,
                             check_manifest, execution_config, parse_trace,
                             reserve_attempt, schedule, sealed, validate_receipt, validate_policy_snapshot)
from cost_fixture import FrozenFixture
from cost_match import canonical, digest, loaded_segments, validate_boot
from cost_measurement import acquire_measurement_lock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('matched_driver', ROOT / 'tests/cost-match-riscv.py')
matched = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(matched)
CACHE = ROOT / 'build/data-path-baselines/sync-b3'
FIXED_SHA = {
    'kernel': 'be7c4c59e657c0310e711ffc431c228883f75430af7b7b92230b1acb7833f8b6',
    'qemu': '5713c546693820d225e87795a14e8177343cb7ac4d9b0610a9e41c7203eb023d',
    'firmware': '894e2aef99590fc07ec6c60ab00282b8bc5d5d5bb2a1d0c6ada0c52df24274c0',
    'latency': '5f4cdf2df9a5d8d9948531d03b06db329eb8f1981eca12a0af4ae56a17d12050',
    'locking': '72fc0ad7f4ca8984231ef71bb2b2f4aadaa7f48e203bf0d4ca4d877aad731300',
}
COMMIT = '9791fed4a33ca32cb722904491abd1526f7d2719'
FIXTURE_SHA = dict(latency='e6991820b02c37c0126608be42494afed37e9e095a35193994dd61180a05621e',
                   locking='590317d2c52fb97c45d46e7501a7a5a637ef9335306abe1096d28bafaa598939')


def source_digest():
    paths = sorted(subprocess.check_output(['git', 'ls-files', '-co', '--exclude-standard'],
                                          cwd=ROOT, text=True).splitlines())
    return hashlib.sha256(b''.join(path.encode() + b'\0' + digest(ROOT/path).encode() + b'\n'
        for path in paths if '__pycache__' not in Path(path).parts)).hexdigest()


def symbols(path):
    result = {}
    for row in subprocess.check_output(['riscv64-linux-gnu-nm', '-S', '-n', str(path)], text=True).splitlines():
        fields = row.split()
        if len(fields) == 4:
            pc, size, kind, name = fields
            if kind.lower() in ('t', 'w'):
                if name in result:
                    result[name] = None  # Selected ambiguous symbols must not silently pick one.
                else:
                    result[name] = dict(pc=int(pc, 16), bytes=int(size, 16))
    return result


def verify_provenance(inputs):
    build = inputs['build']
    worktree = Path(build['source_worktree'])
    if build['source_commit'] != COMMIT or build.get('cost_diagnostics') != 0:
        raise ValueError('corrected frozen FS-Off candidate required')
    diff = subprocess.check_output(['git', 'diff', COMMIT, '--', *matched.PRODUCTION], cwd=worktree)
    if hashlib.sha256(diff).hexdigest() != build['source_diff_sha256']:
        raise ValueError('candidate production source differs')
    if loaded_segments(worktree / 'kernel-rv') != loaded_segments(Path(inputs['kernel']['path'])):
        raise ValueError('candidate source build and frozen kernel load segments differ')
    if [list(row) for row in loaded_segments(Path(inputs['kernel']['path']))] != build['load_segments']:
        raise ValueError('candidate load geometry differs from build identity')
    for name, item in build['tools'].items():
        if digest(Path(item['path'])) != item['sha256']:
            raise ValueError('frozen tool changed: ' + name)


def prepare(output):
    if output.exists():
        raise ValueError('new output directory required; no resume or replacement batch')
    inputs = {name: matched.file_identity(CACHE / file) for name, file in
              (('kernel', 'fs-off-1.elf'), ('identity', 'fs-off-1.identity.json'),
               ('qemu', 'qemu-system-riscv64'), ('firmware', 'firmware.bin'))}
    for name in ('kernel', 'qemu', 'firmware'):
        if inputs[name]['sha256'] != FIXED_SHA[name]:
            raise ValueError('approved frozen input differs: ' + name)
    inputs['build'] = json.loads(Path(inputs['identity']['path']).read_text())
    if inputs['build']['kernel_sha256'] != FIXED_SHA['kernel']:
        raise ValueError('kernel build SHA differs')
    verify_provenance(inputs)
    kernel_symbols = symbols(Path(inputs['kernel']['path']))
    inputs['events'] = {name: dict(symbol=symbol, **kernel_symbols[symbol])
                        for name, symbol in zip(EVENT_NAMES, EVENT_SYMBOLS)}
    inputs['cases'] = {}
    for case in WINDOW_NAMES:
        elf = matched.file_identity(CACHE / f'{case}.elf')
        if elf['sha256'] != FIXED_SHA[case]:
            raise ValueError('original workload ELF differs')
        fixture = FrozenFixture(CACHE / 'fixture-cache', case, elf['sha256']).load()
        if fixture is None or fixture.stat().st_mode & 0o222:
            raise ValueError('read-only frozen fixture required')
        if digest(fixture) != FIXTURE_SHA[case]:
            raise ValueError('approved preboot fixture differs')
        clock = symbols(Path(elf['path']))['clock_gettime']
        inputs['cases'][case] = dict(elf=elf, fixture=matched.file_identity(fixture),
                                    clock_pc=clock['pc'])
    inputs['wrapper'] = matched.file_identity(ROOT / 'tests/cost-diagnostic-qemu.py')
    inputs['compiler'] = matched.file_identity(Path(shutil.which('cc')))
    inputs['plugin_header'] = matched.file_identity(Path('/usr/include/qemu-plugin.h'))
    output.mkdir(parents=True)
    (output / 'assets').mkdir()
    (output / 'attempts').mkdir()
    (output / 'sources').mkdir()
    observer = output / 'assets/observer.so'
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True).split()
    command = [inputs['compiler']['path'], '-std=c11', '-O2', '-shared', '-fPIC', '-Wall',
               '-Wextra', '-Werror', str(ROOT / 'tests/cost/diagnostic_plugin.c'), *flags, '-o', str(observer)]
    subprocess.run(command, check=True)
    observer.chmod(0o444)
    inputs['observer'] = matched.file_identity(observer)
    scripts = ('cost-diagnostic-riscv.py', 'cost-diagnostic-qemu.py', 'cost_diagnostic.py',
               'cost-riscv.py', 'cost-match-riscv.py', 'cost_match.py', 'cost_match_report.py',
               'cost_policy.py', 'cost_policy_guard.py', 'cost_measurement.py', 'cost_report.py',
               'cost_fixture.py', 'cost_seed.py', 'cost_consumer.py', 'cost/diagnostic_plugin.c')
    inputs['scripts'] = {name: matched.file_identity(ROOT / 'tests' / name) for name in scripts}
    for name in scripts:
        target = output / 'sources' / name
        target.parent.mkdir(exist_ok=True)
        shutil.copyfile(ROOT / 'tests' / name, target)
        target.chmod(0o444)
    manifest = sealed(dict(profile=PROFILE, acceptance=False, schedule=schedule(), inputs=inputs,
        runner_source_sha256=source_digest(), host_before=matched.host_state(),
        observer_build=dict(command=command,
            compiler_version=subprocess.check_output(['cc', '--version'], text=True).splitlines()[0],
            glib_version=subprocess.check_output(['pkg-config', '--modversion', 'glib-2.0'], text=True).strip()),
        parameters=dict(cpu=2, siblings=[2, 3], requested_khz=2500000, timeout_seconds=180,
            outer_timeout_seconds=240, ram_mib=512, vcpus=1, transport='modern', cache='writeback',
            qemu_clock='default TCG; no icount', attempts=12, cost_limit_percent=3),
        limitations=['instrumented attribution only; not an OFF performance score',
            'clock function-entry boundaries differ from guest clock samples',
            'sequential wall/CPU sampling has a recorded bracket uncertainty',
            'wall minus thread CPU includes WFI, I/O and host scheduling',
            'context switch counts do not measure descheduled duration',
            'TB instruction proxy is not retired instructions; FS is a partial translation key',
            'function entry is not completion; shared helper execution owner can be unknown']))
    matched.save(output / 'manifest.json', manifest)
    return manifest


def verify_inputs(manifest):
    check_manifest(manifest)
    if source_digest() != manifest['runner_source_sha256']:
        raise ValueError('runner source changed since preparation')
    inputs = manifest['inputs']
    for name in ('kernel', 'qemu', 'firmware'):
        if inputs[name]['sha256'] != FIXED_SHA[name]:
            raise ValueError('approved frozen input differs: ' + name)
    items = [inputs[name] for name in ('kernel', 'identity', 'qemu', 'firmware', 'wrapper',
                                     'observer', 'compiler', 'plugin_header')]
    items += list(inputs['scripts'].values())
    for case in inputs['cases'].values():
        items += [case['elf'], case['fixture']]
    for name, case in inputs['cases'].items():
        if case['elf']['sha256'] != FIXED_SHA[name] or case['fixture']['sha256'] != FIXTURE_SHA[name]:
            raise ValueError('original ELF/fixture changed')
    for item in items:
        if matched.file_identity(Path(item['path'])) != item:
            raise ValueError('sealed input identity/mode/target differs: ' + item['path'])
    verify_provenance(inputs)
    kernel_symbols = symbols(Path(inputs['kernel']['path']))
    for name, event in inputs['events'].items():
        if kernel_symbols[event['symbol']] != {k: event[k] for k in ('pc', 'bytes')}:
            raise ValueError('selected event PC differs: ' + name)
    for case in inputs['cases'].values():
        if symbols(Path(case['elf']['path']))['clock_gettime']['pc'] != case['clock_pc']:
            raise ValueError('original ELF clock PC differs')


def guest_expected(manifest, entry):
    inputs, case = manifest['inputs'], manifest['inputs']['cases'][entry['case']]
    return dict(case=entry['case'], seed=entry['seed'], runner_source_sha256=manifest['runner_source_sha256'],
        kernel_sha256=inputs['kernel']['sha256'], kernel_source_commit=inputs['build']['source_commit'],
        kernel_source_diff_sha256=inputs['build']['source_diff_sha256'],
        firmware_sha256=inputs['firmware']['sha256'], qemu_sha256=inputs['wrapper']['sha256'],
        qemu_path=inputs['wrapper']['path'], elf_sha256=case['elf']['sha256'],
        fixture_sha256=case['fixture']['sha256'])


def verify_boot(manifest, entry, output, returncode):
    config = execution_config(manifest, entry, output)
    validate_receipt(json.loads(Path(config['receipt_path']).read_text()), config['expected'])
    directory = Path(config['directory'])
    rows = json.loads((directory / 'results.json').read_text())
    if not isinstance(rows, list) or len(rows) != 1:
        raise ValueError('exactly one natural guest result required')
    record = rows[0]
    boot = directory / (entry['case'] + '-0')
    owners = validate_boot(record, (boot / 'input.json').read_text(), (boot / 'boot.log').read_text(),
                           guest_expected(manifest, entry), returncode, directory)
    for path, sha in ((directory / 'kernel', manifest['inputs']['kernel']['sha256']),
                      (directory / 'firmware', manifest['inputs']['firmware']['sha256']),
                      (directory / f"{entry['case']}.elf", FIXED_SHA[entry['case']]),
                      (boot / 'boot.dtb', record['dtb_sha256'])):
        if digest(path) != sha:
            raise ValueError('copied execution input differs')
    if json.loads((boot / 'result.json').read_text()) != record:
        raise ValueError('nested guest records disagree')
    trace = parse_trace(Path(config['trace']).read_text(), entry['case'])
    for index, window in enumerate(trace['windows']):
        # TB-start membership is a proxy for these symbol bodies, not work in their callees.
        window['symbol_body_instruction_proxy'] = {name: sum(b['insns'] * b['windows'][index][0]
            for b in trace['blocks'] if event['pc'] <= b['pc'] < event['pc'] + event['bytes'])
            for name, event in manifest['inputs']['events'].items()}
    return dict(owners=owners, timings_ns=record['timings_ns'],
                windows=trace['windows'], tb_count=trace['tb_count'],
                trace_sha256=digest(Path(config['trace'])),
                log_sha256=digest(boot / 'boot.log'))


def run_boot(manifest, entry, output, lease, policy):
    reserve_attempt(output / 'attempts', entry)
    config = execution_config(manifest, entry, output)
    config_path = output / f"{entry['ordinal']:02d}.config.json"
    matched.save(config_path, config)
    item = dict(entry, status='attempt_reserved', policy_before=policy.check())
    path = output / f"{entry['ordinal']:02d}.result.json"
    matched.save(path, item)
    inputs = manifest['inputs']
    command = [sys.executable, '-B', str(ROOT / 'tests/cost-riscv.py'), '--case', entry['case'],
        '--replicas', '1', '--off', '--qemu-seed', str(entry['seed']), '--fixture-cache', str(CACHE/'fixture-cache'),
        '--kernel', inputs['kernel']['path'], '--kernel-identity', inputs['identity']['path'],
        '--coordinator-elf', inputs['cases'][entry['case']]['elf']['path'],
        '--qemu', inputs['wrapper']['path'], '--bios', inputs['firmware']['path'],
        '--transport', 'modern', '--cache', 'writeback', '--measurement-lock-fd', str(lease.fileno()),
        '--output', config['directory']]
    process = None
    try:
        item['command'] = command
        with (output / f"{entry['ordinal']:02d}.runner.log").open('w') as stream:
            process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL, stdout=stream,
                stderr=subprocess.STDOUT, pass_fds=(lease.fileno(),), start_new_session=True,
                preexec_fn=lambda: os.sched_setaffinity(0, {2}),
                env=dict(os.environ, BOAROS_DIAG_CONFIG=str(config_path)))
            process.wait(timeout=240)
        item['returncode'] = process.returncode
        item.update(verify_boot(manifest, entry, output, process.returncode))
        item['policy_after'] = policy.check()
        item['status'] = 'diagnostic_completed'
    except (Exception, KeyboardInterrupt) as error:
        item.update(status='diagnostic_failed', error=str(error), error_type=type(error).__name__)
        if isinstance(error, KeyboardInterrupt):
            raise
    finally:
        if process is not None:
            matched.stop_boot(process)
            item['returncode'] = process.returncode
        matched.save(path, item)
    return item


def campaign(output, lease):
    if os.geteuid() == 0:
        raise ValueError('run unprivileged; only the fixed policy helper uses sudo')
    manifest = json.loads((output / 'manifest.json').read_text())
    verify_inputs(manifest)
    # One irreversible start marker; even a failed acquisition must be inspected before another batch.
    with (output / 'started').open('x') as marker:
        marker.write(PROFILE + '\n')
    saved_affinity = os.sched_getaffinity(0)
    original_signals = {}
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        original_signals[signum] = signal.signal(signum, interrupted)
    result = dict(profile=PROFILE, acceptance=False, performance_verified=False,
                  records=[], host_before=matched.host_state(), status='environment_blocked')
    policy = matched.PolicyClient(output)
    try:
        outside = saved_affinity - {2, 3}
        if not outside or 2 not in saved_affinity:
            raise ValueError('CPU2 and supervisor CPUs outside2/3 required')
        os.sched_setaffinity(0, outside)
        result['policy_ready'] = policy.acquire(lease)
        result['status'] = 'diagnostic_running'
        for entry in manifest['schedule']:
            print(f"DIAGNOSTIC {entry['ordinal']+1}/12 {entry['case']} seed={entry['seed']}", flush=True)
            item = run_boot(manifest, entry, output, lease, policy)
            result['records'].append(item)
            matched.save(output / 'result.json', result)
            if item['status'] != 'diagnostic_completed':
                raise ValueError('failed attempt retained; batch stopped without replacement')
        result['status'] = 'diagnostic_completed'
    except (Exception, KeyboardInterrupt) as error:
        result['error'] = str(error)
        if result['status'] != 'environment_blocked':
            result['status'] = 'diagnostic_failed'
    finally:
        for signum in original_signals:
            signal.signal(signum, signal.SIG_IGN)
        result['restoration'] = policy.close()
        os.sched_setaffinity(0, saved_affinity)
        result['host_after'] = matched.host_state()
        result['attempts_reserved'] = len(list((output / 'attempts').glob('*.spent')))
        result['execution_receipts'] = len(list(output.glob('*.execution.json')))
        if result['restoration']['errors'] or result['host_before']['policies'] != result['host_after']['policies']:
            result['status'] = 'restoration_failed'
        matched.save(output / 'result.json', result)
        for signum, handler in original_signals.items():
            signal.signal(signum, handler)
    return result


def verify_campaign(output):
    manifest = json.loads((output / 'manifest.json').read_text())
    check_manifest(manifest)
    result = json.loads((output / 'result.json').read_text())
    if result.get('profile') != PROFILE or result.get('acceptance') is not False or result.get('performance_verified') is not False:
        raise ValueError('diagnostic result relabelled')
    if result['status'] != 'diagnostic_completed' or len(result['records']) != 12:
        raise ValueError('incomplete diagnostic batch')
    if result['restoration'].get('event') != 'restored' or result['restoration'].get('errors') != []:
        raise ValueError('policy restoration unverified')
    if result['host_before']['policies'] != result['host_after']['policies']:
        raise ValueError('policy originals not restored')
    for name, identity in manifest['inputs']['scripts'].items():
        if digest(output / 'sources' / name) != identity['sha256']:
            raise ValueError('sealed diagnostic source differs')
    if len(list((output / 'attempts').glob('*.spent'))) != 12:
        raise ValueError('attempt budget differs')
    for entry, record in zip(manifest['schedule'], result['records']):
        if any(record.get(k) != v for k, v in entry.items()) or record.get('status') != 'diagnostic_completed':
            raise ValueError('diagnostic order/result differs')
        for field in ('policy_before', 'policy_after'):
            validate_policy_snapshot(result['policy_ready'], record[field])
        if json.loads((output / f"{entry['ordinal']:02d}.result.json").read_text()) != record:
            raise ValueError('per-attempt termination record differs')
        if json.loads((output / 'attempts' / f"{entry['ordinal']:02d}.spent").read_text()) != entry:
            raise ValueError('attempt reservation differs')
        checked = verify_boot(manifest, entry, output, record['returncode'])
        if any(record.get(k) != v for k, v in checked.items()):
            raise ValueError('diagnostic summary differs from complete input/log/trace')
    return dict(status='diagnostic_verified', acceptance=False, performance_verified=False, attempts=12)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--prepare', type=Path)
    group.add_argument('--run', type=Path)
    group.add_argument('--verify', type=Path)
    args = parser.parse_args()
    try:
        if args.prepare:
            output = args.prepare.resolve()
            with measurement_lease():
                prepare(output)
            print(f'Diagnostic inputs sealed; zero guest starts: {output}')
            return 0
        if args.verify:
            print(canonical(verify_campaign(args.verify.resolve())).strip())
            return 0
        with measurement_lease() as lease:
            result = campaign(args.run.resolve(), lease)
        print(canonical({k: result[k] for k in ('status', 'acceptance', 'restoration')}).strip())
        return 0 if result['status'] == 'diagnostic_completed' else 2
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(2, 'diagnostic refused: ' + str(error) + '\n')


def measurement_lease():
    path = ROOT / 'build/cost/measurement.lock'
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        return acquire_measurement_lock(path)
    except BlockingIOError as error:
        raise ValueError('measurement already running; no preparation or diagnostic started') from error


if __name__ == '__main__':
    sys.exit(main())
