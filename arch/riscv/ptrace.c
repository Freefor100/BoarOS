#include "../../kernel/sched/ptrace_internal.h"
#include <kernel/task_state.h>
#include <kernel/errno.h>
#include <string.h>
size_t arch_ptrace_regset_size(unsigned note)
{ return note==1 ? 32*8 : note==2 ? 33*8 : 0; }
void arch_ptrace_regset(struct kernel_task *t,unsigned note,uint64_t *r,uint64_t *orig,int write)
{
    (void)orig;
    struct arch_trap_frame *f=(void *)(t->stack_high-sizeof(*f));
    if (note==1) {
        if (write) { f->sepc=r[0]; memcpy(&f->ra,r+1,31*8); }
        else { r[0]=f->sepc; memcpy(r+1,&f->ra,31*8); }
    } else if (write) {
        memcpy(t->fpu.regs,r,32*8);
        t->fpu.fcsr=r[32]&255; t->fpu.saved=1;
    } else {
        memcpy(r,t->fpu.regs,32*8); r[32]=(uint32_t)t->fpu.fcsr;
    }
}
int arch_ptrace_user(struct kernel_task *t,uint64_t index,uint64_t *word,uint64_t *orig,int write)
{ (void)t; (void)index; (void)word; (void)orig; (void)write; return -KERNEL_EIO; }
void arch_ptrace_syscall(struct arch_trap_frame *f,uint32_t *arch,uint64_t *sp,uint64_t *nr,uint64_t *args)
{
    *arch=UINT32_C(0xc00000f3); *sp=f->sp; *nr=f->a7;
    memcpy(args,&f->a0,6*8);
}
