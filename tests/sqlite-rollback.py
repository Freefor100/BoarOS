#!/usr/bin/env python3
"""Original SQLite DELETE/CLI content and independent reboots on both kernels."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from arch_profiles import PROFILES
sys.path.insert(0,str(ROOT/'tests/diff-abi'))
import harness
sys.path.insert(0,str(ROOT/'tests/loongarch'))
from reference import archive
spec=importlib.util.spec_from_file_location('sqlite_wal_runner',ROOT/'tests/sqlite-wal-riscv.py')
wal=importlib.util.module_from_spec(spec);spec.loader.exec_module(wal)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def debugfs(image,request):
    result=subprocess.run(['debugfs','-w','-R',request,str(image)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,check=True)
    output=result.stdout.decode(errors='replace')
    if any(error in output for error in ('File not found','Command not found','Could not allocate','No such file')):raise RuntimeError(output)
def install(image,name,source):
    debugfs(image,f'write {source} {name}');debugfs(image,f'set_inode_field {name} mode 0100755')
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch',choices=tuple(PROFILES),default='riscv')
    parser.add_argument('--memory',choices=('512M','1G'),action='append')
    parser.add_argument('--kernel',type=Path);parser.add_argument('--qemu')
    parser.add_argument('--cc',help='raw supervisor compiler selected by Make')
    args=parser.parse_args();profile=PROFILES[args.arch];qemu=args.qemu or profile.qemu
    compiler=args.cc or (shutil.which('riscv64-unknown-elf-gcc') if args.arch=='riscv' else None) or profile.compiler
    compiler_path=Path(shutil.which(compiler) or compiler).resolve()
    compiler_identity={'path':str(compiler_path),'sha256':sha(compiler_path),
        'version':subprocess.check_output([compiler,'--version'],text=True).splitlines()[0]}
    work=Path(tempfile.mkdtemp(prefix='sqlite-run.',dir=ROOT/'build'/args.arch))
    if args.arch=='loongarch':
        paths={name:ROOT/'build/loongarch'/name for name in ('sqlite-rollback','sqlite3-static','sqlite3-dynamic','sqlite-cli-init')}
        loader=ROOT/'build/loongarch/dynamic-dp-v2/root/lib/libc.so';loader_name='ld-musl-loongarch64.so.1'
        import prepare_dynamic,prepare_userland
        inputs,_=prepare_dynamic.cache_inputs(argparse.Namespace(cross='loongarch64-unknown-linux-gnu-',jobs=1,output=prepare_dynamic.BASE))
        saved=json.loads((prepare_dynamic.BASE/'identity.json').read_text())
        if saved['inputs']!=inputs or prepare_userland.installed_tree_manifest(prepare_dynamic.BASE,['root'])!=saved['products']:raise RuntimeError('SQLite LA dynamic runtime integrity failed')
    else:
        names={'sqlite-rollback':'SQLITE_ROLLBACK_RV','sqlite3-static':'SQLITE_CLI_STATIC_RV',
            'sqlite3-dynamic':'SQLITE_CLI_DYNAMIC_RV','sqlite-cli-init':'SQLITE_CLI_INIT_RV'}
        paths={name:Path(os.environ.get(variable,ROOT/'build/riscv/tests/user'/(name+'-rv'))) for name,variable in names.items()}
        loader=Path(os.environ.get('MUSL_LDSO',ROOT/'build/riscv/musl-root/lib/ld-musl-riscv64.so.1'));loader_name='ld-musl-riscv64.so.1'
    inputs=ROOT/'references/sqlite/sqlite-amalgamation-3530400.zip'
    pin=next(row.split('\t')[4] for row in (ROOT/'references/sources.tsv').read_text().splitlines() if row.startswith('file\tsqlite/sqlite-amalgamation-3530400.zip\t'))
    if sha(inputs)!=pin:raise RuntimeError('SQLite original archive identity mismatch')
    identity={'arch':args.arch,'archive':pin,'elfs':{name:sha(path) for name,path in paths.items()},'loader':sha(loader),'qemu':sha(shutil.which(qemu) or qemu),'supervisor_compiler':compiler_identity,'runs':[]}
    disk=harness.fixture(work,paths['sqlite-rollback'])
    for name in ('sqlite3-static','sqlite3-dynamic'):install(disk,'/'+name,paths[name])
    debugfs(disk,'mkdir /lib');install(disk,'/lib/'+loader_name,loader)
    debugfs(disk,'symlink /lib/libc.so /lib/'+loader_name)
    supervisor=work/'supervisor';initrd=work/'initramfs.gz'
    subprocess.run([compiler,*profile.raw_flags,'-O2','-DEXPECTED_EXIT_STATUS=42',
        *(['-DROOT_DIRECT_FILESYSTEM'] if args.arch=='riscv' else []),
        '-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdlib','-nostartfiles',
        '-static','-no-pie','-Wl,--build-id=none',f'-Wl,-z,max-page-size={profile.page_size}',
        '-T','tests/common/user.ld','tests/loongarch/root_linux_init.c','tests/common/user_start.S','-o',str(supervisor)],check=True)
    initrd.write_bytes(archive([('dev',0o040755,b'',0,0),('dev/console',0o020600,b'',5,1),('init',0o100755,supervisor.read_bytes(),0,0),('TRAILER!!!',0,b'',0,0)]))
    if args.arch=='riscv':install(disk,'/supervisor',supervisor)
    kernels={}
    for name,source in (('linux',profile.linux_kernel()),('boaros',args.kernel or ROOT/profile.kernel)):
        kernels[name]=work/(name+'-kernel');shutil.copy2(source,kernels[name])
    identity['kernels']={name:sha(path) for name,path in kernels.items()};identity['supervisor']=sha(supervisor)
    for name,kernel in kernels.items():
        for memory in args.memory or (['512M','1G'] if args.arch=='loongarch' else ['512M']):
            image=work/(name+'-'+memory+'.img');shutil.copy2(disk,image)
            for phase in ('rollback','cli','cli-reboot'):
                if phase=='cli':debugfs(image,'rm /init');install(image,'/init',paths['sqlite-cli-init'])
                marker='BoarOS: SQLite rollback smoke passed' if phase=='rollback' else 'BoarOS: SQLite CLI static and dynamic passed'
                log=work/(name+'-'+memory+'-'+phase+'.log')
                wal.boot(qemu,kernel,image,log,name=='linux',marker,arch=args.arch,memory=memory,
                    initrd=initrd if args.arch=='loongarch' else None,direct_init='/supervisor' if args.arch=='riscv' else '/init')
                if name=='linux' and 'Linux '+('LA' if args.arch=='loongarch' else 'RV')+' root application passed' not in log.read_text(errors='replace'):raise RuntimeError('SQLite Linux actual child exit/unmount failed')
                identity['runs'].append({'platform':name,'memory':memory,'phase':phase,'exit':42,'passed':True})
                (work/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
                print(name,memory,phase,'real SQLite content, child status and release PASS',flush=True)
    print('Original SQLite DELETE, static/dynamic CLI, committed/rollback content and independent reboot PASS:',work)
if __name__=='__main__':main()
