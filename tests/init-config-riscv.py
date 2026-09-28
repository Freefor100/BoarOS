#!/usr/bin/env python3
"""Alternate PID 1 profiles in one build directory and check real user stacks."""
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
RUN = ROOT / 'build/init-config-run'

def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)

def main():
    RUN.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get('CROSS_COMPILE') or ('riscv64-elf-' if shutil.which('riscv64-elf-gcc') else 'riscv64-unknown-elf-')
    program = RUN / 'probe'
    run([cc + 'gcc', '-march=rv64gc', '-mabi=lp64d', '-O2', '-ffreestanding', '-fno-builtin', '-nostdlib', '-static', '-Wl,--build-id=none', '-Wl,-T,tests/riscv/user_elf.ld', '-o', program, 'tests/riscv/init_config.c'])
    tree = RUN / 'tree'
    tree.mkdir(exist_ok=True)
    for name in ('init', 'entry'):
        shutil.copyfile(program, tree / name)
        (tree / name).chmod(0o755)
    disk = RUN / 'root.img'
    with disk.open('wb') as stream:
        stream.truncate(32 * 1024 * 1024)
    run(['mkfs.ext4', '-q', '-F', '-d', tree, disk])
    profiles = [dict(path='/init', argv=['/init'], envp=[]), dict(path='/entry', argv=['custom-zero', 'arg with spaces'], envp=['MODE=probe', 'SECOND='])]
    try:
        for index, profile in enumerate([profiles[0], profiles[1], profiles[0], profiles[1], dict(path='/missing-init', argv=['/missing-init'], envp=[])]):
            config = RUN / 'profile.json'
            config.write_text(json.dumps(profile))
            run(['make', 'all', 'INIT_CONFIG=' + str(config)], stdout=subprocess.DEVNULL)
            with (RUN / f'{index}.log').open('wb') as log:
                run([os.environ.get('QEMU_RISCV64', 'qemu-system-riscv64'), '-machine', 'virt', '-bios', 'default', '-kernel', 'kernel-rv', '-m', '512M', '-smp', '1', '-nographic', '-no-reboot', '-drive', f'file={disk},if=none,format=raw,id=root', '-device', 'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0'], stdout=log, stderr=subprocess.STDOUT, timeout=30)
            output = (RUN / f'{index}.log').read_text()
            if index == 4:
                assert 'root boot error' in output and 'INIT DEFAULT PASS' not in output, output[-3000:]
            else:
                assert ('INIT DEFAULT PASS' if index % 2 == 0 else 'INIT CUSTOM PASS!') in output and 'PID 1 exited status=0x0' in output, output[-3000:]
    finally:
        run(['make', 'all'], stdout=subprocess.DEVNULL)
    print('PID 1 profile alternation passed')

if __name__ == '__main__':
    main()
