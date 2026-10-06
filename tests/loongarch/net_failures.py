#!/usr/bin/env python3
"""Real PCI owner rollback at DMA, IRQ, network heap/worker and reset boundaries."""
from pathlib import Path
import tempfile
from guest import run_guest
from root import ROOT,disk
work=Path(tempfile.mkdtemp(prefix='net-failure-run.',dir=ROOT/'build/loongarch'))
for case in range(9):
    for memory in ('512M','1G'):
        directory=work/f'{case}-{memory}';directory.mkdir()
        image=disk(directory,ROOT/'build/loongarch/network-contract')
        command=['build/qemu-la/qemu-system-loongarch64','-machine','virt','-cpu','la464','-smp','1','-m',memory,
            '-kernel',f'build/loongarch/kernel-net-failure-{case}','-nographic','-no-reboot',
            '-drive',f'file={image},if=none,format=raw,id=root','-device','virtio-blk-pci,drive=root,addr=1,disable-legacy=on',
            '-netdev','user,id=net','-device','virtio-net-pci,netdev=net,addr=5,disable-legacy=on']
        code,text=run_guest(command,90);(directory/'console.log').write_text(text)
        good=not code and 'fatal' not in text and 'LA root owners released' in text
        if case<8:
            error=-5 if case in (3,7) else -12
            good &= f'LA root boot errno=0x{error&((1<<64)-1):016x}' in text and 'LA PID 1' not in text
        else:
            good &= 'NETWORK PASS contract' in text and 'LA PID 1 exited reason=0x0000000000000001 status=0x0000000000000000' in text
        marker='LA net DMA/IRQ construction rollback passed' if case<4 else 'LA network heap/task/stack construction rollback passed' if case<7 else 'LA net reset refused with owner retained'
        good &= marker in text
        if not good:print(text);raise SystemExit(f'net failure {case}/{memory}: {work}')
        image.unlink();print(f'LA net failure {case}/{memory}: unpublished failure or real exit plus page/heap/stack/BAR baseline PASS',flush=True)
print(work)
