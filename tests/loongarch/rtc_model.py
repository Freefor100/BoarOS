#!/usr/bin/env python3
"""Exercise actual QEMU LS7A registers, timer IRQs and PM event ownership."""
import argparse
import datetime
import json
from pathlib import Path
import socket
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
RTC=0x100d0100
PM=RTC-0x100
EVENT=1<<10
BASE=datetime.datetime(2026,12,31,23,59,58,tzinfo=datetime.timezone.utc)

class Machine:
    def __init__(self,qemu,directory,incoming=False):
        self.irqs={};self.directory=Path(directory)
        self.log=(self.directory/'qemu.log').open('wb')
        argv=[str(qemu),'-machine','virt','-accel','qtest',
            '-m','512M','-smp','1','-display','none','-net','none',
            '-rtc','base=2026-12-31T23:59:58,clock=vm',
            '-qtest',f'unix:{self.directory}/qtest,server=on,wait=off',
            '-qmp',f'unix:{self.directory}/qmp,server=on,wait=off']
        if incoming:argv+=['-incoming','defer']
        self.process=subprocess.Popen(argv,
            stdout=self.log,stderr=subprocess.STDOUT)
        self.test=self.connect('qtest');self.qmp=self.connect('qmp')
        self.test_file=self.test.makefile('rwb',buffering=0)
        self.qmp_file=self.qmp.makefile('rwb',buffering=0)
        json.loads(self.qmp_file.readline());self.command('qmp_capabilities')
        objects=self.command('qom-list',{'path':'/machine/unattached'})
        rtc=next(item['name'] for item in objects if item['type']=='child<ls7a_rtc>')
        self.ask('irq_intercept_out /machine/unattached/'+rtc+' sysbus-irq')
    def connect(self,name):
        import time
        sock=socket.socket(socket.AF_UNIX);sock.settimeout(10)
        for _ in range(1000):
            try:sock.connect(str(self.directory/name));return sock
            except FileNotFoundError:
                if self.process.poll() is not None:raise RuntimeError((self.directory/'qemu.log').read_text())
                time.sleep(.01)
        raise RuntimeError('QEMU control socket did not appear')
    def command(self,name,arguments=None):
        self.qmp_file.write((json.dumps({'execute':name,'arguments':arguments or {}})+'\n').encode())
        while True:
            value=json.loads(self.qmp_file.readline())
            if 'error' in value:raise RuntimeError(value)
            if 'return' in value:return value['return']
    def ask(self,request):
        self.test_file.write((request+'\n').encode())
        while True:
            reply=self.test_file.readline().decode().strip()
            if reply.startswith('IRQ '):
                _,level,number=reply.split();self.irqs[int(number)]=level=='raise';continue
            if not reply.startswith('OK'):raise RuntimeError(request+': '+reply)
            return reply.split()[1:]
    def read(self,address):return int(self.ask(f'readl {address:#x}')[0],0)
    def write(self,address,value):self.ask(f'writel {address:#x} {value:#x}')
    def step(self,ns):self.ask(f'clock_step {ns}')
    def irq(self,number):
        self.read(RTC+0x40)
        return self.irqs.get(number,False)
    def close(self):
        if self.process.poll() is None:self.process.terminate()
        self.process.wait(timeout=10)
        for item in ('test_file','qmp_file','test','qmp'):
            value=getattr(self,item,None)
            if value:value.close()
        self.log.close()

def match(date):
    return (((date.year-1900)&63)<<26)|(date.month<<22)|(date.day<<17)|(date.hour<<12)|(date.minute<<6)|date.second

def verify(machine):
    m=machine
    assert m.read(PM+0xc)==0
    m.write(PM+0x10,EVENT)
    assert m.read(PM+0x10)==EVENT,'PM1_EN must hold real writable RTC_EN'
    m.write(RTC+0x40,0x900)
    m.step(1)
    assert not m.irq(0),'reset match=0 is an invalid date, not an expired alarm'
    assert m.read(RTC+0x30)==126
    assert m.read(RTC+0x2c)==(12<<26)|(31<<21)|(23<<16)|(59<<10)|(58<<4)
    m.write(RTC+0x34,match(BASE+datetime.timedelta(seconds=2)))
    m.step(1_000_000_000);assert not m.irq(0)
    m.step(1_000_000_000);assert m.irq(0)
    assert m.read(PM+0xc)&EVENT,'alarm must latch PM RTC_STS'
    assert not m.irq(1),'PM global interrupt switch gates SCI'
    m.write(PM+0x14,1);assert m.irq(1)
    m.write(PM+0x10,0);assert not m.irq(1)
    assert m.read(PM+0xc)&EVENT,'masking preserves pending PM event'
    m.write(PM+0x10,EVENT);assert m.irq(1)
    m.write(PM+0xc,0);assert m.irq(1),'zero does not clear W1C status'
    m.write(PM+0xc,EVENT);assert not m.irq(1)
    assert m.irq(0),'PM acknowledgement must not consume direct timer IRQ'
    m.write(RTC+0x34,0);assert not m.irq(0),'Linux ISR match=0 must deassert direct IRQ'
    assert m.read(RTC+0x30)==127 and m.read(RTC+0x2c)==(1<<26)|(1<<21),'UTC year rollover'
    now=BASE+datetime.timedelta(seconds=2)
    for offset in (0x38,0x3c):m.write(RTC+offset,match(now+datetime.timedelta(seconds=1)))
    m.step(1_000_000_000);assert m.irq(0)
    m.write(RTC+0x38,0);assert m.irq(0),'other comparator owns asserted IRQ'
    m.write(RTC+0x3c,0);assert not m.irq(0)
    m.write(RTC+0x34,match(BASE));m.step(1_000_000_000)
    assert not m.irq(0),'past 6-bit year match must await next epoch, not fire immediately'
    m.write(RTC+0x34,match(now+datetime.timedelta(seconds=4)))
    m.write(RTC+0x40,0);m.step(4_000_000_000);assert not m.irq(0)
    m.write(RTC+0x40,0x2900)
    m.write(RTC+0x64,0xfffffff0)
    m.write(RTC+0x70,0x10000);m.write(RTC+0x74,0x10000)
    m.write(RTC+0x6c,0x10)
    m.step(31*1_000_000_000//32768);assert not m.irq(0)
    m.step(2*1_000_000_000//32768+1);assert m.irq(0),'32-bit RTC wrap comparator'
    m.write(RTC+0x6c,0x10000);assert not m.irq(0),'RTC reprogram acknowledges its own IRQ'
    m.command('system_reset');assert not m.irq(0) and not m.irq(1)
    assert m.read(PM+0xc)==0 and m.read(PM+0x10)==0 and m.read(RTC+0x40)==0

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu',default='build/qemu-la-rtc/qemu-system-loongarch64')
    args=parser.parse_args()
    directory=Path(tempfile.mkdtemp(prefix='rtc-model.',dir=ROOT/'build/loongarch'))
    machine=Machine(ROOT/args.qemu,directory)
    try:verify(machine)
    finally:machine.close()
    # Migration keeps latched owners and active comparator deadlines, and save does
    # not delete a timer in the source if migration is cancelled or the VM resumes.
    source=directory/'migration-source';target=directory/'migration-target'
    source.mkdir();target.mkdir();state=directory/'migration.bin'
    machine=Machine(ROOT/args.qemu,source)
    try:
        machine.write(RTC+0x40,0x900)
        machine.write(PM+0x10,EVENT);machine.write(PM+0x14,1)
        machine.write(RTC+0x34,match(BASE+datetime.timedelta(seconds=1)))
        machine.write(RTC+0x38,match(BASE+datetime.timedelta(seconds=3)))
        machine.step(1_000_000_000);assert machine.irq(0) and machine.irq(1)
        machine.command('migrate',{'uri':'file:'+str(state)})
        import time
        for _ in range(1000):
            progress=machine.command('query-migrate')['status']
            if progress=='completed':break
            if progress=='failed':raise RuntimeError(machine.command('query-migrate'))
            time.sleep(.01)
        else:raise RuntimeError('RTC migration did not complete')
        restored=Machine(ROOT/args.qemu,target,incoming=True)
        try:
            # qtest owns a synthetic clock outside the migrated TCG timer state.
            restored.step(1_000_000_000)
            restored.command('migrate-incoming',{'uri':'file:'+str(state)})
            for _ in range(1000):
                if restored.command('query-status')['status']!='inmigrate':break
                time.sleep(.01)
            else:raise RuntimeError('RTC migration did not restore')
            assert restored.read(PM+0xc)&EVENT and restored.irq(0) and restored.irq(1)
            restored.write(RTC+0x34,0);assert not restored.irq(0)
            restored.write(PM+0xc,EVENT);assert not restored.irq(1)
            restored.step(2_000_000_000);assert restored.irq(0) and restored.irq(1)
            restored.write(RTC+0x38,0);restored.write(PM+0xc,EVENT)
            assert not restored.irq(0) and not restored.irq(1)
        finally:restored.close()
    finally:machine.close()
    print('LS7A actual QEMU PM/W1C/mask/independent IRQ/rewrite/UTC/wrap/reset/migration PASS:',directory)
if __name__=='__main__':main()
