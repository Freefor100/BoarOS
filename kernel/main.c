#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>

#define DTB_MAGIC_0 0xd0U
#define DTB_MAGIC_1 0x0dU
#define DTB_MAGIC_2 0xfeU
#define DTB_MAGIC_3 0xedU

static int dtb_has_valid_magic(const void *dtb)
{
    const unsigned char *bytes = dtb;

    return bytes != 0 &&
           bytes[0] == DTB_MAGIC_0 &&
           bytes[1] == DTB_MAGIC_1 &&
           bytes[2] == DTB_MAGIC_2 &&
           bytes[3] == DTB_MAGIC_3;
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    if (!dtb_has_valid_magic(dtb)) {
        virt_uart_puts("BoarOS: invalid DTB handoff\n");
        sbi_shutdown();
    }

    virt_uart_puts("BoarOS: booted hart=");
    virt_uart_put_hex(hart_id);
    virt_uart_puts(" dtb=");
    virt_uart_put_hex((unsigned long)dtb);
    virt_uart_putc('\n');

    sbi_shutdown();
}
