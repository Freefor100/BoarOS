#!/usr/bin/env python3
"""Real indexed/linear directory mutations with and without metadata checksums."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sanitize',action='store_true')
args=parser.parse_args()
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='boaros-dir-empty-') as name:
    work=Path(name);program=work/'probe'
    command=['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
        '-Wno-unused-but-set-variable','-Wno-stringop-truncation','-DCONFIG_USE_DEFAULT_CFG=1',
        '-DCONFIG_USE_USER_MALLOC=1','-include',str(ROOT/'tests/host/lwext4_memory.h'),
        '-I'+str(ROOT/'third_party/lwext4/include'),'-idirafter',str(ROOT/'include'),
        *map(str,sorted((ROOT/'third_party/lwext4/src').glob('*.c'))),
        str(ROOT/'tests/host/lwext4_rename.c'),str(ROOT/'tests/host/block_fault.c'),
        str(ROOT/'kernel/block.c'),'-o',str(program)]
    if args.sanitize:command += ["-fsanitize=address,undefined","-fno-omit-frame-pointer"]
    subprocess.run(command,check=True,timeout=60)
    for size in (1024,4096):
        for indexed in ('dir_index','^dir_index'):
            for checksum in ('metadata_csum','^metadata_csum'):
                image=work/'root.img'
                with image.open('wb') as stream:stream.truncate(32*1024*1024)
                subprocess.run(['mkfs.ext4','-q','-F','-b',str(size),'-O',f'{indexed},{checksum},^orphan_file',str(image)],check=True)
                subprocess.run([str(program),str(image),'seed'],check=True,timeout=30)
                for operation in ('basic','wide'):
                    changed=work/'changed.img';shutil.copyfile(image,changed)
                    subprocess.run([str(program),str(changed),operation],check=True,timeout=30)
                    subprocess.run(['e2fsck','-fn',str(changed)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=30)
                print('PASS empty-directory rename',size,indexed,checksum,flush=True)
