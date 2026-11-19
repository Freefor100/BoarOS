#!/usr/bin/env python3
"""Exercise the actual multicast option importer, with isolated usercopy stubs."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/syscall/socket.c').read_text()
start = source.index('static int ipv6_multicast_sockopt(')
end = source.index('\nstatic int sockopt(', start)
with tempfile.TemporaryDirectory(prefix='boaros-ipv6-sockopt-') as directory:
    work = Path(directory)
    (work / 'ipv6_multicast_sockopt.h').write_text(source[start:end])
    output = work / 'check'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=undefined', '-I' + str(work),
                    str(ROOT / 'tests/host/ipv6-multicast-sockopt.c'), '-o', str(output)], check=True)
    subprocess.run([str(output)], check=True)
