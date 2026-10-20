#!/usr/bin/env python3
r"""Matched data-path I/O experiments; smoke timings are never performance evidence.

  python3 -B tests/io-budget-experiment.py profiles
  python3 -B tests/io-budget-experiment.py build --profiles ra0-wb1,ra4-wb4
  python3 -B tests/io-budget-experiment.py smoke --variant current=kernel-rv
  python3 -B tests/io-budget-experiment.py run --variant current=build/io-budget/kernels/ra0-wb1/kernel-rv \
      --cases ext4:append:fsync:1:16M:64K,ext4:cold-read:cache:4:4M:4K --repeat 3

Cases: backend:operation:completion:files:bytes_per_file:request_bytes.
Backends ext4/tmpfs; operations append/overwrite/cold-read/hot-read/read/mixed-read/parallel-append/range-modify;
completion cache/fsync/fdatasync; files 1/4 (paired append uses 2); sizes 1/4/16/64M; requests 1/4/64K.
Cold-read is ext4 only, from an unread preloaded image. tmpfs read is resident
memory, never a claim of cold disk. A single task rotates requests among files;
completion spread is not a concurrent scheduler fairness or starvation bound.
mixed-read is ext4/fsync/four files: one prepared dirty writer, two independent
cold readers and one open/close task. Per-operation intervals show actual overlap
with final fsync; preparation is excluded and does not measure append throughput.
"""
import argparse
from array import array
import importlib.util
import itertools
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/io-budget'
_spec = importlib.util.spec_from_file_location('network_budget_helpers', ROOT / 'tests/network-budget-experiment.py')
helpers = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(helpers)
digest, save, output = helpers.digest, helpers.save, helpers.output
harness = helpers.harness
contracts = helpers.contracts
DEFAULT_CASES = ('ext4:append:cache:1:1M:4K,ext4:overwrite:fsync:1:1M:64K,'
                 'ext4:cold-read:cache:4:1M:4K,ext4:hot-read:cache:1:1M:1K,'
                 'tmpfs:append:cache:1:1M:4K,tmpfs:read:cache:1:1M:4K')


def profile(name):
    match = re.fullmatch(r'ra(0|1|2|4|8)-wb(1|2|4|8|16|32)', name)
    if not match:
        raise ValueError('invalid bounded profile ' + name)
    read, write = map(int, match.groups())
    return dict(name=name, readahead_pages=read, writeback_pages=write,
                page_bytes=4096, readahead_pages_bytes=read * 4096,
                writeback_pages_bytes=write * 4096, production_default=read == 0 and write == 32,
                scope='Compile-parameter arithmetic, not measured allocation or a guaranteed device batch size')


def profiles(value):
    names = [f'ra{r}-wb{w}' for r, w in itertools.product((0, 1, 2, 4, 8), (1, 2, 4, 8, 16, 32))] if value == 'all' else value.split(',')
    if len(names) != len(set(names)):
        raise ValueError('duplicate profile')
    return [profile(name) for name in names]


def definitions(row):
    return {'BOAROS_PAGE_CACHE_READAHEAD_PAGES': row['readahead_pages'],
            'BOAROS_PAGE_CACHE_WRITEBACK_PAGES': row['writeback_pages']}


def source_support():
    result = {}
    for name in ('BOAROS_PAGE_CACHE_READAHEAD_PAGES', 'BOAROS_PAGE_CACHE_WRITEBACK_PAGES'):
        found = subprocess.run(['rg', '-l', r'^\s*#\s*(ifndef|define)\s+' + name + r'\b', 'fs', 'include'],
                               cwd=ROOT, text=True, capture_output=True)
        if found.returncode:
            raise RuntimeError(name + ' is not wired into production source; refusing an ineffective -D candidate')
        result[name] = {path: digest(ROOT / path) for path in found.stdout.splitlines()}
    return result


def build(args):
    source_support()
    frozen_source = helpers.source_identity()
    compiler = contracts.compiler_identity(helpers.cross_prefix() + 'gcc', ROOT)
    for row in profiles(args.profiles):
        contracts.require_source(frozen_source, helpers.source_identity())
        work = OUT / 'kernels' / (row['name'] + ('-observe' if args.observe else ''))
        work.mkdir(parents=True, exist_ok=True)
        command = ['make', '-j' + str(args.jobs), 'all', f'BUILD_DIR={work / "objects"}',
                   f'KERNEL_RV={work / "kernel-rv"}', f'COST_DIAGNOSTICS={int(args.observe)}',
                   f'CFLAGS_EXTRA=-DBOAROS_PAGE_CACHE_READAHEAD_PAGES={row["readahead_pages"]} -DBOAROS_PAGE_CACHE_WRITEBACK_PAGES={row["writeback_pages"]}']
        effective = contracts.verify_macros(compiler['path'], ROOT, definitions(row), args.observe,
                                            'kernel/page_cache.h', ['-Iinclude'])
        core = contracts.build_identity('storage', row, definitions(row), args.observe, frozen_source, command, compiler, effective)
        executed = command if contracts.cache_matches(work, core, helpers.cross_prefix() + 'objcopy') else command + ['-B']
        with (work / 'build.log').open('w') as log:
            subprocess.run(executed, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        contracts.require_source(frozen_source, helpers.source_identity())
        if contracts.compiler_identity(helpers.cross_prefix() + 'gcc', ROOT) != compiler:
            raise RuntimeError('compiler changed during candidate build batch')
        manifest = contracts.seal_kernel(work / 'kernel-rv', core, helpers.cross_prefix() + 'objcopy')
        save(work / 'identity.json', manifest | dict(configuration_sources=source_support(), executed_command=executed,
             elf_size=output([helpers.cross_prefix() + 'size', str(work / 'kernel-rv')])))
        print(work / 'kernel-rv', flush=True)


def parse_case(name):
    parts = name.split(':')
    if len(parts) != 6:
        raise ValueError('case needs backend:operation:completion:files:size:request')
    backend, operation, completion, files, size, request = parts
    if backend not in ('ext4', 'tmpfs') or operation not in ('append', 'overwrite', 'cold-read', 'hot-read', 'read', 'mixed-read', 'parallel-append', 'range-modify') or completion not in ('cache', 'fsync', 'fdatasync'):
        raise ValueError('unsupported case ' + name)
    if files not in ('1', '2', '4') or size not in ('1M', '4M', '16M', '64M') or request not in ('1K', '4K', '64K'):
        raise ValueError('outside bounded file/request matrix ' + name)
    if files == '2' and operation != 'parallel-append':
        raise ValueError('two files select the paired append workload')
    if operation == 'parallel-append' and (backend != 'ext4' or completion != 'fsync' or files != '2'):
        raise ValueError('parallel-append requires ext4/fsync and two files')
    if operation == 'range-modify' and (backend != 'ext4' or completion != 'fsync' or files != '1'):
        raise ValueError('range-modify requires ext4/fsync and one file')
    if backend == 'tmpfs' and operation == 'cold-read':
        raise ValueError('tmpfs cannot provide a cold-disk read')
    if operation == 'mixed-read' and (backend != 'ext4' or completion != 'fsync' or files != '4'):
        raise ValueError('mixed-read requires ext4, fsync and four independent files')
    if operation not in ('append', 'overwrite', 'mixed-read', 'parallel-append', 'range-modify') and completion != 'cache':
        raise ValueError('read cases do not pretend a redundant fsync measures write durability')
    if backend == 'ext4' and operation == 'read':
        raise ValueError('ext4 read must explicitly choose cold-read or hot-read')
    return dict(name=name, backend=backend, operation=operation, completion=completion,
                files=int(files), bytes=int(size[:-1]) * 1048576, request=int(request[:-1]) * 1024,
                schedule='four processes, writer dirtied before release, final fsync and two cold readers plus last-FD closes' if operation == 'mixed-read' else 'two fixed-affinity appenders, fixed bytes and final fsync each' if operation == 'parallel-append' else 'one task, fixed four range syscalls per iteration and final fsync' if operation == 'range-modify' else 'one task, one request per file per round',
                cache_scope='first guest data read of preloaded image' if operation in ('cold-read', 'mixed-read') else 'one complete verified preread' if operation == 'hot-read' else 'resident tmpfs setup' if backend == 'tmpfs' else 'no target-data preread',
                persistence_scope='prepared dirty writer final fsync, separate reader/close intervals; not append throughput' if operation == 'mixed-read' else 'memory only' if backend == 'tmpfs' else 'explicit per-file sync syscall' if completion != 'cache' else 'cache completion; no durability claim for timed window')


def pattern(identifier, offset, count, epoch=0):
    assert offset % 4 == 0 and count % 4 == 0
    words = array('I', [((identifier * 0x9e3779b9) ^ (i * 0x85ebca6b) ^ (epoch * 0xc2b2ae35)) & 0xffffffff
                        for i in range(offset // 4, (offset + count) // 4)])
    if sys.byteorder != 'little':
        words.byteswap()
    return words.tobytes()


def seed(directory, case):
    directory.mkdir(parents=True, exist_ok=True)
    for identifier in range(case['files']):
        with (directory / f'file-{identifier}').open('wb') as stream:
            if case['operation'] not in ('append', 'parallel-append') and not (case['operation'] == 'mixed-read' and identifier == 0):
                for offset in range(0, case['bytes'], 65536):
                    stream.write(pattern(identifier, offset, min(65536, case['bytes'] - offset)))


def fixture(work, program, case, observe, timeout=240):
    tree = work / 'tree'; tree.mkdir()
    shutil.copyfile(program, tree / 'init'); (tree / 'init').chmod(0o755)
    for name in ('dev', 'proc', 'memory'):
        (tree / name).mkdir()
    if case['backend'] == 'ext4':
        seed(tree / 'data', case)
    else:
        (tree / 'data').mkdir()
    (tree / 'io-config').write_text(('{backend} {operation} {completion} {files} {bytes} {request} ' + str(timeout) + '\n').format(**case))
    if observe:
        (tree / 'observe').touch()
    image = work / 'fixture.img'
    with image.open('wb') as stream:
        stream.truncate(max(128 * 1048576, case['bytes'] * case['files'] * 2 + 64 * 1048576))
    harness.run_logged(['mkfs.ext4', '-q', '-F', '-b', '4096', '-d', str(tree), str(image)], work / 'mkfs.log')
    commands = work / 'devices.debugfs'
    commands.write_text('cd /dev\nmknod console c 5 1\nset_inode_field console mode 020600\n')
    harness.run_logged(['debugfs', '-w', '-f', str(commands), str(image)], work / 'debugfs.log')
    shutil.rmtree(tree)
    return image


def records(text, name):
    return [{key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', line)}
            for line in text.splitlines() if line.startswith('IO ' + name + ' ')]


def kernel_heap_peak(raw):
    values=re.findall(r'(?m)^ root-heap-peak-pages=(\d+)\r?$',raw)
    if len(values)!=1: raise RuntimeError('missing/duplicate root heap high watermark')
    return int(values[0])*4096


def validate(text, case):
    if not text.endswith('IO PASS all\n'):
        raise RuntimeError('incomplete durable guest results')
    if case['operation'] == 'mixed-read':
        result=validate_mixed(text,case)
    else:
        result=validate_regular(text,case)
    bindings,resources=records(text,'AFFINITY'),records(text,'RESOURCE')
    if len(bindings)!=case['files'] or {r.get('role') for r in bindings}!=set(range(case['files'])) or any(r.get('cpu',-1)<0 for r in bindings):
        raise RuntimeError('missing/duplicate actor affinity')
    if len(resources)!=1 or resources[0].get('self_maxrss_kib',0)<=0 or resources[0].get('children_maxrss_kib',-1)<0:
        raise RuntimeError('missing workload resource high watermarks')
    result.update(affinity=bindings,resources=resources[0])
    return result


def validate_regular(text,case):
    windows, rows, sizes = records(text, 'WINDOW'), records(text, 'FILE'), records(text, 'SIZE')
    if len(windows) != 1 or len(rows) != case['files'] or {r.get('id') for r in rows} != set(range(case['files'])):
        raise RuntimeError('missing, duplicate or wrong file/window identity')
    window = windows[0]
    writing = case['operation'] in ('append', 'overwrite', 'parallel-append')
    range_mode = case['operation'] == 'range-modify'
    expected_bytes = 0 if range_mode else case['bytes']
    sync_calls = case['files'] if case['completion'] != 'cache' else 0
    if window.get('bytes') != expected_bytes * case['files'] or window.get('complete') != 1 or window.get('sync_calls') != sync_calls:
        raise RuntimeError('wrong effective bytes, completion or actual sync count')
    if not window['start_ns'] < window['data_end_ns'] <= window['end_ns'] or window['elapsed_ns'] != window['end_ns'] - window['start_ns'] or window['data_ns'] != window['data_end_ns'] - window['start_ns'] or window['sync_ns'] != window['end_ns'] - window['data_end_ns']:
        raise RuntimeError('inconsistent I/O time window')
    if window['cleanup_sync_calls'] != (case['files'] if writing and not sync_calls else 0):
        raise RuntimeError('cache completion confused with cleanup durability')
    for row in rows:
        if row.get('bytes') != expected_bytes or row.get('size_after') != case['bytes'] or row.get('size_before') != (0 if case['operation'] in ('append','parallel-append') else case['bytes']) or row.get('complete') != 1:
            raise RuntimeError('file content/size/completion invalid')
        if row['calls'] < ((case['bytes'] + case['request'] - 1) // case['request']) * (4 if range_mode else 1):
            raise RuntimeError('reported calls cannot cover effective bytes')
        if not window['start_ns'] <= row['start_ns'] < row['end_ns'] <= window['data_end_ns']:
            raise RuntimeError('file data completion outside window')
        if sync_calls and not row['end_ns'] <= row['sync_end_ns'] <= window['end_ns']:
            raise RuntimeError('file sync completion outside window')
        if not sync_calls and row['sync_end_ns']:
            raise RuntimeError('unexpected measured sync')
        growth = [s for s in sizes if s['id'] == row['id']]
        if writing:
            if len(growth) != 5 or [s['sample'] for s in growth] != list(range(5)):
                raise RuntimeError('missing independent file-size checkpoints')
            wanted = [case['request'], case['bytes'] // 4, case['bytes'] // 2, case['bytes'] * 3 // 4, case['bytes']]
            if case['operation'] not in ('append','parallel-append'): wanted = [case['bytes']] * 5
            if [s['bytes'] for s in growth] != wanted:
                raise RuntimeError('append growth or fixed-size overwrite contract failed')
        elif growth:
            raise RuntimeError('unexpected read growth records')
    metrics = {key: window[key] for key in ('bytes', 'elapsed_ns', 'data_ns', 'sync_ns', 'sync_calls', 'verify_ns', 'cleanup_sync_ns', 'cleanup_sync_calls')}
    metrics.update(data_mib_s=window['bytes'] * 1e9 / (1048576 * window['data_ns']),
                   completion_mib_s=window['bytes'] * 1e9 / (1048576 * window['elapsed_ns']),
                   file_completion_span_ns=max(r['end_ns'] for r in rows) - min(r['end_ns'] for r in rows))
    if range_mode:
        operations=4*(case['bytes']//case['request'])
        if window.get('range_operations')!=operations or rows[0]['calls']!=operations:
            raise RuntimeError('range operation count changed')
        metrics.update(range_operations=operations,range_operations_s=operations*1e9/window['elapsed_ns'])
    samples=records(text,'SAMPLE')
    per_file=(case['bytes']+case['request']-1)//case['request']
    if len(samples)!=case['files']*per_file: raise RuntimeError('missing request intervals')
    for row in rows:
        own=[s for s in samples if s.get('id')==row['id']]
        if len(own)!=per_file or [s.get('sequence') for s in own]!=list(range(per_file)):
            raise RuntimeError('wrong request interval identity/order')
        if any(not row['start_ns']<=s['start_ns']<=s['end_ns']<=row['end_ns'] for s in own):
            raise RuntimeError('request interval outside file window')
    latencies={'io':latency_metrics([s['end_ns']-s['start_ns'] for s in samples])}
    metrics.update({'io_'+k:v for k,v in latencies['io'].items()})
    return dict(window=window,files=rows,sizes=sizes,metrics=metrics,latencies=latencies)


def latency_metrics(samples):
    ordered = sorted(samples)
    return dict(samples=len(ordered), p50_ns=ordered[(len(ordered) - 1) // 2] if ordered else None,
                p95_ns=ordered[max(0,(len(ordered)*95+99)//100-1)] if ordered else None,
                p99_ns=ordered[max(0, (len(ordered) * 99 + 99) // 100 - 1)] if ordered else None,
                maximum_ns=ordered[-1] if ordered else None)


def validate_mixed(text, case):
    windows = records(text, 'MIXED_WINDOW')
    rows, samples = records(text, 'MIXED_FILE'), records(text, 'MIXED_SAMPLE')
    if len(windows) != 1 or len(rows) != 4 or {r.get('id') for r in rows} != set(range(4)):
        raise RuntimeError('mixed workload missing or duplicated window/file')
    window = windows[0]
    if window.get('complete') != 1 or window.get('bytes') != case['bytes'] * 2 or window['start_ns'] >= window['end_ns']:
        raise RuntimeError('mixed read window/byte count invalid')
    by_id = {r['id']: r for r in rows}
    writer = by_id[0]
    if not writer['data_start_ns'] < writer['data_end_ns'] <= window['start_ns']:
        raise RuntimeError('dirty preparation must precede the mixed window')
    if not writer['start_ns'] <= writer['sync_start_ns'] < writer['sync_end_ns'] <= writer['end_ns']:
        raise RuntimeError('mixed final fsync interval invalid')
    expected_reads = (case['bytes'] + case['request'] - 1) // case['request']
    for identifier, row in by_id.items():
        expected = 0 if identifier == 0 else 256 if identifier == 3 else expected_reads
        own = [s for s in samples if s.get('id') == identifier]
        if row.get('complete') != 1 or row.get('bytes') != (0 if identifier == 3 else case['bytes']):
            raise RuntimeError('mixed file incomplete or wrong byte count')
        if row.get('samples') != expected or len(own) != expected or [s.get('sequence') for s in own] != list(range(expected)):
            raise RuntimeError('mixed operation samples missing, duplicated or out of order')
        if row.get('calls', 0) < (256 if identifier == 3 else expected_reads):
            raise RuntimeError('mixed operation calls do not cover requested work')
        if not window['start_ns'] <= row['start_ns'] < row['end_ns'] <= window['end_ns']:
            raise RuntimeError('mixed task outside the reported window')
        previous = row['start_ns']
        for sample in own:
            if not previous <= sample['start_ns'] < sample['end_ns'] <= row['end_ns']:
                raise RuntimeError('mixed sample interval invalid')
            opening = sample.get('open_start_ns', 0)
            if (identifier != 3 or sample['sequence'] == 0) and opening:
                raise RuntimeError('unexpected mixed open interval')
            if identifier == 3 and sample['sequence'] and not previous <= opening <= sample['start_ns']:
                raise RuntimeError('mixed open interval invalid')
            previous = sample['end_ns']
        if identifier and (row['data_start_ns'] or row['data_end_ns'] or row['sync_start_ns'] or row['sync_end_ns']):
            raise RuntimeError('reader/closer unexpectedly reports writer work')
    if len(samples) != expected_reads * 2 + 256:
        raise RuntimeError('mixed sample has an unknown identity')
    metrics = dict(bytes=window['bytes'], elapsed_ns=window['end_ns'] - window['start_ns'],
                   sync_ns=writer['sync_end_ns'] - writer['sync_start_ns'],
                   prepared_write_bytes=case['bytes'])
    latencies = {}
    for name, identifiers in (('read', (1, 2)), ('close', (3,))):
        own = [s for s in samples if s['id'] in identifiers]
        overlapping = [s for s in own if s['start_ns'] < writer['sync_end_ns'] and s['end_ns'] > writer['sync_start_ns']]
        latencies[name] = dict(all=latency_metrics([s['end_ns'] - s['start_ns'] for s in own]),
                               overlapping_fsync=latency_metrics([s['end_ns'] - s['start_ns'] for s in overlapping]),
                               completed_during_fsync=sum(writer['sync_start_ns'] < s['end_ns'] <= writer['sync_end_ns'] for s in own))
        for scope in ('all', 'overlapping_fsync'):
            for key, value in latencies[name][scope].items():
                # An fsync can finish before this task runs. Keep absent overlap
                # percentiles absent, rather than reporting a zero-latency sample.
                if value is not None and (scope == 'all' or key == 'samples'):
                    metrics[f'{name}_{scope}_{key}'] = value
        metrics[f'{name}_completed_during_fsync'] = latencies[name]['completed_during_fsync']
    openings = [s['start_ns'] - s['open_start_ns'] for s in samples if s['open_start_ns']]
    latencies['open'] = latency_metrics(openings)
    metrics.update({f'open_{key}': value for key, value in latencies['open'].items()})
    return dict(window=window, files=rows, samples=samples, latencies=latencies, metrics=metrics)


def extract(disk, work):
    readout = work / 'readout.img'; shutil.copyfile(disk, readout)
    command = ['e2fsck', '-p', '-E', 'journal_only', str(readout)]
    with (work / 'readout-journal.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode not in (0, 1):
        raise RuntimeError('result journal replay failed')
    identity = dict(method='journal replay on a copy; original is unchanged', command=command,
                    original_sha256=digest(disk), readout_sha256=digest(readout), returncode=result.returncode)
    target = work / 'guest-results.txt'
    harness.run_logged(['debugfs', '-R', f'dump /io-config.results {target}', str(readout)], work / 'extract.log')
    text = target.read_text(); identity['results_sha256'] = digest(target)
    readout.unlink()
    return text, identity


def boot(work, kernel, image, case, name, repetition, args, program_sha):
    work.mkdir(); disk = work / 'disk.img'
    subprocess.run(['cp', '--reflink=never', '--sparse=never', str(image), str(disk)], check=True)
    with disk.open('rb') as backing:
        os.fsync(backing.fileno())
    command = [args.qemu, '-machine', 'virt', '-bios', str(getattr(args,'firmware','default')), '-kernel', str(kernel), '-m', args.ram,
               '-smp', str(getattr(args, 'cpus', 1)), '-accel','tcg,thread=multi','-nographic', '-no-reboot', '-global', 'virtio-mmio.force-legacy=' + ('true' if args.transport == 'legacy' else 'false'),
               '-drive', f'file={disk},if=none,format=raw,id=root,cache=writeback',
               '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
    if name == 'linux':
        command += ['-append', 'root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
    row = dict(case=case, variant=name, repetition=repetition, kernel_sha256=digest(kernel), program_sha256=program_sha,
               fixture_sha256=digest(image), observation_enabled=args.observe, smoke=args.smoke,
               backing_preparation='written-and-synced', command=command)
    try:
        harness.run_logged(command, work / 'console.log', args.timeout + 20)
        raw = (work / 'console.log').read_text(errors='replace')
        if name != 'linux' and ('heap-live=0x0; shutting down' not in raw or 'exited status=0x0 ' not in raw):
            raise RuntimeError('BoarOS clean exit/heap teardown missing')
        text, row['readout'] = extract(disk, work)
        harness.run_logged(['e2fsck','-fn',str(disk)],work/'fsck.log')
        row['filesystem_check']={'command':['e2fsck','-fn',str(disk)],'returncode':0}
        row.update(validate(text, case))
        if name!='linux':
            row['resources']['root_heap_peak_bytes']=kernel_heap_peak(raw)
            row['resource_scope']='process RSS and root heap lifetime high watermarks; includes setup/result writing; components are not additive'
        bodies = re.findall(r'^IO COST BEGIN\n(.*?)^IO COST END$', text, re.M | re.S)
        if bool(bodies) != args.observe or len(bodies) > 1:
            raise RuntimeError('diagnostic snapshot/mode mismatch')
        if bodies:
            sys.path.insert(0, str(ROOT / 'tests'))
            import cost_report
            errors = []
            for schema in cost_report.supported_schemas():
                try:
                    row['cost'] = cost_report.parse(bodies[0], 1, schema); break
                except ValueError as error: errors.append(str(error))
            else: raise RuntimeError('invalid cost snapshot: ' + errors[0])
            memory = re.search(r'^IO MEM_AFTER BEGIN\n(.*?)^IO MEM_AFTER END$', text, re.M | re.S)
            row['memory_snapshot'] = memory.group(1) if memory else None
            peak = re.search(r'^IO MEM_PEAK BEGIN\n(.*?)^IO MEM_PEAK END$', text, re.M | re.S)
            row['memory_peaks'] = {key: int(value) for key,value in re.findall(r'^(\w+)=(\d+)$', peak.group(1), re.M)} if peak else None
            if not row['memory_peaks'] or any(row['memory_peaks'][key+'_peak_bytes'] < row['memory_peaks'][key+'_current_bytes'] for key in ('managed','heap')):
                raise RuntimeError('missing or inconsistent diagnostic memory high watermarks')
            row['memory_peak_scope'] = 'Boot-to-snapshot high watermarks, sampled before cost formatting; managed pages include kernel runtime, cache and user allocations but exclude reserved kernel image/firmware/MMIO. Heap is a subset; do not add component maxima. These are not reset at workload begin.'

            row['observation_scope'] = 'Cost end may wait for in-flight scopes; user data/sync timestamps are separate. MEM_AFTER is a point snapshot after diagnostic result writing, not a workload peak.'
        row['status'] = 'passed'; disk.unlink()
    except Exception as error:
        row.update(status='failed', error=str(error))
    save(work / 'result.json', row)
    print(name, case['name'], repetition, row['status'], flush=True)
    return row


def summarize(work, identity, rows):
    contracts.validate_variants(identity['variants'], 'storage', profile, definitions, identity['observe'],
        identity['smoke'], helpers.BASELINE_SHA, helpers.BASELINE_COMMIT, identity.get('linux_identity'))
    groups, fixtures, seen = {}, {}, set()
    expected_cases = {c['name']: c for c in identity['cases']}
    for row in rows:
        variant, case = row['variant'], row['case']['name']
        if row['case'] != expected_cases.get(case) or variant not in identity['variants'] or row['kernel_sha256'] != identity['variants'][variant]['kernel_sha256'] or row['program_sha256'] != identity['program_sha256'] or row['observation_enabled'] != identity['observe']:
            raise RuntimeError('mismatched input in I/O results')
        if fixtures.setdefault(case, row['fixture_sha256']) != row['fixture_sha256']:
            raise RuntimeError('fixture changed within matched case')
        key = (variant, case, row['repetition'])
        if key in seen or not 1 <= row['repetition'] <= identity['repeat']:
            raise RuntimeError('duplicate/invalid boot repetition')
        seen.add(key); groups.setdefault((variant, case), []).append(row)
    summary = []
    for (variant, case), values in groups.items():
        good = [r for r in values if r['status'] == 'passed']
        complete = len(good) == identity['repeat']
        status = 'smoke_passed' if identity['smoke'] and complete else 'diagnostic' if identity['observe'] and complete else 'measured' if complete and len(good) >= 3 else 'incomplete'
        item = dict(variant=variant, case=case, passed_boots=len(good), status=status, metrics={})
        item['comparison_eligible'] = status == 'measured'
        if good and not identity['smoke']:
            for key in good[0]['metrics']:
                samples = [r['metrics'][key] for r in good]
                item['metrics'][key] = dict(median=statistics.median(samples), minimum=min(samples), maximum=max(samples), samples=samples)
        summary.append(item)
    expected = len(identity['cases']) * len(identity['variants']) * identity['repeat']
    save(work / 'summary.json', dict(evidence_kind=identity['evidence_kind'], expected_boots=expected, completed_boots=len(rows),
         status='complete' if len(rows) == expected and all(r['status'] == 'passed' for r in rows) else 'incomplete', groups=summary,
         failures=[dict(variant=r['variant'], case=r['case']['name'], error=r['error']) for r in rows if r['status'] != 'passed'],
         comparison_groups=[dict(variant=g['variant'], case=g['case']) for g in summary if g['status'] == 'measured'],
         comparison_policy='Only status=measured groups may support comparisons/recommendations; other metrics are diagnostic subsets.',
         limits='Setup, final readback and cleanup sync are outside workload time. Ordinary cases include pattern generation/read checking and five write-size checks. Mixed cases prepare the dirty writer before the window and record actual fsync overlap separately; absent overlap has no latency percentile. Fresh guest cache does not mean cold host storage. Serial round robin does not establish concurrent fairness. tmpfs has no persistent-medium guarantee.'))


def run(args):
    with contracts.measurement_lock(ROOT, enabled=not args.smoke or args.observe):
        return run_locked(args)


def run_locked(args):
    if (args.smoke or args.observe) and args.repeat != 1 or not (args.smoke or args.observe) and args.repeat < 3:
        raise ValueError('smoke/diagnostic require one boot; release requires at least three')
    baseline = getattr(args, 'baseline', helpers.BASELINE)
    variants = {} if args.smoke or args.observe else {'baseline': baseline}
    if variants and digest(baseline) != helpers.BASELINE_SHA:
        raise ValueError('original baseline SHA mismatch')
    metadata = {'baseline': dict(management='fixed-baseline', kernel_sha256=helpers.BASELINE_SHA, source_commit=helpers.BASELINE_COMMIT)} if variants else {}
    for value in args.variant:
        name, path = value.split('=', 1)
        if not re.fullmatch('[A-Za-z0-9_-]+', name) or name in variants or name in ('linux', 'baseline'):
            raise ValueError('invalid/duplicate variant')
        variants[name] = Path(path).resolve()
    linux_identity = None
    if args.linux_kernel:
        if args.observe: raise ValueError('Linux does not provide BoarOS diagnostics')
        variants['linux'], linux_identity = harness.fixed_linux_image(args.linux_kernel)
        metadata['linux'] = dict(management='fixed-linux', kernel_sha256=linux_identity['image_sha256'])
    for name, path in variants.items():
        if name not in ('linux', 'baseline'):
            metadata[name] = contracts.load_candidate(name, path, 'storage', profile, definitions,
                args.observe, args.smoke, helpers.cross_prefix() + 'objcopy',
                output([helpers.cross_prefix() + 'nm', str(path)]))
    contracts.validate_variants(metadata, 'storage', profile, definitions, args.observe, args.smoke,
                                helpers.BASELINE_SHA, helpers.BASELINE_COMMIT, linux_identity)
    cases = [parse_case(name) for name in args.cases.split(',')]
    if len({c['name'] for c in cases}) != len(cases): raise ValueError('duplicate case')
    work = (args.output or OUT / (('smoke-' if args.smoke else 'run-') + str(time.time_ns()))).resolve()
    work.mkdir(parents=True, exist_ok=False); (work / 'runs').mkdir()
    program = work / 'init'; compiler = ROOT / 'build/riscv/musl-root/bin/musl-gcc'
    subprocess.run([str(compiler), '-fno-link-libatomic', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'tests/workloads/io/data-path.c'), '-o', str(program)], check=True)
    frozen = {}
    for name, path in variants.items():
        frozen[name] = work / (name + '-kernel'); shutil.copyfile(path, frozen[name])
        if digest(frozen[name]) != metadata[name]['kernel_sha256']:
            raise RuntimeError('kernel changed between manifest validation and freezing: ' + name)
    args.qemu = shutil.which(os.environ.get('QEMU_RISCV64', 'qemu-system-riscv64'))
    if not args.qemu: raise RuntimeError('QEMU unavailable')
    identity = dict(runtime_source=helpers.source_identity(), program_sha256=digest(program), workload_source_sha256=digest(ROOT / 'tests/workloads/io/data-path.c'),
        variants={name: metadata[name] | dict(kernel_sha256=digest(path), source_path=str(variants[name])) for name, path in frozen.items()},
        cases=cases, repeat=args.repeat, observe=args.observe, smoke=args.smoke, linux_identity=linux_identity,
        evidence_kind='functional-smoke-not-performance' if args.smoke else 'diagnostic' if args.observe else 'performance',
        qemu_version=output([args.qemu, '--version']), qemu_sha256=digest(args.qemu), compiler=output([str(compiler), '--version']),
        firmware={'selection':args.firmware,'sha256':digest(args.firmware) if args.firmware!='default' else None},
        ram=args.ram, cpus=args.cpus, transport=args.transport, disk_cache='writeback', timeout_seconds=args.timeout, baseline_commit=helpers.BASELINE_COMMIT,
        cache_policy='Fresh kernel boot and matched image copy per case/variant/repetition; no drop_caches or host cache eviction')
    save(work / 'identity.json', identity); rows = []
    for index, case in enumerate(cases):
        directory = work / f'case-{index}'; directory.mkdir()
        image = fixture(directory, program, case, args.observe, args.timeout)
        for repetition in range(1, args.repeat + 1):
            for name, kernel in frozen.items():
                row = boot(work / 'runs' / f'{index:03d}-{repetition}-{name}', kernel, image, case, name, repetition, args, identity['program_sha256'])
                rows.append(row); summarize(work, identity, rows)
                if row['status'] != 'passed': print('incomplete experiment:', work); return 1
        image.unlink()
    print('experiment:', work)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('profiles')
    p = sub.add_parser('build'); p.add_argument('--profiles', default='all'); p.add_argument('--observe', action='store_true'); p.add_argument('--jobs', type=int, default=2)
    for command in ('run', 'smoke'):
        p = sub.add_parser(command); p.set_defaults(smoke=command == 'smoke')
        p.add_argument('--baseline', type=Path, default=helpers.BASELINE)
        p.add_argument('--variant', action='append', required=True); p.add_argument('--linux-kernel', type=Path)
        p.add_argument('--cases', default=DEFAULT_CASES); p.add_argument('--repeat', type=int, default=1 if command == 'smoke' else 3)
        p.add_argument('--observe', action='store_true'); p.add_argument('--output', type=Path)
        p.add_argument('--ram', default='512M'); p.add_argument('--transport', choices=('legacy', 'modern'), default='modern')
        p.add_argument('--cpus', type=int, choices=(1, 2), default=1, help='guest CPUs; two requires a USER_SMP kernel')
        p.add_argument('--firmware',default='default',help='explicit OpenSBI firmware or QEMU default')
        p.add_argument('--timeout', type=int, default=240, help='explicit guest budget in seconds, 1..3600; excludes no failed runs')
    args = parser.parse_args()
    if args.command == 'profiles': print(json.dumps(profiles('all'), indent=2))
    elif args.command == 'build': build(args)
    else:
        if not 1 <= args.timeout <= 3600: parser.error('timeout must be in 1..3600 seconds')
        return run(args)
    return 0


if __name__ == '__main__': sys.exit(main())
