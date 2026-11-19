#include <arch/task.h>
#include <kernel/task_state.h>
#include <kernel/syscall.h>
#include <kernel/uaccess.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int32_t user_tids[] = {1, 17, 45};
static unsigned copies, creates;
static uint64_t args[11] = { [4]=17, [9]=3 };

enum kernel_task_status kernel_task_mm_borrow_mutable(struct kernel_task *task, struct kernel_mm **mm)
{ *mm=&task->mm; return KERNEL_TASK_STATUS_OK; }
enum kernel_uaccess_status kernel_user_range_check(uint64_t addr, size_t size)
{ (void)addr; (void)size; return KERNEL_UACCESS_STATUS_OK; }
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *mm, void *dst, uint64_t src,
    size_t size, size_t *copied)
{
    (void)mm;
    assert(src==(uintptr_t)args || src==(uintptr_t)user_tids);
    memcpy(dst,(const void *)(uintptr_t)src,size); *copied=size;
    if(src==(uintptr_t)user_tids) {
        assert(size==sizeof(user_tids));
        copies++;
        /* User overwrites both the array and clone_args after import. */
        memset(user_tids,0,sizeof(user_tids)); memset(args,0,sizeof(args));
    }
    return KERNEL_UACCESS_STATUS_OK;
}
enum kernel_scheduler_status arch_process_clone_current(const struct arch_trap_frame *frame,
    uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t tls, uint64_t ctid, uint64_t pidfd,
    const int32_t *tids, size_t count, int32_t cgroup, int64_t *result)
{
    (void)frame;
    assert(flags==17 && !stack && !ptid && !tls && !ctid && !pidfd && cgroup==-1);
    assert(count==3 && tids[0]==1 && tids[1]==17 && tids[2]==45);
    creates++; *result=45;
    return KERNEL_SCHEDULER_STATUS_OK;
}
int main(void)
{
    struct kernel_task task={0};
    args[8]=(uintptr_t)user_tids;
    const struct kernel_syscall_request req={.number=435,.arguments={(uintptr_t)args,sizeof(args)}};
    int64_t result=-1;
    assert(!kernel_clone3_current(NULL,&task,&req,&result));
    assert(result==45 && copies==1 && creates==1);
    puts("clone3 import preserved one immutable array and argument snapshot");
}
