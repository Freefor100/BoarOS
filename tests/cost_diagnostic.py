"""Diagnostic-only accounting; never a formal performance verdict."""
import json
import os
from pathlib import Path
from cost_match import canonical, digest, expected_argv

PROFILE = 'rv-fs-off-residual-diagnostic-v1'
SEED = 0xB30020261010
WINDOW_NAMES = {
    'latency': [f'latency-copy-{n}' for n in (4096, 65536, 1048576)] +
               [f'latency-protect-{n}' for n in (4096, 1048576)],
    'locking': [f'locking-{r}-0-{n}' for r in range(3) for n in (1, 8, 32)] +
               [f'locking-1-{op}-8' for op in range(1, 5)],
}
EVENT_NAMES = ('timer', 'context_switch', 'switch_finish', 'park', 'reap',
               'task_resources_cleanup', 'journal_worker')
EVENT_SYMBOLS = ('riscv_timer_handle_interrupt', 'riscv_context_switch',
                 'kernel_scheduler_switch_finish', 'kernel_wait_park',
                 'kernel_scheduler_reap_one', 'cleanup_user_task_resources', 'journal_worker')
MAX_TBS = 131072


def schedule():
    forward = [(case, seed) for seed in range(3) for case in ('latency', 'locking')]
    return [dict(ordinal=i, case=case, seed=SEED + seed)
            for i, (case, seed) in enumerate(forward + list(reversed(forward)))]


def reserve_attempt(directory, entry):
    """A preflight/timeout is spent too. Exclusive creation prevents refilling it."""
    ordinal = entry.get('ordinal')
    if type(ordinal) is not int or not 0 <= ordinal < 12 or entry != schedule()[ordinal]:
        raise ValueError('attempt outside fixed twelve-slot schedule')
    directory = Path(directory)
    try:
        with (directory / f'{ordinal:02d}.spent').open('x') as stream:
            stream.write(json.dumps(entry, sort_keys=True) + '\n')
            stream.flush()
            os.fsync(stream.fileno())
    except FileExistsError as error:
        raise ValueError('attempt already spent; no replacement or resume') from error


def validate_receipt(receipt, expected):
    if receipt.get('profile') != PROFILE or receipt.get('acceptance') is not False:
        raise ValueError('diagnostic relabelled as acceptance')
    if set(receipt) != set(expected) | {'profile', 'acceptance'}:
        raise ValueError('execution receipt fields differ')
    if any(receipt.get(k) != v or type(receipt.get(k)) is not type(v) for k, v in expected.items()):
        raise ValueError('actual execution identity/argv differs')


def execution_config(manifest, entry, output):
    """Both wrapper and verifier derive the actual argv from the same sealed inputs."""
    if entry not in schedule() or manifest.get('schedule') != schedule():
        raise ValueError('schedule differs')
    output = Path(output).resolve()
    directory = output / 'boots' / f"{entry['ordinal']:02d}-{entry['case']}"
    frozen = manifest['inputs']
    observer = frozen['observer']['path']
    case = entry['case']
    case_info = frozen['cases'][case]
    events = [frozen['events'][name]['pc'] for name in EVENT_NAMES]
    trace = str(output / f"{entry['ordinal']:02d}.trace")
    plugin = ','.join([f'file={observer}', f"clock={case_info['clock_pc']}", f'output={trace}',
                       f'windows={len(WINDOW_NAMES[case])}', 'schema=1'] +
                      [f'event={pc}' for pc in events])
    base = expected_argv(dict(qemu_path=frozen['qemu']['path'], case=case, seed=entry['seed']), directory)
    actual = base + ['-plugin', plugin]
    return dict(entry=entry, directory=str(directory), trace=trace,
        manifest_path=str(output / 'manifest.json'),
        receipt_path=str(output / f"{entry['ordinal']:02d}.execution.json"),
        expected=dict(ordinal=entry['ordinal'], argv=actual,
                      qemu_sha256=frozen['qemu']['sha256'], plugin_sha256=frozen['observer']['sha256'],
                      manifest_sha256=digest(output / 'manifest.json'),
                      clock_pc=case_info['clock_pc'], events=events))


def sealed(document):
    import hashlib
    return dict(document, manifest_sha256=hashlib.sha256(canonical(document).encode()).hexdigest())


def check_manifest(document):
    rest = dict(document)
    seal = rest.pop('manifest_sha256', None)
    if (sealed(rest)['manifest_sha256'] != seal or rest.get('profile') != PROFILE or
            rest.get('acceptance') is not False or rest.get('schedule') != schedule()):
        raise ValueError('manifest seal/profile/schedule differs')


def validate_policy_snapshot(ready, snapshot):
    if ready.get('event') != 'ready':
        raise ValueError('CPU policy acquisition missing')
    policies = snapshot['policies']
    if set(policies) != set(ready['requested']):
        raise ValueError('CPU policy membership differs')
    for name, requested in ready['requested'].items():
        reference = ready['snapshot']['policies'][name]
        actual = policies[name]
        if (actual['fields'] != reference['fields'] or actual['cpus'] != reference['cpus'] or
                any(actual['fields'].get(k) != v for k, v in requested.items())):
            raise ValueError('CPU policy drift during diagnostic')


def wrapper_arguments(config, arguments):
    """Only the exact version query, non-running DTB dump, or one approved boot."""
    base = config['expected']['argv'][:-2]
    if arguments == ['--version']:
        return [base[0], '--version'], False
    probe = base[1:-2]
    probe[probe.index('-machine') + 1] = 'virt,dumpdtb=' + base[-1]
    if arguments == probe:
        return [base[0], *arguments], False
    if arguments != base[1:]:
        raise ValueError('actual QEMU arguments differ before exec')
    return config['expected']['argv'], True


def parse_trace(text, case):
    """Bounded, complete traces only; TB proxy and entry events are not completions."""
    count = len(WINDOW_NAMES[case])
    lines = text.splitlines()
    if not lines:
        raise ValueError('empty diagnostic trace')
    try:
        header = [int(x) for x in lines[0].split()[1:]]
        if (lines[0].split()[0] != 'TRACE' or len(header) != 6 or
                header[:5] != [1, count * 2, 0, 1, 1] or not 0 < header[5] <= MAX_TBS):
            raise ValueError('incomplete/overflow/multi-CPU/register trace')
        total = header[5]
        windows, flushes, blocks, ended = {}, {}, [], False
        for line in lines[1:]:
            fields = line.split()
            if not fields or ended:
                raise ValueError('extra or blank trace row')
            kind = fields[0]
            values = [int(x, 16 if kind == 'TB' and i in (1, 2) else 10)
                      for i, x in enumerate(fields[1:])]
            if any(x < 0 or x > (1 << 64) - 1 for x in values):
                raise ValueError('trace counter out of range')
            if kind == 'WINDOW' and len(values) == 16:
                index, wall, cpu, uncertainty, voluntary, involuntary, translations, generated, proxy, *events = values
                if index in windows or not 0 <= index < count or not wall or not 0 < cpu <= wall + uncertainty:
                    raise ValueError('duplicate or invalid window')
                windows[index] = dict(name=WINDOW_NAMES[case][index], wall_ns=wall,
                    thread_cpu_ns=cpu, wall_minus_thread_cpu_ns=wall-cpu,
                    clock_sampling_uncertainty_ns=uncertainty,
                    wall_minus_thread_cpu_interval_ns=[wall-cpu-uncertainty, wall-cpu+uncertainty],
                    voluntary_switches=voluntary, involuntary_switches=involuntary,
                    translations=translations, generated_instructions=generated,
                    tb_instruction_proxy=proxy, entries=dict(zip(EVENT_NAMES, events)))
            elif kind == 'FLUSH' and len(values) == 2:
                index, n = values
                if index in flushes or not 0 <= index <= count * 2:
                    raise ValueError('duplicate or invalid flush phase')
                flushes[index] = n
            elif kind == 'TB' and len(values) == 5 + count * 2:
                index, pc, haddr, insns, phase, *executions = values
                if index != len(blocks) or not pc or not insns or phase > count * 2:
                    raise ValueError('invalid TB generation')
                pairs = list(zip(executions[::2], executions[1::2]))
                if any(fs > 3 if n else fs != 4 for n, fs in pairs):
                    raise ValueError('invalid first-execution FS sample')
                blocks.append(dict(id=index, pc=pc, haddr_identity=haddr, insns=insns,
                                   generation_phase=phase, windows=pairs))
            elif kind == 'END' and values == [total] and len(blocks) == total:
                ended = True
            else:
                raise ValueError('unknown/malformed trace row')
        if not ended or set(windows) != set(range(count)) or set(flushes) != set(range(count * 2 + 1)):
            raise ValueError('truncated diagnostic trace')
        for index, w in windows.items():
            active = [b for b in blocks if b['windows'][index][0]]
            generated = [b for b in blocks if b['generation_phase'] == index * 2 + 1]
            if (sum(b['insns'] * b['windows'][index][0] for b in active) != w['tb_instruction_proxy'] or
                    len(generated) != w['translations'] or
                    sum(b['insns'] for b in generated) != w['generated_instructions']):
                raise ValueError('TB accounting differs from window')
            w['tb_records'] = len(active)
            w['unique_pcs'] = len({b['pc'] for b in active})
            w['code_cache_flushes'] = flushes[index * 2 + 1]
            w['fs_instruction_proxy'] = {str(fs): sum(b['insns'] * n for b in active
                for n, sample in [b['windows'][index]] if sample == fs) for fs in range(4)}
        return dict(profile=PROFILE, acceptance=False, tb_count=total,
                    windows=[windows[i] for i in range(count)], flushes=flushes, blocks=blocks)
    except (IndexError, KeyError, TypeError) as error:
        raise ValueError('malformed diagnostic trace') from error
