#!/usr/bin/env python3
"""Production TCP persist split FIN/PSH queue contracts; no external peer claim."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--old-split-flags', action='store_true', help='negative control: restore legacy OR flag update')
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--page-shift', type=int, choices=(12, 14), default=12)
args = parser.parse_args()
text = (ROOT / 'Makefile').read_text()
block = text.split('LWIP_SOURCES :=', 1)[1].split('\n\n', 1)[0]
sources = re.findall(r'(?:third_party/lwip|net/lwip_port)/[\w/]+\.c', block)
assert len(sources) >= 20
with tempfile.TemporaryDirectory(prefix='boaros-network-owner-') as directory:
    work = Path(directory)
    (work / 'net-config.h').write_text('#define BOAROS_NET_IPV4 0x0a4d0002U\n#define BOAROS_NET_NETMASK 0xffffff00U\n')
    if args.old_split_flags:
        source = ROOT / 'third_party/lwip/src/core/tcp_out.c'
        text = source.read_text()
        old = 'TCPH_FLAGS_SET(useg->tcphdr, split_flags);'
        assert old in text
        control = work / 'tcp_out.c'
        control.write_text(text.replace(old, 'TCPH_SET_FLAG(useg->tcphdr, split_flags);', 1))
        sources = [str(control) if path == str(source.relative_to(ROOT)) else path for path in sources]
    program = work / 'owner'
    command = ['cc', '-std=c11', '-pthread', '-DBOAROS_HOST_IRQ_STATE', '-DBOAROS_HOST_IRQ_COUNTS', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-DBOAROS_MAX_CPUS=2', f'-DBOAROS_PAGE_SHIFT={args.page_shift}', '-DBOAROS_COST_DIAGNOSTICS=1', '-Itests/host/random', '-idirafter', 'include',
               '-idirafter', 'net/lwip_port/include', '-Ithird_party/lwip/src/include', '-I' + str(work),
               'tests/host/tcp-split-flags.c','kernel/raw_lock.c','tests/host/cpu_context.c','-Wl,--wrap=kernel_raw_lock_acquire,--wrap=kernel_bpf_program_get,--wrap=kernel_bpf_program_put,--wrap=kernel_bpf_filter', *sources, '-o', str(program)]
    if args.sanitize: command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command, cwd=ROOT, check=True, timeout=60)
    subprocess.run([str(program)], check=True, timeout=20)
