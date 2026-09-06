#ifndef BOAROS_KERNEL_CONSOLE_H
#define BOAROS_KERNEL_CONSOLE_H

/*
 * Arch-provided console sink for the stdio descriptors.  Callable from any
 * supervisor-mode context; the polling transport makes it synchronous.
 */
void kernel_console_putc(char character);

/*
 * Timer-interrupt hook: wakes one blocked console reader when receive
 * data is pending.  Call with interrupts disabled from the tick path.
 */
void kernel_console_poll_input(void);

#endif
