#!/usr/bin/env python3
"""Check the bounded experiment profiles without booting a guest."""
import itertools
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    for window, pools, memory in itertools.product((8, 16, 32), (1, 2, 4), (1, 2, 4)):
        source = f'''#include "lwip/opt.h"
_Static_assert(TCP_MSS == 1460, "MSS");
_Static_assert(TCP_WND == {window * 1460}, "window candidate ignored");
_Static_assert(TCP_SND_BUF == {window * 1460}, "send candidate ignored");
_Static_assert(MEMP_NUM_TCP_SEG == {128 * pools}, "segments");
_Static_assert(MEMP_NUM_PBUF == {64 * pools}, "pbuf headers");
_Static_assert(PBUF_POOL_SIZE == {64 * pools}, "pbuf payloads");
_Static_assert(MEM_SIZE == {262144 * memory}, "heap");
_Static_assert(MEMP_NUM_TCP_PCB == 32 && MEMP_NUM_TCP_PCB_LISTEN == 16 && MEMP_NUM_UDP_PCB == 16, "PCB capacity changed");
_Static_assert(PBUF_POOL_BUFSIZE == 1536 && LWIP_WND_SCALE == 0 && TCP_WND < 65536, "wire contract changed");
_Static_assert(TCP_SND_QUEUELEN == {window * 4}, "queue budget");
'''
        command = ['cc', '-std=c11', '-Werror', '-fsyntax-only', '-x', 'c',
                   '-Inet/lwip_port/include', '-Ithird_party/lwip/src/include']
        if (window, pools, memory) != (8, 1, 1):
            command += [f'-DBOAROS_LWIP_WINDOW_MSS={window}', f'-DBOAROS_LWIP_POOL_SCALE={pools}',
                        f'-DBOAROS_LWIP_MEM_SCALE={memory}']
        command.append('-')
        subprocess.run(command, cwd=ROOT, input=source, text=True, check=True)
    source = '#include "lwip/opt.h"\n'
    for option in ('WINDOW_MSS=64', 'POOL_SCALE=3', 'MEM_SCALE=0'):
        result = subprocess.run(['cc', '-E', '-x', 'c', '-Inet/lwip_port/include',
                                 '-Ithird_party/lwip/src/include', '-DBOAROS_LWIP_' + option, '-'],
                                cwd=ROOT, input=source, text=True, capture_output=True)
        assert result.returncode != 0, 'invalid experiment profile accepted: ' + option
    for diagnostics, bits in ((0, 16), (1, 32)):
        source = '#include "lwip/stats.h"\n' + f'_Static_assert(sizeof(STAT_COUNTER) * 8 == {bits}, "counter width");\n'
        subprocess.run(['cc', '-std=c11', '-Werror', '-fsyntax-only', '-x', 'c',
                        '-Inet/lwip_port/include', '-Ithird_party/lwip/src/include',
                        f'-DBOAROS_COST_DIAGNOSTICS={diagnostics}', '-'],
                       cwd=ROOT, input=source, text=True, check=True)
    print('27 bounded TCP budget profiles and invalid overrides PASS')


if __name__ == '__main__':
    main()
