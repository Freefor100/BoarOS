#ifndef BOAROS_KERNEL_LOG_H
#define BOAROS_KERNEL_LOG_H
#include <stddef.h>
#include <stdint.h>
#define KERNEL_LOG_CAPACITY 16384U
#define KERNEL_LOG_RECORD_MAX 1024U
/* Published records are immutable while copied under the short IRQ guard.
 * The callback and its user faults run after that guard is released. */
typedef int (*kernel_log_copy_fn)(void *context, size_t offset,
                                  const char *data, size_t length);
int kernel_log_putc(unsigned level, char character);
int kernel_log_action(int action, int length, int privileged,
                      kernel_log_copy_fn copy, void *context,
                      char scratch[KERNEL_LOG_RECORD_MAX]);
#endif
