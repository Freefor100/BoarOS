#!/usr/bin/env python3
"""Build the original attr/libcap tools offline for a private validation disk."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
from oscomp.local_disks import verify_target
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--arch',choices=('riscv','loongarch'),required=True)
    p.add_argument('--archives',type=Path,required=True)
    p.add_argument('--compiler',type=Path,required=True)
    p.add_argument('--uapi',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    pins={row.split('\t')[1].rsplit('/',1)[-1]:row.split('\t')[-1] for row in (ROOT/'references/sources.tsv').read_text().splitlines() if row.startswith('file\txattr-tools/')}
    names=('attr-2.5.2.tar.xz','libcap-2.76.tar.xz')
    for name in names:
        archive=a.archives.resolve()/name
        if hashlib.sha256(archive.read_bytes()).hexdigest()!=pins[name]:raise RuntimeError('source checksum: '+name)
        target=out/name.removesuffix('.tar.xz')
        if not target.exists():
            with tarfile.open(archive) as t:t.extractall(out,filter='data')
    cc=str(a.compiler.resolve())
    probe=subprocess.run([cc,'-fno-link-libatomic','-E','-x','c','/dev/null'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    if not probe.returncode:cc+=' -fno-link-libatomic'
    flags='-O2'+(' -Wl,-z,max-page-size=16384' if a.arch=='loongarch' else '')
    commands=[]
    def run(command,cwd):
        commands.append({'cwd':str(cwd),'argv':command})
        with (out/'build.log').open('a') as log:subprocess.run(command,cwd=cwd,stdout=log,stderr=subprocess.STDOUT,check=True)
    attr=out/'attr-build';attr.mkdir(exist_ok=True)
    if not (attr/'Makefile').exists():run([str(out/'attr-2.5.2/configure'),'--host='+('riscv64' if a.arch=='riscv' else 'loongarch64')+'-linux-musl','--disable-nls','--disable-shared','--enable-static','CC='+cc,'LDFLAGS=-static'],attr)
    # attr 2.5.2 uses basename(); musl declares it in libgen.h.
    for name in ('getfattr','setfattr'):(attr/name).unlink(missing_ok=True)
    run(['make','-j4','getfattr','setfattr','LDFLAGS=-all-static','CFLAGS='+flags+' -include libgen.h'],attr)
    cap=out/'libcap-2.76';common=['CC='+cc+' -idirafter '+str(a.uapi.resolve()),'BUILD_CC=cc','DYNAMIC=no','PAM_CAP=no','LIBCSTATIC=yes','CFLAGS='+flags]
    run(['make','-j4','libcap.a',*common],cap/'libcap')
    run(['make','-j2','getcap','setcap',*common],cap/'progs')
    tools=out/'tools';tools.mkdir(exist_ok=True)
    for name in ('getfattr','setfattr','getcap','setcap'):
        source=(attr if name.endswith('fattr') else cap/'progs')/name
        shutil.copyfile(source,tools/name);(tools/name).chmod(0o755)
        verify_target(tools/name,a.arch,float_abis=(1,3) if a.arch=='loongarch' else (4,))
    (out/'identity.json').write_text(json.dumps({'arch':a.arch,'sources':pins,'commands':commands,'tools':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in tools.iterdir()}},indent=2)+'\n')
if __name__=='__main__':main()
