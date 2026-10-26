#ifndef BOAROS_FS_OPEN_FILE_INTERNAL_H
#define BOAROS_FS_OPEN_FILE_INTERNAL_H

#include <kernel/open_file.h>
#include <kernel/vfs.h>
#include <kernel/sync.h>

struct kernel_epoll;
struct kernel_epoll_item;
struct kernel_record_lock;
struct kernel_char_device;
struct kernel_block_device;

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
    uint64_t proc_identity;
    uint32_t open_flags;
    uint32_t references;
    uint8_t kind;
    uint8_t vfs_closed;
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
    char *generated_data;
    size_t generated_length;
    uint8_t generated_ready;
    const struct kernel_credentials *credentials;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    uint64_t read_end,readahead_cookie,readahead_end; /* Last field is a page boundary. */
    unsigned readahead_pages;
    uint8_t read_sequential;
#endif
};
/* 已有OFD owner下的私有域验证，不为未映射普通文件分配映射登记状态。 */
enum kernel_open_file_status kernel_open_file_check_io_domain(
    const struct kernel_open_file_description *,const struct kernel_heap *,
    const struct physical_page_allocator *);
/* 只用于真正的后端操作；普通缓存访问由调用CPU执行。 */
int kernel_open_file_io_requires_cpu0(const struct kernel_open_file_description *);

/* Caller holds offset_lock for a shared OFD snapshot. */
int kernel_open_file_generate(struct kernel_open_file_description *file);
void kernel_open_file_reset_generated(struct kernel_open_file_description *file);

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
