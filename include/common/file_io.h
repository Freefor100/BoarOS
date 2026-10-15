/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BOAROS_COMMON_FILE_IO_H
#define BOAROS_COMMON_FILE_IO_H

#include <stddef.h>
#include <stdint.h>

/* Immutable caller storage. Context, callbacks and the entire retained
 * allocation size stay fixed until its final borrower releases the owner. */
struct file_data_source {
    void *context;
    void (*retain)(void *context);
    void (*release)(void *context);
    size_t bytes;
};

/* The synchronous caller owns these descriptors; a backend may retain source
 * storage beyond return. Surrounding initialized bytes provide block padding;
 * only offset/size advance file size and written reports logical completion. */
struct file_write_span {
    uint64_t offset;
    const void *buffer;
    size_t size, written;
    struct file_data_source source;
    const void *source_buffer;
    size_t source_size;
};

#endif
