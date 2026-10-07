#include <arch/riscv/uart_tty.h>
#include <arch/riscv/plic.h>
#include <kernel/page.h>
static int register_irq(void *context,uint32_t source,void (*handler)(void *),void *owner)
{(void)context;return riscv_plic_register(source,handler,owner);}
static void unregister_irq(void *context,uint32_t source,void *owner)
{(void)context;riscv_plic_unregister(source,owner);}
int riscv_uart_tty_start(struct ns16550_port **owner,struct kernel_heap *heap,
    const struct dtb_uart_info *info,volatile void *mapping,uint32_t frequency)
{
    struct ns16550_irq_ops irq={.register_irq=register_irq,.unregister_irq=unregister_irq};
    return ns16550_start(owner,heap,info,mapping,frequency,&irq);
}
unsigned riscv_uart_tty_mapping_ranges(struct dtb_memory_range early,
    const struct dtb_uart_info *info, struct dtb_memory_range ranges[2])
{
    if (!info || !ranges || !early.size || early.base > UINT64_MAX-early.size)
        return 0;
    struct dtb_memory_range spans[2] = {early, info->registers};
    unsigned count = spans[1].size ? 2 : 1;
    for (unsigned i = 0; i < count; i++) {
        if (spans[i].base > UINT64_MAX-spans[i].size) return 0;
        uint64_t end = spans[i].base+spans[i].size;
        if (end > UINT64_MAX-BOAROS_PAGE_MASK) return 0;
        spans[i].base &= ~BOAROS_PAGE_MASK;
        spans[i].size = ((end+BOAROS_PAGE_MASK)&~BOAROS_PAGE_MASK)-spans[i].base;
    }
    if (count == 2 && spans[0].base < spans[1].base+spans[1].size &&
        spans[1].base < spans[0].base+spans[0].size) {
        uint64_t start = spans[0].base < spans[1].base ? spans[0].base : spans[1].base;
        uint64_t a = spans[0].base+spans[0].size, b = spans[1].base+spans[1].size;
        spans[0] = (struct dtb_memory_range){start, (a > b ? a : b)-start};
        count = 1;
    }
    for (unsigned i = 0; i < count; i++) ranges[i] = spans[i];
    return count;
}
