#define _GNU_SOURCE
#include <arch/riscv/virt_uart.h>
#include <kernel/log.h>
#include <kernel/console.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

#define BOAROS_ARCH_RISCV_CONTEXT_H

static unsigned queued, logged, disabled;
uintptr_t riscv_interrupt_save(void) { disabled++; return 0; }
int ns16550_console(char character) { (void)character; queued++; return 1; }
int kernel_log_putc(unsigned level, char character)
{ (void)level; (void)character; logged++; return 0; }
#include "../../arch/riscv/virt_uart.c"
int main(void)
{
    unsigned char *registers = mmap((void *)VIRT_UART_MMIO_PHYSICAL_BASE,
        VIRT_UART_MMIO_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(registers == (void *)VIRT_UART_MMIO_PHYSICAL_BASE);
    registers[5] = 32;
    virt_uart_putc('a'); assert(logged == 1 && !queued && !registers[0]);
    virt_uart_emergency_begin();
    virt_uart_putc('b');
    /* 即使console级别屏蔽且worker不能运行，致命现场也必须直接到硬件。 */
    assert(registers[0] == 'b' && logged == 1 && queued == 0 && disabled);
    kernel_console_putc('c'); assert(registers[0] == 'c' && queued == 0);
    assert(munmap(registers, VIRT_UART_MMIO_SIZE) == 0);
    puts("UART emergency: raw sink bypasses disabled log and queued worker");
}
