#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

int run_sv39_tests(void);

void kernel_main(unsigned long hart_id, const void *dtb)
{
    int result;

    (void)hart_id;
    (void)dtb;

    result = run_sv39_tests();
    if (result != 0) {
        virt_uart_puts("BoarOS: Sv39 test failed case=");
        virt_uart_put_hex((unsigned long)result);
        virt_uart_putc('\n');
        sbi_shutdown();
    }

    virt_uart_puts("BoarOS: Sv39 tests passed\n");
    sbi_shutdown();
}
