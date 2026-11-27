#include <kernel/cost.h>
#include <kernel/cpu.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/trap.h>

#include "timer_internal.h"

#include <stddef.h>
#include <stdint.h>

#define SBI_EXT_TIME 0x54494d45UL
#define RISCV_SIE_STIE (1UL << 5)

struct riscv_timer_state {
    uint64_t period;
    uint64_t next_deadline;
    int armed, direct;
};

#if BOAROS_MAX_CPUS > 1
#define timer_state (kernel_cpu_current()->timer)
#else
static struct riscv_timer_state timer_state;
#endif

/* Probe actual supervisor access, including firmware's counter enables.
 * The trap fixup recognizes only this one instruction. Timer IRQs remain
 * disabled until its first deadline and backend have both been published. */
extern const char riscv_timer_compare_probe[], riscv_timer_compare_continue[];

static __attribute__((noinline,noclone)) int riscv_timer_try_compare(uint64_t deadline)
{
    unsigned direct;
    asm volatile("li %0, 0\n"
        ".globl riscv_timer_compare_probe\n"
        "riscv_timer_compare_probe:\n"
        "csrw 0x14d, %1\n"
        "li %0, 1\n"
        ".globl riscv_timer_compare_continue\n"
        "riscv_timer_compare_continue:"
        : "=&r"(direct) : "r"(deadline) : "memory");
    return direct;
}

int riscv_timer_probe_fault(struct riscv_trap_frame *frame)
{
    if (frame->scause != 2 || frame->sepc != (uintptr_t)riscv_timer_compare_probe)
        return 0;
    frame->sepc = (uintptr_t)riscv_timer_compare_continue;
    return 1;
}

static uint64_t compare_deadline(uint64_t deadline)
{
#if BOAROS_RISCV_TIMER_TEST
    return riscv_smp_test_timer_compare_deadline(deadline);
#else
    return deadline;
#endif
}

static long program_deadline(uint64_t deadline)
{
    deadline = compare_deadline(deadline);
    if (!timer_state.direct) return sbi_set_timer(deadline);
    asm volatile("csrw 0x14d, %0" : : "r"(deadline) : "memory");
    return 0;
}

uint64_t riscv_time_read_ordered(void)
{
    uint64_t value;
    /* Zicsr将TIME读归为I输入；普通acquire/release内存屏障不覆盖它。
     * 奇数seq发布/读payload先于采样，采样先于最终seq读取。 */
    asm volatile("fence rw, i\n\tcsrr %0, time\n\tfence i, r"
                 : "=r"(value) : : "memory");
    return value;
}

enum riscv_timer_status riscv_timer_deadline_advance(
    uint64_t now,
    uint64_t period,
    uint64_t previous_deadline,
    uint64_t *next_deadline,
    uint64_t *elapsed_ticks)
{
    uint64_t delta;
    uint64_t computed_elapsed;
    uint64_t computed_next;

    if (period == 0U || next_deadline == NULL || elapsed_ticks == NULL) {
        return RISCV_TIMER_STATUS_INVALID_ARGUMENT;
    }

    delta = now - previous_deadline;
    if ((delta >> 63) != 0U) {
        return RISCV_TIMER_STATUS_EARLY_INTERRUPT;
    }

    computed_elapsed = delta / period + 1U;
    computed_next = previous_deadline + computed_elapsed * period;
    *next_deadline = computed_next;
    *elapsed_ticks = computed_elapsed;
    return RISCV_TIMER_STATUS_OK;
}

enum riscv_timer_status riscv_timer_start(uint32_t timebase_frequency,
                                          uint32_t ticks_per_second)
{
    uint64_t period;
    uint64_t deadline;
    unsigned long stie = RISCV_SIE_STIE;

    if (timebase_frequency == 0U || ticks_per_second == 0U ||
        timebase_frequency < ticks_per_second) {
        return RISCV_TIMER_STATUS_INVALID_FREQUENCY;
    }
    if (timer_state.armed) {
        return RISCV_TIMER_STATUS_ALREADY_STARTED;
    }

    period = timebase_frequency / ticks_per_second;
    deadline = riscv_time_read() + period;
    timer_state.direct = riscv_timer_try_compare(compare_deadline(deadline));
    if (!timer_state.direct) {
        long probe_result = sbi_probe_extension(SBI_EXT_TIME);
        if (probe_result < 0L) {
            return RISCV_TIMER_STATUS_SBI_PROBE_FAILED;
        }
        if (probe_result == 0L) {
            return RISCV_TIMER_STATUS_SBI_TIME_UNAVAILABLE;
        }
        deadline = riscv_time_read() + period;
        if (program_deadline(deadline) != 0L) {
            return RISCV_TIMER_STATUS_SBI_SET_FAILED;
        }
    }

    timer_state.period = period;
    timer_state.next_deadline = deadline;
    timer_state.armed = 1;
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_irq_enable_now();
#endif
    asm volatile("csrs sie, %0\n"
                 "csrsi sstatus, 2"
                 :
                 : "r"(stie)
                 : "memory");
    return RISCV_TIMER_STATUS_OK;
}

enum riscv_timer_status riscv_timer_stop(void)
{
    if (!timer_state.armed) return RISCV_TIMER_STATUS_NOT_STARTED;
    if (program_deadline(UINT64_MAX)) return RISCV_TIMER_STATUS_SBI_SET_FAILED;
    kernel_cpu_current()->timer_deadline = 0;
    timer_state.armed = 0;
    return RISCV_TIMER_STATUS_OK;
}

enum riscv_timer_status riscv_timer_event_advance(uint64_t now, uint64_t period,
    uint64_t tick_deadline,
    uint64_t *next_tick, uint64_t *elapsed_ticks)
{
    enum riscv_timer_status status = riscv_timer_deadline_advance(now, period,
        tick_deadline, next_tick, elapsed_ticks);
    /* STIP may remain pending after a deadline is cancelled or replaced.
     * Keep the periodic phase and let the IRQ caller rearm the current event. */
    if (status == RISCV_TIMER_STATUS_EARLY_INTERRUPT) {
        *next_tick = tick_deadline;
        *elapsed_ticks = 0;
        return RISCV_TIMER_STATUS_OK;
    }
    return status;
}

static uint64_t earlier_deadline(uint64_t periodic, uint64_t event)
{
    return event && (int64_t)(event - periodic) < 0 ? event : periodic;
}

#if BOAROS_RISCV_TIMER_TEST
/* The fixture moves the comparator and its software periodic value together. */
enum riscv_timer_status riscv_timer_test_set_periodic_deadline(uint64_t deadline)
{
    if (!timer_state.armed) return RISCV_TIMER_STATUS_NOT_STARTED;
    uint64_t event = kernel_cpu_current()->timer_deadline;
    if (program_deadline(earlier_deadline(deadline, event)))
        return RISCV_TIMER_STATUS_SBI_SET_FAILED;
    timer_state.next_deadline = deadline;
    return RISCV_TIMER_STATUS_OK;
}
#endif

enum riscv_timer_status riscv_timer_set_scheduler_deadline(uint64_t deadline)
{
    if (!timer_state.armed) return RISCV_TIMER_STATUS_NOT_STARTED;
    uintptr_t irq = arch_interrupt_save();
    struct kernel_cpu *cpu = kernel_cpu_current();
    enum riscv_timer_status status = RISCV_TIMER_STATUS_OK;
    /* 周期 IRQ 重新装载硬件。软件期限变化时，只有实际比较值改变才进入固件。 */
    if (cpu->timer_deadline != deadline) {
        uint64_t periodic = timer_state.next_deadline;
        uint64_t target = earlier_deadline(periodic, deadline);
        if (target != earlier_deadline(periodic, cpu->timer_deadline) && program_deadline(target) != 0L)
            status = RISCV_TIMER_STATUS_SBI_SET_FAILED;
        else cpu->timer_deadline = deadline;
    }
    arch_interrupt_restore(irq);
    return status;
}

enum riscv_timer_status riscv_timer_handle_interrupt(uint64_t *elapsed_ticks)
{
    uint64_t candidate_deadline, candidate_elapsed;
    uint64_t event = kernel_cpu_current()->timer_deadline;
    if (!elapsed_ticks) return RISCV_TIMER_STATUS_INVALID_ARGUMENT;
    if (!timer_state.armed) return RISCV_TIMER_STATUS_NOT_STARTED;
    uint64_t now = riscv_time_read();
    enum riscv_timer_status status = riscv_timer_event_advance(now,
        timer_state.period, timer_state.next_deadline,
        &candidate_deadline, &candidate_elapsed);
    if (status != RISCV_TIMER_STATUS_OK) return status;
    if (event && (int64_t)(now - event) >= 0) event = 0;
    if (program_deadline(earlier_deadline(candidate_deadline, event)) != 0L)
        return RISCV_TIMER_STATUS_SBI_SET_FAILED;
    timer_state.next_deadline = candidate_deadline;
    kernel_cpu_current()->timer_deadline = event;
    *elapsed_ticks = candidate_elapsed;
    return RISCV_TIMER_STATUS_OK;
}
