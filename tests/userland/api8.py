#!/usr/bin/env python3
"""Run the API8 callers in one guest per kernel, using identical ELF files."""
import sys
from pathlib import Path
sys.path[:0] = [str(Path(__file__).resolve().parent), str(Path(__file__).resolve().parents[1])]
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'tests/diff-abi'),str(ROOT/'tests/loongarch')]
import harness
from arch_profiles import PROFILES
from reference import archive

SOURCES={'init':'resource/api8.c','fd_events':'resource/fd_events.c',
         'flock':'locks/flock.c','mmsg':'network/mmsg.c','copy_range':'io/copy_range.c',
         'icache':'resource/icache.c','sched_query':'resource/sched_query.c'}
LEGACY=tuple(name for name in SOURCES if name!='init')
SOURCES['vectors_v2']='io/vectors_v2.c'
SOURCES['file_advice']='io/file_advice.c'

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def build(args,work):
    profile=PROFILES[args.arch]
    musl=ROOT/('build/riscv-user-smp4/riscv/musl-root' if args.arch=='riscv'
               else 'build/loongarch/musl-root')
    compiler=(Path('/usr/bin/riscv64-linux-gnu-gcc') if args.arch=='riscv'
              else ROOT/'build/tools/loongarch/bin/loongarch64-unknown-linux-gnu-gcc')
    if args.libc=='musl':compiler=musl/'bin/musl-gcc'
    env=os.environ.copy()
    if args.arch=='loongarch' and args.libc=='musl':
        env['REALGCC']=str(ROOT/'build/loongarch/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc')
    flags=profile.musl_flags(compiler) if args.libc=='musl' else []
    if args.arch=='loongarch' and args.libc=='glibc':flags+=['-Wl,-z,max-page-size=16384']
    if args.dynamic:
        loader=re.search(r'-dynamic-linker\s+(\S+)',(musl/'lib/musl-gcc.specs').read_text())[1]
        flags += ['-Wl,--dynamic-linker=/lib/'+Path(loader).name]
    programs={}
    for name,source in SOURCES.items():
        if name!='init' and name not in args.programs:continue
        if name=='icache' and args.arch!='riscv':continue
        destination=work/name
        command=[str(compiler),*flags,*([] if args.dynamic else ['-static']),
                 '-O2','-pthread','-Wall','-Wextra','-Werror',
                 str(ROOT/'tests/workloads'/source),'-lrt','-o',str(destination)]
        with (work/(name+'-compile.log')).open('w') as log:
            subprocess.run(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
        programs[name]={'path':str(destination),'sha256':digest(destination),'command':command}
    image=harness.fixture(work,work/'init')
    commands=['mkdir /proc','mkdir /tmp','mkdir /dev/mqueue']
    for name in programs:
        if name!='init':commands += [f'write "{work/name}" /{name}',f'set_inode_field /{name} mode 0100755']
    if 'vectors_v2' in programs:
        empty=work/'append-only';empty.write_bytes(b'')
        commands += [f'write "{empty}" /vectors-append-only',
                     'set_inode_field /vectors-append-only mode 0100600',
                     'set_inode_field /vectors-append-only flags 0x80020']
    selection=work/'api8-programs'
    selection.write_text(''.join('/'+name+'\n' for name in args.programs))
    commands += [f'write \"{selection}\" /api8-programs']
    if args.dynamic:
        headers=subprocess.check_output(['readelf','-lW',str(work/'init')],text=True)
        interpreter=re.findall(r'Requesting program interpreter: ([^\]]+)',headers)
        if len(interpreter)!=1 or not interpreter[0].startswith('/lib/'):
            raise RuntimeError('unexpected musl interpreter')
        commands += ['mkdir /lib',f'write "{musl/"lib/libc.so"}" {interpreter[0]}',
                     f'set_inode_field {interpreter[0]} mode 0100755',
                     f'write "{musl/"lib/libc.so"}" /lib/libc.so']
    supervisor=work/'supervisor'
    command=[profile.compiler,*profile.raw_flags,'-O2','-ffreestanding','-fno-builtin',
             '-fno-stack-protector','-nostdlib','-nostartfiles','-static','-no-pie',
             '-Wl,--build-id=none','-T',str(ROOT/'tests/common/user.ld'),
             str(ROOT/'tests/loongarch/root_linux_init.c'),str(ROOT/'tests/common/user_start.S'),'-o',str(supervisor)]
    if args.arch=='riscv':command+=['-DROOT_DIRECT_FILESYSTEM=1']
    else:command+=['-Wl,-z,max-page-size=16384']
    harness.run_logged(command,work/'supervisor-compile.log')
    if args.arch=='riscv':
        commands += [f'write "{supervisor}" /supervisor','set_inode_field /supervisor mode 0100755']
    else:
        (work/'initramfs.gz').write_bytes(archive([
            ('dev',0o040755,b'',0,0),('dev/console',0o020600,b'',5,1),
            ('init',0o100755,supervisor.read_bytes(),0,0),('TRAILER!!!',0,b'',0,0)]))
    batch=work/'payload.debugfs';batch.write_text('\n'.join(commands)+'\n')
    harness.run_logged(['debugfs','-w','-f',str(batch),str(image)],work/'payload.log')
    harness.run_logged(['e2fsck','-fn',str(image)],work/'fixture-fsck.log')
    return image,programs

def accepted(raw,arch,cpus,target,selected=None):
    programs=[name for name in (selected or LEGACY) if name!='icache' or arch=='riscv']
    ends=[line for line in raw.replace('\r','').splitlines() if line.startswith('API8 END ')]
    expected=[f'API8 END /{name} status=0' for name in programs]
    known={'fd_events':['FD-EVENTS PASS'],'flock':['FLOCK-PASS'],
           'file_advice':['ADVICE RESULT cases=3 failures=0'],'copy_range':['COPY_RANGE PASS'],'sched_query':['SCHED-QUERY END cases=7'],
           'icache':[f'ICACHE PASS cpus={cpus}'],
           'vectors_v2':[*(f'VECTORS2 CASE {name} PASS' for name in (
               'positions-libc-native-width','per-call-append-sync','vectors-errors-prefix',
               'signal-targets-seals','caller-cpus-atomic-append')),'VECTORS2 PASS cases=5']}
    markers=[f'API8 PASS programs={len(programs)}']
    for name in programs:markers+=known.get(name,[])
    lines=raw.replace('\r','').splitlines()
    success=(f'Linux {"RV" if arch=="riscv" else "LA"} root application passed' in raw
             if target=='linux' else PROFILES[arch].root_success(raw,0))
    mmsg=[line for line in lines if re.fullmatch(r'MMSG PASS cases=[1-9][0-9]*',line)]
    limited=('vectors_v2' not in programs or target!='boaros' or all(
        lines.count(f'VECTORS2 CAP flag={flag} result=-95')==1 for flag in (8,64,128,2147483648)))
    return limited and ends==expected and ('mmsg' not in programs or len(mmsg)==1) and all(lines.count(marker)==1 for marker in markers) and success and 'API8 FAIL' not in raw

def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--arch',choices=PROFILES,default='riscv')
    parser.add_argument('--program',action='append',choices=[name for name in SOURCES if name!='init'],
                        help='run only selected existing callers (repeatable)')
    parser.add_argument('--libc',choices=('musl','glibc'),default='musl')
    parser.add_argument('--dynamic',action='store_true')
    parser.add_argument('--kernel',type=Path)
    parser.add_argument('--cpus',type=int,choices=range(1,9),default=1)
    parser.add_argument('--memory',default='512M')
    parser.add_argument('--vfat',action='store_true',help='also check copy_file_range on a BoarOS VFAT disk')
    parser.add_argument('--only',choices=('linux','boaros'))
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if args.dynamic and args.libc!='musl':parser.error('--dynamic requires musl')
    args.programs=tuple(name for name in dict.fromkeys(args.program or LEGACY) if name!='icache' or args.arch=='riscv')
    profile=PROFILES[args.arch]
    work=(args.output or ROOT/'build/api8'/str(time.time_ns())).resolve()
    work.mkdir(parents=True,exist_ok=False)
    report={'arch':args.arch,'libc':args.libc,'dynamic':args.dynamic,'cpus':args.cpus,'runs':[], 'status':'preparing'}
    try:
        fixture,programs=build(args,work)
        report.update(programs=programs,fixture_sha256=digest(fixture))
        kernels=[]
        if args.only!='boaros':
            if args.arch=='riscv':
                linux,identity=harness.linux_build(ROOT/'tests/smp/linux-user.config')
                report['linux_identity']=identity
            else:linux=profile.linux_kernel()
            kernels.append(('linux',linux))
        if args.only!='linux':kernels.append(('boaros',args.kernel or ROOT/profile.kernel))
        qemu=os.environ.get('QEMU_RISCV64' if args.arch=='riscv' else 'QEMU_LOONGARCH64',profile.qemu)
        report['qemu']=subprocess.check_output([qemu,'--version'],text=True)
        for target,kernel in kernels:
            snapshot=work/(target+'-kernel');shutil.copyfile(kernel,snapshot)
            disk=work/(target+'.img');shutil.copyfile(fixture,disk)
            command=profile.boot(qemu,snapshot,args.memory)+['-drive',f'file={disk},if=none,format=raw,id=root',
                      '-device',profile.block('modern'),'-net','none']
            command[command.index('-smp')+1]=str(args.cpus)
            fat=None
            if args.vfat and target=='boaros':
                marker=work/'with-vfat';marker.write_text('VFAT copy consumer\n')
                batch=work/'vfat.debugfs';batch.write_text(f'write "{marker}" /with-vfat\n')
                harness.run_logged(['debugfs','-w','-f',str(batch),str(disk)],work/'vfat-install.log')
                fat=work/'fat.img'
                with fat.open('wb') as stream:stream.truncate(64*1024*1024)
                harness.run_logged(['mkfs.fat','-F','32','-s','1',str(fat)],work/'mkfs-fat.log')
                device=('virtio-blk-device,drive=fat,bus=virtio-mmio-bus.1' if args.arch=='riscv'
                        else 'virtio-blk-pci,drive=fat,addr=2,disable-legacy=on')
                command+=['-drive',f'file={fat},if=none,format=raw,id=fat','-device',device]
            if target=='linux':
                command+=(['-append','root=/dev/vda rw rootwait console=ttyS0 init=/supervisor loglevel=3 panic=-1']
                          if args.arch=='riscv' else ['-initrd',str(work/'initramfs.gz'),'-append','console=ttyS0 rdinit=/init loglevel=3'])
            record={'target':target,'kernel_sha256':digest(snapshot),'command':command,'returncode':0}
            log=work/(target+'.log')
            try:harness.run_logged(command,log,180)
            except (RuntimeError,TimeoutError) as error:record.update(returncode=-1,error=str(error))
            raw=log.read_text(errors='replace')
            record['passed']=not record['returncode'] and accepted(raw,args.arch,args.cpus,target,args.programs)
            report['runs'].append(record)
            if not record['passed']:raise RuntimeError(f'{target} caller/exit/owner failure: {log}')
            harness.run_logged(['e2fsck','-fn',str(disk)],work/(target+'-fsck.log'))
            if fat:
                harness.run_logged(['fsck.fat','-n',str(fat)],work/'fat-fsck.log');fat.unlink()
            if any(digest(row['path'])!=row['sha256'] for row in programs.values()):
                raise RuntimeError('caller ELF changed during comparison')
            disk.unlink();print(target,'PASS',work,flush=True)
        fixture.unlink();report['status']='passed'
    except Exception as error:
        report.update(status='failed',error=str(error));raise
    finally:(work/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    return 0

if __name__=='__main__':sys.exit(main())
