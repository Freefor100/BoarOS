#!/usr/bin/env python3
"""Diagnostic RV/LA projection of the fixed scripts, judges and postwork."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
REF = ROOT / 'references/oscomp-autotest'
GROUPS = ['basic', 'busybox', 'cyclictest', 'iozone', 'iperf', 'libcbench',
          'libctest', 'lmbench', 'lua', 'netperf', 'ltp']
ARCH_KEYS = {'riscv': 'rv', 'loongarch': 'la'}


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def output(args, cwd=ROOT):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def validate(verify_assets=False, architectures=('riscv', 'loongarch')):
    identity = json.loads((HERE / 'inputs.json').read_text())
    if output(['git', 'remote', 'get-url', 'origin'], REF) != identity['autotest_url']:
        raise RuntimeError('autotest origin differs')
    if output(['git', 'rev-parse', 'HEAD'], REF) != identity['autotest_commit'] or output(['git', 'status', '--porcelain'], REF):
        raise RuntimeError('autotest identity or clean worktree differs')
    for arch in architectures:
        image = REF / identity['architectures'][arch]['image']
        if not image.is_file():
            raise RuntimeError('release image is missing: ' + str(image))
    if verify_assets:
        for name, expected in identity['assets'].items():
            if not (REF / name).is_file() or sha(REF / name) != expected:
                raise RuntimeError('missing or altered release asset: ' + name)
    return identity


class ReportJob:
    def __init__(self, summary, config):
        self.summary, self.config = summary, config
        self.integer_score = self.ranking = None
        self.html = ''
    def get_summary(self): return [self.summary]
    def get_config(self): return self.config
    def get_log(self, key): return 'local diagnostic run'
    def get_logs(self): return {}
    def get_logs_detail(self): return {}
    def score(self, value): self.integer_score = value
    def rank(self, value): self.ranking = value
    def comment(self, value): self.html = value
    def detail(self, value): pass
    def verdict(self, value): self.verdict_value = value


def original_modules():
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(REF / 'kernel'))
    name = 'boaros_fixed_oscomp_run'
    if name not in sys.modules:
        spec = importlib.util.spec_from_file_location(name, REF / 'kernel/run.py')
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return sys.modules[name], importlib.import_module('postwork')


def original_score(summary, config):
    _, postwork = original_modules()
    job = ReportJob(summary, config)
    postwork.postwork(job)
    return {'postwork_integer_score': job.integer_score, 'postwork_rank': job.ranking,
            'original_postwork_verdict': job.verdict_value, 'html': job.html}


def script_adaptations(selected):
    return {
        'ltp-supervision': {'active': 'ltp' in selected, 'hook_sha256': sha(HERE / 'ltp-hook.sh')},
        'libctest-explicit-shell': {'active': 'libctest' in selected,
            'hook_sha256': sha(HERE / 'libctest-hook.sh'),
            'scope': 'two nested original text scripts, original BusyBox sh; content and judge unchanged'},
    }


def kernel_failure(serial):
    prefixes = ('BoarOS: fatal ', 'BoarOS: timer error status=',
                'BoarOS: scheduler error status=', 'BoarOS: user fault resolver error status=')
    return next((line for line in serial.splitlines() if line.strip().startswith(prefixes)), None)


def case_observations(section):
    cases, errors, current = [], [], None
    for raw in section.splitlines():
        line = raw.strip()
        visible = re.sub(r'\x1b\[[0-9;]*m', '', line)
        begin = re.fullmatch(r'RUN LTP CASE (.+)', line)
        if begin:
            if current: cases.append(current)
            current = {'name': begin[1], 'state': 'incomplete', 'stage': 'unknown',
                'owner': 'original-case', 'shell_exit_status': None, 'wait_status': None,
                'evidence': [], 'evidence_count': 0}
            continue
        kind = None
        if kernel_failure(line):
            kind = ('kernel-runtime-error', 'running', 'kernel')
        elif 'BOAROS-CASE EXEC-ERROR ' in line or "can't execute '" in line:
            kind = ('load-error', 'load', 'user-exec')
        elif 'BOAROS-CASE WAIT-ERROR ' in line:
            kind = ('runner-error', 'supervision', 'case-supervisor')
        elif 'BOAROS-CASE SETUP-ERROR ' in line:
            kind = ('preparation-error', 'preparation', 'case-supervisor')
        elif 'BOAROS-CASE SKIP ' in line:
            kind = ('source-skip', 'preparation', 'ltp-adapter')
        elif 'BOAROS-CASE EXCLUDE ' in line:
            kind = ('manual-exclusion', 'preparation', 'ltp-adapter')
        elif 'BOAROS-CASE TIMEOUT' in line:
            kind = ('supervision-timeout', 'running', 'case-supervisor')
        elif re.search(r'\b(?:TFAIL|TBROK)\s*:', visible):
            kind = ('reported-failure', 'running', 'original-case')
        elif re.search(r'\bTCONF\s*:', visible):
            kind = ('reported-skip', 'unknown', 'original-case')
        if kind:
            state, stage, owner = kind
            error = {'case': current['name'] if current else None, 'state': state,
                     'stage': stage, 'owner': owner, 'output': raw}
            errno = re.search(r'errno=(\d+)', line)
            if errno: error['errno'] = int(errno[1])
            errors.append(error)
            if current:
                if current['evidence_count'] == 0: current['first_proven_stage'] = stage
                if current['state'] == 'incomplete' or (current['state'] == 'reported-skip' and
                    state == 'reported-failure') or owner != 'original-case':
                    current.update(state=state, stage=stage, owner=owner)
                if errno: current['errno'] = int(errno[1])
                current['evidence_count'] += 1
                if len(current['evidence']) < 4: current['evidence'].append(raw)
                wait = re.search(r'wait_status=(\d+)', line)
                if wait: current['wait_status'] = int(wait[1])
        end = re.fullmatch(r'(?:FAIL|END) LTP CASE (.+?)\s*:\s*(\d+)', line)
        if end and current and end[1] == current['name']:
            current['shell_exit_status'] = int(end[2])
            if current['state'] == 'incomplete':
                # 原外层的0退出也不能代替有效断言；未观察到main的失败阶段保持unknown。
                current['state'] = 'returned' if int(end[2]) == 0 else 'nonzero-exit'
            cases.append(current); current = None
    if current: cases.append(current)
    return cases, errors


def summarize_groups(serial, selected, exit_reason='qemu-exit'):
    groups = {}
    for group in GROUPS:
        for libc in ('glibc', 'musl'):
            name = group + '-' + libc
            entered = 'BOAROS-EVAL ENTER ' + name in serial
            started = '#### OS COMP TEST GROUP START ' + name + ' ####' in serial
            ended = '#### OS COMP TEST GROUP END ' + name + ' ####' in serial
            status = re.search(r'BOAROS-EVAL EXIT ' + re.escape(name) + r' status=(\d+)', serial)
            code = int(status[1]) if status else None
            state = ('not-selected' if group not in selected else
                     'not-reached' if not entered and not started else
                     'script-failure' if code not in (None, 0) else
                     'completed' if ended and code == 0 else
                     'kernel-runtime-error' if exit_reason == 'kernel-runtime-error' else
                     'timeout' if exit_reason == 'total-budget-timeout' else 'incomplete')
            section = ''
            if entered:
                section = serial.split('BOAROS-EVAL ENTER ' + name, 1)[1].split('BOAROS-EVAL EXIT ' + name, 1)[0]
            cases, errors = case_observations(section)
            groups[name] = {'entered': entered, 'started': started, 'ended': ended,
                'script_exit': code, 'state': state, 'cases': cases, 'observed_errors': errors,
                'supervision': {'skipped': section.count('BOAROS-CASE SKIP '),
                    'excluded': section.count('BOAROS-CASE EXCLUDE '),
                    'timed_out': section.count('BOAROS-CASE TIMEOUT-END '),
                    'setup_failed': section.count('BOAROS-CASE SETUP-ERROR '),
                    'exec_failed': section.count('BOAROS-CASE EXEC-ERROR '),
                    'wait_failed': section.count('BOAROS-CASE WAIT-ERROR ')}}
    return groups


def judge(log, config):
    parser, _ = original_modules()
    results = parser.parse_serial_out_new({'testcase_dir': str(REF / 'kernel/judge')}, str(log))
    if set(results) != {group + '-' + libc for group in GROUPS for libc in ('glibc', 'musl')}:
        raise RuntimeError('original judge output differs from the 22 expected groups')
    return results


def boot_command(arch, qemu, kernel, disk, config, rng=False):
    command = [qemu]
    if arch == 'riscv': command += ['-machine', 'virt']
    command += ['-kernel', str(kernel), '-m', str(config.get('qemu.mem', '1G')),
                '-nographic', '-smp', str(config.get('qemu.smp', 1))]
    if arch == 'riscv': command += ['-bios', 'default']
    command += ['-drive', f'file={disk},if=none,format=raw,id=x0', '-device',
        'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0' if arch == 'riscv' else 'virtio-blk-pci,drive=x0',
        '-no-reboot', '-device', 'virtio-net-device,netdev=net0' if arch == 'riscv' else 'virtio-net-pci,netdev=net0',
        '-netdev', 'user,id=net0', '-rtc', 'base=utc']
    if rng:
        command += ['-object', 'rng-random,id=entropy,filename=/dev/urandom', '-device',
                    'virtio-rng-device,rng=entropy' if arch == 'riscv' else 'virtio-rng-pci,rng=entropy']
    return command


def run_guests(runs, budget):
    active = []
    try:
        for arch, entry, command, log in runs:
            stream = log.open('wb')
            try:
                process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.PIPE,
                    stdout=stream, stderr=subprocess.STDOUT)
            except Exception:
                stream.close()
                raise
            active.append((entry, process, stream, time.monotonic()))
            process.stdin.write(b'\n'); process.stdin.close()
        pending = list(active)
        while pending:
            for item in list(pending):
                entry, process, stream, start = item
                expired = budget and time.monotonic() - start >= budget
                code = process.poll()
                if code is None and not expired: continue
                timed_out = code is None and expired
                if code is None:
                    process.kill(); code = process.wait()
                else: process.wait()
                # 文件输出由真实进程关闭后收口，不能在poll结束时丢掉管道尾部。
                stream.close()
                entry.update(qemu_returncode=code, elapsed_seconds=time.monotonic() - start,
                             exit_reason='total-budget-timeout' if timed_out else 'qemu-exit')
                pending.remove(item)
            if pending: time.sleep(0.1)
    finally:
        for entry, process, stream, start in active:
            if process.poll() is None:
                process.kill(); process.wait()
            stream.close()


def diagnostic_build(architectures, selected, case_timeout, exclusions):
    environment = os.environ.copy()
    # 本次评测配置必须与冻结身份一致；调用者make的覆盖不能带入子构建。
    for name in ('INIT_CONFIG', 'INIT_CONFIG_RV', 'INIT_CONFIG_LA', 'MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES'):
        environment.pop(name, None)
    configs = {arch: Path('build/riscv' if arch == 'riscv' else 'build/loongarch') / 'oscomp/init.json'
               for arch in architectures}
    command = ['make', '-j8', *('kernel-rv' if arch == 'riscv' else 'kernel-la' for arch in architectures),
        'OSCOMP_GROUPS=' + ' '.join(selected), 'OSCOMP_CASE_TIMEOUT=' + str(case_timeout),
        'OSCOMP_DIAGNOSTIC_EXCLUDE=' + ' '.join(exclusions)]
    for arch, config in configs.items():
        command.append(('INIT_CONFIG_RV=' if arch == 'riscv' else 'INIT_CONFIG_LA=') + str(config))
    return command, environment, configs


def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument('--arch', choices=tuple(ARCH_KEYS) + ('both',), default='both')
    ap.add_argument('--output', type=Path, default=None)
    ap.add_argument('--verify-inputs', action='store_true')
    ap.add_argument('--diagnostic-timeout', type=int)
    ap.add_argument('--groups', choices=tuple(GROUPS) + ('all', 'benchmarks', 'environment'), default='all')
    ap.add_argument('--case-timeout', type=int, default=300)
    ap.add_argument('--diagnostic-exclude', action='append', default=[], metavar='CASE')
    ap.add_argument('--qemu-riscv', default=os.environ.get('QEMU_RISCV64', 'qemu-system-riscv64'))
    ap.add_argument('--qemu-loongarch', default=os.environ.get('QEMU_LOONGARCH64', 'build/qemu-la-rtc/qemu-system-loongarch64'))
    ap.add_argument('--rng', action='store_true', help='add real entropy only to this labelled diagnostic profile')
    args = ap.parse_args()
    architectures = tuple(ARCH_KEYS) if args.arch == 'both' else (args.arch,)
    directory = args.output.resolve() if args.output else ROOT / 'build' / ('oscomp-diagnostic-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%f'))
    if directory.exists(): ap.error('output already exists; use a new run directory')
    if not directory.is_relative_to(ROOT / 'build'): ap.error('outputs must be under build/')
    if args.diagnostic_timeout is not None and args.diagnostic_timeout < 0: ap.error('budget must be nonnegative')
    if not 0 <= args.case_timeout <= 86400: ap.error('case budget must be 0..86400')
    if any(not re.fullmatch(r'[A-Za-z0-9_.-]+', name) or name in ('.', '..') for name in args.diagnostic_exclude):
        ap.error('exclusions must be literal LTP basenames')
    selected = {'all': GROUPS, 'benchmarks': ['iozone', 'cyclictest', 'iperf', 'libcbench', 'lmbench'],
                'environment': ['basic', 'busybox']}.get(args.groups, [args.groups])
    directory.mkdir(parents=True)
    report = {'kind': 'local-diagnostic', 'diagnostic': True, 'started_utc': datetime.now(timezone.utc).isoformat(),
        'kernel_commit': output(['git', 'rev-parse', 'HEAD']), 'kernel_dirty': output(['git', 'status', '--porcelain']),
        'requested_architectures': architectures, 'selected_groups': selected,
        'ltp_case_timeout_seconds': args.case_timeout, 'ltp_diagnostic_exclusions': args.diagnostic_exclude,
        'script_adaptations': script_adaptations(selected),
        'rng_added': args.rng, 'architectures': {}, 'baseline_completed': False}
    stage = 'preparation'
    try:
        identity = validate(args.verify_inputs, architectures)
        config_path = REF / 'kernel/judge/config.json'
        config = json.loads(config_path.read_text())
        budget = args.diagnostic_timeout if args.diagnostic_timeout is not None else config.get('qemu.timeout', 60)
        report.update(inputs=identity, release_assets_verified_this_run=args.verify_inputs,
                      config=config, config_sha256=sha(config_path), timeout_seconds=budget)
        build, environment, configs = diagnostic_build(architectures, selected, args.case_timeout, args.diagnostic_exclude)
        subprocess.run(build, cwd=ROOT, env=environment, check=True)
        runs = []
        for arch in architectures:
            target = directory / ARCH_KEYS[arch]; target.mkdir()
            facts = identity['architectures'][arch]
            kernel = target / facts['kernel']; shutil.copyfile(ROOT / facts['kernel'], kernel)
            disk = target / 'root.img'
            subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(REF / facts['image']), str(disk)], check=True)
            qemu = args.qemu_riscv if arch == 'riscv' else args.qemu_loongarch
            executable = Path(shutil.which(qemu) or qemu).resolve(strict=True)
            base = ROOT / ('build/riscv' if arch == 'riscv' else 'build/loongarch') / 'oscomp'
            command = boot_command(arch, str(executable), kernel, disk, config, args.rng)
            entry = {'kernel_sha256': sha(kernel), 'fixture_sha256': sha(disk), 'boot_count': 1,
                'init_config_path': str(configs[arch]), 'init_config_sha256': sha(ROOT / configs[arch]),
                'case_sha256': sha(base / 'case'),
                'qemu_command': command, 'qemu': output([str(executable), '--version']).splitlines()[0],
                'qemu_sha256': sha(executable), 'qemu_mode': oct(executable.stat().st_mode & 0o777),
                'qemu_selected_path': qemu, 'qemu_resolved_path': str(executable), 'extra_disk': None}
            for name in ('init.sh', 'ltp-hook.sh', 'libctest-hook.sh', 'ltp-case.sh', 'ltp-skips.tsv'):
                entry[name + '_sha256'] = sha(HERE / name)
            report['architectures'][arch] = entry
            (target / 'identity.json').write_text(json.dumps(entry, indent=2) + '\n')
            runs.append((arch, entry, command, target / 'serial.log'))
        (directory / 'identity.json').write_text(json.dumps(report, indent=2) + '\n')
        print('Running concurrent diagnostic boots:', ','.join(architectures), 'budget:', budget, 'output:', directory, flush=True)
        stage = 'runner'
        run_guests(runs, budget)
        stage = 'grading'
        summary = {}
        sys.path.insert(0, str(ROOT / 'tests'))
        from arch_profiles import PROFILES
        for arch, entry, command, log in runs:
            serial = log.read_text(errors='replace')
            reason = entry['exit_reason']
            if 'root boot error' in serial or 'LA root boot errno=' in serial: reason = 'guest-boot-error'
            elif kernel_failure(serial): reason = 'kernel-runtime-error'
            entry.update(exit_reason=reason, serial_sha256=sha(log), completed_script='BOAROS-EVAL COMPLETE' in serial)
            entry['kernel_failure'] = kernel_failure(serial)
            entry['root_resources_verified'] = reason == 'qemu-exit' and PROFILES[arch].root_success(serial, 0)
            entry['groups'] = summarize_groups(serial, selected, reason)
            results = judge(log, config)
            summary[ARCH_KEYS[arch]] = results
            (log.parent / 'judge.json').write_text(json.dumps(results, indent=2) + '\n')
            _, postwork = original_modules()
            for name, group in entry['groups'].items():
                columns, _ = postwork.build_table(name, [ARCH_KEYS[arch]], {ARCH_KEYS[arch]: results})
                group['judge_score'] = columns['#TOTAL']
        graded = original_score(summary, config)
        (directory / 'original-postwork.html').write_text(graded.pop('html'))
        report.update(graded)
        report['baseline_completed'] = all(entry['completed_script'] and entry['root_resources_verified']
            and entry['qemu_returncode'] == 0 for entry in report['architectures'].values())
    except Exception as error:
        report.update(error={'stage': stage, 'type': type(error).__name__, 'message': str(error)})
        raise
    finally:
        (directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'kind': report['kind'], 'baseline_completed': report['baseline_completed'],
        'score': report['postwork_integer_score'], 'architectures': {
            arch: {'exit_reason': entry['exit_reason'], 'completed_script': entry['completed_script'],
                   'root_resources_verified': entry['root_resources_verified']} for arch, entry in report['architectures'].items()}}, indent=2), flush=True)


if __name__ == '__main__':
    main()
