#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

#define SBI_EXT_SRST 0x53525354UL
#define SBI_SRST_FID_SYSTEM_RESET 0UL
#define SBI_SRST_SHUTDOWN 0UL
#define SBI_SRST_REASON_NONE 0UL

void sbi_shutdown(void)
{
    register unsigned long a0 asm("a0") = SBI_SRST_SHUTDOWN;
    register unsigned long a1 asm("a1") = SBI_SRST_REASON_NONE;
    register unsigned long a6 asm("a6") = SBI_SRST_FID_SYSTEM_RESET;
    register unsigned long a7 asm("a7") = SBI_EXT_SRST;

    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a6), "r"(a7)
                 : "memory");

    virt_uart_puts("BoarOS: SBI shutdown failed\n");

    for (;;) {
        asm volatile("wfi");
    }
}
