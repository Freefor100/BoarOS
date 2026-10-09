#include <kernel/cpu.h>
#include <kernel/raw_lock.h>
#include <kernel/sync.h>
#include <kernel/task.h>
#if defined(BOAROS_ARCH_LOONGARCH)
#include <platform/loongarch_virt.h>
#define puts_native la_virt_puts
#define shutdown_native la_virt_shutdown
#else
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/sbi.h>
#define puts_native virt_uart_puts
#define shutdown_native sbi_shutdown
#endif

static void probe(void)
{
    struct kernel_cpu *cpu = kernel_cpu_current();
    (void)cpu;
    struct kernel_raw_lock lock, inner;
    kernel_raw_lock_init(&lock, KERNEL_RAW_RANK_HEAP);
    kernel_raw_lock_init(&inner, KERNEL_RAW_RANK_PAGE);
    uintptr_t irq = arch_interrupt_save();
    puts_native("SYNC native entered\n");
    {
        KERNEL_RAW_SCOPE(guard, &lock);
#if BOAROS_SYNC_CASE == 0
        { KERNEL_RAW_SCOPE(nested, &inner);
          if (cpu != kernel_cpu_current() || cpu->preempt_depth != 2 || arch_interrupt_is_enabled())
              __builtin_trap(); }
#elif BOAROS_SYNC_CASE == 1
        enum kernel_wait_wake_reason reason;
        (void)kernel_scheduler_block_current(0, 0, 0, &reason);
#elif BOAROS_SYNC_CASE == 2
        (void)kernel_scheduler_yield_current();
#elif BOAROS_SYNC_CASE == 3
        kernel_thread_exit();
#elif BOAROS_SYNC_CASE == 4
        struct kernel_mutex mutex; struct kernel_lock_guard sleeping = {0};
        kernel_mutex_init(&mutex, 1, 0);
        kernel_mutex_lock(&mutex, &sleeping);
#endif
    }
#if BOAROS_SYNC_CASE != 0
    puts_native("SYNC forbidden entry returned\n");
#else
    if (cpu->raw_locks || cpu->preempt_depth || arch_interrupt_is_enabled()) __builtin_trap();
    puts_native("SYNC native passed\n");
#endif
    arch_interrupt_restore(irq);
    shutdown_native();
}
#if defined(BOAROS_ARCH_LOONGARCH)
void __wrap_la_boot_tasks(struct physical_page_allocator *allocator)
{
    (void)allocator;
    uint64_t id; __asm__ volatile("csrrd %0, 0x20" : "=r"(id));
    if (kernel_cpu_current()->hardware_id != (id & 0x7ffU) || !kernel_task_current()) __builtin_trap();
    probe();
}
#else
void __wrap_kernel_main(unsigned long hart, const void *dtb)
{
    (void)dtb;
    if (kernel_cpu_current()->hardware_id != hart || kernel_task_current()) __builtin_trap();
    probe();
}
#endif
