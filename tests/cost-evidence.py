#!/usr/bin/env python3
"""Pack complete verified cost records; preserve original command output and detect data loss."""
import argparse
import hashlib
import json
from pathlib import Path
from cost_report import schema,parse,validate_replicas

def seal(values):
    return hashlib.sha256(json.dumps(values,sort_keys=True,separators=(',',':')).encode()).hexdigest()
def validate_final(records):
    groups=validate_replicas(records)
    cases=('contract','write','locking','mprotect','deadline','latency','consumer')
    required={(case,on,'modern','writeback',False,'boaros') for case in cases for on in (0,1)}
    required|={('locking',1,'modern','writeback',True,'boaros'),('consumer',0,'modern','writeback',False,'linux')}
    required|={('io-pressure',1,t,c,False,'boaros') for t in ('legacy','modern') for c in ('writeback','writethrough')}
    if set(groups)!=required:raise ValueError('final matrix missing/extra configuration: '+str(required^set(groups)))
    names={
        'contract':{'contract','reuse','inflight','after-abort'},
        'write':{f'file-{c}-{n}' for c in ('cold','hot') for n in (0,1,3,63,64,65,4096)}|{f'{k}-1m-{a}' for k in ('file','tcp') for a in ('aligned','misaligned')}|{f'file-sync-{m}-every-{n}' for m in range(3) for n in (1,16,128)}|{'dgram-64k','dgram-oversize','file-osync','file-odsync','file-random','file-extend-accept','file-extend-sync','file-fault-prefix','file-fault-first'},
        'locking':{f'locking-{r}-0-{n}' for r in range(3) for n in (1,8,32)}|{f'locking-1-{o}-8' for o in (1,2,3,4)},
        'mprotect':{f'mprotect-{n}-{r}' for n in (16,64,256) for r in (0,16,64)}|{'mprotect-failure'},
        'deadline':{f'deadline-{n}-0-0' for n in (0,32,128,256)}|{f'deadline-32-{m}-0' for m in (1,2,3)}|{'deadline-0-0-1'},
        'latency':{f'latency-copy-{n}' for n in (4096,65536,1048576)}|{f'latency-protect-{n}' for n in (4096,1048576)},
        'consumer':{f'consumer-{libc}-{i}' for libc in ('musl','glibc') for i in range(8)},
        'io-pressure':{'pressure-io','timeout-cancel'}}
    for case in cases:
        on=groups[(case,1,'modern','writeback',False,'boaros')][0]
        off=groups[(case,0,'modern','writeback',False,'boaros')][0]
        if on['elf_sha256']!=off['elf_sha256']:raise ValueError('on/off workload ELF differs: '+case)
    from cost_consumer import ELFS, SCRIPTS, ORIGINAL_SHA, GROUPS
    for key, rows in groups.items():
        case,on,*_=key
        for row in rows:
            if set(row['timings_ns'])!=(set() if case=='contract' else names[case]):raise ValueError('final timing phases: '+case)
            if on and {s['name'] for s in row['snapshots']}!=names[case]:raise ValueError('final snapshot phases: '+case)
            if any(s['values']['mode']!=('fixture' if case=='io-pressure' else 'user') for s in row['snapshots']):raise ValueError('fixture/user attribution missing')
            if not row.get('firmware_sha256') or not row.get('dtb_sha256') or not row.get('qemu_sha256'):raise ValueError('unfrozen boot input')
            if case=='consumer':
                if row.get('original_sha256')!=ORIGINAL_SHA or row.get('consumer_elf_sha256')!=ELFS or row.get('consumer_script_sha256')!=SCRIPTS:raise ValueError('original consumer changed')
                commands=row.get('commands',[])
                if len(commands)!=16 or {c['name'] for c in commands}!=names[case]:raise ValueError('original command coverage')
                for c in commands:
                    _,libc,index=c['name'].split('-');g=GROUPS[int(index)]
                    argv=['./iozone','-a','-r','1k','-s','4m'] if g is None else ['./iozone','-t','4','-i',str(g[0]),'-i',str(g[1]),'-r','1k','-s','1m']
                    if c['argv']!=argv or c['cwd']!='/'+libc or c['elf_sha256']!=ELFS[libc]:raise ValueError('original argv/ELF/cwd changed')
                    marker='iozone test complete.' in c['raw_output']
                    outcome='completed' if not c['timeout'] and c['wait_status']==0 and marker else 'blocked'
                    if c['timeout'] not in (0,1) or c['completion_marker']!=marker or c['outcome']!=outcome:raise ValueError('incorrect command completion')
    return groups

def pack(records, final=False):
    (validate_final if final else validate_replicas)(records)
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
    return dict(version=1,complete_matrix=final,compression='all unspecified metrics and buckets are zero',metrics=metrics,records=rows)
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
    (validate_final if document.get('complete_matrix') else validate_replicas)(rows);return rows

def main():
    p=argparse.ArgumentParser(__doc__);p.add_argument('inputs',nargs='+',type=Path);p.add_argument('--output',type=Path);p.add_argument('--final',action='store_true',help='require all 60 boots and all original commands');args=p.parse_args()
    if args.output:
        rows=[r for path in args.inputs for r in json.loads(path.read_text())];document=pack(rows,args.final);unpack(document)
        args.output.write_text(json.dumps(document,ensure_ascii=False,separators=(',',':'))+'\n');print('verified evidence:',args.output)
    else:
        for path in args.inputs:print(path,len(unpack(json.loads(path.read_text()))),'verified boots')
if __name__=='__main__':main()
