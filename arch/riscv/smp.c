#include <arch/riscv/smp.h>
#if BOAROS_MAX_CPUS > 1
#include <arch/riscv/direct_map.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/mmu_context.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/trap.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/scheduler.h>
#include <kernel/time.h>
#include <kernel/tick.h>
#include <kernel/page.h>
#include <stddef.h>

static struct kernel_cpu secondary_cpu;
static struct kernel_cpu *cpus[BOAROS_MAX_CPUS];
static unsigned cpu_count;
static uint32_t timebase;
static unsigned ping_seen, timer_seen, stop_requested, secondary_attached;
static uint32_t fatal_claim;
static uint64_t fatal_hart;

int riscv_smp_output_allowed(void)
{
    unsigned state = __atomic_load_n(&fatal_claim, __ATOMIC_ACQUIRE);
    return !state || (state == 2 && kernel_cpu_current()->hardware_id == fatal_hart);
}

static void claim_fatal(void)
{
    (void)arch_interrupt_save();
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&fatal_claim, &expected, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        for (;;) asm volatile("wfi" ::: "memory");
    fatal_hart = kernel_cpu_current()->hardware_id;
    __atomic_store_n(&fatal_claim, 2, __ATOMIC_RELEASE);
    virt_uart_emergency_begin();
}

void riscv_smp_trap_fatal(const struct riscv_trap_frame *frame)
{
    claim_fatal();
    virt_uart_puts("BoarOS: SMP fatal hart=");
    virt_uart_put_hex(kernel_cpu_current()->hardware_id);
    virt_uart_puts(" cause=");
    virt_uart_put_hex(frame->scause);
    virt_uart_puts(" pc=");
    virt_uart_put_hex(frame->sepc);
    virt_uart_puts(" sp=");
    virt_uart_put_hex(frame->sp);
    virt_uart_puts(" stval=");
    virt_uart_put_hex(frame->stval);
    virt_uart_putc('\n');
    sbi_shutdown();
}
static unsigned char secondary_stack[16384] __attribute__((aligned(4096)));
/* 前五项是secondary.S的物理交接格式，mailbox与栈由镜像owner常驻持有。 */
static struct {
    uint64_t stack, transition, final, thread, offset;
} launch;
_Static_assert(offsetof(__typeof__(launch), offset) == 32, "secondary mailbox layout");
extern void riscv_secondary_entry(void);

void riscv_smp_fail(const char *stage, long error)
{
    claim_fatal();
    virt_uart_puts("BoarOS: SMP failure stage=");
    virt_uart_puts(stage);
    virt_uart_puts(" hart=");
    virt_uart_put_hex(kernel_cpu_current()->hardware_id);
    virt_uart_puts(" error=");
    virt_uart_put_hex((unsigned long)error);
    virt_uart_puts(" sp=");
    virt_uart_put_hex(arch_current_stack());
    uintptr_t pc;
    asm volatile("auipc %0, 0" : "=r"(pc));
    virt_uart_puts(" pc=");
    virt_uart_put_hex(pc);
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
    uintptr_t irq = arch_interrupt_save();
    struct kernel_cpu *source = kernel_cpu_current();
    if (!cpu_count && target == source) { arch_interrupt_restore(irq); return; }
    if (!target || target->logical_id >= cpu_count) __builtin_trap();
    __atomic_fetch_or(&target->events, events, __ATOMIC_RELEASE);
    if (target != source) source->outbound |= 1U << target->logical_id;
    if (!source->raw_locks) kernel_cpu_flush_notifications();
    arch_interrupt_restore(irq);
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
        source->ipi_sent++;
    }
}

static void consume_ipi(int interrupt)
{
    /* 先确认SSIP，后消费软件事件；确认后的新通知不会被清除。 */
    asm volatile("csrci sip, 2" ::: "memory");
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (interrupt) cpu->ipi_received++;
    unsigned events = __atomic_exchange_n(&cpu->events, 0, __ATOMIC_ACQ_REL);
    if (events & RISCV_IPI_SCHEDULE) kernel_cpu_request_schedule(cpu);
    if (events & RISCV_IPI_HANDSHAKE) __atomic_store_n(&ping_seen, 1, __ATOMIC_RELEASE);
    if (events & RISCV_IPI_STOP) __atomic_store_n(&stop_requested, 1, __ATOMIC_RELEASE);
    if (events & RISCV_IPI_DEADLINE)
        kernel_scheduler_deadline_changed();
}
void riscv_smp_handle_ipi(void) { consume_ipi(1); }

void riscv_smp_timer_seen(void)
{ if (kernel_cpu_current()->logical_id) __atomic_store_n(&timer_seen, 1, __ATOMIC_RELEASE); }

void riscv_smp_idle(void)
{
    uintptr_t irq = arch_interrupt_save();
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (__atomic_load_n(&cpu->events, __ATOMIC_ACQUIRE)) consume_ipi(0);
    kernel_scheduler_prepare_idle_return();
    /* IRQ关闭下的复查与WFI：pending SSIP即使不陷入，也能唤醒WFI。 */
    if (!kernel_cpu_schedule_requested(cpu) &&
        !__atomic_load_n(&cpu->events, __ATOMIC_ACQUIRE)) {
#if BOAROS_SMP_TEST
        extern void riscv_smp_test_idle_boundary(struct kernel_cpu *);
        riscv_smp_test_idle_boundary(cpu);
#endif
        asm volatile("wfi" ::: "memory");
    }
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
    while (!__atomic_load_n(&ping_seen, __ATOMIC_ACQUIRE) ||
           !__atomic_load_n(&timer_seen, __ATOMIC_ACQUIRE)) riscv_smp_idle();
    __atomic_store_n(&secondary_cpu.state, KERNEL_CPU_ONLINE, __ATOMIC_RELEASE);
    for (;;) {
        riscv_smp_idle();
        if (__atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE)) {
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

void riscv_smp_report_user_execution(void)
{
#if BOAROS_USER_SMP
    for (unsigned i=0; i<cpu_count; i++) {
        virt_uart_puts("BoarOS: USER SMP hart="); virt_uart_put_hex(cpus[i]->hardware_id);
        virt_uart_puts(" traps="); virt_uart_put_hex(cpus[i]->user_traps);
        virt_uart_puts(" returns="); virt_uart_put_hex(cpus[i]->user_returns);
        virt_uart_puts(" local-irqs="); virt_uart_put_hex(cpus[i]->user_local_interrupts);
        virt_uart_puts(" syscalls="); virt_uart_put_hex(cpus[i]->syscall_dispatches);
        virt_uart_putc('\n');
    }
    virt_uart_puts("BoarOS: USER SMP service migrations="); virt_uart_put_hex(cpus[0]->service_migrations);
    virt_uart_puts(" queue-ticks="); virt_uart_put_hex(cpus[0]->service_queue_ticks);
    virt_uart_puts(" samples="); virt_uart_put_hex(cpus[0]->service_samples);
    virt_uart_puts(" elapsed-ticks="); virt_uart_put_hex(cpus[0]->service_elapsed_ticks);
    virt_uart_puts(" running-ns="); virt_uart_put_hex(cpus[0]->service_running_ns);
    virt_uart_putc('\n');
#endif
}

void riscv_smp_stop(void)
{
    if (cpu_count < 2) return;
    if (__atomic_load_n(&cpus[1]->state, __ATOMIC_ACQUIRE) == KERNEL_CPU_STOPPED) return;
    if (kernel_cpu_current()->logical_id) __builtin_trap();
    kernel_preempt_disable();
    if (!kernel_scheduler_core_quiescent(cpus[1])) riscv_smp_fail("stop-live-owner", -1);
    kernel_cpu_notify(cpus[1], RISCV_IPI_STOP);
    uint64_t end = riscv_time_read() + (uint64_t)timebase * 5;
    long state;
    while ((state = sbi_hart_status(cpus[1]->hardware_id)) != SBI_HSM_STOPPED)
        if (state < 0 || (int64_t)(riscv_time_read() - end) >= 0)
            riscv_smp_fail("stop-confirmation", state);
    if (__atomic_load_n(&cpus[1]->state, __ATOMIC_ACQUIRE) != KERNEL_CPU_QUIESCING)
        riscv_smp_fail("stop-kernel-state", cpus[1]->state);
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
        kernel_cpu_current()->rfence_requests++;
    }
}
void riscv_smp_sync_user(uint32_t targets, uint64_t address, uint64_t size,
    int instructions)
{
    struct kernel_cpu *local = kernel_cpu_current();
    if (local->raw_locks || (targets & ~((1U << BOAROS_MAX_CPUS) - 1U))) __builtin_trap();
    if (size == BOAROS_PAGE_SIZE) riscv_mmu_flush_address(address);
    else riscv_mmu_flush_all();
    if (instructions) riscv_mmu_sync_instructions();
    for (unsigned i = 0; i < cpu_count; i++) {
        if (!(targets & (1U << i)) || cpus[i] == local) continue;
        if (!kernel_cpu_online(cpus[i])) riscv_smp_fail("MM-inactive-target", -1);
        long error = sbi_remote_sfence(cpus[i]->hardware_id, address, size);
        if (error) riscv_smp_fail("MM-RFENCE", error);
        local->rfence_requests++;
        if (instructions) {
            error = sbi_remote_fence_i(cpus[i]->hardware_id);
            if (error) riscv_smp_fail("MM-FENCE.I", error);
        }
    }
}
#endif
