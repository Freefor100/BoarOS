#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

unsigned long run_all_vma_cases(void);
unsigned long run_shared_anon_cases(void);

void kernel_main(unsigned long hart_id, const void *dtb)
{
    unsigned long result;

    (void)hart_id;
    (void)dtb;
    result = run_all_vma_cases();
    if (result == 0U) {
        result = run_shared_anon_cases();
        if (result != 0U) result += 0x500U;
    }
    virt_uart_puts("BoarOS: VMA cases result=");
    virt_uart_put_hex(result);
    virt_uart_putc('\n');
    sbi_shutdown();
}
