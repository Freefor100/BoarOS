#!/usr/bin/env python3
"""Build and install the fixed SMP system consumers for a guarded boot runner.

Import build_programs(output, arch), fixture(directory, programs, cpus, smoke),
and validate(text, cpus, smoke). This module never selects or builds a kernel.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCES = {
    'init': 'tests/workloads/cost/smp_system.c',
    'scale': 'tests/workloads/cost/smp_scale.c',
    'pipe': 'tests/workloads/pipe/caller.c',
    'event': 'tests/workloads/event/caller.c',
    'network': 'tests/workloads/network/smp.c',
    'mm': 'tests/workloads/cost/smp_mm.c',
}
TOOLCHAINS = {
    'riscv': ('build/riscv-user-smp4/riscv/musl-root',
              'build/tools/riscv/usr/bin/riscv64-linux-gnu-gcc'),
    'loongarch': ('build/loongarch/musl-root',
                  'build/loongarch/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc'),
}
PIPE_NAMES = ('stream-one', 'stream-four', 'ppoll-four', 'pselect-four', 'epoll-four')
EVENT_NAMES = ('eventfd-direct', 'eventfd-epoll', 'timerfd-direct', 'timerfd-epoll')
SOCKET_NAMES = ('tcp-one', 'tcp-four', 'unix-stream-one', 'unix-stream-four',
                'udp-four', 'unix-dgram-four')
MM_NAMES = ('clock-shared', 'map-shared', 'clock-private', 'fork-exec-wait')
SCALE_NAMES = ('getpid-bound', 'signal-bound', 'compute-unbound', 'futex-bound',
               'futex-unbound', 'mq64-bound', 'mq64-unbound', 'files-bound')


def identity(path):
    path = Path(path).resolve(strict=True)
    return {'path': str(path), 'bytes': path.stat().st_size,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def run_logged(command, log, **kwargs):
    with Path(log).open('w') as stream:
        subprocess.run([str(part) for part in command], stdout=stream,
                       stderr=subprocess.STDOUT, check=True, **kwargs)


def build_programs(output, arch):
    """Return {guest_basename: static_ELF_Path}; record commands and ELF hashes."""
    musl_root, compiler = (ROOT / part for part in TOOLCHAINS[arch])
    wrapper = musl_root / 'bin/musl-gcc'
    output = Path(output).resolve() / arch
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, REALGCC=str(compiler))
    metadata = {'arch': arch, 'compiler': identity(compiler),
                'wrapper': identity(wrapper), 'REALGCC': str(compiler), 'programs': {}}
    programs = {}
    for name, source in SOURCES.items():
        program = output / name
        command = [str(wrapper), '-static', '-O2', '-std=gnu11', '-pthread',
                   '-Wall', '-Wextra', '-Werror', '-MMD', '-MF', str(output / (name + '.d')),
                   str(ROOT / source), '-o', str(program)]
        if arch == 'riscv':
            command.insert(1, '-fno-link-libatomic')
        run_logged(command, output / (name + '-compile.log'), env=env, cwd=ROOT)
        programs[name] = program
        dependencies = (output / (name + '.d')).read_text().replace('\\\n', '').split(':', 1)[1].split()
        metadata['programs'][name] = {
            'compile_command': command, 'elf': identity(program),
            'inputs': [identity(Path(path) if Path(path).is_absolute() else ROOT / path)
                       for path in dependencies],
        }
    (output / 'build.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return programs


def fixture(directory, programs, cpus, smoke=False):
    """Install six binaries and performance markers; return a 32 MiB ext4 image."""
    if cpus not in (1, 2, 4, 8):
        raise ValueError('cpus must be 1, 2, 4 or 8')
    if set(programs) != set(SOURCES):
        raise ValueError('the system fixture requires all six fixed binaries')
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    tree = directory / 'fixture'
    tree.mkdir()
    for name, program in programs.items():
        shutil.copy2(program, tree / name)
        (tree / name).chmod(0o755)
    profile = 'smoke' if smoke else 'formal'
    config = f'{cpus} {profile}\n'
    (tree / 'smp-scale-cost').write_text(config)
    for marker in ('pipe-perf', 'event-perf', 'performance'):
        (tree / marker).write_text('fixed SMP performance consumer\n')
    (tree / 'data').write_bytes(bytes(65 + i % 26 for i in range(9000)))
    (tree / 'dev').mkdir()
    (tree / 'proc').mkdir()
    disk = directory / 'fixture.img'
    with disk.open('wb') as stream:
        stream.truncate(32 * 1024 * 1024)
    run_logged(['mkfs.ext4', '-q', '-F', '-b', '4096', '-d', tree, disk], directory / 'mkfs.log')
    commands = directory / 'fixture.debugfs'
    commands.write_text('cd /dev\n'
                        'mknod console c 5 1\nset_inode_field console mode 020600\n'
                        'mknod null c 1 3\nset_inode_field null mode 020666\n'
                        'mknod zero c 1 5\nset_inode_field zero mode 020666\n')
    run_logged(['debugfs', '-w', '-f', commands, disk], directory / 'debugfs.log')
    metadata = {'config': config, 'cpus': cpus, 'profile': profile, 'windows': 27,
                'programs': {name: identity(program) for name, program in programs.items()},
                'image': identity(disk)}
    (directory / 'fixture.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return disk


def _records(text, prefix):
    return [line[line.index(prefix):] for line in text.replace('\r', '').splitlines()
            if prefix in line]


def _fields(row):
    return dict(part.split('=', 1) for part in row.split() if '=' in part)


def validate(text, cpus, smoke=False):
    """Check all fixed windows and their workload/affinity results after a boot."""
    profile = 'smoke' if smoke else 'formal'
    scale_begin = [_fields(row) for row in _records(text, 'SMP-SCALE BEGIN ')]
    if (len(scale_begin) != 1 or
            any(scale_begin[0].get(key) != value for key, value in
                {'version': '1', 'profile': profile, 'cpus': str(cpus), 'actors': '8',
                 'file_workers': '4', 'cpu_ids': ','.join(map(str, range(cpus)))}.items())):
        raise RuntimeError('scale CPU/profile configuration differs from the requested run')
    expected_ends = ('PIPE-PERF END 5', 'EVENT-PERF END 4', 'SOCKET-PERF END 6',
                     f'SMP-MM END windows=4 profile={profile}',
                     f'SMP-SYSTEM END consumers=5 windows=27 cpus={cpus} profile={profile}')
    for marker in expected_ends:
        if _records(text, marker) != [marker]:
            raise RuntimeError('missing or duplicated system consumer completion: ' + marker)
    scale_end = _records(text, 'SMP-SCALE END ')
    if len(scale_end) != 1 or not re.fullmatch(
            rf'SMP-SCALE END windows=8 checks=[1-9][0-9]* profile={profile}', scale_end[0]):
        raise RuntimeError('scale consumer did not complete its eight windows')
    groups = (('SMP-SCALE RESULT ', SCALE_NAMES), ('PIPE-PERF name=', PIPE_NAMES),
              ('EVENT-PERF name=', EVENT_NAMES), ('SOCKET-PERF name=', SOCKET_NAMES),
              ('SMP-MM RESULT ', MM_NAMES))
    metrics = {}
    for prefix, names in groups:
        rows = [_fields(row) for row in _records(text, prefix)]
        if tuple(row.get('name') for row in rows) != names:
            raise RuntimeError('missing, duplicated or reordered windows: ' + prefix)
        for row in rows:
            if int(row.get('wall_ns', row.get('ns', 0))) <= 0:
                raise RuntimeError('empty measured span: ' + row['name'])
            metrics[row['name']] = row
    getpids, signals = (4096, 64) if smoke else (1000000, 4096)
    rounds, files = (64, 64) if smoke else (4096, 4096)
    # work, transferred bytes, content checks, workers, latency samples.
    scale_expected = {
        'getpid-bound': (getpids, 0, getpids, 8, 0),
        'signal-bound': (signals, 0, signals, 8, signals),
        'compute-unbound': (80000 if smoke else 100000000, 0, 8, 8, 0),
        'futex-bound': (rounds, rounds * 128, rounds * 2, 8, rounds),
        'futex-unbound': (rounds, rounds * 128, rounds * 2, 8, rounds),
        'mq64-bound': (rounds, rounds * 128, rounds * 2, 8, rounds),
        'mq64-unbound': (rounds, rounds * 128, rounds * 2, 8, rounds),
        'files-bound': (files, files * 8192, files, 4, files),
    }
    scale_actors = [_fields(row) for row in _records(text, 'SMP-SCALE ACTOR ')]
    if len(scale_actors) != 60:
        raise RuntimeError('scale actor observations incomplete')
    for name, expected in scale_expected.items():
        row = metrics[name]
        actual = tuple(int(row[key]) for key in ('work', 'bytes', 'checks', 'workers', 'samples'))
        if actual != expected:
            raise RuntimeError('scale fixed work changed: ' + name)
        actors = [actor for actor in scale_actors if actor['name'] == name]
        if [int(actor['actor']) for actor in actors] != list(range(expected[3])):
            raise RuntimeError('scale actor observations incomplete: ' + name)
        begins = [int(actor['begin_ns']) for actor in actors]
        ends = [int(actor['end_ns']) for actor in actors]
        cpu = [int(actor['cpu_ns']) for actor in actors]
        if (any(begin <= 0 or end < begin for begin, end in zip(begins, ends)) or
                any(value < 0 for value in cpu) or
                max(ends) - min(begins) != int(row['wall_ns']) or sum(cpu) != int(row['cpu_ns'])):
            raise RuntimeError('scale actor spans differ from RESULT: ' + name)
    if int(_fields(scale_end[0])['checks']) != sum(row[2] for row in scale_expected.values()):
        raise RuntimeError('scale completion checks differ from the fixed work')
    cpu_rows = [_fields(row) for row in _records(text, 'SMP-SCALE CPU ')]
    if [int(row['actor']) for row in cpu_rows] != list(range(8)):
        raise RuntimeError('scale actual CPU observations incomplete')
    used_cpus = 0
    for row in cpu_rows:
        observed = [int(cpu) for cpu in row['ids'].split(',')]
        if int(row['samples']) != 16 or len(observed) != 16 or any(cpu not in range(cpus) for cpu in observed):
            raise RuntimeError('scale actor ran outside its selected CPU affinity')
        used = sum(1 << cpu for cpu in set(observed))
        if int(row['used_cpu_mask'], 16) != used:
            raise RuntimeError('scale actor CPU mask differs from its actual observations')
        used_cpus |= used
    valid_rows = [_fields(row) for row in _records(text, 'SMP-SCALE VALID ')]
    if tuple(row['name'] for row in valid_rows) != SCALE_NAMES[1:]:
        raise RuntimeError('scale content/completion observations incomplete')
    for row in valid_rows:
        name = row['name']
        if name == 'signal-bound':
            if int(row['delivered']) != signals or int(row['expected']) != signals:
                raise RuntimeError('scale signal delivery count changed')
        elif name == 'compute-unbound':
            checksum = ((0xed8eb42899304480 if smoke else 0x36de7ac67e71a943) * 8) & ((1 << 64) - 1)
            if (int(row['checksum']) != checksum or int(row['used_cpu_mask'], 16) != used_cpus or
                    int(row['allowed_cpu_mask'], 16) != (1 << cpus) - 1):
                raise RuntimeError('scale compute checksum or actual CPU summary differs')
        elif name.startswith('futex-'):
            if int(row['completed']) != 8 or int(row['wake_calls']) != rounds * 2:
                raise RuntimeError('scale futex completion count changed')
        elif name.startswith('mq64-'):
            if (int(row['completed']), int(row['messages']), int(row['queues_closed'])) != (8, rounds * 2, 8):
                raise RuntimeError('scale MQ completion/queue closure count changed')
        elif int(row['final_fsyncs']) != 4:
            raise RuntimeError('scale final file sync count changed')
    for name in PIPE_NAMES:
        row = metrics[name]
        expected_bytes = 4096 * (128 if name in ('ppoll-four', 'pselect-four') else 4096)
        if int(row['count']) != 4096 or int(row['bytes']) != expected_bytes:
            raise RuntimeError('pipe fixed work changed: ' + name)
    for name in EVENT_NAMES:
        if int(metrics[name]['count']) != (512 if name.startswith('timerfd') else 4096):
            raise RuntimeError('event fixed work changed: ' + name)
    for name in SOCKET_NAMES:
        row = metrics[name]
        datagram = 'dgram' in name or name.startswith('udp')
        if int(row['count']) != (4096 if datagram else 256) or int(row['bytes']) != (524288 if datagram else 16777216):
            raise RuntimeError('socket fixed work changed: ' + name)
    for name in MM_NAMES:
        total = ((16 if smoke else 1024) if name == 'map-shared' else
                 (4 if smoke else 32) if name == 'fork-exec-wait' else (1024 if smoke else 65536))
        workers = 4 if name == 'fork-exec-wait' else 8
        rounds = total // workers
        checksum = rounds * (rounds + 1) // 2 * workers * (workers + 1) // 2
        row = metrics[name]
        if (int(row['work']), int(row['checks']), int(row['workers']), int(row['checksum'])) != (total, total, workers, checksum):
            raise RuntimeError('MM work/content checks incomplete: ' + name)
        expected_bytes = total * (65536 if name == 'map-shared' else 16 if name.startswith('clock-') else 0)
        if int(row['bytes']) != expected_bytes:
            raise RuntimeError('MM fixed bytes changed: ' + name)
    for prefix, names in (('PIPE-PERF ACTOR ', PIPE_NAMES), ('EVENT-PERF ACTOR ', EVENT_NAMES),
                          ('SOCKET-PERF ACTOR ', SOCKET_NAMES), ('SMP-MM ACTOR ', MM_NAMES)):
        actors = [_fields(row) for row in _records(text, prefix)]
        for name in names:
            selected = [row for row in actors if row['name'] == name]
            count = (2 if name.endswith('-one') else 4 if name.startswith('timerfd') or name == 'fork-exec-wait' else 8)
            if [int(row['actor']) for row in selected] != list(range(count)):
                raise RuntimeError('actor observations incomplete: ' + name)
            for row in selected:
                cpu = int(row['cpu'])
                if cpu != int(row['actor']) % cpus or int(row['affinity'], 16) != 1 << cpu:
                    raise RuntimeError('actor affinity differs from its role: ' + name)
            if prefix == 'SMP-MM ACTOR ':
                result = metrics[name]
                rounds = int(result['work']) // count
                for row in selected:
                    actor = int(row['actor'])
                    if (int(row['completed']) != rounds or
                            int(row['checksum']) != rounds * (rounds + 1) // 2 * (actor + 1)):
                        raise RuntimeError('MM actor work/content checks incomplete: ' + name)
                begins = [int(row['begin_ns']) for row in selected]
                ends = [int(row['end_ns']) for row in selected]
                actor_cpu = [int(row['cpu_ns']) for row in selected]
                child_cpu = [int(row['child_cpu_ns']) for row in selected]
                if (any(begin <= 0 or end < begin for begin, end in zip(begins, ends)) or
                        any(value < 0 for value in actor_cpu + child_cpu) or
                        max(ends) - min(begins) != int(result['wall_ns']) or
                        sum(actor_cpu) != int(result['actor_cpu_ns']) or
                        sum(child_cpu) != int(result['child_cpu_ns']) or
                        sum(actor_cpu) + sum(child_cpu) != int(result['cpu_ns']) or
                        (name != 'fork-exec-wait' and any(child_cpu))):
                    raise RuntimeError('MM actor CPU/spans differ from RESULT: ' + name)
    return metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=tuple(TOOLCHAINS), default='riscv')
    parser.add_argument('--output', type=Path, required=True, help='build/fixture directory')
    parser.add_argument('--cpus', type=int, choices=(1, 2, 4, 8), default=1)
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--validate-log', type=Path, help='check a saved boot log instead of building')
    args = parser.parse_args()
    if args.validate_log:
        metrics = validate(args.validate_log.read_text(errors='replace'), args.cpus, args.smoke)
        print(f'SMP system: {len(metrics)} fixed windows passed')
    else:
        programs = build_programs(args.output, args.arch)
        image = fixture(args.output / f'{args.arch}-{args.cpus}cpu', programs, args.cpus, args.smoke)
        print(image)


if __name__ == '__main__':
    main()
