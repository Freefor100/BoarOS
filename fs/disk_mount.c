#include <kernel/vfs.h>
#include <kernel/page_cache.h>
#include <kernel/errno.h>
#include <arch/riscv/context.h>
#include <string.h>

struct disk_mount {
    struct kernel_vfs_mount mount;
    struct kernel_page_cache cache;
    struct kernel_heap *heap;
    struct disk_mount *next;
    int pending;
    char *source;
};
static struct disk_mount *disk_mounts;

static void release_disk(struct kernel_vfs_mount *mount)
{
    struct disk_mount *disk = (struct disk_mount *)mount;
    struct kernel_heap *heap = disk->heap;
    if (disk->cache.state == KERNEL_PAGE_CACHE_LIVE ||
        disk->cache.state == KERNEL_PAGE_CACHE_CLEANUP) {
        if (kernel_page_cache_destroy(&disk->cache) != KERNEL_PAGE_CACHE_STATUS_OK)
            __builtin_trap();
    }
    uintptr_t irq = riscv_interrupt_save();
    struct disk_mount **link = &disk_mounts;
    while (*link && *link != disk) link = &(*link)->next;
    if (!*link) __builtin_trap();
    *link = disk->next;
    riscv_interrupt_restore(irq);
    if (disk->source && kernel_heap_release(heap, disk->source) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    if (kernel_heap_release(heap, disk) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
}

int kernel_vfs_disk_cleanup_pending(void)
{
    int first = 0;
    for (;;) {
        uintptr_t irq = riscv_interrupt_save();
        struct disk_mount *disk = disk_mounts;
        while (disk && disk->pending != 1) disk = disk->next;
        if (disk) disk->pending = 2;
        riscv_interrupt_restore(irq);
        if (!disk) break;
        int result = kernel_vfs_unmount(&disk->mount);
        if (result) {
            if (!first) first = result;
            disk->pending = 3;
        }
    }
    /* 本轮失败 owner 只重置一次；不跨 I/O 借用 next 指针。 */
    uintptr_t irq = riscv_interrupt_save();
    for (struct disk_mount *disk = disk_mounts; disk; disk = disk->next)
        if (disk->pending == 3) disk->pending = 1;
    riscv_interrupt_restore(irq);
    return first;
}

int kernel_vfs_disk_create(struct kernel_heap *heap, uint64_t device_number,
    int read_only, const char *source, struct kernel_vfs_mount **owner)
{
    if (!heap || !owner || *owner || !source) return -KERNEL_EINVAL;
    struct kernel_block_device *block = kernel_block_lookup(device_number);
    if (!block) return -KERNEL_ENXIO;
    if (block->claim_owner) return -KERNEL_EBUSY;
    struct disk_mount *disk = 0;
    enum kernel_heap_status allocation = kernel_heap_allocate_zeroed(heap, 1,
        sizeof(*disk), (void **)&disk);
    if (allocation != KERNEL_HEAP_STATUS_OK)
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    disk->heap = heap;
    uintptr_t irq = riscv_interrupt_save();
    disk->next = disk_mounts;
    disk_mounts = disk;
    riscv_interrupt_restore(irq);
    allocation = kernel_heap_allocate(heap, strlen(source) + 1, (void **)&disk->source);
    if (allocation != KERNEL_HEAP_STATUS_OK) {
        release_disk(&disk->mount);
        return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    }
    memcpy(disk->source, source, strlen(source) + 1);
    disk->mount.source_name = disk->source;
    enum kernel_page_cache_status status = kernel_page_cache_init(&disk->cache,
        heap, heap->page_allocator);
    if (status != KERNEL_PAGE_CACHE_STATUS_OK) {
        release_disk(&disk->mount);
        return status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    }
    int result = kernel_vfs_mount_ext4(&disk->mount, block, heap, &disk->cache, read_only);
    /* 后端失败可能仍持有日志和 I/O；发布到清理注册表而非丢失真实 owner。 */
    disk->mount.release_owner = release_disk;
    if (!result) result = kernel_page_cache_start_worker(&disk->cache);
    if (!result) result = kernel_vfs_start_journal_worker(&disk->mount);
    if (result) {
        if (disk->mount.private_data) {
            int cleanup = kernel_vfs_unmount(&disk->mount);
            if (cleanup) disk->pending = 1;
        }
        else release_disk(&disk->mount);
        return result;
    }
    disk->pending = 0;
    *owner = &disk->mount;
    return 0;
}

void kernel_vfs_disk_defer_cleanup(struct kernel_vfs_mount *mount)
{
    if (mount && mount->release_owner == release_disk)
        ((struct disk_mount *)mount)->pending = 1;
}
