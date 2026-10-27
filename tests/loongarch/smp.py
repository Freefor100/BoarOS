#!/usr/bin/env python3
"""Short native LA SMP consumer, using the existing ext4 boot fixture."""
import argparse,json,subprocess,sys,shutil,os,tempfile,signal,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'tests/diff-abi')]
import harness
from arch_profiles import PROFILES
from qemu_thread_affinity import pin_qemu_threads,host_cpu_selection,host_cpu_topology

def parse_performance(raw):
    metrics=[]
    for line in raw.splitlines():
        if line.startswith(('ALTSTACK-PERF mode=','FUTEX-PERF name=','LA-SMP-PERF name=')):
            fields=dict(token.split('=',1) for token in line.split()[1:] if '=' in token)
            metrics.append({key:(int(value) if value.isdecimal() else value) for key,value in fields.items()})
    return metrics

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel',type=Path,required=True);p.add_argument('--program',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--cpus',type=int,choices=(1,2),default=2)
    p.add_argument('--performance',action='store_true');p.add_argument('--host-vcpus');p.add_argument('--host-io',type=int,default=8);p.add_argument('--perf-stat',action='store_true')
    p.add_argument('--loader',type=Path);p.add_argument('--system',choices=('boaros','linux'),default='boaros');p.add_argument('--expect-failure');p.add_argument('--memory',default='512M');a=p.parse_args();out=a.output.resolve();out.mkdir()
    program=out/'program';kernel=out/'kernel'
    shutil.copyfile(a.program.resolve(),program);shutil.copyfile(a.kernel.resolve(),kernel)
    program_hash=harness.digest(program);kernel_hash=harness.digest(kernel)
    disk=harness.fixture(out,program);profile=PROFILES['loongarch']
    if a.loader:
        batch=out/'loader.debugfs';batch.write_text(f'mkdir /lib\nwrite "{a.loader.resolve()}" /lib/ld-musl-loongarch64.so.1\nset_inode_field /lib/ld-musl-loongarch64.so.1 mode 0100755\n')
        harness.run_logged(['debugfs','-w','-f',str(batch),str(disk)],out/'loader.log')
    if a.performance:
        config=out/'cost-config';config.write_text(str(a.cpus))
        stack=out/'stack-config';stack.write_text(f'{a.cpus} 1')
        batch=out/'cost.debugfs';batch.write_text(f'write "{config}" /la-smp-cost\nwrite "{stack}" /account-altstack\n')
        harness.run_logged(['debugfs','-w','-f',str(batch),str(disk)],out/'cost-install.log')
    command=profile.boot(profile.qemu,kernel,a.memory)
    command[command.index('-smp')+1]=str(a.cpus)
    command+=['-net','none','-drive',f'file={disk},if=none,format=raw,id=root','-device',profile.block('pci'),'-object','rng-random,id=entropy,filename=/dev/urandom','-device',profile.rng()]
    if a.system=='linux':
        from root import execute
        from reference import archive
        supervisor=out/'supervisor'
        execute([profile.compiler,*profile.raw_flags,'-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdlib','-nostartfiles','-static','-no-pie','-Wl,-z,max-page-size=16384','-T','tests/common/user.ld','tests/loongarch/root_linux_init.c','tests/common/user_start.S','-o',supervisor])
        initrd=out/'initramfs.gz';initrd.write_bytes(archive([('dev',0o040755,b'',0,0),('dev/console',0o020600,b'',5,1),('init',0o100755,supervisor.read_bytes(),0,0),('TRAILER!!!',0,b'',0,0)]))
        command+=['-initrd',str(initrd),'-append','console=ttyS0 rdinit=/init loglevel=3']
    affinity=None;perf=None
    selection=host_cpu_selection(a.host_vcpus,a.cpus,a.host_io)
    with tempfile.TemporaryDirectory(prefix='la-smp-qmp.') as temporary, (out/'boot.log').open('w') as log:
        if selection:
            qmp=Path(temporary)/'qmp';command+=['-S','-qmp',f'unix:{qmp},server=on,wait=off']
        process=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT,
            preexec_fn=(lambda:os.sched_setaffinity(0,{a.host_io})) if selection else None)
        try:
            if selection:affinity=pin_qemu_threads(process,qmp,selection,a.host_io)
            if a.perf_stat:
                if not selection:raise ValueError('--perf-stat needs --host-vcpus')
                tids=','.join(str(row['tid']) for row in affinity['vcpus'])
                with (out/'perf-stderr.log').open('w') as error:
                    perf=subprocess.Popen(['perf','stat','-x,','-I','100','-e','cpu_core/cycles/u,cpu_core/instructions/u,task-clock:u,context-switches:u,cpu-migrations:u','-t',tids,'-o',str(out/'perf.csv')],stdout=subprocess.DEVNULL,stderr=error,preexec_fn=lambda:os.sched_setaffinity(0,{14}))
            process.wait(timeout=60);code=process.returncode
        except subprocess.TimeoutExpired:
            process.kill();process.wait();code=-1
        finally:
            if process.poll() is None:process.kill();process.wait()
            if perf:
                if perf.poll() is None:perf.send_signal(signal.SIGINT)
                perf.wait(timeout=10)
    raw=(out/'boot.log').read_text();rows=[line for line in raw.splitlines() if line.startswith('LA-SMP ')]
    passed=code==0 and rows==[f'LA-SMP BEGIN cpus={a.cpus}',*[f'LA-SMP CASE {name} PASS' for name in ('remote-affinity','executors-tls-fpu-simd-signals-accounting','shared-mm-protect-replace-unmap','remote-futex-wait-wake','group-virtual-prof-timers','ext4-parallel-files-mmap-orphans','fork-cow-affinity-exec-exit')],'LA-SMP END 7'] and (profile.root_success(raw,0) if a.system=='boaros' else 'Linux LA root application passed' in raw)
    metrics=parse_performance(raw) if a.performance else []
    if a.performance:
        expected=('getpid','signal','roundtrip','mutex4','mutex1','compute','files')
        counts=(1000000,4096,4096,32768,32768,100000000,4096)
        passed=code==0 and tuple(row['name'] for row in metrics)==expected and tuple(row['count'] for row in metrics)==counts and all(row['ns']>0 and row['cpu_ns']>0 for row in metrics) and 'ALTSTACK-PERF END 2' in raw and 'FUTEX-PERF END 3' in raw and 'LA-SMP-PERF END 7' in raw and profile.root_success(raw,0)
        if passed:passed=all(row.get('completed',8)==8 and row.get('cpus',a.cpus)==a.cpus for row in metrics) and metrics[-1]['bytes']==33554432
    if a.expect_failure:
        passed=code==0 and f'LA SMP failure stage={a.expect_failure} target=0x0000000000000001' in raw and 'LA root owners released' not in raw
    if passed and not a.expect_failure:
        with (out/'fsck.log').open('w') as log:passed=subprocess.run(['e2fsck','-fn',disk],stdout=log,stderr=subprocess.STDOUT).returncode==0
        if passed:disk.unlink()
    (out/'result.json').write_text(json.dumps({'passed':passed,'metrics':metrics,'affinity':affinity,'host_topology':host_cpu_topology(selection+[a.host_io]) if selection else None,'command':command,'returncode':code,'rows':rows,'expected_failure':a.expect_failure,'kernel_sha256':kernel_hash,'elf_sha256':program_hash},indent=2)+'\n')
    print('\n'.join(rows));return 0 if passed else 1
if __name__=='__main__':raise SystemExit(main())
