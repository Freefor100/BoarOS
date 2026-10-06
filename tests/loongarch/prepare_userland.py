#!/usr/bin/env python3
"""Build fixed integer-only GCC runtime, musl and unmodified full BusyBox caches."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
from prepare import compiler_identity, fixed_repository
ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'build/loongarch'
FLAGS = ['-mabi=lp64s', '-msoft-float', '-mno-lsx', '-mno-lasx']

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def archive(name):
    rows = [line.split('\t') for line in (ROOT/'references/sources.tsv').read_text().splitlines()
            if line and not line.startswith('#')]
    row = next(r for r in rows if r[1] == name)
    path = ROOT/'references'/name
    if sha(path) != row[4]:
        raise SystemExit(f'fixed archive checksum mismatch: {path}')
    return path

def run(command, directory, log, env=None, input=None):
    with (BASE/log).open('a') as output:
        output.write('\n'+repr(list(map(str, command)))+'\n'); output.flush()
        result = subprocess.run(list(map(str, command)), cwd=directory, env=env,
                                stdout=output, stderr=subprocess.STDOUT, input=input, text=True)
    if result.returncode:
        raise SystemExit(f'build failed ({result.returncode}); see {BASE/log}')

def extract(path, directory):
    directory.mkdir(parents=True, exist_ok=True)
    with tarfile.open(path) as content:
        content.extractall(directory, filter='data')

def cache_inputs(args):
    gcc_tar=archive('gcc/gcc-15.1.0.tar.xz'); musl_tar=archive('musl/musl-1.2.5.tar.gz')
    uapi_tar=archive('linux-uapi/linux-6.6.tar.xz')
    inputs=json.loads((ROOT/'tests/program-inventory/inputs.json').read_text())['busybox']
    tests_source,_=fixed_repository('oscomp-testsuits')
    try:
        configuration=subprocess.check_output(['git','-C',tests_source,'show',inputs['revision']+':'+inputs['config']],stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        raise SystemExit('BusyBox input identity missing from the fixed repository')
    bin_cc=compiler_identity(args.cross+'gcc')
    if bin_cc['version'].find('15.1.0')<0 or not bin_cc['target'].startswith('loongarch64'):
        raise SystemExit('LA binutils input requires the selected GCC15.1.0 tool installation')
    tools={name:Path(shutil.which(args.cross+name) or '').resolve() for name in
           ('as','ld','ar','ranlib','nm','strip','objcopy','objdump')}
    if any(not p.is_file() for p in tools.values()): raise SystemExit('missing LA binutils')
    root=BASE/'gcc-sf/root'
    options=['--target=loongarch64-unknown-linux-gnusf',f'--prefix={root}',
             '--disable-bootstrap','--disable-multilib','--with-fpu=none','--with-simd=none',
             '--enable-languages=c','--disable-shared','--disable-nls','--disable-lto',
             '--disable-libcc1','--disable-libsanitizer','--disable-libquadmath','--disable-libgomp',
             '--disable-libssp','--disable-libatomic','--without-headers','--disable-werror',
             '--enable-checking=release','--with-system-zlib',f'--with-as={tools["as"]}',f'--with-ld={tools["ld"]}',
             'CFLAGS=-O2 -g0','CXXFLAGS=-O2 -g0']
    identity={'profile':'la64-lp64s-static-musl1.2.5-busybox-v2',
              'gcc_archive':sha(gcc_tar),'musl_archive':sha(musl_tar),'uapi_archive':sha(uapi_tar),
              'busybox_revision':inputs['revision'],'busybox_config':inputs['config'],
              'busybox_config_sha256':hashlib.sha256(configuration).hexdigest(),
              'gcc_options':options,'user_flags':list(FLAGS),'host_cxx_flags':'-std=gnu++14 -fno-char8_t',
              'musl_options':['--target=loongarch64','--disable-shared','--enable-gcc-wrapper'],
              'busybox_flags':['ARCH=loongarch','-static','v6.6-LA-UAPI'],
              'host_cc':compiler_identity('cc'),'host_cxx':compiler_identity('g++'),
              'binutils':{k:{'path':str(v),'sha256':sha(v)} for k,v in tools.items()}}
    return identity,(gcc_tar,musl_tar,uapi_tar,tools,options,tests_source,inputs)

def installed_manifest(base):
    paths=[]
    for name in ('gcc-sf/root','musl-root','uapi'):
        directory=base/name
        if not directory.is_dir(): raise SystemExit('LA userland cache product missing: '+str(directory))
        paths.append(directory);paths.extend(directory.rglob('*'))
    configuration=json.loads((ROOT/'tests/program-inventory/inputs.json').read_text())['busybox']['config']
    paths.extend(base/'busybox-source'/name for name in ('busybox/busybox','busybox/.config',configuration))
    result={}
    for path in sorted(paths):
        key=str(path.relative_to(base));mode=path.lstat().st_mode&0o777
        if path.is_symlink():
            target=path.resolve()
            if not target.exists(): raise SystemExit('LA userland cache symlink target missing: '+str(path))
            result[key]={'kind':'link','target':os.readlink(path),'mode':mode}
            if not target.is_relative_to(base):
                if not target.is_file(): raise SystemExit('LA userland cache external link identity unsupported')
                result[key]['target_sha256']=sha(target)
                result[key]['target_mode']=target.stat().st_mode&0o777
        elif path.is_file(): result[key]={'kind':'file','mode':mode,'sha256':sha(path)}
        elif path.is_dir(): result[key]={'kind':'directory','mode':mode}
        else: raise SystemExit('LA userland cache product missing: '+str(path))
    return result

def main(args):
    BASE.mkdir(parents=True, exist_ok=True)
    identity,context=cache_inputs(args)
    gcc_tar,musl_tar,uapi_tar,tools,options,tests_source,inputs=context
    stamp=BASE/'userland-identity.json'
    if stamp.exists():
        previous=json.loads(stamp.read_text())
        if previous['inputs']!=identity: raise SystemExit('LA userland cache input identity changed')
        if installed_manifest(BASE)==previous['products']:
            print('LA static userland cache verified: '+previous['compiler']['target']); return
        raise SystemExit('LA userland cache product identity changed; use a fresh cache')
    sf=BASE/'gcc-sf'; source=sf/'source/gcc-15.1.0'; build=sf/'build-cxx14'; root=sf/'root'
    if not source.exists(): extract(gcc_tar,source.parent)
    build.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy();env['CXX']='g++ -std=gnu++14 -fno-char8_t'
    if not (build/'Makefile').exists(): run([source/'configure',*options],build,'gcc-sf-build.log',env)
    # 顶层 configure 尚未生成 gcc/Makefile；子配置由真实构建目标负责。
    run(['make','configure-gcc'],build,'gcc-sf-build.log')
    config=(build/'gcc/Makefile').read_text()
    if 'loongarch64-unknown-linux-gnusf' not in config or str(root) not in config:
        raise SystemExit('GCC cache configuration differs from integer target/prefix')
    run(['make',f'-j{args.jobs}','all-gcc'],build,'gcc-sf-build.log')
    run(['make','install-gcc'],build,'gcc-sf-build.log')
    for name,path in tools.items():
        link=root/'bin'/('loongarch64-unknown-linux-gnusf-'+name)
        if not link.exists(): link.symlink_to(path)
        if link.resolve()!=path: raise SystemExit('GCC tool alias differs: '+str(link))
    cc=root/'bin/loongarch64-unknown-linux-gnusf-gcc'
    src=BASE/'musl-src/musl-1.2.5'; out=BASE/'musl-build'; prefix=BASE/'musl-root'
    if not src.exists(): extract(musl_tar,src.parent)
    out.mkdir(parents=True,exist_ok=True)
    musl_env=os.environ.copy();musl_env.update(CC=str(cc),CFLAGS='-O2 '+' '.join(FLAGS),
        AR=str(tools['ar']),RANLIB=str(tools['ranlib']),WRAPCC_GCC=str(cc))
    run([src/'configure','--target=loongarch64',f'--prefix={prefix}','--disable-shared','--enable-gcc-wrapper'],out,'musl-build.log',musl_env)
    run(['make','install-headers'],out,'musl-build.log',musl_env)
    gcc_env=os.environ.copy();gcc_env['PATH']=str(root/'bin')+os.pathsep+gcc_env['PATH']
    runtime=['make',f'-j{args.jobs}',f'AR_FOR_TARGET={tools["ar"]}',f'RANLIB_FOR_TARGET={tools["ranlib"]}',
             f'NM_FOR_TARGET={tools["nm"]}',f'CFLAGS_FOR_TARGET=-O2 -I{prefix}/include']
    run([*runtime,'all-target-libgcc'],build,'gcc-sf-build.log',gcc_env)
    run(['make','install-target-libgcc'],build,'gcc-sf-build.log',gcc_env)
    # bootstrap headers 时 runtime 尚不存在，安装后重跑配置并显式生成 GNU wrapper。
    run([src/'configure','--target=loongarch64',f'--prefix={prefix}','--disable-shared','--enable-gcc-wrapper'],out,'musl-build.log',musl_env)
    run(['make',f'-j{args.jobs}'],out,'musl-build.log',musl_env)
    run(['make','install'],out,'musl-build.log',musl_env)
    # BusyBox uses the fixed v6.6 UAPI because modern Linux removed its CBQ declarations.
    linux=BASE/'uapi-source/linux-6.6';headers=BASE/'uapi'
    if not linux.exists(): extract(uapi_tar,linux.parent)
    run(['make','-C',linux,f'O={BASE}/uapi-build','ARCH=loongarch',f'INSTALL_HDR_PATH={headers}','headers'],ROOT,'uapi-build.log')
    shutil.copytree(BASE/'uapi-build/usr/include',headers/'include',dirs_exist_ok=True)
    revision=inputs['revision']
    destination=BASE/'busybox-source'
    if not destination.exists():
        destination.mkdir()
        bundle=BASE/'busybox-source.tar'
        with bundle.open('wb') as output:
            subprocess.run(['git','-C',tests_source,'archive',revision,'busybox',inputs['config']],stdout=output,check=True)
        extract(bundle,destination)
    bb=destination/'busybox';configuration=destination/inputs['config']
    shutil.copy2(configuration,bb/'.config')
    bb_env=os.environ.copy();bb_env['REALGCC']=str(cc)
    commands=['make','-C',bb,'ARCH=loongarch',f'CC={prefix}/bin/musl-gcc -static '+' '.join(FLAGS)+f' -idirafter {headers}/include',
              f'CROSS_COMPILE={args.cross}','HOSTCC=gcc']
    run([*commands,'oldconfig'],ROOT,'busybox-build.log',bb_env,'\n'*4096)
    def settings(path):
        return [line for line in path.read_text().splitlines() if line.startswith('CONFIG_') or
                (line.startswith('# CONFIG_') and line.endswith(' is not set'))]
    if settings(configuration)!=settings(bb/'.config'): raise SystemExit('upstream BusyBox features changed')
    run([*commands,f'-j{args.jobs}'],ROOT,'busybox-build.log',bb_env)
    result={'inputs':identity,'compiler':compiler_identity(str(cc)),
            'busybox_revision':revision,'uapi_archive':sha(uapi_tar),'gcc_options':options,
            'products':installed_manifest(BASE)}
    stamp.write_text(json.dumps(result,indent=2)+'\n')
    print('LA LP64S static musl and original full BusyBox built and recorded')
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--cross',default='loongarch64-unknown-linux-gnu-')
    parser.add_argument('--jobs',type=int,default=min(os.cpu_count() or 1,8))
    parser.add_argument('--output',type=Path,default=BASE,help='use a fresh cache without overwriting another identity')
    args=parser.parse_args();BASE=args.output.resolve();main(args)
