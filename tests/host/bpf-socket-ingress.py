#!/usr/bin/env python3
"""Exercise real cBPF execution at real lwIP receive boundaries."""
from pathlib import Path
import re
import subprocess
root = Path(__file__).resolve().parents[2]
block = (root / 'Makefile').read_text().split('LWIP_SOURCES :=', 1)[1].split('\n\n', 1)[0]
sources = re.findall(r'(?:third_party/lwip|net/lwip_port)/[\w/]+\.c', block)
output = root / 'build/host/bpf-socket-ingress'
output.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-idirafter', 'net/lwip_port/include', '-Ithird_party/lwip/src/include', '-idirafter', 'include',
                'tests/host/bpf_socket_ingress.c', 'kernel/bpf/vm.c', 'kernel/bpf/classic.c',
                'kernel/bpf/verify.c', *sources, '-o', str(output)], cwd=root, check=True)
subprocess.run([str(output)], cwd=root, check=True, timeout=20)
