#!/usr/bin/env python3
"""Validate serial three-boot records and print reproducible per-window distributions."""
import argparse
import json
from pathlib import Path
from statistics import median
from cost_report import validate_replicas

def main():
    p=argparse.ArgumentParser(__doc__); p.add_argument('records',nargs='+',type=Path); args=p.parse_args()
    groups=validate_replicas([r for path in args.records for r in json.loads(path.read_text())])
    for key,rows in groups.items():
        print('configuration:',key)
        print('kernel:',rows[0]['kernel_sha256'],'ELF:',rows[0]['elf_sha256'],'source:',rows[0]['source_sha256'])
        for name in sorted(rows[0]['timings_ns']):
            ns=[r['timings_ns'][name] for r in rows]
            print(name,'guest ns min/median/max:',min(ns),median(ns),max(ns))
            if key[1]:
                values=[next(s['values'] for s in r['snapshots'] if s['name']==name) for r in rows]
                for metric in sorted(values[0]):
                    if metric.endswith('.value') and any(v[metric] for v in values):
                        print(' ',metric,'min/median/max:',min(v[metric] for v in values),median(v[metric] for v in values),max(v[metric] for v in values))
if __name__=='__main__': main()
