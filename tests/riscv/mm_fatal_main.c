#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

void run_mm_resolution_invariant_fatal_case(void);

void kernel_main(unsigned long hart_id, const void *dtb)
{
    (void)hart_id;
    (void)dtb;

    virt_uart_puts("BoarOS: MM resolution invariant test begin\n");
    run_mm_resolution_invariant_fatal_case();
    virt_uart_puts("BoarOS: MM resolution invariant escaped fatal\n");
    sbi_shutdown();
}
