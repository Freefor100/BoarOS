#!/usr/bin/env python3
"""Run inode xattr semantics with the existing native boot fixtures."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'tests/diff-abi'),str(ROOT/'tests/smp')]
import harness
from user import boot_user
from arch_profiles import PROFILES

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch',choices=PROFILES,default='riscv')
    parser.add_argument('--system',choices=('linux','boaros'),required=True)
    parser.add_argument('--kernel',type=Path)
    parser.add_argument('--program',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--tools',type=Path)
    parser.add_argument('--loader',type=Path)
    parser.add_argument('--cpus',type=int,choices=(1,2,4),default=1)
    parser.add_argument('--memory',default='512M')
    args=parser.parse_args();profile=PROFILES[args.arch]
    folder=args.output.resolve();folder.mkdir()
    disk=harness.fixture(folder,args.program.resolve())
    if args.loader:
        name='ld-musl-'+('riscv64' if args.arch=='riscv' else 'loongarch64')+'.so.1'
        batch=folder/'loader.debugfs';batch.write_text(f'mkdir /lib\nwrite "{args.loader.resolve()}" /lib/{name}\nset_inode_field /lib/{name} mode 0100755\n')
        harness.run_logged(['debugfs','-w','-f',str(batch),str(disk)],folder/'loader.log')
    if args.tools:
        batch=folder/'tools.debugfs'
        batch.write_text('mkdir /xattr-tools\n'+'\n'.join(f'write "{(args.tools/name).resolve()}" /xattr-tools/{name}\nset_inode_field /xattr-tools/{name} mode 0100755' for name in ('setfattr','getfattr','setcap','getcap'))+'\n')
        harness.run_logged(['debugfs','-w','-f',str(batch),str(disk)],folder/'tools.log')
    sample=folder/'attribute-value';sample.write_text('initial')
    batch=folder/'flags.debugfs'
    batch.write_text('\n'.join(f'write "{sample}" /xattr-{name}\nea_set /xattr-{name} user.fixture original\nset_inode_field /xattr-{name} flags {flags}' for name,flags in [('immutable','0x80010'),('append','0x80020')])+'\n')
    harness.run_logged(['debugfs','-w','-f',str(batch),str(disk)],folder/'flags.log')
    source=args.kernel or (harness.linux_build(ROOT/'tests/smp/linux-user.config')[0] if args.arch=='riscv' and args.system=='linux' else profile.linux_kernel() if args.system=='linux' else ROOT/profile.kernel)
    kernel=folder/'kernel';shutil.copyfile(source,kernel)
    if args.arch=='riscv':
        raw,execution,_=boot_user(profile.qemu,'default',kernel,disk,args.memory,args.cpus,folder,linux=args.system=='linux')
    else:
        command=profile.boot(profile.qemu,kernel,args.memory)+['-net','none','-drive',f'file={disk},if=none,format=raw,id=root','-device',profile.block('pci'),'-object','rng-random,id=entropy,filename=/dev/urandom','-device',profile.rng()]
        if args.system=='linux':command+=['-append','root=/dev/vda rw rootwait console=ttyS0 init=/init loglevel=0 panic=-1']
        with (folder/'boot.log').open('w') as stream:
            try: result=subprocess.run(command,stdout=stream,stderr=subprocess.STDOUT,timeout=90)
            except subprocess.TimeoutExpired: result=None
        raw=(folder/'boot.log').read_text();execution={'returncode':None if result is None else result.returncode}
    rows=[line for line in raw.replace('\r','').splitlines() if line.startswith('XATTR ')]
    names=['abi-flags-empty-length-follow-unlinked-fd','dac-current-identity-trusted-list-filter','filecap-revision-rootid-empty-killpriv-cache','replace-read-unlinked-inode-stable-value','fault-prefix-input-rollback','user-written-filecap-exec-auxv']
    if args.tools:names+=['original-attr-libcap-tools']
    expected=[*(f'XATTR CASE {name} PASS' for name in names*2),f'XATTR END {len(names)*2}']
    record={'system':args.system,'arch':args.arch,'cpus':args.cpus,'kernel_sha256':harness.digest(kernel),'elf_sha256':harness.digest(args.program),'execution':execution,'passed':rows==expected,'rows':rows}
    if args.system=='boaros':record['passed'] &= profile.root_success(raw,0)
    if record['passed']:
        with (folder/'fsck.log').open('w') as stream:record['fsck']=subprocess.run(['e2fsck','-fn',str(disk)],stdout=stream,stderr=subprocess.STDOUT).returncode
        record['passed'] &= record['fsck']==0
        if record['passed']:disk.unlink()
    (folder/'result.json').write_text(json.dumps(record,indent=2)+'\n');print('\n'.join(rows),flush=True)
    return 0 if record['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
