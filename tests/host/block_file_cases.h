/* Included by file_io_concurrency.c to use its real MM, heap and scheduler. */
#include "../../fs/block_file_internal.h"
static unsigned raw_pause_read, raw_read_copied, raw_resume_read, raw_aligned_done;
struct raw_test_disk {
    unsigned char bytes[4096];
    unsigned reads, writes, flushes, fail_read_at, fail_write_at;
    enum kernel_block_status fail_read, fail_write, fail_flush;
};
static enum kernel_block_status raw_test_read(void *context, uint64_t offset, void *buffer, size_t size)
{
    struct raw_test_disk *disk = context;
    assert(!(offset % 512) && !(size % 512)); disk->reads++;
    if (disk->fail_read && (!disk->fail_read_at || disk->reads == disk->fail_read_at))
        return disk->fail_read;
    memcpy(buffer, disk->bytes + offset, size);
    if (__atomic_exchange_n(&raw_pause_read, 0, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&raw_read_copied, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&raw_resume_read, __ATOMIC_ACQUIRE)) sched_yield();
    }
    return 0;
}
static enum kernel_block_status raw_test_write(void *context, uint64_t offset, const void *buffer, size_t size)
{
    struct raw_test_disk *disk = context;
    assert(!(offset % 512) && !(size % 512)); disk->writes++;
    if (disk->fail_write && (!disk->fail_write_at || disk->writes == disk->fail_write_at))
        return disk->fail_write;
    memcpy(disk->bytes + offset, buffer, size); return 0;
}
static enum kernel_block_status raw_test_flush(void *context)
{ struct raw_test_disk *disk = context; disk->flushes++; return disk->fail_flush; }
struct raw_race_call { struct kernel_block_device *device; unsigned mode; };
static int raw_phase_next(const void *context, const void **cursor, struct kernel_block_span *span)
{
    (void)context;
    if (!*cursor) return 0;
    *span = *(const struct kernel_block_span *)*cursor; *cursor = 0; return 1;
}
static void *raw_rmw_actor(void *argument)
{
    struct raw_race_call *call = argument; mm_test_attach(1);
    unsigned char sector[512], byte = 0x77;
    size_t done;
    assert(!kernel_block_write_bytes(call->device, 13, &byte, 1, sector, sizeof(sector), &done) && done == 1);
    mm_test_finish(); return 0;
}
static void *raw_aligned_actor(void *argument)
{
    struct raw_race_call *call = argument; mm_test_attach(2);
    unsigned char bytes[512]; memset(bytes, 0xa5, sizeof(bytes));
    struct kernel_block_span span = {0, bytes, sizeof(bytes)};
    enum kernel_block_status status = call->mode == 0 ?
        kernel_block_write_at(call->device, 0, bytes, sizeof(bytes)) : call->mode == 1 ?
        kernel_block_write_batch(call->device, &span, 1) :
        kernel_block_write_phase(call->device, raw_phase_next, 0, &span);
    assert(!status); __atomic_store_n(&raw_aligned_done, 1, __ATOMIC_RELEASE);
    mm_test_finish(); return 0;
}
static void raw_write_serialization(struct kernel_block_device *device, struct raw_test_disk *disk)
{
    for (unsigned mode = 0; mode < 3; mode++) {
        memset(disk->bytes, 0x3c, sizeof(disk->bytes));
        raw_read_copied = raw_resume_read = raw_aligned_done = 0; raw_pause_read = 1;
        struct raw_race_call call = {device, mode}; pthread_t rmw, aligned;
        assert(!pthread_create(&rmw, 0, raw_rmw_actor, &call));
        while (!__atomic_load_n(&raw_read_copied, __ATOMIC_ACQUIRE)) sched_yield();
        assert(!pthread_create(&aligned, 0, raw_aligned_actor, &call));
        /* Inspect the actual sleeping-lock waiter, rather than inferring
         * contention from thread creation or a ready message. */
        int waiting = 0;
        while (!waiting && !__atomic_load_n(&raw_aligned_done, __ATOMIC_ACQUIRE)) {
            KERNEL_RAW_SCOPE(guard, &device->io_lock.lock.metadata);
            waiting = device->io_lock.lock.pending_head != 0;
        }
        assert(waiting && !__atomic_load_n(&raw_aligned_done, __ATOMIC_ACQUIRE));
        __atomic_store_n(&raw_resume_read, 1, __ATOMIC_RELEASE);
        assert(!pthread_join(rmw, 0) && !pthread_join(aligned, 0));
        for (unsigned i = 0; i < 512; i++) assert(disk->bytes[i] == 0xa5);
        assert(!device->references && !device->open_count && !device->claim_owner);
    }
    mm_test_finish(); mm_test_attach(0); mm_test_activate_task();
    puts("PASS raw sector RMW vs scalar/batch/journal-phase writes: actual mutex waiter, complete data and request owners");
}
static void raw_kernel_prefix_case(struct kernel_open_file_description *file, struct raw_test_disk *disk,
    struct kernel_files *local)
{
    size_t done;
    uint64_t before_offset = kernel_open_file_offset(file);
    uint32_t before_references = file->block->references;
    unsigned char actual[1024];
    memcpy(disk->bytes + 509, "1234567", 7);
    memset(actual, 0xa5, sizeof(actual));
    disk->fail_read = KERNEL_BLOCK_STATUS_IO; disk->fail_read_at = disk->reads + 2;
    int error = kernel_block_file_pread(file, 509, actual, 7, &done);
    if (error != -KERNEL_EIO || done != 3)
        fprintf(stderr, "kernel block pread prefix: error=%d completed=%zu expected=-EIO/3\n", error, done);
    assert(error == -KERNEL_EIO && done == 3 && !memcmp(actual, "123", 3));
    for (unsigned i = 3; i < 7; i++) assert(actual[i] == 0xa5);
    disk->fail_read = 0; disk->fail_read_at = 0;
    for (unsigned fail_read = 0; fail_read < 2; fail_read++) {
        memset(disk->bytes, 0x3c, sizeof(disk->bytes));
        if (fail_read) { disk->fail_read = KERNEL_BLOCK_STATUS_IO; disk->fail_read_at = disk->reads + 2; }
        else { disk->fail_write = KERNEL_BLOCK_STATUS_IO; disk->fail_write_at = disk->writes + 2; }
        error = kernel_block_file_pwrite(file, 509, "abcdefg", 7, &done);
        assert(error == -KERNEL_EIO && done == 3 && !memcmp(disk->bytes + 509, "abc", 3));
        assert(disk->bytes[508] == 0x3c);
        for (unsigned i = 512; i < 516; i++) assert(disk->bytes[i] == 0x3c);
        disk->fail_read = disk->fail_write = 0; disk->fail_read_at = disk->fail_write_at = 0;
    }
    /* A failed multi-sector callback has no finer completion report. Preserve
     * its one large submission and report only the prefix confirmed before it. */
    memset(actual, 0xa5, sizeof(actual));
    disk->fail_read = KERNEL_BLOCK_STATUS_IO; disk->fail_read_at = disk->reads + 1;
    unsigned before = disk->reads;
    assert(kernel_block_file_pread(file, 512, actual, sizeof(actual), &done) == -KERNEL_EIO &&
        !done && disk->reads == before + 1);
    for (unsigned i = 0; i < sizeof(actual); i++) assert(actual[i] == 0xa5);
    disk->fail_read = 0; disk->fail_read_at = 0;
    disk->fail_write = KERNEL_BLOCK_STATUS_IO; disk->fail_write_at = disk->writes + 1;
    before = disk->writes;
    assert(kernel_block_file_pwrite(file, 512, actual, sizeof(actual), &done) == -KERNEL_EIO &&
        !done && disk->writes == before + 1);
    disk->fail_write = 0; disk->fail_write_at = 0;
    assert(!kernel_block_file_pwrite(file, 509, "abcdefg", 7, &done) && done == 7);
    assert(!kernel_block_file_pread(file, 509, actual, 7, &done) && done == 7 && !memcmp(actual, "abcdefg", 7));
    memset(actual, 0xa5, 7);
    assert(!kernel_copy_to_user(&mm, 0x200600, actual, 7, &done) && done == 7);
    disk->fail_read = KERNEL_BLOCK_STATUS_IO; disk->fail_read_at = disk->reads + 2;
    int64_t result;
    assert(!kernel_files_pread(local, &mm, 0, 0x200600, 7, 509, &result) && result == 3);
    assert(!kernel_copy_from_user(&mm, actual, 0x200600, 7, &done) && done == 7 && !memcmp(actual, "abc", 3));
    for (unsigned i = 3; i < 7; i++) assert(actual[i] == 0xa5);
    disk->fail_read = 0; disk->fail_read_at = 0;
    memset(disk->bytes, 0x3c, sizeof(disk->bytes));
    assert(!kernel_copy_to_user(&mm, 0x200600, "abcdefg", 7, &done) && done == 7);
    disk->fail_write = KERNEL_BLOCK_STATUS_IO; disk->fail_write_at = disk->writes + 2;
    assert(!kernel_files_pwrite(local, &mm, 0, 0x200600, 7, 509, &result) && result == 3 &&
        !memcmp(disk->bytes + 509, "abc", 3));
    for (unsigned i = 512; i < 516; i++) assert(disk->bytes[i] == 0x3c);
    disk->fail_write = 0; disk->fail_write_at = 0;
    assert(kernel_open_file_offset(file) == before_offset && file->block->references == before_references);
    puts("PASS kernel/user block byte prefixes: second-sector read/write/RMW errors retain confirmed 3 bytes; aligned callbacks remain batched");
}
static void raw_block_case(struct kernel_heap *heap, struct kernel_vfs_mount *mount)
{
    struct raw_test_disk disk = {0};
    memset(disk.bytes, 0x3c, sizeof(disk.bytes));
    struct kernel_block_device device = {.context = &disk, .read = raw_test_read,
        .write = raw_test_write, .flush = raw_test_flush, .capacity_bytes = sizeof(disk.bytes),
        .logical_block_size = 512, .cache_mode = KERNEL_BLOCK_CACHE_WRITEBACK};
    const uint64_t number = KERNEL_BLOCK_DEVICE_NUMBER(100);
    assert(!kernel_block_register(&device, number));
    raw_write_serialization(&device, &disk);
    memset(disk.bytes, 0x3c, sizeof(disk.bytes));
    struct kernel_fs_context local_fs = {0}; struct kernel_files local = {0}, child = {0};
    assert(!kernel_fs_context_create(&local_fs, mount, heap) && !kernel_files_create(&local, heap));
    KERNEL_FS_PATHS(paths, &local_fs);
    struct kernel_vfs_path *root = paths.root;
    assert(!kernel_vfs_mknod_at(root, root, "/raw-block", KERNEL_VFS_S_IFBLK | 0600, number));
    size_t done; int64_t result;
    struct kernel_open_file_description *node = 0; int node_error;
    struct kernel_vfs_stat node_stat;
    assert(!kernel_open_file_create(heap, mount, "/raw-block", &node, &node_error) && !node_error);
    assert(!kernel_vfs_fstat(&node->file, &node_stat) && node_stat.rdev == number &&
        !node_stat.size && !node_stat.blocks && !kernel_vfs_file_memory(&node->file));
    assert(!kernel_open_file_release(&node));
    assert(!kernel_vfs_mknod_at(root, root, "/raw-char", KERNEL_VFS_S_IFCHR | 0600, 0x103));
    assert(!kernel_open_file_create(heap, mount, "/raw-char", &node, &node_error) && !node_error);
    assert(!kernel_vfs_fstat(&node->file, &node_stat) && node_stat.rdev == 0x103 &&
        !node_stat.size && !node_stat.blocks && !kernel_vfs_file_memory(&node->file));
    assert(!kernel_open_file_release(&node));
    assert(!kernel_copy_to_user(&mm, 0x200000, "/raw-block", 11, &done) && done == 11);
    assert(!kernel_files_openat(&local, &local_fs, &mm, -100, 0x200000, 2, 0, &result) && result == 0);
    assert(device.open_count == 1 && device.references == 1);
    assert(!kernel_files_dup(&local, 0, &result) && result == 1);
    assert(!kernel_files_fork(&child, &local) && device.open_count == 1 && device.references == 1);
    assert(kernel_block_unregister(&device) == -KERNEL_EBUSY);
    assert(!kernel_copy_to_user(&mm, 0x200200, "abcdefg", 7, &done) && done == 7);
    assert(!kernel_files_lseek(&local, 0, 509, 0, &result) && result == 509);
    assert(!kernel_files_write(&local, &mm, 1, 0x200200, 7, &result) && result == 7);
    assert(!memcmp(disk.bytes + 509, "abcdefg", 7) && disk.bytes[508] == 0x3c && disk.bytes[516] == 0x3c);
    assert(!kernel_files_lseek(&child, 0, 0, 1, &result) && result == 516);
    assert(!kernel_files_pread(&local, &mm, 0, 0x200300, 7, 509, &result) && result == 7);
    char actual[16]; assert(!kernel_copy_from_user(&mm, actual, 0x200300, 7, &done) && !memcmp(actual, "abcdefg", 7));
    assert(!kernel_files_lseek(&local, 0, 0, 1, &result) && result == 516);
    struct kernel_uaccess_iovec iov[2] = {{0x200200, 3}, {0x200203, 4}};
    assert(!kernel_copy_to_user(&mm, 0x200400, iov, sizeof(iov), &done));
    assert(!kernel_files_writev(&local, &mm, 0, 0x200400, 2, &result) && result == 7);
    assert(!kernel_files_lseek(&local, 0, 516, 0, &result) && result == 516);
    assert(!kernel_files_readv(&local, &mm, 1, 0x200400, 2, &result) && result == 7);
    assert(!kernel_files_pwrite(&local, &mm, 0, 0x200200, 7, 4093, &result) && result == 3);
    assert(!kernel_files_pwrite(&local, &mm, 0, 0x200200, 1, 4096, &result) && result == -KERNEL_ENOSPC);
    assert(!kernel_files_pread(&local, &mm, 0, 0x200200, 1, 4096, &result) && result == 0);
    assert(!kernel_files_lseek(&local, 0, 4097, 0, &result) && result == -KERNEL_EINVAL);
    const uint64_t commands[] = {0x1260,0x80081272,0x1268,0x125e,0x80081270};
    const uint64_t values[] = {8,4096,512,0,512};
    for (unsigned i = 0; i < 5; i++) {
        uint64_t value = 0; assert(!kernel_copy_to_user(&mm, 0x200500, &value, 8, &done));
        assert(!kernel_files_ioctl(&local, &mm, 0, commands[i], 0x200500, &result) && result == 0);
        assert(!kernel_copy_from_user(&mm, &value, 0x200500, 8, &done) && value == values[i]);
    }
    assert(!kernel_files_ioctl(&local, &mm, 0, 0x1268, ARCH_MMU_USER_LIMIT + 1, &result) && result == -KERNEL_EFAULT);
    unsigned value = 1024; assert(!kernel_copy_to_user(&mm, 0x200500, &value, 4, &done));
    assert(!kernel_files_ioctl(&local, &mm, 0, 0x40081271, 0x200500, &result) && result == 0 && device.soft_block_size == 1024);
    assert(!kernel_files_sync(&local, 0, 0, &result) && result == 0);
    disk.fail_flush = KERNEL_BLOCK_STATUS_IO;
    assert(!kernel_files_sync(&local, 0, 1, &result) && result == -KERNEL_EIO);
    disk.fail_flush = 0;
    assert(!kernel_files_ioctl(&local, &mm, 0, 0x1261, 0, &result) && result == 0);
    value = 1; assert(!kernel_copy_to_user(&mm, 0x200500, &value, 4, &done));
    assert(!kernel_files_ioctl(&local, &mm, 0, 0x125d, 0x200500, &result) && result == 0);
    assert(!kernel_files_write(&local, &mm, 0, 0x200200, 1, &result) && result == -KERNEL_EPERM);
    const unsigned writer_modes[] = {2, 1};
    for (unsigned i = 0; i < sizeof(writer_modes) / sizeof(writer_modes[0]); i++) {
        assert(!kernel_files_openat(&local, &local_fs, &mm, -100, 0x200000, writer_modes[i], 0, &result));
        if (result != 2) fprintf(stderr, "read-only block open: mode=%u result=%lld expected=2\n",
            writer_modes[i], (long long)result);
        assert(result == 2);
        assert(!kernel_files_write(&local, &mm, 2, 0x200200, 1, &result) && result == -KERNEL_EPERM);
        assert(!kernel_files_pwrite(&local, &mm, 2, 0x200200, 1, 0, &result) && result == -KERNEL_EPERM);
        assert(!kernel_files_close(&local, 2, &result) && !result);
    }
    value = 0; assert(!kernel_copy_to_user(&mm, 0x200500, &value, 4, &done));
    assert(!kernel_files_ioctl(&local, &mm, 0, 0x125d, 0x200500, &result) && result == 0);
    int mount_owner;
    assert(!kernel_block_claim(&device, &mount_owner));
    assert(!kernel_files_openat(&local, &local_fs, &mm, -100, 0x200000, 2, 0, &result) && result == 2);
    assert(!kernel_files_openat(&local, &local_fs, &mm, -100, 0x200000, 2 | 0200, 0, &result) && result == -KERNEL_EBUSY);
    kernel_block_release_claim(&device, &mount_owner);
    assert(!kernel_files_openat(&local, &local_fs, &mm, -100, 0x200000, 2 | 0200, 0, &result) && result == 3);
    assert(kernel_block_claim(&device, &mount_owner) == -KERNEL_EBUSY);
    assert(!kernel_files_openat(&local, &local_fs, &mm, -100, 0x200000, 0, 0, &result) && result == 4);
    assert(!kernel_files_write(&local, &mm, 4, 0x200200, 1, &result) && result == -KERNEL_EBADF);
    assert(!kernel_files_pread(&local, &mm, 0, 0x200200, 1, -1, &result) && result == -KERNEL_EINVAL);
    assert(!kernel_files_ioctl(&local, &mm, 0, 0x1277, 0x200500, &result) && result == -KERNEL_EOPNOTSUPP);
    assert(!kernel_files_ioctl(&local, &mm, 0, 0x9999, 0, &result) && result == -KERNEL_ENOTTY);
    assert(!kernel_files_openat(&local, &local_fs, &mm, -100, 0x200000, 00040000, 0, &result) && result == -KERNEL_ENOTSUP);
    assert(!kernel_files_openat(&local, &local_fs, &mm, -100, 0x200000, 2 | KERNEL_FILES_O_SYNC, 0, &result) && result == 5);
    unsigned before_flush = disk.flushes;
    assert(!kernel_files_pwrite(&local, &mm, 5, 0x200200, 1, 0, &result) && result == 1 && disk.flushes == before_flush + 1);
    disk.fail_flush = KERNEL_BLOCK_STATUS_IO;
    assert(!kernel_files_write(&local, &mm, 5, 0x200200, 1, &result) && result == -KERNEL_EIO);
    assert(!kernel_files_lseek(&local, 5, 0, 1, &result) && result == 1);
    disk.fail_flush = 0;
    disk.fail_read = KERNEL_BLOCK_STATUS_IO;
    assert(!kernel_files_pread(&local, &mm, 0, 0x200200, 1, 0, &result) && result == -KERNEL_EIO);
    disk.fail_read = 0; disk.fail_write = KERNEL_BLOCK_STATUS_IO;
    assert(!kernel_files_pwrite(&local, &mm, 0, 0x200200, 1, 0, &result) && result == -KERNEL_EIO);
    disk.fail_write = 0;
    uint64_t fault_edge = 0x100000 + 2 * BOAROS_PAGE_SIZE - 3;
    assert(!kernel_copy_to_user(&mm, fault_edge, "xyz", 3, &done) && done == 3);
    assert(!kernel_files_pwrite(&local, &mm, 0, fault_edge, 7, 31, &result) && result == 3);
    assert(!memcmp(disk.bytes + 31, "xyz", 3));
    assert(!kernel_files_pread(&local, &mm, 0, fault_edge, 7, 31, &result) && result == 3);
    struct kernel_open_file_description *mapping = 0;
    assert(!kernel_files_pin_data(&local, 0, &mapping, &result) && !result);
    raw_kernel_prefix_case(mapping, &disk, &local);
    uint64_t mapped;
    assert(kernel_mm_mmap_file_private(&mm, &mapping, 0, BOAROS_PAGE_SIZE, 0,
        KERNEL_MM_READ, 0, &mapped) == KERNEL_MM_STATUS_INVALID_ARGUMENT && mapping);
    assert(!kernel_open_file_release(&mapping));
    assert(!kernel_files_release(&child) && !kernel_files_release(&local));
    assert(!device.references && !device.open_count && !device.claim_owner);
    assert(!kernel_block_unregister(&device));
    assert(!kernel_fs_context_release(&local_fs));
    mm_test_finish(); mm_test_attach(0); mm_test_activate_task();
    puts("PASS raw block OFD: sector RMW, vector/positioned I/O, shared dup/fork offsets, EOF, ioctl, RO, flush errors and claim owners");
}

static int raw_image_contains(struct kernel_block_device *device, const unsigned char marker[32])
{
    unsigned char bytes[4096];
    for (uint64_t offset = 0; offset < device->capacity_bytes; offset += sizeof(bytes)) {
        size_t count = device->capacity_bytes - offset < sizeof(bytes) ?
            device->capacity_bytes - offset : sizeof(bytes);
        assert(pread(image, bytes, count, offset) == (ssize_t)count);
        for (size_t i = 0; i + 32 <= count; i++)
            if (!memcmp(bytes + i, marker, 32)) return 1;
    }
    return 0;
}
static void raw_mounted_flush_case(struct kernel_heap *heap, struct kernel_vfs_mount *mount,
    struct kernel_block_device *device)
{
    struct kernel_fs_context local_fs = {0}; struct kernel_files local = {0};
    assert(!kernel_fs_context_create(&local_fs, mount, heap) && !kernel_files_create(&local, heap));
    KERNEL_FS_PATHS(paths, &local_fs);
    struct kernel_vfs_path *root = paths.root;
    assert(!kernel_vfs_mknod_at(root, root, "/mounted-block", KERNEL_VFS_S_IFBLK | 0600, device->device_number));
    size_t done; int64_t result; int error;
    assert(!kernel_copy_to_user(&mm, 0x200000, "/mounted-block", 15, &done) && done == 15);
    assert(!kernel_files_openat(&local, &local_fs, &mm, -100, 0x200000, 2, 0, &result) && result == 0);
    struct kernel_open_file_description *file = NULL;
    assert(!kernel_open_file_create_mode(heap, mount, "/flsbuf-data", 0600, &file, &error) && !error);
    file->open_flags = 2; assert(!kernel_vfs_file_acquire_write(&file->file));
    const unsigned char marker[32] = {0xbd,0xef,0x17,0x45,0x91,0x22,0xa9,0xc5,
        0x37,0x62,0x13,0x43,0x81,0x54,0xe7,0xac,0xde,0xb2,0x47,0x58,0xf9,0x32,
        0x64,0x1d,0x7f,0xc8,0x42,0x15,0x3a,0x72,0x89,0x6d};
    assert(!kernel_vfs_pwrite(&file->file, 0, marker, sizeof(marker), &done) && done == sizeof(marker));
    assert(!raw_image_contains(device, marker));
    assert(!kernel_files_ioctl(&local, &mm, 0, 0x1261, 0, &result) && result == 0);
    assert(raw_image_contains(device, marker));
    assert(!kernel_open_file_release(&file));
    assert(!kernel_files_release(&local) && !kernel_fs_context_release(&local_fs));
    mm_test_finish(); mm_test_attach(0); mm_test_activate_task();
    puts("PASS BLKFLSBUF mounted holder: previously dirty file bytes reach the backing image before unmount");
}
