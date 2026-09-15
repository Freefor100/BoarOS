#ifndef BOAROS_FS_FILES_PRIVATE_H
#define BOAROS_FS_FILES_PRIVATE_H

#include <kernel/files.h>

#include <stdint.h>

struct kernel_pipe;

enum kernel_files_status kernel_files_read_console(
    struct kernel_files *files, struct kernel_mm *mm, uint64_t user_buffer,
    uint64_t count, int64_t *linux_result);

#define KERNEL_FILES_INITIAL_CAPACITY 32U
#define KERNEL_FILES_MAX_CAPACITY 1024U
#define KERNEL_FILES_FD_CLOEXEC UINT32_C(1)

struct kernel_file_slot {
    struct kernel_open_file_description *description;
    uint32_t flags;
};

struct kernel_files_record {
    struct kernel_file_slot *slots;
    struct kernel_open_file_description *cleanup_files;
    struct kernel_files_statistics statistics;
    uint32_t references;
    uint32_t next_fd;
};

struct kernel_open_file_description *kernel_files_lookup_description(
    struct kernel_files *files,
    int64_t fd);

enum kernel_files_status kernel_files_find_free_fd(
    struct kernel_files *files,
    uint32_t *fd,
    int *linux_result);

/* Success consumes *owner into an empty fd slot.  The new-owned variant is
 * for an OFD created by the caller; the shared-acquired variant is for a
 * reference the caller already acquired.  Neither helper changes refcounts. */
enum kernel_files_status kernel_files_install_new_owned_at(
    struct kernel_files *files,
    uint32_t fd,
    uint32_t fd_flags,
    struct kernel_open_file_description **owner);

enum kernel_files_status kernel_files_install_shared_acquired_at(
    struct kernel_files *files,
    uint32_t fd,
    uint32_t fd_flags,
    struct kernel_open_file_description **owner);

enum kernel_files_status kernel_files_release_allocation(
    struct kernel_files *files,
    void *pointer);

void kernel_files_queue_description(
    struct kernel_files *files,
    struct kernel_open_file_description *description);

enum kernel_files_status kernel_files_drain_file_cleanup(
    struct kernel_files *files);

#endif
