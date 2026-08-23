#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

#include <stdint.h>

void __real_sbi_shutdown(void) __attribute__((noreturn));
void __wrap_sbi_shutdown(void) __attribute__((noreturn));
void trap_test_breakpoint(void);

static unsigned int shutdown_calls;

void __wrap_sbi_shutdown(void)
{
    if (shutdown_calls == 0U) {
        shutdown_calls = 1U;
        virt_uart_puts("BoarOS: high-half trap breakpoint=");
        virt_uart_put_hex((unsigned long)(uintptr_t)trap_test_breakpoint);
        virt_uart_putc('\n');

        trap_test_breakpoint();

        virt_uart_puts("BoarOS: high-half trap returned\n");
    }

    __real_sbi_shutdown();
}
