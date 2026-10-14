#!/usr/bin/env python3
"""Select and freeze the installed RV64 OpenSBI identity for MTTCG tests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--github-env',action='store_true')
    args=parser.parse_args()
    files=subprocess.check_output(['dpkg-query','-L','opensbi'],text=True).splitlines()
    candidates=[Path(path) for path in files if path.endswith('/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin') and Path(path).is_file()]
    if len(candidates)!=1:raise SystemExit(f'Expected one installed RV64 generic OpenSBI image, found {candidates}')
    firmware=candidates[0].resolve(strict=True)
    qemu=Path(shutil.which('qemu-system-riscv64')).resolve(strict=True)
    def identity(path):return {'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
    report={'firmware':identity(firmware),'qemu':identity(qemu),
        'qemu_version':subprocess.check_output([str(qemu),'--version'],text=True),
        'opensbi_package':subprocess.check_output(['dpkg-query','-W','opensbi'],text=True)}
    output=Path('build/riscv-smp/ci-executor.json');output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(report,indent=2)+'\n')
    if args.github_env:
        with Path(os.environ['GITHUB_ENV']).open('a') as environment:environment.write(f'QEMU_RISCV_FIRMWARE={firmware}\n')

if __name__=='__main__':main()
