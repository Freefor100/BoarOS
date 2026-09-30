#!/usr/bin/env python3
"""Validate serial three-boot records and print reproducible per-window distributions."""
import argparse
import json
from pathlib import Path
from statistics import median
from cost_report import validate_replicas, percentile

def main():
    p=argparse.ArgumentParser(__doc__); p.add_argument('records',nargs='+',type=Path); args=p.parse_args()
    records=[]
    for path in args.records:
        document=json.loads(path.read_text())
        if isinstance(document,dict):
            import importlib.util
            spec=importlib.util.spec_from_file_location('cost_evidence',Path(__file__).with_name('cost-evidence.py'))
            evidence=importlib.util.module_from_spec(spec);spec.loader.exec_module(evidence)
            records.extend(evidence.unpack(document))
        else:records.extend(document)
    groups=validate_replicas(records)
    for key,rows in groups.items():
        print('configuration:',key)
        print('kernel:',rows[0]['kernel_sha256'],'ELF:',rows[0]['elf_sha256'],'source:',rows[0]['source_sha256'])
        names=set(rows[0]['timings_ns'])|{s['name'] for s in rows[0]['snapshots']}
        for name in sorted(names):
            if name in rows[0]['timings_ns']:
                ns=[r['timings_ns'][name] for r in rows]
                print(name,'guest ns min/median/max:',min(ns),median(ns),max(ns))
            else:
                print(name,'snapshot window (no independent elapsed timer)')
            if key[1]:
                values=[next(s['values'] for s in r['snapshots'] if s['name']==name) for r in rows]
                for metric in sorted(values[0]):
                    if not metric.endswith('.value'):continue
                    prefix=metric[:-5]
                    if not any(v[prefix+'samples'] for v in values):continue
                    print(' ',metric,'unit:',values[0][prefix+'unit'],'min/median/max:',min(v[metric] for v in values),median(v[metric] for v in values),max(v[metric] for v in values),
                          'samples:',[v[prefix+'samples'] for v in values],'max:',[v[prefix+'max'] for v in values])
                    if prefix+'bucket.0' in values[0]:
                        for q in (.50,.95,.99):
                            print('  p'+str(round(q*100))+' bucket intervals:',[percentile([v[prefix+'bucket.'+str(i)] for i in range(65)],q) for v in values])
                spans=[v['end_ticks']-v['start_ticks'] for v in values]
                uncovered=[span-sum(v[lane+'.'+metric+'.value'] for lane in ('foreground','background') for metric in ('run_ticks','idle_ticks')) for span,v in zip(spans,values)]
                if any(n<0 for n in uncovered):raise ValueError('negative runtime partition')
                print('  window ticks:',spans,'uncovered ticks:',uncovered,'observer body subset:',[v['observer.observer_ticks.value'] for v in values])
if __name__=='__main__': main()
