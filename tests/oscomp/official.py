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


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    directory = args.output.resolve() if args.output else ROOT / 'build' / ('oscomp-official-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%f'))
    if directory.exists() or not directory.is_relative_to(ROOT / 'build'):
        parser.error('output must be a new directory under build/')
    directory.mkdir(parents=True)
    report = {'kind': 'official-container-supervised-compatibility', 'started_utc': datetime.now(timezone.utc).isoformat(),
        'baseline_established': False, 'program_matrix_passed': False,
        'supervision': {'case_timeout_seconds': 300, 'term_grace_seconds': 2,
                        'skip_table_sha256': local.sha(HERE / 'ltp-skips.tsv'), 'diagnostic_exclusions': []},
        'architectures': {}}
    stage = 'preparation'; name = 'boaros-oscomp-' + directory.name.removeprefix('oscomp-official-')
    reference = None
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
                     'LA_CROSS_COMPILE': tools['loongarch64-linux-gnu-gcc']['path'].removesuffix('gcc')}
        command = ['docker', 'run', '--rm', '--name', name, '--network', 'none',
            '-v', str(submit) + ':/coursegrader/submit', '-v', str(data) + ':/coursegrader/testdata',
            '-v', str(cg) + ':/cg:ro', '-v', str(hooks) + ':/mnt/cghook']
        for key, value in build_env.items(): command += ['-e', key + '=' + value]
        command += [reference, 'python3', '/cg/kernel.zip']
        report.update(container_command=command, build_environment=build_env)
        (directory / 'identity.json').write_text(json.dumps(report, indent=2) + '\n')
        print('Running unchanged Docker Harness; supervised compatibility baseline:', directory, flush=True)
        stage = 'official-harness'
        with (directory / 'harness.log').open('wb') as output:
            result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, timeout=5400)
        report['container_returncode'] = result.returncode
        if result.returncode: raise RuntimeError('official container failed; see harness.log')
        stage = 'grading'
        original = original_job_output(directory / 'harness.log')
        (directory / 'original-job.json').write_text(json.dumps(original, indent=2) + '\n')
        summary = {}
        for arch, key in local.ARCH_KEYS.items():
            log = submit / ('os_serial_out_' + key + '.txt')
            if not log.is_file(): raise RuntimeError('missing actual serial stream: ' + key)
            serial = log.read_text(errors='replace')
            command_line = serial.splitlines()[0] if serial else ''
            kernel = submit / identity['architectures'][arch]['kernel']
            done = 'BOAROS-EVAL COMPLETE' in serial
            reason = ('guest-boot-error' if 'root boot error' in serial or 'LA root boot errno=' in serial else
                      'qemu-exit' if done else 'incomplete-or-total-budget-timeout')
            groups = local.summarize_groups(serial, local.GROUPS,
                'total-budget-timeout' if not done else reason)
            sys.path.insert(0, str(ROOT / 'tests'))
            from arch_profiles import PROFILES
            entry = {'kernel_sha256': local.sha(kernel), 'serial_sha256': local.sha(log),
                'qemu_command_from_original_stream': command_line, 'boot_count': 1,
                'completed_script': done, 'exit_reason': reason,
                'root_resources_verified': done and PROFILES[arch].root_success(serial, 0), 'groups': groups}
            summary[key] = local.judge(log, config)
            (directory / ('judge-' + key + '.json')).write_text(json.dumps(summary[key], indent=2) + '\n')
            _, postwork = local.original_modules()
            for group, details in groups.items():
                scores, _ = postwork.build_table(group, [key], {key: summary[key]})
                details['judge_score'] = scores['#TOTAL']
            report['architectures'][arch] = entry
        expected = local.original_score(summary, config)
        if int(original['score']) != expected['postwork_integer_score']:
            raise RuntimeError('original Job score differs from joint upstream postwork replay')
        report['postwork_integer_score'] = int(original['score'])
        report['postwork_rank'] = original['rank']; report['original_postwork_verdict'] = original['verdict']
        report['baseline_established'] = True
        report['all_scripts_completed'] = all(entry['completed_script'] for entry in report['architectures'].values())
        print(json.dumps({'baseline_established': True, 'all_scripts_completed': report['all_scripts_completed'],
                          'score': report['postwork_integer_score']}, indent=2), flush=True)
    except BaseException as error:
        report['error'] = {'stage': stage, 'type': type(error).__name__, 'message': str(error)}
        subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        raise
    finally:
        # 容器默认root只写本次隔离目录；归还宿主owner以便既有清理流程处理。
        if reference and any(path.exists() for path in (directory / 'submit', directory / 'hooks', directory / 'testdata')):
            subprocess.run(['docker', 'run', '--rm', '--network', 'none', '--entrypoint', 'chown',
                '-v', str(directory) + ':/owned-run', reference, '-R', str(os.getuid()) + ':' + str(os.getgid()),
                '/owned-run'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        (directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
