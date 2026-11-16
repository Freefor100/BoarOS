#!/usr/bin/env python3
"""Native ordinary redistribution while source and destination stay busy."""
import argparse
import json
from pathlib import Path
import shutil
import sys
from user import boot_user, create_run_directory, harness, validate_execution
from riscv import identity
ROOT=Path(__file__).resolve().parents[2]
MARKER='FAIR-BALANCE PASS busy-source=0 busy-destination=1 children=4'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel',type=Path,required=True)
    parser.add_argument('--program',type=Path,required=True)
    parser.add_argument('--qemu',default='qemu-system-riscv64')
    parser.add_argument('--firmware',default='default')
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    folder=create_run_directory(args.output,ROOT/'build/fair-balance-guest/runs')
    program=folder/'init';kernel=folder/'kernel-rv'
    shutil.copyfile(args.program,program);shutil.copyfile(args.kernel,kernel)
    disk=harness.fixture(folder,program)
    report={'status':'running','program':identity(program),'kernel':identity(kernel)}
    try:
        text,run,log=boot_user(args.qemu,args.firmware,str(kernel),disk,'512M',2,folder)
        if text.replace('\r','').splitlines().count(MARKER)!=1:
            raise RuntimeError('busy-core migration and child-cleanup handshake did not complete')
        validate_execution(text,2,True,require_parallel=True)
        harness.run_logged([sys.executable,str(ROOT/'tests/tools/check-stack-report.py'),str(log)],folder/'stack.log')
        harness.run_logged(['e2fsck','-fn',str(disk)],folder/'fsck.log')
        report.update(status='passed',execution=run);disk.unlink()
        print(MARKER+'; PID1/heap/stack/fsck cleanup verified')
    except Exception as error:
        report.update(status='failed',error=str(error));raise
    finally:
        (folder/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        print('Fair balance report:',folder/'report.json',flush=True)
if __name__=='__main__':main()
