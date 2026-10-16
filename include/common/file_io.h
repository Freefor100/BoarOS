/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BOAROS_COMMON_FILE_IO_H
#define BOAROS_COMMON_FILE_IO_H

#include <stddef.h>
#include <stdint.h>
#include <common/block_io.h>

#define FILE_READ_BATCH_MAX 32U
struct file_read_span {
    uint64_t offset;
    void *buffer;
    size_t size, completed;
    int error;
};

struct file_read_fragment { unsigned span; size_t length; int error; };
/* The request owns this reusable planning storage. Each synchronous layer
 * borrows it until all device reads complete; concurrent requests use distinct
 * workspaces, including when a read sleeps with DMA outstanding. */
struct file_read_workspace {
    size_t limits[FILE_READ_BATCH_MAX];
    struct file_read_fragment fragments[FILE_READ_BATCH_MAX];
    struct block_read_span reads[FILE_READ_BATCH_MAX];
    unsigned owners[FILE_READ_BATCH_MAX];
    int mapping_errors[FILE_READ_BATCH_MAX];
};

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
