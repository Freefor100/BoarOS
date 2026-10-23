#!/usr/bin/env python3
"""Run the two file-capability EA error cases through the existing RV boot fixture."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT=Path(__file__).resolve().parent.parent
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'tests/diff-abi'),str(ROOT/'tests/smp')]
import harness
from user import boot_user


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--system',choices=('linux','boaros'),required=True)
    parser.add_argument('--kernel',type=Path)
    parser.add_argument('--program',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    folder=args.output.resolve();folder.mkdir()
    program=args.program.resolve()
    fixture=harness.fixture(folder,program)
    commands=['mkdir /cap-errors','set_inode_field /cap-errors mode 040755']
    for name,words in [('oversized',(0x02000001,1,0,0,0,0,0)),
                       ('short-format',(0x02000001,1))]:
        payload=folder/(name+'.ea');payload.write_bytes(struct.pack('<'+'I'*len(words),*words))
        commands += [f'write "{program}" /cap-errors/{name}',
                     f'set_inode_field /cap-errors/{name} mode 0100755',
                     f'ea_set -f "{payload}" /cap-errors/{name} security.capability']
    batch=folder/'errors.debugfs';batch.write_text('\n'.join(commands)+'\n')
    harness.run_logged(['debugfs','-w','-f',str(batch),str(fixture)],folder/'fixture-errors.log')
    for name in ('oversized','short-format'):
        exported=folder/(name+'.actual')
        harness.run_logged(['debugfs','-R',f'ea_get -f "{exported}" /cap-errors/{name} security.capability',str(fixture)],folder/(name+'.log'))
        if exported.read_bytes()!=(folder/(name+'.ea')).read_bytes():
            raise RuntimeError('EA fixture differs: '+name)
    source=args.kernel or (harness.linux_build(ROOT/'tests/smp/linux-user.config')[0]
                          if args.system=='linux' else ROOT/'kernel-rv')
    kernel=folder/'kernel';shutil.copyfile(source,kernel)
    disk=folder/'root.img';shutil.copyfile(fixture,disk)
    raw,execution,_=boot_user('qemu-system-riscv64','default',kernel,disk,'512M',1,folder,
                             linux=args.system=='linux')
    rows=[line for line in raw.replace('\r','').splitlines() if line.startswith('CRED CAP ERROR ')]
    expected=['CRED CAP ERROR oversized errno=34 expected=34 PASS',
              'CRED CAP ERROR short-format errno=22 expected=22 PASS',
              'CRED CAP ERROR END 2 failures=0']
    record={'system':args.system,'kernel_sha256':harness.digest(kernel),
            'elf_sha256':harness.digest(program),'execution':execution,'rows':rows,
            'passed':rows==expected}
    if args.system=='boaros':
        record['owner_closed']=harness.PROFILES['riscv'].root_success(raw,0 if record['passed'] else 1)
        with (folder/'fsck.log').open('w') as output:
            record['fsck']=subprocess.run(['e2fsck','-fn',str(disk)],stdout=output,stderr=subprocess.STDOUT).returncode
        record['passed'] &= record['owner_closed'] and record['fsck']==0
        if record['owner_closed'] and record['fsck']==0:disk.unlink();fixture.unlink()
    (folder/'result.json').write_text(json.dumps(record,indent=2)+'\n')
    print('\n'.join(rows),flush=True)
    return 0 if record['passed'] else 1


if __name__=='__main__':
    raise SystemExit(main())
