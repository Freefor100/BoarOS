#ifndef BOAROS_KERNEL_CPU_H
#define BOAROS_KERNEL_CPU_H
#include <arch/context.h>
#include <stdint.h>

#define KERNEL_CPU_INITIALIZED UINT32_C(0x43505531)
struct kernel_task;
struct kernel_io_context;
struct kernel_raw_guard;
/* CPU本地借用须处于IRQ关闭或禁止抢占范围；任务I/O owner不随CPU保存。 */
struct kernel_cpu {
    uint64_t hardware_id;
    struct kernel_task *current;
    struct kernel_io_context *bootstrap_io;
    struct kernel_raw_guard *raw_locks;
    uint32_t initialized, preempt_depth;
    unsigned need_resched;
    unsigned rotate_other;
};
static inline struct kernel_cpu *kernel_cpu_current(void)
{
    struct kernel_cpu *cpu = arch_current_cpu_get();
    if (!cpu || cpu->initialized != KERNEL_CPU_INITIALIZED) __builtin_trap();
    return cpu;
}
void kernel_cpu_initialize(struct kernel_cpu *, uint64_t, struct kernel_io_context *);
void kernel_cpu_boot_initialize(uint64_t hardware_id);
void kernel_cpu_boot_rebind(void);
void kernel_preempt_disable(void);
void kernel_preempt_enable(void);
void kernel_assert_can_block(void);
#endif
