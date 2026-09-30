#!/usr/bin/env python3
"""Serial, independent-boot cost measurements; unimplemented cases fail explicitly."""
import argparse
import fcntl
import hashlib
import json
import os
import sys
from cost_consumer import originals, framed_line, commands as consumer_commands
from pathlib import Path
import shutil
import subprocess
import tempfile
from cost_report import validate_deadline, parse, validate_expected
ROOT=Path(__file__).resolve().parents[1]
CASES=('contract','write','locking','mprotect','deadline','latency','consumer')
IMPLEMENTED={'contract','write','locking','mprotect','deadline','latency','consumer'}
def digest(path):
    with Path(path).open('rb') as stream: return hashlib.file_digest(stream,'sha256').hexdigest()
def run(command, **kwargs): return subprocess.run(command,check=True,**kwargs)
def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--case',choices=(*CASES,'all'),default='all')
    parser.add_argument('--kernel',type=Path,default=ROOT/'build/cost/kernel-rv')
    parser.add_argument('--qemu',default='qemu-system-riscv64')
    parser.add_argument('--linux',action='store_true',help='fixed Linux original consumer reference, diagnostics absent')
    parser.add_argument('--bios',type=Path,default=Path('/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin'))
    parser.add_argument('--two-disks',action='store_true')
    parser.add_argument('--off',action='store_true',help='run identical ELF with diagnostics disabled')
    parser.add_argument('--transport',choices=('legacy','modern'),default='modern')
    parser.add_argument('--cache',choices=('writeback','writethrough'),default='writeback')
    parser.add_argument('--replicas',type=int,choices=(1,3),default=3,help='1 is a pilot, never a complete baseline')
    parser.add_argument('--consumer-timeout-ms',type=int,default=180000,
                        help='explicit per-command guest budget, 1000..3600000 ms; completion checks remain mandatory')
    args=parser.parse_args()
    if not 1000<=args.consumer_timeout_ms<=3600000:parser.error('consumer timeout must be 1000..3600000 ms')
    linux_identity=None
    if args.linux:
        if args.case!='consumer': parser.error('Linux reference applies to original consumers')
        sys.path.insert(0,str(ROOT/'tests/diff-abi')); import harness
        args.kernel,linux_identity=harness.fixed_linux_image(None); args.off=True
    (ROOT/'build/cost').mkdir(parents=True,exist_ok=True)
    measurement_lock=(ROOT/'build/cost/measurement.lock').open('w')
    try: fcntl.flock(measurement_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except BlockingIOError: parser.error('another cost measurement is active; serial runs required')
    cases=CASES if args.case=='all' else (args.case,)
    missing=set(cases)-IMPLEMENTED
    if missing: parser.error('not implemented: '+','.join(sorted(missing)))
    work=Path(tempfile.mkdtemp(prefix='cost-run.',dir=ROOT/'build'))
    identity={'tree':subprocess.check_output(['git','write-tree'],cwd=ROOT,text=True).strip(),
        'base':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        'source_sha256':hashlib.sha256(b''.join(p.encode()+b'\0'+digest(ROOT/p).encode()+b'\n' for p in sorted(subprocess.check_output(['git','ls-files','-co','--exclude-standard'],cwd=ROOT,text=True).splitlines()) if '__pycache__' not in Path(p).parts)).hexdigest(),
        'diff_sha256':hashlib.sha256(subprocess.check_output(['git','diff','HEAD'],cwd=ROOT)).hexdigest(),
        'qemu_version':subprocess.check_output([args.qemu,'--version'],text=True).splitlines()[0],
        'qemu_sha256':digest(shutil.which(args.qemu)), 'cost_diagnostics':0 if args.off else 1,
        'replicas':args.replicas,'acceptance':args.replicas==3,'transport':args.transport,'cache':args.cache,'two_disks':args.two_disks,'platform':'linux' if args.linux else 'boaros','linux_reference':linux_identity}
    kernel=work/'kernel'; shutil.copyfile(args.kernel,kernel); identity['kernel_sha256']=digest(kernel)
    bios=work/'firmware'; shutil.copyfile(args.bios,bios); identity['firmware_sha256']=digest(bios)
    original=None
    if 'consumer' in cases:
        original,consumer_identity=originals(ROOT,work,digest); identity.update(consumer_identity)
        identity['consumer_timeout_ms']=args.consumer_timeout_ms
    compiler=ROOT/'build/riscv/musl-root/bin/musl-gcc'
    identity['compiler']=subprocess.check_output([str(compiler),'--version'],text=True).splitlines()[0]
    records=[]
    try:
        for case in cases:
            program=work/(case+'.elf')
            run([str(compiler),'-fno-link-libatomic','-static','-O2','-pthread','-Wall','-Wextra','-Werror',
                str(ROOT/'tests/workloads/cost'/f'{case}.c'),'-o',str(program)])
            for replica in range(args.replicas):
                folder=work/f'{case}-{replica}'; folder.mkdir()
                disk=folder/'root.img'
                if case=='consumer': run(['cp','--sparse=always','--reflink=auto',str(original),str(disk)])
                else:
                    with disk.open('wb') as stream: stream.truncate(256*1024*1024)
                    run(['mkfs.ext4','-q','-F','-b','4096',str(disk)])
                commands=folder/'fixture.commands'
                contents=f'write {program} /init\nset_inode_field /init mode 0100755\n'
                if case=='consumer':
                    nonce=folder/'replica-id';nonce.write_text(str(replica)+'\n');contents+=f'write {nonce} /cost-replica\n'
                    policy=folder/'consumer-budget';policy.write_text(str(args.consumer_timeout_ms)+'\n')
                    contents+=f'write {policy} /cost-consumer-budget\n'
                if args.linux:
                    flag=folder/'linux-flag'; flag.write_text('fixed Linux reference\n'); contents+=f'write {flag} /cost-linux\n'
                if case=='write':
                    seed=folder/'cold-data'; seed.write_bytes(bytes([0x5a])*1048576)
                    contents+=''.join(f'write {seed} /cold-{n}\n' for n in (0,1,3,63,64,65,4096))
                if case=='mprotect':
                    seed=folder/'resident-data'; seed.write_bytes(bytes([0x5a])*67108864)
                    contents+=f'write {seed} /resident-data\n'
                if case=='locking':
                    seed=folder/'lock-data'; seed.write_bytes(bytes([0x5a])*1048576)
                    contents+=''.join(f'write {seed} /lock-{name}\n' for name in ('a','b','input'))
                if args.two_disks:
                    flag=folder/'dual-flag'; flag.write_text('two disks\n'); contents+=f'write {flag} /cost-dual\n'
                commands.write_text(contents)
                run(['debugfs','-w','-f',str(commands),str(disk)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                record={**identity,'case':case,'replica':replica,'elf_sha256':digest(program),'fixture_sha256':digest(disk),'timebase_hz':10000000}
                invocation=[args.qemu,'-machine','virt','-bios',str(bios),'-kernel',str(kernel),
                    '-m','512M','-smp','1','-nographic','-no-reboot',
                    '-drive',f'file={disk},if=none,format=raw,id=root,cache={args.cache}',
                    '-global','virtio-mmio.force-legacy='+('true' if args.transport=='legacy' else 'false'),
                    '-device','virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
                if args.two_disks:
                    second=folder/'second.img'
                    with second.open('wb') as stream: stream.truncate(256*1024*1024)
                    run(['mkfs.ext4','-q','-F','-b','4096',str(second)])
                    second_commands=folder/'second.commands'; second_commands.write_text(f'write {seed} /lock-b\n')
                    run(['debugfs','-w','-f',str(second_commands),str(second)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                    record['second_fixture_sha256']=digest(second)
                    invocation+=['-drive',f'file={second},if=none,format=raw,id=second,cache={args.cache}',
                        '-device','virtio-blk-device,drive=second,bus=virtio-mmio-bus.1']
                if args.linux: invocation+=['-append','root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1','-object','rng-random,id=entropy,filename=/dev/urandom','-device','virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.7']
                dtb=folder/'boot.dtb'; probe=list(invocation); probe[probe.index('-machine')+1]='virt,dumpdtb='+str(dtb)
                run(probe,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                record['dtb_sha256']=digest(dtb); invocation+=['-dtb',str(dtb)]
                record['argv']=invocation
                frozen=json.dumps(record,sort_keys=True,separators=(',',':'))+'\n'
                (folder/'input.json').write_text(frozen)
                record['input_keys']=list(record);record['input_sha256']=hashlib.sha256(frozen.encode()).hexdigest()
                with (folder/'boot.log').open('w') as boot_log:
                    result=subprocess.Popen(invocation,stdin=subprocess.DEVNULL,stdout=boot_log,stderr=subprocess.STDOUT,text=True)
                    try: returncode=result.wait(timeout=16*args.consumer_timeout_ms/1000+120 if case=='consumer' else 180)
                    except subprocess.TimeoutExpired:
                        result.kill();result.wait();raise
                output=(folder/'boot.log').read_text()
                if returncode or output.count(f'COST PASS {case}\n')!=1 or 'cost contract failed' in output or 'cost workload failed' in output or (not args.linux and ('PID 1 exited status=0x0' not in output or 'heap-live=0x0' not in output)):
                    raise RuntimeError('guest failed: '+output[-4000:])
                snapshots=[]; current=None; body=[]; expectations={}; timings={}; metric_expectations={}
                for line in output.splitlines():
                    if case=='consumer':line=framed_line(line)
                    if line.startswith('COST METRIC '):
                        _,_,name,metric,value=line.split()
                        metric_expectations.setdefault(name,{})['foreground.'+metric+'.value']=int(value)
                    elif line.startswith('COST EXPECT '):
                        _,_,name,kind,calls,requested,accepted=line.split()
                        if name in expectations: raise ValueError('duplicate expectation')
                        expectations[name]={f'foreground.{kind}_calls.value':int(calls),
                            f'foreground.{kind}_requested.value':int(requested),
                            f'foreground.{kind}_accepted.value':int(accepted)}
                        if kind in ('file','dgram'): expectations[name][f'foreground.{kind}_copy.value']=int(accepted)
                        if kind=='file': expectations[name]['foreground.file_staging.value']=4096*int(calls) if int(requested) else 0
                    elif line.startswith('COST RESULT '):
                        _,_,name,ns=line.split()
                        if name in timings: raise ValueError('duplicate timing')
                        timings[name]=int(ns)
                    elif line.startswith('COST SNAPSHOT '):
                        if current is not None: raise ValueError('nested snapshot')
                        _,_,name,epoch=line.split(); current=(name,int(epoch)); body=[]
                    elif line=='COST END':
                        if current is None: raise ValueError('extra snapshot end')
                        name,epoch=current; snap=parse('\n'.join(body),epoch)
                        if name in expectations: validate_expected(snap,expectations[name])
                        if snapshots and epoch<=snapshots[-1]['values']['epoch']: raise ValueError('stale epoch')
                        snapshots.append({'name':name,'values':snap}); current=None
                    elif current is not None: body.append(line)
                if current is not None or (not snapshots and not args.off): raise ValueError('missing snapshot/end')
                if args.off and snapshots: raise ValueError('diagnostic nodes present in off build')
                if case=='write':
                    required={f'file-{cache}-{n}' for cache in ('cold','hot') for n in (0,1,3,63,64,65,4096)}
                    required.update(f'{kind}-1m-{alignment}' for kind in ('file','tcp') for alignment in ('aligned','misaligned'))
                    required.update(f'file-sync-{method}-every-{interval}' for method in range(3) for interval in (1,16,128))
                    required.update(('dgram-64k','dgram-oversize','file-osync','file-odsync','file-random','file-extend-accept','file-extend-sync','file-fault-prefix','file-fault-first'))
                    if set(timings)!=required or (not args.off and {s['name'] for s in snapshots}!=required): raise ValueError('missing/extra workload window')
                if case=='locking':
                    required={f'locking-{relationship}-0-{waiters}' for relationship in range(3) for waiters in (1,8,32)}
                    required.update(f'locking-1-{operation}-8' for operation in (1,2,3,4))
                    if set(timings)!=required or (not args.off and {s['name'] for s in snapshots}!=required): raise ValueError('locking window coverage')
                    if not args.off:
                        for snapshot in snapshots:
                            value=snapshot['values']
                            if value['foreground.lock15_acquired.value']==0: raise ValueError('write operation lock missing')
                            if snapshot['name'].startswith('locking-1-0-') and value['foreground.lock15_blocks.value']==0: raise ValueError('same inode contention was not exercised')
                if not args.off:
                    for snapshot in snapshots:
                        validate_expected(snapshot['values'],metric_expectations.get(snapshot['name'],{}))
                if case=='consumer':
                    record['commands']=consumer_commands(output,expected_budget_ms=args.consumer_timeout_ms)
                    required={c['name'] for c in record['commands']}
                    if set(timings)!=required or (not args.off and {s['name'] for s in snapshots}!=required): raise ValueError('consumer coverage')
                if case=='contract' and not args.off:
                    if {s['name'] for s in snapshots}!={'contract','reuse','inflight','after-abort'} or [s['values']['epoch'] for s in snapshots]!=[1,2,3,5]: raise ValueError('contract epoch/coverage')
                if case=='latency':
                    required={f'latency-copy-{n}' for n in (4096,65536,1048576)}|{f'latency-protect-{n}' for n in (4096,1048576)}
                    if set(timings)!=required or (not args.off and {s['name'] for s in snapshots}!=required): raise ValueError('latency coverage')
                    if not args.off and any(s['values']['foreground.irq_off_ticks.samples']==0 or s['values']['foreground.wake_to_run.samples']==0 for s in snapshots): raise ValueError('IRQ or independent wake not measured')
                if case=='deadline':
                    required={f'deadline-{n}-0-0' for n in (0,32,128,256)}|{f'deadline-32-{m}-0' for m in (1,2,3)}|{'deadline-0-0-1'}
                    if set(timings)!=required or (not args.off and {s['name'] for s in snapshots}!=required): raise ValueError('deadline coverage')
                    if not args.off:
                        for snapshot in snapshots:validate_deadline(snapshot['name'],snapshot['values'])
                if case=='mprotect':
                    required={f'mprotect-{n}-{r}' for n in (16,64,256) for r in (0,16,64)}|{'mprotect-failure'}
                    if set(timings)!=required or (not args.off and {s['name'] for s in snapshots}!=required): raise ValueError('mprotect coverage')
                record['snapshots']=snapshots; record['timings_ns']=timings; records.append(record)
                (folder/'result.json').write_text(json.dumps(record,indent=2)+'\n')
                # Fixture identity is kept; disposable writable copies are pruned at stage end.
                print(f'cost {case} replica {replica+1}: {len(timings)} workloads, {len(snapshots)} diagnostic windows'+(f', completed {sum(c["outcome"]=="completed" for c in record["commands"])}/16 original commands' if case=='consumer' else ''),flush=True)
        (work/'results.json').write_text(json.dumps(records,indent=2)+'\n')
        print('cost records: '+str(work/'results.json'))
    except BaseException:
        print('cost artifacts retained: '+str(work),flush=True); raise
if __name__=='__main__': main()
