"""Strict cost v1 snapshot reader; timestamps are guest CSR ticks, never wall-clock gates."""
from pathlib import Path
import math
import re

ROOT = Path(__file__).resolve().parents[1]
LANES = ('foreground', 'background', 'observer')
HEADER = ('version epoch state mode owner timebase_hz resolution_ns_numerator '
          'resolution_ns_denominator start_ticks end_ticks storage_bytes task_bytes overflow inflight').split()

def schema():
    return [(name,unit,int(hist)) for name,unit,hist in re.findall(
        r'^X\(\w+, (\w+), (\w+), ([01])\)$', (ROOT/'include/kernel/cost.def').read_text(), re.M)]

def parse(text, epoch):
    fields={}
    for line in text.splitlines():
        if not line or line.count('=') != 1: raise ValueError('malformed record')
        key,value=line.split('=')
        if key in fields: raise ValueError('duplicate key: '+key)
        fields[key]=value
    expected=set(HEADER)
    strings={'state','mode'}
    for lane in LANES:
        for name,unit,hist in schema():
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
        for name,unit,hist in schema():
            p=lane+'.'+name+'.'; count=fields[p+'samples']; value=fields[p+'value']; maximum=fields[p+'max']
            if (not count and (value or maximum)) or maximum > value: raise ValueError('counter consistency: '+p)
            if hist:
                bins=[fields[p+'bucket.'+str(i)] for i in range(65)]
                if sum(bins) != count: raise ValueError('histogram sample count: '+p)
                low=sum(n*(0 if i==0 else 1<<(i-1)) for i,n in enumerate(bins))
                high=sum(n*(0 if i==0 else (1<<i)-1) for i,n in enumerate(bins))
                if not low <= value <= high: raise ValueError('histogram sum: '+p)
                if count and not bins[0 if maximum==0 else maximum.bit_length()]: raise ValueError('histogram maximum: '+p)
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
