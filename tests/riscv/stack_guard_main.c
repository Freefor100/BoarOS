#include <arch/riscv/memory_layout.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/boot_memory.h>
#include <kernel/dtb.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/stack.h>
#include <kernel/task.h>
#include "../../kernel/sched/private.h"

#include <stdint.h>

extern unsigned char __kernel_start[];
extern unsigned char __kernel_end[];
extern unsigned char __text_start[];
extern unsigned char __text_end[];
extern unsigned char __rodata_start[];
extern unsigned char __rodata_end[];
extern unsigned char __data_start[];
extern unsigned char __data_end[];
extern unsigned char __boot_stack_bottom[];
extern unsigned char __boot_stack_top[];

static struct physical_page_allocator allocator;
static struct riscv_sv39_page_table page_table;

static void *identity_page_access(uint64_t address)
{
    return (void *)(uintptr_t)address;
}

static void fail_setup(void) __attribute__((noreturn));

static void fail_setup(void)
{
    virt_uart_puts("BoarOS: stack guard setup failed\n");
    sbi_shutdown();
}

static enum riscv_sv39_status map_identity(uint64_t start,
                                           uint64_t end,
                                           uint32_t permissions)
{
    if (start > end) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (start == end) {
        return RISCV_SV39_STATUS_OK;
    }

    return riscv_sv39_map_range(&page_table,
                                start,
                                start,
                                end - start,
                                permissions);
}

static int build_page_table(const struct dtb_boot_info *info)
{
    uint64_t memory_start = info->memory.base & ~BOAROS_PAGE_MASK;
    uint64_t memory_end;
    uint64_t text_start = (uint64_t)(uintptr_t)__text_start;
    uint64_t text_end = (uint64_t)(uintptr_t)__text_end;
    uint64_t rodata_start = (uint64_t)(uintptr_t)__rodata_start;
    uint64_t rodata_end = (uint64_t)(uintptr_t)__rodata_end;
    uint64_t data_start = (uint64_t)(uintptr_t)__data_start;
    enum riscv_sv39_status status;

    if (info->memory.size > UINT64_MAX - info->memory.base) {
        return 0;
    }
    memory_end = info->memory.base + info->memory.size;
    if (memory_end > UINT64_MAX - BOAROS_PAGE_MASK) {
        return 0;
    }
    memory_end = (memory_end + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;

    status = riscv_sv39_page_table_init(&page_table, &allocator);
    if (status != RISCV_SV39_STATUS_OK || memory_start > text_start ||
        text_start > text_end || text_end != rodata_start ||
        rodata_start > rodata_end || rodata_end != data_start ||
        (uint64_t)(uintptr_t)__data_end > memory_end) {
        return 0;
    }
    status = map_identity(memory_start,
                          text_start,
                          RISCV_SV39_READ | RISCV_SV39_WRITE);
    if (status == RISCV_SV39_STATUS_OK) {
        status = map_identity(text_start,
                              text_end,
                              RISCV_SV39_READ | RISCV_SV39_EXECUTE);
    }
    if (status == RISCV_SV39_STATUS_OK) {
        status = map_identity(rodata_start, rodata_end, RISCV_SV39_READ);
    }
    if (status == RISCV_SV39_STATUS_OK) {
        status = map_identity(data_start,
                              memory_end,
                              RISCV_SV39_READ | RISCV_SV39_WRITE);
    }
    if (status == RISCV_SV39_STATUS_OK) {
        status = riscv_sv39_map_range(&page_table,
                                      VIRT_UART_MMIO_PHYSICAL_BASE,
                                      VIRT_UART_MMIO_PHYSICAL_BASE,
                                      VIRT_UART_MMIO_SIZE,
                                      RISCV_SV39_READ | RISCV_SV39_WRITE);
    }
    if (status == RISCV_SV39_STATUS_OK) {
        status = riscv_sv39_kernel_window_reserve(
            &page_table,
            RISCV_KERNEL_STACK_WINDOW_BASE,
            RISCV_KERNEL_STACK_WINDOW_SIZE);
    }

    return status == RISCV_SV39_STATUS_OK;
}

static void guard_worker(void *argument)
{
    (void)argument;
    struct kernel_task *task = kernel_task_current();
    uintptr_t low = task->stack_low;
    uintptr_t guard = low - KERNEL_STACK_GUARD_BYTES - BOAROS_PAGE_SIZE;

    virt_uart_puts("BoarOS: stack guard low=");
    virt_uart_put_hex((unsigned long)low);
    virt_uart_puts(" guard=");
    virt_uart_put_hex((unsigned long)guard);
    virt_uart_putc('\n');
    if (low != (uintptr_t)task->stack_high - KERNEL_STACK_BYTES +
                   KERNEL_STACK_GUARD_BYTES ||
        guard + BOAROS_PAGE_SIZE != low - KERNEL_STACK_GUARD_BYTES) {
        virt_uart_puts("BoarOS: stack guard layout mismatch\n");
        sbi_shutdown();
    }

    /* 映射页内的写入必须成功，证明栈本体可用。 */
    *(volatile unsigned char *)(low + 16U) = 0x5aU;
    virt_uart_puts("BoarOS: stack guard mapped write ok\n");

    /* guard 页没有映射，这一写必须产生 store page fault。 */
    *(volatile unsigned char *)guard = 0x5aU;
    virt_uart_puts("BoarOS: stack guard write returned\n");
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    struct dtb_boot_info info;
    struct boot_memory_layout layout;
    struct kernel_thread_completion completion;

    (void)hart_id;

    if (dtb_read_boot_info(dtb, &info) != DTB_STATUS_OK ||
        boot_memory_build(&info,
                          (uint64_t)(uintptr_t)__kernel_start,
                          (uint64_t)(uintptr_t)__kernel_end,
                          (uint64_t)(uintptr_t)dtb,
                          &layout) != BOOT_MEMORY_STATUS_OK ||
        physical_page_allocator_init(&allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_bind_access(&allocator,
                                            identity_page_access) !=
            PHYSICAL_PAGE_STATUS_OK ||
        !build_page_table(&info) ||
        physical_page_allocator_finalize(&allocator) !=
            PHYSICAL_PAGE_STATUS_OK) {
        fail_setup();
    }
    if (riscv_sv39_activate(&page_table) != RISCV_SV39_STATUS_OK ||
        !riscv_sv39_kernel_window_active()) {
        fail_setup();
    }
    if (kernel_scheduler_init(&allocator,
                              (uintptr_t)__boot_stack_bottom,
                              (uintptr_t)__boot_stack_top) !=
        KERNEL_SCHEDULER_STATUS_OK) {
        fail_setup();
    }

    if (kernel_thread_create(guard_worker, 0) != KERNEL_SCHEDULER_STATUS_OK ||
        kernel_scheduler_on_tick(1U) != KERNEL_SCHEDULER_STATUS_OK) {
        fail_setup();
    }
    while (kernel_scheduler_reap_one(&completion) ==
           KERNEL_SCHEDULER_STATUS_OK) { }
    virt_uart_puts("BoarOS: stack guard not enforced\n");
    sbi_shutdown();
}
