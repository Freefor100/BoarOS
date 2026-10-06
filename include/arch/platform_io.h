#ifndef BOAROS_ARCH_PLATFORM_IO_H
#define BOAROS_ARCH_PLATFORM_IO_H
#include <stdint.h>
#if defined(BOAROS_ARCH_LOONGARCH)
unsigned arch_uart_rx_ready(void);
char arch_uart_getc(void);
int arch_external_interrupt_active(void);
int arch_rtc_read_ns(uint64_t *);
#define ARCH_RTC_STATUS_OK 0
#else
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/plic.h>
#include <arch/riscv/virt_rtc.h>
#define arch_uart_rx_ready virt_uart_rx_ready
#define arch_uart_getc virt_uart_getc
#define arch_external_interrupt_active riscv_plic_in_interrupt
#define arch_rtc_read_ns riscv_virt_rtc_read_ns
#define ARCH_RTC_STATUS_OK RISCV_VIRT_RTC_STATUS_OK
#endif
#endif
