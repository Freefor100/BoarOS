#!/usr/bin/env python3
"""Restore the existing GNU identities from fixed, relocatable packages."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/userland/glibc'))
from profiles import checked_inputs, digest

PACKAGES = {
    'loongarch': ['toolchains/la-gcc15-glibc242.tar.xz'],
    'riscv': ['toolchains/' + name for name in (
        'riscv64-linux-gnu-gcc-16.2.1-1-x86_64.pkg.tar.zst',
        'riscv64-linux-gnu-binutils-2.47-1-x86_64.pkg.tar.zst',
        'riscv64-linux-gnu-glibc-2.44-1-any.pkg.tar.zst',
        'riscv64-linux-gnu-linux-api-headers-7.2-1-any.pkg.tar.zst')],
}
CACHE_INPUTS = ['references/sources.tsv', 'tests/ci/tools.py', '.github/actions/native-environment/action.yml', 'tests/userland/glibc/profiles.py',
    'tests/userland/glibc/inputs.json', 'tests/userland/glibc/inputs-loongarch.json',
    'tests/loongarch/prepare.py', 'tests/loongarch/prepare_userland.py',
    'tests/loongarch/prepare_dynamic.py', 'tests/loongarch/qemu_rtc.py',
    'tests/loongarch/qemu-ls7a-rtc.patch', 'tests/program-inventory/inputs.json',
    'tests/diff-abi/harness.py', 'tests/diff-abi/linux.config']


def installation(directory):
    rows = []
    for path in sorted(directory.rglob('*')):
        if path == directory / 'ci-identity.json':
            continue
        row = [str(path.relative_to(directory)), path.lstat().st_mode & 0o777]
        if path.is_symlink():
            row += ['link', str(path.readlink())]
        elif path.is_file():
            row += ['file', digest(path)]
        elif path.is_dir():
            row += ['directory']
        else:
            raise RuntimeError('unsupported tool package object: ' + str(path))
        rows.append(row)
    return hashlib.sha256(json.dumps(rows, separators=(',', ':')).encode()).hexdigest()


def rows():
    return [line.split('\t') for line in (ROOT / 'references/sources.tsv').read_text().splitlines()
            if line and not line.startswith('#')]


def restore(names, label):
    selected = [row for row in rows() if row[1] in names]
    if len(selected) != len(set(names)):
        raise RuntimeError('missing or duplicate CI reference input')
    directory = ROOT / 'build/tools'
    directory.mkdir(parents=True, exist_ok=True)
    manifest = directory / (label + '-references.tsv')
    manifest.write_text(''.join('\t'.join(row) + '\n' for row in selected))
    subprocess.run(['sh', str(ROOT / 'references/fetch.sh'), '--manifest', str(manifest)], check=True)
    return {row[1]: row[4] for row in selected}


def verify_tool_commands(arch, directory):
    binary = directory / ('usr/bin' if arch == 'riscv' else 'bin')
    prefix = 'riscv64-linux-gnu-' if arch == 'riscv' else 'loongarch64-unknown-linux-gnu-'
    # 文件身份不能证明宿主动态依赖齐全；在导出PATH、保存缓存前实际启动工具。
    for suffix in ('gcc', 'as', 'ld', 'ar', 'objdump', 'readelf'):
        command = binary / (prefix + suffix)
        try:
            result = subprocess.run([str(command), '--version'], stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=15)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise RuntimeError(f'CI tool cannot start: {command}: {error}') from error
        if result.returncode:
            raise RuntimeError(f'CI tool cannot start: {command}: exit {result.returncode}: {result.stdout.strip()}')
        print(f'CI tool executable: {command}: {result.stdout.splitlines()[0]}', flush=True)


def prepare_tools(arch):
    directory = ROOT / 'build/tools' / arch
    selected = {row[1]: row[4] for row in rows() if row[1] in PACKAGES[arch]}
    expected = {'arch': arch, 'archives': selected,
                'profile': digest(ROOT / 'tests/userland/glibc' / ('inputs.json' if arch == 'riscv' else 'inputs-loongarch.json'))}
    stamp = directory / 'ci-identity.json'
    if stamp.is_file():
        recorded = json.loads(stamp.read_text())
        if recorded['inputs'] != expected or recorded['installation'] != installation(directory):
            raise RuntimeError('CI tool cache identity mismatch')
    else:
        if directory.exists():
            raise RuntimeError('unidentified CI tool directory: ' + str(directory))
        restore(PACKAGES[arch], arch + '-tools')
        staging = directory.with_name(arch + '.staging')
        staging.mkdir(exist_ok=False)
        for name in PACKAGES[arch]:
            archive = ROOT / 'references' / name
            command = ['tar', '-xf', str(archive), '--no-same-owner', '-C', str(staging)]
            if arch == 'loongarch':
                command += ['--strip-components=1']
            subprocess.run(command, check=True)
        checked_inputs(arch, staging)
        (staging / 'ci-identity.json').write_text(json.dumps({
            'inputs': expected, 'installation': installation(staging)}, indent=2) + '\n')
        staging.rename(directory)
    checked_inputs(arch, directory)
    verify_tool_commands(arch, directory)
    return directory


def prepare_references(arch):
    names = ['musl/musl-1.2.5.tar.gz', 'sqlite/sqlite-amalgamation-3530400.zip', 'linux',
             'linux-uapi/linux-6.6.tar.xz', 'oscomp-testsuits']
    if arch == 'loongarch':
        names += ['qemu', 'gcc/gcc-15.1.0.tar.xz']
    restore(names, arch + '-native')
    if arch == 'loongarch':
        source = ROOT / 'references/qemu'
        for name in ('berkeley-softfloat-3', 'berkeley-testfloat-3', 'keycodemapdb'):
            fields = dict(line.split('=', 1) for line in (source / 'subprojects' / (name + '.wrap')).read_text().splitlines() if '=' in line)
            fields = {key.strip(): value.strip() for key, value in fields.items()}
            destination = source / 'subprojects' / name
            if not (destination / '.git').exists():
                destination.mkdir(exist_ok=False)
                subprocess.run(['git', '-C', str(destination), 'init', '-q'], check=True)
                subprocess.run(['git', '-C', str(destination), 'remote', 'add', 'origin', fields['url']], check=True)
                subprocess.run(['git', '-C', str(destination), 'fetch', '--depth=1', 'origin', fields['revision']], check=True)
                subprocess.run(['git', '-C', str(destination), 'checkout', '--detach', 'FETCH_HEAD'], check=True)
            actual = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
            if actual != fields['revision']:
                raise RuntimeError('QEMU wrap revision mismatch: ' + name)


def cache_key(arch):
    identity = {'arch': arch, 'workspace': str(ROOT), 'files': {name: digest(ROOT / name) for name in CACHE_INPUTS},
                'python': sys.version}
    for tool in ('cc', 'gcc', 'g++', 'ld', 'as', 'make', 'flex', 'bison', 'bc'):
        path = Path(shutil.which(tool) or tool).resolve()
        identity[tool] = {'path': str(path), 'sha256': digest(path)}
    return hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=tuple(PACKAGES), required=True)
    parser.add_argument('--cache-key', action='store_true')
    parser.add_argument('--references', action='store_true')
    parser.add_argument('--github-env', action='store_true')
    args = parser.parse_args()
    if args.cache_key:
        print(cache_key(args.arch))
        return
    directory = prepare_tools(args.arch)
    if args.references:
        prepare_references(args.arch)
    if args.github_env:
        bin = directory / ('usr/bin' if args.arch == 'riscv' else 'bin')
        with open(os.environ['GITHUB_PATH'], 'a') as stream:
            stream.write(str(bin) + '\n')
        with open(os.environ['GITHUB_ENV'], 'a') as stream:
            stream.write('BOAROS_GLIBC_ROOT=' + str(directory) + '\n')
            if args.arch == 'loongarch':
                stream.write('LA_CROSS_COMPILE=' + str(bin / 'loongarch64-unknown-linux-gnu-') + '\n')
    print(f'CI {args.arch} tools verified: {directory}', flush=True)


if __name__ == '__main__':
    main()
