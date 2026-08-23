#ifndef BOAROS_ARCH_RISCV_VIRT_UART_H
#define BOAROS_ARCH_RISCV_VIRT_UART_H

#define VIRT_UART_MMIO_BASE 0x10000000UL
#define VIRT_UART_MMIO_SIZE 0x1000UL

void virt_uart_putc(char character);
void virt_uart_puts(const char *text);
void virt_uart_put_hex(unsigned long value);

#endif
