#include <arch/riscv/sbi.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/trap.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/scheduler.h>
#include <kernel/syscall.h>
#include <kernel/task.h>
#include <kernel/tick.h>

#include <stdint.h>

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

static void riscv_scheduler_fatal(
    const struct riscv_trap_frame *frame,
    enum kernel_scheduler_status status) __attribute__((noreturn));

static void riscv_scheduler_fatal(
    const struct riscv_trap_frame *frame,
    enum kernel_scheduler_status status)
{
    virt_uart_puts("BoarOS: scheduler error status=");
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
    int from_user = (frame->sstatus & RISCV_SSTATUS_SPP) == 0U;

    if (frame->scause ==
        (RISCV_SCAUSE_INTERRUPT | RISCV_SCAUSE_SUPERVISOR_TIMER)) {
        uint64_t elapsed_ticks;
        enum riscv_timer_status status =
            riscv_timer_handle_interrupt(&elapsed_ticks);

        if (status != RISCV_TIMER_STATUS_OK) {
            riscv_timer_fatal(frame, status);
        }
        kernel_tick_advance(elapsed_ticks);
        {
            enum kernel_scheduler_status scheduler_status =
                kernel_scheduler_on_tick(elapsed_ticks);

            if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK) {
                riscv_scheduler_fatal(frame, scheduler_status);
            }
        }
        return;
    }

    if (from_user && frame->scause == RISCV_SCAUSE_USER_ECALL) {
        struct kernel_syscall_request request = {
            .number = frame->a7,
            .arguments = {
                frame->a0,
                frame->a1,
                frame->a2,
                frame->a3,
                frame->a4,
                frame->a5,
            },
        };
        struct kernel_syscall_result result;

        if (kernel_syscall_dispatch(kernel_task_current(),
                                    &request,
                                    &result) !=
            KERNEL_SYSCALL_STATUS_OK) {
            riscv_trap_fatal(frame);
        }
        if (result.action == KERNEL_SYSCALL_ACTION_EXIT) {
            kernel_user_thread_exit(KERNEL_THREAD_EXIT_SYSCALL,
                                    (uint64_t)result.value,
                                    0U);
        }
        if (result.action != KERNEL_SYSCALL_ACTION_RETURN) {
            riscv_trap_fatal(frame);
        }
        if (frame->sepc > UINT64_MAX - 4U) {
            kernel_user_thread_exit(KERNEL_THREAD_EXIT_USER_FAULT,
                                    frame->scause,
                                    frame->stval);
        }
        frame->a0 = (unsigned long)result.value;
        frame->sepc += 4U;
        return;
    }

    if (from_user &&
        (frame->scause & RISCV_SCAUSE_INTERRUPT) == 0U) {
        kernel_user_thread_exit(KERNEL_THREAD_EXIT_USER_FAULT,
                                frame->scause,
                                frame->stval);
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
