#!/usr/bin/env python3
"""Execute the real IRQ dispatcher against a level/OR model of the fixed PIC."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'platform/loongarch_pci.c').read_text()
source=source.replace('__asm__ volatile("iocsrrd.w %0, %1":"=r"(value):"r"(address):"memory");','value=host_iocsr_read(address);')
source=source.replace('__asm__ volatile("iocsrwr.w %0, %1"::"r"(value),"r"(address):"memory");','host_iocsr_write(address,value);')
source=source.replace('return (void *)(uintptr_t)(LA_UNCACHED_BASE+0x10000000+offset);','return (void *)(host_pic+offset);')
source=source.replace('__asm__ volatile("iocsrrd.d %0, %1":"=r"(misc):"r"(address):"memory");','misc=0;(void)address;')
source=source.replace('__asm__ volatile("iocsrwr.d %0, %1"::"r"(misc),"r"(address):"memory");','(void)misc;')
source=source.replace('__asm__ volatile("csrxchg %0, %1, 4":"+r"(value):"r"(mask):"memory");','(void)mask;(void)value;')
with tempfile.TemporaryDirectory(prefix='la-irq-') as temporary:
    work=Path(temporary);(work/'arch').mkdir()
    (work/'arch/context.h').write_text('#include <stdint.h>\nstatic inline uintptr_t arch_interrupt_save(void){return 0;}\nstatic inline void arch_interrupt_restore(uintptr_t v){(void)v;}\n')
    (work/'arch/bus.h').write_text('static inline void arch_io_barrier(void){__atomic_thread_fence(__ATOMIC_SEQ_CST);}\n')
    (work/'platform.c').write_text('extern unsigned char host_pic[];\nunsigned host_iocsr_read(unsigned);\nvoid host_iocsr_write(unsigned,unsigned);\n'+source)
    (work/'test.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <platform/loongarch_pci.h>
unsigned char host_pic[1024] __attribute__((aligned(8)));
static uint32_t regs[2048];
static unsigned a=1,b=1,signal_high,seen_a,seen_b,injected;
static int first_seen=-1;
static void sync_line(void)
{
    unsigned mask=*(uint32_t *)(host_pic+0x20),enabled=regs[0x1600/4];
    unsigned high=(a||b) && !(mask&(1U<<17)) && (enabled&(1U<<17));
    if(high && !signal_high) regs[0x1800/4]|=1U<<17;
    if(!high) regs[0x1800/4]&=~(1U<<17);
    signal_high=high;
}
unsigned host_iocsr_read(unsigned address) {sync_line();return regs[address/4];}
void host_iocsr_write(unsigned address,unsigned value)
{
    sync_line();
    if(address==0x1800 || address==0x1804) regs[address/4]&=~value;
    else regs[address/4]=value;
}
void la_virt_fatal(const char *s) {fprintf(stderr,"fatal %s\n",s);abort();}
static void first(void *owner)
{
    assert(owner==&a && la_virt_irq_active());
    if(a) {
        seen_a++;
        if(first_seen<0) first_seen=0;
        else if(first_seen==1 && !injected) {b=1;injected=1;}
        a=0;sync_line();
    }
}
static void second(void *owner)
{
    assert(owner==&b && la_virt_irq_active());
    if(b) {
        seen_b++;
        if(first_seen<0) first_seen=1;
        else if(first_seen==0 && !injected) {a=1;injected=1;}
        b=0;sync_line();
    }
}
int main(void)
{
    assert(la_virt_irq_initialize());
    assert(la_virt_irq_register(17,first,&a));
    assert(la_virt_irq_register(17,second,&b));
    sync_line();la_virt_irq_dispatch();
    assert(seen_a==(first_seen==0 ? 2U : 1U) && seen_b==(first_seen==1 ? 2U : 1U) &&
        injected && !a && !b && !la_virt_irq_active());
    la_virt_irq_unregister(17,&a);
    assert(!(*(uint32_t *)(host_pic+0x20)&(1U<<17)));
    la_virt_irq_unregister(17,&b);
    assert(*(uint32_t *)(host_pic+0x20)&(1U<<17));
    puts("LA shared level IRQ keeps completion arriving between ISR snapshots");
}
''')
    exe=work/'test'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(work),
                    '-idirafter',str(root/'include'),str(work/'platform.c'),str(work/'test.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
