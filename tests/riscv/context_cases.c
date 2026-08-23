#include <arch/riscv/context.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

#include <stddef.h>
#include <stdint.h>

extern void riscv_kernel_thread_trampoline(void);

static void context_entry(void *argument)
{
    (void)argument;
}

static unsigned long context_changed(
    const struct riscv_switch_context *context,
    uint64_t value)
{
    const uint64_t *words = (const uint64_t *)context;
    size_t index;

    for (index = 0U; index < sizeof(*context) / sizeof(*words); index++) {
        if (words[index] != value) {
            return 1U;
        }
    }
    return 0U;
}

static void fill_context(struct riscv_switch_context *context,
                         uint64_t value)
{
    uint64_t *words = (uint64_t *)context;
    size_t index;

    for (index = 0U; index < sizeof(*context) / sizeof(*words); index++) {
        words[index] = value;
    }
}

static unsigned long run_init_cases(void)
{
    const uint64_t sentinel = UINT64_C(0xa5a5a5a5a5a5a5a5);
    struct riscv_switch_context context;
    uintptr_t stack_top = UINT64_C(0xffffffff80004000);
    void *argument = (void *)(uintptr_t)UINT64_C(0x1122334455667788);
    void *thread = (void *)(uintptr_t)UINT64_C(0xffffffff80002000);
    unsigned long failures = 0U;
    size_t index;

    fill_context(&context, sentinel);
    if (riscv_context_init(0,
                           stack_top,
                           context_entry,
                           argument,
                           thread) != RISCV_CONTEXT_STATUS_INVALID_ARGUMENT ||
        riscv_context_init(&context,
                           0U,
                           context_entry,
                           argument,
                           thread) != RISCV_CONTEXT_STATUS_INVALID_ARGUMENT ||
        riscv_context_init(&context,
                           stack_top - 8U,
                           context_entry,
                           argument,
                           thread) != RISCV_CONTEXT_STATUS_INVALID_ARGUMENT ||
        riscv_context_init(&context,
                           stack_top,
                           0,
                           argument,
                           thread) != RISCV_CONTEXT_STATUS_INVALID_ARGUMENT ||
        riscv_context_init(&context,
                           stack_top,
                           context_entry,
                           argument,
                           0) != RISCV_CONTEXT_STATUS_INVALID_ARGUMENT ||
        context_changed(&context, sentinel) != 0U) {
        failures++;
    }

    if (riscv_context_init(&context,
                           stack_top,
                           context_entry,
                           argument,
                           thread) != RISCV_CONTEXT_STATUS_OK ||
        context.ra != (uintptr_t)riscv_kernel_thread_trampoline ||
        context.sp != stack_top ||
        context.tp != (uintptr_t)thread ||
        context.s[0] != (uintptr_t)context_entry ||
        context.s[1] != (uintptr_t)argument) {
        failures++;
    }
    for (index = 2U; index < 12U; index++) {
        if (context.s[index] != 0U) {
            failures++;
        }
    }

    return failures;
}

static unsigned long run_register_helper_cases(void)
{
    void *original_thread = riscv_current_thread_get();
    void *test_thread = (void *)(uintptr_t)UINT64_C(0xffffffff80003000);
    uintptr_t old_status;
    unsigned long failures = 0U;

    if (riscv_interrupt_is_enabled()) {
        failures++;
    }
    old_status = riscv_interrupt_save();
    if (riscv_interrupt_is_enabled()) {
        failures++;
    }
    riscv_interrupt_restore(old_status);
    if (riscv_interrupt_is_enabled()) {
        failures++;
    }

    __asm__ volatile("csrsi sstatus, 2" ::: "memory");
    old_status = riscv_interrupt_save();
    if ((old_status & (uintptr_t)2U) == 0U ||
        riscv_interrupt_is_enabled()) {
        failures++;
    }
    riscv_interrupt_restore(old_status);
    if (!riscv_interrupt_is_enabled()) {
        failures++;
    }
    __asm__ volatile("csrci sstatus, 2" ::: "memory");

    riscv_current_thread_set(test_thread);
    if (riscv_current_thread_get() != test_thread) {
        failures++;
    }
    riscv_current_thread_set(original_thread);

    return failures;
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    unsigned long failures;

    (void)hart_id;
    (void)dtb;

    failures = run_init_cases();
    failures += run_register_helper_cases();

    virt_uart_puts("BoarOS: context cases failures=");
    virt_uart_put_hex(failures);
    virt_uart_putc('\n');
    sbi_shutdown();
}
