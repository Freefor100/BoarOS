#ifndef BOAROS_RISCV_UART_TTY_H
#define BOAROS_RISCV_UART_TTY_H
#include <kernel/ns16550.h>
#define riscv_uart_tty ns16550_port
#define riscv_uart_statistics ns16550_statistics
#define riscv_uart_tty_stop ns16550_stop
#define riscv_uart_tty_stop_report ns16550_stop_report
#define riscv_uart_tty_console ns16550_console
#define riscv_uart_tty_statistics ns16550_statistics
unsigned riscv_uart_tty_mapping_ranges(struct dtb_memory_range,const struct dtb_uart_info *,struct dtb_memory_range[2]);
int riscv_uart_tty_start(struct ns16550_port **,struct kernel_heap *,const struct dtb_uart_info *,volatile void *,uint32_t);
#endif
