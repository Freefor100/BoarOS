"""Pinned original iozone fixture and per-command results (script status is never proof)."""
from pathlib import Path
import re
import math
import subprocess
ORIGINAL_SHA='f419468678d342133546add2f8459ea09aeba987ba968e28753d6ee656996b8b'
ELFS={'musl':'019cd6e219263f41b3c1d62024ea9ce38e613541bf2a1d9b7e2aa6506187ecd6','glibc':'984c8ad474072011f38d52a98d422c581b2277d3e2d03f90557f45a8046c66de'}
SCRIPTS={'musl':'0ef595da41baaa5a7fd598f2210ebf70936802938ac1db6e933a2877245efa60','glibc':'6ece77b9527b5cc79ccd69ede7e574ea1e1c0a394e46ff3ea455e7fa232a303b'}
GROUPS=(None,(0,1),(0,2),(0,3),(0,5),(6,7),(9,10),(11,12))
DEPENDENCIES={'/glibc/lib/libc.so.6':'81af558241962fadf1d0199171be78dd2bb1e45098b8f7f786b22881b7bab33c',
    '/glibc/lib/ld-linux-riscv64-lp64d.so.1':'10ac4a073b52a6b05e383ae6b47be92375436174a857fd6299faea1257495eaa',
    '/musl/lib/libc.so':'a174c80743882436816923d3afd8ca4a69ca92a89c88332ac95334052e2698bf'}
def originals(root,work,digest):
    original=root/'references/oscomp-autotest/sdcard-rv.img'
    if digest(original)!=ORIGINAL_SHA:raise ValueError('original image changed')
    deps={}
    for libc in ('musl','glibc'):
        for name in ('iozone','iozone_testcode.sh'):
            path=work/(libc+'-'+name)
            subprocess.run(['debugfs','-R',f'dump /{libc}/{name} {path}',str(original)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            expected=ELFS[libc] if name=='iozone' else SCRIPTS[libc]
            if digest(path)!=expected:raise ValueError('original ELF/script changed: '+str(path))
    # Preserve the complete original filesystem, including both original loaders/libcs.
    for source in ('/glibc/lib/libc.so.6','/glibc/lib/ld-linux-riscv64-lp64d.so.1','/musl/lib/libc.so'):
        path=work/('dep-'+source.replace('/','_'))
        subprocess.run(['debugfs','-R',f'dump {source} {path}',str(original)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        if not path.is_file():raise ValueError('missing original dependency '+source)
        deps[source]=digest(path)
    return original,dict(original_sha256=ORIGINAL_SHA,consumer_elf_sha256=ELFS,consumer_script_sha256=SCRIPTS,consumer_dependencies=deps)
def framed_line(line):
    # An interrupted original program may leave its last row without a newline.
    match=re.search(r'COST RESULT consumer-(?:musl|glibc)-[0-7] [0-9]+$',line)
    return match.group(0) if match else line

def classify(raw, timeout, status):
    marker='iozone test complete.' in raw
    available='Selected test not available on the version.' not in raw
    process_completed=not timeout and status==0 and marker
    outcome='completed' if process_completed and available else 'blocked'
    reason=('selected_tests_unavailable' if not available else 'timeout' if timeout else
            'exit_status' if status else 'no_completion_marker' if not marker else 'completed')
    return dict(completion_marker=marker,process_completed=process_completed,
                requested_tests_available=available,outcome=outcome,reason=reason)

def parse_throughput_methods(raw):
    """Keep original kB (KiB) results and estimate intervals from rounded -C rows.

    Children is the sum of independently timed child rates. Only complete child
    transfer counts supply the numerator for the separately timed Parent rate.
    Missing/zero progress cannot establish an interval and remains unknown.
    """
    methods=[]
    current=None
    seen=set()

    def number(value):
        result=float(value)
        if not math.isfinite(result) or result<0:
            raise ValueError('invalid original throughput value')
        return result

    def finish():
        if current is None:return
        children=current['children']
        count=current['child_count']
        if children is not None and len(children)==count:
            # Each printed rate and their independently printed sum round to
            # two decimals; summing the displayed children is not exact proof.
            summed=sum(c['rate_kib_s'] for c in children)
            if abs(summed-current['children_sum_kib_s'])>0.005*(count+1)+1e-9:
                raise ValueError('child throughput sum differs from original header')
            current['counted_kib']=sum(c['actual_kib'] for c in children)
            parent=current['parent_kib_s']
            if current['counted_kib']>0 and parent is not None and parent>0:
                current['parent_interval_seconds']=current['counted_kib']/parent
        methods.append(current)

    for line in raw.splitlines():
        line=line.strip()
        header=re.fullmatch(r'(Children see|Parent sees) throughput for\s+(\d+)\s+(.+?)\s*=\s*(\S+)\s+kB/sec',line)
        if header:
            who,count,method,value=header.groups()
            count=int(count);method=method.strip()
            if count<1:raise ValueError('invalid original child count')
            value=number(value)
            if who=='Children see':
                finish()
                if method in seen:raise ValueError('duplicate original throughput method')
                seen.add(method)
                current=dict(method=method,child_count=count,children_sum_kib_s=value,
                    parent_kib_s=None,min_kib_s=None,max_kib_s=None,avg_kib_s=None,
                    min_xfer_kib=None,children=None,counted_kib=None,parent_interval_seconds=None)
            else:
                if current is None or current['method']!=method or current['child_count']!=count:
                    raise ValueError('unpaired original Parent throughput')
                if current['parent_kib_s'] is not None:raise ValueError('duplicate original Parent throughput')
                current['parent_kib_s']=value
            continue
        summary=re.fullmatch(r'(Min|Max|Avg) throughput per (?:process|thread)\s*=\s*(\S+)\s+kB/sec',line)
        xfer=re.fullmatch(r'Min xfer\s*=\s*(\S+)\s+kB',line)
        if summary or xfer:
            if current is None:raise ValueError('unpaired original throughput summary')
            key=summary[1].lower()+'_kib_s' if summary else 'min_xfer_kib'
            if current[key] is not None:raise ValueError('duplicate original throughput summary')
            current[key]=number(summary[2] if summary else xfer[1])
            continue
        child=re.match(r'Child\[(\d+)\] xfer count\s*=\s*(\S+)\s+kB,\s*Throughput\s*=\s*(\S+)\s+kB/sec(?:$|,)',line)
        if child:
            if current is None:raise ValueError('unpaired original child throughput')
            index=int(child[1]);actual=number(child[2]);rate=number(child[3])
            if current['children'] is None:current['children']=[]
            children=current['children']
            if index>=current['child_count'] or any(c['child']==index for c in children):
                raise ValueError('duplicate/invalid original child index')
            children.append(dict(child=index,actual_kib=actual,rate_kib_s=rate,
                interval_seconds=actual/rate if actual>0 and rate>0 else None))
            continue
        if line.startswith('Child['):raise ValueError('invalid original child throughput row')
    finish()
    return methods

def commands(output, expected_budget_ms=180000, expected_names=None):
    policies=re.findall(r'^COST CONSUMER BUDGET ([^\n]+)$',output,re.M)
    if len(policies)>1:raise ValueError('duplicate consumer budget')
    # Older sealed records used the fixed 180 s policy without a header.
    budget=int(policies[0]) if policies else 180000
    if not 1000<=budget<=3600000 or budget!=expected_budget_ms:
        raise ValueError('consumer budget differs from frozen input')
    result=[]
    for libc in ('musl','glibc'):
        for index,group in enumerate(GROUPS):
            name=f'consumer-{libc}-{index}'
            if expected_names is not None and name not in expected_names:continue
            start=f'COST COMMAND BEGIN {name}\n';finish=re.findall(r'^COST COMMAND RESULT '+name+r' (\d+) (\d+)$',output,re.M)
            if output.count(start)!=1 or len(finish)!=1:raise ValueError('missing/duplicate original command '+name)
            timeout,status=map(int,finish[0]);raw=output.split(start,1)[1].split('COST RESULT '+name+' ',1)[0]
            argv=['./iozone','-a','-r','1k','-s','4m'] if group is None else ['./iozone','-t','4','-i',str(group[0]),'-i',str(group[1]),'-r','1k','-s','1m']
            sections=[line.strip() for line in raw.splitlines() if 'throughput for' in line or 'Initial write' in line or 'Re-write' in line]
            timing=re.findall(r'^COST COMMAND TIMING '+name+r' (\d+) (\d+)$',output,re.M)
            if len(timing)>1:raise ValueError('duplicate command timing '+name)
            split_timing=dict(zip(('program_ns','drain_ns'),map(int,timing[0]))) if timing else {}
            result.append(dict(command_timeout_ms=budget,reported_sections=sections,
                throughput_methods=parse_throughput_methods(raw),name=name,elf_sha256=ELFS[libc],argv=argv,
                cwd='/'+libc,timeout=timeout,wait_status=status,raw_output=raw,**split_timing,**classify(raw,timeout,status)))
    return result

def validate_method_results(raw, index):
    """Verify original result fields for the methods requested by GROUPS."""
    if index==0:
        rows=re.findall(r'^\s*4096[ \t]+1[ \t]+([^\n]+)$',raw,re.M)
        if len(rows)!=1:raise ValueError('missing/duplicate automatic result row')
        values=rows[0].split()
        if len(values)!=13 or any(not math.isfinite(float(v)) or float(v)<=0 for v in values):
            raise ValueError('invalid automatic throughput results')
        return
    if index==7:return  # Fixed ELF version exclusion, not native vector acceptance.
    requested=((),('initial writers','rewriters','readers','re-readers'),
               ('initial writers','rewriters','random readers','random writers'),
               ('initial writers','rewriters','reverse readers'),
               ('initial writers','rewriters','stride readers'),
               ('fwriters','freaders'),('pwrite writers','pread readers'))[index]
    results={};method=None
    for line in raw.splitlines():
        child=re.search(r'Children see throughput for\s+4\s+(.+?)\s*=\s*(\S+)\s+kB/sec',line)
        if child:
            if method is not None:raise ValueError('missing per-process throughput')
            method=child[1]
            if method in results or not math.isfinite(float(child[2])) or float(child[2])<=0:
                raise ValueError('duplicate/invalid method throughput')
        maximum=re.search(r'Max throughput per process\s*=\s*(\S+)\s+kB/sec',line)
        if maximum:
            if method is None:raise ValueError('unpaired per-process throughput')
            value=float(maximum[1])
            if not math.isfinite(value) or value<=0:raise ValueError('invalid per-process throughput')
            results[method]=value;method=None
    if method is not None or set(results)!=set(requested):raise ValueError('requested method output incomplete')

def validate_record(row, require_completion=False):
    if (row.get('original_sha256')!=ORIGINAL_SHA or row.get('consumer_elf_sha256')!=ELFS or
        row.get('consumer_script_sha256')!=SCRIPTS or row.get('consumer_dependencies')!=DEPENDENCIES):
        raise ValueError('original consumer or dependency changed')
    all_names={f'consumer-{libc}-{i}' for libc in ELFS for i in range(8)}
    expected_names=set(row.get('consumer_command_names',all_names))
    if not expected_names or not expected_names<=all_names:raise ValueError('unknown selected original command')
    rows=row.get('commands',[])
    if len(rows)!=len(expected_names) or {c['name'] for c in rows}!=expected_names:raise ValueError('original command coverage')
    budget=row.get('consumer_timeout_ms',180000)
    if not isinstance(budget,int) or not 1000<=budget<=3600000:raise ValueError('invalid frozen consumer budget')
    for c in rows:
        _,libc,index=c['name'].split('-'); group=GROUPS[int(index)]
        argv=['./iozone','-a','-r','1k','-s','4m'] if group is None else ['./iozone','-t','4','-i',str(group[0]),'-i',str(group[1]),'-r','1k','-s','1m']
        if c['argv']!=argv or c['cwd']!='/'+libc or c['elf_sha256']!=ELFS[libc] or c['command_timeout_ms']!=budget:
            raise ValueError('original argv/ELF/cwd/budget changed')
        actual=classify(c['raw_output'],c['timeout'],c['wait_status'])
        if c['timeout'] not in (0,1) or any(c.get(k)!=v for k,v in actual.items()):raise ValueError('incorrect completion classification')
        if 'throughput_methods' in c and c['throughput_methods']!=parse_throughput_methods(c['raw_output']):
            raise ValueError('parsed original throughput differs from raw output')
        if require_completion:
            # The fixed Linux reference independently rejects only (11,12).
            if not actual['process_completed'] or (index!='7' and actual['outcome']!='completed'):
                raise ValueError('original consumer did not finish: '+c['name'])
            if index=='7' and actual['reason']!='selected_tests_unavailable':raise ValueError('reference exclusion changed')
            validate_method_results(c['raw_output'],int(index))
