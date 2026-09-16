#ifndef BOAROS_KERNEL_OPEN_FILE_H
#define BOAROS_KERNEL_OPEN_FILE_H

#include <kernel/page_cache.h>

#include <stddef.h>
#include <stdint.h>

struct kernel_heap;
struct kernel_open_file_description;
struct kernel_vfs_mount;
struct kernel_vfs_node;

/* Borrowed identity, valid while the regular-file OFD is owned. */
struct kernel_vfs_node *kernel_open_file_node(
    const struct kernel_open_file_description *description);

enum kernel_open_file_status {
    KERNEL_OPEN_FILE_STATUS_OK = 0,
    KERNEL_OPEN_FILE_STATUS_INVALID_ARGUMENT,
    KERNEL_OPEN_FILE_STATUS_NO_MEMORY,
    KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED,
    KERNEL_OPEN_FILE_STATUS_STATE,
};

enum kernel_open_file_kind {
    KERNEL_OPEN_FILE_KIND_REGULAR = 0,
    KERNEL_OPEN_FILE_KIND_DIRECTORY,
    KERNEL_OPEN_FILE_KIND_CONSOLE,
    KERNEL_OPEN_FILE_KIND_PIPE,
    KERNEL_OPEN_FILE_KIND_EPOLL,
};

/* VFS errors are returned through linux_result when status is OK. */
enum kernel_open_file_status kernel_open_file_create(
    struct kernel_heap *heap,
    struct kernel_vfs_mount *mount,
    const char *path,
    struct kernel_open_file_description **owner,
    int *linux_result);

enum kernel_open_file_status kernel_open_file_create_nofollow(
    struct kernel_heap *heap,
    struct kernel_vfs_mount *mount,
    const char *path,
    struct kernel_open_file_description **owner,
    int *linux_result);

enum kernel_open_file_status kernel_open_file_create_mode(
    struct kernel_heap *heap,
    struct kernel_vfs_mount *mount,
    const char *path,
    uint32_t mode,
    struct kernel_open_file_description **owner,
    int *linux_result);

enum kernel_open_file_status kernel_open_file_create_executable(
    struct kernel_heap *heap,
    struct kernel_vfs_mount *mount,
    const char *path,
    struct kernel_open_file_description **owner,
    int *linux_result);

/* A console description owns no VFS node and never touches the page cache. */
enum kernel_open_file_status kernel_open_file_create_console(
    struct kernel_heap *heap,
    struct kernel_open_file_description **owner);

enum kernel_open_file_kind kernel_open_file_kind(
    const struct kernel_open_file_description *file);

int kernel_open_file_supports_epoll(
    const struct kernel_open_file_description *file);

enum kernel_open_file_status kernel_open_file_acquire(
    struct kernel_open_file_description *file);

/* Success consumes owner; cleanup failure leaves it retryable. */
enum kernel_open_file_status kernel_open_file_release(
    struct kernel_open_file_description **owner);

uint64_t kernel_open_file_size(
    const struct kernel_open_file_description *file);
uint32_t kernel_open_file_mode(
    const struct kernel_open_file_description *file);
uint32_t kernel_open_file_flags(
    const struct kernel_open_file_description *file);
/* Access mode is OFD state and therefore survives descriptor duplication. */
int kernel_open_file_readable(
    const struct kernel_open_file_description *file);
uint64_t kernel_open_file_offset(
    const struct kernel_open_file_description *file);

enum kernel_open_file_status kernel_open_file_advance(
    struct kernel_open_file_description *file,
    uint64_t bytes);

/* Absolute reposition; the caller validates the requested offset. */
enum kernel_open_file_status kernel_open_file_seek(
    struct kernel_open_file_description *file,
    uint64_t offset);

enum kernel_page_cache_status kernel_open_file_get_page(
    struct kernel_open_file_description *file,
    uint64_t page_index,
    uint64_t *physical_address,
    size_t *valid_bytes);

enum kernel_page_cache_status kernel_open_file_lookup_page(
    struct kernel_open_file_description *file,
    uint64_t page_index,
    uint64_t *physical_address,
    size_t *valid_bytes);

int kernel_open_file_pread(struct kernel_open_file_description *file,
                           uint64_t offset,
                           void *buffer,
                           size_t size,
                           size_t *bytes_read);

#define KERNEL_POLLIN 0x0001U
#define KERNEL_POLLPRI 0x0002U
#define KERNEL_POLLOUT 0x0004U
#define KERNEL_POLLERR 0x0008U
#define KERNEL_POLLHUP 0x0010U
#define KERNEL_POLLNVAL 0x0020U
#define KERNEL_POLLRDNORM 0x0040U
#define KERNEL_POLLRDBAND 0x0080U
#define KERNEL_POLLWRNORM 0x0100U
#define KERNEL_POLLWRBAND 0x0200U

struct kernel_wait_queue;

uint32_t kernel_open_file_poll(
    struct kernel_open_file_description *file,
    uint32_t requested_events,
    struct kernel_wait_queue **out_queue);

#endif
