#!/usr/bin/env python3
"""Exercise the real RV timer with controlled TIME and SBI at the machine seam."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'arch/riscv/timer.c').read_text()
# Translate only CSR/IRQ instructions; all deadline/state/error code is unchanged.
start=source.index('    asm volatile("fence rw, i')
end=source.index('    return value;',start)
source=source[:start]+'    value=riscv_time_read();\n'+source[end:]
start=source.index('    asm volatile("csrs sie, %0')
end=source.index('    return RISCV_TIMER_STATUS_OK;',start)
source=source[:start]+'    host_timer_enable(stie);\n'+source[end:]
source='void host_timer_enable(unsigned long);\n'+source
with tempfile.TemporaryDirectory(prefix='boaros-timer-events-') as directory:
    work=Path(directory);(work/'timer.c').write_text(source)
    for cpus in (1,2):
        exe=work/f'test-{cpus}'
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror',
            '-DBOAROS_PAGE_SHIFT=12',f'-DBOAROS_MAX_CPUS={cpus}','-Itests/host/random','-idirafter','include',
            '-Iarch/riscv','tests/host/riscv_timer_events.c',str(work/'timer.c'),'-o',str(exe)],cwd=root,check=True)
        subprocess.run([str(exe)],check=True)
