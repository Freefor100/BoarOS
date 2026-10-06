#!/usr/bin/env python3
"""Validate the derived RTC with the unchanged fixed Linux LS7A driver."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from arch_profiles import PROFILES
sys.path.insert(0,str(ROOT/'tests/diff-abi'))
import harness
from reference import archive

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def main():
    profile=PROFILES['loongarch'];work=Path(tempfile.mkdtemp(prefix='rtc-alarm.',dir=ROOT/'build/loongarch'))
    program=work/'probe';supervisor=work/'supervisor'
    environment=__import__('os').environ.copy()
    environment['REALGCC']=str(ROOT/'build/loongarch/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc')
    subprocess.run([str(ROOT/'build/loongarch/musl-root/bin/musl-gcc'),*profile.raw_flags,
        '-static','-O2','-Wall','-Wextra','-Werror','-Wl,-z,max-page-size=16384',
        'tests/loongarch/rtc_alarm.c','-o',str(program)],env=environment,check=True)
    subprocess.run([profile.compiler,*profile.raw_flags,'-O2','-DEXPECTED_EXIT_STATUS=42',
        '-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdlib','-nostartfiles',
        '-static','-no-pie','-Wl,--build-id=none','-Wl,-z,max-page-size=16384',
        '-T','tests/common/user.ld','tests/loongarch/root_linux_init.c','tests/common/user_start.S',
        '-o',str(supervisor)],check=True)
    disk=harness.fixture(work,program)
    initrd=work/'initramfs.gz'
    initrd.write_bytes(archive([('dev',0o040755,b'',0,0),('dev/console',0o020600,b'',5,1),
        ('init',0o100755,supervisor.read_bytes(),0,0),('TRAILER!!!',0,b'',0,0)]))
    kernel=work/'linux-kernel';shutil.copy2(profile.linux_kernel(),kernel)
    identity={'kernel_sha256':sha(kernel),'program_sha256':sha(program),'qemu_sha256':sha(ROOT/profile.qemu),'runs':[]}
    for memory in ('512M','1G'):
        target=work/(memory+'.img');shutil.copy2(disk,target)
        argv=profile.boot(profile.qemu,kernel,memory)+['-net','none','-rtc','base=2026-12-31T23:59:58,clock=vm',
            '-object','rng-random,id=entropy,filename=/dev/urandom','-device',profile.rng(),
            '-drive',f'file={target},if=none,format=raw,id=root','-device',profile.block('modern'),
            '-initrd',str(initrd),'-append','console=ttyS0 rdinit=/init loglevel=3']
        with (work/(memory+'.log')).open('wb') as log:
            result=subprocess.run(argv,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,timeout=60)
        output=(work/(memory+'.log')).read_text(errors='replace')
        passed=result.returncode==0 and 'LS7A Linux actual alarm IRQ, rearm, UTC rollover, errors and mixed device progress PASS' in output and 'Linux LA root application passed' in output
        identity['runs'].append({'memory':memory,'argv':argv,'qemu_status':result.returncode,'passed':passed})
        (work/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
        if not passed:print(output);raise SystemExit('LS7A Linux native alarm failed: '+str(work))
    print('LS7A unchanged Linux driver native alarms, actual exit42 and clean unmount PASS:',work)
if __name__=='__main__':main()
