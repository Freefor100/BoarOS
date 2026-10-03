#ifndef BOAROS_KERNEL_CONSOLE_H
#define BOAROS_KERNEL_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Arch-provided console sink for the stdio descriptors.  Callable from any
 * supervisor-mode context; the polling transport makes it synchronous.
 */
void kernel_console_putc(char character);
/* Optional platform emergency sink; fatal diagnostics must not depend on a worker. */
void kernel_console_emergency_begin(void);

/*
 * Timer-interrupt hook: wakes one blocked console reader when receive
 * data is pending.  Call with interrupts disabled from the tick path.
 */
void kernel_console_poll_input(void);

struct kernel_wait_queue;

uint32_t kernel_console_poll(uint32_t requested_events,
                             struct kernel_wait_queue **out_queue);

/* Reads one bounded UART batch into kernel memory; may sleep unless nonblock. */
int kernel_console_read_buffer(uint32_t flags, void *buffer, size_t size,
                               size_t *bytes_read);

#endif
