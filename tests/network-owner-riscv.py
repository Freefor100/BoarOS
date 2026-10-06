#!/usr/bin/env python3
"""Real TAP copy-fallback smoke with indirect descriptors disabled; no performance claim."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernel', type=Path, required=True)
parser.add_argument('--qemu', default=os.environ.get('QEMU_RISCV64', 'qemu-system-riscv64'))
parser.add_argument('--transport', choices=('legacy', 'modern', 'both'), default='both')
args = parser.parse_args()
qemu = shutil.which(args.qemu)
if not qemu: parser.error('QEMU is unavailable')
(ROOT / 'build').mkdir(exist_ok=True)
work = Path(tempfile.mkdtemp(prefix='network-owner.', dir=ROOT / 'build'))
wrapper = work / 'qemu-no-indirect'
wrapper.write_text('#!/bin/sh\nexec ' + shlex.quote(qemu) + ' -global virtio-net-device.indirect_desc=off "$@"\n')
wrapper.chmod(0o755)
env = os.environ.copy(); env['QEMU_RISCV64'] = str(wrapper)
for transport in ('legacy', 'modern') if args.transport == 'both' else (args.transport,):
    result = work / transport
    command = [sys.executable, '-B', str(ROOT / 'tests/network-budget-experiment.py'), 'smoke',
               '--variant', 'patched=' + str(args.kernel.resolve()), '--transport', transport,
               '--cases', 'tap:nonblocking:mixed:5:rx,tap:nonblocking:mixed:5:tx',
               '--bytes', '262144', '--rounds', '64', '--output', str(result)]
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    for path in sorted((result / 'runs').glob('*/result.json')):
        row = json.loads(path.read_text()); driver = row.get('driver_statistics', {})
        if row['status'] != 'passed' or not driver.get('tx-copy') or driver.get('tx-sg') or driver.get('errors'):
            raise RuntimeError('actual copy-only NIC path not verified: ' + str(path))
        print(transport, row['case']['direction'], 'copy-packets', driver['tx-copy'],
              'control-during-bulk', row['metrics']['control_requests_during_bulk'], flush=True)
print('functional artifacts:', work)
