#define _POSIX_C_SOURCE 200809L
#include "block_fault.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct fault_block_event {
    uint64_t offset;
    unsigned char data[512];
};

static enum kernel_block_status read_bytes(void *context, uint64_t offset,
                                          void *buffer, size_t size)
{
    struct fault_block *disk = context;
    memcpy(buffer, disk->visible + offset, size);
    return KERNEL_BLOCK_STATUS_OK;
}

static enum kernel_block_status write_bytes(void *context, uint64_t offset,
                                           const void *buffer, size_t size)
{
    struct fault_block *disk = context;
    const unsigned char *bytes = buffer;
    if (++disk->writes == disk->fail_write) return KERNEL_BLOCK_STATUS_IO;
    size_t sectors = (offset % 512 + size + 511) / 512;
    if (sectors > SIZE_MAX - disk->count ||
        disk->count + sectors > SIZE_MAX / sizeof(*disk->events))
        return KERNEL_BLOCK_STATUS_NO_MEMORY;
    if (disk->count + sectors > disk->capacity) {
        size_t capacity = disk->count + sectors;
        void *events = realloc(disk->events, capacity * sizeof(*disk->events));
        if (!events) return KERNEL_BLOCK_STATUS_NO_MEMORY;
        disk->events = events;
        disk->capacity = capacity;
    }
    while (size) {
        size_t within = offset % 512;
        size_t amount = size < 512 - within ? size : 512 - within;
        struct fault_block_event *event = &disk->events[disk->count++];
        event->offset = offset - within;
        memcpy(disk->visible + offset, bytes, amount);
        memcpy(event->data, disk->visible + event->offset, 512);
        offset += amount;
        bytes += amount;
        size -= amount;
    }
    return KERNEL_BLOCK_STATUS_OK;
}

int fault_block_persist(struct fault_block *disk, size_t event)
{
    if (event >= disk->count) return -1;
    struct fault_block_event *entry = &disk->events[event];
    return pwrite(disk->fd, entry->data, 512, (off_t)entry->offset) == 512
               ? 0 : -1;
}

static enum kernel_block_status flush_bytes(void *context)
{
    struct fault_block *disk = context;
    if (++disk->flushes == disk->fail_flush) return KERNEL_BLOCK_STATUS_IO;
    for (size_t i = 0; i < disk->count; i++)
        if (fault_block_persist(disk, i)) return KERNEL_BLOCK_STATUS_IO;
    if (fdatasync(disk->fd)) return KERNEL_BLOCK_STATUS_IO;
    disk->count = 0;
    return KERNEL_BLOCK_STATUS_OK;
}

int fault_block_crash(struct fault_block *disk)
{
    size_t length = disk->device.capacity_bytes;
    if (pread(disk->fd, disk->visible, length, 0) != (ssize_t)length) return -1;
    disk->count = 0;
    return 0;
}

int fault_block_open(struct fault_block *disk, const char *path)
{
    struct stat stat;
    memset(disk, 0, sizeof(*disk));
    disk->fd = open(path, O_RDWR);
    if (disk->fd < 0) return -1;
    if (fstat(disk->fd, &stat) || stat.st_size <= 0 || stat.st_size % 512 ||
        (uint64_t)stat.st_size > SIZE_MAX) goto Fail;
    disk->visible = malloc((size_t)stat.st_size);
    if (!disk->visible) goto Fail;
    disk->device = (struct kernel_block_device) {
        .context = disk, .read = read_bytes, .write = write_bytes,
        .capacity_bytes = (uint64_t)stat.st_size, .logical_block_size = 512,
        .flush = flush_bytes, .cache_mode = KERNEL_BLOCK_CACHE_WRITEBACK,
    };
    if (fault_block_crash(disk)) goto Fail;
    return 0;
Fail:
    fault_block_close(disk);
    return -1;
}

void fault_block_close(struct fault_block *disk)
{
    if (disk->fd >= 0) close(disk->fd);
    free(disk->visible);
    free(disk->events);
    memset(disk, 0, sizeof(*disk));
    disk->fd = -1;
}
