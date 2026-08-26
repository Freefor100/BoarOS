#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

unsigned long run_all_uaccess_cases(void);

void kernel_main(unsigned long hart_id, const void *dtb)
{
    unsigned long result;

    (void)hart_id;
    (void)dtb;
    result = run_all_uaccess_cases();
    virt_uart_puts("BoarOS: uaccess cases result=");
    virt_uart_put_hex(result);
    virt_uart_putc('\n');
    sbi_shutdown();
}
