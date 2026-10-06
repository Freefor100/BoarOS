#!/usr/bin/env python3
"""Build the approved LS7A RTC derivation without changing fixed references."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
from prepare import ROOT, QEMU_OPTIONS, compiler_identity, fixed_repository

DIRECTORY=ROOT/'build/qemu-la-rtc'
PATCH=ROOT/'tests/loongarch/qemu-ls7a-rtc.patch'

def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def product(path):
    path=Path(path)
    return {'sha256':digest(path),'mode':stat.S_IMODE(path.stat().st_mode),
            'link':os.readlink(path) if path.is_symlink() else None}
def archive(source,destination,revision):
    destination.mkdir(parents=True,exist_ok=False)
    process=subprocess.Popen(['git','-C',str(source),'archive',revision],stdout=subprocess.PIPE)
    try:subprocess.run(['tar','-x','-C',str(destination)],stdin=process.stdout,check=True)
    finally:process.stdout.close()
    if process.wait():raise RuntimeError('fixed Git archive failed')
def source_files(source,destination):
    names=subprocess.check_output(['git','-C',str(source),'ls-files','-z']).decode().split('\0')
    return {name:product(destination/name) for name in names if name and (destination/name).is_file()}
def configuration_files(directory):
    return {name:product(directory/name) for name in ('build.ninja','config-host.mak',
        'config-host.h','loongarch64-softmmu-config-target.h','config.status',
        'meson-private/coredata.dat') if (directory/name).is_file()}
def prepare(jobs,rebuild=False):
    source,revision=fixed_repository('qemu')
    identity={'revision':revision,'patch':digest(PATCH),'compiler':compiler_identity('cc'),
              'configuration':QEMU_OPTIONS,'profile':'ls7a-rtc-pm-direct-irq-v1'}
    DIRECTORY.mkdir(exist_ok=True)
    stamp=DIRECTORY/'boaros-identity.json'
    recorded=json.loads(stamp.read_text()) if stamp.exists() else None
    derived=DIRECTORY/'source'
    if rebuild:
        if not recorded or recorded.get('revision')!=revision:
            raise RuntimeError('refusing to replace an unidentified derived source')
        shutil.rmtree(derived);recorded=None;stamp.unlink()
    if recorded:
        if any(recorded.get(key)!=value for key,value in identity.items()):
            raise RuntimeError('derived QEMU inputs changed; use a fresh cache directory')
        if source_files(source,derived)!=recorded['source_files']:
            raise RuntimeError('derived QEMU source content, mode or links changed')
        for name,expected in recorded['dependencies'].items():
            if source_files(source/'subprojects'/name,derived/'subprojects'/name)!=expected['files']:
                raise RuntimeError('derived QEMU subproject changed: '+name)
            for relative,entry in expected['overlay'].items():
                if product(derived/'subprojects'/name/relative)!=entry:
                    raise RuntimeError('derived QEMU Meson overlay changed: '+name)
        binary=DIRECTORY/'qemu-system-loongarch64'
        if 'binary' in recorded and product(binary)!=recorded['binary']:
            raise RuntimeError('derived QEMU binary content, mode or links changed')
        if 'binary' in recorded and configuration_files(DIRECTORY/'build')!=recorded.get('configuration_files'):
            raise RuntimeError('derived QEMU build configuration changed; explicitly rebuild approved inputs')
        identity.update({key:recorded[key] for key in ('source_files','dependencies')})
    else:
        if derived.exists():raise RuntimeError('unidentified derived source exists; use a fresh directory')
        archive(source,derived,revision)
        subprocess.run(['git','apply',str(PATCH)],cwd=derived,check=True)
        identity['source_files']=source_files(source,derived)
        identity['dependencies']={}
        # Reuse only exact wrap commits, exporting tracked source instead of Meson overlays.
        for name in ('berkeley-softfloat-3','berkeley-testfloat-3','keycodemapdb'):
            original=source/'subprojects'/name
            revision=next(line.split('=',1)[1].strip() for line in
                (source/'subprojects'/(name+'.wrap')).read_text().splitlines() if line.startswith('revision'))
            actual=subprocess.check_output(['git','-C',str(original),'rev-parse','HEAD'],text=True).strip()
            if actual!=revision:raise RuntimeError('subproject wrap identity mismatch: '+name)
            subprocess.run(['git','-C',str(original),'diff','--exit-code','HEAD','--'],check=True,stdout=subprocess.DEVNULL)
            archive(original,derived/'subprojects'/name,revision)
            overlay=derived/'subprojects/packagefiles'/name
            products={}
            if overlay.is_dir():
                for item in overlay.rglob('*'):
                    if not item.is_file():continue
                    relative=item.relative_to(overlay);target=derived/'subprojects'/name/relative
                    target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(item,target)
                    products[str(relative)]=product(target)
            identity['dependencies'][name]={'revision':revision,'files':source_files(original,derived/'subprojects'/name),'overlay':products}
        stamp.write_text(json.dumps(identity,sort_keys=True,indent=2)+'\n')
    output=DIRECTORY/'build';output.mkdir(exist_ok=True)
    if rebuild or not (output/'build.ninja').exists():
        subprocess.run([str(derived/'configure'),*QEMU_OPTIONS],cwd=output,check=True)
    subprocess.run(['ninja','-C',str(output),f'-j{jobs}','qemu-system-loongarch64'],check=True)
    binary=DIRECTORY/'qemu-system-loongarch64'
    if not binary.exists():binary.symlink_to('build/qemu-system-loongarch64')
    identity['binary']=product(DIRECTORY/'qemu-system-loongarch64')
    identity['configuration_files']=configuration_files(output)
    stamp.write_text(json.dumps(identity,sort_keys=True,indent=2)+'\n')
    print('Derived LS7A QEMU:',DIRECTORY,'base',identity['revision'],'patch',identity['patch'],
          'binary',identity['binary']['sha256'],flush=True)
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs',type=int,default=min(os.cpu_count() or 1,12))
    parser.add_argument('--rebuild',action='store_true',help='replace identified derived source and reconfigure after approved patch changes')
    args=parser.parse_args();prepare(args.jobs,args.rebuild)
