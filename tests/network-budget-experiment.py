#!/usr/bin/env python3
r"""Compile bounded TCP candidates and run serialized, matched independent boots.

Examples (run only after coordinating exclusive access to the measurement host):
  python3 -B tests/network-budget-experiment.py profiles --compile
  python3 -B tests/network-budget-experiment.py build --profiles w8-p1-m1,w16-p2-m2
  python3 -B tests/network-budget-experiment.py run --variant current=build/network-budget/kernels/w8-p1-m1/kernel-rv \
      --variant w16-p2-m2=build/network-budget/kernels/w16-p2-m2/kernel-rv \
      --cases loopback:blocking:bulk:1:rx,tap:nonblocking:bulk:5:tx --repeat 3
  python3 -B tests/network-budget-experiment.py smoke --variant current=kernel-rv \
      --cases tap:nonblocking:mixed:5:tx --tap-delay-ms 0

The run command includes the original fixed baseline unless --observe is used.
Each case is a fresh boot and disk copy; TAP also gets a fresh network namespace.
A partial case/profile selection is reported exactly, never as the full matrix.
Mixed cases add one independent RR control connection; near reduces bulk count
to keep four active PCB slots free. All means 60 cases. TAP delay is one-way
host-to-guest egress netem; 1/10 ms require host kernel support, with no relay or
module-loading fallback. Smoke output is functional evidence, never performance.
Formal candidates require a matching build manifest bound into the ELF. Generic
names are display aliases; canonical profile names must match that manifest.
"""
import argparse
import concurrent.futures
import hashlib
import importlib.util
import itertools
import json
import math
import os
from pathlib import Path
import queue
import re
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import budget_identity as contracts
BASELINE = ROOT / 'build/data-path-baselines/5687377/kernel-rv'
BASELINE_SHA = 'b962c890989d268282b55d49a4fa9b51afbaf7008dab2fc47e717388a6fea2f4'
BASELINE_COMMIT = '5687377c58dc96adfa1f72f1636a319bbd25b419'
OUT = ROOT / 'build/network-budget'
PORT = 18090
RTT_SAMPLES = 16
sys.path.insert(0, str(ROOT / 'tests/diff-abi'))
import harness


class NetworkSetupError(RuntimeError):
    def __init__(self, command, result):
        self.details = {'command': command, 'returncode': result.returncode,
                        'stdout': result.stdout, 'stderr': result.stderr}
        super().__init__('isolated TAP delay is unsupported: ' + result.stderr.strip())


def output(command):
    return subprocess.check_output(command, cwd=ROOT, text=True).strip()


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def cross_prefix():
    prefix = os.environ.get('CROSS_COMPILE')
    if prefix:
        return prefix
    for prefix in ('riscv64-unknown-elf-', 'riscv64-elf-'):
        if shutil.which(prefix + 'gcc'):
            return prefix
    raise RuntimeError('RISC-V bare-metal compiler unavailable')


def profile(name):
    match = re.fullmatch(r'w(8|16|32)-p(1|2|4)-m(1|2|4)', name)
    if not match:
        raise ValueError('profile must be w{8,16,32}-p{1,2,4}-m{1,2,4}: ' + name)
    window, pools, memory = map(int, match.groups())
    return {'name': name, 'window_mss': window, 'pool_scale': pools, 'mem_scale': memory,
            'tcp_mss': 1460, 'tcp_window_bytes': window * 1460,
            'tcp_send_bytes': window * 1460, 'tcp_send_queue_pbufs': window * 4,
            'tcp_segments': 128 * pools, 'pbuf_headers': 64 * pools,
            'pbuf_pool_count': 64 * pools, 'pbuf_pool_payload_bytes': 1536,
            'protocol_heap_bytes': 262144 * memory,
            'tcp_active_pcbs': 32, 'tcp_listen_pcbs': 16, 'udp_pcbs': 16,
            'fragment_pbuf_headers': 15, 'ipv4_reassembly_pbuf_limit': 48,
            'ipv4_reassembly_records': 8, 'ipv6_neighbor_queue_headers': 20,
            'udp_control_heap_headroom_bytes': 65536 + 4096,
            'udp_control_pbuf_headroom_count': 16,
            'near_pool_loopback_connections': 14, 'near_pool_tap_connections': 28,
            'near_pool_unused_active_pcbs': 4,
            'headroom_scope': 'Existing UDP admission headroom remains fixed; TCP has no hard per-flow segment or heap reservation. Four PCB slots are workload headroom, not a production admission guarantee.'}


def names(value):
    if value == 'all':
        return [f'w{w}-p{p}-m{m}' for w, p, m in itertools.product((8, 16, 32), (1, 2, 4), (1, 2, 4))]
    result = value.split(',')
    for name in result:
        profile(name)
    if len(set(result)) != len(result):
        raise ValueError('duplicate profile')
    return result


def definitions(row):
    return {'BOAROS_LWIP_WINDOW_MSS': row['window_mss'],
            'BOAROS_LWIP_POOL_SCALE': row['pool_scale'],
            'BOAROS_LWIP_MEM_SCALE': row['mem_scale']}


def flags(row):
    return [f'-D{name}={value}' for name, value in definitions(row).items()]


def storage_symbols(path):
    values = {}
    for line in output([cross_prefix() + 'nm', '-S', '--defined-only', str(path)]).splitlines():
        words = line.split()
        if len(words) == 4 and (words[3] == 'ram_heap' or words[3].startswith('memp_memory_')):
            values[words[3]] = int(words[1], 16)
    return values


def compile_storage(row):
    work = OUT / 'static' / row['name']
    work.mkdir(parents=True, exist_ok=True)
    symbols, statistics_bytes = {}, {}
    for unit, diagnostics in (('mem', 0), ('memp', 0), ('stats', 0), ('memp', 1), ('stats', 1)):
        obj = work / (unit + ('-observe' if diagnostics else '') + '.o')
        command = [cross_prefix() + 'gcc', '-march=rv64imac_zicsr_zifencei', '-mabi=lp64',
                   '-std=gnu11', '-O2', '-ffreestanding', '-Iinclude', '-Inet/lwip_port/include',
                   '-Ithird_party/lwip/src/include', *flags(row),
                   f'-DBOAROS_COST_DIAGNOSTICS={diagnostics}', '-c',
                   f'third_party/lwip/src/core/{unit}.c', '-o', str(obj)]
        subprocess.run(command, cwd=ROOT, check=True)
        if not diagnostics:
            symbols.update(storage_symbols(obj))
        for line in output([cross_prefix() + 'nm', '-S', '--defined-only', str(obj)]).splitlines():
            fields = line.split()
            if len(fields) == 4 and (fields[3] == 'lwip_stats' or fields[3].startswith('memp_stats_')):
                statistics_bytes[diagnostics] = statistics_bytes.get(diagnostics, 0) + int(fields[1], 16)
    return {'symbols': symbols, 'protocol_storage_array_bytes': sum(symbols.values()),
            'release_lwip_statistics_bytes': statistics_bytes[0],
            'diagnostic_lwip_statistics_bytes': statistics_bytes[1],
            'diagnostic_lwip_statistics_added_bytes': statistics_bytes[1] - statistics_bytes[0],
            'statistics_scope': 'lwIP statistics structures only; excludes the global cost recorder, its code and socket snapshot formatting.',
            'scope': 'Exact RV64 ELF array sizes including alignment padding; excludes protocol globals, socket/kernel heap objects, NIC rings and user memory.'}


def source_identity():
    return contracts.source_identity(ROOT)


def build(args):
    frozen_source = source_identity()
    compiler = contracts.compiler_identity(cross_prefix() + 'gcc', ROOT)
    for name in names(args.profiles):
        contracts.require_source(frozen_source, source_identity())
        row = profile(name)
        work = OUT / 'kernels' / (name + ('-observe' if args.observe else ''))
        work.mkdir(parents=True, exist_ok=True)
        command = ['make', '-j' + str(args.jobs), 'all', f'BUILD_DIR={work / "objects"}',
                   f'KERNEL_RV={work / "kernel-rv"}', f'COST_DIAGNOSTICS={int(args.observe)}',
                   'CFLAGS_EXTRA=' + ' '.join(flags(row))]
        effective = contracts.verify_macros(compiler['path'], ROOT, definitions(row), args.observe,
                    'lwip/opt.h', ['-Iinclude', '-Inet/lwip_port/include', '-Ithird_party/lwip/src/include'])
        core = contracts.build_identity('tcp', row, definitions(row), args.observe, frozen_source, command, compiler, effective)
        executed = command if contracts.cache_matches(work, core, cross_prefix() + 'objcopy') else command + ['-B']
        with (work / 'build.log').open('w') as log:
            subprocess.run(executed, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        contracts.require_source(frozen_source, source_identity())
        if contracts.compiler_identity(cross_prefix() + 'gcc', ROOT) != compiler:
            raise RuntimeError('compiler changed during candidate build batch')
        manifest = contracts.seal_kernel(work / 'kernel-rv', core, cross_prefix() + 'objcopy')
        symbols = storage_symbols(work / 'kernel-rv')
        save(work / 'identity.json', manifest | {'executed_command': executed,
                                    'protocol_storage_arrays': symbols,
                                    'protocol_storage_array_bytes': sum(symbols.values()),
                                    'elf_size': output([cross_prefix() + 'size', str(work / 'kernel-rv')])})
        print(name, work / 'kernel-rv', flush=True)


def parse_case(value, size, rounds):
    parts = value.split(':')
    if len(parts) != 5:
        raise ValueError('case needs path:mode:kind:connections:direction: ' + value)
    path, mode, kind, count, direction = parts
    if path not in ('loopback', 'tap') or mode not in ('blocking', 'nonblocking') or kind not in ('rr', 'bulk', 'mixed') or direction not in ('rx', 'tx'):
        raise ValueError('invalid case ' + value)
    limit = 14 if path == 'loopback' else 28
    bulk_connections = limit - (kind == 'mixed') if count == 'near' else int(count)
    connections = bulk_connections + (kind == 'mixed')
    if not 1 <= connections <= (14 if path == 'loopback' else 28):
        raise ValueError('case exceeds PCB headroom ' + value)
    if kind == 'rr' and direction != 'rx':
        raise ValueError('RR uses direction rx (request to guest, echo to client)')
    return dict(name=value, path=path, mode=mode, kind=kind, connections=connections,
                bulk_connections=bulk_connections if kind != 'rr' else 0,
                control_id=connections - 1 if kind == 'mixed' else None,
                bytes=size, rounds=rounds, direction=direction)


def case_values(value):
    if value != 'all':
        return value.split(',')
    result = []
    for path, mode, count in itertools.product(('loopback', 'tap'), ('blocking', 'nonblocking'), ('1', '5', 'near')):
        result.append(':'.join((path, mode, 'rr', count, 'rx')))
        for direction in ('rx', 'tx'):
            result.append(':'.join((path, mode, 'bulk', count, direction)))
            result.append(':'.join((path, mode, 'mixed', count, direction)))
    return result


def fixture(work, program, case, observe=False):
    tree = work / 'tree'
    tree.mkdir()
    shutil.copyfile(program, tree / 'init')
    (tree / 'init').chmod(0o755)
    for name in ('dev', 'proc', 'tmp'):
        (tree / name).mkdir()
    # Same disk and ELF for Linux and every BoarOS kernel; argv selects Linux address setup.
    (tree / 'budget-config').write_text('{path} {mode} {kind} {connections} {bytes} {rounds} {direction}\n'.format(**case))
    if observe:
        (tree / 'observe').touch()
    image = work / 'fixture.img'
    with image.open('wb') as stream:
        stream.truncate(64 * 1024 * 1024)
    harness.run_logged(['mkfs.ext4', '-q', '-F', '-b', '4096', '-d', str(tree), str(image)], work / 'mkfs.log')
    commands = work / 'devices.debugfs'
    commands.write_text('cd /dev\nmknod console c 5 1\nset_inode_field console mode 020600\n')
    harness.run_logged(['debugfs', '-w', '-f', str(commands), str(image)], work / 'debugfs.log')
    return image


def receive(sock, count):
    parts = bytearray()
    while len(parts) < count:
        part = sock.recv(count - len(parts))
        if not part:
            raise RuntimeError('unexpected TCP EOF')
        parts.extend(part)
    return bytes(parts)


def pattern(identifier, size, offset=0):
    first = (offset + 17 * identifier) % 251
    base = bytes(range(251))
    tile = base[first:] + base[:first]
    return (tile * ((size + 250) // 251))[:size]


def host_flow(identifier, case, gate, address='10.77.0.2'):
    with socket.create_connection((address, PORT), timeout=30) as stream:
        stream.settimeout(120)
        stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        gate.wait(timeout=30)
        stream.sendall(struct.pack('!II', 0x424f4152, identifier))
        rtt = []
        for index in range(RTT_SAMPLES):
            ping = pattern(identifier, 64, index * 64)
            start = time.monotonic_ns()
            stream.sendall(ping)
            if receive(stream, 64) != ping:
                raise RuntimeError('RTT content mismatch')
            rtt.append(time.monotonic_ns() - start)
        gate.wait(timeout=30)
        stream.sendall(b'G')
        start = time.monotonic_ns()
        bulk = is_bulk(case, identifier)
        samples = []
        if bulk:
            for offset in range(0, case['bytes'], 65536):
                size = min(65536, case['bytes'] - offset)
                data = pattern(identifier, size, offset)
                if case['direction'] == 'rx':
                    stream.sendall(data)
                elif receive(stream, size) != data:
                    raise RuntimeError('bulk receiver content mismatch')
        else:
            for index in range(case['rounds']):
                ping = pattern(identifier, 64, index * 64)
                rr_start = time.monotonic_ns()
                stream.sendall(ping)
                if receive(stream, 64) != ping:
                    raise RuntimeError('RR content mismatch')
                if identifier == case['control_id']:
                    samples.append(dict(id=identifier, sequence=index, start_ns=rr_start, end_ns=time.monotonic_ns()))
        if bulk and case['direction'] == 'tx':
            if stream.recv(1):
                raise RuntimeError('unexpected trailing bulk content')
            stream.sendall(struct.pack('!I', 0x444f4e45))
            stream.shutdown(socket.SHUT_WR)
        else:
            stream.shutdown(socket.SHUT_WR)
            if receive(stream, 4) != struct.pack('!I', 0x444f4e45) or stream.recv(1):
                raise RuntimeError('receiver completion acknowledgement missing')
        end = time.monotonic_ns()
        rtt.sort()
        return dict(id=identifier, receiver_bytes=case['bytes'] if bulk else case['rounds'] * 64,
                    start_ns=start, end_ns=end, elapsed_ns=end-start, complete=1,
                    control_samples=samples,
                    rtt_min_ns=rtt[0], rtt_p50_ns=rtt[8], rtt_p95_ns=rtt[15], rtt_max_ns=rtt[15])


def parse_records(raw, tag):
    return [{key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', line)}
            for line in raw.splitlines() if line.startswith('BUDGET ' + tag + ' ')]

def is_bulk(case, identifier):
    return case['kind'] != 'rr' and identifier != case['control_id']


def receiver_bytes(case, identifier):
    return case['bytes'] if is_bulk(case, identifier) else case['rounds'] * 64


def control_metrics(records, samples, case):
    if case['kind'] != 'mixed':
        return {}
    if len(samples) != case['rounds'] or {s.get('sequence') for s in samples} != set(range(case['rounds'])):
        raise RuntimeError('missing/duplicate control RR samples')
    control = next(r for r in records if r['id'] == case['control_id'])
    for sample in samples:
        if sample.get('id') != case['control_id'] or not control['start_ns'] <= sample['start_ns'] < sample['end_ns'] <= control['end_ns']:
            raise RuntimeError('invalid control RR sample identity/time')
    bulk = [r for r in records if is_bulk(case, r['id'])]
    during_bulk = [s for s in samples if any(r['start_ns'] <= s['start_ns'] < r['end_ns'] for r in bulk)]
    fully_contained = [s for s in samples if any(r['start_ns'] <= s['start_ns'] and s['end_ns'] <= r['end_ns'] for r in bulk)]
    if not during_bulk:
        raise RuntimeError('no control RR request started while bulk was active; increase bulk bytes')
    result = {'control_completed_requests': len(samples), 'control_requests_during_bulk': len(during_bulk),
              'control_requests_fully_contained': len(fully_contained),
              'control_received_bytes': control['receiver_bytes'],
              'control_tail_scope': 'Primary during_bulk tail includes every RR started during any bulk interval, including replies after bulk ends; fully_contained is auxiliary.'}
    for label, values in (('all', samples), ('during_bulk', during_bulk), ('fully_contained', fully_contained)):
        durations = sorted(s['end_ns'] - s['start_ns'] for s in values)
        for name, fraction in (('p50', .5), ('p95', .95), ('p99', .99), ('max', 1.0)):
            result[f'control_rr_{label}_{name}_ns'] = durations[math.ceil(len(durations) * fraction) - 1] if durations else None
    return result


def metrics(records, case):
    if len(records) != case['connections'] or {r.get('id') for r in records} != set(range(case['connections'])):
        raise RuntimeError('missing, duplicate, or wrong connection identity')
    for row in records:
        if row.get('complete') != 1 or row.get('receiver_bytes') != receiver_bytes(case, row['id']) or row.get('elapsed_ns', 0) <= 0:
            raise RuntimeError('connection did not complete verified receiver bytes')
        if row['end_ns'] - row['start_ns'] != row['elapsed_ns']:
            raise RuntimeError('inconsistent connection times')
    # Mixed-case bulk throughput/fairness excludes the independent RR control stream.
    records = [r for r in records if case['kind'] != 'mixed' or is_bulk(case, r['id'])]
    elapsed = max(r['end_ns'] for r in records) - min(r['start_ns'] for r in records)
    rates = [r['receiver_bytes'] * 8e3 / r['elapsed_ns'] for r in records]
    return {'receiver_bytes': sum(r['receiver_bytes'] for r in records),
            'elapsed_ns': elapsed, 'receiver_mbit_s': sum(r['receiver_bytes'] for r in records) * 8e3 / elapsed,
            'transactions_s': case['rounds'] * case['connections'] * 1e9 / elapsed if case['kind'] == 'rr' else None,
            'connection_mbit_s_min': min(rates), 'connection_mbit_s_max': max(rates),
            'jain_completion_rate': sum(rates) ** 2 / (len(rates) * sum(r * r for r in rates)),
            'completion_span_ns': max(r['end_ns'] for r in records) - min(r['end_ns'] for r in records),
            'slowest_connection_ns': max(r['elapsed_ns'] for r in records),
            'rtt_p50_ns_median': statistics.median(r['rtt_p50_ns'] for r in records),
            'rtt_max_ns': max(r['rtt_max_ns'] for r in records),
            'fairness_scope': 'Finite equal-byte bulk completion; primary mixed control tail covers requests started during bulk, including replies after bulk ends. This does not prove long-running absence of starvation.'}


def tap(delay_ms):
    spec = importlib.util.spec_from_file_location('network_external', ROOT / 'tests/network-external.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    fd = module.tap()
    settings = {'delay_ms': delay_ms, 'direction': 'host-to-guest TAP egress only',
                'scope': 'Private network namespace, boar-tap only; guest-to-host ingress is unshaped.',
                'tc_version': output(['tc', '-V'])}
    try:
        if delay_ms:
            command = ['tc', 'qdisc', 'add', 'dev', 'boar-tap', 'root', 'netem',
                       'delay', f'{delay_ms:g}ms', 'limit', '4096']
            result = subprocess.run(command, text=True, capture_output=True)
            if result.returncode:
                raise NetworkSetupError(command, result)
            settings['command'] = command
        settings['before'] = json.loads(output(['tc', '-s', '-j', 'qdisc', 'show', 'dev', 'boar-tap']))
    except Exception:
        os.close(fd)
        raise
    return fd, settings


def run_one(specfile):
    spec = json.loads(specfile.read_text())
    work, kernel, image = map(Path, (spec['work'], spec['kernel'], spec['fixture']))
    case = spec['case']
    disk = work / 'disk.img'
    shutil.copyfile(image, disk)
    fd = None
    qemu = spec['qemu']
    command = [qemu, '-machine', 'virt', '-bios', 'default', '-kernel', str(kernel),
               '-global', 'virtio-mmio.force-legacy=' + ('true' if spec['transport'] == 'legacy' else 'false'),
               '-m', spec['ram'], '-smp', '1', '-nographic', '-no-reboot',
               '-drive', f'file={disk},if=none,format=raw,id=root,cache=writeback',
               '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
    if spec.get('reference'):
        command += ['-append', 'root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1 -- /budget-config reference']
    row = dict(case=case, variant=spec['variant'], repetition=spec['repetition'],
               kernel_sha256=digest(kernel), program_sha256=spec['program_sha256'],
               fixture_sha256=digest(image), command=command, observation_enabled=spec['observe'])
    row['evidence_kind'] = spec.get('evidence_kind', 'performance')
    events, lines = queue.Queue(), []
    start = time.monotonic_ns()
    process = None
    try:
        if case['path'] == 'tap':
            fd, row['netem'] = tap(spec['tap_delay_ms'])
            command += ['-netdev', f'tap,id=host,fd={fd},vhost=off', '-device',
                        'virtio-net-device,netdev=host,mac=52:54:00:12:34:56,bus=virtio-mmio-bus.1']
        with (work / 'console.log').open('w') as log:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, bufsize=1,
                                       pass_fds=(fd,) if fd is not None else ())
            def reader():
                for line in process.stdout:
                    log.write(line); log.flush(); lines.append(line)
                    if 'BUDGET READY tap' in line:
                        events.put('BUDGET READY tap')
                events.put(None)
            thread = threading.Thread(target=reader, daemon=True)
            thread.start()
            if fd is not None:
                if events.get(timeout=90) != 'BUDGET READY tap':
                    raise RuntimeError('TAP listener did not become ready')
                gate = threading.Barrier(case['connections'])
                with concurrent.futures.ThreadPoolExecutor(case['connections']) as pool:
                    records = list(pool.map(lambda i: host_flow(i, case, gate), range(case['connections'])))
            process.wait(timeout=200)
            thread.join(timeout=5)
            raw = ''.join(lines)
            if process.returncode:
                raise RuntimeError('guest success marker or clean QEMU exit missing')
            if not spec.get('reference') and ('heap-live=0x0; shutting down' not in raw or 'exited status=0x0 ' not in raw):
                raise RuntimeError('guest success marker, clean exit, or heap teardown missing')
            # A committed Linux ext4 journal need not yet be checkpointed when
            # PID 1 exits. Replay only a readout copy; preserve the original disk.
            readout = work / 'readout.img'
            shutil.copyfile(disk, readout)
            replay = ['e2fsck', '-p', '-E', 'journal_only', str(readout)]
            with (work / 'readout-journal.log').open('w') as log:
                replay_result = subprocess.run(replay, stdout=log, stderr=subprocess.STDOUT)
            row['result_readout'] = {'method': 'journal replay on a copy, then debugfs',
                                     'command': replay, 'returncode': replay_result.returncode,
                                     'original_disk_sha256': digest(disk), 'readout_disk_sha256': digest(readout)}
            if replay_result.returncode not in (0, 1):
                raise RuntimeError('result disk journal replay failed')
            result_file = work / 'guest-results.txt'
            harness.run_logged(['debugfs', '-R', f'dump /budget-config.results {result_file}', str(readout)], work / 'extract-results.log')
            evidence = result_file.read_text()
            if not evidence.endswith('BUDGET PASS all\n'):
                raise RuntimeError('durable guest result stream missing or incomplete')
            row['guest_results_sha256'] = digest(result_file)
            if fd is None:
                records = parse_records(evidence, 'CONNECTION')
            server_records = parse_records(evidence, 'SERVER')
            if len(server_records) != case['connections'] or {r.get('id') for r in server_records} != set(range(case['connections'])) or any(r.get('receiver_bytes') != receiver_bytes(case, r['id']) or r.get('complete') != 1 for r in server_records):
                raise RuntimeError('guest receiver/sender completion missing')
            row.update(status='passed', connections=records, servers=server_records, metrics=metrics(records, case))
            samples = parse_records(evidence, 'CONTROL_SAMPLE') if fd is None else [sample for r in records for sample in r.get('control_samples', [])]
            row['control_samples'] = samples
            row['metrics'].update(control_metrics(records, samples, case))
            row['protocol_snapshots'] = {name: {k: int(v) for k, v in re.findall(r'^(\w+)=(\d+)$', body, re.M)}
                                         for name, body in re.findall(r'^BUDGET PROTOCOL (before|after)\n(.*?)^BUDGET PROTOCOL END$', evidence, re.M | re.S)}
            if spec['observe'] and set(row['protocol_snapshots']) != {'before', 'after'}:
                raise RuntimeError('diagnostic protocol snapshots missing')
            if spec['observe']:
                peak = re.search(r'^BUDGET MEM_PEAK BEGIN\n(.*?)^BUDGET MEM_PEAK END$', evidence, re.M | re.S)
                row['managed_memory_peaks'] = {key:int(value) for key,value in re.findall(r'^(\w+)=(\d+)$',peak.group(1),re.M)} if peak else None
                if not row['managed_memory_peaks'] or any(row['managed_memory_peaks'][key+'_peak_bytes'] < row['managed_memory_peaks'][key+'_current_bytes'] for key in ('managed','heap')):
                    raise RuntimeError('missing or inconsistent managed memory high watermarks')
                row['managed_memory_scope'] = 'Boot-to-snapshot managed allocator/heap high watermarks, before cost formatting. Includes runtime/cache/user pages, excludes reserved kernel image/firmware/MMIO. Heap is a subset; do not sum peaks or infer workload-only peaks.'
                cost = re.search(r'^BUDGET COST BEGIN\n(.*?)^BUDGET COST END$', evidence, re.M | re.S)
                if not cost: raise RuntimeError('missing cost counters')
                sys.path.insert(0,str(ROOT/'tests'))
                import cost_report
                for schema in cost_report.supported_schemas():
                    try: row['cost'] = cost_report.parse(cost.group(1),1,schema); break
                    except ValueError: pass
                else: raise RuntimeError('invalid cost snapshot')
                after = row['protocol_snapshots']['after']
                row['memory_peaks'] = {key: after.get(key) for key in (
                    'heap_max', 'tcp_segment_peak', 'pbuf_peak', 'pbuf_pool_peak', 'tcp_active_peak', 'tcp_listen_peak')}
                row['memory_peak_scope'] = 'lwIP boot-to-snapshot high watermarks; begin/end cost observation does not reset these maxima. Driver and kernel heap owners are reported separately.'
                row['counter_delta_policy'] = 'Raw counters only; an unbounded 16-bit interval has no exact packet delta. Missing legacy peak fields remain null.'
            final = re.search(r'BoarOS: network final (.*)', raw)
            row['driver_statistics'] = {k: int(v) for k, v in re.findall(r'([\w-]+)=(\d+)', final.group(1))} if final else {}
            row['driver_statistics_scope'] = 'Device lifetime, including setup and teardown. DONE is software used-ring harvest, not hardware completion. FREE-to-post is same-slot reuse including idle time; it is not ready-only scheduling delay. First use and reset-only cancellation are excluded from completion samples.'
            if row['driver_statistics'].get('tx-latency-overflow', 0):
                raise RuntimeError('TX latency totals overflowed')
            if row['driver_statistics'].get('errors', 0):
                raise RuntimeError('network device error counter nonzero')
            if fd is not None:
                row['netem']['after'] = json.loads(output(['tc', '-s', '-j', 'qdisc', 'show', 'dev', 'boar-tap']))
                if any(q.get('drops', 0) for q in row['netem']['after']):
                    raise RuntimeError('delay-only netem queue dropped packets; queue bound affected the experiment')
            disk.unlink()
            readout.unlink()
    except Exception as error:
        row.update(status='unsupported' if isinstance(error, NetworkSetupError) else 'failed', error=str(error))
        if isinstance(error, NetworkSetupError):
            row['network_setup_failure'] = error.details
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
        if fd is not None:
            os.close(fd)
        row['boot_elapsed_ns'] = time.monotonic_ns() - start
        save(work / 'result.json', row)
    print(spec['variant'], case['name'], spec['repetition'], row['status'], flush=True)
    return 0 if row['status'] == 'passed' else 1


def summarize(work):
    identity = json.loads((work / 'identity.json').read_text())
    contracts.validate_variants(identity['variants'], 'tcp', profile, definitions, identity['observe'],
        identity.get('smoke', False), BASELINE_SHA, BASELINE_COMMIT, identity.get('linux_identity'))
    rows = [json.loads(path.read_text()) for path in sorted(work.glob('runs/*/result.json'))]
    groups, fixtures, repetitions = {}, {}, set()
    cases = {case['name']: case for case in identity['cases']}
    for row in rows:
        variant, case = row['variant'], row['case']['name']
        if variant not in identity['variants'] or row['case'] != cases.get(case):
            raise RuntimeError('unexpected variant/case in experiment results')
        if (row['kernel_sha256'] != identity['variants'][variant]['kernel_sha256'] or
                row['program_sha256'] != identity['program_sha256'] or
                row['observation_enabled'] != identity['observe']):
            raise RuntimeError('mismatched kernel, ELF or observation identity')
        if fixtures.setdefault(case, row['fixture_sha256']) != row['fixture_sha256']:
            raise RuntimeError('mismatched fixture identity for same case')
        repetition = row['repetition']
        key = (variant, case, repetition)
        if not 1 <= repetition <= identity['repeat'] or key in repetitions:
            raise RuntimeError('duplicate or unexpected boot repetition')
        repetitions.add(key)
        groups.setdefault((row['variant'], row['case']['name']), []).append(row)
    summary = []
    for (variant, case), values in groups.items():
        good = [v for v in values if v['status'] == 'passed']
        item = {'variant': variant, 'case': case, 'boots': len(values), 'passed_boots': len(good),
                'status': 'smoke_passed' if identity.get('smoke') and len(good) == identity['repeat'] else 'measured' if len(good) >= 3 and not identity['observe'] and len(good) == identity['repeat'] else 'diagnostic' if identity['observe'] and len(good) == identity['repeat'] else 'incomplete',
                'metrics': {}}
        if good and not identity.get('smoke'):
            for key, value in good[0]['metrics'].items():
                if value is None or isinstance(value, (int, float)):
                    samples = [v['metrics'][key] for v in good]
                    defined = all(isinstance(v, (int, float)) for v in samples)
                    item['metrics'][key] = dict(median=statistics.median(samples) if defined else None,
                        minimum=min(samples) if defined else None, maximum=max(samples) if defined else None, samples=samples)
        item['comparison_eligible'] = item['status'] == 'measured'
        summary.append(item)
    expected = len(identity['cases']) * len(identity['variants']) * identity['repeat']
    result = {'identity': identity, 'expected_boots': expected, 'completed_boots': len(rows),
              'status': 'complete' if len(rows) == expected and all(r['status'] == 'passed' for r in rows) else 'incomplete',
              'failed_boots': [dict(variant=r['variant'], case=r['case']['name'], repetition=r['repetition'], error=r.get('error')) for r in rows if r['status'] != 'passed'],
              'coverage': 'Only the explicitly listed variants and cases were run. No unselected cross-product cells are implied.',
              'evidence_kind': identity.get('evidence_kind', 'performance'),
              'comparison_groups': [dict(variant=g['variant'], case=g['case']) for g in summary if g['status'] == 'measured'],
              'comparison_policy': 'Only status=measured groups may support comparisons/recommendations; other metrics are diagnostic subsets.',
              'measurement_limits': 'Guest and Python TAP peer share the host CPU. Content checking, Python scheduling and process startup are included; RTT uses 16 pre-transfer echo samples. Jain values cover finite completion rates, not starvation bounds. Observation runs are separate and must be compared with matching release runs before claiming overhead.',
              'groups': summary}
    save(work / 'summary.json', result)
    return result


def run(args):
    with contracts.measurement_lock(ROOT, enabled=not args.smoke or args.observe):
        return run_locked(args)


def run_locked(args):
    if args.repeat < 3 and not args.observe and not args.smoke:
        raise ValueError('release measurement requires at least three independent boots')
    if args.observe and args.repeat != 1:
        raise ValueError('use one explicit diagnostic boot per case; release distributions are separate')
    if args.smoke and args.repeat != 1:
        raise ValueError('smoke is exactly one functional boot per case; use run for performance')
    variants = {} if args.observe or args.smoke else {'baseline': args.baseline.resolve()}
    if not args.observe and not args.smoke and digest(args.baseline) != BASELINE_SHA:
        raise ValueError('original baseline SHA-256 mismatch')
    metadata = {'baseline': dict(management='fixed-baseline', kernel_sha256=BASELINE_SHA, source_commit=BASELINE_COMMIT)} if variants else {}
    for item in args.variant:
        name, path = item.split('=', 1)
        if not re.fullmatch(r'[A-Za-z0-9_-]+', name) or name in variants or name in ('linux', 'baseline'):
            raise ValueError('invalid/duplicate variant label ' + name)
        variants[name] = Path(path).resolve()
    if not variants:
        raise ValueError('at least one variant required')
    linux_identity = None
    if args.linux_kernel:
        if args.observe or 'linux' in variants:
            raise ValueError('Linux reference requires release mode and reserved linux label')
        variants['linux'], linux_identity = harness.fixed_linux_image(args.linux_kernel)
        metadata['linux'] = dict(management='fixed-linux', kernel_sha256=linux_identity['image_sha256'])
    for name, path in variants.items():
        if name in ('linux', 'baseline'):
            continue
        symbols = output([cross_prefix() + 'nm', str(path)])
        metadata[name] = contracts.load_candidate(name, path, 'tcp', profile, definitions,
            args.observe, args.smoke, cross_prefix() + 'objcopy', symbols)
    contracts.validate_variants(metadata, 'tcp', profile, definitions, args.observe, args.smoke,
                                BASELINE_SHA, BASELINE_COMMIT, linux_identity)
    cases = [parse_case(value, args.bytes, args.rounds) for value in case_values(args.cases)]
    if len({c['name'] for c in cases}) != len(cases):
        raise ValueError('duplicate case')
    work = (args.output or OUT / (('smoke-' if args.smoke else 'run-') + str(time.time_ns()))).resolve()
    work.mkdir(parents=True, exist_ok=False)
    (work / 'runs').mkdir()
    program = work / 'init'
    compiler = ROOT / 'build/riscv/musl-root/bin/musl-gcc'
    subprocess.run([str(compiler), '-fno-link-libatomic', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'tests/workloads/network/tcp-budget.c'), '-o', str(program)], check=True)
    qemu = shutil.which(os.environ.get('QEMU_RISCV64', 'qemu-system-riscv64'))
    if not qemu:
        raise RuntimeError('QEMU unavailable')
    frozen = {}
    for name, path in variants.items():
        target = work / (name + '-kernel')
        shutil.copyfile(path, target)
        if digest(target) != metadata[name]['kernel_sha256']:
            raise RuntimeError('kernel changed between manifest validation and freezing: ' + name)
        frozen[name] = target
    identity = dict(runtime_source=source_identity(), program_sha256=digest(program), workload_source_sha256=digest(ROOT / 'tests/workloads/network/tcp-budget.c'),
                    qemu_version=output([qemu, '--version']), qemu_sha256=digest(qemu), compiler=output([str(compiler), '--version']),
                    variants={name: metadata[name] | {'kernel_sha256': digest(path), 'source_path': str(variants[name]),
                                     'static_storage_arrays': storage_symbols(path) if name != 'linux' else {}} for name, path in frozen.items()},
                    baseline_commit=BASELINE_COMMIT, linux_identity=linux_identity, cases=cases, repeat=args.repeat, observe=args.observe,
                    smoke=args.smoke, evidence_kind='functional-smoke-not-performance' if args.smoke else 'diagnostic' if args.observe else 'performance',
                    tap_delay_ms=args.tap_delay_ms, delay_direction='host-to-guest TAP egress only; loopback unchanged',
                    ram=args.ram, transport=args.transport, cache='fresh boot, fresh filesystem copy; no warmed prior network state',
                    io_mode_scope='Guest stream data/response I/O uses the selected mode. Connect/accept setup and the Python TAP peer are blocking in both modes.',
                    fixed_references={'linux': 'f4cdf7ca9a1fdcca413157df19753f388a5a224e', 'lwip': '77dcd25a72509eb83f72b033d219b1d40cd8eb95'})
    save(work / 'identity.json', identity)
    # Interleave variants per repetition to reduce monotonic host-load drift.
    for index, case in enumerate(cases):
        casework = work / ('case-' + str(index)); casework.mkdir()
        image = fixture(casework, program, case, args.observe)
        for repetition in range(1, args.repeat + 1):
            for name, kernel in frozen.items():
                boot = work / 'runs' / f'{index:03d}-{repetition}-{name}'; boot.mkdir()
                spec = dict(work=str(boot), kernel=str(kernel), fixture=str(image), case=case,
                            qemu=qemu, transport=args.transport, ram=args.ram, variant=name,
                            tap_delay_ms=args.tap_delay_ms, evidence_kind=identity['evidence_kind'],
                            repetition=repetition, observe=args.observe, program_sha256=digest(program), reference=name == 'linux')
                specfile = boot / 'spec.json'; save(specfile, spec)
                command = [sys.executable, '-B', str(Path(__file__).resolve()), '_one', str(specfile)]
                if case['path'] == 'tap':
                    command = ['unshare', '--user', '--map-root-user', '--net', *command]
                result = subprocess.call(command)
                if result:
                    summarize(work)
                    print('incomplete experiment:', work)
                    return result
        image.unlink()
    summarize(work)
    print('experiment:', work)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('profiles', help='list all 27 bounded profiles; optionally obtain exact RV64 static array sizes')
    p.add_argument('--compile', action='store_true')
    p = sub.add_parser('build', help='build kernels without changing production kernel-rv')
    p.add_argument('--profiles', default='all')
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--observe', action='store_true')
    for command in ('run', 'smoke'):
        smoke = command == 'smoke'
        p = sub.add_parser(command, help='one functional boot per case; timings are not performance evidence' if smoke else 'serialized independent-boot measurements; coordinate with other workloads first')
        p.set_defaults(smoke=smoke)
        p.add_argument('--variant', action='append', required=True, help='label=kernel path; repeat for multiple variants')
        p.add_argument('--baseline', type=Path, default=BASELINE)
        p.add_argument('--linux-kernel', type=Path, help='optional fixed Linux Image; same ELF, disk, RAM and devices')
        p.add_argument('--cases', default='loopback:blocking:bulk:1:rx,tap:nonblocking:mixed:1:tx', help='comma separated path:mode:kind:connections:direction, or all; mixed adds one control connection, near keeps four active PCB slots free')
        p.add_argument('--bytes', type=int, default=262144 if smoke else 4 * 1024 * 1024)
        p.add_argument('--rounds', type=int, default=64 if smoke else 1024)
        p.add_argument('--repeat', type=int, default=1 if smoke else 3)
        p.add_argument('--observe', action='store_true')
        p.add_argument('--transport', choices=('legacy', 'modern'), default='modern')
        p.add_argument('--tap-delay-ms', type=float, default=0, help='TAP host-to-guest egress netem delay; 0 disables netem, e.g. 1 or 10; actual echo RTT is recorded')
        p.add_argument('--ram', default='512M')
        p.add_argument('--output', type=Path)
    p = sub.add_parser('summarize')
    p.add_argument('work', type=Path)
    p = sub.add_parser('_one', help=argparse.SUPPRESS)
    p.add_argument('spec', type=Path)
    args = parser.parse_args()
    if args.command == 'profiles':
        rows = [profile(name) for name in names('all')]
        if args.compile:
            for row in rows:
                row['static'] = compile_storage(row)
            OUT.mkdir(parents=True, exist_ok=True)
            save(OUT / 'profiles.json', rows)
        print(json.dumps(rows, indent=2))
    elif args.command == 'build':
        build(args)
    elif args.command in ('run', 'smoke'):
        if args.bytes <= 0 or not 1 <= args.rounds <= 65536:
            parser.error('bytes must be positive; rounds must be in 1..65536')
        if not math.isfinite(args.tap_delay_ms) or args.tap_delay_ms < 0:
            parser.error('TAP delay must be finite and nonnegative')
        return run(args)
    elif args.command == '_one':
        return run_one(args.spec)
    else:
        print(json.dumps(summarize(args.work), indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
