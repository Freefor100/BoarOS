#!/usr/bin/env python3
"""One-boot RV projection of the fixed OSComp parser, judges and postwork."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib
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

def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def output(args, cwd=ROOT):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()

def validate():
    identity = json.loads((HERE / 'inputs.json').read_text())
    if output(['git', 'remote', 'get-url', 'origin'], REF) != identity['autotest_url']:
        raise RuntimeError('autotest origin differs')
    actual = output(['git', 'rev-parse', 'HEAD'], REF)
    if actual != identity['autotest_commit'] or output(['git', 'status', '--porcelain'], REF):
        raise RuntimeError('autotest identity or clean worktree differs')
    for name, expected in identity['assets'].items():
        path = REF / name
        if not path.is_file() or sha(path) != expected:
            raise RuntimeError(f'missing or altered release asset: {path}')
    return identity

class ReportJob:
    def __init__(self, results, config):
        self.results, self.config = results, config
        self.integer_score = None
        self.ranking = None
        self.html = ''
    def get_summary(self): return [{'rv': self.results}]
    def get_config(self): return self.config
    def get_log(self, key): return 'local RV-only run'
    def get_logs(self): return {}
    def get_logs_detail(self): return {}
    def score(self, value): self.integer_score = value
    def rank(self, value): self.ranking = value
    def comment(self, value): self.html = value
    def detail(self, value): pass
    def verdict(self, value): self.verdict_value = value

def grade(log, directory, config, selected):
    sys.dont_write_bytecode = True
    os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
    sys.path.insert(0, str(REF / 'kernel'))
    parser = importlib.import_module('run')
    postwork = importlib.import_module('postwork')
    results = parser.parse_serial_out_new({'testcase_dir': str(REF / 'kernel/judge')}, str(log))
    expected = {f'{group}-{libc}' for group in GROUPS for libc in ('glibc', 'musl')}
    if set(results) != expected:
        raise RuntimeError('original judge output does not contain exactly the 22 expected groups')
    (directory / 'judge.json').write_text(json.dumps(results, indent=2) + '\n')
    job = ReportJob(results, config)
    postwork.postwork(job)
    (directory / 'original-postwork.html').write_text(job.html)
    serial = log.read_text(errors='replace')
    groups = {}
    for name in sorted(expected):
        begin = f'#### OS COMP TEST GROUP START {name} ####' in serial
        end = f'#### OS COMP TEST GROUP END {name} ####' in serial
        status = re.search(r'BOAROS-EVAL EXIT ' + re.escape(name) + r' status=(\d+)', serial)
        raw, _ = postwork.build_table(name, ['rv'], {'rv': results})
        groups[name] = {'judge_score': raw['#TOTAL'], 'started': begin, 'ended': end,
                        'script_exit': int(status[1]) if status else None,
                        'state': 'not-selected' if name.rsplit('-',1)[0] not in selected else 'not-reached' if not begin else 'incomplete' if not end else 'completed'}
    return {'groups': groups, 'postwork_integer_score': job.integer_score,
            'postwork_rank': job.ranking, 'scope': 'RV '+','.join(selected)+' only; unselected groups and LA were not run'}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', type=Path, default=ROOT / 'build/oscomp-rv-run')
    ap.add_argument('--diagnostic-timeout', type=int, help='override budget; labels result diagnostic, never the formal baseline')
    ap.add_argument('--groups',choices=('all','iozone','environment','ltp'),default='all',help='subsets run the same original scripts and never imply full Harness acceptance')
    ap.add_argument('--case-timeout', type=int, default=60, help='LTP per-case deadline seconds, 0 disables the deadline; timeout is never a pass')
    args = ap.parse_args()
    directory = args.output.resolve()
    if directory.exists():
        raise SystemExit(f'output already exists; choose a new disposable run directory: {directory}')
    if not directory.is_relative_to(ROOT / 'build'):
        raise SystemExit('run outputs must be under build/')
    identity = validate()
    selected={'all':GROUPS,'iozone':['iozone'],'environment':['basic','busybox'],'ltp':['ltp']}[args.groups]
    config_path = REF / 'kernel/judge/config.json'
    config = json.loads(config_path.read_text())
    budget = args.diagnostic_timeout if args.diagnostic_timeout is not None else config.get('qemu.timeout', 60)
    if budget <= 0: raise SystemExit('timeout must be positive')
    if not 0 <= args.case_timeout <= 86400: raise SystemExit('case timeout must be between 0 and 86400 seconds')
    subprocess.run(['make', 'all', 'OSCOMP_GROUPS=' + ' '.join(selected),
                    'OSCOMP_CASE_TIMEOUT=' + str(args.case_timeout)], cwd=ROOT, check=True)
    directory.mkdir(parents=True)
    disk = directory / 'root.img'
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(REF / 'sdcard-rv.img'), str(disk)], check=True)
    # Official image is copied intact. make all supplies the boot command and helpers.
    with disk.open('rb') as stream:
        os.fsync(stream.fileno())
    kernel = directory / 'kernel-rv';shutil.copyfile(ROOT/'kernel-rv',kernel)
    qemu = os.environ.get('QEMU_RISCV64', 'qemu-system-riscv64')
    command = [qemu, '-machine', 'virt', '-kernel', str(kernel), '-m', str(config.get('qemu.mem', '1G')),
               '-nographic', '-smp', str(config.get('qemu.smp', 1)), '-bios', 'default',
               '-drive', f'file={disk},if=none,format=raw,id=x0', '-device',
               'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-no-reboot',
               '-device', 'virtio-net-device,netdev=net', '-netdev', 'user,id=net', '-rtc', 'base=utc']
    report = {'started_utc': datetime.now(timezone.utc).isoformat(), 'inputs': identity,
              'kernel_commit': output(['git', 'rev-parse', 'HEAD']), 'kernel_dirty': output(['git', 'status', '--porcelain']),
              'kernel_sha256': sha(kernel), 'config': config, 'config_sha256': sha(config_path),
              'init_config_sha256': sha(ROOT / 'build/riscv/oscomp/init.json'), 'init_script_sha256': sha(HERE / 'init.sh'),
              'qemu': output([qemu, '--version']).splitlines()[0], 'qemu_command': command,
              'timeout_seconds': budget, 'diagnostic': args.diagnostic_timeout is not None or args.groups != 'all',
              'ltp_case_timeout_seconds': args.case_timeout,
              'case_sha256': sha(ROOT / 'build/riscv/oscomp/case'),
              'ltp_hook_sha256': sha(HERE / 'ltp-hook.sh'),
              'boot_count': 1, 'extra_disk': None, 'selected_groups':selected,
              'qemu_sha256':sha(shutil.which(qemu)), 'fixture_sha256':sha(disk),
              'source_tree':output(['git','write-tree']), 'timebase_hz':10000000,
              'firmware_sha256':sha(Path('/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin'))}
    dtb=directory/'boot.dtb';probe=list(command);probe[probe.index('-machine')+1]='virt,dumpdtb='+str(dtb)
    subprocess.run(probe,cwd=directory,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
    report['dtb_sha256']=sha(dtb);command+=['-dtb',str(dtb)]
    (directory / 'identity.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Running one RV boot, total budget {budget}s; log: {directory / "serial.log"}', flush=True)
    start = time.monotonic()
    log = directory / 'serial.log'
    with log.open('wb') as stream:
        try:
            result = subprocess.run(command, cwd=directory, input=b'\n', stdout=stream, stderr=subprocess.STDOUT, timeout=budget)
            report['qemu_returncode'] = result.returncode
            report['exit_reason'] = 'qemu-exit'
        except subprocess.TimeoutExpired:
            report['qemu_returncode'] = None
            report['exit_reason'] = 'total-budget-timeout'
    report['elapsed_seconds'] = time.monotonic() - start
    report['serial_sha256'] = sha(log)
    if 'BoarOS: root boot error' in log.read_text(errors='replace'):
        report['exit_reason'] = 'guest-boot-error'
    report.update(grade(log, directory, config, selected))
    for group in report['groups'].values():
        if group['state'] == 'incomplete' and report['exit_reason'] == 'total-budget-timeout':
            group['state'] = 'timeout'
        elif group['script_exit'] not in (None, 0):
            group['state'] = 'script-failure'
    report['completed_script'] = 'BOAROS-EVAL COMPLETE' in log.read_text(errors='replace')
    if report['exit_reason'] == 'qemu-exit' and not report['completed_script']:
        report['exit_reason'] = 'guest-script-incomplete'
    (directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'exit_reason': report['exit_reason'], 'score': report['postwork_integer_score'], 'groups': report['groups']}, indent=2), flush=True)

if __name__ == '__main__':
    main()
