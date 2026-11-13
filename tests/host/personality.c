#include <kernel/personality.h>
#include <kernel/task_state.h>
#include <kernel/syscall.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>
#include "../../kernel/syscall/private.h"
static int64_t invoke(struct kernel_task *task,uint64_t value)
{
    struct kernel_syscall_request request={.number=92,.arguments={value}};
    struct kernel_syscall_result result={0};
    assert(syscall_handle_personality(task,&request,&result)==KERNEL_SYSCALL_STATUS_OK);
    assert(result.action==KERNEL_SYSCALL_ACTION_RETURN);return result.value;
}
int main(void)
{
    struct kernel_task task={0},other={0};
    assert(invoke(&task,UINT32_MAX)==0);
    assert(invoke(&task,KERNEL_PERSONALITY_SUPPORTED)==0);
    assert(invoke(&task,UINT64_C(0x76543210ffffffff))==KERNEL_PERSONALITY_SUPPORTED);
    const uint32_t unsupported[]={1,8,0x80000,0x100000,0x200000,0x800000,0x1000000,0x2000000,0x8000000,0x80000000};
    for(unsigned i=0;i<sizeof(unsupported)/sizeof(*unsupported);i++) {
        assert(invoke(&task,KERNEL_PERSONALITY_SUPPORTED|unsupported[i])==-KERNEL_EOPNOTSUPP);
        assert(task.personality==KERNEL_PERSONALITY_SUPPORTED);
    }
    assert(!other.personality && invoke(&other,UINT32_MAX)==0);
    assert(invoke(&task,UINT64_C(0xfedcba9800000000))==KERNEL_PERSONALITY_SUPPORTED && !task.personality);
    const uint64_t trace_flags[]={0x2000,0x800000,0x802000};
    const uint64_t clone_types[]={17,0x10900};
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<2;j++) {
        struct kernel_syscall_request request={.number=220,.arguments={trace_flags[i]|clone_types[j]}};
        struct kernel_syscall_result result={0};
        syscall_decode_clone(&request,&result);
        assert(result.action==KERNEL_SYSCALL_ACTION_CLONE && result.value==0);
        assert(request.arguments[0]==(trace_flags[i]|clone_types[j]));
    }
    puts("personality actual handler: raw32/query/domain/unsupported-preserves-state/task isolation passed");
}
