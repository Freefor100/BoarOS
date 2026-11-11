#!/usr/bin/env python3
"""Boot original target BPF executables on the fixed Linux BPF profile."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

from build import ARCHIVES, BASE, ROOT, TARGETS, references, run, sha
sys.path.insert(0,str(ROOT/'tests'))
from arch_profiles import PROFILES
sys.path.insert(0,str(ROOT/'tests/loongarch'))
from guest import run_guest


def install_runtime(arch,mode):
    tree=BASE/arch/'root'
    for name in ('lib','lib64','usr/lib','bin','dev','proc','sys','tmp','work'):
        (tree/name).mkdir(parents=True,exist_ok=True)
    source=Path('/usr/riscv64-linux-gnu/lib' if arch=='riscv' else '/opt/loongarch64-tools/target/lib64')
    for pattern in ('ld-linux-*.so.1','libc.so.6','libm.so.6','libgcc_s.so.1','libstdc++.so.6','libatomic.so.1','libzstd.so.1'):
        for path in source.glob(pattern):
            shutil.copy2(path,tree/('lib' if arch=='riscv' else 'lib64')/path.name,follow_symlinks=True)
    busybox=ROOT/('build/program-environment/full-busybox/source/busybox/busybox' if arch=='riscv' else 'build/loongarch/busybox-source/busybox/busybox')
    shutil.copy2(busybox,tree/'bin/busybox')
    for command in ('sh','mount','mkdir','rmdir','rm','cat','grep','mkfifo'):
        link=tree/'bin'/command
        if not link.exists():link.symlink_to('busybox')
    for name in ('run.sh','syscalls.bt'):
        shutil.copy2(ROOT/'tests/workloads/bpf'/name,tree/'work'/name)
    # Runtime libraries produced by these builds have their canonical SONAMEs.
    llvm=BASE/arch/'llvm/lib'
    for pattern in ('libLLVM.so*','libclang-cpp.so*','libclang.so*'):
        for path in llvm.glob(pattern):
            if path.is_file():
                target=tree/'usr/lib'/path.name
                target.unlink(missing_ok=True)
                if mode!='full':continue
                if path.is_symlink():target.symlink_to(path.readlink().name)
                else:shutil.copy2(path,target)
    return tree


def baseline(arch,mode,timeout,platform='linux',kernel_argument=None,cpus=1,output=None):
    profile=PROFILES[arch]
    suffix='' if cpus==1 else '-'+str(cpus)+'cpu'
    directory=output or BASE/arch/(('baseline-' if platform=='linux' else 'boaros-')+mode+suffix)
    directory.mkdir(parents=True,exist_ok=True)
    tree=install_runtime(arch,mode)
    (tree/'work/profile').write_text(mode+' '+platform+'\n')
    kernel_dir=BASE/arch/'linux'
    kernel=kernel_dir/('arch/riscv/boot/Image' if arch=='riscv' else 'vmlinux')
    if platform=='linux':
        if kernel_argument:raise RuntimeError('Linux reference must use its fixed BPF build identity')
        identity=json.loads((kernel_dir/'identity.json').read_text())
        if sha(kernel)!=identity['image_sha256'] or sha(kernel_dir/'.config')!=identity['configuration_sha256']:
            raise RuntimeError('BPF Linux build identity mismatch')
    else:
        kernel=kernel_argument or ROOT/profile.kernel
        identity={'image_sha256':sha(kernel)}
    snapshot=directory/'kernel';shutil.copyfile(kernel,snapshot);kernel=snapshot
    image=directory/'root.ext4'
    with image.open('wb') as stream:stream.truncate(768*1024*1024)
    run(['mkfs.ext4','-q','-F','-b','4096','-d',tree,image],directory/'mkfs.log')
    devices=directory/'devices.debugfs'
    devices.write_text('cd /dev\nmknod console c 5 1\nset_inode_field console mode 020600\n'
                       'mknod null c 1 3\nset_inode_field null mode 020666\n'
                       'mknod zero c 1 5\nset_inode_field zero mode 020666\n')
    run(['debugfs','-w','-f',devices,image],directory/'devices.log')
    qemu=profile.qemu
    if arch=='riscv':qemu='qemu-system-riscv64'
    command=profile.boot(qemu,kernel,'1G')+[
        '-drive','file='+str(image)+',if=none,format=raw,id=root',
        '-device',profile.block('modern'),'-net','none',
        '-object','rng-random,filename=/dev/urandom,id=entropy','-device',profile.rng()]
    command[command.index('-smp')+1]=str(cpus)
    if platform=='linux':command+=['-append','console=ttyS0 root=/dev/vda rw rootwait init=/init loglevel=3']
    programs=['init','usr/bin/bpftool','usr/bin/bpf-udp','usr/bin/bpf-syscalls',
              'work/udp.bpf.o','work/syscalls.bt','work/run.sh']
    if mode=='full':programs.append('usr/bin/bpftrace')
    rows=references()
    metadata={'arch':arch,'platform':platform,'mode':mode,'cpus':cpus,'command':command,'kernel':identity,
              'programs':{name:sha(tree/name) for name in programs},
              'sources':{name:rows[name] for name in ('linux',*ARCHIVES.values())},
              'runtime':{str(path.relative_to(tree)):sha(path) for folder in ('lib','lib64','usr/lib')
                         for path in (tree/folder).glob('*.so*') if path.is_file() and
                         (mode=='full' or not path.name.startswith(('libLLVM','libclang')))}}
    if platform=='boaros':
        linux=json.loads((BASE/arch/('baseline-'+mode+suffix)/'result.json').read_text())
        if not linux['passed'] or linux.get('cpus',1)!=cpus or linux['programs']!=metadata['programs'] or linux['runtime']!=metadata['runtime']:
            raise RuntimeError('BPF tools/runtime differ from recorded fixed-Linux run; run Linux with these inputs first')
    (directory/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
    try:code,text=run_guest(command,timeout=timeout)
    except subprocess.TimeoutExpired as error:
        code=-1;text=error.output
        if isinstance(text,bytes):text=text.decode(errors='replace')
    (directory/'console.log').write_text(text)
    success=not code and 'BPF GUEST INIT PASS' in text and 'BPF ORIGINAL TOOLS BASELINE '+mode+' OK' in text
    if platform=='boaros':success=success and profile.root_success(text,0)
    metadata.update(qemu_returncode=code,passed=success)
    (directory/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print('\n'.join(text.splitlines()[-35:]))
    if not success:raise RuntimeError('original BPF '+platform+' workload failed: '+str(directory/'console.log'))
    image.unlink()
    snapshot.unlink()
    return directory

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--arch',choices=TARGETS,required=True)
    p.add_argument('--mode',choices=('bpftool','full'),default='full');p.add_argument('--timeout',type=int,default=300)
    p.add_argument('--platform',choices=('linux','boaros'),default='linux');p.add_argument('--kernel',type=Path)
    p.add_argument('--cpus',type=int,choices=(1,2,8),default=1)
    p.add_argument('--output',type=Path,help='separate result directory for another production profile')
    a=p.parse_args();baseline(a.arch,a.mode,a.timeout,a.platform,a.kernel,a.cpus,a.output)
