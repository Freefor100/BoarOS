#!/usr/bin/env python3
"""Build the compatibility bootstrap into PID 1's shell command, not the test disk."""
import argparse
import gzip
import json
import re
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--arch', choices=('riscv', 'loongarch'), default='riscv')
    parser.add_argument('--case', type=Path, required=True)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--groups', default='basic busybox cyclictest iozone iperf libcbench libctest lmbench lua netperf ltp')
    parser.add_argument('--case-timeout', type=int, default=300)
    parser.add_argument('--diagnostic-exclude', default='', help='space-separated LTP basenames, empty by default')
    args = parser.parse_args()
    known = {'basic','busybox','cyclictest','iozone','iperf','libcbench','libctest','lmbench','lua','netperf','ltp'}
    if not args.groups.split() or not set(args.groups.split()) <= known:
        parser.error('unknown evaluation group')
    if not 0 <= args.case_timeout <= 86400:
        parser.error('case timeout must be between 0 and 86400 seconds')
    exclusions = args.diagnostic_exclude.split()
    if any(not re.fullmatch(r'[A-Za-z0-9_.-]+', name) or name in ('.', '..') for name in exclusions):
        parser.error('diagnostic exclusions must be literal LTP basenames')
    facts = json.loads((HERE / 'inputs.json').read_text())['architectures'][args.arch]
    helper = args.case.read_bytes()
    if (len(helper) < 64 or helper[:7] != b'\x7fELF\x02\x01\x01' or
            struct.unpack_from('<H', helper, 18)[0] != facts['elf_machine']):
        parser.error('supervisor machine or ELF64 ABI differs from target')
    payload = ''.join(f'\\0{byte:03o}' for byte in helper)
    startup = (HERE / 'init.sh').read_text()
    links = '\n'.join('$BB ln -s ' + source + ' ' + target
                      for source, target in facts['loader_links'])
    startup = startup.replace('# BOAROS_LOADER_LINKS', links)
    runtime_path = ''
    runtime_startup = ''
    if args.arch == 'loongarch':
        if args.runtime is None:
            parser.error('LA default environment requires the Linux scheduling runtime')
        runtime = args.runtime.read_bytes()
        if (len(runtime) < 64 or runtime[:7] != b'\x7fELF\x02\x01\x01' or
                struct.unpack_from('<HH', runtime, 16) != (3, 258) or
                struct.unpack_from('<I', runtime, 48)[0] & 7 != 3):
            parser.error('scheduling runtime must be a LA LP64D shared object')
        compressed = gzip.compress(runtime, mtime=0)
        encoded = ''.join(f'\\0{byte:03o}' for byte in compressed)
        runtime_path = '/lib/boaros-linux-sched.so'
        runtime_startup = ("$BB cat > /tmp/boaros-runtime.sh <<'BOAROS_RUNTIME_END'\n"
            + (HERE / 'runtime.sh').read_text() + '\nBOAROS_RUNTIME_END\n'
            + f"BOAROS_RUNTIME_GZIP='{encoded}'\n"
            + f'$BB sh /tmp/boaros-runtime.sh {runtime_path} "$BOAROS_RUNTIME_GZIP"')
    elif args.runtime is not None:
        parser.error('LA scheduling runtime cannot be attached to RV')
    startup = startup.replace('# BOAROS_RUNTIME_PAYLOAD', runtime_startup)
    # Decode a build-owned executable with the original BusyBox; no host disk injection.
    startup = startup.replace('# BOAROS_CASE_PAYLOAD',
        f"$BB printf '%b' '{payload}' > /tmp/boaros-case\n$BB chmod 755 /tmp/boaros-case\n"
        "$BB cat > /tmp/boaros-libctest-hook.sh <<'BOAROS_LIBCTEST_HOOK_END'\n"
        + (HERE / 'libctest-hook.sh').read_text() + '\nBOAROS_LIBCTEST_HOOK_END\n'
        "$BB cat > /tmp/boaros-ltp-hook.sh <<'BOAROS_LTP_HOOK_END'\n"
        + (HERE / 'ltp-hook.sh').read_text() + '\nBOAROS_LTP_HOOK_END\n'
        + "$BB cat > /tmp/boaros-ltp-case.sh <<'BOAROS_LTP_CASE_END'\n"
        + (HERE / 'ltp-case.sh').read_text() + '\nBOAROS_LTP_CASE_END\n'
        + "$BB cat > /tmp/boaros-ltp-skips.tsv <<'BOAROS_LTP_SKIPS_END'\n"
        + (HERE / 'ltp-skips.tsv').read_text() + '\nBOAROS_LTP_SKIPS_END')
    profile = {'path': '/musl/busybox', 'argv': ['/musl/busybox', 'sh', '-c', startup],
               'envp': ['PATH=/musl','HOME=/','TERM=vt100',
                        'BOAROS_EVAL_ARCH=' + args.arch,
                        'BOAROS_EVAL_GROUPS=' + args.groups,
                        'BOAROS_LINUX_SCHED_PRELOAD=' + runtime_path,
                        'BOAROS_LTP_CASE_TIMEOUT=' + str(args.case_timeout),
                        'BOAROS_DIAGNOSTIC_EXCLUDE=' + ' '.join(exclusions)]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    value = json.dumps(profile, ensure_ascii=False, indent=2) + '\n'
    if not args.output.exists() or args.output.read_text() != value:
        args.output.write_text(value)


if __name__ == '__main__':
    main()
