#include <kernel/cpu.h>
/* 单核旧fixture也使用真实raw锁；只替换硬件CPU定位。 */
void *allocator_host_cpu_get(void)
{
    static _Thread_local struct kernel_cpu cpu = { .initialized = KERNEL_CPU_INITIALIZED };
    return &cpu;
}
