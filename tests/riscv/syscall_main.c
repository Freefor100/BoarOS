#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

unsigned long run_syscall_cases(void);

void kernel_main(unsigned long hart_id, const void *dtb)
{
    unsigned long failures;

    (void)hart_id;
    (void)dtb;

    failures = run_syscall_cases();
    virt_uart_puts("BoarOS: syscall cases failures=");
    virt_uart_put_hex(failures);
    virt_uart_putc('\n');
    sbi_shutdown();
}
