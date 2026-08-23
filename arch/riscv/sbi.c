#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

#define SBI_SUCCESS 0L

#define SBI_EXT_BASE 0x10UL
#define SBI_BASE_FID_PROBE_EXTENSION 3UL
#define SBI_EXT_TIME 0x54494d45UL
#define SBI_TIME_FID_SET_TIMER 0UL
#define SBI_EXT_SRST 0x53525354UL
#define SBI_SRST_FID_SYSTEM_RESET 0UL
#define SBI_SRST_SHUTDOWN 0UL
#define SBI_SRST_REASON_NONE 0UL

struct sbi_ret {
    long error;
    long value;
};

static struct sbi_ret sbi_ecall(unsigned long extension_id,
                                unsigned long function_id,
                                unsigned long argument0,
                                unsigned long argument1)
{
    register unsigned long a0 asm("a0") = argument0;
    register unsigned long a1 asm("a1") = argument1;
    register unsigned long a6 asm("a6") = function_id;
    register unsigned long a7 asm("a7") = extension_id;
    struct sbi_ret result;

    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a6), "r"(a7)
                 : "memory");

    result.error = (long)a0;
    result.value = (long)a1;
    return result;
}

long sbi_probe_extension(unsigned long extension_id)
{
    struct sbi_ret result = sbi_ecall(SBI_EXT_BASE,
                                      SBI_BASE_FID_PROBE_EXTENSION,
                                      extension_id,
                                      0U);

    return result.error == SBI_SUCCESS ? result.value : result.error;
}

long sbi_set_timer(uint64_t absolute_time)
{
    return sbi_ecall(SBI_EXT_TIME,
                     SBI_TIME_FID_SET_TIMER,
                     (unsigned long)absolute_time,
                     0U).error;
}

void sbi_shutdown(void)
{
    (void)sbi_ecall(SBI_EXT_SRST,
                    SBI_SRST_FID_SYSTEM_RESET,
                    SBI_SRST_SHUTDOWN,
                    SBI_SRST_REASON_NONE);

    virt_uart_puts("BoarOS: SBI shutdown failed\n");

    for (;;) {
        asm volatile("wfi");
    }
}
