#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/dtb.h>

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

void kernel_main(unsigned long hart_id, const void *dtb)
{
    struct dtb_memory_range memory;
    enum dtb_status status = dtb_read_first_memory_range(dtb, &memory);

    if (status != DTB_STATUS_OK) {
        shutdown_for_dtb_error(status);
    }

    virt_uart_puts("BoarOS: booted hart=");
    virt_uart_put_hex(hart_id);
    virt_uart_puts(" dtb=");
    virt_uart_put_hex((unsigned long)dtb);
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: memory base=");
    virt_uart_put_hex((unsigned long)memory.base);
    virt_uart_puts(" size=");
    virt_uart_put_hex((unsigned long)memory.size);
    virt_uart_putc('\n');

    sbi_shutdown();
}
