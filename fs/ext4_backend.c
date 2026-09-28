#include "lwext4_port.h"
#include "ext4_backend.h"
#include "vfs_internal.h"
#include "record_lock.h"

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

#define LWEXT4_DEVICE_NAME "root"
#define LWEXT4_MOUNT_POINT "/"
#define LWEXT4_PHYSICAL_BLOCK_SIZE 512U
#define VFS_ROOT_MOUNT_ID UINT64_C(1)

static int mount_error(const struct lwext4_mount_adapter *adapter);
static int prepare_journal_mount(struct lwext4_mount_adapter *adapter);
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
static int ext4_backend_open(struct kernel_vfs_mount *mount,
                        const char *path, uint64_t inode_number,
                        uint32_t inode_mode,
                        struct kernel_vfs_file *file);
static int ext4_backend_create(struct kernel_vfs_mount *mount,
                          const char *path,
                          uint32_t mode,
                          struct kernel_vfs_file *file);
static void ext4_backend_accessed(struct kernel_vfs_file *file);
static int ext4_backend_modified(struct kernel_vfs_file *file,
                              uint64_t offset, int append);
static int ext4_backend_stat(const struct kernel_vfs_file *file,
                     struct kernel_vfs_stat *stat);
static int ext4_backend_mkdir(struct kernel_vfs_mount *mount,
                         const char *path,
                         uint32_t mode);
static int ext4_backend_unlink(struct kernel_vfs_mount *mount,
                          const char *path);
static int ext4_backend_rmdir(struct kernel_vfs_mount *mount,
                         const char *path);
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
static int ext4_backend_writeback(struct kernel_vfs_node *node, uint64_t offset,
                              const void *buffer, size_t size, size_t *written);

static struct kernel_vfs_backend ext4_backend;
static void initialize_backend(void);
static int mount_error(const struct lwext4_mount_adapter *adapter)
{
    if (adapter->mount_error) return adapter->mount_error;
    struct ext4_fs *fs = adapter->device.fs;
    return fs && fs->jbd_journal ? fs->jbd_journal->error : EOK;
}

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
    struct kernel_heap *heap = adapter->instance.heap;

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
    (void)kernel_heap_release(adapter->instance.heap, orphan);
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

    if (adapter->instance.external_files != 0U) {
        return -KERNEL_EBUSY;
    }
    mount->state = VFS_MOUNT_STATE_CLEANUP;
    if (adapter->instance.page_cache != 0) {
        enum kernel_page_cache_status cache_status =
            kernel_page_cache_purge_mount(adapter->instance.page_cache, mount);

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
        (void)kernel_heap_release(adapter->instance.heap, orphan);
        *orphan_link = next;
    }
    cleanup_link = &adapter->instance.cleanup_nodes;
    while (*cleanup_link != 0) {
        struct kernel_vfs_node *node = *cleanup_link;
        struct kernel_vfs_node *next = node->next;

        if (node->unlinked && !node->retired) {
            result = ext4_orphan_free(LWEXT4_MOUNT_POINT, node->inode);
            if (result == EOK) {
                node->retired = 1U;
            } else {
                return lwext4_error(result);
            }
        }
        if (!node->closed) {
            result = ext4_fclose(lwext4_node_file(node));
            if (result != EOK) {
                return lwext4_error(result);
            }
            node->closed = 1U;
        }
        (void)kernel_heap_release(adapter->instance.heap, node);
        *cleanup_link = next;
    }
    if (adapter->orphans != 0 || adapter->instance.cleanup_nodes != 0 ||
        adapter->instance.nodes != 0) {
        return -KERNEL_EIO;
    }
    if (adapter->mounted) {
        result = ext4_umount(LWEXT4_MOUNT_POINT);
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
    kernel_mutex_init(&adapter->instance.namespace_lock, 20, (uintptr_t)adapter);
    initialize_backend();
    adapter->instance.backend_data = adapter;
    adapter->instance.ops = &ext4_backend;
    adapter->instance.heap = heap;
    adapter->block = block;
    adapter->instance.page_cache = page_cache;
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
    adapter->interface.wait_read = boaros_lwext4_wait;
    adapter->interface.wake_read = boaros_lwext4_wake;
    adapter->interface.read_context = boaros_lwext4_read_context;
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
    adapter->instance.read_only = (uint8_t)(read_only != 0 ? 1U : 0U);
    result = ext4_mount(LWEXT4_DEVICE_NAME, LWEXT4_MOUNT_POINT, read_only != 0);
    if (result != EOK) {
        (void)cleanup_mount(mount);
        return lwext4_error(result);
    }
    adapter->mounted = 1U;
    result = ext4_mount_setup_locks(LWEXT4_MOUNT_POINT, &boaros_lwext4_locks);
    if (result != EOK) { (void)cleanup_mount(mount); return lwext4_error(result); }
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
    result = ext4_mount_point_stats(LWEXT4_MOUNT_POINT, &disk);
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
    if (!vfs_realtime(&now)) return -KERNEL_EIO;
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
        result = ext4_fopen_inode(lwext4_node_file(node), LWEXT4_MOUNT_POINT,
                                  inode_number);
        if (result != EOK) {
            (void)kernel_heap_release(adapter->instance.heap, node);
            return lwext4_error(result);
        }
    } else if ((mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR) {
        /* ext4_fopen refuses directories; ext4_dir_open_file accepts them and
         * leaves the same file handle shape without allocating an ext4_dir on stack. */
        result = ext4_dir_open_file(lwext4_node_file(node), path);
        if (result != EOK) {
            (void)kernel_heap_release(adapter->instance.heap, node);
            return lwext4_error(result);
        }
    } else if ((mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFCHR) {
        result = ext4_chrdev_open_file(lwext4_node_file(node), path);
        if (result != EOK) {
            (void)kernel_heap_release(adapter->instance.heap, node);
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
            (void)kernel_heap_release(adapter->instance.heap, node);
            return lwext4_error(result);
        }
    }
    node->inode = lwext4_node_file(node)->inode;
    node->max_size = lwext4_node_file(node)->fmax;
    node->size = ext4_fsize(lwext4_node_file(node));
    node->mode = mode;
    return kernel_vfs_publish_node(mount, node, file, 0);
}

static int ext4_backend_create(struct kernel_vfs_mount *mount,
                          const char *path,
                          uint32_t mode,
                          struct kernel_vfs_file *file)
{
    struct lwext4_mount_adapter *adapter;
    struct kernel_vfs_node *node;
    enum kernel_heap_status heap_status;
    int result;

    if (mount == 0 || mount->state != VFS_MOUNT_STATE_LIVE ||
        mount->private_data == 0 || path == 0 || path[0] != '/' ||
        file == 0 || file->state != VFS_FILE_STATE_EMPTY ||
        file->private_data != 0) {
        return -KERNEL_EINVAL;
    }
    adapter = mount->private_data;
    if (adapter->instance.read_only) {
        return -KERNEL_EROFS;
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
    result = ext4_transaction_begin(LWEXT4_MOUNT_POINT);
    if (result == EOK) {
        result = ext4_fopen2(lwext4_node_file(node), path, O_CREAT | O_RDWR);
        if (result == EOK)
            result = ext4_mode_set(path, (mode & 07777U) | KERNEL_VFS_S_IFREG);
        if (result == EOK) result = ext4_transaction_end(LWEXT4_MOUNT_POINT);
        else (void)ext4_transaction_abort(LWEXT4_MOUNT_POINT, result);
    }
    if (result != EOK) {
        /* This unopened owner has no data or mapping; the mount owns any
         * uncertain journal state. An aborted create must not unlink by name. */
        if ((*lwext4_node_file(node)).mp && ext4_fclose(lwext4_node_file(node)) != EOK) {
            node->instance = &adapter->instance;
            node->next = adapter->instance.cleanup_nodes;
            adapter->instance.cleanup_nodes = node;
        } else {
            (void)kernel_heap_release(adapter->instance.heap, node);
        }
        return lwext4_error(result);
    }

    node->inode = lwext4_node_file(node)->inode;
    node->max_size = lwext4_node_file(node)->fmax;
    node->size = ext4_fsize(lwext4_node_file(node));
    node->mode = (mode & 07777U) | KERNEL_VFS_S_IFREG;
    return kernel_vfs_publish_node(mount, node, file, 1);
}

static void ext4_backend_accessed(struct kernel_vfs_file *file)
{
    struct kernel_vfs_node *node;
    if (file == 0 || file->state != VFS_FILE_STATE_LIVE ||
        file->private_data == 0) return;
    node = file->private_data;
    KERNEL_LOCK_SCOPE(node_guard);
    kernel_vfs_node_lock(node, &node_guard, 1);
    /* Linux touch_atime does not change a read's result on metadata I/O
     * failure. The mount's dirty block cache continues to own that state. */
    (void)ext4_file_touch(lwext4_node_file(node), EXT4_TIME_ATIME | EXT4_TIME_RELATIME);
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
    return lwext4_error(ext4_file_touch(lwext4_node_file(node),
                                       EXT4_TIME_MTIME | EXT4_TIME_CTIME));
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
    if ((node->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFREG)
        stat->size = node->size;
    return 0;
}

static int ext4_backend_mkdir(struct kernel_vfs_mount *mount,
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
    if (adapter->instance.read_only) {
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

static int ext4_backend_unlink(struct kernel_vfs_mount *mount,
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
    if (adapter->instance.read_only) {
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

    for (node = adapter->instance.nodes; node != 0; node = node->next) {
        if (node->inode == inode && !node->unlinked) {
            break;
        }
    }

    if (node != 0) {
        if (is_orphan) {
            node->unlinked = 1U;
            ext4_backend_release_unlinked(node);
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

static int ext4_backend_rmdir(struct kernel_vfs_mount *mount,
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
    if (adapter->instance.read_only) {
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
    for (node = adapter->instance.nodes; node != 0; node = node->next)
        if (node->inode == inode && !node->unlinked) break;
    if (node != 0) {
        node->unlinked = 1U;
        ext4_backend_release_unlinked(node);
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

static void ext4_backend_release_unlinked(struct kernel_vfs_node *node)
{
    struct kernel_vfs_node *temp;
    int result;

    if (node == 0) return;
    node->references++;
    struct kernel_lock_guard guard = {0};
    kernel_vfs_node_lock(node, &guard, 1);
    if (!node->unlinked || node->retired) {
        goto out;
    }
    if (node->open_files > 0U || node->exec_users > 0U) {
        goto out;
    }

    if (node->instance != 0 && node->instance->page_cache != 0) {
        (void)kernel_page_cache_invalidate_node(node->instance->page_cache,
                                                node);
    }

    result = ext4_orphan_free(LWEXT4_MOUNT_POINT, node->inode);
    if (result == EOK) {
        node->retired = 1U;
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
    node->size = current.size;
    (*lwext4_node_file(node)).fsize = current.size;
    file->size = current.size;
    *next_position = position;
    if (position >= node->size) {
        *next_position = node->size;
        return 0;
    }
    start = position & ~UINT64_C(3);
    directory.f = (*lwext4_node_file(node));
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

static int ext4_backend_pread(struct kernel_vfs_node *node,
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
    result = ext4_fpread(lwext4_node_file(node), offset, buffer, requested, &result_count);
    if (result != EOK) {
        return lwext4_error(result);
    }
    if (result_count > requested) {
        return -KERNEL_EIO;
    }
    *bytes_read = result_count;
    return 0;
}

static int ext4_backend_writeback(struct kernel_vfs_node *node, uint64_t offset,
                              const void *buffer, size_t size, size_t *written)
{
    *written = 0;
    boaros_lwext4_locks.lock();
    int result = ext4_fseek(lwext4_node_file(node), (int64_t)offset, SEEK_SET);
    if (result == EOK) result = ext4_fwrite(lwext4_node_file(node), buffer, size, written);
    boaros_lwext4_locks.unlock();
    if (result == EOK && *written != size) result = EIO;
    if (result != EOK) kernel_vfs_record_writeback_error(node, lwext4_error(result));
    return lwext4_error(result);
}

static int ext4_backend_error(struct kernel_vfs_instance *instance)
{ return lwext4_error(mount_error(lwext4_instance(instance))); }
static int ext4_backend_root(struct kernel_vfs_instance *instance, uint64_t *inode, uint32_t *mode)
{ (void)instance; *inode = EXT4_INODE_ROOT_INDEX; return lwext4_error(ext4_mode_get("/", mode)); }
static int ext4_backend_lookup(struct kernel_vfs_instance *instance, uint64_t parent,
    const char *name, size_t length, uint64_t *inode, uint32_t *mode)
{
    int error = ext4_backend_error(instance);
    if (error) return error;
    if (parent > UINT32_MAX) return -KERNEL_EINVAL;
    uint32_t result_inode;
    error = lwext4_error(ext4_lookup_child(LWEXT4_MOUNT_POINT, (uint32_t)parent,
                                             name, length, &result_inode, mode));
    if (!error) *inode = result_inode;
    return error;
}
static int ext4_backend_readlink(struct kernel_vfs_node *node, char *buffer, size_t size, size_t *count)
{ return lwext4_error(ext4_readlink_inode(LWEXT4_MOUNT_POINT, node->inode, buffer, size, count)); }
static int ext4_backend_truncate(struct kernel_vfs_node *node, uint64_t size, uint64_t *actual, int *changed)
{
    uint64_t before = ext4_fsize(lwext4_node_file(node));
    int result = ext4_ftruncate(lwext4_node_file(node), size);
    *actual = ext4_fsize(lwext4_node_file(node));
    *changed = result == EOK || before != *actual;
    return lwext4_error(result);
}
static int ext4_backend_close_node(struct kernel_vfs_node *node)
{ return lwext4_error(ext4_fclose(lwext4_node_file(node))); }
static int ext4_backend_writeback_allowed(struct kernel_vfs_instance *instance)
{ (void)instance; return !boaros_lwext4_allocation_active(); }
static int ext4_backend_sync_metadata(struct kernel_vfs_node *node)
{ return lwext4_error(ext4_file_sync_metadata(lwext4_node_file(node))); }
static int ext4_backend_flush(struct kernel_vfs_instance *instance)
{
    enum kernel_block_status status = kernel_block_flush(lwext4_instance(instance)->block);
    return status == KERNEL_BLOCK_STATUS_OK ? 0 :
        status == KERNEL_BLOCK_STATUS_UNSUPPORTED ? -KERNEL_ENOTSUP : -KERNEL_EIO;
}
static int ext4_backend_symlink(struct kernel_vfs_instance *instance, const char *target, const char *path)
{ (void)instance; return lwext4_error(ext4_fsymlink(target, path)); }
static int ext4_backend_mknod(struct kernel_vfs_instance *instance, const char *path,
    uint32_t type, uint32_t mode, uint32_t device)
{
    (void)instance;
    int status = ext4_transaction_begin(LWEXT4_MOUNT_POINT);
    if (status == EOK) {
        if (type == KERNEL_VFS_S_IFCHR) status = ext4_mknod(path, EXT4_DE_CHRDEV, device);
        else {
            ext4_file file;
            status = ext4_fopen2(&file, path, O_CREAT | O_EXCL | O_RDWR);
            if (status == EOK) status = ext4_fclose(&file);
        }
        if (status == EOK) status = ext4_mode_set(path, type | (mode & 07777U));
        /* 创建和 mode 同属后端写事务；不确定失败仍由 mount 日志持有。 */
        if (status == EOK) status = ext4_transaction_end(LWEXT4_MOUNT_POINT);
        else (void)ext4_transaction_abort(LWEXT4_MOUNT_POINT, status);
    }
    return lwext4_error(status);
}
static int ext4_backend_rename(struct kernel_vfs_instance *instance,
    uint64_t old_parent, const char *old_name, uint64_t new_parent,
    const char *new_name, unsigned flags, struct kernel_vfs_rename_result *result)
{
    (void)instance;
    if (old_parent > UINT32_MAX || new_parent > UINT32_MAX)
        return -KERNEL_EINVAL;
    struct ext4_rename_result renamed = {0};
    int status = ext4_rename_child(LWEXT4_MOUNT_POINT, (uint32_t)old_parent,
        old_name, strlen(old_name), (uint32_t)new_parent, new_name,
        strlen(new_name), flags, &renamed);
    if (!status) {
        result->replaced_inode = renamed.replaced_inode;
        result->replaced_last_link = renamed.replaced_last_link;
        result->changed = renamed.changed;
    }
    return lwext4_error(status);
}

static void initialize_backend(void)
{
    /* 模块测试尚用物理地址；逐项生成当前执行域的回调，不能复制高半区常量表。 */
    volatile struct kernel_vfs_backend *ops = &ext4_backend;
    ops->error = ext4_backend_error;
    ops->root = ext4_backend_root;
    ops->lookup = ext4_backend_lookup;
    ops->readlink = ext4_backend_readlink;
    ops->truncate = ext4_backend_truncate;
    ops->close_node = ext4_backend_close_node;
    ops->writeback_allowed = ext4_backend_writeback_allowed;
    ops->sync_metadata = ext4_backend_sync_metadata;
    ops->flush = ext4_backend_flush;
    ops->symlink = ext4_backend_symlink;
    ops->mknod = ext4_backend_mknod;
    ops->rename = ext4_backend_rename;
    ops->unmount = ext4_backend_unmount;
    ops->statfs = ext4_backend_statfs;
    ops->set_times = ext4_backend_set_times;
    ops->set_mode = ext4_backend_set_mode;
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
}
