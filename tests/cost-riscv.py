#!/usr/bin/env python3
"""Serial, independent-boot cost measurements; unimplemented cases fail explicitly."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from cost_report import parse, validate_expected
ROOT=Path(__file__).resolve().parents[1]
CASES=('contract','write','locking','mprotect','deadline','latency','consumer')
IMPLEMENTED={'contract','write'}
def digest(path):
    with Path(path).open('rb') as stream: return hashlib.file_digest(stream,'sha256').hexdigest()
def run(command, **kwargs): return subprocess.run(command,check=True,**kwargs)
def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--case',choices=(*CASES,'all'),default='all')
    parser.add_argument('--kernel',type=Path,default=ROOT/'build/cost/kernel-rv')
    parser.add_argument('--qemu',default='qemu-system-riscv64')
    parser.add_argument('--off',action='store_true',help='run identical ELF with diagnostics disabled')
    parser.add_argument('--transport',choices=('legacy','modern'),default='modern')
    parser.add_argument('--cache',choices=('writeback','writethrough'),default='writeback')
    parser.add_argument('--replicas',type=int,choices=(1,3),default=3,help='1 is a pilot, never a complete baseline')
    args=parser.parse_args()
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
        'source_sha256':hashlib.sha256(b''.join(p.encode()+b'\0'+digest(ROOT/p).encode()+b'\n' for p in sorted(subprocess.check_output(['git','ls-files','-co','--exclude-standard'],cwd=ROOT,text=True).splitlines()))).hexdigest(),
        'diff_sha256':hashlib.sha256(subprocess.check_output(['git','diff','HEAD'],cwd=ROOT)).hexdigest(),
        'qemu_version':subprocess.check_output([args.qemu,'--version'],text=True).splitlines()[0],
        'qemu_sha256':digest(shutil.which(args.qemu)), 'cost_diagnostics':0 if args.off else 1,
        'replicas':args.replicas,'acceptance':args.replicas==3,'transport':args.transport,'cache':args.cache}
    kernel=work/'kernel'; shutil.copyfile(args.kernel,kernel); identity['kernel_sha256']=digest(kernel)
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
                with disk.open('wb') as stream: stream.truncate(256*1024*1024)
                run(['mkfs.ext4','-q','-F','-b','4096',str(disk)])
                commands=folder/'fixture.commands'
                contents=f'write {program} /init\nset_inode_field /init mode 0100755\n'
                if case=='write':
                    seed=folder/'cold-data'; seed.write_bytes(bytes([0x5a])*1048576)
                    contents+=''.join(f'write {seed} /cold-{n}\n' for n in (0,1,3,63,64,65,4096))
                commands.write_text(contents)
                run(['debugfs','-w','-f',str(commands),str(disk)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                record={**identity,'case':case,'replica':replica,'elf_sha256':digest(program),'fixture_sha256':digest(disk)}
                invocation=[args.qemu,'-machine','virt','-bios','default','-kernel',str(kernel),
                    '-m','512M','-smp','1','-nographic','-no-reboot',
                    '-drive',f'file={disk},if=none,format=raw,id=root,cache={args.cache}',
                    '-global','virtio-mmio.force-legacy='+('true' if args.transport=='legacy' else 'false'),
                    '-device','virtio-blk-device,drive=root,bus=virtio-mmio-bus.0']
                record['argv']=invocation
                result=subprocess.run(invocation,stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=180)
                output=result.stdout+result.stderr; (folder/'boot.log').write_text(output)
                if result.returncode or result.stdout.count(f'COST PASS {case}\n')!=1 or 'cost contract failed' in output or 'cost workload failed' in output or 'PID 1 exited status=0x0' not in output or 'heap-live=0x0' not in output:
                    raise RuntimeError('guest failed: '+output[-4000:])
                snapshots=[]; current=None; body=[]; expectations={}; timings={}
                for line in result.stdout.splitlines():
                    if line.startswith('COST EXPECT '):
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
                record['snapshots']=snapshots; record['timings_ns']=timings; records.append(record)
                (folder/'result.json').write_text(json.dumps(record,indent=2)+'\n')
                # Fixture identity is kept; disposable writable copies are pruned at stage end.
                print(f'cost {case} replica {replica+1}: {len(snapshots)} valid windows',flush=True)
        (work/'results.json').write_text(json.dumps(records,indent=2)+'\n')
        print('cost records: '+str(work/'results.json'))
    except BaseException:
        print('cost artifacts retained: '+str(work),flush=True); raise
if __name__=='__main__': main()
