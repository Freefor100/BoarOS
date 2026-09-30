#!/usr/bin/env python3
"""Verify and seal the original consumer follow-up; incomplete work cannot pass."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
from cost_consumer import validate_record
from cost_report import validate_replicas

spec=importlib.util.spec_from_file_location('cost_evidence',Path(__file__).with_name('cost-evidence.py'))
evidence=importlib.util.module_from_spec(spec);spec.loader.exec_module(evidence)

def validate(records):
    groups=validate_replicas(records)
    required={('consumer',on,'modern','writeback',False,'boaros') for on in (0,1)}
    required.add(('consumer',0,'modern','writeback',False,'linux'))
    if set(groups)!=required:raise ValueError('original consumer follow-up matrix missing/extra configuration')
    if len({r['elf_sha256'] for r in records})!=1:raise ValueError('coordinator ELF differs between comparisons')
    boaros=[r for r in records if r['platform']=='boaros']
    if len({r['source_sha256'] for r in boaros})!=1:raise ValueError('production source differs between ON/OFF')
    for field in ('consumer_timeout_ms','qemu_sha256','firmware_sha256','timebase_hz'):
        if len({r[field] for r in records})!=1:raise ValueError('comparison input differs: '+field)
    for row in records:
        validate_record(row,require_completion=True)
        names={c['name'] for c in row['commands']}
        if set(row['timings_ns'])!=names or any(not isinstance(t,int) or t<=0 for t in row['timings_ns'].values()):
            raise ValueError('missing/invalid original command duration')
        if row['cost_diagnostics'] and {s['name'] for s in row['snapshots']}!=names:raise ValueError('missing consumer cost window')
        frozen={k:row[k] for k in row.get('input_keys',[])}
        encoded=json.dumps(frozen,sort_keys=True,separators=(',',':'))+'\n'
        if not frozen or hashlib.sha256(encoded.encode()).hexdigest()!=row.get('input_sha256'):raise ValueError('boot input manifest changed/missing')
        for field in ('firmware_sha256','dtb_sha256','qemu_sha256','fixture_sha256','source_sha256'):
            if not row.get(field):raise ValueError('unfrozen boot input: '+field)
        if row.get('uname',{}).get('machine')!='riscv64':raise ValueError('unexpected actual guest architecture')
        if row.get('platform')=='linux':
            if row['linux_reference']['inputs']['source'][1]!='f4cdf7ca9a1fdcca413157df19753f388a5a224e':raise ValueError('Linux reference changed')
        elif row['uname']['release']!='4.15.0':raise ValueError('consumer follow-up did not use the evaluation compatibility profile')
    return groups

def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('inputs',nargs='+',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--verify',action='store_true',help='verify a sealed follow-up without rewriting it')
    args=parser.parse_args()
    if args.verify:
        if len(args.inputs)!=1 or args.output:parser.error('--verify needs exactly one document and no output')
        document=json.loads(args.inputs[0].read_text())
        if document.get('original_consumer_closure') is not True:raise ValueError('not an accepted consumer follow-up')
        rows=evidence.unpack(document)
    else:
        rows=evidence.collect([json.loads(path.read_text()) for path in args.inputs])
    groups=validate(rows)
    if not args.verify:
        if not args.output:parser.error('--output required for sealing')
        document=evidence.pack(rows)
        document.update(original_consumer_closure=True,scope='9 serial independent RV boots; original group (11,12) unavailable on both platforms')
        validate(evidence.unpack(document))
        args.output.write_text(json.dumps(document,sort_keys=True,separators=(',',':'))+'\n')
    print(f'original consumer follow-up accepted: {len(rows)} boots, 14/16 requested groups completed per boot; two fixed-reference exclusions')
    for key,boots in groups.items():
        print(f'{key}: '+', '.join(str(sum(b["timings_ns"].values())//1000000000)+'s' for b in boots))

if __name__=='__main__':main()
