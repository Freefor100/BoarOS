#!/usr/bin/env python3
"""Build the compatibility bootstrap into PID 1's shell command, not the test disk."""
import argparse
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--case', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--groups', default='basic busybox cyclictest iozone iperf libcbench libctest lmbench lua netperf ltp')
    parser.add_argument('--case-timeout', type=int, default=60)
    args = parser.parse_args()
    known = {'basic','busybox','cyclictest','iozone','iperf','libcbench','libctest','lmbench','lua','netperf','ltp'}
    if not args.groups.split() or not set(args.groups.split()) <= known:
        parser.error('unknown evaluation group')
    if not 0 <= args.case_timeout <= 86400:
        parser.error('case timeout must be between 0 and 86400 seconds')
    payload = ''.join(f'\\0{byte:03o}' for byte in args.case.read_bytes())
    startup = (HERE / 'init.sh').read_text()
    # Decode a build-owned executable with the original BusyBox; no host disk injection.
    startup = startup.replace('# BOAROS_CASE_PAYLOAD',
        f"$BB printf '%b' '{payload}' > /tmp/boaros-case\n$BB chmod 755 /tmp/boaros-case\n"
        "$BB cat > /tmp/boaros-ltp-hook.sh <<'BOAROS_LTP_HOOK_END'\n"
        + (HERE / 'ltp-hook.sh').read_text() + '\nBOAROS_LTP_HOOK_END')
    profile = {'path': '/musl/busybox', 'argv': ['/musl/busybox', 'sh', '-c', startup],
               'envp': ['PATH=/musl','HOME=/','TERM=vt100',
                        'BOAROS_EVAL_GROUPS=' + args.groups,
                        'BOAROS_LTP_CASE_TIMEOUT=' + str(args.case_timeout)]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    value = json.dumps(profile, ensure_ascii=False, indent=2) + '\n'
    if not args.output.exists() or args.output.read_text() != value:
        args.output.write_text(value)


if __name__ == '__main__':
    main()
