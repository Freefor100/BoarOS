#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/boot_memory.h>
#include <kernel/dtb.h>

#include <stdint.h>

extern unsigned char __kernel_start[];
extern unsigned char __kernel_end[];

static void shutdown_for_dtb_error(enum dtb_status status)
    __attribute__((noreturn));

static void shutdown_for_dtb_error(enum dtb_status status)
{
    if (status == DTB_STATUS_INVALID) {
        virt_uart_puts("BoarOS: invalid DTB\n");
    } else if (status == DTB_STATUS_NOT_FOUND) {
        virt_uart_puts("BoarOS: DTB memory not found\n");
    } else if (status == DTB_STATUS_UNSUPPORTED) {
        virt_uart_puts("BoarOS: unsupported DTB memory format\n");
    } else {
        virt_uart_puts("BoarOS: unknown DTB error\n");
    }

    sbi_shutdown();
}

static void shutdown_for_boot_memory_error(enum boot_memory_status status)
    __attribute__((noreturn));

static void shutdown_for_boot_memory_error(enum boot_memory_status status)
{
    if (status == BOOT_MEMORY_STATUS_INVALID) {
        virt_uart_puts("BoarOS: invalid boot memory input\n");
    } else if (status == BOOT_MEMORY_STATUS_EMPTY) {
        virt_uart_puts("BoarOS: no usable physical memory\n");
    } else {
        virt_uart_puts("BoarOS: unknown boot memory error\n");
    }

    sbi_shutdown();
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    struct dtb_boot_info info;
    struct boot_memory_layout layout;
    enum dtb_status dtb_status = dtb_read_boot_info(dtb, &info);
    enum boot_memory_status memory_status;

    if (dtb_status != DTB_STATUS_OK) {
        shutdown_for_dtb_error(dtb_status);
    }

    memory_status = boot_memory_build(
        &info,
        (uint64_t)(uintptr_t)__kernel_start,
        (uint64_t)(uintptr_t)__kernel_end,
        (uint64_t)(uintptr_t)dtb,
        &layout);
    if (memory_status != BOOT_MEMORY_STATUS_OK) {
        shutdown_for_boot_memory_error(memory_status);
    }

    virt_uart_puts("BoarOS: booted hart=");
    virt_uart_put_hex(hart_id);
    virt_uart_puts(" dtb=");
    virt_uart_put_hex((unsigned long)dtb);
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: memory base=");
    virt_uart_put_hex((unsigned long)info.memory.base);
    virt_uart_puts(" size=");
    virt_uart_put_hex((unsigned long)info.memory.size);
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: memory layout reserved=");
    virt_uart_put_hex((unsigned long)layout.reserved_count);
    virt_uart_puts(" usable=");
    virt_uart_put_hex((unsigned long)layout.usable_count);
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: first reserved base=");
    virt_uart_put_hex((unsigned long)layout.reserved[0].base);
    virt_uart_puts(" size=");
    virt_uart_put_hex((unsigned long)layout.reserved[0].size);
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: first usable base=");
    virt_uart_put_hex((unsigned long)layout.usable[0].base);
    virt_uart_puts(" size=");
    virt_uart_put_hex((unsigned long)layout.usable[0].size);
    virt_uart_putc('\n');

    sbi_shutdown();
}
