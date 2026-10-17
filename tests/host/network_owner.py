#!/usr/bin/env python3
"""Normal NIC capacity and heap socket/PCB lifetime contracts; no timer retry shortcut."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
cases = ('endpoint-owner', 'protocol-owner', 'protocol-transaction', 'receive-prefix', 'receive-chains', 'nodelay', 'nodelay-capacity', 'tcp-batch', 'tcp-batch-last', 'tcp-batch-abort', 'tcp-batch-error', 'unix-payload', 'receive-payload', 'udp-payload', 'copy-indirect', 'copy-ineligible', 'timewait-capacity', 'timewait-timer', 'controls','unix-sender','loopback-work')
parser.add_argument('--case', choices=(*cases, 'all'), default='all')
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
text = (ROOT / 'Makefile').read_text()
block = text.split('LWIP_SOURCES :=', 1)[1].split('\n\n', 1)[0]
sources = re.findall(r'(?:third_party/lwip|net/lwip_port)/[\w/]+\.c', block)
assert len(sources) >= 20
with tempfile.TemporaryDirectory(prefix='boaros-network-owner-') as directory:
    work = Path(directory)
    (work / 'net-config.h').write_text('#define BOAROS_NET_IPV4 0x0a4d0002U\n#define BOAROS_NET_NETMASK 0xffffff00U\n')
    program = work / 'owner'
    command = ['cc', '-std=c11', '-pthread', '-DBOAROS_HOST_IRQ_STATE', '-DBOAROS_HOST_IRQ_COUNTS', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-DBOAROS_PAGE_SHIFT=12', '-DBOAROS_COST_DIAGNOSTICS=1', '-Itests/host/random', '-idirafter', 'include',
               '-idirafter', 'net/lwip_port/include', '-Ithird_party/lwip/src/include', '-I' + str(work),
               'tests/host/network_owner.c', *sources, '-o', str(program)]
    if args.sanitize: command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command, cwd=ROOT, check=True, timeout=60)
    for case in cases if args.case == 'all' else (args.case,):
        subprocess.run([str(program), case], check=True, timeout=20)
