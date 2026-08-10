#include <arch/riscv/virt_uart.h>

#define VIRT_UART_BASE 0x10000000UL
#define UART_THR 0UL
#define UART_LSR 5UL
#define UART_LSR_THR_EMPTY (1U << 5)

static volatile unsigned char *uart_register(unsigned long offset)
{
    return (volatile unsigned char *)(VIRT_UART_BASE + offset);
}

void virt_uart_putc(char character)
{
    while ((*uart_register(UART_LSR) & UART_LSR_THR_EMPTY) == 0U) {
    }

    *uart_register(UART_THR) = (unsigned char)character;
}

void virt_uart_puts(const char *text)
{
    while (*text != '\0') {
        virt_uart_putc(*text);
        text++;
    }
}

void virt_uart_put_hex(unsigned long value)
{
    static const char digits[] = "0123456789abcdef";
    char buffer[sizeof(value) * 2U];
    unsigned int length = 0;

    virt_uart_puts("0x");
    do {
        buffer[length] = digits[value & 0xfUL];
        length++;
        value >>= 4;
    } while (value != 0UL);

    while (length != 0U) {
        length--;
        virt_uart_putc(buffer[length]);
    }
}
