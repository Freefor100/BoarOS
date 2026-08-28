#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/page.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_FILES_INITIAL_CAPACITY 32U
#define KERNEL_FILES_MAX_CAPACITY 1024U
#define KERNEL_FILES_FD_CLOEXEC UINT32_C(1)
#define KERNEL_FILES_MAX_RW_COUNT \
    ((uint64_t)INT32_MAX & ~(uint64_t)BOAROS_PAGE_MASK)

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

struct kernel_open_file_description {
    struct kernel_vfs_file file;
    struct kernel_open_file_description *cleanup_next;
    uint64_t offset;
    uint8_t vfs_closed;
};

struct kernel_file_slot {
    struct kernel_open_file_description *description;
    uint32_t flags;
};

static int empty_files(const struct kernel_files *files)
{
    return files->state == KERNEL_FILES_EMPTY && files->heap == 0 &&
           files->slots == 0 && files->cleanup_files == 0 &&
           files->cleanup_allocations == 0 && files->next_fd == 0U;
}

int kernel_files_is_live(const struct kernel_files *files)
{
    return files != 0 && files->state == KERNEL_FILES_LIVE &&
           files->heap != 0 && files->slots != 0 &&
           files->statistics.capacity >= KERNEL_FILES_INITIAL_CAPACITY;
}

static void finish_files(struct kernel_files *files,
                         enum kernel_files_state state)
{
    files->heap = 0;
    files->slots = 0;
    files->cleanup_files = 0;
    files->cleanup_allocations = 0;
    files->next_fd = 0U;
    files->state = state;
}

enum kernel_files_status kernel_files_create(
    struct kernel_files *files,
    struct kernel_heap *heap)
{
    struct kernel_file_slot *slots;
    enum kernel_heap_status heap_status;

    if (files == 0 || heap == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_files(files)) {
        return KERNEL_FILES_STATUS_STATE;
    }
    heap_status = kernel_heap_allocate_zeroed(
        heap,
        KERNEL_FILES_INITIAL_CAPACITY,
        sizeof(*slots),
        (void **)&slots);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_FILES_STATUS_NO_MEMORY
                   : KERNEL_FILES_STATUS_STATE;
    }
    files->heap = heap;
    files->slots = slots;
    files->statistics.capacity = KERNEL_FILES_INITIAL_CAPACITY;
    files->state = KERNEL_FILES_LIVE;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_move(
    struct kernel_files *destination,
    struct kernel_files *source)
{
    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_files(destination) || !kernel_files_is_live(source)) {
        return KERNEL_FILES_STATUS_STATE;
    }
    *destination = *source;
    finish_files(source, KERNEL_FILES_MOVED);
    return KERNEL_FILES_STATUS_OK;
}

static void queue_allocation_cleanup(struct kernel_files *files,
                                     void *pointer)
{
    *(void **)pointer = files->cleanup_allocations;
    files->cleanup_allocations = pointer;
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
    void **link = &files->cleanup_allocations;
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
    if (!description->vfs_closed) {
        if (kernel_vfs_close(&description->file) != 0) {
            return KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
        }
        description->vfs_closed = 1U;
    }
    return kernel_heap_release(files->heap, description) ==
                   KERNEL_HEAP_STATUS_OK
               ? KERNEL_FILES_STATUS_OK
               : KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
}

static enum kernel_files_status drain_file_cleanup(
    struct kernel_files *files)
{
    struct kernel_open_file_description **link = &files->cleanup_files;
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
    uint32_t old_capacity = files->statistics.capacity;

    *linux_result = 0;

    for (index = files->next_fd; index < old_capacity; index++) {
        if (files->slots[index].description == 0) {
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
            files->slots,
            (size_t)new_capacity * sizeof(*resized),
            (void **)&resized);
        if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
            *linux_result = -KERNEL_ENOMEM;
            return KERNEL_FILES_STATUS_OK;
        }
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        files->slots = resized;
        for (index = old_capacity; index < new_capacity; index++) {
            files->slots[index].description = 0;
            files->slots[index].flags = 0U;
        }
        files->statistics.capacity = new_capacity;
        *fd = old_capacity;
        return KERNEL_FILES_STATUS_OK;
    }
}

static void queue_description(
    struct kernel_files *files,
    struct kernel_open_file_description *description)
{
    description->cleanup_next = files->cleanup_files;
    files->cleanup_files = description;
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
    const struct kernel_mm *mm,
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
    enum kernel_fs_context_status fs_status;
    enum kernel_files_status files_status;

    (void)mode;
    if (!kernel_files_is_live(files) ||
        !kernel_fs_context_is_live(fs) || mm == 0 ||
        linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    files->statistics.open_calls++;
    result = validate_open_flags(flags, &fd_flags);
    if (result != 0) {
        files->statistics.open_failures++;
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    files_status = find_free_fd(files, &fd, &result);
    if (files_status != KERNEL_FILES_STATUS_OK) {
        return files_status;
    }
    if (result != 0) {
        files->statistics.open_failures++;
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    heap_status = kernel_heap_allocate(files->heap,
                                       KERNEL_FS_PATH_MAX,
                                       (void **)&path);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
        files->statistics.open_failures++;
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
        files->statistics.open_failures++;
        if (finish_open_path(files, path) != KERNEL_FILES_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    heap_status = kernel_heap_allocate_zeroed(files->heap,
                                              1U,
                                              sizeof(*description),
                                              (void **)&description);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        files->statistics.open_failures++;
        if (finish_open_path(files, path) != KERNEL_FILES_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
            *linux_result = -KERNEL_ENOMEM;
            return KERNEL_FILES_STATUS_OK;
        }
        return KERNEL_FILES_STATUS_STATE;
    }
    result = kernel_vfs_open(mount, path, &description->file);
    if (finish_open_path(files, path) != KERNEL_FILES_STATUS_OK) {
        if (result == 0) {
            queue_description(files, description);
            (void)drain_file_cleanup(files);
        } else {
            (void)kernel_heap_release(files->heap, description);
        }
        return KERNEL_FILES_STATUS_STATE;
    }
    if (result != 0) {
        files->statistics.open_failures++;
        if (kernel_heap_release(files->heap, description) !=
            KERNEL_HEAP_STATUS_OK) {
            queue_allocation_cleanup(files, description);
            return KERNEL_FILES_STATUS_STATE;
        }
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    if ((description->file.mode & KERNEL_VFS_S_IFMT) !=
        KERNEL_VFS_S_IFREG) {
        int type_result =
            (description->file.mode & KERNEL_VFS_S_IFMT) ==
                    KERNEL_VFS_S_IFDIR
                ? -KERNEL_EISDIR
                : -KERNEL_ENOTSUP;

        files->statistics.open_failures++;
        queue_description(files, description);
        (void)drain_file_cleanup(files);
        *linux_result = type_result;
        return KERNEL_FILES_STATUS_OK;
    }

    files->slots[fd].description = description;
    files->slots[fd].flags = fd_flags;
    files->statistics.current_open_fds++;
    if (files->statistics.current_open_fds >
        files->statistics.peak_open_fds) {
        files->statistics.peak_open_fds =
            files->statistics.current_open_fds;
    }
    if ((fd_flags & KERNEL_FILES_FD_CLOEXEC) != 0U) {
        files->statistics.close_on_exec_fds++;
    }
    files->next_fd = fd + 1U;
    *linux_result = fd;
    return KERNEL_FILES_STATUS_OK;
}

static struct kernel_open_file_description *lookup_description(
    struct kernel_files *files,
    int64_t fd)
{
    if (fd < 0 || (uint64_t)fd >= files->statistics.capacity) {
        return 0;
    }
    return files->slots[fd].description;
}

enum kernel_files_status kernel_files_read(
    struct kernel_files *files,
    const struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    uint64_t count,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    unsigned char *scratch;
    uint64_t request;
    uint64_t total = 0U;
    enum kernel_heap_status heap_status;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    files->statistics.read_calls++;
    description = lookup_description(files, fd);
    if (description == 0) {
        files->statistics.read_failures++;
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_user_range_check(user_buffer, (size_t)count) !=
        KERNEL_UACCESS_STATUS_OK) {
        files->statistics.read_failures++;
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
    heap_status = kernel_heap_allocate(
        files->heap,
        request < BOAROS_PAGE_SIZE ? (size_t)request
                                   : (size_t)BOAROS_PAGE_SIZE,
        (void **)&scratch);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
        files->statistics.read_failures++;
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }

    while (total < request) {
        size_t chunk = BOAROS_PAGE_SIZE;
        size_t bytes_read = 0U;
        size_t copied = 0U;
        int result;
        enum kernel_uaccess_status access_status;

        if ((uint64_t)chunk > request - total) {
            chunk = (size_t)(request - total);
        }
        result = kernel_vfs_pread(&description->file,
                                  description->offset,
                                  scratch,
                                  chunk,
                                  &bytes_read);
        files->statistics.read_chunks++;
        if (result != 0) {
            files->statistics.read_failures++;
            if (release_or_queue_allocation(files, scratch) !=
                KERNEL_FILES_STATUS_OK) {
                return KERNEL_FILES_STATUS_STATE;
            }
            *linux_result = total != 0U ? (int64_t)total : result;
            files->statistics.bytes_read += total;
            return KERNEL_FILES_STATUS_OK;
        }
        if (bytes_read == 0U) {
            break;
        }
        access_status = kernel_copy_to_user(mm,
                                            user_buffer + total,
                                            scratch,
                                            bytes_read,
                                            &copied);
        description->offset += copied;
        total += copied;
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            files->statistics.read_failures++;
            if (release_or_queue_allocation(files, scratch) !=
                KERNEL_FILES_STATUS_OK) {
                return KERNEL_FILES_STATUS_STATE;
            }
            *linux_result = total != 0U ? (int64_t)total
                                        : -KERNEL_EFAULT;
            files->statistics.bytes_read += total;
            return KERNEL_FILES_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != bytes_read) {
            (void)release_or_queue_allocation(files, scratch);
            return KERNEL_FILES_STATUS_STATE;
        }
        if (bytes_read < chunk) {
            break;
        }
    }

    if (release_or_queue_allocation(files, scratch) !=
        KERNEL_FILES_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    files->statistics.bytes_read += total;
    *linux_result = (int64_t)total;
    return KERNEL_FILES_STATUS_OK;
}

static void detach_fd(struct kernel_files *files, uint32_t fd)
{
    struct kernel_file_slot *slot = &files->slots[fd];

    queue_description(files, slot->description);
    slot->description = 0;
    if ((slot->flags & KERNEL_FILES_FD_CLOEXEC) != 0U &&
        files->statistics.close_on_exec_fds != 0U) {
        files->statistics.close_on_exec_fds--;
    }
    slot->flags = 0U;
    if (files->statistics.current_open_fds != 0U) {
        files->statistics.current_open_fds--;
    }
    if (fd < files->next_fd) {
        files->next_fd = fd;
    }
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
    files->statistics.close_calls++;
    description = lookup_description(files, fd);
    if (description == 0) {
        files->statistics.close_failures++;
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    (void)description;
    detach_fd(files, (uint32_t)fd);
    if (drain_file_cleanup(files) != KERNEL_FILES_STATUS_OK ||
        drain_allocation_cleanup(files) != KERNEL_FILES_STATUS_OK) {
        files->statistics.close_failures++;
        *linux_result = -KERNEL_EIO;
        return KERNEL_FILES_STATUS_OK;
    }
    *linux_result = 0;
    return KERNEL_FILES_STATUS_OK;
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
    *statistics = files->statistics;
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
        files->heap == 0 || files->slots == 0) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (files->state == KERNEL_FILES_LIVE) {
        for (index = 0U; index < files->statistics.capacity; index++) {
            if (files->slots[index].description != 0) {
                detach_fd(files, index);
            }
        }
        files->state = KERNEL_FILES_CLEANUP;
    }
    if (drain_file_cleanup(files) != KERNEL_FILES_STATUS_OK ||
        drain_allocation_cleanup(files) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
    }
    if (kernel_heap_release(files->heap, files->slots) !=
        KERNEL_HEAP_STATUS_OK) {
        return KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
    }
    finish_files(files, KERNEL_FILES_RELEASED);
    return KERNEL_FILES_STATUS_OK;
}
