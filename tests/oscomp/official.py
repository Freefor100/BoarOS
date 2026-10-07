#!/usr/bin/env python3
"""Run the unchanged fixed Docker Harness against a clean committed snapshot."""
import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import time
import zipfile

import run as local

ROOT, HERE, REF = local.ROOT, local.HERE, local.REF


def require_digest(reference):
    if not re.fullmatch(r'[^\s@]+@sha256:[0-9a-f]{64}', reference):
        raise ValueError('container reference must include an immutable sha256 digest')
    return reference


def snapshot_source(repository, destination):
    repository, destination = Path(repository), Path(destination)
    if local.output(['git', 'status', '--porcelain'], repository):
        raise RuntimeError('official submission requires a clean committed source tree')
    identity = {'commit': local.output(['git', 'rev-parse', 'HEAD'], repository),
                'tree': local.output(['git', 'rev-parse', 'HEAD^{tree}'], repository)}
    data = subprocess.check_output(['git', 'archive', '--format=tar', identity['commit']], cwd=repository)
    destination.mkdir()
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        # Git-owned content only; extraction must not follow paths outside this snapshot.
        archive.extractall(destination, filter='data')
    identity['archive_sha256'] = hashlib.sha256(data).hexdigest()
    return identity


def gzip_asset(source, expected, cache):
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / (source.name + '.gz')
    if target.exists():
        digest = hashlib.sha256()
        with gzip.open(target, 'rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''): digest.update(block)
        if digest.hexdigest() != expected or target.is_symlink() or target.stat().st_mode & 0o777 != 0o644:
            raise RuntimeError('compressed cache identity differs: ' + str(target))
        return target
    temporary = target.with_suffix('.tmp')
    digest = hashlib.sha256()
    try:
        with source.open('rb') as stream, temporary.open('wb') as output:
            with gzip.GzipFile(filename='', mode='wb', fileobj=output, compresslevel=1, mtime=0) as compressed:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(block); compressed.write(block)
        if digest.hexdigest() != expected: raise RuntimeError('original release content differs')
        temporary.chmod(0o644); temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    return target


def capture_container_identity(reference, directory):
    inspected = json.loads(subprocess.check_output(['docker', 'image', 'inspect', reference], text=True))[0]
    if reference not in inspected.get('RepoDigests', []):
        raise RuntimeError('local image does not carry the pinned RepoDigest')
    script = r"""import hashlib,json,os,shutil,subprocess
names=['qemu-system-riscv64','qemu-system-loongarch64','riscv64-unknown-elf-gcc','loongarch64-linux-gnu-gcc','python3','make']
result={}
for name in names:
 path=shutil.which(name)
 if not path: raise RuntimeError('missing container tool '+name)
 resolved=os.path.realpath(path);digest=hashlib.sha256()
 with open(resolved,'rb') as f:
  for block in iter(lambda:f.read(1048576),b''):digest.update(block)
 result[name]={'path':path,'resolved_path':resolved,'mode':oct(os.stat(resolved).st_mode&511),'sha256':digest.hexdigest(),'version':subprocess.check_output([path,'--version'],text=True).splitlines()[0]}
firmware='/opt/qemu-bin-10.0.2/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin'
if os.path.isfile(firmware):result['firmware']={'path':firmware,'sha256':hashlib.sha256(open(firmware,'rb').read()).hexdigest()}
print(json.dumps(result))
"""
    raw = subprocess.check_output(['docker', 'run', '--rm', '--network', 'none', '--entrypoint',
        'python3', reference, '-c', script], text=True)
    tools = json.loads(raw)
    identity = {'reference': reference, 'image_id': inspected['Id'], 'tools': tools,
                'architecture': inspected['Architecture'], 'os': inspected['Os']}
    (directory / 'container-identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    return identity


def original_job_output(log):
    result = None
    with log.open(errors='replace') as stream:
        for line in stream:
            try: value = json.loads(line)
            except (ValueError, TypeError): continue
            if isinstance(value, dict) and all(key in value for key in ('score', 'verdict', 'rank')):
                result = value
    if result is None: raise RuntimeError('original Harness did not emit its final Job JSON')
    return result


def harness_error_stage(job):
    if job.get('verdict') == 'Compile Error': return 'compilation'
    if job.get('verdict') in ('Runtime Error', 'Unknown Error'): return 'official-harness'
    return None


def classify_official_exit(serial, evidence):
    evidence = evidence or {}
    if 'root boot error' in serial or 'LA root boot errno=' in serial: return 'guest-boot-error'
    if local.kernel_failure(serial): return 'kernel-runtime-error'
    if evidence.get('budget_observed'): return 'total-budget-timeout'
    if evidence.get('end_observed'): return 'qemu-exit'
    return 'unknown-lifecycle'


def baseline_ready(architectures):
    # 原judge分数只证明判分完成；两侧运行与收口证据另外决定基线状态。
    return set(architectures) == set(local.ARCH_KEYS) and all(
        entry['exit_reason'] == 'total-budget-timeout' or
        (entry['exit_reason'] == 'qemu-exit' and entry['root_resources_verified'])
        for entry in architectures.values())


LIFECYCLE_PROBE = r'''import os,json
result={'uptime':float(open('/proc/uptime').read().split()[0]),'processes':{}}
for pid in os.listdir('/proc'):
 if not pid.isdigit():continue
 try:
  argv=open('/proc/'+pid+'/cmdline','rb').read().split(b'\0')
  name=os.path.basename(argv[0].decode())
  arch={'qemu-system-riscv64':'riscv','qemu-system-loongarch64':'loongarch'}.get(name)
  if not arch:continue
  if os.path.basename(os.readlink('/proc/'+pid+'/exe'))!=name:continue
  words=[a.decode() for a in argv if a]
  if words[words.index('-kernel')+1]!={'riscv':'kernel-rv','loongarch':'kernel-la'}[arch]:continue
  stat=open('/proc/'+pid+'/stat').read().rpartition(')')[2].split()
  start=int(stat[19]);elapsed=result['uptime']-start/os.sysconf('SC_CLK_TCK')
  if arch in result['processes']:raise RuntimeError('multiple QEMU owners')
  result['processes'][arch]={'pid':int(pid),'start_ticks':start,'elapsed_seconds':elapsed}
 except (FileNotFoundError,ProcessLookupError,ValueError):continue
print(json.dumps(result))'''


def update_lifecycle(record, snapshot):
    record['samples'] += 1
    for arch in local.ARCH_KEYS:
        item, old = snapshot['processes'].get(arch), record['architectures'].get(arch)
        if item:
            if old and (old['pid'], old['start_ticks']) != (item['pid'], item['start_ticks']):
                raise RuntimeError('QEMU owner changed during the single boot')
            record['architectures'][arch] = {**(old or {}), **item, 'end_observed': False,
                'budget_observed': bool(old and old.get('budget_observed')) or
                                   item['elapsed_seconds'] >= record['budget_seconds'] + 1}
        elif old:
            old['end_observed'] = True
            old.setdefault('end_observed_uptime', snapshot['uptime'])


def observe_lifecycle(directory):
    identity = json.loads((directory / 'identity.json').read_text())
    command = identity['container_command']; name = command[command.index('--name') + 1]
    reference = require_digest(identity['container']['reference'])
    record = {'observer': 'owned-container-proc', 'reference': reference,
        'source_commit': identity['source']['commit'], 'budget_seconds': identity['config']['qemu.timeout'],
        'observer_source_sha256': local.sha(Path(__file__)),
        'probe_sha256': hashlib.sha256(LIFECYCLE_PROBE.encode()).hexdigest(),
        'architectures': {}, 'samples': 0}
    def save():
        target = directory / 'lifecycle.json'; temporary = target.with_suffix('.tmp')
        temporary.write_text(json.dumps(record, indent=2) + '\n'); temporary.replace(target)
    try:
        # 只观察本次隔离提交的容器，名称本身不能证明owner。
        for attempt in range(30):
            inspected = subprocess.run(['docker', 'container', 'inspect', name], capture_output=True,
                                       text=True, timeout=5)
            if not inspected.returncode: break
            time.sleep(1)
        else: raise RuntimeError('owned official container was not observed')
        container = json.loads(inspected.stdout)[0]
        if container['Config']['Image'] != reference or not any(
            mount.get('Destination') == '/coursegrader/submit' and
            Path(mount.get('Source', '')).resolve() == (directory / 'submit').resolve()
            for mount in container['Mounts']):
            raise RuntimeError('container image or submission owner differs')
        record['container_id'] = container['Id']
        while True:
            probe = subprocess.run(['docker', 'exec', container['Id'], 'python3', '-c', LIFECYCLE_PROBE],
                                   capture_output=True, text=True, timeout=5)
            if probe.returncode:
                record['stop_reason'] = 'container-or-probe-unavailable'; break
            update_lifecycle(record, json.loads(probe.stdout)); save(); time.sleep(1)
    except Exception as error:
        record['error'] = str(error)
    finally: save()


def collect_results(directory, report):
    identity, config = report['inputs'], report['config']
    original = original_job_output(directory / 'harness.log')
    (directory / 'original-job.json').write_text(json.dumps(original, indent=2) + '\n')
    failure = harness_error_stage(original)
    if failure: raise RuntimeError('original Harness returned ' + original['verdict'])
    local.validate(False)
    lifecycle = json.loads((directory / 'lifecycle.json').read_text()) if (directory / 'lifecycle.json').is_file() else {}
    if lifecycle and (lifecycle.get('reference') != report['container']['reference'] or
                      lifecycle.get('source_commit') != report['source']['commit']):
        raise RuntimeError('lifecycle evidence belongs to another image or submission')
    report['lifecycle'] = lifecycle
    report['collector'] = {'commit': local.output(['git', 'rev-parse', 'HEAD']),
        'tree': local.output(['git', 'rev-parse', 'HEAD^{tree}']),
        'dirty': local.output(['git', 'status', '--porcelain'])}
    report['architectures'] = {}; summary = {}
    sys.path.insert(0, str(ROOT / 'tests'))
    from arch_profiles import PROFILES
    for arch, key in local.ARCH_KEYS.items():
        submit = directory / 'submit'; log = submit / ('os_serial_out_' + key + '.txt')
        if not log.is_file(): raise RuntimeError('missing actual serial stream: ' + key)
        serial = log.read_text(errors='replace'); done = 'BOAROS-EVAL COMPLETE' in serial
        evidence = lifecycle.get('architectures', {}).get(arch)
        reason = classify_official_exit(serial, evidence)
        base = submit / ('build/riscv' if arch == 'riscv' else 'build/loongarch') / 'oscomp'
        entry = {'kernel_sha256': local.sha(submit / identity['architectures'][arch]['kernel']),
            'case_sha256': local.sha(base / 'case'), 'init_config_sha256': local.sha(base / 'init.json'),
            'serial_sha256': local.sha(log), 'qemu_command_from_original_stream': serial.splitlines()[0] if serial else '',
            'qemu_returncode': None, 'boot_count': 1, 'completed_script': done, 'exit_reason': reason,
            'kernel_failure': local.kernel_failure(serial),
            'root_resources_verified': reason == 'qemu-exit' and done and PROFILES[arch].root_success(serial, 0),
            'groups': local.summarize_groups(serial, local.GROUPS, reason)}
        summary[key] = local.judge(log, config)
        (directory / ('judge-' + key + '.json')).write_text(json.dumps(summary[key], indent=2) + '\n')
        _, postwork = local.original_modules()
        for group, details in entry['groups'].items():
            scores, _ = postwork.build_table(group, [key], {key: summary[key]})
            details['judge_score'] = scores['#TOTAL']
        report['architectures'][arch] = entry
    expected = local.original_score(summary, config)
    if int(original['score']) != expected['postwork_integer_score']:
        raise RuntimeError('original Job score differs from joint upstream postwork replay')
    report.update(postwork_integer_score=int(original['score']), postwork_rank=original['rank'],
                  original_postwork_verdict=original['verdict'], results_captured=True,
                  baseline_established=baseline_ready(report['architectures']))
    report['all_scripts_completed'] = all(entry['completed_script'] for entry in report['architectures'].values())
    report.pop('error', None)
    print(json.dumps({'baseline_established': report['baseline_established'], 'all_scripts_completed': report['all_scripts_completed'],
                      'score': report['postwork_integer_score']}, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--observe-only', type=Path, help='read only the owned running container lifecycle')
    parser.add_argument('--collect-existing', type=Path, help='reclassify one completed original run without booting again')
    args = parser.parse_args()
    if args.observe_only or args.collect_existing:
        if args.output or (args.observe_only and args.collect_existing): parser.error('choose one operation')
        directory = (args.observe_only or args.collect_existing).resolve()
        if not directory.is_relative_to(ROOT / 'build') or not (directory / 'identity.json').is_file():
            parser.error('existing run must have its frozen identity under build/')
        if args.observe_only: observe_lifecycle(directory); return
        report = json.loads((directory / 'identity.json').read_text())
        stage = harness_error_stage(original_job_output(directory / 'harness.log')) or 'collection'
        try:
            collect_results(directory, report)
            if not report['baseline_established']:
                stage = 'runtime-verification'
                raise RuntimeError('official results captured; architecture run/owner blockers remain')
        except Exception as error:
            report['error'] = {'stage': stage, 'type': type(error).__name__, 'message': str(error)}; raise
        finally: (directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        return
    directory = args.output.resolve() if args.output else ROOT / 'build' / ('oscomp-official-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%f'))
    if directory.exists() or not directory.is_relative_to(ROOT / 'build'):
        parser.error('output must be a new directory under build/')
    directory.mkdir(parents=True)
    report = {'kind': 'official-container-supervised-compatibility', 'started_utc': datetime.now(timezone.utc).isoformat(),
        'baseline_established': False, 'program_matrix_passed': False,
        'supervision': {'case_timeout_seconds': 300, 'term_grace_seconds': 2,
                        'skip_table_sha256': local.sha(HERE / 'ltp-skips.tsv'), 'diagnostic_exclusions': []},
        'script_adaptations': local.script_adaptations(local.GROUPS),
        'architectures': {}}
    stage = 'preparation'; name = 'boaros-oscomp-' + directory.name.removeprefix('oscomp-official-')
    reference = None; observer = None
    try:
        identity = local.validate(True)
        report['inputs'] = identity
        reference = require_digest(identity['container']['reference'])
        submit = directory / 'submit'
        report['source'] = snapshot_source(ROOT, submit)
        report['container'] = capture_container_identity(reference, directory)
        data = directory / 'testdata'
        shutil.copytree(REF / 'kernel/judge', data, ignore=shutil.ignore_patterns('__pycache__'))
        config = json.loads((data / 'config.json').read_text())
        if (config.get('qemu.smp'), config.get('qemu.mem'), config.get('qemu.timeout')) != (1, '1G', 3600):
            raise RuntimeError('fixed official CPU/RAM/budget configuration differs')
        cache = ROOT / 'build/tools/oscomp'
        for arch in ('rv', 'la'):
            asset = 'sdcard-' + arch + '.img'
            compressed = gzip_asset(REF / asset, identity['assets'][asset], cache)
            subprocess.run(['cp', '--reflink=auto', str(compressed), str(data / compressed.name)], check=True)
        cg = directory / 'cg'; cg.mkdir()
        harness = cg / 'kernel.zip'
        files = local.output(['git', 'ls-files', 'kernel'], REF).splitlines()
        with zipfile.ZipFile(harness, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for file in files:
                archive.write(REF / file, str(Path(file).relative_to('kernel')))
        report.update(harness_zip_sha256=local.sha(harness), config=config,
                      config_sha256=local.sha(data / 'config.json'))
        hooks = directory / 'hooks'; hooks.mkdir()
        tools = report['container']['tools']
        # 原Harness在PATH前面放旧Kendryte；明确选择镜像已提供且支持当前ISA的编译器。
        build_env = {'CROSS_COMPILE': tools['riscv64-unknown-elf-gcc']['path'].removesuffix('gcc'),
                     'LA_CROSS_COMPILE': tools['loongarch64-linux-gnu-gcc']['path'].removesuffix('gcc'),
                     # 镜像GCC13不识别-mno-lsx/-mno-lasx；soft-float与禁自动向量化保持整数C。
                     'LA_FLAGS': '-march=loongarch64 -mabi=lp64s -msoft-float -mcmodel=normal '
                                 '-fno-tree-vectorize -fno-tree-slp-vectorize'}
        command = ['docker', 'run', '--rm', '--name', name, '--cidfile', str(directory / 'container.cid'), '--network', 'none',
            '-v', str(submit) + ':/coursegrader/submit', '-v', str(data) + ':/coursegrader/testdata',
            '-v', str(cg) + ':/cg:ro', '-v', str(hooks) + ':/mnt/cghook']
        for key, value in build_env.items(): command += ['-e', key + '=' + value]
        command += [reference, 'python3', '/cg/kernel.zip']
        report.update(container_command=command, build_environment=build_env)
        (directory / 'identity.json').write_text(json.dumps(report, indent=2) + '\n')
        print('Running unchanged Docker Harness; supervised compatibility baseline:', directory, flush=True)
        stage = 'official-harness'
        observer_log = (directory / 'observer.log').open('wb')
        observer = subprocess.Popen([sys.executable, '-B', str(HERE / 'official.py'), '--observe-only', str(directory)],
                                    stdout=observer_log, stderr=subprocess.STDOUT)
        observer_log.close()
        with (directory / 'harness.log').open('wb') as output:
            result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, timeout=5400)
        report['container_returncode'] = result.returncode
        if result.returncode: raise RuntimeError('official container failed; see harness.log')
        stage = 'grading'
        original = original_job_output(directory / 'harness.log')
        (directory / 'original-job.json').write_text(json.dumps(original, indent=2) + '\n')
        failure = harness_error_stage(original)
        if failure:
            stage = failure
            raise RuntimeError('original Harness returned ' + original['verdict'] + '; see original-job.json')
        if observer:
            try: observer.wait(timeout=10)
            except subprocess.TimeoutExpired:
                observer.terminate(); observer.wait(timeout=5)
        collect_results(directory, report)
        if not report['baseline_established']:
            stage = 'runtime-verification'
            raise RuntimeError('official results captured; architecture run/owner blockers remain')

    except BaseException as error:
        report['error'] = {'stage': stage, 'type': type(error).__name__, 'message': str(error)}
        cidfile = directory / 'container.cid'
        if cidfile.is_file():
            cid = cidfile.read_text().strip()
            if re.fullmatch(r'[0-9a-f]{64}', cid):
                subprocess.run(['docker', 'rm', '-f', cid], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        raise
    finally:
        if observer and observer.poll() is None:
            try: observer.wait(timeout=10)
            except subprocess.TimeoutExpired:
                observer.terminate(); observer.wait(timeout=5)
        # 容器默认root只写本次隔离目录；归还宿主owner以便既有清理流程处理。
        if reference and any(path.exists() for path in (directory / 'submit', directory / 'hooks', directory / 'testdata')):
            subprocess.run(['docker', 'run', '--rm', '--network', 'none', '--entrypoint', 'chown',
                '-v', str(directory) + ':/owned-run', reference, '-R', str(os.getuid()) + ':' + str(os.getgid()),
                '/owned-run'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        (directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
