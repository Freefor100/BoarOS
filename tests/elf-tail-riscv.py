#!/usr/bin/env python3
"""Check a real ELF's file tail and BSS in identical Linux/BoarOS guests."""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/program-inventory'))
sys.path.insert(0, str(ROOT / 'tests/diff-abi'))
import harness
import suites


def output_contract(case, observation):
    _ = case  # The single test has one fixed, observable success marker.
    stdout = bytes.fromhex(observation['stdout_hex'])
    stderr = bytes.fromhex(observation['stderr_hex'])
    return {'status': 'pass' if stdout == b'ELF BSS PASS\n' and not stderr
            else 'upstream-failure', 'errors': []}


def run():
    tools = ROOT / 'build/diff-abi/tools/usr/bin'
    if tools.is_dir():
        os.environ['PATH'] = str(tools) + os.pathsep + os.environ['PATH']
    destination = ROOT / 'build/elf-tail-riscv'
    destination.mkdir(parents=True, exist_ok=True)
    compiler = ROOT / 'build/riscv/musl-root/bin/musl-gcc'
    if not compiler.is_file():
        raise RuntimeError('build the RISC-V musl toolchain first')
    flags = []
    if subprocess.run([str(compiler), '-fno-link-libatomic', '-E', '-x', 'c',
                       '/dev/null'], stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode == 0:
        flags.append('-fno-link-libatomic')
    program = destination / 'elf-tail'
    driver = destination / 'init'
    subprocess.run([str(compiler), *flags, '-static', '-O2',
                    '-Wl,-z,norelro', '-Wl,--build-id=none',
                    str(ROOT / 'tests/riscv/elf_tail_main.c'), '-o', str(program)],
                   check=True)
    headers = subprocess.check_output(['riscv64-linux-gnu-readelf', '-lW',
                                       str(program)], text=True)
    loads = re.findall(r'^\s*LOAD\s+0x([0-9a-f]+)\s+0x([0-9a-f]+).*?'
                       r'0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+RW\s', headers, re.M)
    if not any(int(vaddr, 16) % 4096 == 0 and int(file_size, 16) > 8192 and
               int(file_size, 16) % 4096 != 0 and int(memory_size, 16) >
               int(file_size, 16) + 4096 for _, vaddr, file_size, memory_size in loads):
        raise RuntimeError('test ELF lacks the intended multi-page file/BSS boundary')
    image = program.read_bytes()
    if not any(any(image[end:min((end + 4095) & ~4095, len(image))])
               for offset, _, file_size, _ in loads
               for end in [int(offset, 16) + int(file_size, 16)]):
        raise RuntimeError('test ELF has no nonzero bytes after the loadable file tail')
    subprocess.run([str(compiler), *flags, '-static', '-O2',
                    str(ROOT / 'tests/program-inventory/suite_driver.c'),
                    '-o', str(driver)], check=True)
    linux, _ = harness.linux_build(ROOT / 'tests/program-inventory/linux.config')
    kernel = ROOT / 'kernel-rv'
    manifest = {'files': [{'source': str(program), 'destination': '/elf-tail',
                           'sha256': suites.digest(program)}],
                'cases': [{'id': 'elf.tail', 'argv': ['/elf-tail'],
                           'expected_exit': 0}]}
    key = hashlib.sha256((suites.digest(kernel) + suites.digest(program) +
                          suites.digest(driver) + suites.digest(__file__) +
                          suites.digest(ROOT / 'tests/program-inventory/suites.py')).encode()).hexdigest()[:16]
    result = suites.run_suite(manifest, destination / key, driver, linux,
                              kernel, output_validator=output_contract)
    row = result['results']['elf.tail']
    print(f"ELF tail: {row['status']} (Linux {row['linux_status']}, "
          f"BoarOS {row['boaros_status']}); evidence: {destination / key}")
    return 0 if row['status'] == 'pass' and result['status'] == 'complete' else 1


if __name__ == '__main__':
    raise SystemExit(run())
