#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <kernel/signal.h>
#include <kernel/task_state.h>
/* Storage reset mirrors the production primitive; the shared full-width
 * replacement predicate under test is compiled from the production header. */
void kernel_signal_clear_syscall_restart(struct kernel_task *task)
{task->syscall_restart=(struct kernel_syscall_restart_state){0};}
int main(void)
{
    struct kernel_task *task=calloc(1,sizeof(*task));assert(task);
    const unsigned kinds[]={KERNEL_SYSCALL_RESTART_NANOSLEEP,KERNEL_SYSCALL_RESTART_FUTEX_TIMED};
    const uint64_t replacement[]={173,UINT64_C(0x100000080),UINT64_MAX};
    for(unsigned k=0;k<2;k++) {
        task->syscall_restart.kind=kinds[k];
        kernel_signal_retire_replaced_restart(task,128,128);
        assert(task->syscall_restart.kind==kinds[k]);
        for(unsigned n=0;n<3;n++) {
            task->syscall_restart.kind=kinds[k];
            kernel_signal_retire_replaced_restart(task,128,replacement[n]);
            assert(task->syscall_restart.kind==KERNEL_SYSCALL_RESTART_NONE);
        }
    }
    free(task);puts("PTRACE restart full-width replacement/unchanged PASS");return 0;
}
