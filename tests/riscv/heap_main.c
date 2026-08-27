#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

void run_kernel_heap_tests(void);

void kernel_main(unsigned long hart_id, const void *dtb)
{
    (void)hart_id;
    (void)dtb;

    run_kernel_heap_tests();

    virt_uart_puts("BoarOS: kernel heap tests passed\n");
    sbi_shutdown();
}
