/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BOAROS_COMMON_BLOCK_IO_H
#define BOAROS_COMMON_BLOCK_IO_H

#include <stddef.h>
#include <stdint.h>

/* Immutable byte ranges borrowed by synchronous block providers until all
 * published DMA completes. Filesystem planners own the descriptor storage. */
struct kernel_block_span {
    uint64_t offset;
    const void *buffer;
    size_t size;
};

/* Physical byte ranges borrowed until synchronous completion. The block
 * provider sets its status; an adapter translates it for its own caller. */
struct block_read_span {
    uint64_t offset;
    void *buffer;
    size_t size, completed;
    unsigned status;
};
/* 可在调用任务内提前发布单项最终结果，每项至多一次；未通知项在同步返回时交付。
 * 回调后不再写该项payload；整批描述、context和缓冲区仍借用至返回。 */
struct block_read_completion {
    void (*complete)(void *context, unsigned index, const struct block_read_span *span);
    void *context;
};

#endif
