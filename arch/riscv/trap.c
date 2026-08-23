#include <arch/riscv/sbi.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/trap.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/tick.h>

static void riscv_trap_fatal(const struct riscv_trap_frame *frame)
    __attribute__((noreturn));

static void riscv_trap_fatal(const struct riscv_trap_frame *frame)
{
    virt_uart_puts("BoarOS: fatal trap scause=");
    virt_uart_put_hex(frame->scause);
    virt_uart_puts(" sepc=");
    virt_uart_put_hex(frame->sepc);
    virt_uart_puts(" stval=");
    virt_uart_put_hex(frame->stval);
    virt_uart_puts(" sstatus=");
    virt_uart_put_hex(frame->sstatus);
    virt_uart_putc('\n');

    sbi_shutdown();
}

static void riscv_timer_fatal(const struct riscv_trap_frame *frame,
                              enum riscv_timer_status status)
    __attribute__((noreturn));

static void riscv_timer_fatal(const struct riscv_trap_frame *frame,
                              enum riscv_timer_status status)
{
    virt_uart_puts("BoarOS: timer error status=");
    virt_uart_put_hex((unsigned long)status);
    virt_uart_puts(" scause=");
    virt_uart_put_hex(frame->scause);
    virt_uart_puts(" sepc=");
    virt_uart_put_hex(frame->sepc);
    virt_uart_putc('\n');
    sbi_shutdown();
}

void riscv_trap_dispatch(struct riscv_trap_frame *frame)
{
    if (frame->scause ==
        (RISCV_SCAUSE_INTERRUPT | RISCV_SCAUSE_SUPERVISOR_TIMER)) {
        uint64_t elapsed_ticks;
        enum riscv_timer_status status =
            riscv_timer_handle_interrupt(&elapsed_ticks);

        if (status != RISCV_TIMER_STATUS_OK) {
            riscv_timer_fatal(frame, status);
        }
        kernel_tick_advance(elapsed_ticks);
        return;
    }

    riscv_trap_fatal(frame);
}

void riscv_trap_bad_return(struct riscv_trap_frame *frame)
{
    virt_uart_puts("BoarOS: invalid trap return sstatus=");
    virt_uart_put_hex(frame->sstatus);
    virt_uart_puts(" sepc=");
    virt_uart_put_hex(frame->sepc);
    virt_uart_putc('\n');

    sbi_shutdown();
}
