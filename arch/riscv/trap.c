#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

void riscv_trap_fatal(unsigned long scause,
                      unsigned long sepc,
                      unsigned long stval,
                      unsigned long sstatus) __attribute__((noreturn));

void riscv_trap_fatal(unsigned long scause,
                      unsigned long sepc,
                      unsigned long stval,
                      unsigned long sstatus)
{
    virt_uart_puts("BoarOS: fatal trap scause=");
    virt_uart_put_hex(scause);
    virt_uart_puts(" sepc=");
    virt_uart_put_hex(sepc);
    virt_uart_puts(" stval=");
    virt_uart_put_hex(stval);
    virt_uart_puts(" sstatus=");
    virt_uart_put_hex(sstatus);
    virt_uart_putc('\n');

    sbi_shutdown();
}
