"""Immutable B3 paired-boot protocol; individual runner pilots stay pilots."""
import hashlib
import json
from pathlib import Path
import re
import struct

from cost_report import validate_locking

PROTOCOL = 'b3-short-v1'
SEED_BASE = 0xB30020261010
TARGETS = {'latency': 'latency-copy-4096', 'locking': 'locking-0-0-32'}
POSITIONS = ('before', 'middle', 'after')


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False) + '\n'


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def build_schedule(cases):
    if not cases or len(set(cases)) != len(cases) or set(cases) - set(TARGETS):
        raise ValueError('supported distinct cases required')
    rows = []
    for stage in ('calibration', 'comparison'):
        for block in range(12):
            for case in cases:
                for position in POSITIONS:
                    role = 'current' if stage == 'calibration' or position == 'middle' else 'baseline'
                    rows.append(dict(stage=stage, case=case, block=block,
                                     position=position, seed=SEED_BASE + block % 3,
                                     kernel_role=role))
    return rows


def seal_manifest(document):
    body = {k: v for k, v in document.items() if k != 'manifest_sha256'}
    return dict(body, manifest_sha256=hashlib.sha256(canonical(body).encode()).hexdigest())


def validate_manifest(document):
    if document.get('protocol') != PROTOCOL or document != seal_manifest(document):
        raise ValueError('campaign manifest changed or protocol missing')
    if document.get('schedule') != build_schedule(document.get('cases', ())):
        raise ValueError('incomplete, duplicate or changed schedule')


def expected_argv(expected, directory):
    boot = directory / (expected['case'] + '-0')
    return [expected['qemu_path'], '-machine', 'virt', '-bios', str(directory / 'firmware'),
            '-kernel', str(directory / 'kernel'), '-m', '512M', '-smp', '1',
            '-nographic', '-no-reboot', '-drive',
            f'file={boot}/root.img,if=none,format=raw,id=root,cache=writeback',
            '-global', 'virtio-mmio.force-legacy=false', '-device',
            'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0',
            '-seed', str(expected['seed']), '-dtb', str(boot / 'boot.dtb')]


def _single_rows(log, prefix):
    rows = {}
    for line in log.splitlines():
        if not line.startswith(prefix):
            continue
        fields = line[len(prefix):].split()
        if len(fields) < 2 or fields[0] in rows:
            raise ValueError('malformed or duplicate ' + prefix)
        rows[fields[0]] = fields[1:]
    return rows


def validate_boot(record, input_text, log, expected, returncode, directory):
    """Recheck the complete OFF log and its immutable input projection.

    A successful root terminal line is reached only after the frozen kernel's
    root_boot_finish gate. This proves that gate ran, not independently printed
    zero counters for every device or wait owner.
    """
    if type(returncode) is not int or returncode != 0:
        raise ValueError('runner did not naturally succeed')
    frozen = json.loads(input_text)
    keys = record.get('input_keys', [])
    if (len(keys) != len(set(keys)) or set(keys) != set(frozen) or
            canonical(frozen) != input_text or
            {k: record.get(k) for k in keys} != frozen or
            hashlib.sha256(input_text.encode()).hexdigest() != record.get('input_sha256')):
        raise ValueError('boot input seal/projection differs')
    for key in ('kernel_sha256', 'firmware_sha256', 'qemu_sha256', 'elf_sha256',
                'fixture_sha256'):
        if record.get(key) != expected[key]:
            raise ValueError('immutable input differs: ' + key)
    if record.get('source_sha256') != expected['runner_source_sha256']:
        raise ValueError('runner source changed during campaign')
    required = dict(case=expected['case'], replicas=1, replica=0,
                    cost_diagnostics=0, transport='modern', cache='writeback',
                    two_disks=False, platform='boaros', platform_config='fixture',
                    fixture_mode='frozen', fixture_input_sha256=expected['fixture_sha256'],
                    qemu_seed=expected['seed'], timebase_hz=10000000)
    if any(record.get(k) != v or type(record.get(k)) is not type(v) for k, v in required.items()):
        raise ValueError('pilot configuration differs')
    if record.get('acceptance') is not False or record.get('snapshots') != []:
        raise ValueError('pilot relabelled or diagnostics present')
    identity = record.get('kernel_build_identity', {})
    if (identity.get('kernel_sha256') != expected['kernel_sha256'] or
            identity.get('source_commit') != expected['kernel_source_commit'] or
            identity.get('source_diff_sha256') != expected['kernel_source_diff_sha256']):
        raise ValueError('kernel build provenance differs')
    if record.get('argv') != expected_argv(expected, directory):
        raise ValueError('QEMU profile/argv differs')
    if not record.get('dtb_sha256'):
        raise ValueError('DTB identity missing')
    log = log.replace('\r\n', '\n')
    if re.search(r'root boot error|root finish failure|scheduler error|fatal trap|cost (?:workload|contract) failed', log):
        raise ValueError('guest failure present')
    case = expected['case']
    passes = list(re.finditer(r'^COST PASS ' + re.escape(case) + r'$', log, re.M))
    if re.findall(r'^COST PASS (\S+)$', log, re.M) != [case]:
        raise ValueError('mixed or missing workload completion')
    stacks = list(re.finditer(r'^BoarOS: task stacks released=(0x[0-9a-f]+) '
                             r'min-free=(0x[0-9a-f]+) max-used=(0x[0-9a-f]+)$', log, re.M))
    exits = list(re.finditer(r'^BoarOS: PID 1 exited status=(0x[0-9a-f]+) '
                            r'pages=(0x[0-9a-f]+) heap-live=(0x[0-9a-f]+); shutting down$', log, re.M))
    if len(passes) != 1 or len(stacks) != 1 or len(exits) != 1:
        raise ValueError('missing/duplicate complete terminal owner gate')
    status, pages, live = (int(v, 16) for v in exits[0].groups())
    released, free, used = (int(v, 16) for v in stacks[0].groups())
    if status or live or not pages or not released or not passes[0].start() < stacks[0].start() < exits[0].start():
        raise ValueError('root status/resource/ordering failure')
    rows = _single_rows(log, 'COST RESULT ')
    if any(len(fields) != 1 or not fields[0].isdecimal() or int(fields[0]) <= 0 for fields in rows.values()):
        raise ValueError('invalid workload time')
    timings = {name: int(fields[0]) for name, fields in rows.items()}
    if timings != record.get('timings_ns'):
        raise ValueError('workload times differ from complete log')
    expectations = _single_rows(log, 'COST EXPECT ')
    if case == 'latency':
        names = {f'latency-copy-{n}' for n in (4096, 65536, 1048576)}
        names |= {f'latency-protect-{n}' for n in (4096, 1048576)}
        wanted = {f'latency-copy-{n}': ['file', '8', str(n * 8), str(n * 8)]
                  for n in (4096, 65536, 1048576)}
        if set(timings) != names or expectations != wanted:
            raise ValueError('latency work/coverage differs')
    else:
        validate_locking(timings, [], False)
        wanted, statuses = {}, {}
        for name in timings:
            _, relation, operation, waiters = name.split('-')
            jobs = int(waiters) + 1
            calls = jobs - (int(operation) in (3, 4))
            wanted[name] = ['file', str(calls), str(calls * 8192), str(calls * 8192)]
            for index in range(jobs):
                statuses[(name, index)] = 15 if int(operation) == 3 and index == 0 else 0
        actual = {}
        for line in log.splitlines():
            if not line.startswith('COST STATUS '):
                continue
            fields = line.split()
            if len(fields) != 5 or not fields[3].isdecimal() or not fields[4].isdecimal():
                raise ValueError('malformed child wait status')
            key = (fields[2], int(fields[3]))
            if key in actual:
                raise ValueError('duplicate child wait status')
            actual[key] = int(fields[4])
        if expectations != wanted or actual != statuses:
            raise ValueError('locking work/wait-status coverage differs')
    return dict(pages=pages, heap_live=live, stacks_released=released,
                stack_min_free=free, stack_max_used=used,
                evidence='natural exit after frozen root_boot_finish success gate')


def loaded_segments(path):
    """Compare exact ELF64/RISC-V load bytes and geometry, not debug sections."""
    data = Path(path).read_bytes()
    if len(data) < 64:
        raise ValueError('truncated ELF')
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    ident, _, machine, version, _, phoff, _, _, size, phsize, phnum, *_ = header
    if ident[:7] != b'\x7fELF\x02\x01\x01' or machine != 243 or version != 1 or size != 64 or phsize != 56 or not 0 < phnum <= 64:
        raise ValueError('expected bounded little-endian RV64 ELF')
    if phoff + phnum * phsize > len(data):
        raise ValueError('truncated program headers')
    result = []
    for index in range(phnum):
        kind, flags, offset, va, pa, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', data, phoff + index * phsize)
        if kind != 1:
            continue
        if filesz > memsz or offset + filesz > len(data):
            raise ValueError('invalid load segment')
        result.append((flags, va, pa, filesz, memsz, align,
                       hashlib.sha256(data[offset:offset + filesz]).hexdigest()))
    if not result:
        raise ValueError('no load segments')
    return result
