/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BOAROS_COMMON_BLOCK_IO_H
#define BOAROS_COMMON_BLOCK_IO_H

#include <stddef.h>
#include <stdint.h>

/* Physical byte ranges borrowed until synchronous completion. The block
 * provider sets its status; an adapter translates it for its own caller. */
struct block_read_span {
    uint64_t offset;
    void *buffer;
    size_t size, completed;
    unsigned status;
};

#endif
