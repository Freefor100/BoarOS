#include <arch/riscv/smp.h>
#if BOAROS_MAX_CPUS > 1
#include <arch/riscv/direct_map.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/scheduler.h>
#include <kernel/time.h>
#include <kernel/tick.h>
#include <stddef.h>

static struct kernel_cpu secondary_cpu;
static struct kernel_cpu *cpus[BOAROS_MAX_CPUS];
static unsigned cpu_count;
static uint32_t timebase;
static unsigned ping_seen, timer_seen, stop_requested, secondary_attached;
static unsigned char secondary_stack[16384] __attribute__((aligned(4096)));
/* 前五项是secondary.S的物理交接格式，mailbox与栈由镜像owner常驻持有。 */
static struct {
    uint64_t stack, transition, final, thread, offset;
} launch;
_Static_assert(offsetof(__typeof__(launch), offset) == 32, "secondary mailbox layout");
extern void riscv_secondary_entry(void);

void riscv_smp_fail(const char *stage, long error)
{
    (void)arch_interrupt_save();
    virt_uart_emergency_begin();
    virt_uart_puts("BoarOS: SMP failure stage=");
    virt_uart_puts(stage);
    virt_uart_puts(" hart=");
    virt_uart_put_hex(kernel_cpu_current()->hardware_id);
    virt_uart_puts(" error=");
    virt_uart_put_hex((unsigned long)error);
    virt_uart_putc('\n');
    sbi_shutdown();
}

static uint64_t image_physical(const void *p, uint64_t size)
{
    uint64_t address;
    if (riscv_image_va_to_pa((uintptr_t)p, size, &address))
        riscv_smp_fail("image-address", -1);
    return address;
}

struct kernel_cpu *riscv_smp_cpu(unsigned logical)
{ return logical < cpu_count ? cpus[logical] : 0; }

void kernel_cpu_notify(struct kernel_cpu *target, unsigned events)
{
    struct kernel_cpu *source = kernel_cpu_current();
    if (!cpu_count && target == source) return;
    if (!target || target->logical_id >= cpu_count) __builtin_trap();
    __atomic_fetch_or(&target->events, events, __ATOMIC_RELEASE);
    if (target != source) source->outbound |= 1U << target->logical_id;
    if (!source->raw_locks) kernel_cpu_flush_notifications();
}

void kernel_cpu_flush_notifications(void)
{
    struct kernel_cpu *source = kernel_cpu_current();
    if (source->raw_locks) __builtin_trap();
    unsigned mask = source->outbound;
    source->outbound = 0;
    for (unsigned i = 0; i < cpu_count; i++) {
        if (!(mask & (1U << i))) continue;
        long error = sbi_send_ipi(cpus[i]->hardware_id);
        if (error) riscv_smp_fail("IPI", error);
    }
}

void riscv_smp_handle_ipi(void)
{
    /* 先确认SSIP，后消费软件事件；确认后的新通知不会被清除。 */
    asm volatile("csrci sip, 2" ::: "memory");
    struct kernel_cpu *cpu = kernel_cpu_current();
    unsigned events = __atomic_exchange_n(&cpu->events, 0, __ATOMIC_ACQ_REL);
    if (events & RISCV_IPI_SCHEDULE) kernel_cpu_request_schedule(cpu);
    if (events & RISCV_IPI_HANDSHAKE) ping_seen = 1;
    if (events & RISCV_IPI_STOP) stop_requested = 1;
    if (events & RISCV_IPI_DEADLINE)
        kernel_scheduler_deadline_changed();
}

void riscv_smp_timer_seen(void)
{ if (kernel_cpu_current()->logical_id) timer_seen = 1; }

void riscv_smp_idle(void)
{
    uintptr_t irq = arch_interrupt_save();
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (__atomic_load_n(&cpu->events, __ATOMIC_ACQUIRE)) riscv_smp_handle_ipi();
    kernel_scheduler_prepare_idle_return();
    /* IRQ关闭下的复查与WFI：pending SSIP即使不陷入，也能唤醒WFI。 */
    if (!kernel_cpu_schedule_requested(cpu) &&
        !__atomic_load_n(&cpu->events, __ATOMIC_ACQUIRE)) asm volatile("wfi" ::: "memory");
    arch_interrupt_restore(irq | 2U);
}

void riscv_smp_secondary_high(void)
{
    if (riscv_sv39_switch_satp(launch.final)) riscv_smp_fail("secondary-satp", -1);
    kernel_cpu_secondary_attach(&secondary_cpu, (uintptr_t)secondary_stack,
                               (uintptr_t)secondary_stack + sizeof(secondary_stack));
    uintptr_t mask = 0x6000U, clean = 0x4000U;
    asm volatile("csrc sstatus, %0; csrs sstatus, %1; csrsi sie, 2"
                 :: "r"(mask), "r"(clean) : "memory");
    enum riscv_timer_status status = riscv_timer_start(timebase, KERNEL_TICKS_PER_SECOND);
    if (status) riscv_smp_fail("secondary-timer", status);
    __atomic_store_n(&secondary_attached, 1, __ATOMIC_RELEASE);
    while (!ping_seen || !timer_seen) riscv_smp_idle();
    __atomic_store_n(&secondary_cpu.state, KERNEL_CPU_ONLINE, __ATOMIC_RELEASE);
    for (;;) {
        riscv_smp_idle();
        if (stop_requested) {
            (void)arch_interrupt_save();
            if (secondary_cpu.current != secondary_cpu.idle || secondary_cpu.switch_previous ||
                secondary_cpu.raw_locks || secondary_cpu.preempt_depth)
                riscv_smp_fail("stop-owner", -1);
            __atomic_store_n(&secondary_cpu.state, KERNEL_CPU_QUIESCING, __ATOMIC_RELEASE);
            asm volatile("csrw sie, zero" ::: "memory");
            (void)sbi_set_timer(UINT64_MAX);
            riscv_smp_fail("HSM-stop-returned", sbi_hart_stop());
        }
    }
}

void riscv_smp_start(const struct dtb_cpu_info *info, uint64_t transition,
                     uint64_t final, uint32_t frequency)
{
    if (!info || !info->count || info->count > BOAROS_MAX_CPUS || cpu_count ||
        !frequency || info->hart_ids[0] != kernel_cpu_current()->hardware_id)
        riscv_smp_fail("CPU-input", -1);
    cpus[0] = kernel_cpu_current();
    cpu_count = info->count;
    timebase = frequency;
    cpus[0]->logical_id = 0;
    cpus[0]->state = KERNEL_CPU_ONLINE;
    asm volatile("csrsi sie, 2" ::: "memory");
    if (cpu_count == 1) return;
    const unsigned long required[] = { SBI_EXT_HSM, SBI_EXT_IPI, SBI_EXT_RFENCE };
    for (unsigned i = 0; i < 3; i++)
        if (sbi_probe_extension(required[i]) <= 0) riscv_smp_fail("SBI-extension", required[i]);
    cpus[1] = &secondary_cpu;
    struct kernel_task *thread = kernel_cpu_secondary_prepare(cpus[1], info->hart_ids[1], 1);
    launch.stack = image_physical(secondary_stack, sizeof(secondary_stack)) + sizeof(secondary_stack);
    launch.transition = transition;
    launch.final = final;
    launch.thread = image_physical(thread, 1);
    launch.offset = (uintptr_t)secondary_stack - image_physical(secondary_stack, 1);
    cpus[1]->state = KERNEL_CPU_PREPARED;
    __atomic_store_n(&cpus[1]->state, KERNEL_CPU_STARTING, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    long error = sbi_hart_start(cpus[1]->hardware_id, image_physical(riscv_secondary_entry, 4),
                               image_physical(&launch, sizeof(launch)));
    if (error) riscv_smp_fail("HSM-start", error);
    uint64_t end = riscv_time_read() + (uint64_t)frequency * 5;
    /* OpenSBI的IPI目标集合只含已启动hart；HSM start返回时可能仍pending。 */
    while (!__atomic_load_n(&secondary_attached, __ATOMIC_ACQUIRE))
        if ((int64_t)(riscv_time_read() - end) >= 0)
            riscv_smp_fail("attach-timeout", sbi_hart_status(cpus[1]->hardware_id));
    kernel_cpu_notify(cpus[1], RISCV_IPI_HANDSHAKE);
    while (__atomic_load_n(&cpus[1]->state, __ATOMIC_ACQUIRE) != KERNEL_CPU_ONLINE)
        if ((int64_t)(riscv_time_read() - end) >= 0)
            riscv_smp_fail("start-timeout", sbi_hart_status(cpus[1]->hardware_id));
    virt_uart_puts("BoarOS: SMP online logical=1 hart=");
    virt_uart_put_hex(cpus[1]->hardware_id);
    virt_uart_putc('\n');
}

void riscv_smp_stop(void)
{
    if (cpu_count < 2 || cpus[1]->state == KERNEL_CPU_STOPPED) return;
    if (kernel_cpu_current()->logical_id) __builtin_trap();
    kernel_preempt_disable();
    if (!kernel_scheduler_core_quiescent(cpus[1])) riscv_smp_fail("stop-live-owner", -1);
    kernel_cpu_notify(cpus[1], RISCV_IPI_STOP);
    uint64_t end = riscv_time_read() + (uint64_t)timebase * 5;
    long state;
    while ((state = sbi_hart_status(cpus[1]->hardware_id)) != SBI_HSM_STOPPED)
        if (state < 0 || (int64_t)(riscv_time_read() - end) >= 0)
            riscv_smp_fail("stop-confirmation", state);
    __atomic_store_n(&cpus[1]->state, KERNEL_CPU_STOPPED, __ATOMIC_RELEASE);
    kernel_preempt_enable();
}

void riscv_smp_sync_kernel(uint64_t address, uint64_t size)
{
    if (kernel_cpu_current()->logical_id || kernel_cpu_current()->raw_locks) __builtin_trap();
    asm volatile("sfence.vma" ::: "memory");
    if (cpu_count > 1 && __atomic_load_n(&cpus[1]->state, __ATOMIC_ACQUIRE) == KERNEL_CPU_ONLINE) {
        long error = sbi_remote_sfence(cpus[1]->hardware_id, address, size);
        if (error) riscv_smp_fail("RFENCE", error);
    }
}
#endif
