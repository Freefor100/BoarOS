#include "lwext4_port.h"
#include "ext4_backend.h"
#include "vfs_internal.h"
#include "record_lock.h"

#include <kernel/cost.h>
#include <kernel/block.h>
#include <kernel/errno.h>
#include <kernel/file_mapping.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/irq.h>
#include <kernel/page_cache.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/vfs.h>
#include <kernel/time.h>
#include <arch/context.h>

#include <ext4.h>
#include <ext4_bcache.h>
#include <ext4_blockdev.h>
#include <ext4_inode.h>
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <ext4_orphan.h>
#include <ext4_misc.h>
#include <ext4_super.h>
#include <ext4_types.h>
#include <string.h>

#include <stddef.h>
#include <stdint.h>

static struct lwext4_mount_adapter *adapters;
static uintptr_t adapter_serial;

/* lwext4 内部前缀不暴露给 VFS；路径缓冲随本次操作释放。 */
struct backend_path { struct kernel_heap *heap; char *value; };
static void backend_path_release(struct backend_path *path)
{
    if (path->value) kernel_heap_release(path->heap, path->value);
}
static int backend_path_make(struct lwext4_mount_adapter *adapter,
                            const char *path, struct backend_path *out)
{
    if (!path) return 0;
    size_t prefix = strlen(adapter->mount_point), length = strlen(path);
    if (length > SIZE_MAX - prefix) return -KERNEL_ENAMETOOLONG;
    out->heap = adapter->instance.heap;
    if (kernel_heap_allocate(out->heap, prefix + length, (void **)&out->value)
            != KERNEL_HEAP_STATUS_OK) return -KERNEL_ENOMEM;
    memcpy(out->value, adapter->mount_point, prefix);
    memcpy(out->value + prefix, path + 1, length);
    return 0;
}
#define BACKEND_PATH(adapter, path) \
    struct backend_path translated __attribute__((cleanup(backend_path_release))) = {0}; \
    int translated_status = backend_path_make(adapter, path, &translated); \
    if (translated_status) return translated_status; \
    path = translated.value

static void adapter_names(struct lwext4_mount_adapter *adapter)
{
    static const char digits[] = "0123456789abcdef";
    uintptr_t serial = ++adapter_serial;
    if (!serial) __builtin_trap();
    memcpy(adapter->device_name, "ext4-", 5);
    for (unsigned i = 0; i < sizeof(serial) * 2; i++)
        adapter->device_name[5 + i] = digits[(serial >> ((sizeof(serial) * 2 - 1 - i) * 4)) & 15];
    adapter->mount_point[0] = '/';
    size_t length = strlen(adapter->device_name);
    memcpy(adapter->mount_point + 1, adapter->device_name, length);
    adapter->mount_point[length + 1] = '/';
}

static int mount_error(const struct lwext4_mount_adapter *adapter);
static int prepare_journal_mount(struct lwext4_mount_adapter *adapter);
static void complete_sync_waiters(struct lwext4_mount_adapter *adapter);
static bool vfs_realtime(struct ext4_timestamp *now);
static int lwext4_error(int error);
static int block_open(struct ext4_blockdev *device);
static int block_read(struct ext4_blockdev *device,
                      void *buffer,
                      uint64_t block,
                      uint32_t count);
static int block_write(struct ext4_blockdev *device,
                       const void *buffer,
                       uint64_t block,
                       uint32_t count);
static int block_write_batch(struct ext4_blockdev *device,
                            const struct ext4_block_span *spans, unsigned count);
static int block_close(struct ext4_blockdev *device);
static int block_flush(struct ext4_blockdev *device);
static int release_mount_storage(struct kernel_vfs_mount *mount);
static void queue_orphan(struct lwext4_mount_adapter *adapter,
                         struct lwext4_orphan *orphan);
static void release_orphan(struct lwext4_mount_adapter *adapter,
                           struct lwext4_orphan *orphan);
static int reserve_orphan(struct lwext4_mount_adapter *adapter,
                          struct lwext4_orphan **owner);
static int cleanup_mount(struct kernel_vfs_mount *mount);
int kernel_vfs_mount_root(struct kernel_vfs_mount *mount,
                          struct kernel_block_device *block,
                          struct kernel_heap *heap,
                          struct kernel_page_cache *page_cache);
static int valid_utime_nsec(int64_t value);
static int inode_extra_field_present(struct ext4_sblock *superblock,
                                     struct ext4_inode *inode,
                                     size_t field_offset,
                                     size_t field_size);
static struct kernel_vfs_timespec decode_inode_time(
    uint32_t base, uint32_t extra, int has_extra);
static void fill_stat(struct kernel_vfs_mount *mount,
                      struct ext4_sblock *superblock,
                      uint32_t inode_number,
                      struct ext4_inode *inode,
                      struct kernel_vfs_stat *stat);
static uint8_t dirent_type_from_ext4(uint8_t inode_type);
static int ext4_backend_unmount(struct kernel_vfs_mount *mount);
static int ext4_backend_statfs(struct kernel_vfs_mount *mount,
                            struct kernel_vfs_statfs *stat);
static int ext4_backend_set_times(struct kernel_vfs_file *file,
                              const struct kernel_vfs_timespec times[2]);
static int ext4_backend_set_mode(struct kernel_vfs_file *file, uint32_t mode);
static int ext4_backend_set_owner(struct kernel_vfs_file *file, uint32_t uid, uint32_t gid);

static int ext4_backend_open(struct kernel_vfs_mount *mount,
                        const char *path, uint64_t inode_number,
                        uint32_t inode_mode,
                        struct kernel_vfs_file *file);
static int ext4_backend_create(struct kernel_vfs_mount *mount,
                          uint64_t parent_inode, const char *path,
                          uint32_t mode,
                          struct kernel_vfs_file *file);
static void ext4_backend_accessed(struct kernel_vfs_file *file);
static int ext4_backend_modified(struct kernel_vfs_file *file,
                              uint64_t offset, int append);
static int ext4_backend_stat(const struct kernel_vfs_file *file,
                     struct kernel_vfs_stat *stat);
static int ext4_backend_mkdir(struct kernel_vfs_mount *mount,
                         uint64_t parent_inode, const char *path,
                         uint32_t mode);
/* The namespace lock holds names, not cache-only registry ancestors. Pin the
 * indexed target before reclaim can rotate the tree or retire its last owner. */
static struct kernel_vfs_node *pin_linked_node(struct lwext4_mount_adapter *adapter, uint32_t inode)
{
    struct kernel_vfs_node *node = kernel_vfs_node_pin(&adapter->instance, inode);
    if (node && node->unlinked && kernel_vfs_node_release(&node)) __builtin_trap();
    return node;
}

static int ext4_backend_unlink(struct kernel_vfs_mount *mount,
                          uint64_t parent_inode, const char *path);
static int ext4_backend_rmdir(struct kernel_vfs_mount *mount,
                         uint64_t parent_inode, const char *path);
static void ext4_backend_release_unlinked(struct kernel_vfs_node *node);
static int ext4_backend_dir_entry(struct kernel_vfs_file *file,
                         uint64_t position,
                         uint64_t *next_position,
                         uint64_t *inode,
                         uint8_t *type,
                         char *name,
                         size_t name_size);
static int ext4_backend_pread(struct kernel_vfs_node *node,
                          uint64_t offset,
                          void *buffer,
                          size_t size,
                          size_t *bytes_read);
static int ext4_backend_writeback(struct kernel_vfs_node *node,
    struct file_write_span *spans, size_t count);

static struct kernel_vfs_backend ext4_backend;
static int ext4_check_domain(const struct kernel_vfs_node *node,
    const struct kernel_heap *heap, const struct physical_page_allocator *allocator)
{
    if (!node || !node->instance || !node->mount ||
        node->closed || node->retired || (node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG ||
        node->instance->ops != &ext4_backend || node->mount->private_data != node->instance ||
        !kernel_page_cache_check_domain(node->instance->page_cache,heap,allocator)) return -KERNEL_EINVAL;
    return 0;
}

static void initialize_backend(void);
static int mount_error(const struct lwext4_mount_adapter *adapter)
{
    int error=__atomic_load_n(&adapter->mount_error,__ATOMIC_ACQUIRE);
    if (error) return error;
    struct ext4_fs *fs = adapter->device.fs;
    return fs && fs->jbd_journal ? __atomic_load_n(&fs->jbd_journal->error,__ATOMIC_ACQUIRE) : EOK;
}

static void journal_wake(struct kernel_wait_queue *queue)
{
    KERNEL_IRQ_SCOPE(irq);
    if (kernel_wait_queue_wake_all(queue)) __builtin_trap();
}

/* Only the backend writer publishes these values. Waiters borrow the short
 * control domain instead of reading a live transaction while its lock sleeps. */
static int journal_publish_locked(struct lwext4_mount_adapter *adapter)
{
    struct ext4_fs *fs = adapter->device.fs;
    struct jbd_journal *journal = fs ? fs->jbd_journal : NULL;
    if (!journal || !journal->grouped) return 0;
    int error = adapter->published_error ? adapter->published_error : mount_error(adapter);
    if (adapter->published_sealed == journal->sealed_sequence &&
        adapter->published_durable == journal->durable_sequence &&
        adapter->published_checkpoint == journal->checkpoint_sequence &&
        adapter->published_error == error) return 0;
    int committed = adapter->published_durable != journal->durable_sequence;
    int reclaimed = adapter->published_checkpoint != journal->checkpoint_sequence;
    {
        KERNEL_TASK_LOCK_SCOPE(guard, &adapter->journal_control);
        adapter->published_sealed = journal->sealed_sequence;
        adapter->published_durable = journal->durable_sequence;
        adapter->published_checkpoint = journal->checkpoint_sequence;
        adapter->published_error = error;
        if (committed) adapter->checkpoint_requested = 1;
        if (reclaimed) adapter->journal_requested = 1;
    }
    journal_wake(&adapter->journal_progress);
    if (committed) journal_wake(&adapter->checkpoint_work);
    if (reclaimed) journal_wake(&adapter->journal_work);
    return 1;
}

static void backend_unlock(void *lock)
{
    struct lwext4_mount_adapter *adapter = (void *)((char *)lock -
        offsetof(struct lwext4_mount_adapter, backend_lock));
    struct kernel_io_context *context = kernel_io_context_current();
    int changed = context->backend_depth == 1 && !context->backend_read &&
        journal_publish_locked(adapter);
    boaros_lwext4_unlock(lock);
    if (changed) complete_sync_waiters(adapter);
}

static void journal_request_flags(struct lwext4_mount_adapter *adapter, int force)
{
    {
        KERNEL_TASK_LOCK_SCOPE(guard, &adapter->journal_control);
        adapter->journal_requested = adapter->checkpoint_requested = 1;
        if (force) adapter->journal_force = 1;
    }
    journal_wake(&adapter->journal_work);
    journal_wake(&adapter->checkpoint_work);
}

static void journal_stop_workers(struct lwext4_mount_adapter *adapter)
{
    {
        KERNEL_TASK_LOCK_SCOPE(guard, &adapter->journal_control);
        adapter->journal_stopping = 1;
    }
    journal_wake(&adapter->journal_work);
    journal_wake(&adapter->checkpoint_work);
    journal_wake(&adapter->journal_progress);
}

static void journal_idle(struct lwext4_mount_adapter *adapter, int checkpoint, uint64_t deadline)
{
    struct kernel_wait_token token;
    int needed;
    {
        KERNEL_TASK_LOCK_SCOPE(guard, &adapter->journal_control);
        needed = !adapter->journal_stopping &&
            !(checkpoint ? adapter->checkpoint_requested : adapter->journal_requested);
        if (needed && kernel_wait_prepare(checkpoint ? &adapter->checkpoint_work :
            &adapter->journal_work, deadline, 0, &token)) __builtin_trap();
    }
    if (needed) {
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token, &reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
}
static int free_orphan_inode(struct lwext4_mount_adapter *adapter,uint32_t inode)
{
    int result=ext4_orphan_free(adapter->mount_point,inode);
    if (!result) {
        /* 失败 close 的旧 handle 不再进入 inode 索引。真正释放 inode 时
         * 同时退休它的待清理状态，不能在该 inode 复用后再截断新所有者。 */
        KERNEL_TASK_LOCK_SCOPE(state, &kernel_vfs_state_lock);
        for (struct kernel_vfs_node *node=adapter->instance.cleanup_nodes;node;node=node->next)
            if (node->inode==inode) {
                ((struct lwext4_node *)node)->write_completion=0;
                node->retired=1;
            }
    }
    return result;
}

static int prepare_journal_mount(struct lwext4_mount_adapter *adapter)
{
    struct ext4_fs *fs = adapter->device.fs;
    int result = EOK;
    if (!fs->jbd_journal) {
        if (fs->jbd_fs)
            result = ext4_journal_stop(adapter->mount_point);
        if (result == EOK) result = ext4_recover(adapter->mount_point);
        if (result == EOK) result = ext4_orphan_validate(fs);
        if (result == EOK) result = ext4_journal_start(adapter->mount_point);
    }
    if (result == EOK) result = ext4_orphan_recover(adapter->mount_point);
    if (result == EOK)
        adapter->recovery_pending = 0U;
    else if (result != ENOMEM && result != ENOSPC)
        __atomic_store_n(&adapter->mount_error,result,__ATOMIC_RELEASE);
    return result;
}

static bool vfs_realtime(struct ext4_timestamp *now)
{
    uint64_t nanoseconds;
    if (!kernel_time_is_initialized()) return false;
    nanoseconds = kernel_time_realtime_ns();
    now->seconds = (int64_t)(nanoseconds / UINT64_C(1000000000));
    now->nanoseconds = (uint32_t)(nanoseconds % UINT64_C(1000000000));
    return true;
}

/* SMP readers share the mount lock; a single-hart build only needs IRQ
 * exclusion for these nonblocking updates. Final node releases stay outside. */
#if BOAROS_MAX_CPUS > 1
#define TIMESTAMP_SCOPE(name, adapter) KERNEL_RAW_SCOPE(name, &(adapter)->timestamp_lock)
#else
#define TIMESTAMP_SCOPE(name, adapter) KERNEL_IRQ_SCOPE(name); (void)(adapter)
#endif
static int time_compare(struct ext4_timestamp a, struct ext4_timestamp b)
{
    if (a.seconds != b.seconds) return a.seconds < b.seconds ? -1 : 1;
    return (a.nanoseconds > b.nanoseconds) - (a.nanoseconds < b.nanoseconds);
}

static struct ext4_timestamp normalize_time(struct lwext4_node *node,
                                            unsigned field, struct ext4_timestamp time)
{
    int extended = node->extra_times & (1U << field);
    int64_t maximum = extended ? INT64_C(15032385535) : INT32_MAX;
    if (time.seconds <= INT32_MIN) { time.seconds = INT32_MIN; time.nanoseconds = 0; }
    if (time.seconds >= maximum) { time.seconds = maximum; time.nanoseconds = 0; }
    if (!extended) time.nanoseconds = 0;
    return time;
}

static void time_unlink_dirty(struct lwext4_node *node)
{
    struct lwext4_mount_adapter *adapter = lwext4_instance(node->node.instance);
    *node->dirty_previous = node->dirty_next;
    if (node->dirty_next) node->dirty_next->dirty_previous = node->dirty_previous;
    else adapter->dirty_times_tail = node->dirty_previous;
    node->dirty_previous = 0;
    node->dirty_next = 0;
    if (!adapter->dirty_times_count) __builtin_trap();
    adapter->dirty_times_count--;
    if (!adapter->dirty_times) adapter->times_deadline = 0;
}

static void time_append_dirty(struct lwext4_node *node)
{
    struct lwext4_mount_adapter *adapter = lwext4_instance(node->node.instance);
    if (!adapter->dirty_times_tail) adapter->dirty_times_tail = &adapter->dirty_times;
    node->dirty_previous = adapter->dirty_times_tail;
    *adapter->dirty_times_tail = node;
    adapter->dirty_times_tail = &node->dirty_next;
    adapter->dirty_times_count++;
}

static struct kernel_vfs_node *time_drop_dirty(struct lwext4_node *node)
{
    if (node->dirty_fields || !node->dirty_previous) return NULL;
    time_unlink_dirty(node);
    return &node->node;
}

/* Caller holds timestamp_lock. The acquired list pin is atomic; wakeups and
 * final node releases stay outside this short cross-hart publication. */
static int time_dirty_locked(struct lwext4_node *node, unsigned fields,
                             struct ext4_timestamp now)
{
    struct lwext4_mount_adapter *adapter = lwext4_instance(node->node.instance);
    int notify = 0;
    for (unsigned i = 0; i < 3; i++) if (fields & (1U << i)) {
        struct ext4_timestamp value = normalize_time(node, i, now);
        if (!time_compare(node->times[i], value)) continue;
        node->times[i] = value;
        if (!++node->time_version[i]) __builtin_trap();
        node->dirty_fields |= 1U << i;
    }
    if (node->dirty_fields && !node->dirty_previous) {
        if (kernel_vfs_node_acquire(&node->node)) __builtin_trap();
        time_append_dirty(node);
        if (!adapter->times_deadline) {
            adapter->times_deadline = kernel_time_monotonic_ns() + UINT64_C(100000000);
            notify = adapter->journal_started;
        }
    }
    return notify;
}

static void time_notify(struct lwext4_mount_adapter *adapter, int notify)
{
    if (!notify) return;
    {
        KERNEL_TASK_LOCK_SCOPE(guard, &adapter->journal_control);
        adapter->journal_requested = 1;
    }
    journal_wake(&adapter->journal_work);
}

static void time_dirty(struct lwext4_node *node, unsigned fields,
                        struct ext4_timestamp now)
{
    struct lwext4_mount_adapter *adapter = lwext4_instance(node->node.instance);
    int notify;
    {
        TIMESTAMP_SCOPE(times, adapter);
        notify = time_dirty_locked(node, fields, now);
    }
    time_notify(adapter, notify);
}

static uint64_t time_deadline(struct lwext4_mount_adapter *adapter)
{
    TIMESTAMP_SCOPE(times, adapter);
    return adapter->times_deadline;
}

static void time_stage(void *context, void *owner, uint32_t inode, unsigned field,
                       struct ext4_timestamp value)
{
    struct lwext4_mount_adapter *adapter = context;
    struct kernel_vfs_node *pin = owner ? 0 : kernel_vfs_node_pin(&adapter->instance, inode);
    struct kernel_vfs_node *base = owner ? owner : pin;
    {
    TIMESTAMP_SCOPE(times, adapter);
    if (base && (base->instance != &adapter->instance || base->inode != inode)) __builtin_trap();
    if (base && (base->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFREG) {
        struct lwext4_node *node = (struct lwext4_node *)base;
        if (!node->staged_fields) {
            if (kernel_vfs_node_acquire(base)) __builtin_trap();
            node->staged_next = adapter->staged_times;
            adapter->staged_times = node;
        }
        node->staged[field] = value;
        node->staged_version[field] = node->time_version[field];
        node->staged_fields |= 1U << field;
    }
    }
    if (pin && kernel_vfs_node_release(&pin)) __builtin_trap();
}

static void time_finish(void *context, bool accepted)
{
    struct lwext4_mount_adapter *adapter = context;
    for (;;) {
        struct kernel_vfs_node *dirty_pin, *staged_pin;
        {
            TIMESTAMP_SCOPE(times, adapter);
            struct lwext4_node *node = adapter->staged_times;
            if (!node) break;
            adapter->staged_times = node->staged_next;
            for (unsigned i = 0; i < 3; i++) {
                if (!accepted || !(node->staged_fields & (1U << i)) ||
                    node->staged_version[i] != node->time_version[i]) continue;
                node->times[i] = node->staged[i];
                if (!++node->time_version[i]) __builtin_trap();
                node->dirty_fields &= ~(1U << i);
            }
            node->staged_fields = 0;
            node->staged_next = 0;
            dirty_pin = time_drop_dirty(node);
            staged_pin = &node->node;
        }
        if (dirty_pin && kernel_vfs_node_release(&dirty_pin)) __builtin_trap();
        if (kernel_vfs_node_release(&staged_pin)) __builtin_trap();
    }
}

static unsigned ext4_backend_pending_times(struct kernel_vfs_node *base,
                                            struct ext4_timestamp times[3])
{
    struct lwext4_node *node = (struct lwext4_node *)base;
    struct lwext4_mount_adapter *adapter = lwext4_instance(base->instance);
    TIMESTAMP_SCOPE(guard, adapter);
    memcpy(times, node->times, sizeof(node->times));
    return node->dirty_fields;
}

static void ext4_backend_finish_times(struct kernel_vfs_node *base, unsigned fields,
                                      const struct ext4_timestamp times[3], int result)
{
    if (result) return;
    struct lwext4_node *node = (struct lwext4_node *)base;
    struct lwext4_mount_adapter *adapter = lwext4_instance(base->instance);
    struct kernel_vfs_node *dirty_pin;
    {
        TIMESTAMP_SCOPE(guard, adapter);
        for (unsigned i = 0; i < 3; i++)
            if ((fields & (1U << i)) && !time_compare(node->times[i], times[i]))
                node->dirty_fields &= ~(1U << i);
        dirty_pin = time_drop_dirty(node);
    }
    if (dirty_pin && kernel_vfs_node_release(&dirty_pin)) __builtin_trap();
}

static unsigned time_snapshot(void *owner, struct ext4_timestamp times[3])
{ return owner ? ext4_backend_pending_times(owner, times) : 0; }

static void time_written(void *owner, unsigned fields,
                          const struct ext4_timestamp times[3], int result)
{ if (owner) ext4_backend_finish_times(owner, fields, times, result); }

static int writeback_times(struct kernel_vfs_node *node, bool wait)
{ return ext4_file_writeback_times(lwext4_node_file(node), wait); }

static int writeback_mount_times(struct lwext4_mount_adapter *adapter, bool wait)
{
    size_t count;
    {
        TIMESTAMP_SCOPE(times, adapter);
        count = adapter->dirty_times_count;
    }
    int result = EOK;
    while (count--) {
        struct kernel_vfs_node *node;
        {
            TIMESTAMP_SCOPE(times, adapter);
            node = adapter->dirty_times ? &adapter->dirty_times->node : NULL;
            if (node && kernel_vfs_node_acquire(node)) __builtin_trap();
        }
        if (!node) break;
        result = writeback_times(node, wait);
        {
            TIMESTAMP_SCOPE(times, adapter);
            struct lwext4_node *current = (struct lwext4_node *)node;
            /* Newer RAM updates keep their pin without hiding other inodes
             * from this bounded flush pass. New entries join the tail. */
            if (!result && current->dirty_previous && adapter->dirty_times == current) {
                time_unlink_dirty(current);
                time_append_dirty(current);
            }
        }
        if (result && result != EAGAIN) kernel_vfs_record_writeback_error(node, lwext4_error(result));
        if (kernel_vfs_node_release(&node)) __builtin_trap();
        if (result) break;
    }
    {
        TIMESTAMP_SCOPE(times, adapter);
        adapter->times_deadline = adapter->dirty_times
            ? kernel_time_monotonic_ns() + UINT64_C(100000000) : 0;
    }
    return result;
}

/* The node is still unpublished; its initial times and immutable format flags
 * become visible together when kernel_vfs_publish_node exposes it. */
static int load_times(struct kernel_vfs_node *base, struct lwext4_mount_adapter *adapter)
{
    if ((base->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG) return EOK;
    struct ext4_inode inode;
    int result = ext4_fraw_inode_fill(lwext4_node_file(base), &inode);
    if (result) return result;
    struct lwext4_node *node = (struct lwext4_node *)base;
    lwext4_node_file(base)->time_owner = base;
    node->noatime = ext4_inode_has_flag(&inode, EXT4_INODE_FLAG_NOATIME);
    size_t offsets[3] = {offsetof(struct ext4_inode, atime_extra),
        offsetof(struct ext4_inode, mtime_extra), offsetof(struct ext4_inode, ctime_extra)};
    uint32_t seconds[3] = {inode.access_time, inode.modification_time, inode.change_inode_time};
    uint32_t extra[3] = {inode.atime_extra, inode.mtime_extra, inode.ctime_extra};
    for (unsigned i = 0; i < 3; i++) {
        int extended = inode_extra_field_present(adapter->superblock, &inode, offsets[i], sizeof(uint32_t));
        if (extended) node->extra_times |= 1U << i;
        struct kernel_vfs_timespec value = decode_inode_time(seconds[i], extra[i], extended);
        node->times[i] = (struct ext4_timestamp){value.seconds, value.nanoseconds};
    }
    return EOK;
}

static int lwext4_error(int error)
{
    if (error == EOK) {
        return 0;
    }
    if (error < 0 || error > 4095) {
        return -KERNEL_EIO;
    }
    return -error;
}

static int block_open(struct ext4_blockdev *device)
{
    struct lwext4_mount_adapter *adapter;

    if (device == 0 || device->bdif == 0) {
        return EINVAL;
    }
    adapter = device->bdif->p_user;
    if (adapter == 0 || adapter->block == 0) {
        return ENODEV;
    }
    return EOK;
}

static int block_read(struct ext4_blockdev *device,
                      void *buffer,
                      uint64_t block,
                      uint32_t count)
{
    struct lwext4_mount_adapter *adapter;
    uint64_t offset;
    uint64_t byte_count;
    enum kernel_block_status status;

    if (device == 0 || device->bdif == 0 ||
        (buffer == 0 && count != 0U)) {
        return EINVAL;
    }
    adapter = device->bdif->p_user;
    if (adapter == 0 || adapter->block == 0 ||
        block > UINT64_MAX / device->bdif->ph_bsize) {
        return EINVAL;
    }
    offset = block * device->bdif->ph_bsize;
    byte_count = (uint64_t)count * device->bdif->ph_bsize;
    if (byte_count > SIZE_MAX) {
        return EINVAL;
    }

    status = kernel_block_read_at(adapter->block,
                                  offset,
                                  buffer,
                                  (size_t)byte_count);
    switch (status) {
    case KERNEL_BLOCK_STATUS_OK:
        return EOK;
    case KERNEL_BLOCK_STATUS_NO_MEMORY:
        return ENOMEM;
    case KERNEL_BLOCK_STATUS_UNSUPPORTED:
        return ENOTSUP;
    case KERNEL_BLOCK_STATUS_INVALID:
        return EINVAL;
    case KERNEL_BLOCK_STATUS_OUT_OF_RANGE:
    case KERNEL_BLOCK_STATUS_IO:
    case KERNEL_BLOCK_STATUS_TIMEOUT:
    case KERNEL_BLOCK_STATUS_STATE:
    default:
        return EIO;
    }
}

static int block_write(struct ext4_blockdev *device,
                       const void *buffer,
                       uint64_t block,
                       uint32_t count)
{
    struct lwext4_mount_adapter *adapter;
    uint64_t offset;
    uint64_t byte_count;
    enum kernel_block_status status;

    if (device == 0 || device->bdif == 0 ||
        (buffer == 0 && count != 0U)) {
        return EINVAL;
    }
    adapter = device->bdif->p_user;
    if (adapter == 0 || adapter->block == 0 ||
        block > UINT64_MAX / device->bdif->ph_bsize) {
        return EINVAL;
    }
    if (kernel_block_read_only(adapter->block)) {
        return EROFS;
    }
    offset = block * device->bdif->ph_bsize;
    byte_count = (uint64_t)count * device->bdif->ph_bsize;
    if (byte_count > SIZE_MAX) {
        return EINVAL;
    }

    status = kernel_block_write_at(adapter->block,
                                   offset,
                                   buffer,
                                   (size_t)byte_count);
    switch (status) {
    case KERNEL_BLOCK_STATUS_OK:
        return EOK;
    case KERNEL_BLOCK_STATUS_NO_MEMORY:
        return ENOMEM;
    case KERNEL_BLOCK_STATUS_UNSUPPORTED:
    case KERNEL_BLOCK_STATUS_READ_ONLY:
        return EROFS;
    case KERNEL_BLOCK_STATUS_INVALID:
        return EINVAL;
    case KERNEL_BLOCK_STATUS_OUT_OF_RANGE:
    case KERNEL_BLOCK_STATUS_IO:
    case KERNEL_BLOCK_STATUS_TIMEOUT:
    case KERNEL_BLOCK_STATUS_STATE:
    default:
        return EIO;
    }
}

static int block_close(struct ext4_blockdev *device)
{
    return block_open(device);
}

static int block_write_batch(struct ext4_blockdev *device,
                            const struct ext4_block_span *spans, unsigned count)
{
    struct lwext4_mount_adapter *adapter = device->bdif->p_user;
    if (count > KERNEL_BLOCK_BATCH_MAX || (count && !spans)) return EINVAL;
    struct kernel_block_span single = {0}, *physical = &single;
    if (count > 1 && kernel_heap_allocate(adapter->instance.heap,
            count * sizeof(*physical), (void **)&physical)) return ENOMEM;
    for (unsigned i = 0; i < count; i++) {
        if (spans[i].block > UINT64_MAX / device->bdif->ph_bsize ||
            (uint64_t)spans[i].count * device->bdif->ph_bsize > SIZE_MAX) {
            if (count > 1) kernel_heap_release(adapter->instance.heap, physical);
            return EINVAL;
        }
        physical[i] = (struct kernel_block_span){spans[i].block * device->bdif->ph_bsize,
            spans[i].data, (size_t)spans[i].count * device->bdif->ph_bsize};
    }
    enum kernel_block_status status = kernel_block_write_batch(adapter->block, physical, count);
    if (count > 1) kernel_heap_release(adapter->instance.heap, physical);
    switch (status) {
    case KERNEL_BLOCK_STATUS_OK: return EOK;
    case KERNEL_BLOCK_STATUS_NO_MEMORY: return ENOMEM;
    case KERNEL_BLOCK_STATUS_UNSUPPORTED:
    case KERNEL_BLOCK_STATUS_READ_ONLY: return EROFS;
    case KERNEL_BLOCK_STATUS_INVALID: return EINVAL;
    default: return EIO;
    }
}

static int block_phase_next(const void *context, const void **cursor, struct kernel_block_span *span)
{
    (void)context;
    const struct ext4_block_write *io = *cursor;
    if (!io) return 0;
    *span = (struct kernel_block_span){io->offset, io->data, io->size};
    *cursor = io->next;
    return 1;
}

static int block_write_phase(struct ext4_blockdev *device,
                            const struct ext4_block_write *first)
{
    struct lwext4_mount_adapter *adapter = device->bdif->p_user;
    switch (kernel_block_write_phase(adapter->block, block_phase_next, 0, first)) {
    case KERNEL_BLOCK_STATUS_OK: return EOK;
    case KERNEL_BLOCK_STATUS_NO_MEMORY: return ENOMEM;
    case KERNEL_BLOCK_STATUS_UNSUPPORTED:
    case KERNEL_BLOCK_STATUS_READ_ONLY: return EROFS;
    default: return EIO;
    }
}

static void block_read_error(struct block_read_span *span)
{
    switch (span->status) {
    case KERNEL_BLOCK_STATUS_OK: span->status = span->completed == span->size ? EOK : EIO; break;
    case KERNEL_BLOCK_STATUS_NO_MEMORY: span->status = ENOMEM; break;
    case KERNEL_BLOCK_STATUS_INVALID: span->status = EINVAL; break;
    case KERNEL_BLOCK_STATUS_UNSUPPORTED: span->status = ENOTSUP; break;
    default: span->status = EIO; break;
    }
}
/* 错误转换只转发当前结果，禁止间接递归内联放大I/O栈。 */
static void __attribute__((noinline)) block_read_completed(void *context, unsigned index, const struct block_read_span *span)
{
    const struct block_read_completion *completion = context;
    struct block_read_span result = *span;
    block_read_error(&result);
    completion->complete(completion->context, index, &result);
}
static int block_read_batch(struct ext4_blockdev *device,
    struct block_read_span *spans, unsigned count, const struct block_read_completion *completion)
{
    struct lwext4_mount_adapter *adapter = device->bdif->p_user;
    if (count > KERNEL_BLOCK_BATCH_MAX || (count && !spans)) return EINVAL;
    for (unsigned i = 0; i < count; i++) spans[i].status = KERNEL_BLOCK_STATUS_NOT_SUBMITTED;
    const struct block_read_completion forward = {block_read_completed, (void *)completion};
    kernel_block_read_batch(adapter->block, spans, count, completion ? &forward : NULL);
    int result = EOK;
    for (unsigned i = 0; i < count; i++) {
        block_read_error(&spans[i]);
        if (!result) result = spans[i].status;
    }
    return result;
}

static int block_flush(struct ext4_blockdev *device)
{
    struct lwext4_mount_adapter *adapter = device->bdif->p_user;
    switch (kernel_block_flush(adapter->block)) {
    case KERNEL_BLOCK_STATUS_OK: return EOK;
    case KERNEL_BLOCK_STATUS_NO_MEMORY: return ENOMEM;
    case KERNEL_BLOCK_STATUS_UNSUPPORTED: return ENOTSUP;
    default: return EIO;
    }
}

static uint64_t ext4_buffer_bytes(void *context)
{
    uint64_t total = 0;
    for (struct lwext4_mount_adapter *adapter = adapters; adapter; adapter = adapter->next_adapter) {
        struct ext4_bcache *bc = adapter->device.bc;
        if (adapter->instance.heap == context && adapter->mounted && bc)
            total += (uint64_t)bc->ref_blocks * bc->itemsize;
    }
    return total;
}

int kernel_vfs_sync_block_device(struct kernel_block_device *device)
{
    KERNEL_SERVICE_SCOPE(service);
    if (!device) return -KERNEL_EINVAL;
    struct lwext4_mount_adapter *selected = NULL;
    kernel_task_lock_guard irq = {0};
    kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
    /* 设备的独占 mount claim 保证至多一个匹配实例。 */
    for (struct lwext4_mount_adapter *a = adapters; a; a = a->next_adapter)
        if (a->block == device && a->mount->state == VFS_MOUNT_STATE_LIVE) {
            selected = a;
            break;
        }
    if (!selected) { kernel_task_lock_release(&irq); return 0; }
    if (selected->instance.quiescing) { kernel_task_lock_release(&irq); return -KERNEL_EBUSY; }
    struct kernel_vfs_path *root = NULL;
    for (struct kernel_vfs_path *p = selected->instance.paths; p; p = p->next)
        if (!p->parent) { root = p; break; }
    int result = 0;
    if (root) {
        result = kernel_vfs_path_acquire(root);
        if (result) root = NULL;
    } else {
        /* 没有现存 path 时，先保留本次打开的挂载资格；路径分配可睡眠，
         * 不能让卸载在取得真实 root path owner 之前释放 adapter。 */
        if (selected->instance.external_files == UINT32_MAX) result = -KERNEL_EOVERFLOW;
        else __atomic_fetch_add(&selected->instance.external_files, 1, __ATOMIC_RELAXED);
    }
    kernel_task_lock_release(&irq);
    if (!root && !result) {
        result = kernel_vfs_path_root(selected->mount,selected->instance.heap,&root);
        kernel_task_lock_acquire(&kernel_vfs_state_lock, &irq);
        if (!selected->instance.external_files) __builtin_trap();
        __atomic_fetch_sub(&selected->instance.external_files, 1, __ATOMIC_RELEASE);
        kernel_task_lock_release(&irq);
    }
    if (!result) result = kernel_vfs_sync_mount(root->file.mount,NULL);
    if (root && kernel_vfs_path_release(&root)) __builtin_trap();
    return result;
}

static int release_mount_storage(struct kernel_vfs_mount *mount)
{
    struct lwext4_mount_adapter *adapter = mount->private_data;
    struct kernel_heap *heap = adapter->instance.heap;

    struct lwext4_mount_adapter **link = &adapters;
    while (*link && *link != adapter) link = &(*link)->next_adapter;
    if (*link) *link = adapter->next_adapter;
    if (!adapters && heap->page_allocator->buffer_context == heap) {
        heap->page_allocator->buffer_bytes = 0;
        heap->page_allocator->buffer_context = 0;
    }
    if (adapter->physical_buffer != 0) {
        kernel_heap_release(heap, adapter->physical_buffer);
        adapter->physical_buffer = 0;
    }
    if (adapter->heap_bound) {
        boaros_lwext4_heap_unbind(heap);
        adapter->heap_bound = 0U;
    }

    if (adapter->block_claimed) kernel_block_release_claim(adapter->block, mount);
    kernel_heap_release(heap, adapter);
    mount->private_data = 0;
    mount->source_name = 0;
    mount->id = 0U;
    mount->state = VFS_MOUNT_STATE_EMPTY;
    return 0;
}

static void queue_orphan(struct lwext4_mount_adapter *adapter,
                         struct lwext4_orphan *orphan)
{
    orphan->next = adapter->orphans;
    adapter->orphans = orphan;
}

static void release_orphan(struct lwext4_mount_adapter *adapter,
                           struct lwext4_orphan *orphan)
{
    kernel_heap_release(adapter->instance.heap, orphan);
}

static int reserve_orphan(struct lwext4_mount_adapter *adapter,
                          struct lwext4_orphan **owner)
{
    enum kernel_heap_status status;

    status = kernel_heap_allocate_zeroed(adapter->instance.heap,
                                         1U,
                                         sizeof(**owner),
                                         (void **)owner);
    if (status == KERNEL_HEAP_STATUS_OK) {
        (*owner)->orphan_freed = 1U;
        return 0;
    }
    return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                              : -KERNEL_EIO;
}

static int ext4_backend_prepare_unmount(struct kernel_vfs_mount *mount)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    struct lwext4_mount_adapter *adapter = mount->private_data;
    struct kernel_vfs_node *root = mount->root_path
        ? mount->root_path->file.private_data : 0;
    struct lwext4_orphan **orphan_link;
    struct kernel_vfs_node **cleanup_link;
    int result;

    if (adapter->unmount_prepared) return 0;
    if (adapter->instance.external_files != (root ? 1U : 0U))
        return -KERNEL_EBUSY;
    /* Each outstanding sync still borrows its file and mount. Leave the
     * completion workers alive until those callers have consumed the result. */
    {
        KERNEL_TASK_LOCK_SCOPE(guard, &adapter->journal_control);
        if (adapter->sync_waiters) return -KERNEL_EBUSY;
    }
    adapter->instance.quiescing = 1U;
    /* 停止并等待本实例 worker；后续失败仍保持静止，不能重新发布可写挂载。 */
    if (adapter->instance.page_cache)
        kernel_page_cache_stop_worker(adapter->instance.page_cache);

    /* An uncertain journal/recovery failure still owns its cache and log.
     * Never mark that filesystem clean or recycle its adapter on a retry. */
    result = mount_error(adapter);
    if (result != EOK) return lwext4_error(result);
    if (adapter->recovery_pending) {
        result = prepare_journal_mount(adapter);
        if (result != EOK) return lwext4_error(result);
    }

    if (!root) mount->state = VFS_MOUNT_STATE_CLEANUP;
    if (adapter->instance.page_cache != 0) {
        enum kernel_page_cache_status cache_status =
            kernel_page_cache_purge_mount(adapter->instance.page_cache, mount);

        if (cache_status != KERNEL_PAGE_CACHE_STATUS_OK) {
            return cache_status == KERNEL_PAGE_CACHE_STATUS_STATE
                       ? -KERNEL_EBUSY : -KERNEL_EIO;
        }
    }
    result = writeback_mount_times(adapter, true);
    if (result != EOK) return lwext4_error(result);
    if (!kernel_vfs_nodes_idle(&adapter->instance, root)) return -KERNEL_EBUSY;
    orphan_link = &adapter->orphans;
    while (*orphan_link != 0) {
        struct lwext4_orphan *orphan = *orphan_link;
        struct lwext4_orphan *next = orphan->next;

        if (!orphan->orphan_freed) {
            result = free_orphan_inode(adapter, orphan->inode);
            if (result != EOK) {
                return lwext4_error(result);
            }
            orphan->orphan_freed = 1U;
        }
        kernel_heap_release(adapter->instance.heap, orphan);
        *orphan_link = next;
    }
    cleanup_link = &adapter->instance.cleanup_nodes;
    while (*cleanup_link != 0) {
        struct kernel_vfs_node *node = *cleanup_link;
        struct kernel_vfs_node *next = node->next;

        if (node->unlinked && !node->retired) {
            result = free_orphan_inode(adapter, node->inode);
            if (result == EOK) {
                node->retired = 1U;
            } else {
                return lwext4_error(result);
            }
        }
        if (((struct lwext4_node *)node)->write_completion && !node->retired) {
            result=ext4_file_complete_write(lwext4_node_file(node));
            if (result!=EOK) return lwext4_error(result);
            ((struct lwext4_node *)node)->write_completion=0;
        }
        if (!node->closed) {
            result = ext4_fclose(lwext4_node_file(node));
            if (result != EOK) {
                return lwext4_error(result);
            }
            node->closed = 1U;
        }
        kernel_file_mapping_destroy(node);
        kernel_heap_release(adapter->instance.heap, node);
        *cleanup_link = next;
    }
    if (adapter->orphans != 0 || adapter->instance.cleanup_nodes != 0 ||
        !kernel_vfs_nodes_idle(&adapter->instance, root)) {
        return -KERNEL_EIO;
    }
    /* root 的路径身份保留至摘树；私有 handle 不再访问即将释放的 lwext4 mount。 */
    if (root && !root->closed) {
        result = ext4_fclose(lwext4_node_file(root));
        if (result != EOK) return lwext4_error(result);
        root->closed = 1U;
    }
    if (adapter->journal_started) {
        if (adapter->device.fs->jbd_journal) {
            /* 持久 orphan 意图可由已关闭的外层事务留下；恢复会接受
             * 新的日志组，必须在提交与 checkpoint worker 仍存活时完成。 */
            result = ext4_orphan_recover(adapter->mount_point);
            if (result != EOK) return lwext4_error(result);
            result = ext4_journal_group_drain(adapter->mount_point);
            if (result != EOK) return lwext4_error(result);
        }
        journal_stop_workers(adapter);
        kernel_thread_join(&adapter->journal_worker);
        if (adapter->checkpoint_started) {
            kernel_thread_join(&adapter->checkpoint_worker);
            adapter->checkpoint_started = 0;
        }
        adapter->journal_started = 0;
    }
    if (adapter->mounted) {
        result = ext4_umount(adapter->mount_point);
        if (result != EOK) {
            return lwext4_error(result);
        }
        adapter->mounted = 0U;
        adapter->unmount_sync_pending = !adapter->instance.read_only;
    }
    if (adapter->unmount_sync_pending) {
        if (kernel_block_flush(adapter->block) != KERNEL_BLOCK_STATUS_OK)
            return -KERNEL_EIO;
        adapter->unmount_sync_pending = 0;
    }
    if (adapter->registered) {
        result = ext4_device_unregister(adapter->device_name);
        if (result != EOK) {
            return lwext4_error(result);
        }
        adapter->registered = 0U;
    }

    adapter->unmount_prepared = 1U;
    return 0;
}

static uint64_t journal_now(void *context)
{ (void)context; return kernel_time_monotonic_ns(); }

static void journal_request(void *context)
{
    journal_request_flags(context, 0);
}

static uint64_t journal_reached(const struct lwext4_mount_adapter *adapter, enum ext4_journal_wait kind)
{
    return kind == EXT4_JOURNAL_WAIT_SEALED ? adapter->published_sealed :
        kind == EXT4_JOURNAL_WAIT_DURABLE ? adapter->published_durable : adapter->published_checkpoint;
}

static unsigned sync_target_height(const struct kernel_vfs_sync_request *request)
{
    return request ? request->index_height : 0;
}

static void sync_target_update(struct kernel_vfs_sync_request *request)
{
    unsigned left=sync_target_height(request->index_left);
    unsigned right=sync_target_height(request->index_right);
    request->index_height=1+(left>right?left:right);
}

static struct kernel_vfs_sync_request *sync_target_rotate(
    struct kernel_vfs_sync_request *root,int left)
{
    struct kernel_vfs_sync_request *pivot;
    if (left) {
        pivot=root->index_right;
        root->index_right=pivot->index_left;
        pivot->index_left=root;
    } else {
        pivot=root->index_left;
        root->index_left=pivot->index_right;
        pivot->index_right=root;
    }
    sync_target_update(root);
    sync_target_update(pivot);
    return pivot;
}

static struct kernel_vfs_sync_request *sync_target_balance(struct kernel_vfs_sync_request *root)
{
    sync_target_update(root);
    int balance=(int)sync_target_height(root->index_left)-(int)sync_target_height(root->index_right);
    if (balance>1) {
        if (sync_target_height(root->index_left->index_right)>sync_target_height(root->index_left->index_left))
            root->index_left=sync_target_rotate(root->index_left,1);
        return sync_target_rotate(root,0);
    }
    if (balance < -1) {
        if (sync_target_height(root->index_right->index_left)>sync_target_height(root->index_right->index_right))
            root->index_right=sync_target_rotate(root->index_right,0);
        return sync_target_rotate(root,1);
    }
    return root;
}

static struct kernel_vfs_sync_request *sync_target_insert(struct kernel_vfs_sync_request *root,
    struct kernel_vfs_sync_request *request)
{
    if (!root) { request->index_height=1; return request; }
    if (root==request) __builtin_trap();
    if (request->target<root->target ||
        (request->target==root->target && (uintptr_t)request<(uintptr_t)root))
        root->index_left=sync_target_insert(root->index_left,request);
    else root->index_right=sync_target_insert(root->index_right,request);
    return sync_target_balance(root);
}

static struct kernel_vfs_sync_request *sync_target_take_first(struct kernel_vfs_sync_request *root)
{
    if (!root->index_left) return root->index_right;
    root->index_left=sync_target_take_first(root->index_left);
    return sync_target_balance(root);
}

static void complete_sync_waiters(struct lwext4_mount_adapter *adapter)
{
    for (;;) {
        struct kernel_vfs_sync_request *request;
        int error;
        {
            KERNEL_TASK_LOCK_SCOPE(guard, &adapter->journal_control);
            request = adapter->sync_waiters;
            while (request && request->index_left) request = request->index_left;
            error = adapter->published_error;
            if (!request || (!error && adapter->published_durable < request->target)) return;
            adapter->sync_waiters = sync_target_take_first(adapter->sync_waiters);
            request->index_left = request->index_right = NULL;
            request->index_height = 0;
        }
        /* Notification can retire the caller's stack. The removed request is
         * borrowed through this completion; never touch it afterwards. */
        kernel_vfs_sync_complete(request, lwext4_error(error));
    }
}

static int journal_wait(void *context, uint64_t sequence, enum ext4_journal_wait kind)
{
#if BOAROS_COST_DIAGNOSTICS
    struct kernel_cost_scope elapsed __attribute__((cleanup(kernel_cost_leave))) = kernel_cost_enter(
        kind == EXT4_JOURNAL_WAIT_SEALED ? COST_JOURNAL_WAIT_SEALED_TICKS :
        kind == EXT4_JOURNAL_WAIT_DURABLE ? COST_JOURNAL_WAIT_DURABLE_TICKS : COST_JOURNAL_WAIT_CHECKPOINT_TICKS);
#endif
    if (kind == EXT4_JOURNAL_WAIT_SEALED) COST_ADD(JOURNAL_WAIT_SEALED, 1);
    else if (kind == EXT4_JOURNAL_WAIT_DURABLE) COST_ADD(JOURNAL_WAIT_DURABLE, 1);
    else COST_ADD(JOURNAL_WAIT_CHECKPOINT, 1);
    struct lwext4_mount_adapter *adapter = context;
    int changed = journal_publish_locked(adapter);
    journal_request_flags(adapter, 1);
    unsigned depth = boaros_lwext4_pause();
    if (changed) complete_sync_waiters(adapter);
    int result;
    for (;;) {
        struct kernel_wait_token token;
        {
            KERNEL_TASK_LOCK_SCOPE(guard, &adapter->journal_control);
            result = adapter->published_error;
            if (result || journal_reached(adapter, kind) >= sequence) break;
            if (adapter->journal_stopping) { result = EIO; break; }
            if (kernel_wait_prepare(&adapter->journal_progress, 0, 0, &token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token, &reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
    boaros_lwext4_resume(&adapter->backend_lock, depth);
    return result;
}

static void journal_worker(void *context)
{
    struct lwext4_mount_adapter *adapter = context;
    kernel_io_context_current()->background_reclaim = 1;
    for (;;) {
        int force, stopping;
        {
            KERNEL_TASK_LOCK_SCOPE(guard, &adapter->journal_control);
            force = adapter->journal_force;
            adapter->journal_force = adapter->journal_requested = 0;
            stopping = adapter->journal_stopping;
        }
        if (stopping) break;
        int metadata = EOK;
        uint64_t times_due = time_deadline(adapter);
        if (times_due && kernel_time_monotonic_ns() >= times_due)
            metadata = writeback_mount_times(adapter, false);
        int result = EOK;
        struct ext4_journal_progress progress = {0};
        if (adapter->device.fs->jbd_journal) {
            result = ext4_journal_group_commit(adapter->mount_point, force || metadata == EAGAIN);
            if (ext4_journal_group_progress(adapter->mount_point, &progress) != EOK) __builtin_trap();
        }
        complete_sync_waiters(adapter);
        uint64_t deadline = 0;
        uint64_t due = progress.deadline_ns;
        times_due = time_deadline(adapter);
        if (!result && times_due && (!due || times_due < due)) due = times_due;
        if (!result && (progress.commit_ready || (due &&
            kernel_time_deadline_from_monotonic(due, &deadline) == KERNEL_TIME_STATUS_DEADLINE_PASSED))) continue;
        journal_idle(adapter, 0, deadline);
    }
    kernel_io_context_current()->background_reclaim = 0;
}

static void journal_checkpoint_worker(void *context)
{
    struct lwext4_mount_adapter *adapter = context;
    uint64_t pending_deadline = 0;
    kernel_io_context_current()->background_reclaim = 1;
    for (;;) {
        int stopping;
        {
            KERNEL_TASK_LOCK_SCOPE(guard, &adapter->journal_control);
            adapter->checkpoint_requested = 0;
            stopping = adapter->journal_stopping;
        }
        if (stopping) break;
        struct ext4_journal_progress progress;
        if (ext4_journal_group_progress(adapter->mount_point, &progress) != EOK) __builtin_trap();
        if (!progress.checkpoint_pending) pending_deadline = 0;
        else if (!pending_deadline) pending_deadline = kernel_time_monotonic_ns() + 100000000;
        int result = progress.error;
        if (!result && (progress.checkpoint_ready || (pending_deadline &&
            kernel_time_monotonic_ns() >= pending_deadline))) {
            result = ext4_journal_group_checkpoint(adapter->mount_point);
            if (ext4_journal_group_progress(adapter->mount_point, &progress) != EOK) __builtin_trap();
            if (!progress.checkpoint_pending) pending_deadline = 0;
        }
        if (!result && progress.checkpoint_ready) continue;
        uint64_t deadline = 0;
        /* New groups share the first pending group's checkpoint window. */
        if (!result && pending_deadline &&
            kernel_time_deadline_from_monotonic(pending_deadline, &deadline) == KERNEL_TIME_STATUS_DEADLINE_PASSED)
            continue;
        journal_idle(adapter, 1, deadline);
    }
    kernel_io_context_current()->background_reclaim = 0;
}

int kernel_vfs_start_journal_worker(struct kernel_vfs_mount *mount)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    struct lwext4_mount_adapter *adapter = mount->private_data;
    if (adapter->instance.backend_data != adapter || !adapter->mounted) return -KERNEL_EINVAL;
    if (adapter->instance.read_only) return 0;
    if (adapter->journal_started) return -KERNEL_EBUSY;
    /* The same mount worker also owns timestamp-only writeback on writable
     * filesystems without JBD; dirty inode pins need a consumer there too. */
    if (adapter->device.fs->jbd_journal) {
        uint64_t budget = physical_page_total(adapter->instance.page_cache->allocator) * BOAROS_PAGE_SIZE / 32;
        if (budget > 4 * 1024 * 1024) budget = 4 * 1024 * 1024;
        struct ext4_journal_runtime runtime = {adapter, journal_now, journal_request, journal_wait};
        int result = ext4_journal_group_enable(adapter->mount_point, &runtime, budget);
        if (result != EOK) return lwext4_error(result);
    }
    enum kernel_scheduler_status status = KERNEL_SCHEDULER_STATUS_OK;
    if (adapter->device.fs->jbd_journal) {
        status = kernel_thread_create_joinable(journal_checkpoint_worker, adapter, &adapter->checkpoint_worker);
        if (status == KERNEL_SCHEDULER_STATUS_OK) adapter->checkpoint_started = 1;
    }
    if (status == KERNEL_SCHEDULER_STATUS_OK)
        status = kernel_thread_create_joinable(journal_worker, adapter, &adapter->journal_worker);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        if (adapter->checkpoint_started) {
            journal_stop_workers(adapter);
            kernel_thread_join(&adapter->checkpoint_worker);
            adapter->checkpoint_started = adapter->journal_stopping = 0;
        }
        /* No operation could join while startup held this execution path. */
        if (adapter->device.fs->jbd_journal) jbd_journal_group_fini(adapter->device.fs->jbd_journal);
        return status == KERNEL_SCHEDULER_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    }
    adapter->journal_started = 1;
    return 0;
}

static int cleanup_mount(struct kernel_vfs_mount *mount)
{
    struct lwext4_mount_adapter *adapter = mount->private_data;
    if (adapter->instance.external_files || mount->root_path)
        return -KERNEL_EBUSY;
    int result = ext4_backend_prepare_unmount(mount);
    if (result) return result;
    return release_mount_storage(mount);
}

int kernel_vfs_mount_ext4(struct kernel_vfs_mount *mount,
                          struct kernel_block_device *block,
                          struct kernel_heap *heap,
                          struct kernel_page_cache *page_cache, int read_only)
{
    struct lwext4_mount_adapter *adapter;
    struct ext4_sblock *superblock;
    enum kernel_heap_status heap_status;
    int result;

    if (mount == 0 || mount->state != VFS_MOUNT_STATE_EMPTY ||
        mount->private_data != 0 || mount->id != 0U ||
        block == 0 || (block->parent ? block->parent : block)->read == 0 ||
        heap == 0 || page_cache == 0 ||
        page_cache->state != KERNEL_PAGE_CACHE_LIVE) {
        return -KERNEL_EINVAL;
    }
    if (!boaros_lwext4_heap_bind(heap)) {
        return -KERNEL_EBUSY;
    }

    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*adapter),
                                              (void **)&adapter);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        boaros_lwext4_heap_unbind(heap);
        return heap_status == KERNEL_HEAP_STATUS_EMPTY ?
                   -KERNEL_ENOMEM : -KERNEL_EIO;
    }
    adapter_names(adapter);
    boaros_lwext4_lock_init(&adapter->locks, &adapter->backend_lock);
    adapter->locks.unlock = backend_unlock;
    kernel_raw_lock_init(&adapter->journal_control, KERNEL_RAW_RANK_OBJECT);
    /* Progress publication also runs during synchronous journal service,
     * before background workers exist. Queues belong to the mount lifetime. */
    kernel_wait_queue_init(&adapter->journal_work);
    kernel_wait_queue_init(&adapter->checkpoint_work);
    kernel_wait_queue_init(&adapter->journal_progress);
#if BOAROS_MAX_CPUS > 1
    kernel_raw_lock_init(&adapter->timestamp_lock, KERNEL_RAW_RANK_OBJECT);
#endif
    adapter->mount = mount;
    adapter->next_adapter = adapters;
    adapters = adapter;
    kernel_mutex_init(&adapter->instance.namespace_lock, 20, (uintptr_t)adapter);
    kernel_raw_lock_init(&adapter->instance.error_lock, KERNEL_RAW_RANK_OBJECT);
    initialize_backend();
    adapter->instance.backend_data = adapter;
    adapter->instance.ops = &ext4_backend;
    adapter->instance.heap = heap;
    adapter->block = block;
    adapter->instance.page_cache = page_cache;
    adapter->heap_bound = 1U;
    mount->private_data = adapter;
    mount->state = VFS_MOUNT_STATE_CLEANUP;
    result = kernel_block_claim(block, mount);
    if (result) { (void)cleanup_mount(mount); return result; }
    adapter->block_claimed = 1U;
    /* Claim excludes loop reconfiguration before allocating its sector buffer. */
    const uint32_t sector = block->logical_block_size;
    if (sector < 512 || sector > BOAROS_PAGE_SIZE || (sector & (sector - 1))) {
        (void)cleanup_mount(mount); return -KERNEL_EINVAL;
    }

    heap_status = kernel_heap_allocate(heap,
                                       sector,
                                       (void **)&adapter->physical_buffer);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        (void)cleanup_mount(mount);
        return heap_status == KERNEL_HEAP_STATUS_EMPTY ?
                   -KERNEL_ENOMEM : -KERNEL_EIO;
    }

    adapter->interface.open = block_open;
    adapter->interface.bread = block_read;
    adapter->interface.bread_batch = block_read_batch;
    adapter->interface.bwrite = block_write;
    adapter->interface.bwrite_batch = block_write_batch;
    adapter->interface.bwrite_phase = block_write_phase;
    adapter->interface.close = block_close;
    adapter->interface.flush = block_flush;
    adapter->interface.lock = 0;
    adapter->interface.unlock = 0;
    adapter->interface.ph_bsize = sector;
    adapter->interface.ph_bcnt =
        block->capacity_bytes / sector;
    adapter->interface.ph_bbuf = adapter->physical_buffer;
    adapter->interface.p_user = adapter;
    adapter->interface.wait_read = boaros_lwext4_wait;
    adapter->interface.wake_read = boaros_lwext4_wake;
    adapter->interface.read_context = boaros_lwext4_read_context;
    adapter->device.bdif = &adapter->interface;
    adapter->device.part_offset = 0U;
    adapter->device.part_size =
        adapter->interface.ph_bcnt * sector;

    result = ext4_device_register(&adapter->device, adapter->device_name);
    if (result != EOK) {
        (void)cleanup_mount(mount);
        return lwext4_error(result);
    }
    adapter->registered = 1U;
    read_only = read_only || kernel_block_read_only(block);
    adapter->instance.read_only = (uint8_t)(read_only != 0 ? 1U : 0U);
    result = ext4_mount(adapter->device_name, adapter->mount_point, read_only != 0);
    if (result != EOK) {
        (void)cleanup_mount(mount);
        return lwext4_error(result);
    }
    adapter->mounted = 1U;
    heap->page_allocator->buffer_bytes = ext4_buffer_bytes;
    heap->page_allocator->buffer_context = heap;
    result = ext4_mount_setup_locks(adapter->mount_point, &adapter->locks);
    if (result != EOK) { (void)cleanup_mount(mount); return lwext4_error(result); }
    struct ext4_time_observer observer = {adapter, time_stage, time_finish, time_snapshot, time_written};
    result = ext4_mount_setup_time_observer(adapter->mount_point, &observer);
    if (result != EOK) { (void)cleanup_mount(mount); return lwext4_error(result); }
    result = ext4_mount_setup_clock(adapter->mount_point, vfs_realtime);
    if (result != EOK) {
        (void)cleanup_mount(mount);
        return lwext4_error(result);
    }

    result = ext4_get_sblock(adapter->mount_point, &superblock);
    if (result != EOK) {
        (void)cleanup_mount(mount);
        return lwext4_error(result);
    }
    if (read_only && (ext4_sb_feature_incom(superblock, EXT4_FINCOM_RECOVER) ||
        ext4_get32(superblock, last_orphan) != 0 ||
        ext4_sb_feature_ro_com(superblock, EXT4_FRO_COM_ORPHAN_PRESENT))) {
        (void)cleanup_mount(mount);
        return -KERNEL_EUCLEAN;
    }

    if (ext4_sb_feature_com(superblock, EXT4_FCOM_HAS_JOURNAL)) {
        adapter->recovery_pending = 1U;
        result = prepare_journal_mount(adapter);
        if (result != EOK) {
            if (!mount_error(adapter)) (void)cleanup_mount(mount);
            return lwext4_error(result);
        }
    }

    adapter->superblock = superblock;
    adapter->instance.writeback_block_size = ext4_sb_get_block_size(superblock);
    mount->id = kernel_vfs_allocate_mount_id();
    mount->state = VFS_MOUNT_STATE_LIVE;
    return 0;
}

int kernel_vfs_mount_root(struct kernel_vfs_mount *mount,
                          struct kernel_block_device *block,
                          struct kernel_heap *heap,
                          struct kernel_page_cache *page_cache)
{
    int result = kernel_vfs_mount_ext4(mount, block, heap, page_cache, 0);
    if (!result && block->registered) {
        struct lwext4_mount_adapter *adapter = mount->private_data;
        uint64_t number = block->device_number;
        unsigned major = (number >> 8) & 4095;
        unsigned minor = (number & 255) | ((number >> 12) & 0xffffff00U);
        char *out = adapter->root_source;
        memcpy(out, "/dev/block/", 11); out += 11;
        unsigned values[2] = {major, minor};
        for (unsigned i = 0; i < 2; i++) {
            char reverse[10]; unsigned n = 0, value = values[i];
            do { reverse[n++] = '0' + value % 10; value /= 10; } while (value);
            while (n) *out++ = reverse[--n];
            if (!i) *out++ = ':';
        }
        *out = 0; mount->source_name = adapter->root_source;
    }
    return result;
}

static int valid_utime_nsec(int64_t value)
{
    return (value >= 0 && value < INT64_C(1000000000)) ||
           value == KERNEL_VFS_UTIME_NOW || value == KERNEL_VFS_UTIME_OMIT;
}

static int inode_extra_field_present(struct ext4_sblock *superblock,
                                     struct ext4_inode *inode,
                                     size_t field_offset,
                                     size_t field_size)
{
    uint16_t extra_size = ext4_inode_get_extra_isize(superblock, inode);

    return field_offset <= SIZE_MAX - field_size &&
           field_offset + field_size <= ext4_get16(superblock, inode_size) &&
           field_offset + field_size <=
               EXT4_GOOD_OLD_INODE_SIZE + (size_t)extra_size;
}

static struct kernel_vfs_timespec decode_inode_time(
    uint32_t base, uint32_t extra, int has_extra)
{
    struct kernel_vfs_timespec result = {
        .seconds = (int32_t)base,
        .nanoseconds = 0,
    };

    if (has_extra) {
        extra = to_le32(extra);
        if ((extra & 3U) != 0U)
            result.seconds += (int64_t)(extra & 3U) << 32U;
        result.nanoseconds = (int64_t)(extra >> 2U);
    }
    return result;
}

static void fill_stat(struct kernel_vfs_mount *mount,
                      struct ext4_sblock *superblock,
                      uint32_t inode_number,
                      struct ext4_inode *inode,
                      struct kernel_vfs_stat *stat)
{
    uint32_t creator_os;

    memset(stat, 0, sizeof(*stat));
    struct kernel_block_device *block = lwext4_instance(mount->private_data)->block;
    stat->dev = block->device_number ? block->device_number : mount->id;
    stat->ino = inode_number;
    stat->mode = ext4_inode_get_mode(superblock, inode);
    stat->nlink = ext4_inode_get_links_cnt(inode);
    stat->uid = to_le16(inode->uid);
    stat->gid = to_le16(inode->gid);
    creator_os = ext4_get32(superblock, creator_os);
    if (creator_os == EXT4_SUPERBLOCK_OS_LINUX) {
        stat->uid |= (uint32_t)to_le16(inode->osd2.linux2.uid_high) << 16U;
        stat->gid |= (uint32_t)to_le16(inode->osd2.linux2.gid_high) << 16U;
    }
    if ((stat->mode & EXT4_INODE_MODE_TYPE_MASK) ==
            EXT4_INODE_MODE_CHARDEV ||
        (stat->mode & EXT4_INODE_MODE_TYPE_MASK) ==
            EXT4_INODE_MODE_BLOCKDEV) {
        stat->rdev = ext4_inode_get_dev(inode);
    }
    stat->size = ext4_inode_get_size(superblock, inode);
    stat->blocks = ext4_inode_get_blocks_count(superblock, inode);
    stat->blksize = ext4_sb_get_block_size(superblock);
    stat->atime = decode_inode_time(
        ext4_inode_get_access_time(inode), inode->atime_extra,
        inode_extra_field_present(superblock, inode,
                                  offsetof(struct ext4_inode, atime_extra),
                                  sizeof(inode->atime_extra)));
    stat->mtime = decode_inode_time(
        ext4_inode_get_modif_time(inode), inode->mtime_extra,
        inode_extra_field_present(superblock, inode,
                                  offsetof(struct ext4_inode, mtime_extra),
                                  sizeof(inode->mtime_extra)));
    stat->ctime = decode_inode_time(
        ext4_inode_get_change_inode_time(inode), inode->ctime_extra,
        inode_extra_field_present(superblock, inode,
                                  offsetof(struct ext4_inode, ctime_extra),
                                  sizeof(inode->ctime_extra)));
}

static uint8_t dirent_type_from_ext4(uint8_t inode_type)
{
    switch (inode_type) {
    case 1U:
        return KERNEL_VFS_DT_REG;
    case 2U:
        return KERNEL_VFS_DT_DIR;
    case 3U:
        return KERNEL_VFS_DT_CHR;
    case 4U:
        return KERNEL_VFS_DT_BLK;
    case 5U:
        return KERNEL_VFS_DT_FIFO;
    case 6U:
        return KERNEL_VFS_DT_SOCK;
    case 7U:
        return KERNEL_VFS_DT_LNK;
    default:
        return KERNEL_VFS_DT_UNKNOWN;
    }
}

static int ext4_backend_unmount(struct kernel_vfs_mount *mount)
{
    if (mount == 0 || mount->private_data == 0 ||
        (mount->state != VFS_MOUNT_STATE_LIVE &&
         mount->state != VFS_MOUNT_STATE_CLEANUP)) {
        return -KERNEL_EINVAL;
    }

    return cleanup_mount(mount);
}

static int ext4_backend_statfs(struct kernel_vfs_mount *mount,
                            struct kernel_vfs_statfs *stat)
{
    struct ext4_mount_stats disk;
    if (!mount || mount->state != VFS_MOUNT_STATE_LIVE ||
        !mount->private_data || !stat) return -KERNEL_EINVAL;
    struct lwext4_mount_adapter *adapter = mount->private_data;
    int result = mount_error(adapter);
    if (result) return lwext4_error(result);
    result = ext4_mount_point_stats(adapter->mount_point, &disk);
    if (result) return lwext4_error(result);
    if (disk.overhead_blocks > disk.blocks_count) return -KERNEL_EUCLEAN;
    memset(stat, 0, sizeof(*stat));
    stat->type = UINT64_C(0xef53);
    stat->block_size = disk.block_size;
    stat->blocks = disk.blocks_count - disk.overhead_blocks;
    stat->free_blocks = disk.free_blocks_count;
    stat->available_blocks = disk.free_blocks_count > disk.reserved_blocks_count
                          ? disk.free_blocks_count - disk.reserved_blocks_count : 0;
    stat->inodes = disk.inodes_count;
    stat->free_inodes = disk.free_inodes_count;
    for (unsigned i = 0; i < 8U; i++)
        stat->fsid |= (uint64_t)(disk.uuid[i] ^ disk.uuid[i + 8U]) << (8U * i);
    stat->name_length = 255U;
    stat->flags = UINT64_C(0x20) | UINT64_C(0x1000) | (adapter->instance.read_only ? 1U : 0U);
    return 0;
}

static int ext4_backend_set_times(struct kernel_vfs_file *file,
                              const struct kernel_vfs_timespec times[2])
{
    if (!file || !file->private_data || file->state != VFS_FILE_STATE_LIVE)
        return -KERNEL_EINVAL;
    if (times && (!valid_utime_nsec(times[0].nanoseconds) ||
                  !valid_utime_nsec(times[1].nanoseconds))) return -KERNEL_EINVAL;
    if (times && times[0].nanoseconds == KERNEL_VFS_UTIME_OMIT &&
                 times[1].nanoseconds == KERNEL_VFS_UTIME_OMIT) return 0;
    struct kernel_vfs_node *node = file->private_data;
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 1);
    if (node->instance->read_only) return -KERNEL_EROFS;
    int result = mount_error(lwext4_instance(node->instance));
    if (result) return lwext4_error(result);
    struct ext4_timestamp now, values[3];
    if (!ext4_file_read_clock(lwext4_node_file(node), &now)) return -KERNEL_EIO;
    unsigned fields = EXT4_TIME_CTIME;
    values[2] = now;
    for (unsigned i = 0; i < 2; i++) {
        if (times && times[i].nanoseconds == KERNEL_VFS_UTIME_OMIT) continue;
        fields |= i ? EXT4_TIME_MTIME : EXT4_TIME_ATIME;
        values[i] = !times || times[i].nanoseconds == KERNEL_VFS_UTIME_NOW
                  ? now : (struct ext4_timestamp){times[i].seconds,
                                                    (uint32_t)times[i].nanoseconds};
    }
    return lwext4_error(ext4_file_set_times(lwext4_node_file(node), fields, values));
}

static int ext4_backend_set_mode(struct kernel_vfs_file *file, uint32_t mode)
{
    if (!file || !file->private_data || file->state != VFS_FILE_STATE_LIVE)
        return -KERNEL_EINVAL;
    struct kernel_vfs_node *node = file->private_data;
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 1);
    if (node->instance->read_only) return -KERNEL_EROFS;
    int result = mount_error(lwext4_instance(node->instance));
    if (result) return lwext4_error(result);
    result = lwext4_error(ext4_file_set_mode(lwext4_node_file(node), mode & 07777U));
    if (!result) {
        node->mode = (node->mode & ~07777U) | (mode & 07777U);
        file->mode = node->mode;
    }
    return result;
}

static int ext4_backend_set_owner(struct kernel_vfs_file *file, uint32_t uid, uint32_t gid)
{
    if (!file || !file->private_data || file->state != VFS_FILE_STATE_LIVE)
        return -KERNEL_EINVAL;
    struct kernel_vfs_node *node = file->private_data;
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 1);
    if (node->instance->read_only) return -KERNEL_EROFS;
    int result = mount_error(lwext4_instance(node->instance));
    if (result) return lwext4_error(result);
    result = lwext4_error(ext4_file_set_owner(lwext4_node_file(node), uid, gid));
    if (!result) file->mode = node->mode = kernel_vfs_chown_mode(node->mode);
    return result;
}

static int ext4_backend_open(struct kernel_vfs_mount *mount,
                        const char *path, uint64_t inode_number,
                        uint32_t inode_mode,
                        struct kernel_vfs_file *file)
{
    struct lwext4_mount_adapter *adapter;
    struct kernel_vfs_node *node;
    enum kernel_heap_status heap_status;
    uint32_t mode;
    int result;

    if (mount == 0 || mount->state != VFS_MOUNT_STATE_LIVE ||
        mount->private_data == 0 ||
        (path == 0 ? inode_number == 0U : path[0] != '/') ||
        file == 0 || file->state != VFS_FILE_STATE_EMPTY ||
        file->private_data != 0) {
        return -KERNEL_EINVAL;
    }
    if (inode_number > UINT32_MAX) return -KERNEL_EINVAL;
    adapter = mount->private_data;
    BACKEND_PATH(adapter, path);
    if (path != 0) {
        result = ext4_mode_get(path, &mode);
        if (result != EOK) return lwext4_error(result);
    } else {
        mode = inode_mode;
    }
    heap_status = kernel_heap_allocate_zeroed(adapter->instance.heap,
                                              1U,
                                              sizeof(struct lwext4_node),
                                              (void **)&node);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY ?
                   -KERNEL_ENOMEM : -KERNEL_EIO;
    }

    node->backend_data = &((struct lwext4_node *)node)->file;
    if (path == 0) {
        result = ext4_fopen_inode(lwext4_node_file(node), adapter->mount_point,
                                  inode_number);
        if (result != EOK) {
            kernel_heap_release(adapter->instance.heap, node);
            return lwext4_error(result);
        }
    } else if ((mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR) {
        /* ext4_fopen refuses directories; ext4_dir_open_file accepts them and
         * leaves the same file handle shape without allocating an ext4_dir on stack. */
        result = ext4_dir_open_file(lwext4_node_file(node), path);
        if (result != EOK) {
            kernel_heap_release(adapter->instance.heap, node);
            return lwext4_error(result);
        }
    } else if ((mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFCHR) {
        result = ext4_chrdev_open_file(lwext4_node_file(node), path);
        if (result != EOK) {
            kernel_heap_release(adapter->instance.heap, node);
            return lwext4_error(result);
        }
    } else {
        if (!adapter->instance.read_only) {
            result = ext4_fopen(lwext4_node_file(node), path, "r+");
            if (result != EOK) {
                result = ext4_fopen(lwext4_node_file(node), path, "r");
            }
        } else {
            result = ext4_fopen(lwext4_node_file(node), path, "r");
        }
        if (result != EOK) {
            kernel_heap_release(adapter->instance.heap, node);
            return lwext4_error(result);
        }
    }
    node->inode = lwext4_node_file(node)->inode;
    node->max_size = lwext4_node_file(node)->fmax;
    node->size = ext4_fsize(lwext4_node_file(node));
    node->mode = mode;
    result = load_times(node, adapter);
    if (result) {
        if (ext4_fclose(lwext4_node_file(node)) != EOK) {
            node->instance = &adapter->instance;
            kernel_vfs_cleanup_node(node);
        } else kernel_heap_release(adapter->instance.heap, node);
        return lwext4_error(result);
    }
    return kernel_vfs_publish_node(mount, node, file, 0);
}

struct namespace_create {
    const char *mount, *path, *target;
    ext4_file *file;
    uint32_t parent, mode, type, device;
};
static int apply_namespace_create(void *context)
{
    struct namespace_create *request = context;
    if (request->file && request->file->mp) {
        int result = ext4_fclose(request->file);
        if (result) return result;
        memset(request->file, 0, sizeof(*request->file));
    }
    return ext4_create_child(request->mount, request->parent, request->path,
        request->mode | request->type, request->device, request->target, request->file);
}
struct namespace_remove {
    const char *mount, *path;
    uint32_t parent, inode;
    bool orphan;
    int directory;
};
static int apply_namespace_remove(void *context)
{
    struct namespace_remove *request = context;
    request->inode = 0; request->orphan = false;
    return ext4_unlink_child(request->mount, request->parent, request->path,
        request->directory, &request->inode, &request->orphan);
}
struct namespace_rename {
    const char *mount,*old_name,*new_name;
    uint32_t old_parent,new_parent;
    unsigned flags;
    struct ext4_rename_result result;
};
static int apply_namespace_rename(void *context)
{
    struct namespace_rename *request=context;
    request->result=(struct ext4_rename_result){0};
    return ext4_rename_child(request->mount,request->old_parent,request->old_name,strlen(request->old_name),
        request->new_parent,request->new_name,strlen(request->new_name),request->flags,&request->result);
}
struct namespace_link { const char *mount,*name;uint32_t source,parent; };
static int apply_namespace_link(void *context)
{
    struct namespace_link *request=context;
    return ext4_link_child(request->mount,request->source,request->parent,request->name,strlen(request->name));
}


static int ext4_backend_create(struct kernel_vfs_mount *mount,
                          uint64_t parent_inode, const char *path,
                          uint32_t mode,
                          struct kernel_vfs_file *file)
{
    struct lwext4_mount_adapter *adapter;
    struct kernel_vfs_node *node;
    enum kernel_heap_status heap_status;
    int result;

    if (mount == 0 || mount->state != VFS_MOUNT_STATE_LIVE ||
        mount->private_data == 0 || path == 0 || !parent_inode || parent_inode > UINT32_MAX ||
        file == 0 || file->state != VFS_FILE_STATE_EMPTY ||
        file->private_data != 0) {
        return -KERNEL_EINVAL;
    }
    adapter = mount->private_data;
    if (adapter->instance.read_only) {
        return -KERNEL_EROFS;
    }
    if (!adapter->device.fs->jbd_journal) return -KERNEL_EOPNOTSUPP;
    heap_status = kernel_heap_allocate_zeroed(adapter->instance.heap,
                                              1U,
                                              sizeof(struct lwext4_node),
                                              (void **)&node);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY ?
                   -KERNEL_ENOMEM : -KERNEL_EIO;
    }

    node->backend_data = &((struct lwext4_node *)node)->file;
    struct namespace_create request={.mount=adapter->mount_point,.parent=parent_inode,.path=path,.file=lwext4_node_file(node),.mode=mode&07777U,.type=KERNEL_VFS_S_IFREG};
    result=ext4_metadata_execute(adapter->mount_point,apply_namespace_create,&request);
    if (result != EOK) {
        /* This unopened owner has no data or mapping; the mount owns any
         * uncertain journal state. An aborted create must not unlink by name. */
        if (lwext4_node_file(node)->mp && ext4_fclose(lwext4_node_file(node))!=EOK) {
            node->instance=&adapter->instance;
            kernel_vfs_cleanup_node(node);
        } else kernel_heap_release(adapter->instance.heap,node);
        return lwext4_error(result);
    }

    node->inode = lwext4_node_file(node)->inode;
    node->max_size = lwext4_node_file(node)->fmax;
    node->size = ext4_fsize(lwext4_node_file(node));
    node->mode = (mode & 07777U) | KERNEL_VFS_S_IFREG;
    result = load_times(node, adapter);
    if (result) {
        if (ext4_fclose(lwext4_node_file(node)) != EOK) {
            node->instance = &adapter->instance;
            kernel_vfs_cleanup_node(node);
        } else kernel_heap_release(adapter->instance.heap, node);
        return lwext4_error(result);
    }
    return kernel_vfs_publish_node(mount, node, file, 1);
}

static void ext4_backend_accessed(struct kernel_vfs_file *file)
{
    if (!file || file->state != VFS_FILE_STATE_LIVE || !file->private_data) return;
    struct kernel_vfs_node *base = file->private_data;
    if (base->instance->read_only || mount_error(lwext4_instance(base->instance))) return;
    if ((base->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG) {
        (void)ext4_file_touch(lwext4_node_file(base), EXT4_TIME_ATIME | EXT4_TIME_RELATIME);
        return;
    }
    struct lwext4_node *node = (struct lwext4_node *)base;
    if (node->noatime) return;
    struct ext4_timestamp now;
    if (!ext4_file_read_clock(lwext4_node_file(base), &now)) return;
    struct lwext4_mount_adapter *adapter = lwext4_instance(base->instance);
    int notify = 0;
    {
        TIMESTAMP_SCOPE(times, adapter);
        if (time_compare(node->times[1], node->times[0]) >= 0 ||
            time_compare(node->times[2], node->times[0]) >= 0 ||
            now.seconds - node->times[0].seconds >= 86400)
            notify = time_dirty_locked(node, EXT4_TIME_ATIME, now);
    }
    time_notify(adapter, notify);
}

static int ext4_backend_modified(struct kernel_vfs_file *file,
                              uint64_t offset, int append)
{
    struct kernel_vfs_node *node;
    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0) return -KERNEL_EINVAL;
    node = file->private_data;
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 1);
    if (node->instance->read_only) return -KERNEL_EROFS;
    if (append) offset = node->size;
    /* Linux generic/ext4 write checks reject the maximum position before
     * file_modified, even when the user buffer would fault. */
    if (offset >= node->max_size) return -KERNEL_EFBIG;
    int result = mount_error(lwext4_instance(node->instance));
    if (result) return lwext4_error(result);
    struct ext4_timestamp now;
    if (ext4_file_read_clock(lwext4_node_file(node), &now)) time_dirty((struct lwext4_node *)node,
                                      EXT4_TIME_MTIME | EXT4_TIME_CTIME, now);
    return 0;
}

static int ext4_backend_stat(const struct kernel_vfs_file *file,
                     struct kernel_vfs_stat *stat)
{
    const struct kernel_vfs_node *node;
    struct ext4_inode inode;
    int result;

    if (file == 0 || stat == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0 || file->mount == 0 ||
        file->mount->state != VFS_MOUNT_STATE_LIVE || file->mount->id == 0U) {
        return -KERNEL_EINVAL;
    }
    node = file->private_data;
    if (node->instance == 0 || lwext4_instance(node->instance)->superblock == 0)
        return -KERNEL_EIO;
    result = ext4_fraw_inode_fill(lwext4_node_file(node), &inode);
    if (result != EOK) return lwext4_error(result);
    fill_stat(file->mount, lwext4_instance(node->instance)->superblock,
              node->inode, &inode, stat);
    if ((node->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFREG) {
        stat->size = node->size;
        struct ext4_timestamp times[3];
        (void)ext4_backend_pending_times((struct kernel_vfs_node *)node, times);
        stat->atime = (struct kernel_vfs_timespec){times[0].seconds, times[0].nanoseconds};
        stat->mtime = (struct kernel_vfs_timespec){times[1].seconds, times[1].nanoseconds};
        stat->ctime = (struct kernel_vfs_timespec){times[2].seconds, times[2].nanoseconds};
    }
    return 0;
}

static int ext4_backend_mkdir(struct kernel_vfs_mount *mount,
                         uint64_t parent_inode, const char *path,
                         uint32_t mode)
{
    struct lwext4_mount_adapter *adapter;
    int result;

    if (mount == 0 || mount->state != VFS_MOUNT_STATE_LIVE ||
        mount->private_data == 0 || path == 0 || !parent_inode || parent_inode > UINT32_MAX) {
        return -KERNEL_EINVAL;
    }
    adapter = mount->private_data;
    if (adapter->instance.read_only) {
        return -KERNEL_EROFS;
    }

    if (!adapter->device.fs->jbd_journal) return -KERNEL_EOPNOTSUPP;
    struct namespace_create request={.mount=adapter->mount_point,.parent=parent_inode,.path=path,.mode=mode&07777U,.type=KERNEL_VFS_S_IFDIR};
    result=ext4_metadata_execute(adapter->mount_point,apply_namespace_create,&request);
    if (result != EOK) return lwext4_error(result);
    return 0;
}

static int ext4_backend_unlink(struct kernel_vfs_mount *mount,
                          uint64_t parent_inode, const char *path)
{
    struct lwext4_mount_adapter *adapter;
    struct kernel_vfs_node *node;
    struct lwext4_orphan *orphan = 0;
    uint32_t inode = 0U;
    bool is_orphan = false;
    int result;

    if (mount == 0 || mount->state != VFS_MOUNT_STATE_LIVE ||
        mount->private_data == 0 || path == 0 || !parent_inode || parent_inode > UINT32_MAX) {
        return -KERNEL_EINVAL;
    }
    adapter = mount->private_data;
    if (adapter->instance.read_only) {
        return -KERNEL_EROFS;
    }
    result = reserve_orphan(adapter, &orphan);
    if (result != 0) {
        return result;
    }

    struct namespace_remove request={.mount=adapter->mount_point,.parent=parent_inode,.path=path};
    result=ext4_metadata_execute(adapter->mount_point,apply_namespace_remove,&request);
    inode=request.inode;is_orphan=request.orphan;
    if (result != EOK) {
        release_orphan(adapter, orphan);
        return lwext4_error(result);
    }

    node = pin_linked_node(adapter, inode);

    if (node != 0) {
        if (is_orphan) {
            node->unlinked = 1U;
            ext4_backend_release_unlinked(node);
        }
        release_orphan(adapter, orphan);
        if (kernel_vfs_node_release(&node)) __builtin_trap();
        return 0;
    } else if (is_orphan) {
        orphan->inode = inode;
        orphan->orphan_freed = 0U;
        result = free_orphan_inode(adapter, inode);
        if (result != EOK) {
            queue_orphan(adapter, orphan);
            return 0;
        }
        orphan->orphan_freed = 1U;
    }
    release_orphan(adapter, orphan);
    return 0;
}

static int ext4_backend_rmdir(struct kernel_vfs_mount *mount,
                         uint64_t parent_inode, const char *path)
{
    struct lwext4_mount_adapter *adapter;
    struct kernel_vfs_node *node;
    struct lwext4_orphan *orphan = 0;
    uint32_t inode = 0U;
    int result;

    if (mount == 0 || mount->state != VFS_MOUNT_STATE_LIVE ||
        mount->private_data == 0 || path == 0 || !parent_inode || parent_inode > UINT32_MAX) {
        return -KERNEL_EINVAL;
    }
    adapter = mount->private_data;
    if (adapter->instance.read_only) {
        return -KERNEL_EROFS;
    }
    result = reserve_orphan(adapter, &orphan);
    if (result != 0) return result;
    struct namespace_remove request={.mount=adapter->mount_point,.parent=parent_inode,.path=path,.directory=1};
    result=ext4_metadata_execute(adapter->mount_point,apply_namespace_remove,&request);
    inode=request.inode;
    if (result != EOK) {
        release_orphan(adapter, orphan);
        return lwext4_error(result);
    }
    node = pin_linked_node(adapter, inode);
    if (node != 0) {
        node->unlinked = 1U;
        ext4_backend_release_unlinked(node);
        release_orphan(adapter, orphan);
        if (kernel_vfs_node_release(&node)) __builtin_trap();
        return 0;
    }
    orphan->inode = inode;
    orphan->orphan_freed = 0U;
    result = free_orphan_inode(adapter, inode);
    if (result != EOK) {
        queue_orphan(adapter, orphan);
        return 0;
    }
    orphan->orphan_freed = 1U;
    release_orphan(adapter, orphan);
    return 0;
}

static void ext4_backend_release_unlinked(struct kernel_vfs_node *node)
{
    struct kernel_vfs_node *temp;
    int result;

    if (node == 0) return;
    __atomic_fetch_add(&node->references,1,__ATOMIC_RELAXED);
    struct kernel_lock_guard guard = {0};
    kernel_vfs_node_lock(node, &guard, 1);
    if (!node->unlinked || node->retired) {
        goto out;
    }
    if (node->open_files > 0U || node->exec_users > 0U) {
        goto out;
    }

    if (node->instance != 0 && node->instance->page_cache != 0) {
        /* Inode writer qualification has drained submitted backend I/O.
         * Retire prepared snapshots through the same path as truncation;
         * they observe retirement before obtaining the inode read side. */
        kernel_page_cache_truncate(node->instance->page_cache, node, 0);
    }

    result = free_orphan_inode(lwext4_instance(node->instance), node->inode);
    if (result == EOK) {
        node->retired = 1U;
        struct lwext4_mount_adapter *adapter = lwext4_instance(node->instance);
        struct kernel_vfs_node *dirty_pin;
        {
            TIMESTAMP_SCOPE(times, adapter);
            ((struct lwext4_node *)node)->dirty_fields = 0;
            dirty_pin = time_drop_dirty((struct lwext4_node *)node);
        }
        if (dirty_pin && kernel_vfs_node_release(&dirty_pin)) __builtin_trap();
    }

out:
    kernel_lock_scope_release(&guard);
    temp = node;
    (void)kernel_vfs_node_release(&temp);

}

static int ext4_backend_dir_entry(struct kernel_vfs_file *file,
                         uint64_t position,
                         uint64_t *next_position,
                         uint64_t *inode,
                         uint8_t *type,
                         char *name,
                         size_t name_size)
{
    struct kernel_vfs_node *node;
    ext4_dir directory;
    uint64_t entry_offset;
    uint64_t start;
    int result;
    size_t name_length;

    if (file == 0 || file->private_data == 0 || next_position == 0 ||
        inode == 0 || type == 0 || name == 0 || name_size == 0U ||
        file->state != VFS_FILE_STATE_LIVE) {
        return -KERNEL_EINVAL;
    }
    node = file->private_data;
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 0);
    if ((node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) {
        return -KERNEL_ENOTDIR;
    }
    struct kernel_vfs_stat current;
    result = kernel_vfs_fstat(file, &current);
    if (result) return result;
    *next_position = position;
    if (position >= current.size) {
        *next_position = current.size;
        return 0;
    }
    start = position & ~UINT64_C(3);
    directory.f = (*lwext4_node_file(node));
    directory.f.fsize = current.size;
    directory.next_off = start;
    for (;;) {
        result = ext4_dir_entry_next_status(&directory,
                                            &directory.de,
                                            &entry_offset);
        if (result != EOK) {
            return lwext4_error(result);
        }
        if (entry_offset == UINT64_MAX) {
            *next_position = current.size;
            return 0;
        }
        if (entry_offset < position) {
            if (directory.next_off <= entry_offset ||
                directory.next_off == UINT64_MAX) {
                return -KERNEL_EIO;
            }
            continue;
        }
        name_length = directory.de.name_length;
        if (name_length >= name_size) {
            return -KERNEL_ENAMETOOLONG;
        }
        memcpy(name, directory.de.name, name_length);
        name[name_length] = '\0';
        *inode = directory.de.inode;
        *type = dirent_type_from_ext4(directory.de.inode_type);
        *next_position = directory.next_off;
        if (entry_offset >= current.size || *next_position <= entry_offset ||
            *next_position > current.size ||
            *next_position == UINT64_MAX) {
            return -KERNEL_EIO;
        }
        return 1;
    }
}

static int ext4_backend_pread(struct kernel_vfs_node *node,
                          uint64_t offset,
                          void *buffer,
                          size_t size,
                          size_t *bytes_read)
{
    size_t requested;
    size_t result_count;
    int result;

    if (node == 0 || __atomic_load_n(&node->references,__ATOMIC_ACQUIRE) == 0U || node->closed ||
        bytes_read == 0 || (buffer == 0 && size != 0U)) {
        return -KERNEL_EINVAL;
    }
    if (offset > INT64_MAX) {
        return -KERNEL_EOVERFLOW;
    }
    if (offset >= node->size || size == 0U) {
        *bytes_read = 0U;
        return 0;
    }
    requested = size;
    if ((uint64_t)requested > node->size - offset) {
        requested = (size_t)(node->size - offset);
    }
    struct lwext4_mount_adapter *adapter=lwext4_instance(node->instance);
    adapter->locks.read_lock(adapter->locks.context);
    uint64_t stored=ext4_fsize(lwext4_node_file(node));
    if (stored>node->size) stored=node->size;
    size_t prefix=offset>=stored?0:stored-offset<requested?(size_t)(stored-offset):requested;
    result = ext4_fpread(lwext4_node_file(node), offset, buffer, prefix, &result_count);
    adapter->locks.unlock(adapter->locks.context);
    if (result != EOK) return lwext4_error(result);
    if (result_count != prefix) return -KERNEL_EIO;
    /* Cache-only growth has known zero holes beyond the accepted inode EOF. */
    memset((unsigned char *)buffer+prefix,0,requested-prefix);
    *bytes_read = requested;
    return 0;
}

struct ext4_file_read_progress {
    struct kernel_vfs_node *node;
    uint64_t stored;
    const struct file_read_completion *completion;
    uint32_t completed;
};
static void file_read_result(struct ext4_file_read_progress *progress, struct file_read_span *span, int fill)
{
    span->error = lwext4_error(span->error);
    size_t requested = span->offset >= progress->stored ? 0 : progress->stored - span->offset < span->size ?
        (size_t)(progress->stored - span->offset) : span->size;
    if (span->completed > requested) __builtin_trap();
    if (!span->error && span->completed != requested) span->error = -KERNEL_EIO;
    if (!span->error) {
        uint64_t size = progress->node->size;
        size_t logical = span->offset >= size ? 0 : size - span->offset < span->size ?
            (size_t)(size - span->offset) : span->size;
        if (logical < span->completed) __builtin_trap();
        if (fill) memset((unsigned char *)span->buffer + span->completed, 0, logical - span->completed);
        span->completed = logical;
    }
}
static void file_read_completed(void *context, unsigned index, const struct file_read_span *span)
{
    struct ext4_file_read_progress *progress = context;
    struct file_read_span result = *span;
    file_read_result(progress, &result, 1);
    progress->completed |= UINT32_C(1) << index;
    progress->completion->complete(progress->completion->context, index, &result);
}
static int ext4_backend_pread_batch(struct kernel_vfs_node *node,
    struct file_read_span *spans, size_t count, struct file_read_workspace *workspace,
    const struct file_read_completion *completion)
{
    if (!node || !__atomic_load_n(&node->references,__ATOMIC_ACQUIRE) || node->closed || count > KERNEL_VFS_READ_BATCH_MAX) return -KERNEL_EINVAL;
    struct lwext4_mount_adapter *adapter = lwext4_instance(node->instance);
    adapter->locks.read_lock(adapter->locks.context);
    uint64_t stored = ext4_fsize(lwext4_node_file(node));
    if (stored > node->size) stored = node->size;
    struct ext4_file_read_progress progress = {node, stored, completion, 0};
    const struct file_read_completion forward = {file_read_completed, &progress};
    int result = ext4_fpread_batch(lwext4_node_file(node), spans, count, workspace, stored,
        completion ? &forward : NULL);
    adapter->locks.unlock(adapter->locks.context);
    for (size_t i = 0; i < count; i++)
        file_read_result(&progress, &spans[i], !(progress.completed & (UINT32_C(1) << i)));
    return lwext4_error(result);
}

static int ext4_backend_writeback(struct kernel_vfs_node *node,
    struct file_write_span *spans, size_t count)
{
    _Static_assert(KERNEL_VFS_WRITEBACK_MAX <= EXT4_BLOCK_BATCH_MAX,
        "VFS writeback batches must fit ext4");
    if (count > KERNEL_VFS_WRITEBACK_MAX) __builtin_trap();
    for (size_t i = 0; i < count; i++) COST_ADD(BACKEND_REQUESTED, spans[i].size);
    int result = ext4_fpwrite_batch(lwext4_node_file(node), spans, count);
    for (size_t i = 0; i < count; i++) {
        COST_ADD(BACKEND_ACCEPTED, spans[i].written);
    }
    return lwext4_error(result);
}

static int ext4_backend_error(struct kernel_vfs_instance *instance)
{ return instance->quiescing ? -KERNEL_EIO : lwext4_error(mount_error(lwext4_instance(instance))); }
static int ext4_backend_root(struct kernel_vfs_instance *instance, uint64_t *inode, uint32_t *mode)
{ *inode = EXT4_INODE_ROOT_INDEX; return lwext4_error(ext4_mode_get(lwext4_instance(instance)->mount_point, mode)); }
static int ext4_backend_lookup(struct kernel_vfs_instance *instance, uint64_t parent,
    const char *name, size_t length, uint64_t *inode, uint32_t *mode)
{
    int error = ext4_backend_error(instance);
    if (error) return error;
    if (parent > UINT32_MAX) return -KERNEL_EINVAL;
    uint32_t result_inode;
    error = lwext4_error(ext4_lookup_child(lwext4_instance(instance)->mount_point, (uint32_t)parent,
                                             name, length, &result_inode, mode));
    if (!error) *inode = result_inode;
    return error;
}
static int ext4_backend_readlink(struct kernel_vfs_node *node, char *buffer, size_t size, size_t *count)
{ return lwext4_error(ext4_readlink_inode(lwext4_instance(node->instance)->mount_point, node->inode, buffer, size, count)); }
static int ext4_backend_truncate(struct kernel_vfs_node *node, uint64_t size, uint64_t *actual, int *changed)
{
    uint64_t before = ext4_fsize(lwext4_node_file(node));
    int result = ext4_ftruncate(lwext4_node_file(node), size);
    *actual = ext4_fsize(lwext4_node_file(node));
    *changed = result == EOK || before != *actual;
    return lwext4_error(result);
}
static int ext4_backend_fallocate(struct kernel_vfs_node *node, unsigned mode,
    uint64_t offset, uint64_t length, uint64_t *actual, int *changed)
{
    bool committed;
    int result = ext4_fallocate(lwext4_node_file(node), mode, offset, length, &committed);
    *actual = ext4_fsize(lwext4_node_file(node));
    *changed = committed;
    return lwext4_error(result);
}
static int ext4_backend_prepare_write(struct kernel_vfs_node *node,uint64_t offset,
    size_t length,size_t *ready,int *complete)
{
    bool needed;
    int result=ext4_file_prepare_write(lwext4_node_file(node),offset,length,ready,&needed);
    if (needed && !lwext4_instance(node->instance)->device.fs->jbd_journal)
        ((struct lwext4_node *)node)->write_completion=1;
    *complete=needed;
    return lwext4_error(result);
}
static int ext4_backend_complete_write(struct kernel_vfs_node *node)
{
    /* Legacy 指针没有 unwritten 状态。已接纳数据先交给后端，再按持久
     * EOF 回收没有被复制的尾部；失败保留脏数据及持久 orphan owner。 */
    KERNEL_LOCK_SCOPE(io); kernel_vfs_node_lock(node,&io,1);
    int result=ext4_file_complete_write(lwext4_node_file(node));
    if (!result) ((struct lwext4_node *)node)->write_completion=0;
    return lwext4_error(result);
}
static struct kernel_block_device *ext4_backend_block_device(struct kernel_vfs_instance *instance)
{ return lwext4_instance(instance)->block; }
static int ext4_backend_close_node(struct kernel_vfs_node *node)
{
    /* close 不做递归 I/O；通用末引用路径把未完成的对象交给 cleanup_nodes。 */
    if (((struct lwext4_node *)node)->write_completion) return -KERNEL_EIO;
    return lwext4_error(ext4_fclose(lwext4_node_file(node)));
}
static int ext4_backend_writeback_allowed(struct kernel_vfs_instance *instance)
{ (void)instance; return !boaros_lwext4_allocation_active(); }
static void ext4_backend_sync_prepare(struct kernel_vfs_node *node, int data_only,
    struct kernel_vfs_sync_request *request)
{
    struct lwext4_mount_adapter *adapter = lwext4_instance(node->instance);
    int result = writeback_times(node, true);
    if (result == EOK)
        result = ext4_file_sync_metadata_prepare(lwext4_node_file(node), data_only != 0,
                                                 &request->target);
    if (result == EOK && !adapter->device.fs->jbd_journal) result = block_flush(&adapter->device);
    if (result != EOK || !request->target) {
        kernel_vfs_sync_complete(request, lwext4_error(result));
        return;
    }
    int reached;
    {
        KERNEL_TASK_LOCK_SCOPE(guard, &adapter->journal_control);
        result = adapter->published_error;
        reached = result || adapter->published_durable >= request->target;
        if (!reached) adapter->sync_waiters = sync_target_insert(adapter->sync_waiters, request);
    }
    if (reached) kernel_vfs_sync_complete(request, lwext4_error(result));
}
static int ext4_backend_flush(struct kernel_vfs_instance *instance)
{
    enum kernel_block_status status = kernel_block_flush(lwext4_instance(instance)->block);
    return status == KERNEL_BLOCK_STATUS_OK ? 0 :
        status == KERNEL_BLOCK_STATUS_UNSUPPORTED ? -KERNEL_ENOTSUP : -KERNEL_EIO;
}
static int ext4_backend_sync_filesystem(struct kernel_vfs_instance *instance,
                                        struct kernel_vfs_node *root)
{
    struct lwext4_mount_adapter *adapter = lwext4_instance(instance);
    int result = writeback_mount_times(adapter, true);
    /* Timestamp failures have already been published on the affected inode. */
    if (result != EOK) return lwext4_error(result);
    result = ext4_file_sync_metadata_mode(lwext4_node_file(root), false);
    if (result == EOK && !adapter->device.fs->jbd_journal) {
        result = ext4_cache_flush(adapter->mount_point);
        if (result == EOK) result = block_flush(&adapter->device);
    }
    if (result != EOK) kernel_vfs_record_writeback_error(root, lwext4_error(result));
    return lwext4_error(result);
}
static int ext4_backend_symlink(struct kernel_vfs_instance *instance,
    uint64_t parent_inode, const char *target, const char *name)
{
    if (!parent_inode || parent_inode > UINT32_MAX) return -KERNEL_EOVERFLOW;
    struct lwext4_mount_adapter *adapter = lwext4_instance(instance);
    struct namespace_create request = {.mount=adapter->mount_point, .parent=parent_inode,
        .path=name, .target=target, .type=KERNEL_VFS_S_IFLNK, .mode=0777};
    return lwext4_error(ext4_metadata_execute(adapter->mount_point, apply_namespace_create, &request));
}
static int ext4_backend_mknod(struct kernel_vfs_instance *instance,
    uint64_t parent_inode, const char *name, uint32_t type, uint32_t mode, uint32_t device)
{
    if (!parent_inode || parent_inode > UINT32_MAX) return -KERNEL_EOVERFLOW;
    struct lwext4_mount_adapter *adapter = lwext4_instance(instance);
    if (!adapter->device.fs->jbd_journal) return -KERNEL_EOPNOTSUPP;
    struct namespace_create request = {.mount=adapter->mount_point, .parent=parent_inode,
        .path=name, .type=type, .mode=mode & 07777U, .device=device};
    return lwext4_error(ext4_metadata_execute(adapter->mount_point, apply_namespace_create, &request));
}
static int ext4_backend_rename(struct kernel_vfs_instance *instance,
    uint64_t old_parent, const char *old_name, uint64_t new_parent,
    const char *new_name, unsigned flags, struct kernel_vfs_rename_result *result)
{
    struct lwext4_mount_adapter *adapter = lwext4_instance(instance);
    if (old_parent > UINT32_MAX || new_parent > UINT32_MAX)
        return -KERNEL_EINVAL;
    struct namespace_rename request={.mount=adapter->mount_point,.old_parent=old_parent,
        .old_name=old_name,.new_parent=new_parent,.new_name=new_name,.flags=flags};
    int status=ext4_metadata_execute(adapter->mount_point,apply_namespace_rename,&request);
    if (!status) {
        result->replaced_inode = request.result.replaced_inode;
        result->replaced_last_link = request.result.replaced_last_link;
        result->changed = request.result.changed;
    }
    return lwext4_error(status);
}

static int ext4_backend_link(struct kernel_vfs_instance *instance,
    uint64_t source, uint64_t parent, const char *name)
{
    if (source > UINT32_MAX || parent > UINT32_MAX) return -KERNEL_EINVAL;
    const char *mount=lwext4_instance(instance)->mount_point;
    struct namespace_link request={mount,name,source,parent};
    return lwext4_error(ext4_metadata_execute(mount,apply_namespace_link,&request));
}

static void initialize_backend(void)
{
    /* 首次在当前执行域生成回调；其他挂载不能再写远端读者共享的表。 */
    if (__atomic_load_n(&ext4_backend.pread_batch,__ATOMIC_ACQUIRE)) return;
    volatile struct kernel_vfs_backend *ops = &ext4_backend;
    ops->read_execution = KERNEL_VFS_EXECUTION_CALLER;
    ops->file_metadata_execution = KERNEL_VFS_EXECUTION_CALLER;
    ops->namespace_execution = KERNEL_VFS_EXECUTION_CALLER;
    ops->error = ext4_backend_error;
    ops->root = ext4_backend_root;
    ops->lookup = ext4_backend_lookup;
    ops->readlink = ext4_backend_readlink;
    ops->check_domain = ext4_check_domain;
    ops->truncate = ext4_backend_truncate;
    ops->fallocate = ext4_backend_fallocate;
    ops->prepare_write = ext4_backend_prepare_write;
    ops->complete_write = ext4_backend_complete_write;
    ops->block_device = ext4_backend_block_device;
    ops->close_node = ext4_backend_close_node;
    ops->writeback_allowed = ext4_backend_writeback_allowed;
    ops->sync_prepare = ext4_backend_sync_prepare;
    ops->sync_filesystem = ext4_backend_sync_filesystem;
    ops->flush = ext4_backend_flush;
    ops->symlink = ext4_backend_symlink;
    ops->mknod = ext4_backend_mknod;
    ops->rename = ext4_backend_rename;
    ops->link = ext4_backend_link;
    ops->unmount = ext4_backend_unmount;
    ops->prepare_unmount = ext4_backend_prepare_unmount;
    ops->statfs = ext4_backend_statfs;
    ops->set_times = ext4_backend_set_times;
    ops->set_mode = ext4_backend_set_mode;
    ops->set_owner = ext4_backend_set_owner;
    ops->open = ext4_backend_open;
    ops->create = ext4_backend_create;
    ops->accessed = ext4_backend_accessed;
    ops->modified = ext4_backend_modified;
    ops->stat = ext4_backend_stat;
    ops->mkdir = ext4_backend_mkdir;
    ops->unlink = ext4_backend_unlink;
    ops->rmdir = ext4_backend_rmdir;
    ops->release_unlinked = ext4_backend_release_unlinked;
    ops->dir_entry = ext4_backend_dir_entry;
    ops->pread = ext4_backend_pread;
    ops->writeback = ext4_backend_writeback;
    __atomic_store_n(&ext4_backend.pread_batch,ext4_backend_pread_batch,__ATOMIC_RELEASE);
}
