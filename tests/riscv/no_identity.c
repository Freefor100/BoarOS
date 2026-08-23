#include <arch/riscv/sbi.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/virt_uart.h>

#include <stdint.h>

#define KERNEL_PHYSICAL_START UINT64_C(0x80200000)

enum riscv_timer_status __real_riscv_timer_start(
    uint32_t timebase_frequency,
    uint32_t ticks_per_second);
enum riscv_timer_status __wrap_riscv_timer_start(
    uint32_t timebase_frequency,
    uint32_t ticks_per_second);

static unsigned int timer_start_calls;

static uint64_t load_low_kernel_alias(uint64_t address)
{
    uint64_t value;

    __asm__ volatile("ld %0, 0(%1)"
                     : "=r"(value)
                     : "r"(address)
                     : "memory");
    return value;
}

enum riscv_timer_status __wrap_riscv_timer_start(
    uint32_t timebase_frequency,
    uint32_t ticks_per_second)
{
    if (timer_start_calls == 0U) {
        uint64_t value;

        timer_start_calls = 1U;
        virt_uart_puts("BoarOS: no-identity target=");
        virt_uart_put_hex((unsigned long)KERNEL_PHYSICAL_START);
        virt_uart_putc('\n');

        value = load_low_kernel_alias(KERNEL_PHYSICAL_START);
        virt_uart_puts("BoarOS: no-identity load returned value=");
        virt_uart_put_hex((unsigned long)value);
        virt_uart_putc('\n');
        sbi_shutdown();
    }

    return __real_riscv_timer_start(timebase_frequency, ticks_per_second);
}
