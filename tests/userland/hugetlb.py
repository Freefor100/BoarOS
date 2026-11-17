#!/usr/bin/env python3
"""Disposable guest-only explicit hugetlb test; never changes the host pool.
Uses existing compiler, kernel and QEMU. No downloads or broad baseline runs.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tests'), str(ROOT / 'tests/diff-abi')]
import harness
from arch_profiles import PROFILES


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--arch', choices=PROFILES, required=True)
    p.add_argument('--cc', required=True)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--qemu', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--memory', default='768M')
    p.add_argument('--shm-flags-only', action='store_true', help='test SysV huge-size flags and key lookup only')
    p.add_argument('--timeout', type=int, default=120)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    profile = PROFILES[a.arch]
    program = a.output / 'init'
    source = ROOT / ('tests/workloads/resource/shm_huge_flags.c' if a.shm_flags_only else 'tests/workloads/resource/hugetlb.c')
    command = [a.cc, '-static', '-O2', '-Wall', '-Wextra', '-Werror']
    if a.arch == 'loongarch':
        command += ['-Wl,-z,max-page-size=16384']
    command += [str(source), '-o', str(program)]
    subprocess.run(command, check=True)
    image = harness.fixture(a.output, program)
    kernel = a.output / 'kernel'
    kernel.write_bytes(a.kernel.read_bytes())
    # No RTC-sensitive assertions: stock QEMU is sufficient, no patched RTC
    # property is requested. Native profile supplies block device topology.
    boot = [a.qemu, '-machine', 'virt', '-smp', '1', '-m', a.memory,
            '-kernel', str(kernel), '-nographic', '-no-reboot', '-net', 'none']
    boot += ['-cpu', 'la464'] if a.arch == 'loongarch' else ['-bios', 'default']
    boot += ['-object', 'rng-random,id=entropy,filename=/dev/urandom',
             '-device', profile.rng()]
    boot += ['-drive', f'file={image},if=none,format=raw,id=root',
             '-device', profile.block('virtio')]
    metadata = dict(arch=a.arch, compiler_command=command, boot=boot,
                    source_sha256=digest(source), elf_sha256=digest(program),
                    kernel_sha256=digest(kernel), status='running')
    metadata_path = a.output / 'metadata.json'
    metadata_path.write_text(json.dumps(metadata, indent=2))
    try:
        with (a.output / 'guest.log').open('w') as log:
            result = subprocess.run(boot, stdin=subprocess.DEVNULL, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=a.timeout)
        raw = (a.output / 'guest.log').read_text().replace('\r', '')
        cases = ('growsdown-flag-isolation', 'real-pool', 'fault-offset-mincore', 'private-fork-cow',
                 'protect-unmap-boundaries', 'shared-fork-offset', 'pipe-compound-owners', 'sysv-huge-owners', 'noreserve-sigbus', 'sysv-noreserve-sigbus')
        markers = [line for line in raw.splitlines() if line.startswith(('HUGETLB CASE ', 'HUGETLB END ', 'HUGETLB FAIL '))]
        expected = [f'HUGETLB CASE {name} PASS' for name in cases] + [f'HUGETLB END {len(cases)}']
        if a.shm_flags_only:
            markers = [line for line in raw.splitlines() if line.startswith('SHM_HUGE_FLAGS ')]
            expected = ['SHM_HUGE_FLAGS PASS']
        passed = result.returncode == 0 and markers == expected and profile.root_success(raw, 0)
        metadata.update(status='passed' if passed else 'failed', returncode=result.returncode,
                        markers=markers, root_cleanup=profile.root_success(raw, 0))
    except subprocess.TimeoutExpired:
        metadata.update(status='timeout')
        passed = False
    finally:
        metadata_path.write_text(json.dumps(metadata, indent=2))
    print(json.dumps(metadata, indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
