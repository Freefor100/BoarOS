#!/usr/bin/env python3
"""Check TAP as the job user; grant only a private unshare copy userns access."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
DIRECTORY = ROOT / 'build/tools/ci-netns'
RESTRICTION = Path('/proc/sys/kernel/apparmor_restrict_unprivileged_userns')
PROBE_TIMEOUT = 15


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def probe(binary):
    environment = os.environ.copy()
    environment['BOAROS_UNSHARE'] = str(binary)
    command = [sys.executable, '-B', str(ROOT / 'tests/network-external.py'), '--host-probe']
    with subprocess.Popen(command, cwd=ROOT, env=environment, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True, start_new_session=True) as process:
        try:
            stdout, stderr = process.communicate(timeout=PROBE_TIMEOUT)
        except subprocess.TimeoutExpired:
            # unshare/探针可能已有后代；只杀Python父进程不能证明namespace已退出。
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, stderr = process.communicate()
            stderr += '\nprivate namespace/TAP probe timed out\n'
        return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


def prepare(configure=False):
    binary = shutil.which(os.environ.get('BOAROS_UNSHARE', 'unshare'))
    if binary is None:
        raise RuntimeError('TAP environment is missing unshare')
    binary = Path(binary).resolve()
    result = probe(binary)
    policy = None
    if result.returncode:
        print(result.stdout + result.stderr, file=sys.stderr, flush=True)
        if result.returncode < 0 or not configure or os.environ.get('GITHUB_ACTIONS') != 'true' or \
                not RESTRICTION.is_file() or RESTRICTION.read_text().strip() != '1':
            raise RuntimeError('private user/net namespace or TAP access unavailable; host policy was not changed')
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        private = DIRECTORY / 'unshare'
        shutil.copy2(binary, private)
        private.chmod(0o755)
        if digest(private) != digest(binary):
            raise RuntimeError('private unshare copy identity mismatch')
        policy = DIRECTORY / 'userns.profile'
        policy.write_text('abi <abi/4.0>,\ninclude <tunables/global>\n'
                          f'profile boaros-ci-unshare {json.dumps(str(private))} flags=(unconfined) {{\n'
                          '  userns,\n}\n')
        # 仅CI专用副本取得userns许可；测试进程与产物仍属于普通job用户。
        subprocess.run(['sudo', '-n', 'apparmor_parser', '-r', str(policy)], check=True)
        binary = private
        result = probe(binary)
        if result.returncode:
            print(result.stdout + result.stderr, file=sys.stderr, flush=True)
            raise RuntimeError('private namespace/TAP probe still failed after the scoped AppArmor profile')
    DIRECTORY.mkdir(parents=True, exist_ok=True)
    (DIRECTORY / 'identity.json').write_text(json.dumps({
        'status': 'passed', 'unshare': str(binary), 'sha256': digest(binary),
        'profile': str(policy) if policy else None,
        'scope': 'ordinary job UID, private user/net namespace, actual TAP ioctl and IP setup',
    }, indent=2) + '\n')
    print(result.stdout, end='', flush=True)
    return binary


def cleanup():
    policy = DIRECTORY / 'userns.profile'
    if policy.is_file():
        if os.environ.get('GITHUB_ACTIONS') != 'true':
            raise RuntimeError('AppArmor cleanup is limited to the explicit CI host setup')
        subprocess.run(['sudo', '-n', 'apparmor_parser', '-R', str(policy)], check=True)
        policy.unlink()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configure-apparmor', action='store_true')
    parser.add_argument('--github-env', action='store_true')
    parser.add_argument('--cleanup', action='store_true')
    args = parser.parse_args()
    if args.cleanup:
        cleanup()
    else:
        binary = prepare(args.configure_apparmor)
        if args.github_env:
            with open(os.environ['GITHUB_ENV'], 'a') as stream:
                stream.write('BOAROS_UNSHARE=' + str(binary) + '\n')
