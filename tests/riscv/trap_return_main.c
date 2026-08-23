#include <arch/riscv/sbi.h>
#include <arch/riscv/trap.h>
#include <arch/riscv/virt_uart.h>

#include <stdint.h>

enum trap_test_expectation {
    TRAP_TEST_EXPECT_NONE,
    TRAP_TEST_EXPECT_BREAKPOINT,
    TRAP_TEST_EXPECT_SOFTWARE_INTERRUPT,
    TRAP_TEST_EXPECT_BAD_RETURN,
};

#define TRAP_TEST_HANDLER_CAUSE (1UL << 0)
#define TRAP_TEST_HANDLER_EPC (1UL << 1)
#define TRAP_TEST_HANDLER_STATUS (1UL << 2)
#define TRAP_TEST_HANDLER_FRAME (1UL << 3)
#define TRAP_TEST_HANDLER_MISSING (1UL << 4)

unsigned long trap_test_breakpoint_return(void);
unsigned long trap_test_software_interrupt_return(void);
void trap_test_bad_return(void);
extern unsigned char trap_test_recoverable_breakpoint[];
extern unsigned char trap_test_bad_return_breakpoint[];
extern unsigned char trap_test_software_interrupt_resume[];

void __real_riscv_trap_dispatch(struct riscv_trap_frame *frame);
void __wrap_riscv_trap_dispatch(struct riscv_trap_frame *frame);

static enum trap_test_expectation expected_trap;
static unsigned long handler_failures;
static unsigned long observed_scause;
static unsigned long observed_sepc;
static unsigned long observed_sstatus;
static unsigned long observed_frame;

static void record_frame(const struct riscv_trap_frame *frame)
{
    observed_scause = frame->scause;
    observed_sepc = frame->sepc;
    observed_sstatus = frame->sstatus;
    observed_frame = (unsigned long)(uintptr_t)frame;

    if ((observed_frame & 0xfUL) != 0UL ||
        frame->sp != observed_frame + RISCV_TRAP_FRAME_SIZE) {
        handler_failures |= TRAP_TEST_HANDLER_FRAME;
    }
}

static void require_kernel_status(const struct riscv_trap_frame *frame,
                                  unsigned long expected_spie)
{
    unsigned long status = frame->sstatus;

    if ((status & RISCV_SSTATUS_SPP) == 0UL ||
        (status & RISCV_SSTATUS_SIE) != 0UL ||
        (status & RISCV_SSTATUS_SPIE) != expected_spie) {
        handler_failures |= TRAP_TEST_HANDLER_STATUS;
    }
}

void __wrap_riscv_trap_dispatch(struct riscv_trap_frame *frame)
{
    record_frame(frame);

    if (expected_trap == TRAP_TEST_EXPECT_BREAKPOINT) {
        expected_trap = TRAP_TEST_EXPECT_NONE;
        if (frame->scause != RISCV_SCAUSE_BREAKPOINT) {
            handler_failures |= TRAP_TEST_HANDLER_CAUSE;
        }
        if (frame->sepc !=
            (unsigned long)(uintptr_t)trap_test_recoverable_breakpoint) {
            handler_failures |= TRAP_TEST_HANDLER_EPC;
        }
        require_kernel_status(frame, 0UL);
        frame->sepc += 4UL;
        return;
    }

    if (expected_trap == TRAP_TEST_EXPECT_SOFTWARE_INTERRUPT) {
        expected_trap = TRAP_TEST_EXPECT_NONE;
        if (frame->scause !=
            (RISCV_SCAUSE_INTERRUPT |
             RISCV_SCAUSE_SUPERVISOR_SOFTWARE)) {
            handler_failures |= TRAP_TEST_HANDLER_CAUSE;
        }
        if (frame->sepc !=
            (unsigned long)(uintptr_t)trap_test_software_interrupt_resume) {
            handler_failures |= TRAP_TEST_HANDLER_EPC;
        }
        require_kernel_status(frame, RISCV_SSTATUS_SPIE);
        __asm__ volatile("csrci sip, 2" ::: "memory");
        return;
    }

    if (expected_trap == TRAP_TEST_EXPECT_BAD_RETURN) {
        expected_trap = TRAP_TEST_EXPECT_NONE;
        if (frame->scause != RISCV_SCAUSE_BREAKPOINT ||
            frame->sepc !=
                (unsigned long)(uintptr_t)trap_test_bad_return_breakpoint) {
            handler_failures |= TRAP_TEST_HANDLER_CAUSE;
        }
        frame->sstatus &= ~RISCV_SSTATUS_SPP;
        frame->sstatus &= ~RISCV_SSTATUS_SIE;
        return;
    }

    __real_riscv_trap_dispatch(frame);
}

static void print_result(const char *name, unsigned long register_failures)
{
    if (expected_trap != TRAP_TEST_EXPECT_NONE) {
        handler_failures |= TRAP_TEST_HANDLER_MISSING;
    }

    virt_uart_puts("BoarOS: trap return ");
    virt_uart_puts(name);
    virt_uart_puts(" scause=");
    virt_uart_put_hex(observed_scause);
    virt_uart_puts(" sepc=");
    virt_uart_put_hex(observed_sepc);
    virt_uart_puts(" sstatus=");
    virt_uart_put_hex(observed_sstatus);
    virt_uart_puts(" frame=");
    virt_uart_put_hex(observed_frame);
    virt_uart_puts(" handler=");
    virt_uart_put_hex(handler_failures);
    virt_uart_puts(" registers=");
    virt_uart_put_hex(register_failures);
    virt_uart_putc('\n');
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    unsigned long register_failures;

    (void)hart_id;
    (void)dtb;

    expected_trap = TRAP_TEST_EXPECT_BREAKPOINT;
    handler_failures = 0UL;
    register_failures = trap_test_breakpoint_return();
    print_result("breakpoint", register_failures);

    expected_trap = TRAP_TEST_EXPECT_SOFTWARE_INTERRUPT;
    handler_failures = 0UL;
    register_failures = trap_test_software_interrupt_return();
    print_result("software-interrupt", register_failures);

    virt_uart_puts("BoarOS: trap bad return breakpoint=");
    virt_uart_put_hex(
        (unsigned long)(uintptr_t)trap_test_bad_return_breakpoint);
    virt_uart_putc('\n');
    expected_trap = TRAP_TEST_EXPECT_BAD_RETURN;
    handler_failures = 0UL;
    trap_test_bad_return();

    virt_uart_puts("BoarOS: trap bad return escaped validation\n");
    sbi_shutdown();
}
