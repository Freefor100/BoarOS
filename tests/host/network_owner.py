#!/usr/bin/env python3
"""Normal NIC capacity and heap socket/PCB lifetime contracts; no timer retry shortcut."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
cases = ('unix-peer-identity', 'async-events', 'ipv4-address-shutdown', 'ipv4-route', 'unix-shutdown-state', 'unix-seq-records', 'unix-seq-control', 'unix-seq-reservation', 'unix-seq-pending-control', 'unix-late-source', 'unix-pending-reset', 'unix-control-cleanup', 'unix-control-owner', 'unix-packet-boundary', 'unix-write-notify', 'unix-stream-state', 'unix-space-topology', 'unix-dgram-shutdown', 'unix-dgram-reconnect', 'unix-listener-lookup-close', 'unix-names', 'unix-peer-queued', 'unix-peer-accepted', 'unix-autobind-collision', 'notify-stop', 'stop-wait', 'remote-udp-stop', 'remote-copy-stop', 'remote-udp', 'remote-udp-fault', 'remote-protocol', 'remote-batch', 'remote-batch-ack', 'remote-turn', 'concurrent-reservation', 'endpoint-owner', 'protocol-owner', 'protocol-transaction', 'receive-prefix', 'receive-chains', 'nodelay', 'minshall', 'nodelay-capacity', 'tcp-batch', 'pool-release-owner', 'tcp-batch-last', 'tcp-batch-abort', 'tcp-batch-error', 'unix-payload', 'receive-payload', 'udp-payload', 'copy-indirect', 'copy-ineligible', 'timewait-capacity', 'timewait-timer', 'controls','unix-sender','loopback-work')
cases += ('tcp-quota-notify', 'remote-quota-notify', 'remote-batch-writers', 'remote-direct', 'queued-completion')
cases += ('internet-checksum', 'tcp-loopback-checksum', 'tcp-detached', 'tcp-input-output', 'tcp-reset-receive-retry', 'raw-tcp-ingress')
cases += ('tcp-more', 'tcp-cork', 'tcp-receive-before-wait', 'tcp-close-capacity-retry', 'tcp-passive-capacity-retry',
          'tcp-ack-batch', 'serial-tcp-credit', 'tcp-serial-train')
cases += ('ipv6-mld', 'ipv6-ra', 'ipv6-source-prefix', 'ipv6-global', 'ipv6-prepare-withdraw', 'ipv6-linklocal', 'raw-icmp-owner', 'raw-icmp-close', 'ipv4-options', 'ipv4-igmp', 'packet-tap', 'packet-ring')
smp_cases = ('parallel-receive-retire', 'parallel-receive-reset', 'udp-caller-submit', 'udp-caller-stop',
             'tcp-owner-lookup', 'tcp-owner-train', 'tcp-owner-placement', 'tcp-physical-submit', 'tcp-resolved-output',
             'tcp-single-cpu-service', 'tcp-serial-resolved-output', 'tcp-shard-receive-before-wait')
parser.add_argument('--case', choices=(*cases, *smp_cases, 'all'), action='append', help='repeat to share one host build; default: all')
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
    program = work / 'owner'
    command = ['cc', '-std=c11', '-pthread', '-DBOAROS_HOST_IRQ_STATE', '-DBOAROS_HOST_IRQ_COUNTS', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-DBOAROS_MAX_CPUS=2', f'-DBOAROS_PAGE_SHIFT={args.page_shift}', '-DBOAROS_COST_DIAGNOSTICS=1', '-Itests/host/random', '-idirafter', 'include',
               '-idirafter', 'net/lwip_port/include', '-Ithird_party/lwip/src/include', '-I' + str(work),
               'tests/host/network_owner.c','kernel/raw_lock.c','kernel/physical_page.c','net/packet_ring.c','tests/host/cpu_context.c','-Wl,--wrap=netif_get_by_index,--wrap=kernel_raw_lock_acquire,--wrap=kernel_bpf_program_get,--wrap=kernel_bpf_program_put,--wrap=kernel_bpf_filter', *sources, '-o', str(program)]
    if args.sanitize: command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    selected = (*cases, *smp_cases) if not args.case or 'all' in args.case else args.case
    ordinary = [case for case in selected if case not in smp_cases]
    if ordinary:
        subprocess.run(command, cwd=ROOT, check=True, timeout=60)
        for case in ordinary:
            subprocess.run([str(program), case], check=True, timeout=20)
    if any(case in smp_cases for case in selected):
        command[command.index('-DBOAROS_MAX_CPUS=2')] = '-DBOAROS_MAX_CPUS=4'
        subprocess.run(command + ['-DBOAROS_USER_SMP=1'], cwd=ROOT, check=True, timeout=60)
        for case in selected:
            if case in smp_cases:
                subprocess.run([str(program), case], check=True, timeout=20)
