#include <arch/riscv/sbi.h>
#include <arch/riscv/trap.h>
#include <arch/riscv/virt_uart.h>

#include <stdint.h>

void trap_test_bad_return(void);
extern unsigned char trap_test_bad_return_breakpoint[];
void __real_riscv_trap_dispatch(struct riscv_trap_frame *frame);
void __wrap_riscv_trap_dispatch(struct riscv_trap_frame *frame);

static unsigned int expecting_bad_return;

void __wrap_riscv_trap_dispatch(struct riscv_trap_frame *frame)
{
    if (expecting_bad_return != 0U &&
        frame->scause == RISCV_SCAUSE_BREAKPOINT &&
        frame->sepc ==
            (unsigned long)(uintptr_t)trap_test_bad_return_breakpoint) {
        expecting_bad_return = 0U;
        frame->sstatus |= RISCV_SSTATUS_SPP | RISCV_SSTATUS_SIE;
        return;
    }

    __real_riscv_trap_dispatch(frame);
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    (void)hart_id;
    (void)dtb;

    virt_uart_puts("BoarOS: trap SIE bad return breakpoint=");
    virt_uart_put_hex(
        (unsigned long)(uintptr_t)trap_test_bad_return_breakpoint);
    virt_uart_putc('\n');

    expecting_bad_return = 1U;
    trap_test_bad_return();

    virt_uart_puts("BoarOS: trap SIE bad return escaped validation\n");
    sbi_shutdown();
}
