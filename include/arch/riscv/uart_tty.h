#ifndef BOAROS_RISCV_UART_TTY_H
#define BOAROS_RISCV_UART_TTY_H
#include <stdint.h>
#include <kernel/dtb.h>
struct kernel_heap;
struct riscv_uart_tty;
struct riscv_uart_statistics {
    uint64_t interrupts, received, overruns, console_dropped, transmitted;
};
/* Mapping spans include the fixed early sink and merge overlapping MMIO pages. */
unsigned riscv_uart_tty_mapping_ranges(struct dtb_memory_range early,
    const struct dtb_uart_info *info, struct dtb_memory_range ranges[2]);
int riscv_uart_tty_start(struct riscv_uart_tty **owner, struct kernel_heap *heap,
    const struct dtb_uart_info *info, volatile void *mapping, uint32_t frequency);
int riscv_uart_tty_stop(struct riscv_uart_tty **owner);
/* Freeze final counters after drain/join; failure or empty owner leaves output unchanged. */
int riscv_uart_tty_stop_report(struct riscv_uart_tty **owner,
                             struct riscv_uart_statistics *statistics);
/* Returns one when a live transport accepted or counted this character. */
int riscv_uart_tty_console(char character);
void riscv_uart_tty_statistics(struct riscv_uart_tty *port,
                              struct riscv_uart_statistics *statistics);
#endif
