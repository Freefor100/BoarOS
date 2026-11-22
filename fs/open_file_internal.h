#ifndef BOAROS_FS_OPEN_FILE_INTERNAL_H
#define BOAROS_FS_OPEN_FILE_INTERNAL_H

#include <kernel/open_file.h>
#include <kernel/vfs.h>
#include <kernel/sync.h>
#include "flock.h"

struct kernel_epoll;
struct kernel_epoll_item;
struct kernel_record_lock;
struct kernel_char_device;
struct kernel_block_device;
struct kernel_async_io;
struct kernel_namespace;
struct kernel_mm_pagemap;
enum kernel_proc_page_stream {
    KERNEL_PROC_PAGE_STREAM_NONE,
    KERNEL_PROC_PAGE_STREAM_PAGEMAP,
    KERNEL_PROC_PAGE_STREAM_FLAGS,
};

int kernel_open_file_pread_mode(struct kernel_open_file_description *file,
    uint64_t offset, void *buffer, size_t size, uint32_t status_flags, size_t *bytes_read);

struct kernel_open_file_description {
    struct kernel_vfs_file file;
    struct kernel_open_file_description *cleanup_next;
    struct kernel_heap *heap;
    /* read/readv与位置操作的调用者持有；CPU0请求只借用已有资格，
     * 不能先进入executor后等待此锁，否则会阻挡持锁读者的cache miss。 */
    struct kernel_mutex offset_lock;
    uint64_t offset;
    uint64_t observed_writeback_error;
    uint64_t observed_mount_error;
    union {
        uint64_t proc_identity; /* Pathless socket/namespace inode, never a generated file. */
        struct kernel_pid_registry *ipc_proc_pid_view; /* Valid iff ipc_proc_namespace != NULL. */
    };
    uint32_t open_flags;
    uint32_t references;
    struct kernel_async_io *async_io; /* On demand; shared by all OFD aliases. */
    uint8_t kind;
    uint8_t vfs_closed;
    uint8_t notify_opened;
    struct kernel_pipe *pipe;
    uint64_t pipe_observed_writers;
    uint8_t pipe_endpoint;
    uint8_t pipe_endpoint_closed;
    struct kernel_epoll *epoll;
    struct kernel_socket *socket;
    struct kernel_epoll_item *ep_items;
    struct kernel_record_lock *record_locks;
    struct kernel_block_device *block;
    const struct kernel_char_device *device;
    void *device_instance;
    uint8_t device_opened;
    union {
        char *generated_data;
        struct kernel_mm_pagemap *proc_pagemap;
    };
    size_t generated_length;
    uint64_t generated_read_offset;
    uint8_t generated_ready;
    uint8_t proc_page_stream;
    const struct kernel_credentials *credentials;
    struct kernel_namespace *ipc_proc_namespace; /* /proc/sysvipc discovery owner from open. */
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    uint64_t read_end,readahead_cookie,readahead_end; /* Last field is a page boundary. */
    unsigned readahead_pages;
    uint8_t read_sequential;
#endif
    struct kernel_flock_owner flock_owner;
    struct kernel_flock_identity flock_identity; /* Pathless private inode. */
    uint32_t read_advice; /* Shared by dup/fork/SCM_RIGHTS, independently of status flags. */
    struct kernel_vfs_mount *mount_tree; /* Borrowed through file.path; one detached tree owner. */
    struct kernel_vfs_path *notify_backing; /* Backing path opened by this stacked file. */
    struct kernel_vfs_path *mapping_path; /* Visible name of an internal backing description. */
};
enum { KERNEL_READ_RANDOM=1, KERNEL_READ_SEQUENTIAL=2, KERNEL_READ_NOREUSE=4 };
static inline int kernel_open_file_read_no_reuse(const struct kernel_open_file_description *file)
{ return !!(__atomic_load_n(&file->read_advice,__ATOMIC_RELAXED)&KERNEL_READ_NOREUSE); }
struct kernel_flock_identity *kernel_open_file_flock_identity(
    struct kernel_open_file_description *);
/* 已有OFD owner下的私有域验证，不为未映射普通文件分配映射登记状态。 */
enum kernel_open_file_status kernel_open_file_check_io_domain(
    const struct kernel_open_file_description *,const struct kernel_heap *,
    const struct physical_page_allocator *);
/* 只用于真正的后端操作；普通缓存访问由调用CPU执行。 */
int kernel_open_file_io_requires_cpu0(const struct kernel_open_file_description *);

/* Caller holds offset_lock for a shared OFD snapshot. */
int kernel_open_file_generate(struct kernel_open_file_description *file);
void kernel_open_file_reset_generated(struct kernel_open_file_description *file);

int kernel_open_file_create_memfd(struct kernel_heap *, const char *, uint32_t,
    struct kernel_open_file_description **);
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


#endif
