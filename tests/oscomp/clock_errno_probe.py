#!/usr/bin/env python3
"""Attribute the original static glibc clock case; this is not official scoring."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from arch_profiles import PROFILES

# 这些位置只描述该外部原ELF；字节身份变化时必须重新调查，不能沿用PC猜测。
ORIGINAL_SHA='f140123cee82e5a0a1fc48ef9d845f24be920dfac9a07e5bd31ead053d3657ef'
POINTS={'TEXT_START':0x10000,'TEXT_END':0xfa514,'ERRNO_OFFSET':136,
        'MALLOC_INIT_PC':0x518c6,'MAIN_PC':0x23692,'CLOCK_RETURN_PC':0x10e32}

def run(command,**kwargs):
    return subprocess.run([str(v) for v in command],check=True,**kwargs)

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def observation(text,serial):
    def one(pattern):
        matches=re.findall(pattern,text,re.MULTILINE)
        if len(matches)!=1:raise ValueError('missing or duplicate original program observation: '+pattern)
        return matches[0]
    result={
        'malloc_entry_errno':int(one(r'^MALLOC-INIT .* errno=(-?\d+)$')),
        'getrandom_return':int(one(r'^RETURN nr=278 .* result=(-?\d+) errno=')),
        'main_errno':int(one(r'^MAIN .* errno=(-?\d+)$')),
        'clock_return':int(one(r'^CLOCK-RESULT .* result=(-?\d+) errno=')),
        'clock_errno':int(one(r'^CLOCK-RESULT .* errno=(-?\d+)$')),
        'child_wait_status':None,
        'first_errno_write':next((line for line in text.splitlines() if line.startswith('ERRNO-STORE ')),None),
        'return_injected':'INJECT ' in text}
    waits=re.findall(r'AUDIT WAIT pid=\d+ status=(\d+)',serial)
    if len(waits)!=1:raise ValueError('missing or duplicate native wait status')
    result['child_wait_status']=int(waits[0])
    if result['malloc_entry_errno']!=0 or result['clock_return']!=0:
        raise ValueError('this does not prove initialization leaked errno across a successful clock call')
    expected=(11,256) if result['getrandom_return']==-11 else (0,0)
    if result['getrandom_return'] not in (-11,8) or (result['main_errno'],result['child_wait_status'])!=expected or result['clock_errno']!=expected[0]:
        raise ValueError('original program behavior differs; preserve the run for investigation')
    if result['getrandom_return']==-11:
        writer=result['first_errno_write']
        if (writer is None or not re.search(r' value=11\b',writer)
                or not text.index('RETURN nr=278 ')<text.index(writer)<text.index('MAIN ')):
            raise ValueError('the first errno writer is missing or outside the initialization interval')
    return result

def main():
    parser=argparse.ArgumentParser(__doc__);parser.add_argument('--output',type=Path)
    args=parser.parse_args();arch=PROFILES['riscv']
    (ROOT/'build').mkdir(exist_ok=True)
    directory=args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='oscomp-clock-errno-',dir=ROOT/'build'))
    if args.output:
        if directory.exists() or not directory.is_relative_to(ROOT/'build'):parser.error('output must be new and inside build/')
        directory.mkdir(parents=True)
    source=ROOT/'references/oscomp-autotest/sdcard-rv.img';original=directory/'original.exe'
    run(['debugfs','-R','dump /glibc/entry-static.exe '+str(original),source],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    if digest(original)!=ORIGINAL_SHA:raise RuntimeError('original ELF identity differs')
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True).split()
    plugin=directory/'observer.so'
    run(['cc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',*(f'-D{k}={v}' for k,v in POINTS.items()),
         ROOT/'tests/workloads/diagnostics/libc-errno-plugin.c','-o',plugin,*flags])
    raw=[arch.compiler,*arch.raw_flags,'-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdlib','-static','-no-pie',
         '-Wl,--build-id=none','-Wl,-T,'+str(ROOT/'tests/riscv/user_elf.ld')]
    for name,defines in [('driver',[]),('driver-ready',['-DREQUIRE_RANDOM_READY']),('linux-init',['-DROOT_DIRECT_FILESYSTEM','-DROOT_CREATE_SESSION'])]:
        program=ROOT/('tests/loongarch/root_linux_init.c' if name=='linux-init' else 'tests/oscomp/clock_errno_driver.c')
        run([*raw,*defines,program,ROOT/'tests/common/user_start.S','-o',directory/name])
    config=directory/'init.json';config.write_text(json.dumps({'path':'/init','argv':['/init'],'envp':[]})+'\n')
    with (directory/'build.log').open('wb') as out:run(['make','-j8','kernel-rv','INIT_CONFIG='+str(config)],cwd=ROOT,stdout=out,stderr=subprocess.STDOUT)
    kernel=directory/'kernel-rv';shutil.copyfile(ROOT/'kernel-rv',kernel);linux=arch.linux_kernel()
    base=directory/'base.img'
    with base.open('wb') as out:out.truncate(64*1024*1024)
    run(['mkfs.ext4','-q','-F','-b4096','-O','^metadata_csum,^64bit,^orphan_file',base])
    def edit(image,command):
        result=run(['debugfs','-w','-R',command,image],capture_output=True,text=True)
        if re.search(r'No such|not found|File exists|Could not',result.stderr):raise RuntimeError(result.stderr)
    edit(base,'mkdir /glibc');edit(base,'write '+str(original)+' /glibc/entry-static.exe');edit(base,'set_inode_field /glibc/entry-static.exe mode 0100755')
    metadata={'kind':'original-runtime-diagnostic','original_sha256':ORIGINAL_SHA,'points':POINTS,
        'kernel_sha256':digest(kernel),'linux_sha256':digest(linux),'qemu':subprocess.check_output([arch.qemu,'--version'],text=True).splitlines()[0],
        'qemu_sha256':digest(shutil.which(arch.qemu)),
        'compiler':subprocess.check_output([arch.compiler,'--version'],text=True).splitlines()[0],
        'driver_sha256':digest(directory/'driver'),'ready_driver_sha256':digest(directory/'driver-ready'),'linux_init_sha256':digest(directory/'linux-init'),
        'plugin_sha256':digest(plugin),'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        'source_dirty':subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True),
        'sources':{str(path.relative_to(ROOT)):digest(path) for path in [Path(__file__).resolve(),ROOT/'tests/oscomp/clock_errno_driver.c',ROOT/'tests/workloads/diagnostics/libc-errno-plugin.c',ROOT/'tests/common/user_start.S',ROOT/'tests/loongarch/root_linux_init.c']},'runs':{}}
    (directory/'identity.json').write_text(json.dumps(metadata,indent=2)+'\n')
    for memory in ('512M','1G'):
        for system,mode in [('boaros','missing'),('boaros','ready'),('linux','normal'),('linux','getrandom-error')]:
            name=system+'-'+mode+'-'+memory;image=directory/(name+'.img');shutil.copyfile(base,image)
            edit(image,'write '+str(directory/('driver-ready' if mode=='ready' else 'driver'))+' /init');edit(image,'set_inode_field /init mode 0100755')
            if system=='linux':edit(image,'write '+str(directory/'linux-init')+' /linux-init');edit(image,'set_inode_field /linux-init mode 0100755')
            trace=directory/(name+'.trace');options=str(plugin)+',trace='+str(trace)+(',fault-getrandom=on' if mode=='getrandom-error' else '')
            command=arch.boot(arch.qemu,linux if system=='linux' else kernel,memory)+['-net','none','-drive','file='+str(image)+',if=none,format=raw,id=root','-device',arch.block('modern'),'-plugin',options]
            if system=='linux':command+=['-append','console=ttyS0 root=/dev/vda rw init=/linux-init loglevel=3']
            if mode=='ready':command+=['-object','rng-random,id=entropy,filename=/dev/urandom','-device',arch.rng()]
            log=directory/(name+'.log')
            with log.open('wb') as out:run(command,stdout=out,stderr=subprocess.STDOUT,timeout=90)
            serial=log.read_text(errors='replace');result=observation(trace.read_text(),serial)
            if result['return_injected']!=(mode=='getrandom-error'):raise RuntimeError('wrong injection identity')
            if 'BoarOS: fatal ' in serial:raise RuntimeError('kernel fatal during attribution')
            result.update(command=command,root_resources_verified=('Linux RV root application passed' in serial if system=='linux' else arch.root_success(serial,0)),serial_sha256=digest(log),trace_sha256=digest(trace))
            if not result['root_resources_verified']:raise RuntimeError('root owner teardown was not verified')
            metadata['runs'][name]=result;(directory/'report.json').write_text(json.dumps(metadata,indent=2)+'\n')
            print(name,{k:result[k] for k in ('getrandom_return','main_errno','clock_return','clock_errno','child_wait_status')},flush=True)
    print('Original runtime attribution verified; output:',directory)

if __name__=='__main__':main()
