#ifndef BOAROS_UART_STARTUP_MMIO_H
#define BOAROS_UART_STARTUP_MMIO_H
#include <stdint.h>
uint8_t fixture_uart_read(unsigned offset);
void fixture_uart_write(unsigned offset, uint8_t value);
#define UART_READ(port, offset) ((void)(port), fixture_uart_read(offset))
#define UART_WRITE(port, offset, value) ((void)(port), fixture_uart_write(offset, value))
#endif
