#!/usr/bin/env python3
"""Explicit repaired-input diagnostic; never used by the official runner."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'tests'))
from arch_profiles import PROFILES
_spec=importlib.util.spec_from_file_location('oscomp_runtime_scoring',HERE/'run.py')
scoring=importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(scoring)

# 仅允许用户批准的外部输入修正；不是内核按程序或地址选择行为。
BRK={
    'riscv':dict(expected_sha='3756dce8d8734a564300ca4404e6b80136f3d369672c03afaffab63da1fae086',
                 machine=243,address=0x1e10,before=bytes.fromhex('0125'),after=bytes.fromhex('0100')),
    'loongarch':dict(expected_sha='d3882df3c12108f783d23be0db1eb66429f750686d54ac1151f545067f0c9310',
                     machine=258,address=0x2478,before=bytes.fromhex('84804000'),after=bytes.fromhex('00004003'))}
LA_LIBC_SHA='816cff1d1abbef3f1423bbce01a56f00c97b6ff8e20d49966c9d4b6d27e5e7be'
LA_CYCLIC_SHA='21c81ebe791caa060a72fcacf1ed4e2b24db289f9af0e872c2fd91ba5c311d38'

def run(command,**kwargs):
    return subprocess.run([str(x) for x in command],check=True,**kwargs)

def digest(path):
    return scoring.sha(path)

def repair_instruction(data,*,expected_sha,machine,address,before,after):
    if hashlib.sha256(data).hexdigest()!=expected_sha:
        raise ValueError('original ELF identity differs')
    if len(data)<64 or data[:7]!=b'\x7fELF\x02\x01\x01' or struct.unpack_from('<H',data,18)[0]!=machine:
        raise ValueError('original ELF machine or format differs')
    if not before or len(before)!=len(after):raise ValueError('instruction width differs')
    offset=struct.unpack_from('<Q',data,32)[0]
    width,count=struct.unpack_from('<HH',data,54)
    if width!=56 or offset+count*width>len(data):raise ValueError('invalid program headers')
    matches=[]
    for i in range(count):
        kind,flags,file_offset,virtual,_,file_size,_,_=struct.unpack_from('<IIQQQQQQ',data,offset+i*width)
        if kind==1 and flags&1 and virtual<=address and address+len(before)<=virtual+file_size:
            target=file_offset+address-virtual
            if target+len(before)>len(data):raise ValueError('load range outside file')
            matches.append(target)
    if len(matches)!=1:raise ValueError('instruction is not uniquely file-backed executable data')
    target=matches[0]
    if data[target:target+len(before)]!=before:raise ValueError('original instruction differs')
    return data[:target]+after+data[target+len(before):]

def brk_observations(text):
    result=[];record=None
    for line in text.splitlines():
        call=re.fullmatch(r'BRK CALL requested=([0-9a-f]+)',line)
        raw=re.fullmatch(r'BRK RAW mode=(original|adapted) value=([0-9a-f]+)',line)
        returned=re.fullmatch(r'BRK USER value=([0-9a-f]+)',line)
        if call:
            if record is not None:raise ValueError('incomplete brk observation')
            record={'requested':int(call[1],16)}
        elif raw:
            if record is None or 'raw' in record:raise ValueError('unpaired raw brk return')
            record.update(mode=raw[1],raw=int(raw[2],16))
        elif returned:
            if record is None or 'raw' not in record:raise ValueError('unpaired user brk return')
            record['user']=int(returned[1],16)
            truncated=(record['raw']&0xffffffff)
            if truncated&0x80000000:truncated|=0xffffffff00000000
            expected=record['raw'] if record['mode']=='adapted' else truncated
            if record['raw']<=0xffffffff or record['user']!=expected:
                raise ValueError('high-address brk return contract not observed')
            result.append(record);record=None
    if record is not None or not result:raise ValueError('missing brk evidence')
    return result

def native_statuses(text,expected):
    if 'RUNTIME EXEC ' in text:raise ValueError('exec failure is not program behavior')
    matches=re.findall(r'^RUNTIME WAIT (\S+) status=(-?\d+)$',text,re.MULTILINE)
    result={name:int(status) for name,status in matches}
    if len(matches)!=len(result) or result!=expected:raise ValueError('native wait status missing, duplicated or unexpected')
    return result

def cyclic_group(text):
    cases={}
    for name,threads in [('NO_STRESS_P1',1),('NO_STRESS_P8',8),('STRESS_P1',1),('STRESS_P8',8)]:
        pattern=r'====== cyclictest '+name+r' begin ======\n(.*?)====== cyclictest '+name+r' end: (\S+) ======'
        matched=re.findall(pattern,text,re.DOTALL)
        if len(matched)!=1 or matched[0][1]!='success':raise ValueError('cyclic child failed or did not complete: '+name)
        counts=[int(value) for value in re.findall(r'\bC:\s*(\d+)',matched[0][0])]
        if len(counts)!=threads:raise ValueError('missing cyclic thread sample records: '+name)
        cases[name]={'samples':counts,'zero_sample_threads':[i for i,count in enumerate(counts) if count==0]}
    return {'cases':cases,'all_threads_sampled':all(not row['zero_sample_threads'] for row in cases.values())}

def edit(image,command):
    image=Path(image)
    if image.is_symlink() or not image.resolve().is_relative_to(ROOT/'build'):
        raise ValueError('only a build-owned diagnostic disk may be written')
    result=run(['debugfs','-w','-R',command,image],capture_output=True,text=True)
    if re.search(r'File not found|No such file|already exists|Could not|not found',result.stderr):
        raise RuntimeError('disk preparation failed: '+result.stderr)

def publish(image,source,target):
    edit(image,'write '+str(source)+' '+target)
    edit(image,'set_inode_field '+target+' mode 0100755')

def build_arch(name,directory,identity):
    # 仅真实运行时加载Linux启动辅助，host发现测试不能污染其他runner的模块查找。
    sys.path.insert(0,str(ROOT/'tests/loongarch'))
    try:
        from reference import archive
    finally:
        sys.path.pop(0)
    arch=PROFILES[name];facts=identity['architectures'][name]
    source=scoring.REF/facts['image'];inputs=directory/'inputs';inputs.mkdir()
    if source.is_symlink() or digest(source)!=identity['assets'][facts['image']]:
        raise ValueError('original release disk identity differs')
    repairs={}
    for libc in ('glibc','musl'):
        original=inputs/(libc+'-brk')
        run(['debugfs','-R','dump /'+libc+'/basic/brk '+str(original),source],capture_output=True)
        patched=inputs/(libc+'-brk-repaired')
        patched.write_bytes(repair_instruction(original.read_bytes(),**BRK[name]))
        repairs[libc]={'original_sha256':digest(original),'repaired_sha256':digest(patched),
                       'instruction_address':BRK[name]['address'],
                       'before':BRK[name]['before'].hex(),'after':BRK[name]['after'].hex()}
    setup=directory/'setup.sh'
    links='\n'.join('$BB ln -sf '+a+' '+b for a,b in facts['loader_links'])
    setup.write_text('#!/musl/busybox sh\nset -e\nBB=/musl/busybox\n'
        '$BB mkdir -p /bin /lib /lib64 /usr/lib64 /proc /dev/shm\n'
        '$BB --install -s /bin\n'+links+'\n'
        '$BB chmod +x /glibc/basic/run-all.sh /musl/basic/run-all.sh\n'
        '$BB mount -t proc proc /proc\n$BB mount -t tmpfs -o mode=1777 tmpfs /dev/shm\n')
    flags=[arch.compiler,*arch.raw_flags,'-O2','-ffreestanding','-fno-builtin',
           '-fno-stack-protector','-nostdlib','-static','-no-pie','-Wl,--build-id=none',
           '-Wl,-z,max-page-size='+str(arch.page_size)]
    linker=ROOT/('tests/loongarch/user.ld' if name=='loongarch' else 'tests/riscv/user_elf.ld')
    driver=directory/'driver'
    run([*flags,'-T',linker,HERE/'user_runtime_driver.c',ROOT/'tests/common/user_start.S','-o',driver])
    config=directory/'init.json';config.write_text(json.dumps({'path':'/init','argv':['/init'],'envp':[]})+'\n')
    environment=os.environ.copy()
    for key in ('INIT_CONFIG','INIT_CONFIG_RV','INIT_CONFIG_LA','MAKEFLAGS','MFLAGS','MAKEOVERRIDES'):environment.pop(key,None)
    with (directory/'build.log').open('wb') as out:
        run(['make','-j8',arch.kernel,'INIT_CONFIG='+str(config)],cwd=ROOT,env=environment,stdout=out,stderr=subprocess.STDOUT)
    kernel=directory/arch.kernel;shutil.copyfile(ROOT/arch.kernel,kernel)
    linux_init=directory/'linux-init'
    run([*flags,'-DROOT_CREATE_SESSION','-DROOT_REAP_CHILDREN',*(['-DROOT_DIRECT_FILESYSTEM'] if name=='riscv' else []),
         '-T',linker,ROOT/'tests/loongarch/root_linux_init.c',ROOT/'tests/common/user_start.S','-o',linux_init])
    ramdisk=directory/'initramfs.gz'
    ramdisk.write_bytes(archive([('dev',0o040755,b'',0,0),('dev/console',0o020600,b'',5,1),
        ('init',0o100755,linux_init.read_bytes(),0,0),('TRAILER!!!',0,b'',0,0)]))
    runtime={}
    if name=='loongarch':
        libc=inputs/'libc.so';cyclic=inputs/'cyclictest'
        for target,path in [(libc,'/musl/lib/libc.so'),(cyclic,'/musl/cyclictest')]:
            run(['debugfs','-R','dump '+path+' '+str(target),source],capture_output=True)
        if digest(libc)!=LA_LIBC_SHA or digest(cyclic)!=LA_CYCLIC_SHA:
            raise ValueError('original LA runtime or cyclictest differs')
        dynamic=[arch.compiler,'-mabi=lp64d','-mno-lsx','-mno-lasx','-O2','-fno-builtin',
                 '-fno-stack-protector','-nostdlib','-Wl,--build-id=none','-Wl,-z,max-page-size=16384']
        shim=directory/'sched.so';probe=directory/'sched-probe'
        run([*dynamic,'-shared','-fPIC',HERE/'sched_compat.c','-o',shim])
        run([*dynamic,'-no-pie','-Wl,-e,_start','-Wl,--dynamic-linker=/lib64/ld-musl-loongarch-lp64d.so.1',
             HERE/'sched_compat_probe.c',ROOT/'tests/common/user_start.S','-L'+str(inputs),'-l:libc.so','-o',probe])
        runtime={'original_libc_sha256':digest(libc),'original_cyclictest_sha256':digest(cyclic),
                 'shim_sha256':digest(shim),'probe_sha256':digest(probe)}
    return source,kernel,linux_init,ramdisk,repairs,runtime

def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--arch',choices=('both',*PROFILES),default='both')
    parser.add_argument('--output',type=Path)
    args=parser.parse_args();names=tuple(PROFILES) if args.arch=='both' else (args.arch,)
    (ROOT/'build').mkdir(exist_ok=True)
    directory=args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='oscomp-user-runtime-',dir=ROOT/'build'))
    if args.output:
        if directory.exists() or not directory.is_relative_to(ROOT/'build'):parser.error('output must be new and inside build/')
        directory.mkdir(parents=True)
    report={'kind':'repaired-user-runtime-diagnostic','official':False,
            'kernel_commit':scoring.output(['git','rev-parse','HEAD']),
            'kernel_dirty':scoring.output(['git','status','--porcelain']),
            'source_inputs':scoring.validate(False,names),'release_assets_verified_this_run':False,
            'architectures':{},'complete':False}
    report['diagnostic_sources']={str(path.relative_to(ROOT)):digest(path) for path in
        [Path(__file__),HERE/'sched_compat.c',HERE/'sched_compat_probe.c',HERE/'user_runtime_driver.c',
         ROOT/'tests/workloads/diagnostics/brk-return-plugin.c',ROOT/'tests/loongarch/root_linux_init.c']}
    (directory/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    pkg=subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True).split()
    plugin=directory/'brk-observer.so'
    run(['cc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',ROOT/'tests/workloads/diagnostics/brk-return-plugin.c','-o',plugin,*pkg])
    config=json.loads((scoring.REF/'kernel/judge/config.json').read_text())
    try:
        for name in names:
            target=directory/name;target.mkdir();arch=PROFILES[name]
            source,kernel,linux_init,ramdisk,repairs,runtime=build_arch(name,target,report['source_inputs'])
            info={'brk_repairs':repairs,'runtime':runtime,'kernel_sha256':digest(kernel),
                  'driver_sha256':digest(target/'driver'),'setup_sha256':digest(target/'setup.sh'),
                  'linux_init_sha256':digest(target/'linux-init'),
                  'config_sha256':digest(target/'init.json'),'observer_sha256':digest(plugin),
                  'linux_sha256':digest(arch.linux_kernel()),'qemu_sha256':digest(shutil.which(arch.qemu)),
                  'qemu_version':scoring.output([arch.qemu,'--version']).splitlines()[0],
                  'compiler_version':scoring.output([arch.compiler,'--version']).splitlines()[0],
                  'source_image_sha256':report['source_inputs']['assets'][source.name],
                  'source_image_verified_before':True,'source_image_verified_after':False,'runs':{}}
            report['architectures'][name]=info
            for memory in ('512M','1G'):
                for system in ('linux','boaros'):
                    label=system+'-'+memory;image=target/(label+'.img')
                    run(['cp','--reflink=auto','--sparse=always',source,image])
                    edit(image,'mkdir /compat')
                    for host,guest in [(target/'driver','/init'),(target/'setup.sh','/compat/setup.sh'),
                                       (target/'inputs/glibc-brk-repaired','/compat/brk-glibc'),
                                       (target/'inputs/musl-brk-repaired','/compat/brk-musl')]:publish(image,host,guest)
                    if name=='loongarch':
                        publish(image,target/'sched.so','/compat/sched.so')
                        publish(image,target/'sched-probe','/compat/sched-probe')
                    if system=='linux' and name=='riscv':publish(image,linux_init,'/linux-init')
                    log=target/(label+'.log');trace=target/(label+'.trace')
                    command=arch.boot(arch.qemu,arch.linux_kernel() if system=='linux' else kernel,memory)
                    command+=['-net','none','-drive','file='+str(image)+',if=none,format=raw,id=root',
                              '-device',arch.block('modern'),'-plugin',str(plugin)+',trace='+str(trace)]
                    if system=='linux':command+=(['-append','console=ttyS0 root=/dev/vda rw init=/linux-init loglevel=3']
                        if name=='riscv' else ['-initrd',str(ramdisk),'-append','console=ttyS0 rdinit=/init loglevel=3'])
                    print(name,label,'running original/repaired input comparison',flush=True)
                    with log.open('wb') as out:result=run(command,cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,timeout=180)
                    text=log.read_text(errors='replace').replace('\r\n','\n')
                    expected={'setup':0,**{'basic-'+mode+'-'+libc:0 for mode in ('original','adapted') for libc in ('glibc','musl')},
                              'cyclic-original':256 if name=='loongarch' else 0}
                    if name=='loongarch':expected.update({'sched-original':91<<8,'sched-adapted':0,'cyclic-adapted':0,'cyclic-group-adapted':0})
                    statuses=native_statuses(text,expected)
                    owner=('Linux '+('LA' if name=='loongarch' else 'RV')+' root application passed' in text
                           if system=='linux' else arch.root_success(text,0))
                    if system=='linux' and 'remaining children reaped' not in text:owner=False
                    heap_zero='heap-live=0' in text if name=='loongarch' else 'heap-live=0x0' in text
                    if not owner or (system=='boaros' and not heap_zero) or scoring.kernel_failure(text) or 'RUNTIME DONE' not in text:
                        raise RuntimeError('root owner closure or full progress missing: '+str(log))
                    observations=brk_observations(trace.read_text())
                    if len(observations)!=20 or [x['mode'] for x in observations]!=['original']*10+['adapted']*10:
                        raise ValueError('missing original or repaired brk invocations')
                    for start in (10,15):
                        calls=observations[start:start+5]
                        base=calls[0]['raw']
                        if ([x['requested'] for x in calls]!=[0,base+64,0,base+128,0] or
                            [x['raw'] for x in calls]!=[base,base+64,base+64,base+128,base+128]):
                            raise ValueError('repaired program did not actually grow the high-address heap')
                    groups={}
                    for mode in ('original','adapted'):
                        for libc in ('glibc','musl'):
                            case='basic-'+mode+'-'+libc
                            section=text.split('RUNTIME BEGIN '+case+'\n',1)[1].split('RUNTIME WAIT '+case+' ',1)[0]
                            excerpt=target/(label+'-'+case+'.log');excerpt.write_text(section)
                            judged=scoring.judge(excerpt,config)['basic-'+libc]
                            brk=next(row for row in judged if row['name']=='test_brk')
                            if brk['score']!=(1 if mode=='original' else 3):raise ValueError('original/repaired brk judge result differs')
                            groups[case]={'score':sum(row['score'] for row in judged),'results':judged}
                    differences={}
                    for libc in ('glibc','musl'):
                        original=groups['basic-original-'+libc]['results']
                        adapted=groups['basic-adapted-'+libc]['results']
                        by_name={row['name']:row for row in adapted}
                        # 原pipe字符输出可交错；两次独立执行的其他分差如实记录，不推断因果。
                        differences[libc]=[{'original':row,'adapted':by_name[row['name']]} for row in original
                                           if row['name']!='test_brk' and row!=by_name[row['name']]]
                    if name=='loongarch':
                        for case in ('cyclic-original','cyclic-adapted','cyclic-group-adapted'):
                            section=text.split('RUNTIME BEGIN '+case+'\n',1)[1].split('RUNTIME WAIT '+case+' ',1)[0]
                            excerpt=target/(label+'-'+case+'.log');excerpt.write_text(section)
                            groups[case]={'sample_lines':[line for line in section.splitlines() if re.search(r'\bC:\s*\d+',line)]}
                            if case=='cyclic-group-adapted':
                                groups[case].update(cyclic_group(section))
                                groups[case]['results']=scoring.judge(excerpt,config)['cyclictest-musl']
                            elif case=='cyclic-adapted' and not any(re.search(r'\bC:\s*[1-9]\d*',line) for line in section.splitlines()):
                                raise ValueError('successful cyclic child produced no samples')
                    info['runs'][label]={'native_statuses':statuses,'brk':observations,'groups':groups,
                                        'other_basic_differences':differences,
                                        'command':list(map(str,command)),'serial_sha256':digest(log),'trace_sha256':digest(trace),
                                        'root_resources_verified':owner,'qemu_returncode':result.returncode}
                    (directory/'report.json').write_text(json.dumps(report,indent=2)+'\n')
                    print(name,label,'brk 1→3; native statuses and root owners verified',flush=True)
            if digest(source)!=info['source_image_sha256']:raise ValueError('original release disk changed during diagnosis')
            info['source_image_verified_after']=True
        report['complete']=True
    except Exception as error:
        report['error']={'type':type(error).__name__,'message':str(error)}
        raise
    finally:
        environment=os.environ.copy()
        for key in ('INIT_CONFIG','INIT_CONFIG_RV','INIT_CONFIG_LA','MAKEFLAGS','MFLAGS','MAKEOVERRIDES'):
            environment.pop(key,None)
        with (directory/'restore-defaults.log').open('wb') as out:
            restored=subprocess.run(['make','-j8','all'],cwd=ROOT,env=environment,stdout=out,stderr=subprocess.STDOUT)
        report['defaults_restored']=restored.returncode==0
        if restored.returncode:report['complete']=False
        (directory/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        if restored.returncode and 'error' not in report:raise RuntimeError('default kernel build restoration failed')
    print('Repaired-input diagnostic completed:',directory,flush=True)

if __name__=='__main__':main()
