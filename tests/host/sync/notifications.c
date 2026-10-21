#include <kernel/cpu.h>
#include <kernel/time.h>
#include "../../../kernel/sched/private.h"
#include <assert.h>
#if BOAROS_MAX_CPUS > 1
/* Models without a scheduler/clock have not published secondary CPUs or
 * initialized time. Their object locks take the ordinary sleeping path. */
__attribute__((weak)) struct kernel_scheduler scheduler;
__attribute__((weak)) uint64_t kernel_time_monotonic_ns(void) { return 0; }
__attribute__((weak)) enum kernel_time_status kernel_time_deadline_from_monotonic(
    uint64_t target,uint64_t *deadline)
{ (void)target;(void)deadline;return KERNEL_TIME_STATUS_NOT_INITIALIZED; }
__attribute__((weak)) uint64_t riscv_time_read(void) { return 0; }
/* 宿主等待器直接发布 pthread 可见状态，不模拟硬件 IPI 排队。
 * 有专用 CPU 模型的夹具覆盖这两个平台入口。 */
__attribute__((weak)) void kernel_assert_service_context(void)
{ assert(!kernel_cpu_current()->logical_id); }
__attribute__((weak)) void kernel_cpu_flush_notifications(void)
{ assert(!kernel_cpu_current()->outbound); }
__attribute__((weak)) void kernel_cpu_notify(struct kernel_cpu *cpu, unsigned events)
{ __atomic_fetch_or(&cpu->events, events, __ATOMIC_RELEASE); }
#endif
