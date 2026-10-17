#include <kernel/cpu.h>
#include <assert.h>
#if BOAROS_MAX_CPUS > 1
/* 宿主等待器直接发布 pthread 可见状态，不模拟硬件 IPI 排队。
 * 有专用 CPU 模型的夹具覆盖这两个平台入口。 */
__attribute__((weak)) void kernel_assert_service_context(void)
{ assert(!kernel_cpu_current()->logical_id); }
__attribute__((weak)) void kernel_cpu_flush_notifications(void)
{ assert(!kernel_cpu_current()->outbound); }
__attribute__((weak)) void kernel_cpu_notify(struct kernel_cpu *cpu, unsigned events)
{ __atomic_fetch_or(&cpu->events, events, __ATOMIC_RELEASE); }
#endif
