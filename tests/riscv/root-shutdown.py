#!/usr/bin/env python3
"""Root teardown after PID 1 exit or explicit reboot, with real user owners."""
import sys
from pathlib import Path
sys.path[:0] = [str(Path(__file__).resolve().parent), str(Path(__file__).resolve().parents[1])]
import argparse
import os
import json
import selectors
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/diff-abi"))
import harness
from arch_profiles import PROFILES


def action_guest(command, work, mode, timeout):
    """Observe platform reset/power events independently of guest console text."""
    qmp_path=work/(mode+'.qmp')
    command=[*command,'-S','-qmp',f'unix:{qmp_path},server=on,wait=off']
    output=bytearray();events=[];messages=bytearray();status=None;halted_at=None
    guest=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    qmp=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
    selector=selectors.DefaultSelector()
    try:
        deadline=time.monotonic()+timeout
        while True:
            try:qmp.connect(str(qmp_path));break
            except (FileNotFoundError,ConnectionRefusedError):
                if guest.poll() is not None or time.monotonic()>deadline:raise RuntimeError('QMP startup failed')
                time.sleep(.01)
        qmp.sendall(b'{"execute":"qmp_capabilities","id":"cap"}\n{"execute":"cont","id":"cont"}\n')
        qmp.setblocking(False)
        selector.register(qmp,selectors.EVENT_READ,'qmp')
        selector.register(guest.stdout,selectors.EVENT_READ,'console')
        while selector.get_map():
            now=time.monotonic()
            if now>deadline:raise TimeoutError(f'{mode} guest timed out')
            for key,_ in selector.select(.1):
                data=os.read(key.fileobj.fileno(),65536)
                if not data:selector.unregister(key.fileobj);continue
                if key.data=='console':output.extend(data)
                else:
                    messages.extend(data)
                    while b'\n' in messages:
                        line,_,remaining=messages.partition(b'\n');messages[:]=remaining
                        message=json.loads(line)
                        if 'event' in message:events.append(message)
                        if message.get('id')=='halt-status':status=message.get('return',{}).get('status')
            if b'BoarOS: reboot failed stage=' in output:
                raise RuntimeError('root teardown refused reboot; guest retained remaining owners and parked')
            if mode=='halt' and halted_at is None and b'BoarOS: halted\n' in output.replace(b'\r',b''):
                halted_at=time.monotonic()
                qmp.sendall(b'{"execute":"query-status","id":"halt-status"}\n')
            if mode=='halt' and halted_at is not None:
                if any(event['event'] in ('RESET','SHUTDOWN') for event in events):
                    raise RuntimeError('HALT reset or powered off the guest')
                if guest.poll() is not None:raise RuntimeError('HALT exited QEMU')
                if time.monotonic()-halted_at>=2 and status=='running':
                    guest.terminate();guest.wait(timeout=5)
                    break
        code=guest.wait(timeout=5)
        text=output.decode(errors='replace')
        if mode=='reboot':
            resets=[event for event in events if event['event']=='RESET']
            shutdowns=[event for event in events if event['event']=='SHUTDOWN']
            if code or len(resets)!=2 or len(shutdowns)!=1 or any(
                event.get('data',{}).get('reason')!='guest-reset' or not event['data'].get('guest') for event in resets
            ) or shutdowns[0].get('data',{}).get('reason')!='guest-shutdown':
                raise RuntimeError(f'reset/poweroff QMP events differ: exit={code}, events={events}')
            for generation,action in enumerate(('restart','restart2','poweroff')):
                marker=f'REBOOT READY generation={generation} action={action} invalid=PASS persistence=PASS stopped=PASS pid1=live'
                if text.count(marker)!=1:raise RuntimeError('missing reboot generation: '+marker)
            expected=['BoarOS: root owners released; reboot action=restart command=',
                'BoarOS: root owners released; reboot action=restart command='+'R'*255,
                'BoarOS: root owners released; reboot action=poweroff command=']
            actual=[line for line in text.replace('\r','').splitlines() if line.startswith('BoarOS: root owners released; reboot action=')]
            if actual!=expected:raise RuntimeError(f'root teardown/action output differs: {actual}')
        elif halted_at is None or status!='running':raise RuntimeError('HALT was not observed in a live VM')
        if 'REBOOT RETURNED' in text or 'fatal' in text or 'root finish failure' in text or 'root cleanup errno=' in text or 'reboot failed stage=' in text:
            raise RuntimeError('guest action or root teardown failed')
        return text
    finally:
        if guest.poll() is None:guest.kill();guest.wait()
        if guest.stdout:
            output.extend(guest.stdout.read());guest.stdout.close()
        (work/(mode+'.log')).write_bytes(output)
        (work/(mode+'-qmp.json')).write_text(json.dumps(events,indent=2)+'\n')
        selector.close();qmp.close();qmp_path.unlink(missing_ok=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--arch',choices=tuple(PROFILES),default='riscv')
    p.add_argument("--kernel", type=Path)
    p.add_argument('--qemu')
    p.add_argument('--cc',type=Path)
    p.add_argument('--program',type=Path)
    p.add_argument('--cpus',type=int,default=1)
    p.add_argument('--timeout',type=float,default=60)
    p.add_argument('--prepare-only',action='store_true')
    p.add_argument("--cases", default="daemon,stopped,threads,forking")
    args = p.parse_args()
    profile=PROFILES[args.arch]
    args.kernel=args.kernel or ROOT/profile.kernel
    args.qemu=args.qemu or os.environ.get('QEMU_RISCV64' if args.arch=='riscv' else 'QEMU_LOONGARCH64',profile.qemu)
    work = Path(tempfile.mkdtemp(prefix="root-shutdown.", dir=ROOT / "build"))
    program = args.program or work / "init"
    if not args.program:
        cc=args.cc or ROOT/('build/riscv/musl-root/bin/musl-gcc' if args.arch=='riscv' else 'build/loongarch/musl-root/bin/musl-gcc')
        subprocess.run([str(cc),*profile.musl_flags(cc),"-static", "-O2", "-pthread",
                        "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/riscv/root_shutdown.c"), "-o", str(program)], check=True)
    fixture = harness.fixture(work, program)
    for mode in args.cases.split(","):
        if mode not in ("daemon", "stopped", "threads", "forking",'reboot','halt'):
            p.error("unknown shutdown scenario")
        mode_file = work / "mode"
        mode_file.write_text(mode)
        disk = work / (mode + ".img")
        shutil.copyfile(fixture, disk)
        subprocess.run(["debugfs", "-w", "-R", f"write {mode_file} /mode", str(disk)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        log = work / (mode + ".log")
        if args.prepare_only:continue
        command = [args.qemu,
                   "-machine", "virt", "-kernel", str(args.kernel),
                   "-m", "512M", "-smp", str(args.cpus), "-nographic",
                   "-drive", f"file={disk},if=none,format=raw,id=root",
                   "-device", profile.block('modern')]
        command+=['-cpu','la464','-global','ls7a_rtc.toy-enabled=on','-net','none'] if args.arch=='loongarch' else ['-bios','default']
        if mode in ('reboot','halt'):
            action_guest(command,work,mode,args.timeout)
            subprocess.run(['e2fsck','-fn',str(disk)],stdout=(work/(mode+'.fsck')).open('w'),stderr=subprocess.STDOUT,check=True)
            generation=work/(mode+'-generation');persisted=work/(mode+'-persisted')
            for source,target in (('/generation',generation),('/persisted',persisted)):
                subprocess.run(['debugfs','-R',f'dump {source} {target}',str(disk)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            expected_generation=3 if mode=='reboot' else 1
            if int.from_bytes(generation.read_bytes(),'little')!=expected_generation or persisted.read_bytes()!=bytes(
                (i*7+expected_generation-1)%256 for i in range(32791)):
                raise RuntimeError('persisted reboot data differs')
            print(f'PASS {args.arch}/{args.cpus}CPU root action: {mode}',flush=True)
            continue
        command+=['-no-reboot']
        with log.open("w") as stream:
            subprocess.run(command, stdin=subprocess.DEVNULL, stdout=stream,
                           stderr=subprocess.STDOUT, timeout=args.timeout, check=True)
        text = log.read_text()
        if (f"INIT SHUTDOWN READY {mode}" not in text or
                not profile.root_success(text,37)):
            raise RuntimeError(f"shutdown failed: {log}\n" + text[-1800:])
        print("PASS PID 1 shutdown:", mode, flush=True)
    print("shutdown records:", work)


if __name__ == "__main__":
    main()
