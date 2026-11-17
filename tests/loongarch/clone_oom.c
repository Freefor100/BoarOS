#include <arch/task.h>
#include <kernel/errno.h>
#include <kernel/uaccess.h>
#include <platform/loongarch_virt.h>
#include "../../kernel/sched/private.h"
void la_fault_injection_set(int);
enum kernel_scheduler_status __real_arch_process_clone_current(const struct arch_trap_frame *,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,int32_t,int64_t *);
enum kernel_scheduler_status __wrap_arch_process_clone_current(const struct arch_trap_frame *frame,
    uint64_t flags,uint64_t stack,uint64_t parent_tid,uint64_t tls,uint64_t child_tid,uint64_t pidfd,int32_t requested_tid,int64_t *result)
{
    static int injected;
    int inject=!injected && (flags&UINT64_C(0x10000));
    uint64_t before=physical_page_available(scheduler.allocator);
    int32_t parent_tid_before = 0;
    int check_tid = inject && (flags & UINT64_C(0x100000));
    size_t copied;
    struct kernel_task *parent = kernel_task_current();
    if (check_tid && (kernel_copy_from_user(&parent->mm, &parent_tid_before, parent_tid,
            sizeof(parent_tid_before), &copied) || copied != sizeof(parent_tid_before)))
        la_virt_fatal("clone OOM parent TID fixture");
    if(inject) {injected=1;la_fault_injection_set(CLONE_OOM_AFTER);}
    enum kernel_scheduler_status status=__real_arch_process_clone_current(frame,flags,stack,parent_tid,tls,child_tid,pidfd,requested_tid,result);
    if(inject) {
        la_fault_injection_set(-1);
        if(status!=KERNEL_SCHEDULER_STATUS_OK || *result!=-KERNEL_ENOMEM || physical_page_available(scheduler.allocator)!=before)
            la_virt_fatal("clone construction rollback");
        int32_t after = 0;
        if (check_tid && (kernel_copy_from_user(&parent->mm, &after, parent_tid,
            sizeof(after), &copied) || copied != sizeof(after) || after != parent_tid_before))
            la_virt_fatal("failed clone changed parent TID");
        la_virt_puts("LA clone failed before publication; task/stack pages restored\n");
    }
    return status;
}
