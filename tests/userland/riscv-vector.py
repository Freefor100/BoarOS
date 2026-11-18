#!/usr/bin/env python3
"""RVV user context contract; reuse a built kernel and baseline-RV64GC static ELF."""
import argparse,os,sys,tempfile,json,hashlib,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'tests/diff-abi')]
from arch_profiles import PROFILES
import harness
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--kernel',type=Path,required=True)
p.add_argument('--program',type=Path,required=True)
p.add_argument('--qemu',default=os.environ.get('QEMU_RISCV64','qemu-system-riscv64'))
p.add_argument('--clone-oom',action='store_true')
p.add_argument('--cpus',type=int,default=1)
p.add_argument('--vlen',type=int,default=128,help='0 tests a CPU without V')
a=p.parse_args();profile=PROFILES['riscv']
work=Path(tempfile.mkdtemp(prefix='vector-',dir=ROOT/'build'))
disk=harness.fixture(work,a.program.resolve())
command=profile.boot(a.qemu,a.kernel.resolve(),'1G');command[command.index('-smp')+1]=str(a.cpus)
command+=['-cpu',f'rv64,v=true,vlen={a.vlen},elen=64' if a.vlen else 'rv64,v=false',
    '-net','none','-drive',f'file={disk},if=none,format=raw,id=root','-device',profile.block('modern'),
    '-global','virtio-mmio.force-legacy=false']
identity={str(f.resolve()):hashlib.sha256(f.read_bytes()).hexdigest() for f in (a.kernel,a.program)}
source={key:subprocess.check_output(['git',*args],cwd=ROOT,text=True).strip() for key,args in [('commit',['rev-parse','HEAD']),('status',['status','--porcelain'])]}
(work/'identity.json').write_text(json.dumps({'command':command,'files':identity,'source':source},indent=2)+'\n')
log=work/'guest.log';harness.run_logged(command,log,timeout=120);text=log.read_text()
expected='VECTOR ALL PASS' if a.vlen else 'VECTOR NOV PASS'
if expected not in text or 'VECTOR FAIL' in text or not ('PID 1 exited status=0x0' in text and 'heap-live=0x0' in text and 'task stacks released=' in text):
    raise RuntimeError(f'incomplete vector contract or root cleanup: {log}')
assert identity=={str(f.resolve()):hashlib.sha256(f.read_bytes()).hexdigest() for f in (a.kernel,a.program)},'inputs changed during run'
if a.clone_oom:
    assert 'VECTOR CLONE OOM PASS' in text and 'VECTOR clone allocation failure injected' in text and 'VECTOR first-use allocation failure injected' in text, 'missing actual clone-allocation failure boundary'
disk.unlink();print(f'PASS RVV VLEN={a.vlen} cpus={a.cpus}: {log}')
