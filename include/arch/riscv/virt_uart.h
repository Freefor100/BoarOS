#ifndef BOAROS_ARCH_RISCV_VIRT_UART_H
#define BOAROS_ARCH_RISCV_VIRT_UART_H

#include <arch/riscv/memory_layout.h>

#define VIRT_UART_MMIO_PHYSICAL_BASE 0x10000000UL
#define VIRT_UART_MMIO_KERNEL_BASE \
    (RISCV_KERNEL_MMIO_BASE + VIRT_UART_MMIO_PHYSICAL_BASE)
#define VIRT_UART_MMIO_SIZE 0x1000UL

void virt_uart_use_kernel_mapping(void);
void virt_uart_putc(char character);
void virt_uart_puts(const char *text);
void virt_uart_put_hex(unsigned long value);
uint32_t virt_uart_rx_ready(void);
char virt_uart_getc(void);

#endif
