#ifndef BOAROS_KERNEL_NS16550_H
#define BOAROS_KERNEL_NS16550_H
#include <stdint.h>
#include <kernel/dtb.h>
struct kernel_heap;
struct ns16550_port;
struct ns16550_statistics {
    uint64_t interrupts, received, overruns, console_dropped, transmitted;
};
struct ns16550_irq_ops {
    void *context;
    int (*register_irq)(void *,uint32_t,void (*)(void *),void *);
    void (*unregister_irq)(void *,uint32_t,void *);
};
int ns16550_start(struct ns16550_port **owner, struct kernel_heap *heap,
    const struct dtb_uart_info *info, volatile void *mapping, uint64_t frequency,
    const struct ns16550_irq_ops *irq);
int ns16550_stop(struct ns16550_port **owner);
/* Freeze final counters after drain/join; failure or empty owner leaves output unchanged. */
int ns16550_stop_report(struct ns16550_port **owner,
                             struct ns16550_statistics *statistics);
/* Returns one when a live transport accepted or counted this character. */
int ns16550_console(char character);
void ns16550_statistics(struct ns16550_port *port,
                              struct ns16550_statistics *statistics);
#endif
