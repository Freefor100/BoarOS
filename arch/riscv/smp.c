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


static struct kernel_cpu *cpus[BOAROS_MAX_CPUS];
static unsigned cpu_count;
static uint32_t timebase;

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

void riscv_smp_trap_fatal(const struct riscv_trap_frame *frame,
                          const char *source, unsigned long status)
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
    virt_uart_puts(" source=");
    virt_uart_puts(source);
    virt_uart_puts(" status=");
    virt_uart_put_hex(status);
    virt_uart_putc('\n');
    sbi_shutdown();
}
/* 前五项是 secondary.S 的物理交接格式，发布后由本核独占读取。 */
struct secondary_launch {
    uint64_t stack, transition, final, thread, offset;
};
_Static_assert(offsetof(struct secondary_launch, offset) == 32, "secondary mailbox layout");
struct secondary_state {
    struct kernel_cpu cpu;
    struct secondary_launch launch;
    unsigned ping_seen, timer_seen, stop_requested, attached;
    unsigned char stack[16384] __attribute__((aligned(4096)));
};
/* 邮箱、idle 和栈由镜像常驻持有，HSM 返回不结束远核的借用。 */
static struct secondary_state secondaries[BOAROS_MAX_CPUS - 1];
extern void riscv_secondary_entry(void);

static struct secondary_state *current_secondary(void)
{
    unsigned logical = kernel_cpu_current()->logical_id;
    if (!logical || logical >= cpu_count) __builtin_trap();
    struct secondary_state *state = &secondaries[logical - 1];
    if (&state->cpu != kernel_cpu_current()) __builtin_trap();
    return state;
}

static void fail_hart(const char *stage, long error, uint64_t target)
    __attribute__((noreturn));
static void fail_hart(const char *stage, long error, uint64_t target)
{
    claim_fatal();
    virt_uart_puts("BoarOS: SMP failure stage=");
    virt_uart_puts(stage);
    virt_uart_puts(" hart=");
    virt_uart_put_hex(kernel_cpu_current()->hardware_id);
    virt_uart_puts(" target=");
    virt_uart_put_hex(target);
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

void riscv_smp_fail(const char *stage, long error)
{ fail_hart(stage, error, kernel_cpu_current()->hardware_id); }

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
    int fresh=kernel_cpu_publish_events(target,events);
    if (target != source) {
        COST_ADD(SCHED_IPI_REQUESTS, 1);
        if(!fresh) {COST_ADD(SCHED_IPI_COALESCED,1);}
        else {
            unsigned bit = 1U << target->logical_id;
            if (source->notification_batch) source->notification_batch->targets |= bit;
            else source->outbound |= bit;
        }
    }
    if (!source->raw_locks && source->outbound) kernel_cpu_flush_notifications();
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
#if BOAROS_COST_DIAGNOSTICS
        uint64_t started = kernel_cost_clock();
#endif
        long error = sbi_send_ipi(cpus[i]->hardware_id);
#if BOAROS_COST_DIAGNOSTICS
        uint64_t elapsed = kernel_cost_clock() - started;
        COST_ADD(SCHED_IPI_SENDS, 1);
        COST_ADD(SCHED_IPI_TICKS, elapsed);
#endif
        if (error) fail_hart("IPI", error, cpus[i]->hardware_id);
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
    if ((events & RISCV_IPI_HANDSHAKE) && cpu->logical_id)
        __atomic_store_n(&current_secondary()->ping_seen, 1, __ATOMIC_RELEASE);
    if (events & RISCV_IPI_STOP)
        __atomic_store_n(&current_secondary()->stop_requested, 1, __ATOMIC_RELEASE);
    if (events & RISCV_IPI_DEADLINE)
        kernel_scheduler_deadline_changed();
}
void riscv_smp_handle_ipi(void) { consume_ipi(1); }

void riscv_smp_timer_seen(void)
{ if (kernel_cpu_current()->logical_id)
    __atomic_store_n(&current_secondary()->timer_seen, 1, __ATOMIC_RELEASE); }

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
    struct secondary_state *state = current_secondary();
    struct kernel_cpu *cpu = &state->cpu;
    if (riscv_sv39_switch_satp(state->launch.final)) riscv_smp_fail("secondary-satp", -1);
    kernel_cpu_secondary_attach(cpu, (uintptr_t)state->stack,
                               (uintptr_t)state->stack + sizeof(state->stack));
    uintptr_t mask = 0x6000U, clean = 0x4000U;
    asm volatile("csrc sstatus, %0; csrs sstatus, %1; csrsi sie, 2"
                 :: "r"(mask), "r"(clean) : "memory");
    enum riscv_timer_status status = riscv_timer_start(timebase, KERNEL_TICKS_PER_SECOND);
    if (status) riscv_smp_fail("secondary-timer", status);
    __atomic_store_n(&state->attached, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&state->ping_seen, __ATOMIC_ACQUIRE) ||
           !__atomic_load_n(&state->timer_seen, __ATOMIC_ACQUIRE)) riscv_smp_idle();
    __atomic_store_n(&cpu->state, KERNEL_CPU_ONLINE, __ATOMIC_RELEASE);
    for (;;) {
        riscv_smp_idle();
        if (__atomic_load_n(&state->stop_requested, __ATOMIC_ACQUIRE)) {
            (void)arch_interrupt_save();
            if (cpu->current != cpu->idle || cpu->switch_previous ||
                cpu->raw_locks || cpu->preempt_depth)
                riscv_smp_fail("stop-owner", -1);
            __atomic_store_n(&cpu->state, KERNEL_CPU_QUIESCING, __ATOMIC_RELEASE);
            asm volatile("csrw sie, zero" ::: "memory");
            (void)sbi_set_timer(UINT64_MAX);
            riscv_smp_fail("HSM-stop-returned", sbi_hart_stop());
        }
    }
}

void riscv_smp_start(const struct dtb_cpu_info *info, uint64_t transition,
                     uint64_t final, uint32_t frequency)
{
    if (!info || !info->count || info->count > BOAROS_MAX_CPUS ||
        info->count != kernel_cpu_get_boot_count() || cpu_count ||
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
    for (unsigned i = 1; i < cpu_count; i++) {
        struct secondary_state *state = &secondaries[i - 1];
        cpus[i] = &state->cpu;
        struct kernel_task *thread = kernel_cpu_secondary_prepare(cpus[i], info->hart_ids[i], i);
        state->launch = (struct secondary_launch){
            .stack = image_physical(state->stack, sizeof(state->stack)) + sizeof(state->stack),
            .transition = transition, .final = final,
            .thread = image_physical(thread, 1),
            .offset = (uintptr_t)state->stack - image_physical(state->stack, 1)
        };
        cpus[i]->state = KERNEL_CPU_PREPARED;
    }
    for (unsigned i = 1; i < cpu_count; i++) {
        struct secondary_state *state = &secondaries[i - 1];
        __atomic_store_n(&cpus[i]->state, KERNEL_CPU_STARTING, __ATOMIC_RELEASE);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        long error = sbi_hart_start(cpus[i]->hardware_id, image_physical(riscv_secondary_entry, 4),
                                   image_physical(&state->launch, sizeof(state->launch)));
        if (error) fail_hart("HSM-start", error, cpus[i]->hardware_id);
        uint64_t end = riscv_time_read() + (uint64_t)frequency * 5;
        /* OpenSBI 的 IPI 目标只含已启动 hart，先等本核真实 attach。 */
        while (!__atomic_load_n(&state->attached, __ATOMIC_ACQUIRE))
            if ((int64_t)(riscv_time_read() - end) >= 0)
                fail_hart("attach-timeout", sbi_hart_status(cpus[i]->hardware_id), cpus[i]->hardware_id);
        kernel_cpu_notify(cpus[i], RISCV_IPI_HANDSHAKE);
        while (__atomic_load_n(&cpus[i]->state, __ATOMIC_ACQUIRE) != KERNEL_CPU_ONLINE)
            if ((int64_t)(riscv_time_read() - end) >= 0)
                fail_hart("start-timeout", sbi_hart_status(cpus[i]->hardware_id), cpus[i]->hardware_id);
        virt_uart_puts("BoarOS: SMP online logical=");
        virt_uart_putc('0' + i); virt_uart_puts(" hart=");
        virt_uart_put_hex(cpus[i]->hardware_id); virt_uart_putc('\n');
    }
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
#if BOAROS_COST_DIAGNOSTICS
    virt_uart_puts("BoarOS: USER SMP service migrations="); virt_uart_put_hex(cpus[0]->service_migrations);
    virt_uart_puts(" queue-ticks="); virt_uart_put_hex(cpus[0]->service_queue_ticks);
    virt_uart_puts(" samples="); virt_uart_put_hex(cpus[0]->service_samples);
    virt_uart_puts(" elapsed-ticks="); virt_uart_put_hex(cpus[0]->service_elapsed_ticks);
    virt_uart_puts(" running-ns="); virt_uart_put_hex(cpus[0]->service_running_ns);
    virt_uart_putc('\n');
#endif
#endif
}

void riscv_smp_stop(void)
{
    if (cpu_count < 2) return;
    if (kernel_cpu_current()->logical_id) __builtin_trap();
    kernel_preempt_disable();
    /* 先核对全体使用者，避免停下一核后才发现另一核仍持活动资源。 */
    for (unsigned i = 1; i < cpu_count; i++) {
        if (__atomic_load_n(&cpus[i]->state, __ATOMIC_ACQUIRE) == KERNEL_CPU_STOPPED) continue;
        if (!kernel_scheduler_core_quiescent(cpus[i]))
            fail_hart("stop-live-owner", -1, cpus[i]->hardware_id);
    }
    for (unsigned i = 1; i < cpu_count; i++) {
        if (__atomic_load_n(&cpus[i]->state, __ATOMIC_ACQUIRE) == KERNEL_CPU_STOPPED) continue;
        kernel_cpu_notify(cpus[i], RISCV_IPI_STOP);
        uint64_t end = riscv_time_read() + (uint64_t)timebase * 5;
        long state;
        while ((state = sbi_hart_status(cpus[i]->hardware_id)) != SBI_HSM_STOPPED)
            if (state < 0 || (int64_t)(riscv_time_read() - end) >= 0)
                fail_hart("stop-confirmation", state, cpus[i]->hardware_id);
        if (__atomic_load_n(&cpus[i]->state, __ATOMIC_ACQUIRE) != KERNEL_CPU_QUIESCING)
            fail_hart("stop-kernel-state", cpus[i]->state, cpus[i]->hardware_id);
        __atomic_store_n(&cpus[i]->state, KERNEL_CPU_STOPPED, __ATOMIC_RELEASE);
    }
    kernel_preempt_enable();
}

void riscv_smp_sync_kernel(uint64_t address, uint64_t size)
{
    if (kernel_cpu_current()->logical_id || kernel_cpu_current()->raw_locks) __builtin_trap();
    asm volatile("sfence.vma" ::: "memory");
    for (unsigned i = 1; i < cpu_count; i++) {
        if (!kernel_cpu_online(cpus[i])) continue;
        long error = sbi_remote_sfence(cpus[i]->hardware_id, address, size);
        if (error) fail_hart("RFENCE", error, cpus[i]->hardware_id);
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
        if (error) fail_hart("MM-RFENCE", error, cpus[i]->hardware_id);
        local->rfence_requests++;
        if (instructions) {
            error = sbi_remote_fence_i(cpus[i]->hardware_id);
            if (error) fail_hart("MM-FENCE.I", error, cpus[i]->hardware_id);
        }
    }
}
#endif
