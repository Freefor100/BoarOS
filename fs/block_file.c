#include "block_file_internal.h"
#include "open_file_internal.h"
#include "uaccess_iov_internal.h"
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/uaccess.h>

static int block_error(enum kernel_block_status status)
{
    switch (status) {
    case KERNEL_BLOCK_STATUS_OK: return 0;
    case KERNEL_BLOCK_STATUS_NO_MEMORY: return -KERNEL_ENOMEM;
    case KERNEL_BLOCK_STATUS_OUT_OF_RANGE: return -KERNEL_ENOSPC;
    case KERNEL_BLOCK_STATUS_UNSUPPORTED: return -KERNEL_EOPNOTSUPP;
    case KERNEL_BLOCK_STATUS_READ_ONLY: return -KERNEL_EPERM;
    case KERNEL_BLOCK_STATUS_INVALID: return -KERNEL_EINVAL;
    default: return -KERNEL_EIO;
    }
}
int kernel_block_file_open(struct kernel_open_file_description *file, uint64_t number, uint32_t flags)
{
    struct kernel_block_device *device = kernel_block_get(number);
    if (!device) return -KERNEL_ENXIO;
    int error = kernel_block_open(device, file, flags);
    if (!error) { file->block = device; file->kind = KERNEL_OPEN_FILE_KIND_BLOCK; }
    kernel_block_put(device);
    return error;
}
void kernel_block_file_close(struct kernel_open_file_description *file)
{
    if (!file->block) return;
    struct kernel_block_device *device = file->block;
    file->block = 0;
    kernel_block_close(device, file, file->open_flags);
}
int kernel_block_file_sync(struct kernel_open_file_description *file)
{ KERNEL_SERVICE_SCOPE(service); return block_error(kernel_block_flush(file->block)); }

/* The OFD owns the device. Each backend request borrows only kernel memory;
 * user faults cannot leave DMA pointed at a user page or a freed staging area. */
enum kernel_files_status kernel_block_file_io(struct kernel_open_file_description *file,
    struct kernel_mm *mm, const struct kernel_uaccess_iovec *iov, size_t iov_count,
    uint64_t count, int writing, int positioned, uint64_t offset, int64_t *result)
{
    KERNEL_SERVICE_SCOPE(service);
    KERNEL_LOCK_SCOPE(position);
    if (!positioned) kernel_mutex_lock(&file->offset_lock, &position);
    if ((writing && !kernel_open_file_writable(file)) ||
        (!writing && !kernel_open_file_readable(file))) { *result = -KERNEL_EBADF; return 0; }
    for (size_t i = 0; i < iov_count; i++)
        if (kernel_user_range_check(iov[i].base, iov[i].length) ||
            (!iov[i].length && iov[i].base && kernel_user_range_check(iov[i].base - 1, 1))) {
            *result = -KERNEL_EFAULT; return 0;
        }
    if (!count) { *result = 0; return 0; }
    struct kernel_block_device *device = file->block;
    if (writing && kernel_block_read_only(device)) { *result = -KERNEL_EPERM; return 0; }
    if (!positioned) offset = file->offset;
    uint64_t capacity = __atomic_load_n(&device->capacity_bytes, __ATOMIC_ACQUIRE);
    if (offset >= capacity) { *result = writing ? -KERNEL_ENOSPC : 0; return 0; }
    if (count > capacity - offset) count = capacity - offset;
    if (count > KERNEL_FILES_MAX_RW_COUNT) count = KERNEL_FILES_MAX_RW_COUNT;
    size_t sector = device->logical_block_size;
    if (!sector || sector > SIZE_MAX - BOAROS_PAGE_SIZE) { *result = -KERNEL_EINVAL; return 0; }
    unsigned char *buffer = 0;
    enum kernel_heap_status allocation = kernel_heap_allocate(file->heap,
        BOAROS_PAGE_SIZE + sector, (void **)&buffer);
    if (allocation) {
        if (allocation != KERNEL_HEAP_STATUS_EMPTY) return KERNEL_FILES_STATUS_STATE;
        *result = -KERNEL_ENOMEM; return 0;
    }
    struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0, 0};
    uint64_t total = 0;
    int error = 0;
    while (total < count) {
        size_t amount = count - total < BOAROS_PAGE_SIZE ? count - total : BOAROS_PAGE_SIZE;
        /* A backend error reports no accepted bytes for this sector; use a
         * sector boundary so earlier successful sectors remain observable. */
        size_t boundary = sector - (offset + total) % sector;
        if (amount > boundary) amount = boundary;
        size_t copied = 0, completed = 0;
        enum kernel_uaccess_status access = KERNEL_UACCESS_STATUS_OK;
        if (writing) {
            access = kernel_copy_from_user_iov(mm, &cursor, buffer, amount, &copied);
            if (copied) error = block_error(kernel_block_write_bytes(device, offset + total,
                buffer, copied, buffer + BOAROS_PAGE_SIZE, sector, &completed));
            total += completed;
        } else {
            error = block_error(kernel_block_read_bytes(device, offset + total, buffer,
                amount, buffer + BOAROS_PAGE_SIZE, sector, &completed));
            if (completed) {
                access = kernel_copy_to_user_iov(mm, &cursor, buffer, completed, &copied);
                total += copied;
            }
        }
        if (error) break;
        if (access == KERNEL_UACCESS_STATUS_FAULT) { error = -KERNEL_EFAULT; break; }
        if (access || copied != amount) { kernel_heap_release(file->heap, buffer); return KERNEL_FILES_STATUS_STATE; }
    }
    kernel_heap_release(file->heap, buffer);
    if (!positioned) file->offset += total;
    *result = total ? (int64_t)total : error;
    if (writing && total && (kernel_open_file_flags(file) & KERNEL_FILES_O_DSYNC)) {
        error = kernel_block_file_sync(file);
        if (error) *result = error;
    }
    return KERNEL_FILES_STATUS_OK;
}
int kernel_block_file_pread(struct kernel_open_file_description *file, uint64_t offset,
    void *buffer, size_t size, size_t *completed)
{
    *completed = 0;
    uint64_t capacity = __atomic_load_n(&file->block->capacity_bytes, __ATOMIC_ACQUIRE);
    if (offset >= capacity) return 0;
    if (size > capacity - offset) size = capacity - offset;
    if (!size) return 0;
    void *scratch = 0;
    size_t sector = file->block->logical_block_size;
    enum kernel_heap_status allocation = kernel_heap_allocate(file->heap, sector, &scratch);
    if (allocation) return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    int error = block_error(kernel_block_read_bytes(file->block, offset, buffer, size, scratch, sector, completed));
    kernel_heap_release(file->heap, scratch);
    return error;
}
int kernel_block_file_pwrite(struct kernel_open_file_description *file, uint64_t offset,
    const void *buffer, size_t size, size_t *completed)
{
    *completed = 0;
    if (!kernel_open_file_writable(file)) return -KERNEL_EBADF;
    uint64_t capacity = __atomic_load_n(&file->block->capacity_bytes, __ATOMIC_ACQUIRE);
    if (!size) return 0;
    if (offset >= capacity) return -KERNEL_ENOSPC;
    if (size > capacity - offset) size = capacity - offset;
    void *scratch = 0;
    size_t sector = file->block->logical_block_size;
    enum kernel_heap_status allocation = kernel_heap_allocate(file->heap, sector, &scratch);
    if (allocation) return allocation == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    int error = block_error(kernel_block_write_bytes(file->block, offset, buffer, size, scratch, sector, completed));
    kernel_heap_release(file->heap, scratch);
    return error;
}
static int transfer(struct kernel_mm *mm, uint64_t argument, void *data, size_t size, int from_user)
{
    size_t copied = 0;
    enum kernel_uaccess_status status = from_user ?
        kernel_copy_from_user(mm, data, argument, size, &copied) :
        kernel_copy_to_user(mm, argument, data, size, &copied);
    return status || copied != size ? -KERNEL_EFAULT : 0;
}
int64_t kernel_block_file_ioctl(struct kernel_files *files, struct kernel_open_file_description **owner,
    struct kernel_mm *mm, uint64_t command, uint64_t argument)
{
    struct kernel_open_file_description *file = *owner;
    struct kernel_block_device *device = file->block;
    uint64_t value = 0;
    uint32_t integer = 0;
    switch (command) {
    case 0x1260: /* BLKGETSIZE: native unsigned long, in 512-byte sectors. */
        value = __atomic_load_n(&device->capacity_bytes, __ATOMIC_ACQUIRE) / 512;
        return transfer(mm, argument, &value, sizeof(value), 0);
    case 0x80081272: /* BLKGETSIZE64 */
        value = __atomic_load_n(&device->capacity_bytes, __ATOMIC_ACQUIRE);
        return transfer(mm, argument, &value, sizeof(value), 0);
    case 0x1268: integer = device->logical_block_size; break; /* BLKSSZGET */
    case 0x125e: integer = kernel_block_read_only(device); break; /* BLKROGET */
    case 0x80081270: integer = __atomic_load_n(&device->soft_block_size, __ATOMIC_ACQUIRE); break;
    case 0x125d: /* BLKROSET; BoarOS currently has one privileged user identity. */
        if (transfer(mm, argument, &integer, sizeof(integer), 1)) return -KERNEL_EFAULT;
        return kernel_block_set_read_only(device, integer != 0);
    case 0x40081271: { /* BLKBSZSET changes the shared software size. */
        if (transfer(mm, argument, &integer, sizeof(integer), 1)) return -KERNEL_EFAULT;
        if (integer < 512 || integer > BOAROS_PAGE_SIZE || (integer & (integer - 1)) ||
            integer < device->logical_block_size) return -KERNEL_EINVAL;
        int claim = !(file->open_flags & 0200U);
        int error = claim ? kernel_block_claim(device, file) : 0;
        if (error) return error;
        error = kernel_block_file_sync(file);
        if (!error) __atomic_store_n(&device->soft_block_size, integer, __ATOMIC_RELEASE);
        if (claim) kernel_block_release_claim(device, file);
        return error;
    }
    case 0x1261: { /* BLKFLSBUF: drain the mounted holder before raw flush. */
        int error = kernel_vfs_sync_block_device(device);
        return error ? error : kernel_block_file_sync(file);
    }
    case 0x1277: case 0x127d: case 0x127f: /* DISCARD, SECURE_DISCARD, ZEROOUT */
        return -KERNEL_EOPNOTSUPP;
    default:
        return device->ioctl ? device->ioctl(device, files, owner, mm, command, argument) : -KERNEL_ENOTTY;
    }
    return transfer(mm, argument, &integer, sizeof(integer), 0);
}
