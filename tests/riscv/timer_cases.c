#include <arch/riscv/sbi.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/tick.h>

#include "../../arch/riscv/timer_internal.h"

#include <stddef.h>
#include <stdint.h>

#define SBI_EXT_TIME 0x54494d45UL
#define RISCV_SIE_STIE (1UL << 5)
#define RISCV_SSTATUS_SIE (1UL << 1)

long __real_sbi_probe_extension(unsigned long extension_id);
long __real_sbi_set_timer(uint64_t absolute_time);

static long wrapped_probe_result;
static long wrapped_set_result;
static unsigned long wrapped_probe_calls;
static unsigned long wrapped_set_calls;
static uint64_t wrapped_deadline;

long __wrap_sbi_probe_extension(unsigned long extension_id)
{
    if (extension_id != SBI_EXT_TIME) {
        return -1L;
    }

    wrapped_probe_calls++;
    return wrapped_probe_result;
}

long __wrap_sbi_set_timer(uint64_t absolute_time)
{
    wrapped_set_calls++;
    wrapped_deadline = absolute_time;
    return wrapped_set_result;
}

static uint64_t read_time(void)
{
    uint64_t value;

    asm volatile("csrr %0, time" : "=r"(value));
    return value;
}

static unsigned long read_sie(void)
{
    unsigned long value;

    asm volatile("csrr %0, sie" : "=r"(value));
    return value;
}

static unsigned long read_sstatus(void)
{
    unsigned long value;

    asm volatile("csrr %0, sstatus" : "=r"(value));
    return value;
}

static unsigned long run_sbi_cases(void)
{
    unsigned long failures = 0U;

    if (__real_sbi_probe_extension(SBI_EXT_TIME) <= 0L) {
        failures++;
    }
    if (__real_sbi_set_timer(UINT64_MAX) != 0L) {
        failures++;
    }

    return failures;
}

static unsigned long expect_deadline(enum riscv_timer_status expected_status,
                                     uint64_t now,
                                     uint64_t period,
                                     uint64_t previous,
                                     uint64_t expected_next,
                                     uint64_t expected_elapsed)
{
    uint64_t next = 0xa5a5a5a5a5a5a5a5ULL;
    uint64_t elapsed = 0x5a5a5a5a5a5a5a5aULL;
    enum riscv_timer_status status;

    status = riscv_timer_deadline_advance(now,
                                          period,
                                          previous,
                                          &next,
                                          &elapsed);
    if (status != expected_status) {
        return 1U;
    }
    if (status == RISCV_TIMER_STATUS_OK) {
        return next != expected_next || elapsed != expected_elapsed;
    }

    return next != 0xa5a5a5a5a5a5a5a5ULL ||
           elapsed != 0x5a5a5a5a5a5a5a5aULL;
}

static unsigned long run_deadline_cases(void)
{
    unsigned long failures = 0U;
    uint64_t output = 0xa5a5a5a5a5a5a5a5ULL;

    failures += expect_deadline(RISCV_TIMER_STATUS_OK,
                                100U, 10U, 100U, 110U, 1U);
    failures += expect_deadline(RISCV_TIMER_STATUS_OK,
                                109U, 10U, 100U, 110U, 1U);
    failures += expect_deadline(RISCV_TIMER_STATUS_OK,
                                110U, 10U, 100U, 120U, 2U);
    failures += expect_deadline(RISCV_TIMER_STATUS_OK,
                                139U, 10U, 100U, 140U, 4U);
    failures += expect_deadline(RISCV_TIMER_STATUS_OK,
                                UINT64_MAX - 1U,
                                10U,
                                UINT64_MAX - 4U,
                                5U,
                                1U);
    failures += expect_deadline(RISCV_TIMER_STATUS_EARLY_INTERRUPT,
                                UINT64_MAX,
                                10U,
                                5U,
                                0U,
                                0U);
    failures += expect_deadline(RISCV_TIMER_STATUS_INVALID_ARGUMENT,
                                100U, 0U, 100U, 0U, 0U);

    if (riscv_timer_deadline_advance(100U,
                                     10U,
                                     100U,
                                     NULL,
                                     &output) !=
            RISCV_TIMER_STATUS_INVALID_ARGUMENT ||
        output != 0xa5a5a5a5a5a5a5a5ULL) {
        failures++;
    }
    if (riscv_timer_deadline_advance(100U,
                                     10U,
                                     100U,
                                     &output,
                                     NULL) !=
            RISCV_TIMER_STATUS_INVALID_ARGUMENT ||
        output != 0xa5a5a5a5a5a5a5a5ULL) {
        failures++;
    }

    return failures;
}

static unsigned long run_timer_state_cases(void)
{
    uint64_t elapsed = 0x1122334455667788ULL;
    uint64_t after;
    uint64_t before;
    uint64_t first_deadline;
    enum riscv_timer_status status;
    unsigned long failures = 0U;

    if (riscv_timer_handle_interrupt(NULL) !=
        RISCV_TIMER_STATUS_INVALID_ARGUMENT) {
        failures++;
    }
    status = riscv_timer_handle_interrupt(&elapsed);
    if (status != RISCV_TIMER_STATUS_NOT_STARTED ||
        elapsed != 0x1122334455667788ULL) {
        failures++;
    }
    if (riscv_timer_start(0U, 100U) !=
            RISCV_TIMER_STATUS_INVALID_FREQUENCY ||
        riscv_timer_start(99U, 100U) !=
            RISCV_TIMER_STATUS_INVALID_FREQUENCY ||
        riscv_timer_start(100U, 0U) !=
            RISCV_TIMER_STATUS_INVALID_FREQUENCY) {
        failures++;
    }

    wrapped_probe_result = -2L;
    if (riscv_timer_start(10000000U, 100U) !=
        RISCV_TIMER_STATUS_SBI_PROBE_FAILED) {
        failures++;
    }
    wrapped_probe_result = 0L;
    if (riscv_timer_start(10000000U, 100U) !=
        RISCV_TIMER_STATUS_SBI_TIME_UNAVAILABLE) {
        failures++;
    }
    wrapped_probe_result = 1L;
    wrapped_set_result = -3L;
    if (riscv_timer_start(10000000U, 100U) !=
        RISCV_TIMER_STATUS_SBI_SET_FAILED) {
        failures++;
    }
    if ((read_sie() & RISCV_SIE_STIE) != 0U ||
        (read_sstatus() & RISCV_SSTATUS_SIE) != 0U) {
        failures++;
    }

    wrapped_set_result = 0L;
    before = read_time();
    status = riscv_timer_start(10000000U, 10U);
    after = read_time();
    if (status != RISCV_TIMER_STATUS_OK ||
        wrapped_deadline - before < 1000000U ||
        wrapped_deadline - after > 1000000U ||
        (wrapped_deadline - after) >> 63 != 0U ||
        (read_sie() & RISCV_SIE_STIE) == 0U ||
        (read_sstatus() & RISCV_SSTATUS_SIE) == 0U) {
        failures++;
    }
    if (riscv_timer_start(10000000U, 100U) !=
        RISCV_TIMER_STATUS_ALREADY_STARTED) {
        failures++;
    }

    first_deadline = wrapped_deadline;
    while (((read_time() - first_deadline) >> 63) != 0U) {
        asm volatile("" ::: "memory");
    }

    wrapped_set_result = -4L;
    elapsed = 0x1122334455667788ULL;
    status = riscv_timer_handle_interrupt(&elapsed);
    if (status != RISCV_TIMER_STATUS_SBI_SET_FAILED ||
        elapsed != 0x1122334455667788ULL) {
        failures++;
    }

    wrapped_set_result = 0L;
    status = riscv_timer_handle_interrupt(&elapsed);
    if (status != RISCV_TIMER_STATUS_OK || elapsed == 0U) {
        failures++;
    }
    if (wrapped_probe_calls != 4U || wrapped_set_calls != 4U) {
        failures++;
    }

    {
        unsigned long stie = RISCV_SIE_STIE;

        asm volatile("csrci sstatus, 2\n"
                     "csrc sie, %0"
                     :
                     : "r"(stie)
                     : "memory");
    }
    return failures;
}

static unsigned long run_tick_cases(void)
{
    unsigned long failures = 0U;

    if (kernel_tick_count() != 0U) {
        failures++;
    }
    kernel_tick_advance(0U);
    if (kernel_tick_count() != 0U) {
        failures++;
    }
    kernel_tick_advance(3U);
    if (kernel_tick_count() != 3U) {
        failures++;
    }

    return failures;
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    unsigned long failures;

    (void)hart_id;
    (void)dtb;

    failures = run_sbi_cases() +
               run_tick_cases() +
               run_deadline_cases() +
               run_timer_state_cases();
    virt_uart_puts("BoarOS: timer cases failures=");
    virt_uart_put_hex(failures);
    virt_uart_putc('\n');
    sbi_shutdown();
}
