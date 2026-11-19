"""Run unchanged final-2026 public rootfs scripts and their upstream judges."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from local_disks import copy_disk_image
from prepare import DEFAULT_LTP_CASE_TIMEOUT

ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'tests'))
from arch_profiles import PROFILES
from qemu_thread_affinity import host_cpu_selection, pin_qemu_threads
from sdcard import directory as sdcard_directory

GROUPS=('cagent','buildstorm')
TAGS={'riscv':'rv','loongarch':'la'}

def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()

def save(path,value):
    path.write_text(json.dumps(value,indent=2)+'\n')

def functional_success(reports,groups):
    for group in groups:
        cases=reports[group]['cases']
        if group=='cagent':
            if len(cases)!=10 or any(case['pass']!=1 for case in cases):return False
        else:
            required={'buildstorm env toolchain','buildstorm env minibuild','buildstorm compile ok'}
            if {case['name'] for case in cases if case['pass']==1}&required!=required:return False
    return True

def run_status(records):
    if any(not record['completed'] for record in records):return 'incomplete'
    return 'passed' if all(record['functional_success'] for record in records) else 'completed-with-failures'

def judge_reports(raw,groups,suite):
    reports={}
    for group in groups:
        judge=suite/'judge'/f'judge_{group}-glibc.py'
        result=subprocess.run([sys.executable,str(judge)],input=raw,text=True,capture_output=True,check=True)
        reports[group]={'cases':json.loads(result.stdout),'notes':result.stderr}
    return reports

def build(architectures,groups,directory):
    configs={}
    for arch in architectures:
        config=directory/TAGS[arch]/'init.json';configs[arch]=config
        subprocess.run([sys.executable,str(HERE/'prepare.py'),'--profile','final-2026',
                        '--arch',arch,'--groups',' '.join(groups),'--output',str(config)],cwd=ROOT,check=True)
    command=['make','-j8','SMP=1','USER_SMP=1','SMP_CPUS=8','LA_SMP=1','LA_SMP_CPUS=8','COST_DIAGNOSTICS=0']
    kernels={}
    if 'riscv' in architectures:
        command+=['INIT_CONFIG_RV='+str(configs['riscv']),'build/riscv-user-smp8/kernel-rv']
        kernels['riscv']=ROOT/'build/riscv-user-smp8/kernel-rv'
    if 'loongarch' in architectures:
        command+=['INIT_CONFIG_LA='+str(configs['loongarch']),'kernel-la']
        kernels['loongarch']=ROOT/'build/loongarch-smp8/kernel-la'
    environment=os.environ.copy()
    for name in ('INIT_CONFIG','INIT_CONFIG_RV','INIT_CONFIG_LA','MAKEFLAGS','MFLAGS','MAKEOVERRIDES'):
        environment.pop(name,None)
    with (directory/'build.log').open('w') as log:
        subprocess.run(command,cwd=ROOT,env=environment,stdout=log,stderr=subprocess.STDOUT,check=True)
    for arch,source in kernels.items():shutil.copyfile(source,directory/TAGS[arch]/'kernel')
    return command

def run_guest(args,arch,work,groups,budget,host_vcpus,suite):
    profile=PROFILES[arch]
    selected=args.qemu_riscv if arch=='riscv' else args.qemu_loongarch
    qemu=Path(shutil.which(selected) or selected).resolve(strict=True)
    command=profile.boot(str(qemu),work/'kernel','8G')
    command[command.index('-smp')+1]='8'
    command+=['-accel','tcg,thread=multi','-drive',f'file={work/"root.img"},if=none,format=raw,id=root',
              '-device',profile.block('modern'),'-object','rng-random,id=entropy,filename=/dev/urandom',
              '-device',profile.rng(),'-netdev','user,id=net0','-device',
              'virtio-net-device,netdev=net0,bus=virtio-mmio-bus.1' if arch=='riscv' else
              'virtio-net-pci,netdev=net0,addr=2,disable-legacy=on,romfile=']
    if arch=='riscv':command+=['-global','virtio-mmio.force-legacy=false']
    record={'command':command,'kernel_sha256':sha(work/'kernel'),'qemu_sha256':sha(qemu),
            'qemu_version':subprocess.check_output([str(qemu),'--version'],text=True).splitlines()[0]}
    start=time.monotonic()
    with tempfile.TemporaryDirectory(prefix='oscomp-final-') as temporary,(work/'serial.log').open('w') as output:
        qmp=Path(temporary)/'qmp'
        if host_vcpus:command+=['-S','-qmp',f'unix:{qmp},server=on,wait=off']
        process=subprocess.Popen(command,cwd=ROOT,stdin=subprocess.DEVNULL,stdout=output,stderr=subprocess.STDOUT,
            preexec_fn=(lambda:os.sched_setaffinity(0,{args.host_io_cpu})) if host_vcpus else None)
        try:
            if host_vcpus:record['host_thread_affinity']=pin_qemu_threads(process,qmp,host_vcpus,args.host_io_cpu)
            save(work/'identity.json',record)
            try:
                record['returncode']=process.wait(timeout=budget or None)
                record['exit_reason']='qemu-exit'
            except subprocess.TimeoutExpired:
                process.kill();record['returncode']=process.wait();record['exit_reason']='outer-timeout'
        finally:
            if process.poll() is None:process.kill();process.wait()
            record['wall_seconds']=time.monotonic()-start
            save(work/'identity.json',record)
    raw=(work/'serial.log').read_text(errors='replace')
    lines=raw.replace('\r','').splitlines()
    record['groups']={}
    for group in groups:
        prefix=f'BOAROS-EVAL EXIT {group}-glibc status='
        statuses=[int(line[len(prefix):]) for line in lines if line.startswith(prefix)]
        record['groups'][group]={'entered':f'BOAROS-EVAL ENTER {group}-glibc' in lines,
            'script_completed':f'#### OS COMP TEST GROUP END {group}-glibc ####' in lines,
            'shell_statuses':statuses}
    record['root_resources_verified']=profile.root_success(raw,0)
    record['completed']=record['root_resources_verified'] and 'BOAROS-EVAL COMPLETE' in lines and all(
        value['entered'] and value['script_completed'] and value['shell_statuses']==[0]
        for value in record['groups'].values())
    record['judge_results']=judge_reports(raw,groups,suite)
    record['functional_success']=record['completed'] and functional_success(record['judge_results'],groups)
    for group,judged in record['judge_results'].items():
        save(work/f'{group}-judge.json',judged['cases'])
        (work/f'{group}-judge.log').write_text(judged['notes'])
    if record['root_resources_verified']:
        with (work/'fsck.log').open('w') as log:
            status=subprocess.run(['e2fsck','-fn',str(work/'root.img')],stdout=log,stderr=subprocess.STDOUT).returncode
        checked=(work/'fsck.log').read_text(errors='replace')
        clean=status==0 and 'Fix?' not in checked and 'skipping journal recovery' not in checked
        record['filesystem_check']={'returncode':status,'clean':clean}
        record['functional_success'] &= clean
        if clean and not args.keep_disks:(work/'root.img').unlink()
    save(work/'result.json',record)
    return record

def main(args):
    groups=GROUPS if args.groups=='all' else (args.groups,)
    architectures=tuple(TAGS) if args.arch=='both' else (args.arch,)
    if args.case_timeout != DEFAULT_LTP_CASE_TIMEOUT or args.diagnostic_exclude:
        raise ValueError('final-2026 retains its original internal timeouts and does not use LTP exclusions')
    if args.host_vcpu_cpus is not None and args.host_io_cpu is None:
        raise ValueError('--host-io-cpu is required with --host-vcpu-cpus')
    if args.host_vcpu_cpus is None and args.host_io_cpu is not None:
        raise ValueError('--host-vcpu-cpus is required with --host-io-cpu')
    host_vcpus=host_cpu_selection(args.host_vcpu_cpus,8,args.host_io_cpu)
    budget=args.diagnostic_timeout if args.diagnostic_timeout is not None else 0
    if budget<0:raise ValueError('budget must be nonnegative')
    directory=(args.output or ROOT/'build'/('oscomp-final-2026-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S'))).resolve()
    if not directory.is_relative_to(ROOT/'build'):raise ValueError('outputs must be under build/')
    suite=ROOT/'references/oscomp-testsuits'
    for group in groups:
        judge=suite/'judge'/f'judge_{group}-glibc.py'
        if not judge.is_file():
            raise FileNotFoundError(f'Use the final-2026 checkout for {judge}')
    revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=suite,text=True).strip()
    if subprocess.check_output(['git','status','--porcelain','--','judge'],cwd=suite,text=True).strip():
        raise ValueError('commit local changes to the final-2026 judges before using them')
    images=(args.images_dir or sdcard_directory('final-2026')).resolve()
    disks={arch:images/f'sdcard-{TAGS[arch]}-pub.img' for arch in architectures}
    for disk in disks.values():
        if not disk.is_file():raise FileNotFoundError(f'Extract the release disk to {disk}')
    directory.mkdir(parents=True,exist_ok=False)
    report={'profile':'final-2026','kind':'local-diagnostic','testsuite_commit':revision,'selected_groups':groups,
            'guest_cpus':8,'guest_memory':'8G','timeout_seconds':budget,'architectures':{},'status':'preparing',
            'kernel_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
            'kernel_dirty':subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True),
            'runner_sha256':sha(Path(__file__)),'bootstrap_sha256':sha(HERE/'final-2026-init.sh')}
    try:
        shutil.copyfile(Path(__file__),directory/'runner.py')
        shutil.copyfile(HERE/'final-2026-init.sh',directory/'bootstrap.sh')
        for arch in architectures:(directory/TAGS[arch]).mkdir()
        report['build_command']=build(architectures,groups,directory)
        for arch in architectures:
            work=directory/TAGS[arch]
            print('Preparing original public disk:',arch,flush=True)
            copy_disk_image(disks[arch], work/'root.img', sparse=True)
            report['architectures'][arch]={'root_image':str(disks[arch])}
        save(directory/'identity.json',report)
        for arch in architectures:
            report['status']='running';save(directory/'report.json',report)
            print('Running final-2026:',arch,groups,flush=True)
            report['architectures'][arch].update(run_guest(args,arch,directory/TAGS[arch],groups,budget,host_vcpus,suite))
            save(directory/'report.json',report)
        passed=all(record['functional_success'] for record in report['architectures'].values())
        report['status']=run_status(report['architectures'].values())
        print(report['status'],directory,flush=True)
        return 0 if passed else 1
    except BaseException as error:
        report.update(status='runner-error',error=str(error))
        raise
    finally:save(directory/'report.json',report)


def cli(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=(*TAGS, 'both'), default='both')
    parser.add_argument('--groups', choices=('all', *GROUPS), default='all')
    parser.add_argument('--images-dir', type=Path, help='default: sdcard/final-2026/')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--diagnostic-timeout', type=int, help='outer timeout in seconds (default: none)')
    parser.add_argument('--host-vcpu-cpus', help='one distinct host CPU per guest CPU')
    parser.add_argument('--host-io-cpu', type=int, help='host CPU for QEMU main and I/O threads')
    parser.add_argument('--keep-disks', action='store_true', help='retain checked runtime images')
    parser.add_argument('--qemu-riscv', default=os.environ.get('QEMU_RISCV64', 'qemu-system-riscv64'))
    parser.add_argument('--qemu-loongarch', default=os.environ.get('QEMU_LOONGARCH64', 'build/qemu-la-rtc/qemu-system-loongarch64'))
    parser.set_defaults(case_timeout=DEFAULT_LTP_CASE_TIMEOUT, diagnostic_exclude=[])
    return main(parser.parse_args(argv))


if __name__ == '__main__':
    sys.exit(cli())
