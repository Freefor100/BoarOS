#ifndef BOAROS_FS_OPEN_FILE_INTERNAL_H
#define BOAROS_FS_OPEN_FILE_INTERNAL_H

#include <kernel/open_file.h>
#include <kernel/vfs.h>

struct kernel_epoll;
struct kernel_epoll_item;

struct kernel_open_file_description {
    struct kernel_vfs_file file;
    struct kernel_open_file_description *cleanup_next;
    struct kernel_heap *heap;
    uint64_t offset;
    uint64_t observed_writeback_error;
    uint32_t open_flags;
    uint32_t references;
    uint8_t kind;
    uint8_t vfs_closed;
    struct kernel_pipe *pipe;
    uint8_t pipe_endpoint;
    uint8_t pipe_endpoint_closed;
    struct kernel_epoll *epoll;
    struct kernel_epoll_item *ep_items;
};

enum kernel_open_file_status kernel_open_file_create_pipe(
    struct kernel_heap *heap,
    struct kernel_pipe *pipe,
    uint32_t endpoint,
    uint64_t flags,
    struct kernel_open_file_description **owner);

enum kernel_open_file_status kernel_open_file_create_epoll(
    struct kernel_heap *heap,
    struct kernel_epoll *epoll,
    uint32_t flags,
    struct kernel_open_file_description **owner);

/* Drop a live container reference. Final pipe endpoints close immediately;
 * a real cleanup failure leaves a retry owner, as for other final OFDs. */
enum kernel_open_file_status kernel_open_file_detach(
    struct kernel_open_file_description **owner);

#endif
