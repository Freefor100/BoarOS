#ifndef BOAROS_FS_PIPE_INTERNAL_H
#define BOAROS_FS_PIPE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <kernel/scheduler.h>
#include <kernel/sync.h>
#include <kernel/vfs.h>

struct kernel_heap;
struct physical_page_allocator;
struct kernel_mm;
struct kernel_uaccess_iovec;

/* Shared pipe endpoint and ring ownership. */
struct kernel_pipe {
    struct kernel_heap *heap;
    uint64_t proc_identity;
    uint32_t mode;
    struct kernel_vfs_timespec atime, mtime, ctime;
    struct physical_page_allocator *allocator;
    uint64_t buffer_physical;
    unsigned char *buffer;
    uint16_t head;
    uint16_t tail;
    uint16_t slots;
    uint16_t page_offset[16];
    uint16_t page_length[16];
    uint8_t can_merge[16];
    uint64_t bytes;
    struct kernel_vfs_node *fifo_node; /* 弱关联；每个owner另持节点引用。 */
    uint32_t owners;
    uint64_t reader_generation, writer_generation;
    uint32_t readers;
    uint32_t writers;
    struct kernel_wait_queue read_queue;
    struct kernel_wait_queue write_queue;
    struct kernel_wait_queue both_queue;
    struct kernel_mutex copy_lock;
};

#define KERNEL_PIPE_ENDPOINT_READ 1U
#define KERNEL_PIPE_ENDPOINT_WRITE 2U
#define KERNEL_PIPE_ENDPOINT_BOTH 3U
#define KERNEL_PIPE_NONBLOCK UINT32_C(00004000)
#define KERNEL_PIPE_CAPACITY (UINT64_C(16) * UINT64_C(4096))
#define KERNEL_PIPE_ATOMIC_WRITE UINT64_C(4096)

enum kernel_pipe_status {
    KERNEL_PIPE_STATUS_OK = 0,
    KERNEL_PIPE_STATUS_INVALID_ARGUMENT,
    KERNEL_PIPE_STATUS_NO_MEMORY,
    KERNEL_PIPE_STATUS_STATE,
};

struct kernel_pipe;

enum kernel_pipe_status kernel_pipe_create(
    struct kernel_heap *heap,
    struct kernel_pipe **owner);

int kernel_pipe_fifo_open(struct kernel_heap *heap, struct kernel_vfs_node *node,
    uint32_t flags, struct kernel_pipe **owner, uint8_t *endpoint,
    uint64_t *observed_writers);

uint64_t kernel_pipe_proc_identity(const struct kernel_pipe *pipe);
int kernel_pipe_stat(const struct kernel_pipe *pipe, struct kernel_vfs_stat *stat);
int kernel_pipe_set_mode(struct kernel_pipe *pipe, uint32_t mode);

/* Disposes a newly-created pipe before either endpoint is attached. */
enum kernel_pipe_status kernel_pipe_destroy_unowned(
    struct kernel_pipe *pipe);

enum kernel_pipe_status kernel_pipe_acquire_endpoint(
    struct kernel_pipe *pipe,
    uint32_t endpoint);

/* The final endpoint also releases the contiguous order-4 backing block. */
enum kernel_pipe_status kernel_pipe_release_endpoint(
    struct kernel_pipe *pipe,
    uint32_t endpoint);

enum kernel_pipe_status kernel_pipe_readv(
    struct kernel_pipe *pipe,
    struct kernel_mm *mm,
    const struct kernel_uaccess_iovec *iov,
    size_t iov_count,
    uint64_t count,
    uint32_t open_flags,
    int64_t *linux_result);

enum kernel_pipe_status kernel_pipe_writev(
    struct kernel_pipe *pipe,
    struct kernel_mm *mm,
    const struct kernel_uaccess_iovec *iov,
    size_t iov_count,
    uint64_t count,
    uint32_t open_flags,
    int64_t *linux_result);

uint32_t kernel_pipe_poll(
    struct kernel_pipe *pipe,
    uint32_t endpoint,
    uint64_t observed_writers,
    struct kernel_wait_queue **out_queue);

enum kernel_pipe_status kernel_pipe_write_buffer(
    struct kernel_pipe *pipe, const void *buffer, size_t count,
    uint32_t open_flags, int64_t *linux_result);

#endif
