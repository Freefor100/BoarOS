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

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch',choices=('riscv','loongarch'),default='riscv')
    parser.add_argument('--kernel',action='append',required=True,metavar='LABEL=PATH')
    parser.add_argument('--program',type=Path,required=True)
    parser.add_argument('--sqlite',type=Path,required=True)
    parser.add_argument('--firmware',type=Path,help='explicit RV OpenSBI firmware')
    parser.add_argument('--repeat',type=int,default=5)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.arch=='riscv' and args.firmware is None:parser.error('RV requires --firmware')
    if args.repeat<1:parser.error('repeat must be positive')
    sources={}
    for value in args.kernel:
        label,separator,path=value.partition('=')
        if not separator or not re.fullmatch(r'[a-z][a-z0-9-]*',label) or label in sources:
            parser.error('kernel labels must be unique literal names')
        sources[label]=Path(path)
    if args.output.exists() and any(args.output.iterdir()):parser.error('output must be new or empty')
    args.output.mkdir(parents=True,exist_ok=True);base=args.output.resolve()
    profile=harness.PROFILES[args.arch];kernels={}
    for label,source in sources.items():
        kernels[label]=base/(label+'-kernel');shutil.copyfile(source,kernels[label])
    program=base/'program';sqlite=base/'sqlite3'
    shutil.copyfile(args.program,program);shutil.copyfile(args.sqlite,sqlite)
    fixture=harness.fixture(base,program)
    commands=base/'sqlite.debugfs';commands.write_text(f'write "{sqlite}" /sqlite3\nset_inode_field /sqlite3 mode 0100755\n')
    harness.run_logged(['debugfs','-w','-f',str(commands),str(fixture)],base/'sqlite-install.log')
    qemu=Path(shutil.which(profile.qemu) or profile.qemu).resolve()
    report={'host_affinity':sorted(os.sched_getaffinity(0)),'arch':args.arch,'status':'running','program':harness.digest(program),'sqlite':harness.digest(sqlite),
        'firmware':{'path':str(args.firmware.resolve()),'sha256':harness.digest(args.firmware)} if args.firmware else None,
        'kernels':{label:harness.digest(path) for label,path in kernels.items()},
        'qemu':{'path':str(qemu),'sha256':harness.digest(qemu),'version':harness.output([str(qemu),'--version'])},'runs':[]}
    try:
        for repeat in range(args.repeat):
            # 轮换顺序；每次独立盘、相同工作量和最终同步，正式计时保持串行。
            labels=list(kernels);labels=labels[repeat%len(labels):]+labels[:repeat%len(labels)]
            for label in labels:
                disk=base/f'{repeat}-{label}.img';shutil.copyfile(fixture,disk)
                command=profile.boot(str(qemu),kernels[label],'1G')+['-net','none',
                    '-object','rng-random,id=entropy,filename=/dev/urandom','-device',profile.rng(),
                    '-drive',f'file={disk},if=none,format=raw,id=root','-device',profile.block('modern')]
                if args.arch=='riscv':
                    command[command.index('-bios')+1]=str(args.firmware.resolve())
                    command+=['-global','virtio-mmio.force-legacy=false']
                log=base/f'{repeat}-{label}.log';harness.run_logged(command,log,90);raw=log.read_text(errors='replace')
                values=measurements(raw)
                if not profile.root_success(raw,42):raise RuntimeError('native workload resource release failed')
                harness.run_logged(['e2fsck','-fn',str(disk)],base/f'{repeat}-{label}-fsck.log')
                report['runs'].append({'repeat':repeat,'label':label,'ns':values,'command':command})
                disk.unlink();print(label,repeat,values,flush=True)
        report['median_ns']={label:{case:statistics.median(row['ns'][case] for row in report['runs'] if row['label']==label)
            for case in CASES} for label in kernels};report['status']='passed'
    except BaseException as error:
        report.update(status='failed',error=str(error));raise
    finally:(base/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print('medians:',report['median_ns'],flush=True)
if __name__=='__main__':main()
