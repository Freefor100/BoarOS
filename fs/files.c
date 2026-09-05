#include "open_file_internal.h"

#include <kernel/console.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/physical_page.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_FILES_INITIAL_CAPACITY 32U
#define KERNEL_FILES_MAX_CAPACITY 1024U
#define KERNEL_FILES_FD_CLOEXEC UINT32_C(1)
#define KERNEL_FILES_MAX_RW_COUNT \
    ((uint64_t)INT32_MAX & ~(uint64_t)BOAROS_PAGE_MASK)
#define KERNEL_FILES_WRITE_STAGING 64U

#define LINUX_O_ACCMODE UINT64_C(00000003)
#define LINUX_O_WRONLY UINT64_C(00000001)
#define LINUX_O_RDWR UINT64_C(00000002)
#define LINUX_O_CREAT UINT64_C(00000100)
#define LINUX_O_EXCL UINT64_C(00000200)
#define LINUX_O_TRUNC UINT64_C(00001000)
#define LINUX_O_APPEND UINT64_C(00002000)
#define LINUX_O_NONBLOCK UINT64_C(00004000)
#define LINUX_O_DSYNC UINT64_C(00010000)
#define LINUX_O_DIRECT UINT64_C(00040000)
#define LINUX_O_LARGEFILE UINT64_C(00100000)
#define LINUX_O_DIRECTORY UINT64_C(00200000)
#define LINUX_O_NOFOLLOW UINT64_C(00400000)
#define LINUX_O_NOATIME UINT64_C(01000000)
#define LINUX_O_CLOEXEC UINT64_C(02000000)
#define LINUX_O_SYNC UINT64_C(04010000)
#define LINUX_O_PATH UINT64_C(010000000)
#define LINUX_O_TMPFILE UINT64_C(020200000)

struct kernel_file_slot {
    struct kernel_open_file_description *description;
    uint32_t flags;
};

struct kernel_files_record {
    struct kernel_file_slot *slots;
    struct kernel_open_file_description *cleanup_files;
    void *cleanup_allocations;
    struct kernel_files_statistics statistics;
    uint32_t references;
    uint32_t next_fd;
};

static int empty_files(const struct kernel_files *files)
{
    return files->state == KERNEL_FILES_EMPTY && files->heap == 0 &&
           files->record == 0;
}

int kernel_files_is_live(const struct kernel_files *files)
{
    return files != 0 && files->state == KERNEL_FILES_LIVE &&
           files->heap != 0 && files->record != 0 &&
           files->record->references == 1U &&
           files->record->slots != 0 &&
           files->record->statistics.capacity >=
               KERNEL_FILES_INITIAL_CAPACITY;
}

static void finish_files(struct kernel_files *files,
                         enum kernel_files_state state)
{
    files->heap = 0;
    files->record = 0;
    files->state = state;
}

static enum kernel_files_status create_files(
    struct kernel_files *files,
    struct kernel_heap *heap,
    uint32_t capacity)
{
    struct kernel_files_record *record;
    struct kernel_file_slot *slots;
    enum kernel_heap_status heap_status;

    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*record),
                                              (void **)&record);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_FILES_STATUS_NO_MEMORY
                   : KERNEL_FILES_STATUS_STATE;
    }
    record->references = 1U;
    heap_status = kernel_heap_allocate_zeroed(heap,
                                              capacity,
                                              sizeof(*slots),
                                              (void **)&slots);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        if (kernel_heap_release(heap, record) != KERNEL_HEAP_STATUS_OK) {
            files->heap = heap;
            files->record = record;
            files->state = KERNEL_FILES_CLEANUP;
            return KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
        }
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_FILES_STATUS_NO_MEMORY
                   : KERNEL_FILES_STATUS_STATE;
    }
    record->slots = slots;
    record->statistics.capacity = capacity;
    files->heap = heap;
    files->record = record;
    files->state = KERNEL_FILES_LIVE;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_create(
    struct kernel_files *files,
    struct kernel_heap *heap)
{
    if (files == 0 || heap == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_files(files)) {
        return KERNEL_FILES_STATUS_STATE;
    }
    return create_files(files, heap, KERNEL_FILES_INITIAL_CAPACITY);
}

enum kernel_files_status kernel_files_fork(
    struct kernel_files *destination,
    const struct kernel_files *source)
{
    uint32_t index;
    enum kernel_files_status status;

    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_files(destination) || !kernel_files_is_live(source)) {
        return KERNEL_FILES_STATUS_STATE;
    }
    status = create_files(destination,
                          source->heap,
                          source->record->statistics.capacity);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    destination->record->next_fd = source->record->next_fd;
    for (index = 0U;
         index < source->record->statistics.capacity;
         index++) {
        struct kernel_open_file_description *description =
            source->record->slots[index].description;

        if (description != 0) {
            if (kernel_open_file_acquire(description) !=
                KERNEL_OPEN_FILE_STATUS_OK) {
                status = KERNEL_FILES_STATUS_STATE;
                break;
            }
            destination->record->statistics.current_open_fds++;
            if ((source->record->slots[index].flags &
                 KERNEL_FILES_FD_CLOEXEC) != 0U) {
                destination->record->statistics.close_on_exec_fds++;
            }
        }
        destination->record->slots[index] = source->record->slots[index];
    }
    if (index == source->record->statistics.capacity) {
        destination->record->statistics.peak_open_fds =
            destination->record->statistics.current_open_fds;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_files_release(destination) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
    }
    return status;
}

enum kernel_files_status kernel_files_move(
    struct kernel_files *destination,
    struct kernel_files *source)
{
    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_files(destination) ||
        (source->state != KERNEL_FILES_LIVE &&
         source->state != KERNEL_FILES_CLEANUP) ||
        source->heap == 0 || source->record == 0) {
        return KERNEL_FILES_STATUS_STATE;
    }
    *destination = *source;
    finish_files(source, KERNEL_FILES_MOVED);
    return KERNEL_FILES_STATUS_OK;
}

static void queue_allocation_cleanup(struct kernel_files *files,
                                     void *pointer)
{
    *(void **)pointer = files->record->cleanup_allocations;
    files->record->cleanup_allocations = pointer;
}

static enum kernel_files_status release_or_queue_allocation(
    struct kernel_files *files,
    void *pointer)
{
    if (kernel_heap_release(files->heap, pointer) ==
        KERNEL_HEAP_STATUS_OK) {
        return KERNEL_FILES_STATUS_OK;
    }
    queue_allocation_cleanup(files, pointer);
    return KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
}

static enum kernel_files_status drain_allocation_cleanup(
    struct kernel_files *files)
{
    void **link = &files->record->cleanup_allocations;
    int failed = 0;

    while (*link != 0) {
        void *pointer = *link;
        void *next = *(void **)pointer;

        if (kernel_heap_release(files->heap, pointer) !=
            KERNEL_HEAP_STATUS_OK) {
            link = (void **)pointer;
            failed = 1;
        } else {
            *link = next;
        }
    }
    return failed ? KERNEL_FILES_STATUS_CLEANUP_REQUIRED
                  : KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status cleanup_description(
    struct kernel_files *files,
    struct kernel_open_file_description *description)
{
    enum kernel_open_file_status status =
        kernel_open_file_release(&description);

    (void)files;
    return status == KERNEL_OPEN_FILE_STATUS_OK
               ? KERNEL_FILES_STATUS_OK
               : status == KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED
                     ? KERNEL_FILES_STATUS_CLEANUP_REQUIRED
                     : KERNEL_FILES_STATUS_STATE;
}

static enum kernel_files_status drain_file_cleanup(
    struct kernel_files *files)
{
    struct kernel_open_file_description **link =
        &files->record->cleanup_files;
    int failed = 0;

    while (*link != 0) {
        struct kernel_open_file_description *description = *link;
        struct kernel_open_file_description *next =
            description->cleanup_next;

        if (cleanup_description(files, description) !=
            KERNEL_FILES_STATUS_OK) {
            link = &description->cleanup_next;
            failed = 1;
        } else {
            *link = next;
        }
    }
    return failed ? KERNEL_FILES_STATUS_CLEANUP_REQUIRED
                  : KERNEL_FILES_STATUS_OK;
}

static int validate_open_flags(uint64_t flags, uint32_t *fd_flags)
{
    const uint64_t write_flags = LINUX_O_CREAT | LINUX_O_TRUNC |
                                 LINUX_O_APPEND;
    const uint64_t unsupported_flags = LINUX_O_EXCL |
        LINUX_O_NONBLOCK | LINUX_O_DSYNC | LINUX_O_DIRECT |
        LINUX_O_DIRECTORY | LINUX_O_NOFOLLOW | LINUX_O_NOATIME |
        LINUX_O_SYNC | LINUX_O_PATH | LINUX_O_TMPFILE;
    const uint64_t known_flags = LINUX_O_ACCMODE | write_flags |
        unsupported_flags | LINUX_O_LARGEFILE | LINUX_O_CLOEXEC;
    uint64_t access_mode = flags & LINUX_O_ACCMODE;

    if (access_mode == LINUX_O_WRONLY || access_mode == LINUX_O_RDWR ||
        (flags & write_flags) != 0U) {
        return -KERNEL_EROFS;
    }
    if (access_mode != 0U) {
        return -KERNEL_EINVAL;
    }
    if ((flags & unsupported_flags) != 0U) {
        return -KERNEL_ENOTSUP;
    }
    if ((flags & ~known_flags) != 0U) {
        return -KERNEL_EINVAL;
    }
    *fd_flags = (flags & LINUX_O_CLOEXEC) != 0U
                    ? KERNEL_FILES_FD_CLOEXEC
                    : 0U;
    return 0;
}

static enum kernel_files_status find_free_fd(struct kernel_files *files,
                                              uint32_t *fd,
                                              int *linux_result)
{
    uint32_t index;
    uint32_t old_capacity = files->record->statistics.capacity;

    *linux_result = 0;

    for (index = files->record->next_fd; index < old_capacity; index++) {
        if (files->record->slots[index].description == 0) {
            *fd = index;
            return KERNEL_FILES_STATUS_OK;
        }
    }
    if (old_capacity == KERNEL_FILES_MAX_CAPACITY) {
        *linux_result = -KERNEL_EMFILE;
        return KERNEL_FILES_STATUS_OK;
    }
    {
        uint32_t new_capacity = old_capacity * 2U;
        struct kernel_file_slot *resized;
        enum kernel_heap_status heap_status;

        if (new_capacity > KERNEL_FILES_MAX_CAPACITY) {
            new_capacity = KERNEL_FILES_MAX_CAPACITY;
        }
        heap_status = kernel_heap_resize(
            files->heap,
            files->record->slots,
            (size_t)new_capacity * sizeof(*resized),
            (void **)&resized);
        if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
            *linux_result = -KERNEL_ENOMEM;
            return KERNEL_FILES_STATUS_OK;
        }
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        files->record->slots = resized;
        for (index = old_capacity; index < new_capacity; index++) {
            files->record->slots[index].description = 0;
            files->record->slots[index].flags = 0U;
        }
        files->record->statistics.capacity = new_capacity;
        *fd = old_capacity;
        return KERNEL_FILES_STATUS_OK;
    }
}

static void queue_description(
    struct kernel_files *files,
    struct kernel_open_file_description *description)
{
    description->cleanup_next = files->record->cleanup_files;
    files->record->cleanup_files = description;
}

static enum kernel_files_status finish_open_path(
    struct kernel_files *files,
    char *path)
{
    return release_or_queue_allocation(files, path) ==
                   KERNEL_FILES_STATUS_OK
               ? KERNEL_FILES_STATUS_OK
               : KERNEL_FILES_STATUS_STATE;
}

enum kernel_files_status kernel_files_openat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint64_t flags,
    uint64_t mode,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    struct kernel_vfs_mount *mount;
    char *path;
    uint32_t fd;
    uint32_t fd_flags;
    int result;
    enum kernel_heap_status heap_status;
    enum kernel_open_file_status open_status;
    enum kernel_fs_context_status fs_status;
    enum kernel_files_status files_status;

    (void)mode;
    if (!kernel_files_is_live(files) ||
        !kernel_fs_context_is_live(fs) || mm == 0 ||
        linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    files->record->statistics.open_calls++;
    result = validate_open_flags(flags, &fd_flags);
    if (result != 0) {
        files->record->statistics.open_failures++;
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    files_status = find_free_fd(files, &fd, &result);
    if (files_status != KERNEL_FILES_STATUS_OK) {
        return files_status;
    }
    if (result != 0) {
        files->record->statistics.open_failures++;
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    heap_status = kernel_heap_allocate(files->heap,
                                       KERNEL_FS_PATH_MAX,
                                       (void **)&path);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
        files->record->statistics.open_failures++;
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    fs_status = kernel_fs_context_resolve_user_path(fs,
                                                    mm,
                                                    dirfd,
                                                    user_path,
                                                    path,
                                                    KERNEL_FS_PATH_MAX,
                                                    &mount,
                                                    &result);
    if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
        (void)finish_open_path(files, path);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result != 0) {
        files->record->statistics.open_failures++;
        if (finish_open_path(files, path) != KERNEL_FILES_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    description = 0;
    open_status = kernel_open_file_create(files->heap,
                                          mount,
                                          path,
                                          &description,
                                          &result);
    if (finish_open_path(files, path) != KERNEL_FILES_STATUS_OK) {
        if (description != 0) {
            queue_description(files, description);
            (void)drain_file_cleanup(files);
        }
        return KERNEL_FILES_STATUS_STATE;
    }
    if (open_status == KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED) {
        if (description != 0) {
            queue_description(files, description);
        }
        return KERNEL_FILES_STATUS_STATE;
    }
    if (open_status != KERNEL_OPEN_FILE_STATUS_OK) {
        return open_status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                   ? KERNEL_FILES_STATUS_NO_MEMORY
                   : KERNEL_FILES_STATUS_STATE;
    }
    if (result != 0) {
        files->record->statistics.open_failures++;
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    if ((kernel_open_file_mode(description) & KERNEL_VFS_S_IFMT) !=
        KERNEL_VFS_S_IFREG) {
        int type_result =
            (kernel_open_file_mode(description) & KERNEL_VFS_S_IFMT) ==
                    KERNEL_VFS_S_IFDIR
                ? -KERNEL_EISDIR
                : -KERNEL_ENOTSUP;

        files->record->statistics.open_failures++;
        queue_description(files, description);
        (void)drain_file_cleanup(files);
        *linux_result = type_result;
        return KERNEL_FILES_STATUS_OK;
    }

    files->record->slots[fd].description = description;
    files->record->slots[fd].flags = fd_flags;
    files->record->statistics.current_open_fds++;
    if (files->record->statistics.current_open_fds >
        files->record->statistics.peak_open_fds) {
        files->record->statistics.peak_open_fds =
            files->record->statistics.current_open_fds;
    }
    if ((fd_flags & KERNEL_FILES_FD_CLOEXEC) != 0U) {
        files->record->statistics.close_on_exec_fds++;
    }
    files->record->next_fd = fd + 1U;
    *linux_result = fd;
    return KERNEL_FILES_STATUS_OK;
}

static struct kernel_open_file_description *lookup_description(
    struct kernel_files *files,
    int64_t fd)
{
    if (fd < 0 ||
        (uint64_t)fd >= files->record->statistics.capacity) {
        return 0;
    }
    return files->record->slots[fd].description;
}

enum kernel_files_status kernel_files_pin(
    struct kernel_files *files,
    int64_t fd,
    struct kernel_open_file_description **owner,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;

    if (!kernel_files_is_live(files) || owner == 0 || *owner != 0 ||
        linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    description = lookup_description(files, fd);
    if (description == 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_acquire(description) !=
        KERNEL_OPEN_FILE_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    *owner = description;
    *linux_result = 0;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_read(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    uint64_t count,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    uint64_t request;
    uint64_t total = 0U;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    files->record->statistics.read_calls++;
    description = lookup_description(files, fd);
    if (description == 0) {
        files->record->statistics.read_failures++;
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_CONSOLE) {
        /* No console input source exists yet; report the empty stream as EOF. */
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_user_range_check(user_buffer, (size_t)count) !=
        KERNEL_UACCESS_STATUS_OK) {
        files->record->statistics.read_failures++;
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }
    if (count == 0U) {
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    }
    request = count > KERNEL_FILES_MAX_RW_COUNT
                  ? KERNEL_FILES_MAX_RW_COUNT
                  : count;
    while (total < request) {
        uint64_t file_offset = kernel_open_file_offset(description);
        uint64_t page_index = file_offset >> BOAROS_PAGE_SHIFT;
        size_t page_offset = (size_t)(file_offset & BOAROS_PAGE_MASK);
        size_t valid_bytes;
        size_t chunk;
        size_t copied = 0U;
        uint64_t physical_address;
        void *page;
        enum kernel_page_cache_status cache_status;
        enum kernel_uaccess_status access_status;

        cache_status = kernel_open_file_get_page(description,
                                                 page_index,
                                                 &physical_address,
                                                 &valid_bytes);
        files->record->statistics.read_chunks++;
        if (cache_status == KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE) {
            break;
        }
        if (cache_status != KERNEL_PAGE_CACHE_STATUS_OK) {
            int result = cache_status ==
                                     KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                                 ? -KERNEL_ENOMEM
                                 : -KERNEL_EIO;

            files->record->statistics.read_failures++;
            *linux_result = total != 0U ? (int64_t)total : result;
            files->record->statistics.bytes_read += total;
            return KERNEL_FILES_STATUS_OK;
        }
        if (page_offset >= valid_bytes) {
            if (physical_page_release(files->heap->page_allocator,
                                      physical_address) !=
                PHYSICAL_PAGE_STATUS_OK) {
                return KERNEL_FILES_STATUS_STATE;
            }
            break;
        }
        chunk = valid_bytes - page_offset;
        if ((uint64_t)chunk > request - total) {
            chunk = (size_t)(request - total);
        }
        if (physical_page_resolve(files->heap->page_allocator,
                                  physical_address,
                                  &page) !=
            PHYSICAL_PAGE_STATUS_OK) {
            (void)physical_page_release(files->heap->page_allocator,
                                        physical_address);
            return KERNEL_FILES_STATUS_STATE;
        }
        access_status = kernel_copy_to_user(mm,
                                            user_buffer + total,
                                            (unsigned char *)page +
                                                page_offset,
                                            chunk,
                                            &copied);
        if (kernel_open_file_advance(description, copied) !=
                KERNEL_OPEN_FILE_STATUS_OK ||
            physical_page_release(files->heap->page_allocator,
                                  physical_address) !=
                PHYSICAL_PAGE_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        total += copied;
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            files->record->statistics.read_failures++;
            *linux_result = total != 0U ? (int64_t)total
                                        : -KERNEL_EFAULT;
            files->record->statistics.bytes_read += total;
            return KERNEL_FILES_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != chunk) {
            return KERNEL_FILES_STATUS_STATE;
        }
        if (page_offset + chunk == valid_bytes &&
            valid_bytes < BOAROS_PAGE_SIZE) {
            break;
        }
    }

    files->record->statistics.bytes_read += total;
    *linux_result = (int64_t)total;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_write(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    uint64_t count,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    unsigned char staging[KERNEL_FILES_WRITE_STAGING];
    uint64_t request;
    uint64_t total = 0U;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    files->record->statistics.write_calls++;
    description = lookup_description(files, fd);
    if (description == 0 ||
        kernel_open_file_kind(description) !=
            KERNEL_OPEN_FILE_KIND_CONSOLE) {
        /* Every regular-file descriptor is read-only on the read-only root. */
        files->record->statistics.write_failures++;
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_user_range_check(user_buffer, (size_t)count) !=
        KERNEL_UACCESS_STATUS_OK) {
        files->record->statistics.write_failures++;
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }
    if (count == 0U) {
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    }
    request = count > KERNEL_FILES_MAX_RW_COUNT
                  ? KERNEL_FILES_MAX_RW_COUNT
                  : count;
    while (total < request) {
        size_t chunk = request - total;
        size_t copied = 0U;
        size_t index;
        enum kernel_uaccess_status access_status;

        if (chunk > sizeof(staging)) {
            chunk = sizeof(staging);
        }
        access_status = kernel_copy_from_user(mm,
                                              staging,
                                              user_buffer + total,
                                              chunk,
                                              &copied);
        total += copied;
        for (index = 0U; index < copied; index++) {
            kernel_console_putc((char)staging[index]);
        }
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            files->record->statistics.write_failures++;
            files->record->statistics.bytes_written += total;
            *linux_result = total != 0U ? (int64_t)total : -KERNEL_EFAULT;
            return KERNEL_FILES_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK || copied != chunk) {
            return KERNEL_FILES_STATUS_STATE;
        }
    }
    files->record->statistics.bytes_written += total;
    *linux_result = (int64_t)total;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_open_console(
    struct kernel_files *files,
    int64_t fd,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description = 0;
    enum kernel_open_file_status open_status;

    if (!kernel_files_is_live(files) || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (fd < 0 || (uint64_t)fd >= files->record->statistics.capacity ||
        lookup_description(files, fd) != 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    open_status = kernel_open_file_create_console(files->heap, &description);
    if (open_status != KERNEL_OPEN_FILE_STATUS_OK) {
        return open_status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                   ? KERNEL_FILES_STATUS_NO_MEMORY
                   : KERNEL_FILES_STATUS_STATE;
    }
    files->record->slots[fd].description = description;
    files->record->slots[fd].flags = 0U;
    files->record->statistics.current_open_fds++;
    if (files->record->statistics.current_open_fds >
        files->record->statistics.peak_open_fds) {
        files->record->statistics.peak_open_fds =
            files->record->statistics.current_open_fds;
    }
    *linux_result = 0;
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status detach_fd(struct kernel_files *files,
                                           uint32_t fd)
{
    struct kernel_file_slot *slot = &files->record->slots[fd];

    struct kernel_open_file_description *description;
    enum kernel_open_file_status status;

    if (slot->description == 0) {
        return KERNEL_FILES_STATUS_STATE;
    }
    description = slot->description;
    status = kernel_open_file_detach(&description);
    if (status == KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED) {
        if (description == 0) {
            return KERNEL_FILES_STATUS_STATE;
        }
        queue_description(files, description);
    } else if (status != KERNEL_OPEN_FILE_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }

    slot->description = 0;
    if ((slot->flags & KERNEL_FILES_FD_CLOEXEC) != 0U &&
        files->record->statistics.close_on_exec_fds != 0U) {
        files->record->statistics.close_on_exec_fds--;
    }
    slot->flags = 0U;
    if (files->record->statistics.current_open_fds != 0U) {
        files->record->statistics.current_open_fds--;
    }
    if (fd < files->record->next_fd) {
        files->record->next_fd = fd;
    }
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_close(
    struct kernel_files *files,
    int64_t fd,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;

    if (!kernel_files_is_live(files) || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    files->record->statistics.close_calls++;
    description = lookup_description(files, fd);
    if (description == 0) {
        files->record->statistics.close_failures++;
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    (void)description;
    if (detach_fd(files, (uint32_t)fd) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (drain_file_cleanup(files) != KERNEL_FILES_STATUS_OK ||
        drain_allocation_cleanup(files) != KERNEL_FILES_STATUS_OK) {
        files->record->statistics.close_failures++;
        *linux_result = -KERNEL_EIO;
        return KERNEL_FILES_STATUS_OK;
    }
    *linux_result = 0;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_close_on_exec(
    struct kernel_files *files)
{
    uint32_t index;
    int cleanup_required = 0;

    if (!kernel_files_is_live(files)) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U;
         index < files->record->statistics.capacity;
         index++) {
        if (files->record->slots[index].description != 0 &&
            (files->record->slots[index].flags &
             KERNEL_FILES_FD_CLOEXEC) != 0U &&
            detach_fd(files, index) != KERNEL_FILES_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
    }
    if (drain_file_cleanup(files) != KERNEL_FILES_STATUS_OK) {
        cleanup_required = 1;
    }
    if (drain_allocation_cleanup(files) != KERNEL_FILES_STATUS_OK) {
        cleanup_required = 1;
    }
    return cleanup_required ? KERNEL_FILES_STATUS_CLEANUP_REQUIRED
                            : KERNEL_FILES_STATUS_OK;
}

void kernel_files_get_statistics(
    const struct kernel_files *files,
    struct kernel_files_statistics *statistics)
{
    if (files == 0 || statistics == 0 ||
        (files->state != KERNEL_FILES_LIVE &&
         files->state != KERNEL_FILES_CLEANUP)) {
        return;
    }
    if (files->record == 0) {
        return;
    }
    *statistics = files->record->statistics;
}

enum kernel_files_status kernel_files_release(
    struct kernel_files *files)
{
    uint32_t index;

    if (files == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if ((files->state != KERNEL_FILES_LIVE &&
         files->state != KERNEL_FILES_CLEANUP) ||
        files->heap == 0 || files->record == 0 ||
        files->record->references != 1U) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (files->state == KERNEL_FILES_LIVE) {
        for (index = 0U;
             index < files->record->statistics.capacity;
             index++) {
            if (files->record->slots[index].description != 0 &&
                detach_fd(files, index) != KERNEL_FILES_STATUS_OK) {
                return KERNEL_FILES_STATUS_STATE;
            }
        }
        files->state = KERNEL_FILES_CLEANUP;
    }
    if (drain_file_cleanup(files) != KERNEL_FILES_STATUS_OK ||
        drain_allocation_cleanup(files) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
    }
    if (files->record->slots != 0) {
        if (kernel_heap_release(files->heap,
                                files->record->slots) !=
            KERNEL_HEAP_STATUS_OK) {
            return KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
        }
        files->record->slots = 0;
    }
    if (kernel_heap_release(files->heap, files->record) !=
        KERNEL_HEAP_STATUS_OK) {
        return KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
    }
    finish_files(files, KERNEL_FILES_RELEASED);
    return KERNEL_FILES_STATUS_OK;
}
