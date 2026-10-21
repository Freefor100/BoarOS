#ifndef BOAROS_KERNEL_IO_CONTEXT_H
#define BOAROS_KERNEL_IO_CONTEXT_H

#include <kernel/sync.h>

#if defined(__riscv) || defined(__loongarch__)
#include <kernel/task_state.h>
#endif

/* Inline access to the same executing owner as the external provider. TP still
 * owns this stack while cpu.current may already name the next task. */
static __inline__ __attribute__((__always_inline__))
struct kernel_io_context *kernel_io_context_inline(void)
{
#if defined(__riscv) || defined(__loongarch__)
    /* CPU boot installs a full bootstrap_task before I/O, then stack switches
     * install real tasks. Native callers always have this embedded owner. */
    struct kernel_task *task = arch_current_thread_get();
    return &task->io_context;
#else
    return kernel_io_context_current();
#endif
}

#endif
