"""Strict cost v1 snapshot reader; timestamps are guest CSR ticks, never wall-clock gates."""
from pathlib import Path
import math
import re

ROOT = Path(__file__).resolve().parents[1]
LANES = ('foreground', 'background', 'observer')
HEADER = ('irq_user_prefix_instructions irq_supervisor_prefix_instructions irq_sret_suffix_instructions irq_c_enable_suffix_min_instructions version epoch state mode owner timebase_hz resolution_ns_numerator '
          'resolution_ns_denominator start_ticks end_ticks storage_bytes task_bytes overflow inflight').split()

def schema():
    return [(name,unit,int(hist)) for name,unit,hist in re.findall(
        r'^X\(\w+, (\w+), (\w+), ([01])\)$', (ROOT/'include/kernel/cost.def').read_text(), re.M)]

def supported_schemas():
    current=schema()
    pipeline={'journal_checkpoint_ticks','journal_wait_sealed','journal_wait_durable','journal_wait_checkpoint'}
    stored_pipeline=current[:next(i+1 for i,x in enumerate(current) if x[0]=='io_complete_to_resume')]
    journal=[metric for metric in stored_pipeline if metric[0] not in pipeline]
    original=current[:next(i+1 for i,x in enumerate(current) if x[0]=='wake_to_run')]
    previous=current[:next(i for i,x in enumerate(current) if x[0]=='journal_seal_operations')]
    memory_previous=current[:next(i for i,x in enumerate(current) if x[0]=='heap_zero_bytes')]
    resize_previous=current[:next(i for i,x in enumerate(current) if x[0]=='resize_visits')]
    network_previous=current[:next(i for i,x in enumerate(current) if x[0]=='network_service_calls')]
    admission_previous=current[:next(i for i,x in enumerate(current) if x[0]=='stream_admit_blocked')]
    allocator_previous=current[:next(i for i,x in enumerate(current) if x[0]=='page_meta_checked')]
    raw_previous=current[:next(i for i,x in enumerate(current) if x[0]=='raw_wait_ticks')]
    return current,memory_previous,previous,stored_pipeline,journal,original,network_previous,admission_previous,resize_previous,allocator_previous,raw_previous

def parse(text, epoch, metrics=None):
    metrics=schema() if metrics is None else metrics
    if list(metrics) not in supported_schemas():raise ValueError('unsupported metric registry')
    fields={}
    for line in text.splitlines():
        if not line or line.count('=') != 1: raise ValueError('malformed record')
        key,value=line.split('=')
        if key in fields: raise ValueError('duplicate key: '+key)
        fields[key]=value
    expected=set(HEADER)
    strings={'state','mode'}
    for lane in LANES:
        for name,unit,hist in metrics:
            prefix=lane+'.'+name+'.'
            expected.update(prefix+s for s in ('unit','value','samples','max'))
            strings.add(prefix+'unit')
            if fields.get(prefix+'unit') != unit: raise ValueError('wrong unit: '+prefix)
            if hist: expected.update(prefix+'bucket.'+str(i) for i in range(65))
    if set(fields) != expected: raise ValueError('schema keys missing/unknown: '+str(expected^set(fields)))
    for key in expected-strings:
        if not re.fullmatch(r'0|[1-9][0-9]*', fields[key]): raise ValueError('invalid integer: '+key)
        fields[key]=int(fields[key])
        if fields[key] > (1<<64)-1: raise ValueError('integer overflow')
    if fields['version'] != 1 or fields['epoch'] != epoch or epoch < 1: raise ValueError('version/epoch')
    if fields['state'] != 'complete' or fields['mode'] not in ('user','fixture'): raise ValueError('incomplete snapshot')
    if fields['overflow'] or fields['inflight']: raise ValueError('overflow/inflight')
    if not 0 < fields['task_bytes'] <= 64 or not 0 < fields['storage_bytes'] <= 65536: raise ValueError('storage budget')
    if not fields['owner'] or not fields['timebase_hz'] or fields['end_ticks'] < fields['start_ticks']: raise ValueError('invalid timing/owner')
    if fields['resolution_ns_numerator'] != 1000000000 or fields['resolution_ns_denominator'] != fields['timebase_hz']: raise ValueError('clock resolution')
    for lane in LANES:
        for name,unit,hist in metrics:
            p=lane+'.'+name+'.'; count=fields[p+'samples']; value=fields[p+'value']; maximum=fields[p+'max']
            if (not count and (value or maximum)) or maximum > value or value > count * maximum: raise ValueError('counter consistency: '+p)
            if hist:
                bins=[fields[p+'bucket.'+str(i)] for i in range(65)]
                if sum(bins) != count: raise ValueError('histogram sample count: '+p)
                low=sum(n*(0 if i==0 else 1<<(i-1)) for i,n in enumerate(bins))
                high=sum(n*min(maximum,0 if i==0 else (1<<i)-1) for i,n in enumerate(bins))
                if not low <= value <= high: raise ValueError('histogram sum: '+p)
                if count:
                    maximum_bucket=maximum.bit_length()
                    highest=max(i for i,n in enumerate(bins) if n)
                    if highest!=maximum_bucket:raise ValueError('histogram maximum: '+p)
                    minimum=0 if highest==0 else 1<<(highest-1)
                    if value<low-minimum+maximum:raise ValueError('maximum sample absent: '+p)
    return fields

def percentile(bins, quantile):
    total=sum(bins)
    if not total: return None
    target=max(1,math.ceil(total*quantile)); count=0
    for i,n in enumerate(bins):
        count+=n
        if count >= target: return (0,0) if i==0 else (1<<(i-1),(1<<i)-1)

def validate_expected(snapshot, expected):
    for key,value in expected.items():
        if snapshot.get(key) != value: raise ValueError(f'independent expected {key}: {snapshot.get(key)} != {value}')

def validate_allocator(timings, snapshots, output, diagnostics):
    expected={f'allocator-{pages}-{count}':(pages,count,pages*count*90)
              for pages,count in ((1,1024),(64,64),(4096,4))}
    work=re.findall(r'^COST ALLOCATOR WORK (\S+) (\d+) (\d+) (\d+)$',output,re.M)
    if len(work)!=3 or {name for name,*_ in work}!=set(expected):
        raise ValueError('allocator work missing/duplicate')
    for name,pages,count,checksum in work:
        if tuple(map(int,(pages,count,checksum)))!=expected[name]:
            raise ValueError('allocator work/content mismatch')
    if set(timings)!=set(expected) or any(value<=0 for value in timings.values()):
        raise ValueError('allocator timing coverage')
    closing=re.findall(r'^COST CLOSING (\S+) (\d+) (\d+)$',output,re.M)
    if (diagnostics and (len(closing)!=3 or {name for name,*_ in closing}!=set(expected))) or (not diagnostics and closing):
        raise ValueError('allocator closing coverage')
    if (diagnostics and (len(snapshots)!=3 or {s['name'] for s in snapshots}!=set(expected))) or (not diagnostics and snapshots):
        raise ValueError('allocator snapshot coverage')
    available=[]
    for snapshot in snapshots:
        values=snapshot['values'];pages,count,_=expected[snapshot['name']]
        if values['foreground.page_accepted.value']<pages*count or not values['foreground.irq_off_ticks.samples']:
            raise ValueError('allocator pages/IRQ work absent')
        metrics=('page_meta_checked','page_meta_written','allocator_meta_ticks')
        present=[f'foreground.{name}.value' in values for name in metrics]
        if any(present) and not all(present):raise ValueError('partial allocator schema')
        available.append(all(present))
        if all(present) and any(not values[f'foreground.{name}.value'] for name in metrics):
            raise ValueError('allocator metadata work absent')
    if available and len(set(available))!=1:raise ValueError('changed allocator registry')
    return {'work':[dict(name=name,pages=int(pages),iterations=int(count),checksum=int(checksum))
                    for name,pages,count,checksum in work],
            'closing':[dict(name=name,ns=int(ns),retries=int(retries)) for name,ns,retries in closing],
            'metadata_available':bool(available and available[0])}

def validate_deadline(name, snapshot):
    _, unrelated, mode, _ = name.split('-')
    if int(mode) == 0:
        lanes=LANES[:2]
        samples=sum(snapshot[lane+'.deadline_visits.samples'] for lane in lanes)
        visits=sum(snapshot[lane+'.deadline_visits.value'] for lane in lanes)
        expired=sum(snapshot[lane+'.deadline_expired.value'] for lane in lanes)
        # 到期工作必须与到期数一致，而不是与含 N 个无期限成员的阻塞总数一致。
        if not samples or visits != expired or expired < 4:
            raise ValueError('deadline work does not track expirations: '+name)

def validate_replicas(records):
    """Three independent launches per immutable configuration, with complete identical windows."""
    groups={}
    for record in records:
        key=tuple(record[k] for k in ('case','cost_diagnostics','transport','cache'))+(record.get('two_disks',False),record.get('platform','boaros'))
        groups.setdefault(key,[]).append(record)
    if not groups: raise ValueError('empty measurement')
    for key,rows in groups.items():
        if len(rows)!=3 or {r['replica'] for r in rows}!={0,1,2}: raise ValueError('three distinct boot replicas required')
        for field in ('kernel_sha256','elf_sha256','source_sha256'):
            if len({r[field] for r in rows})!=1: raise ValueError('changed immutable input: '+field)
        if len({r['fixture_sha256'] for r in rows})!=3: raise ValueError('independent fixtures required')
        if any(r['replicas']!=3 or not r['acceptance'] for r in rows): raise ValueError('pilot is not acceptance')
        if len({tuple(sorted(r['timings_ns'])) for r in rows})!=1: raise ValueError('missing workload phase')
        for row in rows:
            if row['cost_diagnostics'] and not row['snapshots']: raise ValueError('missing snapshots')
            if not row['cost_diagnostics'] and row['snapshots']: raise ValueError('unexpected diagnostic snapshots')
            build=row.get('kernel_build_identity',{})
            if build and build.get('kernel_sha256')!=row['kernel_sha256']:raise ValueError('kernel build identity mismatch')
            registry=row.get('metric_schema');frozen=build.get('metric_schema')
            registry=[tuple(metric) for metric in registry] if registry is not None else None
            frozen=[tuple(metric) for metric in frozen] if frozen is not None else None
            if registry is not None and frozen is not None and registry!=frozen:raise ValueError('kernel metric registry mismatch')
            if registry is None:registry=frozen
            seen=set(); previous_epoch=0
            for snapshot in row['snapshots']:
                if snapshot['name'] in seen: raise ValueError('duplicate window')
                seen.add(snapshot['name'])
                text=''.join(f'{k}={v}\n' for k,v in snapshot['values'].items())
                parsed=parse(text,snapshot['values']['epoch'],registry)
                if row['case']=='deadline':validate_deadline(snapshot['name'],parsed)
                if parsed['epoch']<=previous_epoch:raise ValueError('reused/stale boot epoch')
                previous_epoch=parsed['epoch']
            if row['cost_diagnostics'] and row['case'] in ('contract','write','locking','mprotect','deadline','latency','consumer','io-pressure','allocator'):
                expected_epochs=[1,2,3,5] if row['case']=='contract' else list(range(1,len(row['snapshots'])+1))
                if [s['values']['epoch'] for s in row['snapshots']]!=expected_epochs:raise ValueError('boot epoch sequence')
            if row['cost_diagnostics'] and row['timings_ns'] and seen!=set(row['timings_ns']): raise ValueError('window coverage')
    return groups

def validate_locking(timings, snapshots, diagnostics):
    required = {f'locking-{relationship}-0-{waiters}'
                for relationship in range(3) for waiters in (1, 8, 32)}
    required.update(f'locking-1-{operation}-8' for operation in (1, 2, 3, 4))
    if set(timings) != required or (diagnostics and {s['name'] for s in snapshots} != required):
        raise ValueError('locking window coverage')
    if diagnostics:
        for snapshot in snapshots:
            values = snapshot['values']
            if values['foreground.lock15_acquired.value'] == 0:
                raise ValueError('write operation lock missing')
            # 普通热写可能全程无睡眠；同步持锁等待和truncate才是强制竞争窗口。
            if snapshot['name'] in ('locking-1-2-8', 'locking-1-4-8') and values['foreground.lock15_blocks.value'] == 0:
                raise ValueError('synchronous same inode contention was not exercised')

def validate_resume_selection(case, record, selected):
    if case not in ('consumer', 'metadata'):
        return
    expected = ['consumer-musl-1', 'consumer-glibc-1'] if case == 'metadata' else selected
    if record.get('consumer_command_names') != expected:
        raise ValueError('resume command selection differs')
