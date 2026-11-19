#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
int run_sv39_huge_tests(void);
void kernel_main(unsigned long hart, const void *dtb)
{
    (void)hart; (void)dtb;
    int result=run_sv39_huge_tests();
    if (result) {
        virt_uart_puts("BoarOS: huge leaf failed case=");
        virt_uart_put_hex(result);virt_uart_putc('\n');
    } else virt_uart_puts("BoarOS: huge leaf lifecycle passed\n");
    sbi_shutdown();
}
