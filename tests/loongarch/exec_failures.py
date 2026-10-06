#!/usr/bin/env python3
"""Native interpreter errno/old-image and initial dynamic construction rollback."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from root import ROOT,disk,execute
from userland import add_file
from guest import run_guest

def main(args):
    directory=Path(tempfile.mkdtemp(prefix='exec-fail-run.',dir=ROOT/'build/loongarch'))
    compiler=ROOT/'build/loongarch/dynamic-dp-v2/root/bin/musl-gcc'
    environment=os.environ.copy();environment['REALGCC']=args.cc
    for name in ('missing','noexec','wrong','broken','short'):
        subprocess.run([str(compiler),'-mabi=lp64d','-mno-lsx','-mno-lasx','-O2','-fPIE','-pie',
            '-Wl,-z,max-page-size=16384',f'-Wl,--dynamic-linker=/lib/{name}.so.1',
            'tests/loongarch/exec_target.c','-o',str(directory/f'bad-{name}')],env=environment,check=True)
    loader=ROOT/'build/loongarch/dynamic-dp-v2/root/lib/libc.so'
    wrong=directory/'wrong.so.1';content=bytearray(loader.read_bytes());content[18:20]=(243).to_bytes(2,'little');wrong.write_bytes(content)
    broken=directory/'broken.so.1';broken.write_bytes(b'x'*64)
    short=directory/'short.so.1';short.write_bytes(b'broken interpreter')
    # Runtime failures compare the same actual static process with both kernels.
    files=[]
    for name in ('missing','noexec','wrong','broken','short'):files+=['--file',f'/bad-{name}={directory}/bad-{name}']
    files+=['--file',f'/lib/wrong.so.1={wrong}','--file',f'/lib/broken.so.1={broken}','--file',f'/lib/short.so.1={short}']
    # Program creates the non-executable interpreter itself from /loader-source.
    files+=['--file',f'/loader-source={loader}']
    subprocess.run(['python3','-B','tests/loongarch/userland.py','--qemu',args.qemu,'--cc',args.cc,
        '--program','build/loongarch/exec-errors-probe','--marker','LA interpreter errno/old image/TLS/threads/fd/signal preserved',*files],check=True)
    errors={'missing':-2,'noexec':-13,'wrong':-80,'broken':-80,'short':-5,'oom':-12}
    for memory in ('512M','1G'):
        for case,error in errors.items():
            area=directory/(case+'-'+memory);area.mkdir()
            image=disk(area,ROOT/'build/loongarch/dynamic-probe')
            path='/lib/ld-musl-loongarch64.so.1'
            if case!='missing':add_file(image,path,wrong if case=='wrong' else broken if case=='broken' else short if case=='short' else loader,0o100644 if case=='noexec' else 0o100755)
            kernel='build/loongarch/kernel-root-oom-1' if case=='oom' else 'kernel-la'
            command=[args.qemu,'-machine','virt','-cpu','la464','-global','ls7a_rtc.toy-enabled=on','-smp','1','-m',memory,'-kernel',kernel,
                '-drive',f'file={image},format=raw,if=none,id=root','-device','virtio-blk-pci,drive=root,addr=1,disable-legacy=on','-net','none','-nographic','-no-reboot']
            code,text=run_guest(command,120);(area/'log').write_text(text)
            expected=f'LA root boot errno=0x{error&((1<<64)-1):016x}'
            if code or expected not in text or 'LA root owners released' not in text or 'LA PID 1' in text or 'fatal' in text:
                print(text);raise SystemExit('dynamic construction failure '+case+'/'+memory)
            print(f'BoarOS/{memory} interpreter {case}: errno, unpublished PID1, source/MM/heap/root baseline PASS')
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--cc',default='loongarch64-unknown-linux-gnu-gcc');parser.add_argument('--qemu',default='build/qemu-la-rtc/qemu-system-loongarch64');main(parser.parse_args())
