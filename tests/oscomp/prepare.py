#!/usr/bin/env python3
"""Build the compatibility bootstrap into PID 1's shell command, not the test disk."""
import argparse
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
    inputs = json.loads((HERE / 'inputs.json').read_text())
    facts = inputs['architectures'][args.arch]
    helper = args.case.read_bytes()
    if (len(helper) < 64 or helper[:7] != b'\x7fELF\x02\x01\x01' or
            struct.unpack_from('<H', helper, 18)[0] != facts['elf_machine']):
        parser.error('supervisor machine or ELF64 ABI differs from target')
    startup = (HERE / 'init.sh').read_text()
    links = '\n'.join('$BB ln -s ' + source + ' ' + target
                      for source, target in facts['loader_links'])
    startup = startup.replace('# BOAROS_LOADER_LINKS', links)
    runtime_path = ''
    if args.arch == 'loongarch':
        if args.runtime is None:
            parser.error('LA default environment requires the Linux scheduling runtime')
        runtime = args.runtime.read_bytes()
        if (len(runtime) < 64 or runtime[:7] != b'\x7fELF\x02\x01\x01' or
                struct.unpack_from('<HH', runtime, 16) != (3, 258) or
                struct.unpack_from('<I', runtime, 48)[0] & 7 != 3):
            parser.error('scheduling runtime must be a LA LP64D shared object')
        runtime_path = '/boaros-tools/linux-sched.so'
    elif args.runtime is not None:
        parser.error('LA scheduling runtime cannot be attached to RV')
    devices = '\n'.join(f'$BB mknod -m 600 /dev/{name} b 252 {minor}'
                         for name, minor in [('vda', 0), ('vdb', 16), ('vdb1', 17), ('vdb2', 18), ('vdb3', 19)])
    devices += '\n$BB mknod -m 600 /dev/loop-control c 10 237\n' + '\n'.join(
        f'$BB mknod -m 600 /dev/loop{i} b 7 {i}' for i in range(8))
    startup = startup.replace('# BOAROS_LOCAL_DEVICE_NODES', devices)
    startup = startup.replace('# BOAROS_LOCAL_DISKS',
        '$BB mkdir -p /boaros-tools\n'
        '$BB mount -t ext4 -o ro /dev/vdb1 /boaros-tools\n'
        '/boaros-tools/seed\n'
        'if [ -e /dev/vda2 ] || [ -L /dev/vda2 ]; then\n'
        '    echo "BOAROS-BOOT ERROR /dev/vda2 already exists" >&2; exit 125\n'
        'fi\n'
        '$BB ln -s /dev/vdb2 /dev/vda2\n'
        'install_mkfs() {\n'
        '    if [ -e "$2" ] || [ -L "$2" ]; then\n'
        '        if [ ! -L "$2" ] || [ "$($BB readlink "$2")" != "$BB" ]; then\n'
        '            echo "BOAROS-BOOT ERROR unexpected tool at $2" >&2; return 125\n'
        '        fi\n'
        '        $BB rm "$2"\n'
        '    fi\n'
        '    $BB ln -s "$1" "$2"\n'
        '}\n'
        'install_mkfs /boaros-tools/mke2fs /bin/mke2fs\n' +
        '\n'.join(f'install_mkfs /boaros-tools/mkfs.ext{i} /bin/mkfs.ext{i}' for i in (2, 3, 4)) + '\n'
        'export MKE2FS_CONFIG=/boaros-tools/mke2fs.conf\n'
        'export LTP_DEV=/dev/vdb3 LTP_DEV_FS_TYPE=ext4 LTP_SINGLE_FS_TYPE=ext4')
    # The kernel carries policy; the read-only tools partition carries executable content.
    startup = startup.replace('# BOAROS_CASE_PAYLOAD', '\n'.join(
        f'$BB ln -s /boaros-tools/{source} /tmp/boaros-{target}' for source, target in
        [('case', 'case'), ('ltp-hook.sh', 'ltp-hook.sh'), ('libctest-hook.sh', 'libctest-hook.sh'),
         ('ltp-case.sh', 'ltp-case.sh'), ('ltp-skips.tsv', 'ltp-skips.tsv')]))
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
