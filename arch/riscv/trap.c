#include <arch/riscv/process.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/trap.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/console.h>
#include <kernel/errno.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <arch/riscv/signal.h>
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
    struct kernel_task *curr = kernel_task_current();
    kernel_pid_t tid = 0;
    virt_uart_puts("BoarOS: scheduler error status=");
    virt_uart_put_hex((unsigned long)status);
    virt_uart_puts(" scause=");
    virt_uart_put_hex(frame->scause);
    virt_uart_puts(" sepc=");
    virt_uart_put_hex(frame->sepc);
    if (curr != 0 && kernel_task_tid(curr, &tid) == KERNEL_TASK_STATUS_OK) {
        virt_uart_puts(" tid=");
        virt_uart_put_hex((unsigned long)tid);
    }
    virt_uart_putc('\n');
    sbi_shutdown();
}

static void riscv_user_fault_resolver_fatal(
    const struct riscv_trap_frame *frame,
    enum kernel_mm_status status) __attribute__((noreturn));

static void riscv_user_fault_resolver_fatal(
    const struct riscv_trap_frame *frame,
    enum kernel_mm_status status)
{
    virt_uart_puts("BoarOS: user fault resolver error status=");
    virt_uart_put_hex((unsigned long)status);
    virt_uart_puts(" scause=");
    virt_uart_put_hex(frame->scause);
    virt_uart_puts(" sepc=");
    virt_uart_put_hex(frame->sepc);
    virt_uart_puts(" stval=");
    virt_uart_put_hex(frame->stval);
    virt_uart_putc('\n');
    sbi_shutdown();
}

static int user_page_fault_access(uint64_t scause, uint32_t *access)
{
    switch (scause) {
    case RISCV_SCAUSE_INSTRUCTION_PAGE_FAULT:
        *access = KERNEL_MM_EXECUTE;
        return 1;
    case RISCV_SCAUSE_LOAD_PAGE_FAULT:
        *access = KERNEL_MM_READ;
        return 1;
    case RISCV_SCAUSE_STORE_PAGE_FAULT:
        *access = KERNEL_MM_WRITE;
        return 1;
    default:
        return 0;
    }
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
        kernel_scheduler_charge_ticks(elapsed_ticks, (int)from_user);
        {
            enum kernel_scheduler_status scheduler_status =
                kernel_scheduler_expire_deadlines(riscv_time_read());

            if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK) {
                riscv_scheduler_fatal(frame, scheduler_status);
            }
        }
        kernel_console_poll_input();
        {
            enum kernel_scheduler_status scheduler_status =
                kernel_scheduler_on_tick(elapsed_ticks);

            if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK) {
                riscv_scheduler_fatal(frame, scheduler_status);
            }
        }
        return;
    }

    if (from_user &&
        (frame->scause & RISCV_SCAUSE_INTERRUPT) == 0U) {
        uint32_t access;

        if (user_page_fault_access(frame->scause, &access)) {
            enum kernel_mm_status status =
                kernel_scheduler_resolve_current_user_fault(
                    frame->stval,
                    access);

            if (status == KERNEL_MM_STATUS_OK) {
                return;
            }
            if (status == KERNEL_MM_STATUS_NOT_MAPPED) {
                kernel_user_thread_exit(KERNEL_THREAD_EXIT_SIGNAL,
                                        11U,
                                        frame->stval);
            }
            if (status == KERNEL_MM_STATUS_BUS_FAULT) {
                kernel_user_thread_exit(KERNEL_THREAD_EXIT_SIGNAL,
                                        7U,
                                        frame->stval);
            }
            if (status == KERNEL_MM_STATUS_NO_MEMORY) {
                kernel_user_thread_exit(
                    KERNEL_THREAD_EXIT_RESOURCE,
                    KERNEL_THREAD_RESOURCE_NO_MEMORY,
                    frame->stval);
            }
            riscv_user_fault_resolver_fatal(frame, status);
        }
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
        if (result.action == KERNEL_SYSCALL_ACTION_EXIT_GROUP) {
            kernel_user_group_exit(KERNEL_THREAD_EXIT_SYSCALL,
                                   (uint64_t)result.value, 0U);
        }
        if (result.action == KERNEL_SYSCALL_ACTION_EXIT) {
            kernel_user_thread_exit(KERNEL_THREAD_EXIT_SYSCALL,
                                    (uint64_t)result.value,
                                    0U);
        }
        if (result.action == KERNEL_SYSCALL_ACTION_EXEC) {
            enum kernel_scheduler_status scheduler_status =
                kernel_scheduler_exec_commit();

            if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK) {
                riscv_scheduler_fatal(frame, scheduler_status);
            }
            return;
        }
        if (result.action == KERNEL_SYSCALL_ACTION_CLONE) {
            enum kernel_scheduler_status scheduler_status =
                riscv_process_clone_current(
                    frame,
                    request.arguments[0],
                    request.arguments[1],
                    request.arguments[2],
                    request.arguments[3],
                    request.arguments[4],
                    &result.value);

            if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK) {
                riscv_scheduler_fatal(frame, scheduler_status);
            }
            result.action = KERNEL_SYSCALL_ACTION_RETURN;
        }
        if (result.action == KERNEL_SYSCALL_ACTION_WAIT4) {
            enum kernel_scheduler_status scheduler_status =
                kernel_scheduler_wait4_current(
                    (int64_t)(int32_t)(uint32_t)request.arguments[0],
                    request.arguments[1],
                    (uint32_t)request.arguments[2],
                    request.arguments[3],
                    &result.value);

            if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK) {
                riscv_scheduler_fatal(frame, scheduler_status);
            }
            result.action = KERNEL_SYSCALL_ACTION_RETURN;
        }
        if (result.action == KERNEL_SYSCALL_ACTION_YIELD) {
            enum kernel_scheduler_status scheduler_status =
                kernel_scheduler_yield_current();

            if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK) {
                riscv_scheduler_fatal(frame, scheduler_status);
            }
            result.value = 0;
            result.action = KERNEL_SYSCALL_ACTION_RETURN;
        }
        if (result.action == KERNEL_SYSCALL_ACTION_SIGNAL_RETURN) {
            riscv_signal_restore_current(frame);
            return;
        }
        if (result.action != KERNEL_SYSCALL_ACTION_RETURN) {
            riscv_trap_fatal(frame);
        }
        if (result.value == -KERNEL_ERESTARTSYS) {
            kernel_signal_note_syscall_restart(kernel_task_current());
            return;
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

void riscv_trap_return_prepare(struct riscv_trap_frame *frame)
{
    riscv_signal_prepare_user_return(frame);
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
