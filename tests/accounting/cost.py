#!/usr/bin/env python3
"""Compare fixed native workloads serially on supplied BoarOS kernels."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/diff-abi'))
import harness
CASES={'compute':3953732515821365571,'tmpfs-small-io':640000,
       'sqlite-memory':1250025000,'sqlite-commit':12502540}

def measurements(raw):
    rows=re.findall(r'ACCOUNT COST ([a-z-]+) ns=(\d+) check=(\d+)',raw)
    if [name for name,_,_ in rows]!=list(CASES) or raw.count('ACCOUNT END 4')!=1 or 'ACCOUNT FAIL' in raw:
        raise RuntimeError('incomplete native cost workload')
    if any(int(check)!=CASES[name] for name,_,check in rows):
        raise RuntimeError('native workload content differs')
    return {name:int(ns) for name,ns,_ in rows}


TIMER_MODES=('off','virtual-armed','prof-armed','virtual-periodic','prof-periodic')
TIMER_CASES={'getpid':1000000,'compute':17117817320143602816,'tmpfs-small-io':5120000,
             'compute-threads':13183116052861372952,'compute-processes':13183116052861372952}

def timer_measurements(raw,mode,expected=None,cpus=1):
    if mode not in TIMER_MODES or cpus not in (1,2):
        raise RuntimeError('invalid timer measurement profile')
    expected=list(TIMER_CASES) if expected is None else list(expected)
    lines=[line[line.index('ACCOUNT TIMER '):] for line in raw.replace('\r','').splitlines() if 'ACCOUNT TIMER ' in line]
    if (len(lines)!=len(expected)+2 or lines[0]!=f'ACCOUNT TIMER BEGIN mode={mode} cpus={cpus}' or
            lines[-1]!=f'ACCOUNT TIMER END {len(expected)}' or 'ACCOUNT FAIL' in raw):
        raise RuntimeError('incomplete timer cost workload')
    values={}
    for name,line in zip(expected,lines[1:-1]):
        match=re.fullmatch(r'ACCOUNT TIMER ([a-z-]+) wall_ns=(\d+) user_ns=(\d+) system_ns=(\d+) signals=(\d+) check=(\d+) workers=(\d+) cpu_mask=(\d+) overlap=(\d+)',line)
        if not match or match.group(1)!=name:
            raise RuntimeError('unknown, reordered or malformed timer workload')
        row=dict(zip(('wall_ns','user_ns','system_ns','signals','check','workers','cpu_mask','overlap'),map(int,match.groups()[1:])))
        parallel=name in ('compute-threads','compute-processes')
        if (row['check']!=TIMER_CASES[name] or not row['wall_ns'] or row['workers']!=(8 if parallel else 1) or
                row['cpu_mask']!=((1<<cpus)-1 if parallel else 1) or row['overlap']!=int(parallel and cpus==2)):
            raise RuntimeError('timer workload content, CPU assignment or completion differs')
        if mode in ('off','virtual-armed','prof-armed') and row['signals']:
            raise RuntimeError('disabled or nonexpiring timer delivered a signal')
        charged=row['user_ns']+(row['system_ns'] if mode.startswith('prof-') else 0)
        if mode.endswith('-periodic') and charged>=100000000 and not row['signals']:
            raise RuntimeError('periodic timer did not deliver during sufficient CPU work')
        values[name]=row
    return values

def timer_run_plan(labels,timer_kernel,repeat):
    plan=[(label,mode) for label in labels for mode in (TIMER_MODES if label==timer_kernel else ('off',))]
    shift=repeat%len(plan)
    return plan[shift:]+plan[:shift]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch',choices=('riscv','loongarch'),default='riscv')
    parser.add_argument('--kernel',action='append',required=True,metavar='LABEL=PATH')
    parser.add_argument('--program',type=Path,required=True)
    parser.add_argument('--sqlite',type=Path,help='required for the legacy SQLite cost suite')
    parser.add_argument('--timer-suite',action='store_true',help='same-ELF fixed CPU timer workloads instead of legacy SQLite')
    parser.add_argument('--timer-kernel',help='kernel label receiving all five modes; other labels run off')
    parser.add_argument('--cpus',type=int,choices=(1,2),default=1)
    parser.add_argument('--qemu',help='explicit QEMU binary; default follows the native architecture profile')
    parser.add_argument('--firmware',type=Path,help='explicit RV OpenSBI firmware')
    parser.add_argument('--repeat',type=int,default=5)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.arch=='riscv' and args.firmware is None:parser.error('RV requires --firmware')
    if args.repeat<1:parser.error('repeat must be positive')
    if not args.timer_suite and args.sqlite is None:parser.error('legacy suite requires --sqlite')
    if args.cpus==2 and (args.arch!='riscv' or not args.timer_suite):parser.error('two CPUs require RV timer suite and a real USER_SMP kernel')
    sources={}
    for value in args.kernel:
        label,separator,path=value.partition('=')
        if not separator or not re.fullmatch(r'[a-z][a-z0-9-]*',label) or label in sources:
            parser.error('kernel labels must be unique literal names')
        sources[label]=Path(path)
    if args.timer_suite:
        args.timer_kernel=args.timer_kernel or ('after' if 'after' in sources else next(iter(sources)))
        if args.timer_kernel not in sources:parser.error('--timer-kernel must name a supplied kernel')
    elif args.timer_kernel:parser.error('--timer-kernel requires --timer-suite')
    if args.output.exists() and any(args.output.iterdir()):parser.error('output must be new or empty')
    args.output.mkdir(parents=True,exist_ok=True);base=args.output.resolve()
    profile=harness.PROFILES[args.arch];kernels={}
    for label,source in sources.items():
        kernels[label]=base/(label+'-kernel');shutil.copyfile(source,kernels[label])
    program=base/'program'
    shutil.copyfile(args.program,program)
    fixture=harness.fixture(base,program)
    if not args.timer_suite:
        sqlite=base/'sqlite3';shutil.copyfile(args.sqlite,sqlite)
        commands=base/'sqlite.debugfs';commands.write_text(f'write "{sqlite}" /sqlite3\nset_inode_field /sqlite3 mode 0100755\n')
        harness.run_logged(['debugfs','-w','-f',str(commands),str(fixture)],base/'sqlite-install.log')
    qemu=Path(shutil.which(args.qemu or profile.qemu) or args.qemu or profile.qemu).resolve()
    report={'host_affinity':sorted(os.sched_getaffinity(0)),'arch':args.arch,'status':'running','program':harness.digest(program),'sqlite':harness.digest(sqlite) if not args.timer_suite else None,'suite':'timers' if args.timer_suite else 'legacy','guest_cpus':args.cpus,'runner_sha256':harness.digest(__file__),
        'firmware':{'path':str(args.firmware.resolve()),'sha256':harness.digest(args.firmware)} if args.firmware else None,
        'kernels':{label:harness.digest(path) for label,path in kernels.items()},
        'qemu':{'path':str(qemu),'sha256':harness.digest(qemu),'version':harness.output([str(qemu),'--version'])},'runs':[]}
    try:
        for repeat in range(args.repeat):
            # 每个轮次轮换全部组合；相同旧核control仍是独立启动。
            plan=timer_run_plan(list(kernels),args.timer_kernel,repeat) if args.timer_suite else timer_run_plan(list(kernels),None,repeat)
            for label,mode in plan:
                name=f'{repeat}-{label}-{mode}' if args.timer_suite else f'{repeat}-{label}'
                disk=base/f'{name}.img';shutil.copyfile(fixture,disk)
                if args.timer_suite:
                    config=base/f'{name}.config';config.write_text(f'{mode} {args.cpus}\n')
                    batch=base/f'{name}.debugfs';batch.write_text(f'write "{config}" /account-timer\n')
                    harness.run_logged(['debugfs','-w','-f',str(batch),str(disk)],base/f'{name}-config.log')
                command=profile.boot(str(qemu),kernels[label],'1G')+['-net','none',
                    '-object','rng-random,id=entropy,filename=/dev/urandom','-device',profile.rng(),
                    '-drive',f'file={disk},if=none,format=raw,id=root','-device',profile.block('modern')]
                if args.arch=='riscv':
                    command[command.index('-bios')+1]=str(args.firmware.resolve())
                    command+=['-global','virtio-mmio.force-legacy=false']
                if args.cpus==2:
                    command[command.index('-smp')+1]='2'
                    command+=['-accel','tcg,thread=multi']
                log=base/f'{name}.log';harness.run_logged(command,log,120);raw=log.read_text(errors='replace')
                values=timer_measurements(raw,mode,cpus=args.cpus) if args.timer_suite else measurements(raw)
                if not profile.root_success(raw,42):raise RuntimeError('native workload resource release failed')
                if args.cpus==2:
                    sys.path.insert(0,str(ROOT/'tests/smp'))
                    from user import validate_execution
                    validate_execution(raw,2,True,require_parallel=True)
                harness.run_logged(['e2fsck','-fn',str(disk)],base/f'{name}-fsck.log')
                report['runs'].append({'repeat':repeat,'label':label,'mode':mode,'values':values,'command':command} if args.timer_suite else
                                      {'repeat':repeat,'label':label,'ns':values,'command':command})
                (base/'results.json').write_text(json.dumps(report,indent=2)+'\n')
                disk.unlink();print(label,mode,repeat,values,flush=True)
        if args.timer_suite:
            report['median']={label:{mode:{case:{field:statistics.median(row['values'][case][field] for row in report['runs']
                    if row['label']==label and row['mode']==mode) for field in ('wall_ns','user_ns','system_ns','signals')}
                    for case in TIMER_CASES} for mode in (TIMER_MODES if label==args.timer_kernel else ('off',))} for label in kernels}
            report['status']='passed'
        else:
            report['median_ns']={label:{case:statistics.median(row['ns'][case] for row in report['runs'] if row['label']==label)
                for case in CASES} for label in kernels};report['status']='passed'
    except BaseException as error:
        report.update(status='failed',error=str(error));raise
    finally:(base/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print('medians:',report.get('median',report.get('median_ns')),flush=True)
if __name__=='__main__':main()
