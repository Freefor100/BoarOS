#include "lwext4_port.h"
#include "vfs_internal.h"

#include <kernel/block.h>
#include <kernel/errno.h>
#include <kernel/file_mapping.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/page_cache.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/vfs.h>
#include <kernel/time.h>

#include <ext4.h>
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

#define VFS_MOUNT_STATE_EMPTY 0U
#define VFS_MOUNT_STATE_LIVE UINT32_C(0x564d4e54)
#define VFS_MOUNT_STATE_CLEANUP UINT32_C(0x56434c4e)
#define VFS_FILE_STATE_EMPTY 0U
#define VFS_FILE_STATE_LIVE UINT32_C(0x5646494c)
#define VFS_FILE_STATE_CLEANUP UINT32_C(0x5646434c)

#define LWEXT4_DEVICE_NAME "root"
#define LWEXT4_MOUNT_POINT "/"
#define LWEXT4_PHYSICAL_BLOCK_SIZE 512U
#define VFS_ROOT_MOUNT_ID UINT64_C(1)
#define VFS_SYMLINK_MAX_FOLLOWS 40U

static int lwext4_error(int error);

/* Mutation backends still take a pathname; derive it from the same held
 * parent/inode walk used by open and stat.  The pathname is only an adapter
 * argument, never the authority for deciding which object was found. */
static int resolve_object(struct kernel_vfs_path *start,
                          struct kernel_vfs_path *root,
                          const char *path, int follow_final,
                          int allow_missing_final,
                          int allow_missing_trailing,
                          int follow_trailing_final,
                          struct kernel_vfs_path **owner,
                          char missing_name[256]);
static int path_to_backend_name(const struct kernel_vfs_path *path,
                                const char *missing_name,
                                char *buffer, size_t capacity);

static void release_path(struct kernel_heap *heap, char *work)
{
    if (kernel_heap_release(heap, work) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
}

struct lwext4_orphan {
    struct lwext4_orphan *next;
    uint32_t inode;
    uint8_t orphan_freed;
};

struct lwext4_mount_adapter {
    struct kernel_heap *heap;
    struct kernel_block_device *block;
    struct ext4_blockdev_iface interface;
    struct ext4_blockdev device;
    unsigned char *physical_buffer;
    struct kernel_page_cache *page_cache;
    struct ext4_sblock *superblock;
    struct kernel_vfs_node *nodes;
    struct kernel_vfs_node *cleanup_nodes;
    struct lwext4_orphan *orphans;
    struct kernel_vfs_path *paths; /* Weak registry; live callers own references. */
    uint32_t external_files;
    uint8_t registered;
    uint8_t mounted;
    uint8_t heap_bound;
    uint8_t read_only;
    uint8_t unmount_sync_pending;
    uint8_t recovery_pending;
    int mount_error;
};

static int mount_error(const struct lwext4_mount_adapter *adapter)
{
    if (adapter->mount_error) return adapter->mount_error;
    struct ext4_fs *fs = adapter->device.fs;
    return fs && fs->jbd_journal ? fs->jbd_journal->error : EOK;
}

/* Resource exhaustion before commit remains recoverable. Keep the actual
 * mount/cache owner and resume preparation before trying to tear it down. */
static int prepare_journal_mount(struct lwext4_mount_adapter *adapter)
{
    struct ext4_fs *fs = adapter->device.fs;
    int result = EOK;
    if (!fs->jbd_journal) {
        if (fs->jbd_fs)
            result = ext4_journal_stop(LWEXT4_MOUNT_POINT);
        if (result == EOK) result = ext4_recover(LWEXT4_MOUNT_POINT);
        if (result == EOK) result = ext4_orphan_validate(fs);
        if (result == EOK) result = ext4_journal_start(LWEXT4_MOUNT_POINT);
    }
    if (result == EOK) result = ext4_orphan_recover(LWEXT4_MOUNT_POINT);
    if (result == EOK)
        adapter->recovery_pending = 0U;
    else if (result != ENOMEM && result != ENOSPC)
        adapter->mount_error = result;
    return result;
}

struct kernel_vfs_node {
    struct kernel_vfs_node *next;
    struct kernel_page_cache_entry *cache_pages;
    struct kernel_file_mapping *mappings;
    struct lwext4_mount_adapter *adapter;
    struct kernel_vfs_mount *mount;
    ext4_file file;
    uint64_t size;
    uint64_t writeback_error_sequence;
    int writeback_error;
    uint32_t mode;
    uint32_t references;
    uint32_t open_files;
    uint32_t write_openers;
    uint32_t exec_users;
    uint8_t closed;
    uint8_t unlinked;
    uint8_t orphan_freed;
};

struct kernel_vfs_path {
    struct kernel_vfs_file file;
    struct kernel_heap *heap;
    struct kernel_vfs_path *parent;
    struct kernel_vfs_path *next;
    struct kernel_vfs_path **previous;
    uint32_t references;
    uint8_t detached;
    char *name;
    char initial_name[];
};

static void register_path(struct kernel_vfs_path *path)
{
    struct lwext4_mount_adapter *adapter = path->file.mount->private_data;
    path->next = adapter->paths;
    path->previous = &adapter->paths;
    if (path->next) path->next->previous = &path->next;
    adapter->paths = path;
}

static void unregister_path(struct kernel_vfs_path *path)
{
    if (!path->previous || *path->previous != path) __builtin_trap();
    *path->previous = path->next;
    if (path->next) path->next->previous = path->previous;
    path->next = 0;
    path->previous = 0;
}

static int path_to_backend_name(const struct kernel_vfs_path *path,
                                const char *missing_name,
                                char *buffer, size_t capacity)
{
    const struct kernel_vfs_path *cursor = path;
    size_t position;
    size_t length;

    if (path == 0 || path->references == 0U || buffer == 0 ||
        capacity < 2U) return -KERNEL_EINVAL;
    position = capacity - 1U;
    buffer[position] = '\0';
    if (missing_name != 0 && missing_name[0] != '\0') {
        length = strlen(missing_name);
        if (length > 255U || length >= position)
            return -KERNEL_ENAMETOOLONG;
        position -= length;
        memcpy(buffer + position, missing_name, length);
        if (position == 0U) return -KERNEL_ENAMETOOLONG;
        buffer[--position] = '/';
    }
    while (cursor->parent != 0) {
        if (cursor->detached ||
            ((struct kernel_vfs_node *)cursor->file.private_data)->unlinked)
            return -KERNEL_ENOENT;
        length = strlen(cursor->name);
        if (length == 0U || length >= position)
            return -KERNEL_ENAMETOOLONG;
        position -= length;
        memcpy(buffer + position, cursor->name, length);
        if (position == 0U) return -KERNEL_ENAMETOOLONG;
        buffer[--position] = '/';
        cursor = cursor->parent;
    }
    if (position == capacity - 1U) buffer[--position] = '/';
    memmove(buffer, buffer + position, capacity - position);
    return 0;
}

static int vfs_open_inode_raw(struct kernel_vfs_mount *mount,
                              uint32_t inode, uint32_t mode,
                              struct kernel_vfs_file *file);

uint64_t kernel_vfs_path_inode(const struct kernel_vfs_path *path)
{
    const struct kernel_vfs_node *node;
    if (path == 0 || path->references == 0U ||
        path->file.private_data == 0) return 0;
    node = path->file.private_data;
    return node->file.inode;
}

int kernel_vfs_path_stat(const struct kernel_vfs_path *path,
                         struct kernel_vfs_stat *stat)
{
    if (path == 0 || path->references == 0U)
        return -KERNEL_EINVAL;
    return kernel_vfs_fstat(&path->file, stat);
}

int kernel_vfs_path_lookup(struct kernel_vfs_path *parent,
                           const char *name, size_t name_length,
                           struct kernel_vfs_path **owner)
{
    struct kernel_vfs_path *child;
    struct kernel_vfs_mount *mount;
    struct lwext4_mount_adapter *adapter;
    enum kernel_heap_status allocation;
    uint32_t inode, mode;
    int result;

    if (parent == 0 || parent->references == 0U || name == 0 ||
        name_length == 0U || owner == 0 ||
        *owner != 0 || parent->file.private_data == 0)
        return -KERNEL_EINVAL;
    if (name_length > 255U) return -KERNEL_ENAMETOOLONG;
    if ((parent->file.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR)
        return -KERNEL_ENOTDIR;
    if (name_length == 1U && name[0] == '.') {
        result = kernel_vfs_path_acquire(parent);
        if (result == 0) *owner = parent;
        return result;
    }
    if (name_length == 2U && name[0] == '.' && name[1] == '.') {
        struct kernel_vfs_path *ancestor = parent->parent != 0
                                                ? parent->parent : parent;
        result = kernel_vfs_path_acquire(ancestor);
        if (result == 0) *owner = ancestor;
        return result;
    }
    if (((struct kernel_vfs_node *)parent->file.private_data)->unlinked)
        return -KERNEL_ENOENT;
    for (size_t i = 0; i < name_length; i++)
        if (name[i] == '/' || name[i] == '\0') return -KERNEL_EINVAL;
    mount = parent->file.mount;
    if (mount == 0 || mount->private_data == 0) return -KERNEL_EINVAL;
    adapter = mount->private_data;
    result = mount_error(adapter);
    if (result != EOK) return lwext4_error(result);
    result = ext4_lookup_child(LWEXT4_MOUNT_POINT,
                               (uint32_t)kernel_vfs_path_inode(parent),
                               name, (uint32_t)name_length, &inode, &mode);
    if (result != EOK) return lwext4_error(result);
    for (child = adapter->paths; child != 0; child = child->next) {
        if (!child->detached && child->parent == parent &&
            strlen(child->name) == name_length &&
            !memcmp(child->name, name, name_length)) {
            if (kernel_vfs_path_inode(child) != inode) {
                child->detached = 1U;
                continue;
            }
            result = kernel_vfs_path_acquire(child);
            if (result == 0) *owner = child;
            return result;
        }
    }
    allocation = kernel_heap_allocate_zeroed(adapter->heap, 1U,
                    sizeof(*child) + name_length + 1U, (void **)&child);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                      : -KERNEL_EIO;
    result = kernel_vfs_path_acquire(parent);
    if (result != 0) {
        if (kernel_heap_release(adapter->heap, child) !=
            KERNEL_HEAP_STATUS_OK) __builtin_trap();
        return result;
    }
    child->heap = adapter->heap;
    child->parent = parent;
    child->references = 1U;
    result = vfs_open_inode_raw(mount, inode, mode, &child->file);
    if (result != 0) {
        struct kernel_vfs_path *parent_owner = child->parent;
        if (kernel_vfs_path_release(&parent_owner) != 0)
            __builtin_trap();
        if (kernel_heap_release(adapter->heap, child) !=
            KERNEL_HEAP_STATUS_OK) __builtin_trap();
        return result;
    }
    child->name = child->initial_name;
    memcpy(child->name, name, name_length);
    child->name[name_length] = '\0';
    register_path(child);
    *owner = child;
    return 0;
}

static int resolve_object(struct kernel_vfs_path *start,
                          struct kernel_vfs_path *root,
                          const char *path, int follow_final,
                          int allow_missing_final,
                          int allow_missing_trailing,
                          int follow_trailing_final,
                          struct kernel_vfs_path **owner,
                          char missing_name[256])
{
    struct kernel_vfs_path *current;
    struct lwext4_mount_adapter *adapter;
    char *work = 0;
    char *pending, *target;
    size_t cursor = 0U;
    size_t length;
    uint32_t follows = 0U;
    enum kernel_heap_status allocation;
    int result = 0;

    if (start == 0 || root == 0 || path == 0 || owner == 0 ||
        *owner != 0 ||
        start->file.mount != root->file.mount ||
        start->file.mount == 0 || start->file.mount->private_data == 0)
        return -KERNEL_EINVAL;
    if (path[0] == '\0') return -KERNEL_ENOENT;
    adapter = start->file.mount->private_data;
    length = strlen(path);
    if (length >= KERNEL_FS_PATH_MAX) return -KERNEL_ENAMETOOLONG;
    allocation = kernel_heap_allocate(adapter->heap,
                                      2U * KERNEL_FS_PATH_MAX,
                                      (void **)&work);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                      : -KERNEL_EIO;
    pending = work;
    target = work + KERNEL_FS_PATH_MAX;
    memcpy(pending, path, length + 1U);
    current = pending[0] == '/' ? root : start;
    result = kernel_vfs_path_acquire(current);
    if (result != 0) {
        current = 0;
        goto Finish;
    }
    while (pending[cursor] != '\0') {
        struct kernel_vfs_path *next = 0;
        size_t begin, end, component_length;
        int suffix;
        int trailing_only;

        while (pending[cursor] == '/') cursor++;
        if (pending[cursor] == '\0') break;
        begin = cursor;
        while (pending[cursor] != '\0' && pending[cursor] != '/') cursor++;
        end = cursor;
        component_length = end - begin;
        suffix = pending[end] != '\0';
        {
            size_t remainder = end;
            while (pending[remainder] == '/') remainder++;
            trailing_only = pending[remainder] == '\0';
        }
        if (component_length > 255U) {
            result = -KERNEL_ENAMETOOLONG;
            goto Finish;
        }
        result = kernel_vfs_path_lookup(current, pending + begin,
                                        component_length, &next);
        if (result == -KERNEL_ENOENT && allow_missing_final &&
            (!suffix || (allow_missing_trailing && trailing_only)) &&
            missing_name != 0) {
            memcpy(missing_name, pending + begin, component_length);
            missing_name[component_length] = '\0';
            *owner = current;
            current = 0;
            result = 0;
            goto Finish;
        }
        if (result != 0) goto Finish;
        if ((next->file.mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFLNK &&
            ((suffix && (!trailing_only || follow_trailing_final == 1)) ||
             follow_final)) {
            size_t target_length = 0U;
            size_t remainder = strlen(pending + end);
            int backend;
            if (++follows > VFS_SYMLINK_MAX_FOLLOWS) {
                result = -KERNEL_ELOOP;
                (void)kernel_vfs_path_release(&next);
                goto Finish;
            }
            backend = ext4_readlink_inode(LWEXT4_MOUNT_POINT,
                         (uint32_t)kernel_vfs_path_inode(next), target,
                         KERNEL_FS_PATH_MAX - 1U, &target_length);
            (void)kernel_vfs_path_release(&next);
            if (backend != EOK) {
                result = lwext4_error(backend);
                goto Finish;
            }
            if (target_length == 0U) {
                result = -KERNEL_ENOENT;
                goto Finish;
            }
            if (target_length + remainder >= KERNEL_FS_PATH_MAX) {
                result = -KERNEL_ENAMETOOLONG;
                goto Finish;
            }
            memmove(pending + target_length, pending + end, remainder + 1U);
            memcpy(pending, target, target_length);
            if (target[0] == '/') {
                (void)kernel_vfs_path_release(&current);
                current = root;
                result = kernel_vfs_path_acquire(current);
                if (result != 0) {
                    current = 0;
                    goto Finish;
                }
            }
            cursor = 0U;
            continue;
        }
        if (suffix && (next->file.mode & KERNEL_VFS_S_IFMT) !=
                          KERNEL_VFS_S_IFDIR &&
            !(trailing_only && follow_trailing_final == 2)) {
            result = -KERNEL_ENOTDIR;
            (void)kernel_vfs_path_release(&next);
            goto Finish;
        }
        (void)kernel_vfs_path_release(&current);
        current = next;
    }
    *owner = current;
    current = 0;
Finish:
    if (current != 0) (void)kernel_vfs_path_release(&current);
    (void)kernel_heap_release(adapter->heap, work);
    return result;
}

int kernel_vfs_path_resolve(struct kernel_vfs_path *start,
                            struct kernel_vfs_path *root,
                            const char *path, int follow_final,
                            struct kernel_vfs_path **owner)
{
    return resolve_object(start, root, path, follow_final, 0, 0, 1,
                          owner, 0);
}

int kernel_vfs_path_root(struct kernel_vfs_mount *mount,
                         struct kernel_heap *heap,
                         struct kernel_vfs_path **owner)
{
    struct kernel_vfs_path *path;
    enum kernel_heap_status allocation;
    uint32_t mode;
    int result;

    if (mount == 0 || mount->private_data == 0 ||
        heap == 0 || owner == 0 || *owner != 0)
        return -KERNEL_EINVAL;
    struct lwext4_mount_adapter *adapter = mount->private_data;
    for (path = adapter->paths; path != 0; path = path->next) {
        if (path->parent == 0) {
            result = kernel_vfs_path_acquire(path);
            if (result == 0) *owner = path;
            return result;
        }
    }
    allocation = kernel_heap_allocate_zeroed(heap, 1U, sizeof(*path) + 2U,
                                              (void **)&path);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                     : -KERNEL_EIO;
    result = ext4_mode_get("/", &mode);
    if (result == EOK)
        result = vfs_open_inode_raw(mount, EXT4_INODE_ROOT_INDEX,
                                    mode, &path->file);
    if (result != 0) {
        if (kernel_heap_release(heap, path) != KERNEL_HEAP_STATUS_OK)
            __builtin_trap();
        return result > 0 ? lwext4_error(result) : result;
    }
    path->heap = heap;
    path->references = 1U;
    path->name = path->initial_name;
    path->name[0] = '/';
    register_path(path);
    *owner = path;
    return 0;
}

int kernel_vfs_path_acquire(struct kernel_vfs_path *path)
{
    if (path == 0 || path->references == 0U ||
        path->references == UINT32_MAX) return -KERNEL_EOVERFLOW;
    path->references++;
    return 0;
}

int kernel_vfs_path_release(struct kernel_vfs_path **owner)
{
    struct kernel_vfs_path *path;
    if (owner == 0 || *owner == 0) return -KERNEL_EINVAL;
    path = *owner;
    if (path->references == 0U) __builtin_trap();
    if (path->references > 1U) {
        path->references--;
        *owner = 0;
        return 0;
    }
    while (path != 0) {
        struct kernel_vfs_path *parent;
        if (path->references == 0U) __builtin_trap();
        if (path->references > 1U) {
            path->references--;
            *owner = 0;
            return 0;
        }
        /* Real backend close failures transfer to the mount cleanup list in
         * kernel_vfs_node_release.  Any remaining close error is an ownership
         * invariant violation, not a retryable path-release result. */
        unregister_path(path);
        if (kernel_vfs_close(&path->file) != 0) __builtin_trap();
        parent = path->parent;
        path->references = 0U;
        if (path->name != path->initial_name)
            (void)kernel_heap_release(path->heap, path->name);
        if (kernel_heap_release(path->heap, path) !=
            KERNEL_HEAP_STATUS_OK) __builtin_trap();
        path = parent;
        *owner = path;
    }
    *owner = 0;
    return 0;
}

struct kernel_vfs_mount *kernel_vfs_path_mount(
    const struct kernel_vfs_path *path)
{
    return path != 0 && path->references != 0U ? path->file.mount : 0;
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
        block > UINT64_MAX / LWEXT4_PHYSICAL_BLOCK_SIZE) {
        return EINVAL;
    }
    offset = block * LWEXT4_PHYSICAL_BLOCK_SIZE;
    byte_count = (uint64_t)count * LWEXT4_PHYSICAL_BLOCK_SIZE;
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
        block > UINT64_MAX / LWEXT4_PHYSICAL_BLOCK_SIZE) {
        return EINVAL;
    }
    if (adapter->block->write == 0) {
        return EROFS;
    }
    offset = block * LWEXT4_PHYSICAL_BLOCK_SIZE;
    byte_count = (uint64_t)count * LWEXT4_PHYSICAL_BLOCK_SIZE;
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

static int release_mount_storage(struct kernel_vfs_mount *mount)
{
    struct lwext4_mount_adapter *adapter = mount->private_data;
    struct kernel_heap *heap = adapter->heap;

    if (adapter->physical_buffer != 0) {
        (void)kernel_heap_release(heap, adapter->physical_buffer);
        adapter->physical_buffer = 0;
    }
    if (adapter->heap_bound) {
        boaros_lwext4_heap_unbind(heap);
        adapter->heap_bound = 0U;
    }

    (void)kernel_heap_release(heap, adapter);
    mount->private_data = 0;
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
    (void)kernel_heap_release(adapter->heap, orphan);
}

static int reserve_orphan(struct lwext4_mount_adapter *adapter,
                          struct lwext4_orphan **owner)
{
    enum kernel_heap_status status;

    status = kernel_heap_allocate_zeroed(adapter->heap,
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

static int cleanup_mount(struct kernel_vfs_mount *mount)
{
    struct lwext4_mount_adapter *adapter = mount->private_data;
    struct lwext4_orphan **orphan_link;
    struct kernel_vfs_node **cleanup_link;
    int result;

    /* An uncertain journal/recovery failure still owns its cache and log.
     * Never mark that filesystem clean or recycle its adapter on a retry. */
    result = mount_error(adapter);
    if (result != EOK) return lwext4_error(result);
    if (adapter->recovery_pending) {
        result = prepare_journal_mount(adapter);
        if (result != EOK) return lwext4_error(result);
    }

    if (adapter->external_files != 0U) {
        return -KERNEL_EBUSY;
    }
    mount->state = VFS_MOUNT_STATE_CLEANUP;
    if (adapter->page_cache != 0) {
        enum kernel_page_cache_status cache_status =
            kernel_page_cache_purge_mount(adapter->page_cache, mount);

        if (cache_status != KERNEL_PAGE_CACHE_STATUS_OK) {
            return cache_status == KERNEL_PAGE_CACHE_STATUS_STATE
                       ? -KERNEL_EBUSY : -KERNEL_EIO;
        }
    }
    orphan_link = &adapter->orphans;
    while (*orphan_link != 0) {
        struct lwext4_orphan *orphan = *orphan_link;
        struct lwext4_orphan *next = orphan->next;

        if (!orphan->orphan_freed) {
            result = ext4_orphan_free(LWEXT4_MOUNT_POINT, orphan->inode);
            if (result != EOK) {
                return lwext4_error(result);
            }
            orphan->orphan_freed = 1U;
        }
        (void)kernel_heap_release(adapter->heap, orphan);
        *orphan_link = next;
    }
    cleanup_link = &adapter->cleanup_nodes;
    while (*cleanup_link != 0) {
        struct kernel_vfs_node *node = *cleanup_link;
        struct kernel_vfs_node *next = node->next;

        if (node->unlinked && !node->orphan_freed) {
            result = ext4_orphan_free(LWEXT4_MOUNT_POINT, node->file.inode);
            if (result == EOK) {
                node->orphan_freed = 1U;
            } else {
                return lwext4_error(result);
            }
        }
        if (!node->closed) {
            result = ext4_fclose(&node->file);
            if (result != EOK) {
                return lwext4_error(result);
            }
            node->closed = 1U;
        }
        (void)kernel_heap_release(adapter->heap, node);
        *cleanup_link = next;
    }
    if (adapter->orphans != 0 || adapter->cleanup_nodes != 0 ||
        adapter->nodes != 0) {
        return -KERNEL_EIO;
    }
    if (adapter->mounted) {
        result = ext4_umount(LWEXT4_MOUNT_POINT);
        if (result != EOK) {
            return lwext4_error(result);
        }
        adapter->mounted = 0U;
        adapter->unmount_sync_pending = !adapter->read_only;
    }
    if (adapter->unmount_sync_pending) {
        if (kernel_block_flush(adapter->block) != KERNEL_BLOCK_STATUS_OK)
            return -KERNEL_EIO;
        adapter->unmount_sync_pending = 0;
    }
    if (adapter->registered) {
        result = ext4_device_unregister(LWEXT4_DEVICE_NAME);
        if (result != EOK) {
            return lwext4_error(result);
        }
        adapter->registered = 0U;
    }

    return release_mount_storage(mount);
}

int kernel_vfs_mount_root(struct kernel_vfs_mount *mount,
                          struct kernel_block_device *block,
                          struct kernel_heap *heap,
                          struct kernel_page_cache *page_cache)
{
    struct lwext4_mount_adapter *adapter;
    struct ext4_sblock *superblock;
    enum kernel_heap_status heap_status;
    int result;

    if (mount == 0 || mount->state != VFS_MOUNT_STATE_EMPTY ||
        mount->private_data != 0 || mount->id != 0U ||
        block == 0 || block->read == 0 ||
        block->logical_block_size != LWEXT4_PHYSICAL_BLOCK_SIZE ||
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
    adapter->heap = heap;
    adapter->block = block;
    adapter->page_cache = page_cache;
    adapter->heap_bound = 1U;
    mount->private_data = adapter;
    mount->state = VFS_MOUNT_STATE_CLEANUP;

    heap_status = kernel_heap_allocate(heap,
                                       LWEXT4_PHYSICAL_BLOCK_SIZE,
                                       (void **)&adapter->physical_buffer);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        (void)cleanup_mount(mount);
        return heap_status == KERNEL_HEAP_STATUS_EMPTY ?
                   -KERNEL_ENOMEM : -KERNEL_EIO;
    }

    adapter->interface.open = block_open;
    adapter->interface.bread = block_read;
    adapter->interface.bwrite = block_write;
    adapter->interface.close = block_close;
    adapter->interface.flush = block_flush;
    adapter->interface.lock = 0;
    adapter->interface.unlock = 0;
    adapter->interface.ph_bsize = LWEXT4_PHYSICAL_BLOCK_SIZE;
    adapter->interface.ph_bcnt =
        block->capacity_bytes / LWEXT4_PHYSICAL_BLOCK_SIZE;
    adapter->interface.ph_bbuf = adapter->physical_buffer;
    adapter->interface.p_user = adapter;
    adapter->device.bdif = &adapter->interface;
    adapter->device.part_offset = 0U;
    adapter->device.part_size =
        adapter->interface.ph_bcnt * LWEXT4_PHYSICAL_BLOCK_SIZE;

    result = ext4_device_register(&adapter->device, LWEXT4_DEVICE_NAME);
    if (result != EOK) {
        (void)cleanup_mount(mount);
        return lwext4_error(result);
    }
    adapter->registered = 1U;
    int read_only = (block->write == 0);
    adapter->read_only = (uint8_t)(read_only != 0 ? 1U : 0U);
    result = ext4_mount(LWEXT4_DEVICE_NAME, LWEXT4_MOUNT_POINT, read_only != 0);
    if (result != EOK) {
        (void)cleanup_mount(mount);
        return lwext4_error(result);
    }
    adapter->mounted = 1U;
    result = ext4_mount_setup_clock(LWEXT4_MOUNT_POINT, vfs_realtime);
    if (result != EOK) {
        (void)cleanup_mount(mount);
        return lwext4_error(result);
    }

    result = ext4_get_sblock(LWEXT4_MOUNT_POINT, &superblock);
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
    mount->id = VFS_ROOT_MOUNT_ID;
    mount->state = VFS_MOUNT_STATE_LIVE;
    return 0;
}

int kernel_vfs_unmount(struct kernel_vfs_mount *mount)
{
    if (mount == 0 || mount->private_data == 0 ||
        (mount->state != VFS_MOUNT_STATE_LIVE &&
         mount->state != VFS_MOUNT_STATE_CLEANUP)) {
        return -KERNEL_EINVAL;
    }

    return cleanup_mount(mount);
}

int kernel_vfs_mount_is_readonly(const struct kernel_vfs_mount *mount)
{
    if (mount == 0 || mount->private_data == 0) {
        return 0;
    }
    const struct lwext4_mount_adapter *adapter = mount->private_data;
    return adapter->read_only != 0;
}

static int vfs_open_raw(struct kernel_vfs_mount *mount,
                        const char *path, uint32_t inode_number,
                        uint32_t inode_mode,
                        struct kernel_vfs_file *file)
{
    struct lwext4_mount_adapter *adapter;
    struct kernel_vfs_node *node;
    struct kernel_vfs_node *existing;
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
    adapter = mount->private_data;
    if (path != 0) {
        result = ext4_mode_get(path, &mode);
        if (result != EOK) return lwext4_error(result);
    } else {
        mode = inode_mode;
    }
    heap_status = kernel_heap_allocate_zeroed(adapter->heap,
                                              1U,
                                              sizeof(*node),
                                              (void **)&node);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY ?
                   -KERNEL_ENOMEM : -KERNEL_EIO;
    }

    if (path == 0) {
        result = ext4_fopen_inode(&node->file, LWEXT4_MOUNT_POINT,
                                  inode_number);
        if (result != EOK) {
            (void)kernel_heap_release(adapter->heap, node);
            return lwext4_error(result);
        }
    } else if ((mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR) {
        /* ext4_fopen refuses directories; ext4_dir_open_file accepts them and
         * leaves the same file handle shape without allocating an ext4_dir on stack. */
        result = ext4_dir_open_file(&node->file, path);
        if (result != EOK) {
            (void)kernel_heap_release(adapter->heap, node);
            return lwext4_error(result);
        }
    } else if ((mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFCHR) {
        result = ext4_chrdev_open_file(&node->file, path);
        if (result != EOK) {
            (void)kernel_heap_release(adapter->heap, node);
            return lwext4_error(result);
        }
    } else {
        if (!adapter->read_only) {
            result = ext4_fopen(&node->file, path, "r+");
            if (result != EOK) {
                result = ext4_fopen(&node->file, path, "r");
            }
        } else {
            result = ext4_fopen(&node->file, path, "r");
        }
        if (result != EOK) {
            (void)kernel_heap_release(adapter->heap, node);
            return lwext4_error(result);
        }
    }
    for (existing = adapter->nodes;
         existing != 0;
         existing = existing->next) {
        if (existing->file.inode == node->file.inode && !existing->orphan_freed) {
            break;
        }
    }
    if (existing != 0) {
        result = ext4_fclose(&node->file);
        if (result != EOK) {
            node->adapter = adapter;
            node->next = adapter->cleanup_nodes;
            adapter->cleanup_nodes = node;
            return lwext4_error(result);
        }
        node->closed = 1U;
        (void)kernel_heap_release(adapter->heap, node);
        if (existing->references == UINT32_MAX ||
            existing->open_files == UINT32_MAX) {
            return -KERNEL_EOVERFLOW;
        }
        existing->references++;
        existing->open_files++;
        node = existing;
    } else {
        node->adapter = adapter;
        node->mount = mount;
        node->size = ext4_fsize(&node->file);
        node->mode = mode;
        node->references = 1U;
        node->open_files = 1U;
        node->write_openers = 0U;
        node->exec_users = 0U;
        node->unlinked = 0U;
        node->orphan_freed = 0U;
        node->next = adapter->nodes;
        adapter->nodes = node;
    }
    file->private_data = node;
    file->mount = mount;
    file->size = node->size;
    file->mode = node->mode;
    file->state = VFS_FILE_STATE_LIVE;
    file->write_lease = 0U;
    file->exec_lease = 0U;
    adapter->external_files++;
    return 0;
}

static int vfs_open_inode_raw(struct kernel_vfs_mount *mount,
                              uint32_t inode, uint32_t mode,
                              struct kernel_vfs_file *file)
{
    return vfs_open_raw(mount, 0, inode, mode, file);
}

int kernel_vfs_path_string(const struct kernel_vfs_path *path,
                           const struct kernel_vfs_path *root,
                           char *buffer, size_t capacity)
{
    if (!path || !root || path->file.mount != root->file.mount)
        return -KERNEL_EINVAL;
    /* This stage has one mount and no chroot; refuse a foreign root. */
    if (root->parent) return -KERNEL_EINVAL;
    if (capacity < 2U) return -KERNEL_ERANGE;
    int result = path_to_backend_name(path, 0, buffer, capacity);
    return result == -KERNEL_ENAMETOOLONG ? -KERNEL_ERANGE : result;
}

int kernel_vfs_path_open(struct kernel_vfs_path *path,
                         struct kernel_vfs_file *file)
{
    if (!path || !file || file->path) return -KERNEL_EINVAL;
    int result = kernel_vfs_path_acquire(path);
    if (result) return result;
    result = vfs_open_inode_raw(path->file.mount,
              (uint32_t)kernel_vfs_path_inode(path), path->file.mode, file);
    if (result) (void)kernel_vfs_path_release(&path);
    else file->path = path;
    return result;
}

int kernel_vfs_open_at(struct kernel_vfs_path *start,
                       struct kernel_vfs_path *root, const char *path,
                       int follow_final, struct kernel_vfs_file *file)
{
    struct kernel_vfs_path *resolved = 0;
    int result = kernel_vfs_path_resolve(start, root, path, follow_final,
                                        &resolved);
    if (!result) {
        result = (resolved->file.mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFLNK
                     ? -KERNEL_ELOOP : kernel_vfs_path_open(resolved, file);
        (void)kernel_vfs_path_release(&resolved);
    }
    return result;
}

static int mount_root_path(struct kernel_vfs_mount *mount,
                            struct kernel_vfs_path **root)
{
    if (!mount || !mount->private_data) return -KERNEL_EINVAL;
    return kernel_vfs_path_root(mount,
               ((struct lwext4_mount_adapter *)mount->private_data)->heap, root);
}

static int vfs_open_resolved(struct kernel_vfs_mount *mount,
                             const char *path,
                             struct kernel_vfs_file *file,
                             int follow_final)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    result = kernel_vfs_open_at(root, root, path, follow_final, file);
    (void)kernel_vfs_path_release(&root);
    return result;
}

int kernel_vfs_open(struct kernel_vfs_mount *mount,
                    const char *path,
                    struct kernel_vfs_file *file)
{
    return vfs_open_resolved(mount, path, file, 1);
}

int kernel_vfs_open_nofollow(struct kernel_vfs_mount *mount,
                             const char *path,
                             struct kernel_vfs_file *file)
{
    return vfs_open_resolved(mount, path, file, 0);
}

static int vfs_create_raw(struct kernel_vfs_mount *mount,
                          const char *path,
                          uint32_t mode,
                          struct kernel_vfs_file *file)
{
    struct lwext4_mount_adapter *adapter;
    struct kernel_vfs_node *node;
    struct kernel_vfs_node *existing;
    enum kernel_heap_status heap_status;
    int result;

    if (mount == 0 || mount->state != VFS_MOUNT_STATE_LIVE ||
        mount->private_data == 0 || path == 0 || path[0] != '/' ||
        file == 0 || file->state != VFS_FILE_STATE_EMPTY ||
        file->private_data != 0) {
        return -KERNEL_EINVAL;
    }
    adapter = mount->private_data;
    if (adapter->read_only) {
        return -KERNEL_EROFS;
    }
    heap_status = kernel_heap_allocate_zeroed(adapter->heap,
                                              1U,
                                              sizeof(*node),
                                              (void **)&node);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY ?
                   -KERNEL_ENOMEM : -KERNEL_EIO;
    }

    result = ext4_transaction_begin(LWEXT4_MOUNT_POINT);
    if (result == EOK) {
        result = ext4_fopen2(&node->file, path, O_CREAT | O_RDWR);
        if (result == EOK)
            result = ext4_mode_set(path, (mode & 07777U) | KERNEL_VFS_S_IFREG);
        if (result == EOK) result = ext4_transaction_end(LWEXT4_MOUNT_POINT);
        else (void)ext4_transaction_abort(LWEXT4_MOUNT_POINT, result);
    }
    if (result != EOK) {
        /* This unopened owner has no data or mapping; the mount owns any
         * uncertain journal state. An aborted create must not unlink by name. */
        if (node->file.mp && ext4_fclose(&node->file) != EOK) {
            node->adapter = adapter;
            node->next = adapter->cleanup_nodes;
            adapter->cleanup_nodes = node;
        } else {
            (void)kernel_heap_release(adapter->heap, node);
        }
        return lwext4_error(result);
    }

    for (existing = adapter->nodes;
         existing != 0;
         existing = existing->next) {
        if (existing->file.inode == node->file.inode && !existing->orphan_freed) {
            break;
        }
    }
    if (existing != 0) {
        if (existing->exec_users > 0U) {
            result = ext4_fclose(&node->file);
            if (result != EOK) {
                node->adapter = adapter;
                node->next = adapter->cleanup_nodes;
                adapter->cleanup_nodes = node;
            } else {
                (void)kernel_heap_release(adapter->heap, node);
            }
            return -KERNEL_ETXTBSY;
        }
        result = ext4_fclose(&node->file);
        if (result != EOK) {
            node->adapter = adapter;
            node->next = adapter->cleanup_nodes;
            adapter->cleanup_nodes = node;
            return lwext4_error(result);
        }
        node->closed = 1U;
        (void)kernel_heap_release(adapter->heap, node);
        if (existing->references == UINT32_MAX ||
            existing->open_files == UINT32_MAX) {
            return -KERNEL_EOVERFLOW;
        }
        existing->references++;
        existing->open_files++;
        node = existing;
    } else {
        node->adapter = adapter;
        node->mount = mount;
        node->size = ext4_fsize(&node->file);
        node->mode = (mode & 07777U) | KERNEL_VFS_S_IFREG;
        node->references = 1U;
        node->open_files = 1U;
        node->write_openers = 0U;
        node->exec_users = 0U;
        node->unlinked = 0U;
        node->orphan_freed = 0U;
        node->next = adapter->nodes;
        adapter->nodes = node;
    }
    file->private_data = node;
    file->mount = mount;
    file->size = node->size;
    file->mode = node->mode;
    file->state = VFS_FILE_STATE_LIVE;
    file->write_lease = 0U;
    file->exec_lease = 0U;
    adapter->external_files++;
    return 0;
}

/* Keep the resolved identity alive until namespace mutation has committed. */
static int mutation_path(struct kernel_vfs_path *start,
                          struct kernel_vfs_path *root, const char *input,
                          int follow, int missing, int trailing,
                          int follow_trailing, struct kernel_vfs_path **path,
                          char missing_name[256], char **backend)
{
    if (!start || !root || !input) return -KERNEL_EINVAL;
    struct kernel_heap *heap = start->heap;
    int result = resolve_object(start, root, input, follow, missing, trailing,
                                follow_trailing, path, missing_name);
    if (result) return result;
    /* PATH_MAX constrains the supplied path, not a dirfd's ancestry. The
     * pathname-only backend bridge needs the full current ancestor chain. */
    size_t capacity = 2U;
    for (const struct kernel_vfs_path *cursor = *path; cursor->parent;
         cursor = cursor->parent) {
        if (cursor->detached ||
            ((struct kernel_vfs_node *)cursor->file.private_data)->unlinked) {
            result = -KERNEL_ENOENT;
            goto Finish;
        }
        size_t length = strlen(cursor->name) + 1U;
        if (length > SIZE_MAX - capacity) { result = -KERNEL_ENOMEM; goto Finish; }
        capacity += length;
    }
    size_t length = strlen(missing_name) + 1U;
    if (length > SIZE_MAX - capacity) { result = -KERNEL_ENOMEM; goto Finish; }
    capacity += length;
    enum kernel_heap_status status = kernel_heap_allocate(heap, capacity,
                                                           (void **)backend);
    if (status != KERNEL_HEAP_STATUS_OK) {
        result = status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        goto Finish;
    }
    result = path_to_backend_name(*path, missing_name, *backend, capacity);
Finish:
    if (result) {
        (void)kernel_vfs_path_release(path);
        if (*backend) release_path(heap, *backend);
        *backend = 0;
    }
    return result;
}

int kernel_vfs_create_at(struct kernel_vfs_path *start,
                         struct kernel_vfs_path *root, const char *input,
                         uint32_t mode, struct kernel_vfs_file *file)
{
    struct kernel_vfs_path *parent = 0, *child = 0;
    char name[256] = {0};
    char *backend = 0;
    if (!start || !file || file->state || file->private_data || file->path)
        return -KERNEL_EINVAL;
    int result = mutation_path(start, root, input, 1, 1, 0, 1,
                                &parent, name, &backend);
    if (result) {
        size_t length = input ? strlen(input) : 0U;
        return result == -KERNEL_ENOENT && length && input[length - 1U] == '/'
                   ? -KERNEL_EISDIR : result;
    }
    if (!name[0]) { result = -KERNEL_EEXIST; goto Finish; }
    enum kernel_heap_status allocation = kernel_heap_allocate_zeroed(
        start->heap, 1U, sizeof(*child) + strlen(name) + 1U, (void **)&child);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        result = allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                        : -KERNEL_EIO;
        goto Finish;
    }
    /* Reserve the path and its parent reference before creating disk state. */
    child->heap = start->heap;
    child->parent = parent;
    parent = 0;
    child->references = 1U;
    child->name = child->initial_name;
    memcpy(child->name, name, strlen(name) + 1U);
    result = vfs_create_raw(start->file.mount, backend, mode, file);
    if (!result) {
        struct kernel_vfs_node *node = file->private_data;
        struct lwext4_mount_adapter *adapter = file->mount->private_data;
        child->file = *file;
        node->references++;
        node->open_files++;
        adapter->external_files++;
        file->path = child;
        register_path(child);
        child = 0;
    }
Finish:
    if (child) {
        (void)kernel_vfs_path_release(&child->parent);
        release_path(child->heap, (char *)child);
    }
    if (parent) (void)kernel_vfs_path_release(&parent);
    release_path(start->heap, backend);
    return result;
}

int kernel_vfs_create(struct kernel_vfs_mount *mount, const char *path,
                      uint32_t mode, struct kernel_vfs_file *file)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    result = kernel_vfs_create_at(root, root, path, mode, file);
    (void)kernel_vfs_path_release(&root);
    return result;
}

static int finish_executable_open(struct kernel_vfs_file *file, int result)
{
    if (result == -KERNEL_EISDIR) {
        return -KERNEL_EACCES;
    }
    if (result != 0) {
        return result;
    }
    if ((file->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG ||
        (file->mode & (KERNEL_VFS_S_IXUSR |
                       KERNEL_VFS_S_IXGRP |
                       KERNEL_VFS_S_IXOTH)) == 0U) {
        return kernel_vfs_close(file) == 0 ? -KERNEL_EACCES
                                           : -KERNEL_EIO;
    }
    struct kernel_vfs_node *node = file->private_data;
    if (node->write_openers > 0U) {
        (void)kernel_vfs_close(file);
        return -KERNEL_ETXTBSY;
    }
    if (node->exec_users == UINT32_MAX) {
        (void)kernel_vfs_close(file);
        return -KERNEL_EOVERFLOW;
    }
    node->exec_users++;
    file->exec_lease = 1U;
    return 0;
}

int kernel_vfs_open_executable(struct kernel_vfs_mount *mount,
                               const char *path, struct kernel_vfs_file *file)
{
    return finish_executable_open(file, kernel_vfs_open(mount, path, file));
}

int kernel_vfs_open_executable_at(struct kernel_vfs_path *start,
                                  struct kernel_vfs_path *root,
                                  const char *path, struct kernel_vfs_file *file)
{
    return finish_executable_open(file, kernel_vfs_open_at(start, root, path,
                                                           1, file));
}

int kernel_vfs_file_acquire_write(struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node;

    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0) {
        return -KERNEL_EINVAL;
    }
    node = file->private_data;
    if (node->adapter != 0 && node->adapter->read_only) {
        return -KERNEL_EROFS;
    }
    if (node->exec_users > 0U) {
        return -KERNEL_ETXTBSY;
    }
    if (file->write_lease == 0U) {
        if (node->write_openers == UINT32_MAX) {
            return -KERNEL_EOVERFLOW;
        }
        node->write_openers++;
        file->write_lease = 1U;
    }
    return 0;
}

void kernel_vfs_file_accessed(struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node;
    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0) return;
    node = file->private_data;
    /* Linux touch_atime does not change a read's result on metadata I/O
     * failure. The mount's dirty block cache continues to own that state. */
    (void)ext4_file_touch(&node->file, EXT4_TIME_ATIME | EXT4_TIME_RELATIME);
}

int kernel_vfs_file_modified(struct kernel_vfs_file *file,
                              uint64_t offset, int append)
{
    struct kernel_vfs_node *node;
    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0) return -KERNEL_EINVAL;
    node = file->private_data;
    if (node->adapter->read_only) return -KERNEL_EROFS;
    if (append) offset = node->size;
    /* Linux generic/ext4 write checks reject the maximum position before
     * file_modified, even when the user buffer would fault. */
    if (offset >= node->file.fmax) return -KERNEL_EFBIG;
    return lwext4_error(ext4_file_touch(&node->file,
                                       EXT4_TIME_MTIME | EXT4_TIME_CTIME));
}

int kernel_vfs_pread(struct kernel_vfs_file *file,
                     uint64_t offset,
                     void *buffer,
                     size_t size,
                     size_t *bytes_read)
{
    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0 || bytes_read == 0 ||
        (buffer == 0 && size != 0U)) {
        return -KERNEL_EINVAL;
    }
    struct kernel_vfs_node *node = file->private_data;
    struct kernel_page_cache *cache = node->adapter->page_cache;
    *bytes_read = 0;
    if (offset > INT64_MAX) return -KERNEL_EOVERFLOW;
    while (*bytes_read < size && offset < node->size) {
        uint64_t address;
        size_t valid, start = (size_t)(offset & (BOAROS_PAGE_SIZE - 1U));
        void *page;
        enum kernel_page_cache_status status = kernel_page_cache_get(
              cache, file, offset >> BOAROS_PAGE_SHIFT, &address, &valid);
        if (status != KERNEL_PAGE_CACHE_STATUS_OK)
            return status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                       ? -KERNEL_ENOMEM : -KERNEL_EIO;
        if (physical_page_resolve(cache->allocator, address, &page) !=
                PHYSICAL_PAGE_STATUS_OK || valid <= start) __builtin_trap();
        size_t count = valid - start;
        if (count > size - *bytes_read) count = size - *bytes_read;
        memcpy((unsigned char *)buffer + *bytes_read,
                (unsigned char *)page + start, count);
        (void)physical_page_release(cache->allocator, address);
        *bytes_read += count;
        offset += count;
    }
    return 0;
}

static void apply_truncated_size(struct kernel_vfs_node *node,
                                 struct kernel_vfs_file *file, uint64_t size)
{
    uint64_t old_size = node->size;
    node->size = size;
    file->size = node->size;
    if (node->size < old_size) {
        for (struct kernel_file_mapping *mapping = node->mappings;
             mapping != 0; mapping = mapping->next) {
            mapping->truncate(mapping->owner, node, node->size);
        }
    }
    if (node->adapter->page_cache != 0) {
        kernel_page_cache_truncate(node->adapter->page_cache, node, node->size);
    }
}

int kernel_vfs_pwrite(struct kernel_vfs_file *file,
                      uint64_t offset,
                      const void *buffer,
                      size_t size,
                      size_t *bytes_written)
{
    struct kernel_vfs_node *node;
    size_t written = 0U;
    int result;

    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0 || (buffer == 0 && size != 0U) ||
        bytes_written == 0) {
        return -KERNEL_EINVAL;
    }
    node = file->private_data;
    if ((node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG) {
        return -KERNEL_EINVAL;
    }
    if (node->adapter == 0 || node->adapter->read_only) {
        return -KERNEL_EROFS;
    }
    result = mount_error(node->adapter);
    if (result != EOK) return lwext4_error(result);
    if (size == 0U) {
        *bytes_written = 0U;
        return 0;
    }

    if (offset >= node->file.fmax) return -KERNEL_EFBIG;
    if (size > node->file.fmax - offset)
        size = (size_t)(node->file.fmax - offset);
    result = kernel_page_cache_write(node->adapter->page_cache, file, offset,
                                      buffer, size, &written);
    file->size = node->size;
    *bytes_written = written;
    if (written > size) {
        return -KERNEL_EIO;
    }
    if (result != 0 && written == 0U) {
        return result;
    }
    return 0;
}

int kernel_vfs_append(struct kernel_vfs_file *file,
                      const void *buffer,
                      size_t size,
                      uint64_t *written_offset,
                      size_t *bytes_written)
{
    struct kernel_vfs_node *node;
    uint64_t offset;
    size_t written = 0U;
    int result;

    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0 || (buffer == 0 && size != 0U) ||
        bytes_written == 0) {
        return -KERNEL_EINVAL;
    }
    node = file->private_data;
    if ((node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG) {
        return -KERNEL_EINVAL;
    }
    if (node->adapter == 0 || node->adapter->read_only) {
        return -KERNEL_EROFS;
    }
    offset = node->size;
    if (size == 0U) {
        if (written_offset != 0) {
            *written_offset = offset;
        }
        *bytes_written = 0U;
        return 0;
    }
    result = kernel_vfs_pwrite(file, offset, buffer, size, &written);
    if (written_offset != 0) {
        *written_offset = offset + (uint64_t)written;
    }
    *bytes_written = written;
    if (written > size) {
        return -KERNEL_EIO;
    }
    if (result != 0 && written == 0U) {
        return result;
    }
    return 0;
}

int kernel_vfs_ftruncate(struct kernel_vfs_file *file,
                         uint64_t size)
{
    struct kernel_vfs_node *node;
    int result;

    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0) {
        return -KERNEL_EINVAL;
    }
    node = file->private_data;
    if ((node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG) {
        return -KERNEL_EINVAL;
    }
    if (node->adapter == 0 || node->adapter->read_only) {
        return -KERNEL_EROFS;
    }
    if (node->exec_users > 0U) {
        return -KERNEL_ETXTBSY;
    }

    /* Even a same-size ftruncate updates mtime/ctime. Reconciliation also
     * preserves visible inode mutations when the backend reports an error. */
    if (size > node->file.fmax) return -KERNEL_EFBIG;
    result = kernel_page_cache_writeback_before(node->adapter->page_cache, node, size);
    if (result != 0) return result;
    uint64_t prior_disk_size = ext4_fsize(&node->file);
    result = ext4_ftruncate(&node->file, size);
    if (result == EOK || ext4_fsize(&node->file) != prior_disk_size)
        apply_truncated_size(node, file, ext4_fsize(&node->file));
    if (result != EOK) {
        return lwext4_error(result);
    }
    return 0;
}

static int vfs_source_read_at(void *context,
                              uint64_t offset,
                              void *buffer,
                              size_t size)
{
    struct kernel_vfs_file *file = context;
    size_t bytes_read = 0U;
    int result = kernel_vfs_pread(file,
                                  offset,
                                  buffer,
                                  size,
                                  &bytes_read);

    if (result != 0) {
        return result;
    }
    return bytes_read == size ? 0 : -KERNEL_EIO;
}

int kernel_vfs_file_read_source(struct kernel_vfs_file *file,
                                struct kernel_read_source *source)
{
    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0 || source == 0) {
        return -KERNEL_EINVAL;
    }
    source->context = file;
    source->size = file->size;
    source->read_at = vfs_source_read_at;
    return 0;
}

static void kernel_vfs_try_release_orphan(struct kernel_vfs_node *node)
{
    struct kernel_vfs_node *temp;
    int result;

    if (node == 0 || !node->unlinked || node->orphan_freed) {
        return;
    }
    if (node->open_files > 0U || node->exec_users > 0U) {
        return;
    }

    node->references++;

    if (node->adapter != 0 && node->adapter->page_cache != 0) {
        (void)kernel_page_cache_invalidate_node(node->adapter->page_cache,
                                                node);
    }

    result = ext4_orphan_free(LWEXT4_MOUNT_POINT, node->file.inode);
    if (result == EOK) {
        node->orphan_freed = 1U;
    }

    temp = node;
    (void)kernel_vfs_node_release(&temp);

}

int kernel_vfs_close(struct kernel_vfs_file *file)
{
    struct lwext4_mount_adapter *adapter;
    struct kernel_vfs_node *node;

    if (file == 0 || file->private_data == 0 || file->mount == 0 ||
        (file->state != VFS_FILE_STATE_LIVE &&
         file->state != VFS_FILE_STATE_CLEANUP)) {
        return -KERNEL_EINVAL;
    }
    node = file->private_data;
    if (file->path && kernel_vfs_path_release(&file->path) != 0)
        __builtin_trap();
    if (file->write_lease != 0U) {
        if (node->write_openers > 0U) {
            node->write_openers--;
        }
        file->write_lease = 0U;
    }
    if (file->exec_lease != 0U) {
        if (node->exec_users > 0U) {
            node->exec_users--;
        }
        file->exec_lease = 0U;
    }
    if (node->open_files > 0U) {
        node->open_files--;
    }
    adapter = file->mount->private_data;
    if (adapter == 0) {
        return -KERNEL_EIO;
    }

    if (node->unlinked) {
        (void)kernel_vfs_try_release_orphan(node);
    }

    file->state = VFS_FILE_STATE_CLEANUP;
    if (kernel_vfs_node_release(&node) != 0) {
        return -KERNEL_EIO;
    }
    if (adapter->external_files == 0U) {
        return -KERNEL_EIO;
    }
    adapter->external_files--;
    file->private_data = 0;
    file->mount = 0;
    file->size = 0U;
    file->mode = 0U;
    file->state = VFS_FILE_STATE_EMPTY;
    return 0;
}

uint32_t kernel_vfs_file_inode(const struct kernel_vfs_file *file)
{
    const struct kernel_vfs_node *node;

    if (file == 0 || file->private_data == 0 ||
        file->state != VFS_FILE_STATE_LIVE) {
        return 0U;
    }
    node = file->private_data;
    return node->file.inode;
}

uint64_t kernel_vfs_file_size(const struct kernel_vfs_file *file)
{
    const struct kernel_vfs_node *node;

    if (file == 0 || file->private_data == 0 ||
        file->state != VFS_FILE_STATE_LIVE) {
        return 0U;
    }
    node = file->private_data;
    return node->size;
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
    stat->dev = mount->id;
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

int kernel_vfs_fstat(const struct kernel_vfs_file *file,
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
    if (node->adapter == 0 || node->adapter->superblock == 0)
        return -KERNEL_EIO;
    result = ext4_fraw_inode_fill(&node->file, &inode);
    if (result != EOK) return lwext4_error(result);
    fill_stat(file->mount, node->adapter->superblock,
              node->file.inode, &inode, stat);
    if ((node->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFREG)
        stat->size = node->size;
    return 0;
}

int kernel_vfs_stat_at(struct kernel_vfs_path *start,
                       struct kernel_vfs_path *root, const char *path,
                       int follow_final, struct kernel_vfs_stat *stat)
{
    struct kernel_vfs_path *resolved = 0;
    int result = kernel_vfs_path_resolve(start, root, path, follow_final,
                                         &resolved);
    if (!result) {
        result = kernel_vfs_path_stat(resolved, stat);
        (void)kernel_vfs_path_release(&resolved);
    }
    return result;
}

int kernel_vfs_stat_path(struct kernel_vfs_mount *mount, const char *path,
                         int follow_final, struct kernel_vfs_stat *stat)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    result = kernel_vfs_stat_at(root, root, path, follow_final, stat);
    (void)kernel_vfs_path_release(&root);
    return result;
}

static int vfs_mkdir_raw(struct kernel_vfs_mount *mount,
                         const char *path,
                         uint32_t mode)
{
    struct lwext4_mount_adapter *adapter;
    int result;

    if (mount == 0 || mount->state != VFS_MOUNT_STATE_LIVE ||
        mount->private_data == 0 || path == 0 || path[0] != '/') {
        return -KERNEL_EINVAL;
    }
    adapter = mount->private_data;
    if (adapter->read_only) {
        return -KERNEL_EROFS;
    }

    result = ext4_transaction_begin(LWEXT4_MOUNT_POINT);
    if (result != EOK) return lwext4_error(result);
    result = ext4_dir_mk(path);
    if (result == EOK)
        result = ext4_mode_set(path, (mode & 07777U) | KERNEL_VFS_S_IFDIR);
    if (result == EOK) result = ext4_transaction_end(LWEXT4_MOUNT_POINT);
    else (void)ext4_transaction_abort(LWEXT4_MOUNT_POINT, result);
    if (result != EOK) return lwext4_error(result);
    return 0;
}

static int vfs_unlink_raw(struct kernel_vfs_mount *mount,
                          const char *path)
{
    struct lwext4_mount_adapter *adapter;
    struct kernel_vfs_node *node;
    struct lwext4_orphan *orphan = 0;
    uint32_t inode = 0U;
    bool is_orphan = false;
    int result;

    if (mount == 0 || mount->state != VFS_MOUNT_STATE_LIVE ||
        mount->private_data == 0 || path == 0 || path[0] != '/') {
        return -KERNEL_EINVAL;
    }
    adapter = mount->private_data;
    if (adapter->read_only) {
        return -KERNEL_EROFS;
    }
    result = reserve_orphan(adapter, &orphan);
    if (result != 0) {
        return result;
    }

    result = ext4_funlink_dentry(path, &inode, &is_orphan);
    if (result != EOK) {
        release_orphan(adapter, orphan);
        return lwext4_error(result);
    }

    for (node = adapter->nodes; node != 0; node = node->next) {
        if (node->file.inode == inode && !node->unlinked) {
            break;
        }
    }

    if (node != 0) {
        if (is_orphan) {
            node->unlinked = 1U;
            kernel_vfs_try_release_orphan(node);
        }
        release_orphan(adapter, orphan);
        return 0;
    } else if (is_orphan) {
        orphan->inode = inode;
        orphan->orphan_freed = 0U;
        result = ext4_orphan_free(LWEXT4_MOUNT_POINT, inode);
        if (result != EOK) {
            queue_orphan(adapter, orphan);
            return 0;
        }
        orphan->orphan_freed = 1U;
    }
    release_orphan(adapter, orphan);
    return 0;
}

static int vfs_rmdir_raw(struct kernel_vfs_mount *mount,
                         const char *path)
{
    struct lwext4_mount_adapter *adapter;
    struct kernel_vfs_node *node;
    struct lwext4_orphan *orphan = 0;
    uint32_t mode = 0U;
    uint32_t inode = 0U;
    int result;

    if (mount == 0 || mount->state != VFS_MOUNT_STATE_LIVE ||
        mount->private_data == 0 || path == 0 || path[0] != '/') {
        return -KERNEL_EINVAL;
    }
    adapter = mount->private_data;
    if (adapter->read_only) {
        return -KERNEL_EROFS;
    }
    result = ext4_mode_get(path, &mode);
    if (result != EOK) {
        return lwext4_error(result);
    }
    if ((mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) {
        return -KERNEL_ENOTDIR;
    }

    result = reserve_orphan(adapter, &orphan);
    if (result != 0) return result;
    result = ext4_fdir_unlink_dentry(path, &inode);
    if (result != EOK) {
        release_orphan(adapter, orphan);
        return lwext4_error(result);
    }
    for (node = adapter->nodes; node != 0; node = node->next)
        if (node->file.inode == inode && !node->unlinked) break;
    if (node != 0) {
        node->unlinked = 1U;
        kernel_vfs_try_release_orphan(node);
        release_orphan(adapter, orphan);
        return 0;
    }
    orphan->inode = inode;
    orphan->orphan_freed = 0U;
    result = ext4_orphan_free(LWEXT4_MOUNT_POINT, inode);
    if (result != EOK) {
        queue_orphan(adapter, orphan);
        return 0;
    }
    orphan->orphan_freed = 1U;
    release_orphan(adapter, orphan);
    return 0;
}

int kernel_vfs_mkdir_at(struct kernel_vfs_path *start,
                        struct kernel_vfs_path *root, const char *input,
                        uint32_t mode)
{
    struct kernel_vfs_path *path = 0;
    char name[256] = {0}, *backend = 0;
    int result = mutation_path(start, root, input, 0, 1, 1, 2,
                                &path, name, &backend);
    if (result) return result;
    result = !name[0] ? -KERNEL_EEXIST :
                 vfs_mkdir_raw(start->file.mount, backend, mode);
    (void)kernel_vfs_path_release(&path);
    release_path(start->heap, backend);
    return result;
}

int kernel_vfs_mkdir(struct kernel_vfs_mount *mount, const char *path,
                     uint32_t mode)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    result = kernel_vfs_mkdir_at(root, root, path, mode);
    (void)kernel_vfs_path_release(&root);
    return result;
}

static int special_last_component(const char *path)
{
    if (!path) return 0;
    size_t end = strlen(path), begin;
    while (end && path[end - 1U] == '/') end--;
    if (!end && path[0] == '/') return 3;
    begin = end;
    while (begin && path[begin - 1U] != '/') begin--;
    if (end - begin == 1U && path[begin] == '.') return 1;
    if (end - begin == 2U && path[begin] == '.' && path[begin + 1U] == '.')
        return 2;
    return 0;
}

int kernel_vfs_unlink_at(struct kernel_vfs_path *start,
                         struct kernel_vfs_path *root, const char *input,
                         int directory)
{
    struct kernel_vfs_path *path = 0;
    char name[256] = {0}, *backend = 0;
    int special = special_last_component(input);
    if (directory && special) return special == 1 ? -KERNEL_EINVAL :
                          special == 2 ? -KERNEL_ENOTEMPTY : -KERNEL_EBUSY;
    int result = mutation_path(start, root, input, 0, 0, 0, 0,
                                &path, name, &backend);
    if (result) return result;
    result = directory ? vfs_rmdir_raw(start->file.mount, backend)
                       : vfs_unlink_raw(start->file.mount, backend);
    if (!result) path->detached = 1U;
    (void)kernel_vfs_path_release(&path);
    release_path(start->heap, backend);
    return result;
}

static int unlink_root(struct kernel_vfs_mount *mount, const char *path,
                        int directory)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    result = kernel_vfs_unlink_at(root, root, path, directory);
    (void)kernel_vfs_path_release(&root);
    return result;
}

int kernel_vfs_unlink(struct kernel_vfs_mount *mount, const char *path)
{
    return unlink_root(mount, path, 0);
}

int kernel_vfs_rmdir(struct kernel_vfs_mount *mount, const char *path)
{
    return unlink_root(mount, path, 1);
}

int kernel_vfs_symlink_at(struct kernel_vfs_path *start,
                          struct kernel_vfs_path *root, const char *target,
                          const char *input)
{
    struct kernel_vfs_path *path = 0;
    char name[256] = {0}, *backend = 0;
    if (!start || !target || !target[0]) return -KERNEL_EINVAL;
    if (kernel_vfs_mount_is_readonly(start->file.mount)) return -KERNEL_EROFS;
    int result = mutation_path(start, root, input, 0, 1, 0, 2,
                                &path, name, &backend);
    if (result) return result;
    result = !name[0] ? -KERNEL_EEXIST :
                        lwext4_error(ext4_fsymlink(target, backend));
    (void)kernel_vfs_path_release(&path);
    release_path(start->heap, backend);
    return result;
}

int kernel_vfs_symlink(struct kernel_vfs_mount *mount, const char *target,
                       const char *path)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    result = kernel_vfs_symlink_at(root, root, target, path);
    (void)kernel_vfs_path_release(&root);
    return result;
}

int kernel_vfs_readlink_at(struct kernel_vfs_path *start,
                           struct kernel_vfs_path *root, const char *path,
                           char *buffer, size_t size, size_t *bytes_read)
{
    struct kernel_vfs_path *resolved = 0;
    if (!buffer || !bytes_read || !size) return -KERNEL_EINVAL;
    int result = kernel_vfs_path_resolve(start, root, path, 0, &resolved);
    if (!result) {
        if ((resolved->file.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFLNK)
            result = -KERNEL_EINVAL;
        else
            result = lwext4_error(ext4_readlink_inode(LWEXT4_MOUNT_POINT,
                     (uint32_t)kernel_vfs_path_inode(resolved), buffer,
                     size, bytes_read));
        (void)kernel_vfs_path_release(&resolved);
    }
    return result;
}

int kernel_vfs_readlink(struct kernel_vfs_mount *mount, const char *path,
                        char *buffer, size_t size, size_t *bytes_read)
{
    struct kernel_vfs_path *root = 0;
    int result = mount_root_path(mount, &root);
    if (result) return result;
    result = kernel_vfs_readlink_at(root, root, path, buffer, size, bytes_read);
    (void)kernel_vfs_path_release(&root);
    return result;
}

static int rename_parent(struct kernel_vfs_path *start,
                          struct kernel_vfs_path *root, const char *input,
                          struct kernel_vfs_path **parent, char name[256],
                          int *special, int *trailing)
{
    if (!input[0]) return -KERNEL_ENOENT;
    size_t length = strlen(input), end = length, begin;
    if (length >= KERNEL_FS_PATH_MAX) return -KERNEL_ENAMETOOLONG;
    *trailing = input[length - 1U] == '/';
    while (end && input[end - 1U] == '/') end--;
    begin = end;
    while (begin && input[begin - 1U] != '/') begin--;
    if (end - begin > 255U) return -KERNEL_ENAMETOOLONG;
    memcpy(name, input + begin, end - begin);
    name[end - begin] = 0;
    *special = special_last_component(input);
    if (!begin) {
        struct kernel_vfs_path *base = end ? start : root;
        int result = kernel_vfs_path_acquire(base);
        if (!result) *parent = base;
        return result;
    }
    char *prefix;
    enum kernel_heap_status allocation = kernel_heap_allocate(start->heap,
                                                   begin + 1U, (void **)&prefix);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    memcpy(prefix, input, begin);
    prefix[begin] = 0;
    int result = kernel_vfs_path_resolve(start, root, prefix, 1, parent);
    release_path(start->heap, prefix);
    return result;
}

int kernel_vfs_rename_at(struct kernel_vfs_path *old_start,
                         struct kernel_vfs_path *new_start,
                         struct kernel_vfs_path *root,
                         const char *old_input, const char *new_input,
                         unsigned flags)
{
    struct kernel_vfs_path *source = 0, *target = 0, *new_parent = 0;
    struct kernel_vfs_path *source_parent = 0;
    char missing[256] = {0}, old_component[256];
    char *new_name = 0;
    struct ext4_rename_result renamed = {0};
    int old_special, new_special, old_trailing, new_trailing;
    int result;
    if ((flags & ~7U) || ((flags & 2U) && (flags & 5U))) return -KERNEL_EINVAL;
    if (flags & 6U) return -KERNEL_ENOTSUP;
    if (!old_start || !new_start || !root || !old_input || !new_input)
        return -KERNEL_EINVAL;
    if (old_start->file.mount != new_start->file.mount ||
        root->file.mount != old_start->file.mount) return -KERNEL_EXDEV;
    struct lwext4_mount_adapter *adapter = root->file.mount->private_data;
    result = rename_parent(old_start, root, old_input, &source_parent,
                             old_component, &old_special, &old_trailing);
    if (result) goto Finish;
    result = rename_parent(new_start, root, new_input, &new_parent,
                             missing, &new_special, &new_trailing);
    if (result) goto Finish;
    if (old_special) { result = -KERNEL_EBUSY; goto Finish; }
    if (new_special) { result = flags & 1U ? -KERNEL_EEXIST : -KERNEL_EBUSY; goto Finish; }
    if (adapter->read_only) { result = -KERNEL_EROFS; goto Finish; }
    result = kernel_vfs_path_lookup(source_parent, old_component,
                                    strlen(old_component), &source);
    if (result) goto Finish;
    result = kernel_vfs_path_lookup(new_parent, missing, strlen(missing), &target);
    if (result && result != -KERNEL_ENOENT) goto Finish;
    if ((flags & 1U) && target) { result = -KERNEL_EEXIST; goto Finish; }
    if ((old_trailing || new_trailing) &&
        (source->file.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) {
        result = -KERNEL_ENOTDIR;
        goto Finish;
    }
    if (((struct kernel_vfs_node *)new_parent->file.private_data)->unlinked) {
        result = -KERNEL_ENOENT;
        goto Finish;
    }
    enum kernel_heap_status allocation = kernel_heap_allocate(adapter->heap,
                                  strlen(missing) + 1U, (void **)&new_name);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        result = allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM
                                                        : -KERNEL_EIO;
        goto Finish;
    }
    memcpy(new_name, missing, strlen(missing) + 1U);
    /* The single-hart VFS is entered with scheduling/interrupts serialized.
     * All references and storage are reserved before disk mutation; this call
     * cannot yield between the transaction and publishing dentry identity. */
    result = lwext4_error(ext4_rename_child(LWEXT4_MOUNT_POINT,
                 (uint32_t)kernel_vfs_path_inode(source->parent), source->name,
                 (uint32_t)strlen(source->name),
                 (uint32_t)kernel_vfs_path_inode(new_parent), new_name,
                 (uint32_t)strlen(new_name), flags, &renamed));
    if (result || !renamed.changed) goto Finish;
    if (target) {
        struct kernel_vfs_node *node = target->file.private_data;
        if (renamed.replaced_inode != node->file.inode) __builtin_trap();
        target->detached = 1U;
        if (renamed.replaced_last_link) node->unlinked = 1U;
    } else if (renamed.replaced_inode) __builtin_trap();
    struct kernel_vfs_path *old_parent = source->parent;
    char *old_name = source->name;
    source->parent = new_parent;
    source->name = new_name;
    new_parent = 0;
    new_name = 0;
    if (old_name != source->initial_name) release_path(adapter->heap, old_name);
    (void)kernel_vfs_path_release(&old_parent);
Finish:
    if (new_name) release_path(adapter->heap, new_name);
    if (new_parent) (void)kernel_vfs_path_release(&new_parent);
    if (target) (void)kernel_vfs_path_release(&target);
    if (source) (void)kernel_vfs_path_release(&source);
    if (source_parent) (void)kernel_vfs_path_release(&source_parent);
    return result;
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

int kernel_vfs_dir_entry(struct kernel_vfs_file *file,
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
    if ((node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) {
        return -KERNEL_ENOTDIR;
    }
    struct kernel_vfs_stat current;
    result = kernel_vfs_fstat(file, &current);
    if (result) return result;
    node->size = current.size;
    node->file.fsize = current.size;
    file->size = current.size;
    *next_position = position;
    if (position >= node->size) {
        *next_position = node->size;
        return 0;
    }
    start = position & ~UINT64_C(3);
    directory.f = node->file;
    directory.next_off = start;
    for (;;) {
        result = ext4_dir_entry_next_status(&directory,
                                            &directory.de,
                                            &entry_offset);
        if (result != EOK) {
            return lwext4_error(result);
        }
        if (entry_offset == UINT64_MAX) {
            *next_position = node->size;
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
        if (entry_offset >= node->size || *next_position <= entry_offset ||
            *next_position > node->size ||
            *next_position == UINT64_MAX) {
            return -KERNEL_EIO;
        }
        return 1;
    }
}

struct kernel_vfs_node *kernel_vfs_file_node(
    const struct kernel_vfs_file *file)
{
    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0) {
        return 0;
    }
    return file->private_data;
}

int kernel_vfs_node_acquire(struct kernel_vfs_node *node)
{
    if (node == 0 || node->references == 0U || node->closed ||
        node->references == UINT32_MAX) {
        return -KERNEL_EINVAL;
    }
    node->references++;
    return 0;
}

void kernel_file_mapping_register(struct kernel_file_mapping *mapping)
{
    if (mapping == 0 || mapping->node == 0 || mapping->owner == 0 ||
        mapping->truncate == 0 || mapping->previous != 0) {
        __builtin_trap();
    }
    mapping->next = mapping->node->mappings;
    mapping->previous = &mapping->node->mappings;
    if (mapping->next != 0) mapping->next->previous = &mapping->next;
    mapping->node->mappings = mapping;
}

void kernel_file_mapping_unregister(struct kernel_file_mapping *mapping)
{
    if (mapping->previous == 0) return;
    *mapping->previous = mapping->next;
    if (mapping->next != 0) mapping->next->previous = mapping->previous;
    mapping->next = 0;
    mapping->previous = 0;
}

int kernel_vfs_node_release(struct kernel_vfs_node **owner)
{
    struct kernel_vfs_node *node;
    struct kernel_vfs_node **link;
    int result;

    if (owner == 0 || *owner == 0) {
        return -KERNEL_EINVAL;
    }
    node = *owner;
    if (node->references > 1U) {
        node->references--;
        *owner = 0;
        return 0;
    }
    if (node->references == 1U) {
        if (node->mappings != 0) __builtin_trap();
        node->references = 0U;
        link = &node->adapter->nodes;
        while (*link != 0 && *link != node) {
            link = &(*link)->next;
        }
        if (*link != node) {
            return -KERNEL_EIO;
        }
        *link = node->next;
        node->next = 0;
        if (node->unlinked && !node->orphan_freed) {
            node->next = node->adapter->cleanup_nodes;
            node->adapter->cleanup_nodes = node;
            *owner = 0;
            return 0;
        }
    }
    if (!node->closed) {
        result = ext4_fclose(&node->file);
        if (result != EOK) {
            node->next = node->adapter->cleanup_nodes;
            node->adapter->cleanup_nodes = node;
            *owner = 0;
            return 0;
        }
        node->closed = 1U;
    }
    (void)kernel_heap_release(node->adapter->heap, node);
    *owner = 0;
    return 0;
}

int kernel_vfs_node_pread(struct kernel_vfs_node *node,
                          uint64_t offset,
                          void *buffer,
                          size_t size,
                          size_t *bytes_read)
{
    size_t requested;
    size_t result_count;
    int result;

    if (node == 0 || node->references == 0U || node->closed ||
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
    result = ext4_fseek(&node->file, (int64_t)offset, SEEK_SET);
    if (result != EOK) {
        return lwext4_error(result);
    }
    result = ext4_fread(&node->file, buffer, requested, &result_count);
    if (result != EOK) {
        return lwext4_error(result);
    }
    if (result_count > requested) {
        return -KERNEL_EIO;
    }
    *bytes_read = result_count;
    return 0;
}

uint64_t kernel_vfs_node_size(const struct kernel_vfs_node *node)
{
    return node != 0 && node->references != 0U && !node->closed
               ? node->size : 0U;
}

struct kernel_page_cache_entry **kernel_vfs_node_cache_pages(
    struct kernel_vfs_node *node)
{
    return &node->cache_pages;
}

void kernel_vfs_node_written(struct kernel_vfs_node *node, uint64_t end)
{
    if (end > node->size) node->size = end;
}

int kernel_vfs_node_writeback_allowed(const struct kernel_vfs_node *node)
{
    return node != 0 && !boaros_lwext4_allocation_active();
}

static void record_writeback_error(struct kernel_vfs_node *node, int error)
{
    node->writeback_error = error;
    node->writeback_error_sequence++;
    if (node->writeback_error_sequence == 0) __builtin_trap();
}

int kernel_vfs_node_writeback(struct kernel_vfs_node *node, uint64_t offset,
                              const void *buffer, size_t size, size_t *written)
{
    *written = 0;
    int result = ext4_fseek(&node->file, (int64_t)offset, SEEK_SET);
    if (result == EOK) result = ext4_fwrite(&node->file, buffer, size, written);
    if (result == EOK && *written != size) result = EIO;
    if (result != EOK) record_writeback_error(node, lwext4_error(result));
    return lwext4_error(result);
}

uint64_t kernel_vfs_error_sequence(const struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    return node != 0 ? node->writeback_error_sequence : 0;
}

int kernel_vfs_sync(struct kernel_vfs_file *file, int datasync,
                    uint64_t *observed_error)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);
    int result;
    (void)datasync; /* Metadata is currently submitted with each mutation. */
    if (node == 0 || observed_error == 0) return -KERNEL_EBADF;
    if ((node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG &&
        (node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR)
        return -KERNEL_EINVAL;
    result = 0;
    if (node->adapter->read_only) goto observe;
    result = kernel_page_cache_writeback(node->adapter->page_cache, node);
    if (result == 0) {
        result = lwext4_error(ext4_file_sync_metadata(&node->file));
        if (result != 0) record_writeback_error(node, result);
    }
    if (result == 0) {
        enum kernel_block_status status = kernel_block_flush(node->adapter->block);
        if (status != KERNEL_BLOCK_STATUS_OK) {
            result = status == KERNEL_BLOCK_STATUS_UNSUPPORTED
                         ? -KERNEL_ENOTSUP : -KERNEL_EIO;
            record_writeback_error(node, result);
        }
    }
observe:
    if (*observed_error != node->writeback_error_sequence) {
        if (result == 0) result = node->writeback_error;
        *observed_error = node->writeback_error_sequence;
    }
    return result;
}

const struct kernel_vfs_mount *kernel_vfs_node_mount(
    const struct kernel_vfs_node *node)
{
    return node != 0 ? node->mount : 0;
}

int kernel_vfs_files_share_node(const struct kernel_vfs_file *left,
                                const struct kernel_vfs_file *right)
{
    struct kernel_vfs_node *left_node = kernel_vfs_file_node(left);

    return left_node != 0 && left_node == kernel_vfs_file_node(right);
}

struct kernel_page_cache *kernel_vfs_file_page_cache(
    const struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(file);

    return node != 0 ? node->adapter->page_cache : 0;
}
