#include <kernel/task_state.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct kernel_task task = {0};
    struct kernel_signal_stack s = {.pointer=0x10000,.size=8192}, old, saved;
    kernel_signal_stack_reset(&task);
    assert(!kernel_signal_stack_update(&task, &s, &old, 0x50000));
    assert(old.flags == LINUX_SS_DISABLE);
    assert(!kernel_signal_stack_update(&task, 0, &old, 0x10000));
    assert(old.flags == 0);
    assert(!kernel_signal_stack_update(&task, 0, &old, 0x12000));
    assert(old.flags == LINUX_SS_ONSTACK);
    s.flags = 7;
    assert(kernel_signal_stack_update(&task, &s, 0, 0x12000) == -KERNEL_EPERM);
    assert(kernel_signal_stack_update(&task, &s, 0, 0x50000) == -KERNEL_EINVAL);
    s.flags = 0; s.size=2047;
    assert(kernel_signal_stack_update(&task, &s, 0, 0x50000) == -KERNEL_ENOMEM);
    s.size=8192;s.flags=LINUX_SS_AUTODISARM;
    assert(!kernel_signal_stack_update(&task, &s, 0, 0x50000));
    assert(kernel_signal_stack_prepare(&task, 0x50000, LINUX_SA_ONSTACK, &saved) == 0x12000);
    assert(saved.flags == LINUX_SS_AUTODISARM);
    assert(kernel_signal_stack_frame_valid(&saved,0x50000,LINUX_SA_ONSTACK,0x11000,0x12000));
    assert(!kernel_signal_stack_frame_valid(&saved,0x50000,LINUX_SA_ONSTACK,0xffff,0x12000));
    kernel_signal_stack_delivered(&task);
    assert(task.signal_stack.flags == LINUX_SS_DISABLE);
    assert(!kernel_signal_stack_update(&task,&saved,0,0x50000));
    assert(task.signal_stack.flags == LINUX_SS_AUTODISARM);
    s.pointer=UINT64_MAX-15;s.size=8192;
    assert(!kernel_signal_stack_update(&task,&s,0,0x50000));
    uint64_t top=kernel_signal_stack_prepare(&task,0x50000,LINUX_SA_ONSTACK,&saved);
    assert(!kernel_signal_stack_frame_valid(&saved,0x50000,LINUX_SA_ONSTACK,top-1000,top));
    puts("signal stack state/bounds passed");
    return 0;
}
