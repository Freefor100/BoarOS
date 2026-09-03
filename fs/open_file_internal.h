#ifndef BOAROS_FS_OPEN_FILE_INTERNAL_H
#define BOAROS_FS_OPEN_FILE_INTERNAL_H

#include <kernel/open_file.h>
#include <kernel/vfs.h>

struct kernel_open_file_description {
    struct kernel_vfs_file file;
    struct kernel_open_file_description *cleanup_next;
    struct kernel_heap *heap;
    uint64_t offset;
    uint32_t references;
    uint8_t vfs_closed;
};

/* Drop a live container reference without running fallible final cleanup. */
enum kernel_open_file_status kernel_open_file_detach(
    struct kernel_open_file_description **owner);

#endif
