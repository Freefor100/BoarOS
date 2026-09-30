"""Pinned original iozone fixture and per-command results (script status is never proof)."""
from pathlib import Path
import re
import subprocess
ORIGINAL_SHA='f419468678d342133546add2f8459ea09aeba987ba968e28753d6ee656996b8b'
ELFS={'musl':'019cd6e219263f41b3c1d62024ea9ce38e613541bf2a1d9b7e2aa6506187ecd6','glibc':'984c8ad474072011f38d52a98d422c581b2277d3e2d03f90557f45a8046c66de'}
SCRIPTS={'musl':'0ef595da41baaa5a7fd598f2210ebf70936802938ac1db6e933a2877245efa60','glibc':'6ece77b9527b5cc79ccd69ede7e574ea1e1c0a394e46ff3ea455e7fa232a303b'}
GROUPS=(None,(0,1),(0,2),(0,3),(0,5),(6,7),(9,10),(11,12))
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
