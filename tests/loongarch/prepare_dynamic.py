#!/usr/bin/env python3
"""Build isolated original LP64D shared musl; preserve the integer cache."""
import argparse
import json
import os
from pathlib import Path
from prepare import compiler_identity
import prepare_userland as static

BASE=static.BASE/'dynamic-dp-v2'
FLAGS=['-mabi=lp64d','-mdouble-float','-mno-lsx','-mno-lasx']
def cache_inputs(args):
    musl=static.archive('musl/musl-1.2.5.tar.gz')
    base=args.output.resolve()
    prefix=base/'root';source=base/'source/musl-1.2.5';build=base/'build'
    cc=Path(compiler_identity(args.cross+'gcc')['path'])
    _,context=static.cache_inputs(args);tools=context[3]
    options=['--target=loongarch64',f'--prefix={prefix}',f'--syslibdir={prefix}/lib',
             '--enable-shared','--enable-gcc-wrapper']
    import subprocess
    runtime={}
    for name in ('crtbegin.o','crtend.o','crtbeginS.o','crtendS.o','libgcc.a','libgcc_eh.a'):
        file=Path(subprocess.check_output([str(cc),'-mabi=lp64d','-print-file-name='+name],text=True).strip()).resolve()
        if not file.is_file():raise SystemExit('LP64D runtime missing: '+name)
        runtime[name]={'path':str(file),'sha256':static.sha(file)}
    frontend=Path(subprocess.check_output([str(cc),'-print-prog-name=cc1'],text=True).strip()).resolve()
    includes=Path(subprocess.check_output([str(cc),'-print-file-name=include'],text=True).strip()).resolve()
    if not frontend.is_file() or not includes.is_dir():raise SystemExit('LP64D compiler frontend/headers missing')
    identity={'profile':'la64-lp64d-shared-musl1.2.5-v2','musl_archive':static.sha(musl),
        'static_toolchain_identity':static.sha(static.BASE/'userland-identity.json'),
        'compiler':compiler_identity(str(cc)),'frontend':{'path':str(frontend),'sha256':static.sha(frontend)},
        'compiler_headers':static.installed_tree_manifest(includes.parent,[includes.name]),'runtime':runtime,'user_flags':list(FLAGS),
        'linker_flags':['-Wl,-z,max-page-size=16384'],'configure':options}
    return identity,(base,prefix,source,build,cc,tools,musl,options)

def main(args):
    static.main(argparse.Namespace(cross=args.cross,jobs=args.jobs))
    identity,context=cache_inputs(args)
    base,prefix,source,build,cc,tools,musl,options=context
    base.mkdir(parents=True,exist_ok=True)
    stamp=base/'identity.json'
    if stamp.exists():
        previous=json.loads(stamp.read_text())
        if previous['inputs']!=identity:raise SystemExit('LA dynamic cache input identity changed')
        if static.installed_tree_manifest(base,['root'])!=previous['products']:
            raise SystemExit('LA dynamic cache product identity changed')
        print('LA shared musl cache verified');return
    # A fresh profile always reads the pinned archive, never a stale source tree.
    if source.exists():
        raise SystemExit('unfinished LA dynamic cache; use a fresh --output')
    static.extract(musl,source.parent);build.mkdir()
    env=os.environ.copy();env.update(CC=str(cc),CFLAGS='-O2 '+' '.join(FLAGS),
        LDFLAGS='-Wl,-z,max-page-size=16384',AR=str(tools['ar']),RANLIB=str(tools['ranlib']),WRAPCC_GCC=str(cc))
    static.run([source/'configure',*options],build,'build.log',env)
    static.run(['make',f'-j{args.jobs}'],build,'build.log',env)
    static.run(['make','install'],build,'build.log',env)
    if not (prefix/'lib/libc.so').is_file():raise SystemExit('shared musl loader missing')
    stamp.write_text(json.dumps({'inputs':identity,'products':static.installed_tree_manifest(base,['root'])},indent=2)+'\n')
    print('LA shared musl built and complete installation recorded')
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--cross',default='loongarch64-unknown-linux-gnu-')
    parser.add_argument('--jobs',type=int,default=min(os.cpu_count() or 1,8));parser.add_argument('--output',type=Path,default=BASE)
    main(parser.parse_args())
