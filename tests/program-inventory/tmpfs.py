#!/usr/bin/env python3
"""Run unchanged pinned BusyBox file applets on tmpfs, outside the default inventory."""
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import suites
from run import validate_build

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
EXPECTED = (b'/tmpfs-check\n2\ntmpfs payload\n1\ntmpfs payload\n1024\n'
            b'/tmpfs-check/child\n/tmpfs-check/renamed\nnested write\n'
            b'tmpfs-filesystem-ok\n')


def validate(case, observation):
    del case
    okay = (bytes.fromhex(observation['stdout_hex']) == EXPECTED and
            bytes.fromhex(observation['stderr_hex']) == b'')
    return {'status': 'pass' if okay else 'output-contract-failed'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=ROOT / 'kernel-rv')
    parser.add_argument('--linux-kernel', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--qemu', default='qemu-system-riscv64')
    args = parser.parse_args()
    inputs = json.loads((HERE / 'inputs.json').read_text())
    build = json.loads((ROOT / 'build/program-environment/full-busybox/build.json').read_text())
    validate_build(build, inputs['busybox']['revision'])
    sys.path.insert(0, str(ROOT / 'tests/diff-abi'))
    import harness
    if args.linux_kernel:
        image = args.linux_kernel.resolve(strict=True)
        identity_path = next((p / 'identity.json' for p in image.parents
                              if (p / 'identity.json').is_file()), None)
        if identity_path is None:
            raise ValueError('Linux image has no fixed build identity')
        identity = json.loads(identity_path.read_text())
        if (identity['inputs']['source'][1] != 'f4cdf7ca9a1fdcca413157df19753f388a5a224e' or
                identity['image_sha256'] != suites.digest(image)):
            raise ValueError('Linux image identity mismatch')
    else:
        image, _ = harness.linux_build(HERE / 'linux.config')
    output = args.output or Path(tempfile.mkdtemp(prefix='busybox-tmpfs-run.', dir=ROOT / 'build/riscv'))
    output.mkdir(parents=True, exist_ok=True)
    driver = output / 'suite-driver-rv'
    compiler = ROOT / 'build/riscv/musl-root/bin/musl-gcc'
    subprocess.run([str(compiler), *build['environment']['identity']['compiler_flags'],
                    '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(HERE / 'suite_driver.c'), '-o', str(driver)], check=True)
    # Use a stable kernel snapshot even if another task rebuilds kernel-rv.
    snapshot = output / 'kernel-snapshot'
    shutil.copyfile(args.kernel, snapshot)
    script = HERE / 'tmpfs-filesystem.sh'
    manifest = {
        'schema': 1,
        'files': [{'source': build['binary'], 'destination': '/busybox',
                   'sha256': build['binary_sha256'], 'mode': '755'},
                  {'source': str(script), 'destination': '/tmpfs-filesystem.sh',
                   'sha256': suites.digest(script), 'mode': '755'}],
        'cases': [{'id': 'busybox-tmpfs-filesystem',
                   'argv': ['/busybox', 'sh', '/tmpfs-filesystem.sh'],
                   'timeout': 20, 'output_contract': 'tmpfs-filesystem'}],
        'sources': {'busybox_revision': build['revision'],
                    'busybox_sha256': build['binary_sha256']},
    }
    result = suites.run_suite(manifest, output / 'runs', driver, image, snapshot,
                              qemu=args.qemu, output_validator=validate,
                              keep_pass_images=False)
    row = result['results']['busybox-tmpfs-filesystem']
    print('BusyBox tmpfs identity:', json.dumps({
        'busybox_revision': build['revision'], 'busybox_sha256': build['binary_sha256'],
        'script_sha256': suites.digest(script), 'driver_sha256': suites.digest(driver),
        'kernels': result['identity']['kernels']}, sort_keys=True))
    print('BusyBox tmpfs verdict:', row['status'])
    print('Evidence:', output)
    if not suites.strict_result_passes(result):
        for name, guest in row['guests'].items():
            print(name, guest['guest_status'], guest['exit_code'],
                  bytes.fromhex(guest['stdout_hex']).decode(errors='replace'),
                  bytes.fromhex(guest['stderr_hex']).decode(errors='replace'))
        return 1
    print(EXPECTED.decode(), end='')
    snapshot.unlink()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
