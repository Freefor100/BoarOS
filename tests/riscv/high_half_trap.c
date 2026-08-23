#include <arch/riscv/sbi.h>
#include <arch/riscv/trap.h>
#include <arch/riscv/virt_uart.h>

#include <stdint.h>

void __real_sbi_shutdown(void) __attribute__((noreturn));
void __wrap_sbi_shutdown(void) __attribute__((noreturn));
void __real_riscv_trap_dispatch(struct riscv_trap_frame *frame);
void __wrap_riscv_trap_dispatch(struct riscv_trap_frame *frame);
void trap_test_breakpoint(void);
unsigned long trap_test_software_interrupt_return(void);
extern unsigned char trap_test_software_interrupt_resume[];

static unsigned int shutdown_calls;
static unsigned int expecting_software_interrupt;
static unsigned long return_handler_failures;
static unsigned long return_scause;
static unsigned long return_sepc;
static unsigned long return_frame;

void __wrap_riscv_trap_dispatch(struct riscv_trap_frame *frame)
{
    if (expecting_software_interrupt != 0U) {
        expecting_software_interrupt = 0U;
        return_scause = frame->scause;
        return_sepc = frame->sepc;
        return_frame = (unsigned long)(uintptr_t)frame;

        if (frame->scause !=
                (RISCV_SCAUSE_INTERRUPT |
                 RISCV_SCAUSE_SUPERVISOR_SOFTWARE) ||
            frame->sepc !=
                (unsigned long)(uintptr_t)trap_test_software_interrupt_resume ||
            (frame->sstatus &
             (RISCV_SSTATUS_SPP |
              RISCV_SSTATUS_SPIE |
              RISCV_SSTATUS_SIE)) !=
                (RISCV_SSTATUS_SPP | RISCV_SSTATUS_SPIE) ||
            (return_frame & 0xfUL) != 0UL ||
            frame->sp != return_frame + RISCV_TRAP_FRAME_SIZE) {
            return_handler_failures = 1UL;
        }

        __asm__ volatile("csrci sip, 2" ::: "memory");
        return;
    }

    __real_riscv_trap_dispatch(frame);
}

void __wrap_sbi_shutdown(void)
{
    if (shutdown_calls == 0U) {
        unsigned long register_failures;

        shutdown_calls = 1U;
        expecting_software_interrupt = 1U;
        register_failures = trap_test_software_interrupt_return();
        if (expecting_software_interrupt != 0U) {
            return_handler_failures = 1UL;
        }

        virt_uart_puts("BoarOS: high-half trap return scause=");
        virt_uart_put_hex(return_scause);
        virt_uart_puts(" sepc=");
        virt_uart_put_hex(return_sepc);
        virt_uart_puts(" frame=");
        virt_uart_put_hex(return_frame);
        virt_uart_puts(" handler=");
        virt_uart_put_hex(return_handler_failures);
        virt_uart_puts(" registers=");
        virt_uart_put_hex(register_failures);
        virt_uart_putc('\n');

        virt_uart_puts("BoarOS: high-half trap breakpoint=");
        virt_uart_put_hex((unsigned long)(uintptr_t)trap_test_breakpoint);
        virt_uart_putc('\n');

        trap_test_breakpoint();

        virt_uart_puts("BoarOS: high-half trap returned\n");
    }

    __real_sbi_shutdown();
}
