#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

void run_physical_page_tests(void);

void kernel_main(unsigned long hart_id, const void *dtb)
{
    (void)hart_id;
    (void)dtb;

    run_physical_page_tests();

    virt_uart_puts("BoarOS: physical page tests passed\n");
    sbi_shutdown();
}
