#include "lwext4_port.h"
#include "vfs_internal.h"

#include <kernel/block.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/page_cache.h>
#include <kernel/vfs.h>

#include <ext4.h>
#include <ext4_blockdev.h>
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

struct lwext4_mount_adapter {
    struct kernel_heap *heap;
    struct kernel_block_device *block;
    struct ext4_blockdev_iface interface;
    struct ext4_blockdev device;
    unsigned char *physical_buffer;
    struct kernel_page_cache *page_cache;
    struct kernel_vfs_node *nodes;
    struct kernel_vfs_node *cleanup_nodes;
    uint32_t external_files;
    uint8_t registered;
    uint8_t mounted;
    uint8_t heap_bound;
};

struct kernel_vfs_node {
    struct kernel_vfs_node *next;
    struct lwext4_mount_adapter *adapter;
    struct kernel_vfs_mount *mount;
    ext4_file file;
    uint64_t size;
    uint32_t mode;
    uint32_t references;
    uint8_t closed;
};

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
    (void)device;
    (void)buffer;
    (void)block;
    (void)count;
    return EROFS;
}

static int block_close(struct ext4_blockdev *device)
{
    return block_open(device);
}

static int release_mount_storage(struct kernel_vfs_mount *mount)
{
    struct lwext4_mount_adapter *adapter = mount->private_data;
    struct kernel_heap *heap = adapter->heap;
    enum kernel_heap_status heap_status;

    if (adapter->physical_buffer != 0) {
        heap_status = kernel_heap_release(heap, adapter->physical_buffer);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            return -KERNEL_EIO;
        }
        adapter->physical_buffer = 0;
    }
    if (adapter->heap_bound) {
        boaros_lwext4_heap_unbind(heap);
        adapter->heap_bound = 0U;
    }

    heap_status = kernel_heap_release(heap, adapter);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return -KERNEL_EIO;
    }
    mount->private_data = 0;
    mount->state = VFS_MOUNT_STATE_EMPTY;
    return 0;
}

static int cleanup_mount(struct kernel_vfs_mount *mount)
{
    struct lwext4_mount_adapter *adapter = mount->private_data;
    struct kernel_vfs_node **cleanup_link;
    int result;

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
    cleanup_link = &adapter->cleanup_nodes;
    while (*cleanup_link != 0) {
        struct kernel_vfs_node *node = *cleanup_link;
        struct kernel_vfs_node *next = node->next;

        if (!node->closed) {
            result = ext4_fclose(&node->file);
            if (result != EOK) {
                return lwext4_error(result);
            }
            node->closed = 1U;
        }
        if (kernel_heap_release(adapter->heap, node) !=
            KERNEL_HEAP_STATUS_OK) {
            cleanup_link = &node->next;
        } else {
            *cleanup_link = next;
        }
    }
    if (adapter->cleanup_nodes != 0 || adapter->nodes != 0) {
        return -KERNEL_EIO;
    }
    if (adapter->mounted) {
        result = ext4_umount(LWEXT4_MOUNT_POINT);
        if (result != EOK) {
            return lwext4_error(result);
        }
        adapter->mounted = 0U;
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

int kernel_vfs_mount_root_readonly(struct kernel_vfs_mount *mount,
                                   struct kernel_block_device *block,
                                   struct kernel_heap *heap,
                                   struct kernel_page_cache *page_cache)
{
    struct lwext4_mount_adapter *adapter;
    struct ext4_sblock *superblock;
    enum kernel_heap_status heap_status;
    int result;

    if (mount == 0 || mount->state != VFS_MOUNT_STATE_EMPTY ||
        mount->private_data != 0 || block == 0 || block->read == 0 ||
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
    result = ext4_mount(LWEXT4_DEVICE_NAME, LWEXT4_MOUNT_POINT, true);
    if (result != EOK) {
        (void)cleanup_mount(mount);
        return lwext4_error(result);
    }
    adapter->mounted = 1U;

    result = ext4_get_sblock(LWEXT4_MOUNT_POINT, &superblock);
    if (result != EOK) {
        (void)cleanup_mount(mount);
        return lwext4_error(result);
    }
    if (ext4_sb_feature_incom(superblock, EXT4_FINCOM_RECOVER)) {
        (void)cleanup_mount(mount);
        return -KERNEL_EUCLEAN;
    }

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

int kernel_vfs_open(struct kernel_vfs_mount *mount,
                    const char *path,
                    struct kernel_vfs_file *file)
{
    struct lwext4_mount_adapter *adapter;
    struct kernel_vfs_node *node;
    struct kernel_vfs_node *existing;
    enum kernel_heap_status heap_status;
    uint32_t mode;
    int result;

    if (mount == 0 || mount->state != VFS_MOUNT_STATE_LIVE ||
        mount->private_data == 0 || path == 0 || path[0] != '/' ||
        file == 0 || file->state != VFS_FILE_STATE_EMPTY ||
        file->private_data != 0) {
        return -KERNEL_EINVAL;
    }
    adapter = mount->private_data;
    result = ext4_mode_get(path, &mode);
    if (result != EOK) {
        return lwext4_error(result);
    }
    heap_status = kernel_heap_allocate_zeroed(adapter->heap,
                                              1U,
                                              sizeof(*node),
                                              (void **)&node);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY ?
                   -KERNEL_ENOMEM : -KERNEL_EIO;
    }

    if ((mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFDIR) {
        /* ext4_fopen refuses directories; ext4_dir_open accepts them and
         * leaves the same file handle shape. */
        ext4_dir directory;

        result = ext4_dir_open(&directory, path);
        if (result != EOK) {
            (void)kernel_heap_release(adapter->heap, node);
            return lwext4_error(result);
        }
        node->file = directory.f;
    } else {
        result = ext4_fopen(&node->file, path, "r");
        if (result != EOK) {
            (void)kernel_heap_release(adapter->heap, node);
            return lwext4_error(result);
        }
    }
    for (existing = adapter->nodes;
         existing != 0;
         existing = existing->next) {
        if (existing->file.inode == node->file.inode) {
            break;
        }
    }
    if (existing != 0) {
        result = ext4_fclose(&node->file);
        node->closed = 1U;
        if (result != EOK ||
            kernel_heap_release(adapter->heap, node) !=
                KERNEL_HEAP_STATUS_OK) {
            node->next = adapter->cleanup_nodes;
            adapter->cleanup_nodes = node;
            return result != EOK ? lwext4_error(result)
                                 : -KERNEL_EIO;
        }
        if (existing->references == UINT32_MAX) {
            return -KERNEL_EOVERFLOW;
        }
        existing->references++;
        node = existing;
    } else {
        node->adapter = adapter;
        node->mount = mount;
        node->size = ext4_fsize(&node->file);
        node->mode = mode;
        node->references = 1U;
        node->next = adapter->nodes;
        adapter->nodes = node;
    }
    file->private_data = node;
    file->mount = mount;
    file->size = node->size;
    file->mode = node->mode;
    file->state = VFS_FILE_STATE_LIVE;
    adapter->external_files++;
    return 0;
}

int kernel_vfs_open_executable(struct kernel_vfs_mount *mount,
                               const char *path,
                               struct kernel_vfs_file *file)
{
    int result = kernel_vfs_open(mount, path, file);

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
    return 0;
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
    return kernel_vfs_node_pread(file->private_data,
                                 offset,
                                 buffer,
                                 size,
                                 bytes_read);
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
    adapter = file->mount->private_data;
    if (adapter == 0) {
        return -KERNEL_EIO;
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
                         uint64_t index,
                         uint64_t *inode,
                         uint8_t *type,
                         char *name,
                         size_t name_size)
{
    const struct kernel_vfs_node *node;
    ext4_dir directory;
    const ext4_direntry *entry;
    uint64_t position = 0U;
    size_t name_length;

    if (file == 0 || file->private_data == 0 || inode == 0 || type == 0 ||
        name == 0 || name_size == 0U ||
        file->state != VFS_FILE_STATE_LIVE) {
        return -KERNEL_EINVAL;
    }
    node = file->private_data;
    if ((node->mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR) {
        return -KERNEL_ENOTDIR;
    }

    directory.f = node->file;
    ext4_dir_entry_rewind(&directory);
    while ((entry = ext4_dir_entry_next(&directory)) != 0) {
        if (entry->name_length == 1U && entry->name[0] == '.') {
            continue;
        }
        if (entry->name_length == 2U && entry->name[0] == '.' &&
            entry->name[1] == '.') {
            continue;
        }
        if (position == index) {
            break;
        }
        position++;
    }
    if (entry == 0) {
        return 0;
    }
    name_length = entry->name_length;
    if (name_length >= name_size) {
        return -KERNEL_ENAMETOOLONG;
    }
    memcpy(name, entry->name, name_length);
    name[name_length] = '\0';
    *inode = entry->inode;
    *type = dirent_type_from_ext4(entry->inode_type);
    return 1;
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
    }
    if (!node->closed) {
        result = ext4_fclose(&node->file);
        if (result != EOK) {
            return lwext4_error(result);
        }
        node->closed = 1U;
    }
    if (kernel_heap_release(node->adapter->heap, node) !=
        KERNEL_HEAP_STATUS_OK) {
        return -KERNEL_EIO;
    }
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
