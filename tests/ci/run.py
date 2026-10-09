#!/usr/bin/env python3
"""Run independent main-branch contracts and retain every command result."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def jobs_passed(required, jobs):
    return bool(required) and all(jobs.get(name, {}).get('result') == 'success' for name in required)


def stop(process):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        pass
    # make 的子孙可能仍持有 QEMU/磁盘；父进程退出不代表整个进程组已停止。
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def run_case(case, log):
    started = time.monotonic()
    row = {'name': case['name'], 'argv': case['argv'], 'timeout': case['timeout'], 'log': str(log)}
    with log.open('wb') as stream:
        try:
            process = subprocess.Popen(case['argv'], cwd=ROOT, stdout=stream,
                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        except OSError as error:
            row.update(status='runner-error', error=str(error), exit=None)
        else:
            try:
                code = process.wait(timeout=case['timeout'])
                row.update(status='passed' if code == 0 else 'failed', exit=code)
            except subprocess.TimeoutExpired:
                stop(process)
                row.update(status='timeout', exit=process.returncode, resource_recovery='unverified')
            except BaseException:
                stop(process)
                raise
    row['seconds'] = round(time.monotonic() - started, 3)
    return row


def run_cases(cases, directory):
    names = [case['name'] for case in cases]
    if not names or len(names) != len(set(names)) or any(not re.fullmatch(r'[a-z0-9][a-z0-9-]*', name) for name in names):
        raise ValueError('empty, duplicate or invalid CI case IDs')
    if any(not case['argv'] or case['timeout'] <= 0 for case in cases):
        raise ValueError('invalid CI command or budget')
    directory.mkdir(parents=True, exist_ok=True)
    rows = []
    report = {'status': 'incomplete', 'cases': rows, 'expected': names}
    try:
        for case in cases:
            row = run_case(case, directory / (case['name'] + '.log'))
            rows.append(row)
            print(f"{row['status'].upper()} {row['name']} ({row['seconds']}s)", flush=True)
        report['status'] = 'passed' if all(row['status'] == 'passed' for row in rows) else 'failed'
    finally:
        # 被取消也留下已执行项；缺失项保持 incomplete，不补记成功。
        (directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=('host', 'riscv', 'loongarch'))
    parser.add_argument('--suite', choices=('host', 'core', 'runtime', 'platform', 'extended'))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--check-jobs', nargs='+')
    args = parser.parse_args()
    if args.check_jobs:
        jobs = json.loads(os.environ['CI_NEEDS'])
        for name in args.check_jobs:
            print(name, jobs.get(name, {}).get('result', 'missing'))
        raise SystemExit(0 if jobs_passed(args.check_jobs, jobs) else 1)
    if not args.arch or not args.suite:
        parser.error('--arch and --suite are required')
    suites = json.loads(Path(__file__).with_name('suites.json').read_text())
    selected = suites[args.suite].get(args.arch)
    if not selected:
        parser.error('no declared suite for this architecture')
    cases = []
    for item in selected:
        argv = item.get('argv') or ['make', '--no-print-directory',
            'INIT_CONFIG=config/init.json', *item['targets']]
        cases.append({'name': item['name'], 'argv': argv, 'timeout': item.get('timeout', 1800)})
    if args.list:
        print(json.dumps(cases, indent=2))
        return
    def interrupted(signum, frame):
        raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGTERM, interrupted)
    directory = args.output or ROOT / 'build/ci-run' / (args.arch + '-' + args.suite)
    report = run_cases(cases, directory)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as stream:
            stream.write(f'### {args.arch} / {args.suite}\n\n| Contract | Status | Seconds |\n|---|---|---|\n')
            for row in report['cases']:
                stream.write(f"| {row['name']} | {row['status']} | {row['seconds']} |\n")
    raise SystemExit(0 if report['status'] == 'passed' else 1)


if __name__ == '__main__':
    main()
