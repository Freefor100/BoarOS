#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

#include <stdint.h>

#define KERNEL_PHYSICAL_START UINT64_C(0x80200000)

void __real_sbi_shutdown(void) __attribute__((noreturn));
void __wrap_sbi_shutdown(void) __attribute__((noreturn));

static unsigned int shutdown_calls;

static uint64_t load_low_kernel_alias(uint64_t address)
{
    uint64_t value;

    __asm__ volatile("ld %0, 0(%1)"
                     : "=r"(value)
                     : "r"(address)
                     : "memory");
    return value;
}

void __wrap_sbi_shutdown(void)
{
    if (shutdown_calls == 0U) {
        uint64_t value;

        shutdown_calls = 1U;
        virt_uart_puts("BoarOS: no-identity target=");
        virt_uart_put_hex((unsigned long)KERNEL_PHYSICAL_START);
        virt_uart_putc('\n');

        value = load_low_kernel_alias(KERNEL_PHYSICAL_START);
        virt_uart_puts("BoarOS: no-identity load returned value=");
        virt_uart_put_hex((unsigned long)value);
        virt_uart_putc('\n');
    }

    __real_sbi_shutdown();
}
