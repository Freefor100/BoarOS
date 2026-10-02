#!/usr/bin/env python3
"""Compile the actual worker against bounded protocol/scheduler boundary models."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='boaros-ethernet-worker-') as name:
    work = Path(name)
    (work / 'net-config.h').write_text('#define BOAROS_NET_IPV4 0x0a4d0002U\n#define BOAROS_NET_NETMASK 0xffffff00U\n')
    exe = work / 'worker'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-DBOAROS_PAGE_SHIFT=12', '-Itests/host/random', '-idirafter', 'include', '-idirafter', 'net/lwip_port/include',
                    '-Ithird_party/lwip/src/include', '-I' + str(work),
                    'tests/host/ethernet_worker.c', '-o', str(exe)], cwd=root, check=True, timeout=30)
    subprocess.run([str(exe)], check=True, timeout=10)
