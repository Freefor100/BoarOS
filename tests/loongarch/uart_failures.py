#!/usr/bin/env python3
"""UART failures before publication must leave no PID 1 or resource owner."""
from pathlib import Path
import tempfile
from guest import run_guest
from root import ROOT,disk
work=Path(tempfile.mkdtemp(prefix='uart-failure-run.',dir=ROOT/'build/loongarch'))
for case in range(6):
    for memory in ('512M','1G'):
        directory=work/f'{case}-{memory}';directory.mkdir()
        image=disk(directory,ROOT/'build/loongarch/network-contract')
        argv=['build/qemu-la/qemu-system-loongarch64','-machine','virt','-cpu','la464','-smp','1','-m',memory,
            '-kernel',f'build/loongarch/kernel-uart-failure-{case}','-nographic','-no-reboot','-net','none',
            '-drive',f'file={image},if=none,format=raw,id=root','-device','virtio-blk-pci,drive=root,addr=1,disable-legacy=on']
        code,text=run_guest(argv,90);(directory/'console.log').write_text(text)
        if case==5:
            if code or "BoarOS: fatal LA UART emergency bypass passed" not in text:
                print(text);raise SystemExit(f"UART emergency {memory}: {work}")
            image.unlink();print(f"LA UART active emergency/{memory}: loglevel/queue bypass PASS",flush=True);continue
        error=-5 if case==4 else -12
        if code or 'fatal' in text or 'LA PID 1' in text or 'LA root owners released' not in text or             'LA UART heap/task/stack/IRQ rollback passed' not in text or             f'LA root boot errno=0x{error&((1<<64)-1):016x}' not in text:
            print(text);raise SystemExit(f'UART failure {case}/{memory}: {work}')
        image.unlink();print(f'LA UART rollback {case}/{memory}: pages/heap/stack/BAR baseline PASS',flush=True)
print(work)
