#include "../../kernel/sched/ptrace_internal.h"
#include <kernel/task_state.h>
#include <kernel/errno.h>
#include <string.h>
size_t arch_ptrace_regset_size(unsigned note)
{ return note==1 ? 45*8 : note==2 ? 34*8 : 0; }
void arch_ptrace_regset(struct kernel_task *t,unsigned note,uint64_t *r,uint64_t *orig,int write)
{
    struct arch_trap_frame *f=(void *)(t->stack_high-sizeof(*f));
    if (note==1) {
        if (write) {
            memcpy(f->regs,r,32*8); *orig=r[32]; f->era=r[33]; f->badv=r[34];
        } else {
            memcpy(r,f->regs,32*8); r[32]=*orig; r[33]=f->era; r[34]=f->badv;
            memset(r+35,0,10*8);
        }
    } else if (write) {
        for (unsigned i=0;i<32;i++) t->fpu.regs[i][0]=r[i];
        t->fpu.fcc=r[32]; t->fpu.fcsr=(uint32_t)r[33];
        if (!t->fpu.width) t->fpu.width=1;
        if (!t->fpu.live_width) t->fpu.live_width=1;
        t->fpu.saved=1;
    } else {
        for (unsigned i=0;i<32;i++) r[i]=t->fpu.regs[i][0];
        r[32]=t->fpu.fcc; r[33]=t->fpu.fcsr;
    }
}
int arch_ptrace_user(struct kernel_task *t,uint64_t index,uint64_t *word,uint64_t *orig,int write)
{
    struct arch_trap_frame *f=(void *)(t->stack_high-sizeof(*f));
    if (index>34) return -KERNEL_EIO;
    uint64_t *slot=index<32 ? &f->regs[index] : index==32 ? orig : index==33 ? &f->era : &f->badv;
    if (write) *slot=*word; else *word=*slot;
    return 0;
}
void arch_ptrace_syscall(struct arch_trap_frame *f,uint32_t *arch,uint64_t *sp,uint64_t *nr,uint64_t *args)
{
    *arch=UINT32_C(0xc0000102); *sp=f->regs[3]; *nr=f->regs[11]; memcpy(args,f->regs+4,6*8);
}
