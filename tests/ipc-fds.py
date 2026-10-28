#!/usr/bin/env python3
"""Native POSIX mqueue / pipe capacity consumers on the existing disk fixture."""
import argparse,json,shutil,subprocess,sys,os,re,tempfile,signal,threading,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'tests/diff-abi')]
import harness
from arch_profiles import PROFILES
from cost_fixture import FrozenFixture
from qemu_thread_affinity import host_cpu_selection,pin_qemu_threads,host_cpu_topology
MQ_CASES=('native64-libc-priority-fifo-segments-ofd-flags-timeout',
 'poll-epoll-stat-write-mmap-usercopy',
 'signal-real-sender-value64-childclose-owner-anyfd-none-single-registration',
 'libc-sigev-thread-callback-and-removed-cookie',
 'creation-realuid-quota-existing-open-unlinked-owner-lifetime',
 'shared-mount-namespace-state-read-readv-pread-mode-inode-proc-unlink')
PIPE_CASES=('shared-endpoints-alias uint32-rounding invalid-fd nonpipe',
 'wrapped-growth occupied-slot-shrink exact-content','fifo-resize content-close',
 'real-uid-soft-quota two-page-default permitted-shrink returned-charge','all')
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--arch',choices=PROFILES,default='riscv');p.add_argument('--case',choices=('mqueue','pipe-capacity','cost'),required=True)
 p.add_argument('--system',choices=('linux','boaros'),required=True);p.add_argument('--kernel',type=Path,required=True)
 p.add_argument('--program',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
 p.add_argument('--loader',type=Path);p.add_argument('--memory',default='512M');p.add_argument('--cpus',type=int,choices=(1,2,4),default=1)
 p.add_argument('--cost-mode',choices=('legacy','features','all'),default='all');p.add_argument('--smoke',action='store_true')
 p.add_argument('--fixture-cache',type=Path);p.add_argument('--host-vcpus');p.add_argument('--host-io',type=int,default=8);p.add_argument('--perf-stat',action='store_true')
 a=p.parse_args();profile=PROFILES[a.arch];out=a.output.resolve();out.mkdir();program=out/'program';kernel=out/'kernel'
 shutil.copyfile(a.program.resolve(),program);shutil.copyfile(a.kernel.resolve(),kernel)
 configuration={'arch':a.arch,'cpus':a.cpus,'memory':a.memory,'mode':a.cost_mode,'profile':'smoke' if a.smoke else 'formal'}
 cache=FrozenFixture(a.fixture_cache,'ipc-fds',harness.digest(program),configuration) if a.fixture_cache else None
 if cache and (a.case!='cost' or a.loader):raise ValueError('sealed cost fixture requires static cost ELF')
 template=cache.load() if cache else None
 if template:disk=out/'fixture.img';shutil.copyfile(template,disk)
 else:
  disk=harness.fixture(out,program)
  if a.case=='cost':
   config=out/'cost-config';config.write_text(f"{a.cost_mode} {a.cpus} {configuration['profile']}\n")
   batch=out/'cost.debugfs';batch.write_text(f'write "{config}" /mqueue-pipe-cost\n')
   harness.run_logged(['debugfs','-w','-f',str(batch),str(disk)],out/'cost-install.log')
  if cache:cache.publish(disk)
 fixture_hash=harness.digest(disk)
 if a.loader:
  headers=subprocess.run(['readelf','-lW',str(program)],check=True,capture_output=True,text=True,env={**os.environ,'LC_ALL':'C'}).stdout
  match=re.search(r'\[Requesting program interpreter: (/lib/ld-musl-[^]]+)\]',headers)
  if not match:raise ValueError('loader requires a matching dynamic musl ELF')
  loader=out/'loader';shutil.copyfile(a.loader.resolve(),loader)
  batch=out/'loader.debugfs';batch.write_text(f'mkdir /lib\nwrite "{loader}" {match[1]}\nset_inode_field {match[1]} mode 0100755\n')
  harness.run_logged(['debugfs','-w','-f',str(batch),str(disk)],out/'loader.log')
 argv=profile.boot(profile.qemu,kernel,a.memory);argv[argv.index('-smp')+1]=str(a.cpus)
 argv+=['-net','none','-drive',f'file={disk},if=none,format=raw,cache=writeback,id=root','-device',profile.block('modern'),'-object','rng-random,id=entropy,filename=/dev/urandom','-device',profile.rng()]
 if a.arch=='riscv':argv+=['-global','virtio-mmio.force-legacy=false']
 if a.system=='linux':argv+=['-append','root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
 affinity=None;perf=None;host_samples=[];stop=threading.Event();sampler=None
 selection=host_cpu_selection(a.host_vcpus,a.cpus,a.host_io)
 with tempfile.TemporaryDirectory(prefix='ipc-fds-qmp.') as temporary,(out/'boot.log').open('w') as log:
  if selection:
   qmp=Path(temporary)/'qmp';argv+=['-S','-qmp',f'unix:{qmp},server=on,wait=off']
   if a.arch=='riscv' and a.system=='boaros':
    gdb=Path(temporary)/'gdb';argv+=['-chardev',f'socket,id=gdb,path={gdb},server=on,wait=off','-gdb','chardev:gdb']
  process=subprocess.Popen(argv,stdout=log,stderr=subprocess.STDOUT,preexec_fn=(lambda:os.sched_setaffinity(0,{a.host_io})) if selection else None)
  try:
   if selection:affinity=pin_qemu_threads(process,qmp,selection,a.host_io,**({'riscv_kernel':kernel,'gdb_socket':gdb} if a.arch=='riscv' and a.system=='boaros' else {}))
   if selection:
    def sample_host():
     os.sched_setaffinity(0,{15})
     while not stop.is_set():
      tasks={}
      for row in affinity['vcpus']:
       tid=row['tid']
       try:
        status=Path(f'/proc/{tid}/status').read_text()
        tasks[str(tid)]={'affinity':sorted(os.sched_getaffinity(tid)),'schedstat':list(map(int,Path(f'/proc/{tid}/schedstat').read_text().split())),'context_switches':{key:int(value) for key,value in re.findall(r'^(voluntary_ctxt_switches|nonvoluntary_ctxt_switches):\s+(\d+)',status,re.M)}}
       except (OSError,ProcessLookupError):pass
      if tasks:host_samples.append({'time_ns':time.time_ns(),'tasks':tasks})
      stop.wait(.1)
    sampler=threading.Thread(target=sample_host);sampler.start()
   if a.perf_stat:
    if not selection:raise ValueError('perf requires explicit host CPUs')
    tids=','.join(str(row['tid']) for row in affinity['vcpus'])
    with (out/'perf-stderr.log').open('w') as error:
     perf=subprocess.Popen(['perf','stat','-x,','-I','100','-e','cpu_core/cycles/u,cpu_core/instructions/u,task-clock,context-switches,cpu-migrations','-t',tids,'-o',str(out/'perf.csv')],stdout=subprocess.DEVNULL,stderr=error,preexec_fn=lambda:os.sched_setaffinity(0,{14}))
   process.wait(timeout=100);code=process.returncode
  except subprocess.TimeoutExpired:process.kill();process.wait();code=-1
  finally:
   if process.poll() is None:process.kill();process.wait()
   stop.set()
   if sampler:sampler.join()
   if perf:
    if perf.poll() is None:perf.send_signal(signal.SIGINT)
    perf.wait(timeout=10)
 raw=(out/'boot.log').read_text(errors='replace').replace('\r','');prefix='POSIX-MQ ' if a.case=='mqueue' else 'PIPE-CAPACITY '
 rows=[l for l in raw.splitlines() if l.startswith(prefix)]
 expected=(['POSIX-MQ BEGIN',*[f'POSIX-MQ CASE {name} PASS' for name in MQ_CASES],'POSIX-MQ END 6'] if a.case=='mqueue' else ['PIPE-CAPACITY PASS '+name for name in PIPE_CASES])
 metrics=[]
 if a.case=='cost':
  rows=[line for line in raw.splitlines() if line.startswith('MQPIPE ')]
  for line in rows:
   if line.startswith('MQPIPE RESULT '):
    fields=dict(x.split('=',1) for x in line.split()[2:]);metrics.append({k:int(v) if k!='name' else v for k,v in fields.items()})
  legacy=['legacy-getpid','legacy-signal','legacy-futex','legacy-pipe-default','legacy-unix']
  features=['mq-block-64','mq-block-4096','mq-epoll-64','mq-epoll-4096','pipe-default','pipe-onepage','pipe-1m','pipe-resize']
  names=legacy if a.cost_mode=='legacy' else features if a.cost_mode=='features' else legacy+features
  checks=(12964 if a.smoke else 1048432) if a.cost_mode=='all' else (1012800 if a.cost_mode=='legacy' else 35632)
  if a.smoke and a.cost_mode!='all':raise ValueError('smoke protocol is only checked as all')
  expected_end=f'MQPIPE END windows={len(names)} checks={checks}'
  expected=names
  passed=code==0 and len(rows)==len(names)+2 and rows[0].startswith(f"MQPIPE BEGIN version=1 mode={a.cost_mode} profile={configuration['profile']} cpus={a.cpus} ") and rows[-1]==expected_end and [m['name'] for m in metrics]==names and all(m['wall_ns']>0 and m['cpu_ns']>0 for m in metrics) and sum(m['checks'] for m in metrics)==checks
 else:passed=code==0 and rows==expected
 if a.system=='boaros':passed=passed and profile.root_success(raw,0)
 fsck=None
 if passed:
  with (out/'fsck.log').open('w') as log:fsck=subprocess.run(['e2fsck','-fn',str(disk)],stdout=log,stderr=subprocess.STDOUT).returncode
  passed=fsck==0
 record={'passed':passed,'arch':a.arch,'system':a.system,'case':a.case,'cpus':a.cpus,'memory':a.memory,'kernel_sha256':harness.digest(kernel),'elf_sha256':harness.digest(program),'loader_sha256':harness.digest(out/'loader') if a.loader else None,'command':argv,'returncode':code,'rows':rows,'expected':expected,'fsck':fsck,'metrics':metrics,'fixture_sha256':fixture_hash,'fixture_configuration':configuration if a.case=='cost' else None,'affinity':affinity,'host_samples':host_samples,'host_topology':host_cpu_topology(selection+[a.host_io]) if selection else None}
 (out/'result.json').write_text(json.dumps(record,indent=2)+'\n');print('\n'.join(rows),flush=True)
 if passed:disk.unlink()
 return 0 if passed else 1
if __name__=='__main__':raise SystemExit(main())
