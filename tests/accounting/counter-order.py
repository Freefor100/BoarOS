#!/usr/bin/env python3
"""Check RV TIME input fences in the actual SMP counter/snapshot code."""
import argparse
import re
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernel',required=True)
parser.add_argument('--objdump',default='riscv64-elf-objdump')
args=parser.parse_args()
def function(name):
    return subprocess.check_output([args.objdump,'-d',args.kernel,'--disassemble='+name],text=True)
text=function('riscv_time_read_ordered')
assert '<riscv_time_read_ordered>:' in text,'ordered TIME counter entry missing'
lines=text.splitlines();reads=[i for i,line in enumerate(lines) if re.search(r'\brdtime\b|\bcsrr\s+\w+,time',line)]
assert len(reads)==1,'counter sample missing or duplicated'
i=reads[0]
def fence(line,pred,succ):
    m=re.search(r'\bfence\s+([iorw]+),\s*([iorw]+)',line)
    return m and set(pred)<=set(m[1]) and set(succ)<=set(m[2])
assert any(fence(line,'rw','i') for line in lines[:i]),'seq reads/writes must precede TIME input'
assert any(fence(line,'i','r') for line in lines[i+1:]),'TIME input must precede final seq read'
for name in ('scheduler_task_cpu_time_locked','kernel_cpu_account_trap_begin',
             'kernel_cpu_account_trap_end','kernel_cpu_account_user_return'):
    body=function(name)
    assert '<riscv_time_read_ordered>' in body,name+' bypasses ordered sample'
    assert not re.search(r'<riscv_time_read>',body),name+' contains an unordered sample'
print('RV SMP sequence counter: RW -> TIME input -> R, real snapshot/trap callers verified')
