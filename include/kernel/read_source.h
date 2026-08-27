#ifndef BOAROS_KERNEL_READ_SOURCE_H
#define BOAROS_KERNEL_READ_SOURCE_H

#include <stddef.h>
#include <stdint.h>

typedef int (*kernel_read_at_fn)(void *context,
                                 uint64_t offset,
                                 void *buffer,
                                 size_t size);

/* read_at returns zero only after filling the complete requested range. */
struct kernel_read_source {
    void *context;
    uint64_t size;
    kernel_read_at_fn read_at;
};

/* Returns zero or a negative Linux-compatible errno value. */
int kernel_read_source_read_exact(const struct kernel_read_source *source,
                                  uint64_t offset,
                                  void *buffer,
                                  size_t size);

/* The caller keeps bytes alive while the resulting source is in use. */
int kernel_read_source_from_memory(const void *bytes,
                                   size_t size,
                                   struct kernel_read_source *source);

#endif
