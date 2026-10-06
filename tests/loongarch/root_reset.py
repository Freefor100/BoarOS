#!/usr/bin/env python3
"""An unpublished root must retain then reclaim a partially initialized PCI owner."""
from pathlib import Path
import tempfile
from root import disk
from guest import run_guest
with tempfile.TemporaryDirectory(prefix='pci-root-reset-',dir='build/loongarch') as directory:
    directory=Path(directory)
    for memory in ('512M','1G'):
        image=disk(directory,Path('build/loongarch/root-probe'))
        command=['build/qemu-la-rtc/qemu-system-loongarch64','-machine','virt','-cpu','la464','-global','ls7a_rtc.toy-enabled=on',
                 '-smp','1','-m',memory,'-kernel','build/loongarch/kernel-pci-root-reset',
                 '-drive',f'file={image},format=raw,if=none,id=root',
                 '-device','virtio-blk-pci,drive=root,addr=5,disable-legacy=on',
                 '-net','none','-nographic','-no-reboot']
        code,text=run_guest(command,30)
        if code or 'fatal' in text or 'LA PID 1' in text or any(marker not in text for marker in (
            'LA root PCI initial reset confirmation denied',
            'LA root boot errno=0xfffffffffffffffb','LA root owners released')):
            print(text);raise SystemExit(f'LA PCI partial root owner failed ({memory})')
        print(f'LA PCI partial root/{memory}: EIO, unpublished PID1 and full owner recovery PASS')
