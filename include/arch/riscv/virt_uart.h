#ifndef BOAROS_ARCH_RISCV_VIRT_UART_H
#define BOAROS_ARCH_RISCV_VIRT_UART_H

void virt_uart_putc(char character);
void virt_uart_puts(const char *text);
void virt_uart_put_hex(unsigned long value);

#endif
