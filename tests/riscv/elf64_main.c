#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

unsigned long run_elf64_cases(void);

void kernel_main(unsigned long hart_id, const void *dtb)
{
    unsigned long failures;

    (void)hart_id;
    (void)dtb;

    failures = run_elf64_cases();
    virt_uart_puts("BoarOS: ELF64 cases failures=");
    virt_uart_put_hex(failures);
    virt_uart_putc('\n');
    sbi_shutdown();
}
