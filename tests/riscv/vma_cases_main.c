#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

unsigned long run_all_vma_cases(void);
unsigned long run_shared_anon_cases(void);
unsigned long run_vma_lookup_baseline(unsigned long cycles[3]);

void kernel_main(unsigned long hart_id, const void *dtb)
{
    unsigned long result;
    unsigned long cycles[3];

    (void)hart_id;
    (void)dtb;
    result = run_all_vma_cases();
    if (result == 0U) {
        result = run_shared_anon_cases();
        if (result != 0U) result += 0x500U;
    }
    if (result == 0U) {
        result = run_vma_lookup_baseline(cycles);
        if (result != 0U) {
            result += 0x400U;
        }
    }
    virt_uart_puts("BoarOS: VMA cases result=");
    virt_uart_put_hex(result);
    virt_uart_putc('\n');
    if (result == 0U) {
        virt_uart_puts("BoarOS: VMA lookup cycles n=1 ");
        virt_uart_put_hex(cycles[0]);
        virt_uart_putc('\n');
        virt_uart_puts("BoarOS: VMA lookup cycles n=64 ");
        virt_uart_put_hex(cycles[1]);
        virt_uart_putc('\n');
        virt_uart_puts("BoarOS: VMA lookup cycles n=1024 ");
        virt_uart_put_hex(cycles[2]);
        virt_uart_putc('\n');
    }
    sbi_shutdown();
}
