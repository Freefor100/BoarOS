#!/usr/bin/env python3
"""Pack complete verified cost records; preserve original command output and detect data loss."""
import argparse
import hashlib
import json
from pathlib import Path
from cost_report import schema,parse,validate_replicas

def seal(values):
    return hashlib.sha256(json.dumps(values,sort_keys=True,separators=(',',':')).encode()).hexdigest()
def pack(records):
    validate_replicas(records)
    rows=[]
    metrics=schema()
    for record in records:
        row={k:v for k,v in record.items() if k!='snapshots'}; row['snapshots']=[]
        for snap in record['snapshots']:
            full=snap['values']; packed={k:v for k,v in full.items() if '.' not in k}; counters={}
            for lane in ('foreground','background','observer'):
                for name,unit,hist in metrics:
                    prefix=f'{lane}.{name}.';value=[full[prefix+s] for s in ('value','samples','max')]
                    if hist:value.append({str(i):full[prefix+'bucket.'+str(i)] for i in range(65) if full[prefix+'bucket.'+str(i)]})
                    if any(value[:3]):counters[lane+'.'+name]=value
            row['snapshots'].append(dict(name=snap['name'],header=packed,counters=counters,sha256=seal(full)))
        rows.append(row)
    return dict(version=1,compression='all unspecified metrics and buckets are zero',metrics=metrics,records=rows)
def unpack(document):
    if document['version']!=1 or [tuple(x) for x in document['metrics']]!=schema():raise ValueError('evidence schema/version')
    rows=[]
    for record in document['records']:
        row={k:v for k,v in record.items() if k!='snapshots'};row['snapshots']=[]
        for snap in record['snapshots']:
            full=dict(snap['header']);used=set()
            for lane in ('foreground','background','observer'):
                for name,unit,hist in schema():
                    key=lane+'.'+name;used.add(key);prefix=key+'.';value=snap['counters'].get(key,[0,0,0,{}] if hist else [0,0,0])
                    if len(value)!=(4 if hist else 3):raise ValueError('counter shape')
                    full[prefix+'unit']=unit
                    for i,s in enumerate(('value','samples','max')):full[prefix+s]=value[i]
                    if hist:
                        if any(not k.isdecimal() or not 0<=int(k)<=64 for k in value[3]):raise ValueError('bucket index')
                        for i in range(65):full[prefix+'bucket.'+str(i)]=value[3].get(str(i),0)
            if set(snap['counters'])-used:raise ValueError('unknown counter')
            if seal(full)!=snap['sha256']:raise ValueError('evidence lost/changed counters')
            parse(''.join(f'{k}={v}\n' for k,v in full.items()),full['epoch'])
            row['snapshots'].append(dict(name=snap['name'],values=full))
        rows.append(row)
    validate_replicas(rows);return rows

def main():
    p=argparse.ArgumentParser(__doc__);p.add_argument('inputs',nargs='+',type=Path);p.add_argument('--output',type=Path);args=p.parse_args()
    if args.output:
        rows=[r for path in args.inputs for r in json.loads(path.read_text())];document=pack(rows);unpack(document)
        args.output.write_text(json.dumps(document,ensure_ascii=False,separators=(',',':'))+'\n');print('verified evidence:',args.output)
    else:
        for path in args.inputs:print(path,len(unpack(json.loads(path.read_text()))),'verified boots')
if __name__=='__main__':main()
