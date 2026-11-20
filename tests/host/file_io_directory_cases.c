/* Included by the file I/O fixture so faults run through production MM/VFS. */
#include <ext4_dir.h>

static _Thread_local size_t directory_copy_limit = SIZE_MAX;
static _Thread_local unsigned directory_iterator_steps, directory_iterator_fail;
static unsigned directory_copy_pause, directory_copy_entered, directory_copy_resume;

enum kernel_uaccess_status __real_kernel_copy_to_user(struct kernel_mm *, uint64_t,
    const void *, size_t, size_t *);
enum kernel_uaccess_status __wrap_kernel_copy_to_user(struct kernel_mm *space,
    uint64_t address, const void *data, size_t size, size_t *copied)
{
    if (__atomic_exchange_n(&directory_copy_pause, 0, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&directory_copy_entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&directory_copy_resume, __ATOMIC_ACQUIRE)) sched_yield();
    }
    if (directory_copy_limit < size) {
        size_t prefix = directory_copy_limit;
        directory_copy_limit = SIZE_MAX;
        if (prefix) assert(!__real_kernel_copy_to_user(space, address, data, prefix, copied));
        else *copied = 0;
        return KERNEL_UACCESS_STATUS_FAULT;
    }
    return __real_kernel_copy_to_user(space, address, data, size, copied);
}
int __real_ext4_dir_iterator_next_raw(struct ext4_dir_iter *);
int __wrap_ext4_dir_iterator_next_raw(struct ext4_dir_iter *iterator)
{
    if (directory_iterator_fail && ext4_dir_en_get_inode(iterator->curr) &&
        ++directory_iterator_steps == directory_iterator_fail) return 5;
    return __real_ext4_dir_iterator_next_raw(iterator);
}
static uint64_t directory_number(const unsigned char *bytes, unsigned length)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < length; i++) value |= (uint64_t)bytes[i] << (8U * i);
    return value;
}
static size_t directory_complete(const unsigned char *bytes, size_t size, uint64_t *cookie)
{
    size_t complete = 0;
    while (complete + 19 <= size) {
        size_t length = directory_number(bytes + complete + 16, 2);
        assert(length >= 24 && length <= 280 && !(length & 7));
        if (length > size - complete) break;
        *cookie = directory_number(bytes + complete + 8, 8);
        complete += length;
    }
    return complete;
}
static void directory_rewind(struct kernel_files *local)
{
    int64_t result;
    assert(!kernel_files_lseek(local, 0, 0, 0, &result) && !result);
}
struct directory_reader {
    struct kernel_files *files;
    unsigned actor;
    uint64_t address, count;
    int64_t result;
};
static void *directory_read_thread(void *argument)
{
    struct directory_reader *reader = argument;
    mm_test_attach(reader->actor); mm_test_bind(&mm); mm_test_context = context;
    assert(!kernel_files_getdents(reader->files, &mm, 0, reader->address, reader->count, &reader->result));
    mm_test_finish();
    return NULL;
}
static void directory_batch_cases(struct kernel_heap *heap, struct kernel_vfs_mount *mount, int ext4)
{
    assert(!kernel_vfs_mkdir(mount, "/batch-directory", 0700));
    for (unsigned i = 0; i < 300; i++) {
        char path[300];
        int prefix = snprintf(path, sizeof(path), "/batch-directory/%03u-", i);
        unsigned length = i == 151 ? 255 : 40;
        memset(path + prefix, 'a' + i % 26, length - 4);
        path[prefix + length - 4] = 0;
        struct kernel_open_file_description *entry = NULL; int error;
        assert(!kernel_open_file_create_mode(heap, mount, path, 0600, &entry, &error) && !error);
        assert(!kernel_open_file_release(&entry));
    }
    struct kernel_files local = {0};
    struct kernel_open_file_description *directory = NULL; int error; int64_t result;
    assert(!kernel_open_file_create(heap, mount, "/batch-directory", &directory, &error) && !error);
    directory->kind = KERNEL_OPEN_FILE_KIND_DIRECTORY;
    struct kernel_open_file_description *borrowed = directory;
    assert(!kernel_files_create(&local, heap) && !kernel_files_install_new_owned_at(&local, 0, 0, &directory));
    const uint64_t address = 0x900000;
    uint64_t mapped;
    assert(!kernel_mm_mmap_anonymous(&mm, address, 8 * BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED_NOREPLACE, &mapped));
    unsigned char expected[32768], actual[32768]; size_t copied;
    assert(!kernel_files_getdents(&local, &mm, 0, address, sizeof(expected), &result) && result > (int64_t)BOAROS_PAGE_SIZE);
    size_t size = (size_t)result;
    assert(!kernel_copy_from_user(&mm, expected, address, size, &copied) && copied == size);
    uint64_t cookie = 0;
    assert(directory_complete(expected, size, &cookie) == size);
    unsigned entries = 0, seen[300] = {0};
    for (size_t at = 0; at < size; at += directory_number(expected + at + 16, 2)) {
        const char *name = (const char *)expected + at + 19;
        if (entries < 2) assert(!strcmp(name, entries ? ".." : "."));
        else {
            char *end; unsigned long index = strtoul(name, &end, 10);
            assert(index < 300 && *end == '-' && !seen[index]++);
            assert(strlen(name) == (index == 151 ? 255U : 40U));
        }
        entries++;
    }
    assert(entries == 302);
    uint64_t eof_cookie = borrowed->offset;
    for (unsigned i = 0; i < 3; i++) {
        assert(!kernel_files_getdents(&local, &mm, 0, address, 0, &result) && !result);
        assert(!kernel_files_lseek(&local, 0, 0, 1, &result) && result == (int64_t)eof_cookie);
    }
    directory_rewind(&local);
    directory_scratch_fail = 1;
    assert(!kernel_files_getdents(&local, &mm, 0, address, sizeof(expected), &result) && result == (int64_t)size);
    directory_scratch_fail = 0;
    assert(!kernel_copy_from_user(&mm, actual, address, size, &copied) && !memcmp(actual, expected, size));
    const size_t limits[] = {0, 1, 23, 24, 25, 47, 48, 71, 280, 4095, 4096};
    for (unsigned i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
        directory_rewind(&local);
        assert(!kernel_files_getdents(&local, &mm, 0, address, limits[i], &result));
        uint64_t accepted_cookie = 0;
        size_t complete = directory_complete(expected, limits[i], &accepted_cookie);
        assert(result == (complete ? (int64_t)complete : -KERNEL_EINVAL));
        assert(!kernel_files_lseek(&local, 0, 0, 1, &result) && result == (int64_t)accepted_cookie);
        assert(!kernel_files_getdents(&local, &mm, 0, address, sizeof(expected), &result) && result == (int64_t)(size - complete));
        assert(!kernel_copy_from_user(&mm, actual, address, result, &copied) && copied == (size_t)result);
        assert(!memcmp(actual, expected + complete, copied));
    }
    for (unsigned i = 0; i < 9; i++) {
        directory_rewind(&local);
        directory_copy_limit = limits[i];
        assert(!kernel_files_getdents(&local, &mm, 0, address, 512, &result));
        directory_copy_limit = SIZE_MAX;
        uint64_t accepted_cookie = 0;
        size_t complete = directory_complete(expected, limits[i], &accepted_cookie);
        assert(result == (complete ? (int64_t)complete : -KERNEL_EFAULT));
        assert(!kernel_files_lseek(&local, 0, 0, 1, &result) && result == (int64_t)accepted_cookie);
    }
    /* Real unmapped boundary, with a complete prefix and part of its successor. */
    directory_rewind(&local);
    uint64_t edge = address + 8 * BOAROS_PAGE_SIZE - 25;
    assert(!kernel_files_getdents(&local, &mm, 0, edge, 512, &result) && result == 24);
    assert(!kernel_files_getdents(&local, &mm, 0, address, 24, &result) && result == 24);
    assert(!kernel_copy_from_user(&mm, actual, address, 24, &copied) && !memcmp(actual, expected + 24, 24));
    if (ext4) {
        directory_rewind(&local);
        size_t prefix = 0;
        for (unsigned i = 0; i < 3; i++)
            prefix += directory_number(expected + prefix + 16, 2);
        directory_iterator_steps = 0; directory_iterator_fail = 3;
        assert(!kernel_files_getdents(&local, &mm, 0, address, 512, &result) && result == (int64_t)prefix);
        directory_iterator_fail = 0;
        assert(!kernel_files_getdents(&local, &mm, 0, address, sizeof(expected), &result) && result == (int64_t)(size - prefix));
        assert(!kernel_copy_from_user(&mm, actual, address, result, &copied) && !memcmp(actual, expected + prefix, copied));
    }
    /* Pause at the actual batch publication to user memory. Namespace writers
     * can progress, but a shared-OFD reader must wait for the offset commit. */
    directory_rewind(&local);
    struct kernel_files child = {0}; assert(!kernel_files_fork(&child, &local));
    directory_copy_entered = directory_copy_resume = directory_offset_waiting = 0;
    directory_offset_queue = &borrowed->offset_lock.lock.waiters;
    directory_copy_pause = 1;
    struct directory_reader first = {&local, 1, address, 24, 0};
    struct directory_reader second = {&child, 2, address + 512, 24, 0};
    pthread_t readers[2];
    assert(!pthread_create(&readers[0], NULL, directory_read_thread, &first));
    while (!__atomic_load_n(&directory_copy_entered, __ATOMIC_ACQUIRE)) sched_yield();
    assert(!pthread_create(&readers[1], NULL, directory_read_thread, &second));
    while (!__atomic_load_n(&directory_offset_waiting, __ATOMIC_ACQUIRE)) sched_yield();
    assert(!kernel_vfs_mkdir(mount, "/batch-directory/concurrent", 0700));
    __atomic_store_n(&directory_copy_resume, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(readers[0], NULL) && !pthread_join(readers[1], NULL));
    directory_offset_queue = NULL;
    assert(first.result == 24 && second.result == 24);
    assert(!kernel_copy_from_user(&mm, actual, address, 24, &copied) && !memcmp(actual, expected, 24));
    assert(!kernel_copy_from_user(&mm, actual, address + 512, 24, &copied) && !memcmp(actual, expected + 24, 24));
    assert(!kernel_files_release(&child) && !kernel_files_release(&local));
    assert(!kernel_mm_munmap(&mm, address, 8 * BOAROS_PAGE_SIZE));
    printf("PASS %s directory batches: 302 entries, long name, boundaries/cookies, partial faults, EOF, shared OFD and concurrent namespace publication\n", ext4 ? "ext4" : "tmpfs");
}
