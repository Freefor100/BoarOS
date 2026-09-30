"""Pinned original iozone fixture and per-command results (script status is never proof)."""
from pathlib import Path
import re
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

def commands(output, expected_budget_ms=180000):
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
            start=f'COST COMMAND BEGIN {name}\n';finish=re.findall(r'^COST COMMAND RESULT '+name+r' (\d+) (\d+)$',output,re.M)
            if output.count(start)!=1 or len(finish)!=1:raise ValueError('missing/duplicate original command '+name)
            timeout,status=map(int,finish[0]);raw=output.split(start,1)[1].split('COST RESULT '+name+' ',1)[0]
            argv=['./iozone','-a','-r','1k','-s','4m'] if group is None else ['./iozone','-t','4','-i',str(group[0]),'-i',str(group[1]),'-r','1k','-s','1m']
            sections=[line.strip() for line in raw.splitlines() if 'throughput for' in line or 'Initial write' in line or 'Re-write' in line]
            result.append(dict(command_timeout_ms=budget,reported_sections=sections,name=name,elf_sha256=ELFS[libc],argv=argv,cwd='/'+libc,timeout=timeout,wait_status=status,raw_output=raw,**classify(raw,timeout,status)))
    return result

def validate_record(row, require_completion=False):
    if (row.get('original_sha256')!=ORIGINAL_SHA or row.get('consumer_elf_sha256')!=ELFS or
        row.get('consumer_script_sha256')!=SCRIPTS or row.get('consumer_dependencies')!=DEPENDENCIES):
        raise ValueError('original consumer or dependency changed')
    expected_names={f'consumer-{libc}-{i}' for libc in ELFS for i in range(8)}
    rows=row.get('commands',[])
    if len(rows)!=16 or {c['name'] for c in rows}!=expected_names:raise ValueError('original command coverage')
    budget=row.get('consumer_timeout_ms',180000)
    if not isinstance(budget,int) or not 1000<=budget<=3600000:raise ValueError('invalid frozen consumer budget')
    for c in rows:
        _,libc,index=c['name'].split('-'); group=GROUPS[int(index)]
        argv=['./iozone','-a','-r','1k','-s','4m'] if group is None else ['./iozone','-t','4','-i',str(group[0]),'-i',str(group[1]),'-r','1k','-s','1m']
        if c['argv']!=argv or c['cwd']!='/'+libc or c['elf_sha256']!=ELFS[libc] or c['command_timeout_ms']!=budget:
            raise ValueError('original argv/ELF/cwd/budget changed')
        actual=classify(c['raw_output'],c['timeout'],c['wait_status'])
        if c['timeout'] not in (0,1) or any(c.get(k)!=v for k,v in actual.items()):raise ValueError('incorrect completion classification')
        if require_completion:
            # The fixed Linux reference independently rejects only (11,12).
            if not actual['process_completed'] or (index!='7' and actual['outcome']!='completed'):
                raise ValueError('original consumer did not finish: '+c['name'])
            if index=='7' and actual['reason']!='selected_tests_unavailable':raise ValueError('reference exclusion changed')
