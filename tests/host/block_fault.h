#ifndef BOAROS_TEST_BLOCK_FAULT_H
#define BOAROS_TEST_BLOCK_FAULT_H

#include <kernel/block.h>

struct fault_block_event;

/* The backing file is stable storage. Reads see the volatile image; a power
 * cut discards it. Events are complete 512-byte writes and may be persisted
 * in any chosen order before a cut. A successful flush orders all prior events. */
struct fault_block {
    struct kernel_block_device device;
    int fd;
    unsigned char *visible;
    struct fault_block_event *events;
    size_t count, capacity;
    uint64_t writes, flushes;
    uint64_t fail_write, fail_flush;
};

int fault_block_open(struct fault_block *disk, const char *path);
void fault_block_close(struct fault_block *disk);
int fault_block_persist(struct fault_block *disk, size_t event);
int fault_block_crash(struct fault_block *disk);

#endif
