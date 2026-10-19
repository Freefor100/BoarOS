#!/usr/bin/env python3
"""Original workloads in user SMP; controlled owner evidence is separate."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).parent))
from user import ROOT,boot_user,harness,identity,validate_execution,create_run_directory

CASES=('concurrent-first-fault','concurrent-file-first-fault',
    'ordinary-hot-read-pread-readv','ordinary-cold-cache','ordinary-cold-batch-local','ordinary-user-fault',
    'ordinary-read-fault-prefix','ordinary-read-seek','ordinary-read-close-signal',
    'ordinary-read-fork-pin','ordinary-write-pwrite-writev-local','mm-reclaim')+tuple(f'{name}-{count}' for count in (1,8,32)
    for name in ('fd-pipe','futex','group-exit'))

def validate_boot_output(text):
    rows=list(re.finditer(r'(?m)^BoarOS: timer frequency=0x[0-9a-f]+ '
        r'tick-hz=0x[0-9a-f]+ period=0x[0-9a-f]+\r?$',text))
    first_user=text.find('COMPOSITION CASE ')
    if len(rows)!=1 or first_user<0 or rows[0].start()>first_user:
        raise RuntimeError('startup output was incomplete or overlapped user output')

CLOCK_CASES=('cpu-clock-reap-held-target','cpu-clock-nonleader-exec-held-target')

WAIT_CASES=('waitid-pgid-entry','wait4-pgid-entry','waitid-interrupted-output','waitid-claim-handoff')

def validate_functional(text,sqlite=False,wait=False,clock=False):
    expected=CLOCK_CASES if clock else WAIT_CASES if wait else ('sqlite-pressure',) if sqlite else CASES
    rows=[line[line.index('COMPOSITION CASE '):] for line in text.splitlines() if 'COMPOSITION CASE ' in line]
    ends=re.findall(r'COMPOSITION END (\d+)',text)
    if rows!=[f'COMPOSITION CASE {name} OK' for name in expected] or ends!=[str(len(expected))] or 'COMPOSITION FAIL' in text:
        raise RuntimeError('incomplete or failed composition stream')
    return list(expected)

def validate_clock(text):
    rows=re.findall(r'COMPOSITION clock claims=(0x[0-9a-f]+) active=(0x[0-9a-f]+)',text)
    if len(rows)!=1 or tuple(int(value,16) for value in rows[0])!=(2,0):
        raise RuntimeError('two real CPU-clock target borrows or their release missing')
    owners=re.findall(r'COMPOSITION owners held=(0x[0-9a-f]+) released=(0x[0-9a-f]+) reclaimed=(0x[0-9a-f]+) '
        r'oom=(0x[0-9a-f]+) retirement=(0x[0-9a-f]+) remote-retirement=(0x[0-9a-f]+) '
        r'waits=(0x[0-9a-f]+) ledger=(0x[0-9a-f]+) control=(0x[0-9a-f]+)',text)
    if len(owners)!=1 or any(int(value,16) for value in owners[0]):
        raise RuntimeError('CPU-clock lifecycle owners remain at shutdown')

def validate_wait(text):
    rows=re.findall(r'COMPOSITION wait claims=(0x[0-9a-f]+) active=(0x[0-9a-f]+)',text)
    if len(rows)!=1 or tuple(int(value,16) for value in rows[0])!=(1,0):
        raise RuntimeError('real zombie claim was not held/released exactly once')
    owners=re.findall(r'COMPOSITION owners held=(0x[0-9a-f]+) released=(0x[0-9a-f]+) reclaimed=(0x[0-9a-f]+) '
        r'oom=(0x[0-9a-f]+) retirement=(0x[0-9a-f]+) remote-retirement=(0x[0-9a-f]+) '
        r'waits=(0x[0-9a-f]+) ledger=(0x[0-9a-f]+) control=(0x[0-9a-f]+)',text)
    if len(owners)!=1 or tuple(int(value,16) for value in owners[0])!=(0,0,0,0,0,0,4,0,0):
        raise RuntimeError('four child wait registrations or final owner release missing')

def validate_shutdown_output(text):
    tail='COMPOSITION shutdown-tail '+'x'*358
    rows=list(re.finditer(r'(?m)^'+re.escape(tail)+r'\r?$',text))
    final=text.find(' root-heap-peak-pages=')
    if len(rows)!=1 or final<rows[0].end():
        raise RuntimeError('queued TTY tail overlapped shutdown diagnostics')

def validate_fault_race(text):
    rows=re.findall(r'COMPOSITION fault-race arrivals=(0x[0-9a-f]+) stale=(0x[0-9a-f]+) resolved=(0x[0-9a-f]+)',text)
    if len(rows)!=1 or tuple(int(value,16) for value in rows[0])!=(2,1,2):
        raise RuntimeError('concurrent first-fault interleaving or stale-permission retry missing')
    syncs=re.findall(r'COMPOSITION fault-race-syncs=(0x[0-9a-f]+)',text)
    if len(syncs)!=1 or int(syncs[0],16)!=1:
        raise RuntimeError('anonymous spurious-fault retry repeated the page synchronization')

def validate_file_fault_race(text):
    rows=re.findall(r'COMPOSITION file-fault-race arrivals=(0x[0-9a-f]+) retries=(0x[0-9a-f]+) '
        r'resumed=(0x[0-9a-f]+) resolved=(0x[0-9a-f]+)',text)
    if len(rows)!=1 or tuple(int(value,16) for value in rows[0])!=(2,1,1,2):
        raise RuntimeError('file-fault retry path did not observe two faults, one resumed retry, and two completions')
    syncs=re.findall(r'COMPOSITION file-fault-race-syncs=(0x[0-9a-f]+)',text)
    if len(syncs)!=1 or int(syncs[0],16)!=1:
        raise RuntimeError('resumed remote file fault repeated the publisher synchronization')

def validate_ordinary_io(text,cpus=2):
    rows=re.findall(r'COMPOSITION ordinary-io phases=(0x[0-9a-f]+) local=(0x[0-9a-f]+) '
        r'copy0=(0x[0-9a-f]+) copy1=(0x[0-9a-f]+) '
        r'batch=(0x[0-9a-f]+) local-faults=(0x[0-9a-f]+) writes=(0x[0-9a-f]+) '
        r'write-pin0=(0x[0-9a-f]+) write-pin1=(0x[0-9a-f]+) close=(0x[0-9a-f]+) '
        r'seek=(0x[0-9a-f]+) fork=(0x[0-9a-f]+)',text)
    if len(rows)!=1:raise RuntimeError('missing or duplicate ordinary I/O observations')
    phases,local,copy0,copy1,batches,faults,writes,pin0,pin1,close,seek,fork=map(lambda value:int(value,16),rows[0])
    if (phases,local,batches,writes,close,seek,fork)!=(10,10,1,3,1,1,1) or not faults:
        raise RuntimeError('ordinary I/O, demand batch, local fault or owner/wait handshake missing')
    expected=(0,65666,0,4) if cpus>1 else (65666,0,4,0)
    if (copy0,copy1,pin0,pin1)!=expected:
        raise RuntimeError('copy bytes or source pins did not stay on the issuing CPU')

def validate_controlled(text,sqlite=False,cpus=2):
    rows=re.findall(r'COMPOSITION owners held=(0x[0-9a-f]+) released=(0x[0-9a-f]+) reclaimed=(0x[0-9a-f]+) '
        r'oom=(0x[0-9a-f]+) retirement=(0x[0-9a-f]+) remote-retirement=(0x[0-9a-f]+) '
        r'waits=(0x[0-9a-f]+) ledger=(0x[0-9a-f]+) control=(0x[0-9a-f]+)',text)
    if len(rows)!=1:raise RuntimeError('missing or duplicate controlled owner evidence')
    held,released,reclaimed,oom,retirement,remote_retirement,waits,ledger,control=map(lambda x:int(x,16),rows[0])
    if not held or held!=released or not reclaimed or ledger or control or (
            not sqlite and (oom!=1 or retirement!=1 or remote_retirement!=int(cpus>1) or waits!=123)) or (
            sqlite and remote_retirement):
        raise RuntimeError('pressure, retirement, registration or resource closure failed')
    return dict(zip(('held','released','reclaimed','oom','retirement','remote_retirement','waits','ledger','control'),
        (held,released,reclaimed,oom,retirement,remote_retirement,waits,ledger,control)))

def install(disk,path,source,folder):
    escaped=str(source).replace('\\','\\\\').replace('"','\\"')
    command=folder/(path.replace('/','_')+'.debugfs')
    command.write_text(f'write "{escaped}" {path}\nset_inode_field {path} mode 0100755\n')
    harness.run_logged(['debugfs','-w','-f',str(command),str(disk)],command.with_suffix('.log'))

def executable_identity(name):
    path=shutil.which(name)
    if not path:return None
    executable=Path(path).resolve(strict=True)
    result=identity(executable)
    result['version']=subprocess.check_output([str(executable),'--version'],text=True).splitlines()[0]
    return result

def resolve_build_root(value):
    return Path(value).resolve() if value else ROOT/'build/riscv-user-smp-composition'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu',default='qemu-system-riscv64');parser.add_argument('--firmware',required=True)
    parser.add_argument('--program',type=Path,default=ROOT/'build/riscv-user-smp-composition/composition-rv')
    parser.add_argument('--memory',choices=('512M','1G'),action='append');parser.add_argument('--cpus',type=int,choices=(1,2),action='append')
    parser.add_argument('--scene',choices=('mm','wait','cpu-clock','sqlite-delete','sqlite-wal'),action='append')
    parser.add_argument('--mode',choices=('common','controlled','both'),default='both')
    parser.add_argument('--build-root',type=Path,help='isolated common/controlled kernel build cache')
    parser.add_argument('--output',type=Path)
    args=parser.parse_args();output=create_run_directory(args.output,ROOT/'build/riscv-user-smp-composition/runs')
    build_root=resolve_build_root(args.build_root)
    config=output/'init.json';config.write_text(json.dumps({'path':'/composition-supervisor','argv':['/composition-supervisor'],'envp':[]})+'\n')
    compiler='riscv64-unknown-elf-gcc' if shutil.which('riscv64-unknown-elf-gcc') else 'riscv64-elf-gcc'
    report={'status':'preparing','runs':[],'program':identity(args.program),
        'build_root':str(build_root),'executor':{
            'qemu':executable_identity(args.qemu),'make':executable_identity('make'),
            'kernel_compiler':executable_identity(compiler)}}
    try:
        kernels={}
        for mode in ('common','controlled'):
            build=build_root/mode
            harness.run_logged(['make','-j4','SMP=1','USER_SMP=1',f'USER_SMP_COMPOSITION={int(mode=="controlled")}',
                f'USER_SMP_BUILD={build}',f'INIT_CONFIG={config}','all'],output/f'build-{mode}.log')
            kernels[mode]=output/(mode+'-kernel');shutil.copyfile(build/'kernel-rv',kernels[mode])
        linux,linux_identity=harness.linux_build(Path(__file__).with_name('linux-user.config'))
        kernels['linux']=output/'linux-kernel';shutil.copyfile(linux,kernels['linux'])
        program=output/'composition-elf';shutil.copyfile(args.program,program)
        report.update(linux=linux_identity,kernels={name:identity(path) for name,path in kernels.items()},status='running')
        for scene in args.scene or ('mm','sqlite-delete','sqlite-wal'):
            for memory in args.memory or ('512M','1G'):
                for cpus in args.cpus or (2,1):
                    group=output/f'{scene}-{memory}-{cpus}cpu';group.mkdir()
                    is_sqlite=scene.startswith('sqlite-')
                    workload=ROOT/'build/riscv/tests/user'/('sqlite-rollback-rv' if scene=='sqlite-delete' else 'sqlite-wal-rv') if is_sqlite else program
                    snapshot=group/'original-elf';shutil.copyfile(workload,snapshot)
                    fixture=harness.fixture(group,snapshot);install(fixture,'/composition-supervisor',program,group)
                    marker=group/'marker';marker.write_text('controlled composition scene\n')
                    if is_sqlite:install(fixture,'/sqlite-scene',marker,group)
                    if scene=='wait':install(fixture,'/wait-scene',marker,group)
                    if scene=='cpu-clock':install(fixture,'/cpu-clock-scene',marker,group)
                    support={}
                    if scene=='sqlite-delete':
                        for name in ('sqlite-cli-init','sqlite3-static','sqlite3-dynamic'):
                            source=ROOT/'build/riscv/tests/user'/(name+'-rv')
                            support[name]=group/name;shutil.copyfile(source,support[name])
                            install(fixture,'/'+name,support[name],group)
                        support['loader']=group/'musl-loader';shutil.copyfile(ROOT/'build/riscv/musl-root/lib/ld-musl-riscv64.so.1',support['loader'])
                        harness.run_logged(['debugfs','-w','-R','mkdir /lib',str(fixture)],group/'mkdir-lib.log')
                        install(fixture,'/lib/ld-musl-riscv64.so.1',support['loader'],group)
                        harness.run_logged(['debugfs','-w','-R','symlink /lib/libc.so /lib/ld-musl-riscv64.so.1',str(fixture)],group/'libc-link.log')
                    targets=(('linux','common'),('boaros','common'),('boaros-controlled','controlled'))
                    for target,mode in targets:
                        if args.mode=='common' and mode=='controlled' or args.mode=='controlled' and mode=='common':continue
                        folder=group/target;folder.mkdir();disk=folder/'guest.img';shutil.copyfile(fixture,disk)
                        if mode=='controlled':install(disk,'/controlled',marker,folder)
                        if target!='linux':install(disk,'/observer',marker,folder)
                        # BoarOS尚无每线程proc状态入口，使用只读登记观察者。
                        # 功能比较不触发压力/OOM/失效暂停；专属控制证据另行验证。
                        kernel=kernels['linux' if target=='linux' else 'controlled']
                        for phase in ('initial','reboot') if is_sqlite else ('initial',):
                            if scene=='sqlite-delete' and phase=='reboot':
                                harness.run_logged(['debugfs','-w','-R','rm /init',str(disk)],folder/'replace-init.log')
                                install(disk,'/init',support['sqlite-cli-init'],folder)
                            phase_dir=folder/phase;phase_dir.mkdir()
                            text,run,log=boot_user(args.qemu,args.firmware,kernel,disk,memory,cpus,phase_dir,target=='linux','/composition-supervisor')
                            validate_execution(text,cpus,target!='linux',require_parallel=True)
                            validate_functional(text,is_sqlite,scene=='wait',scene=='cpu-clock')
                            if target!='linux':
                                validate_boot_output(text)
                                validate_shutdown_output(text)
                            if target!='linux' and scene=='mm':
                                validate_fault_race(text)
                                validate_file_fault_race(text)
                                validate_ordinary_io(text,cpus)
                            if target!='linux' and scene=='wait':validate_wait(text)
                            if target!='linux' and scene=='cpu-clock':validate_clock(text)
                            owners=validate_controlled(text,is_sqlite,cpus) if mode=='controlled' and scene not in ('wait','cpu-clock') else None
                            if is_sqlite:
                                wanted=('BoarOS: SQLite WAL multiprocess passed' if phase=='initial' else 'BoarOS: SQLite WAL reboot verified') if scene=='sqlite-wal' else (
                                    'BoarOS: SQLite rollback smoke passed' if phase=='initial' else 'BoarOS: SQLite CLI static and dynamic passed')
                                if wanted not in text:raise RuntimeError('original SQLite content/exit contract missing')
                            report['runs'].append(dict(scene=scene,target=target,phase=phase,memory=memory,cpus=cpus,
                                workload=identity(snapshot if scene!='sqlite-delete' or phase=='initial' else support['sqlite-cli-init']),
                                support={name:identity(path) for name,path in support.items()},owners=owners,**run))
                            (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
                            print(f'composition {scene}/{target}/{phase} {memory}/{cpus}: content, progress and exit verified',flush=True)
                        harness.run_logged(['e2fsck','-fn',str(disk)],folder/'fsck.log')
        report['status']='passed'
    except BaseException as error:report.update(status='failed',error=str(error));raise
    finally:(output/'report.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
