#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

void trap_test_breakpoint(void);

void kernel_main(unsigned long hart_id, const void *dtb)
{
    (void)hart_id;
    (void)dtb;

    virt_uart_puts("BoarOS: trap test breakpoint=");
    virt_uart_put_hex((unsigned long)trap_test_breakpoint);
    virt_uart_putc('\n');

    trap_test_breakpoint();

    virt_uart_puts("BoarOS: trap test returned\n");
    sbi_shutdown();
}
