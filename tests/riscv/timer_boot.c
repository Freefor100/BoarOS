#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/tick.h>

#include <stdint.h>

void __real_kernel_tick_advance(uint64_t elapsed_ticks);
void __wrap_kernel_tick_advance(uint64_t elapsed_ticks);

static unsigned long interrupt_entries;
static unsigned long timer_failures;

void __wrap_kernel_tick_advance(uint64_t elapsed_ticks)
{
    uint64_t ticks;

    interrupt_entries++;
    if (elapsed_ticks == 0U) {
        timer_failures++;
    }
    __real_kernel_tick_advance(elapsed_ticks);
    ticks = kernel_tick_count();

    if ((ticks >= 3U && interrupt_entries >= 3U) ||
        interrupt_entries > 8U) {
        if (ticks < 3U) {
            timer_failures++;
        }
        virt_uart_puts("BoarOS: timer interrupt ticks=");
        virt_uart_put_hex((unsigned long)ticks);
        virt_uart_puts(" entries=");
        virt_uart_put_hex(interrupt_entries);
        virt_uart_puts(" failures=");
        virt_uart_put_hex(timer_failures);
        virt_uart_putc('\n');
        sbi_shutdown();
    }
}
