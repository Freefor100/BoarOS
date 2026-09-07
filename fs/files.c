#include "open_file_internal.h"
#include "pipe_internal.h"

#include <arch/riscv/context.h>
#include <arch/riscv/virt_uart.h>
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
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>
#include <string.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_FILES_INITIAL_CAPACITY 32U
#define KERNEL_FILES_MAX_CAPACITY 1024U
#define KERNEL_FILES_FD_CLOEXEC UINT32_C(1)
#define KERNEL_FILES_MAX_RW_COUNT \
    ((uint64_t)INT32_MAX & ~(uint64_t)BOAROS_PAGE_MASK)
#define KERNEL_FILES_WRITE_STAGING 64U
#define KERNEL_FILES_CONSOLE_STAGING 64U

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
    struct kernel_pipe *cleanup_pipes;
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

static void queue_pipe_cleanup(struct kernel_files *files,
                               struct kernel_pipe *pipe)
{
    pipe->cleanup_next = files->record->cleanup_pipes;
    files->record->cleanup_pipes = pipe;
}

static enum kernel_files_status drain_pipe_cleanup(
    struct kernel_files *files)
{
    struct kernel_pipe **link = &files->record->cleanup_pipes;
    int failed = 0;

    while (*link != 0) {
        struct kernel_pipe *pipe = *link;
        struct kernel_pipe *next = pipe->cleanup_next;

        if (kernel_pipe_destroy_unowned(pipe) != KERNEL_PIPE_STATUS_OK) {
            link = &pipe->cleanup_next;
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
    /* O_TMPFILE embeds the O_DIRECTORY bit; only its own bit is
     * unsupported, so a plain O_DIRECTORY open still validates. */
    const uint64_t unsupported_flags = LINUX_O_EXCL |
        LINUX_O_NONBLOCK | LINUX_O_DSYNC | LINUX_O_DIRECT |
        LINUX_O_NOFOLLOW | LINUX_O_NOATIME |
        LINUX_O_SYNC | LINUX_O_PATH |
        (LINUX_O_TMPFILE & ~LINUX_O_DIRECTORY);
    const uint64_t known_flags = LINUX_O_ACCMODE | write_flags |
        unsupported_flags | LINUX_O_DIRECTORY | LINUX_O_LARGEFILE |
        LINUX_O_CLOEXEC;
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
    uint32_t fd_flags = 0U;
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
    if ((kernel_open_file_mode(description) & KERNEL_VFS_S_IFMT) ==
        KERNEL_VFS_S_IFDIR) {
        description->kind = KERNEL_OPEN_FILE_KIND_DIRECTORY;
    } else if ((kernel_open_file_mode(description) & KERNEL_VFS_S_IFMT) ==
               KERNEL_VFS_S_IFREG) {
        if ((flags & LINUX_O_DIRECTORY) != 0U) {
            files->record->statistics.open_failures++;
            queue_description(files, description);
            (void)drain_file_cleanup(files);
            *linux_result = -KERNEL_ENOTDIR;
            return KERNEL_FILES_STATUS_OK;
        }
        description->kind = KERNEL_OPEN_FILE_KIND_REGULAR;
    } else {
        files->record->statistics.open_failures++;
        queue_description(files, description);
        (void)drain_file_cleanup(files);
        *linux_result = -KERNEL_ENOTSUP;
        return KERNEL_FILES_STATUS_OK;
    }

    files->record->slots[fd].description = description;
    files->record->slots[fd].flags = fd_flags;
    description->open_flags = (uint32_t)flags;
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

/*
 * All console descriptors share one receive channel.  The timer tick
 * polls the UART and wakes a reader through this queue; readers drain
 * the FIFO themselves, so the poller only needs the data-ready edge.
 */
static struct kernel_wait_queue console_input_queue;

void kernel_console_poll_input(void)
{
    if (console_input_queue.initialized == KERNEL_WAIT_QUEUE_INITIALIZED &&
        virt_uart_rx_ready() != 0U) {
        (void)kernel_wait_queue_wake_one(&console_input_queue);
    }
}

static enum kernel_files_status kernel_files_read_console(
    struct kernel_files *files,
    struct kernel_mm *mm,
    uint64_t user_buffer,
    uint64_t count,
    int64_t *linux_result)
{
    unsigned char staging[KERNEL_FILES_CONSOLE_STAGING];
    enum kernel_wait_wake_reason wake_reason = KERNEL_WAIT_WOKEN;
    enum kernel_uaccess_status access_status;
    enum kernel_scheduler_status sleep_status;
    size_t staged = 0;
    size_t copied = 0;
    uintptr_t saved;

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

    saved = riscv_interrupt_save();
    if (console_input_queue.initialized != KERNEL_WAIT_QUEUE_INITIALIZED) {
        kernel_wait_queue_init(&console_input_queue);
    }
    for (;;) {
        while (staged < KERNEL_FILES_CONSOLE_STAGING &&
               virt_uart_rx_ready() != 0U) {
            staging[staged] = (unsigned char)virt_uart_getc();
            staged++;
        }
        if (staged != 0U) {
            break;
        }
        sleep_status = kernel_scheduler_block_current(&console_input_queue,
                                                      0U,
                                                      1,
                                                      &wake_reason);
        if (sleep_status != KERNEL_SCHEDULER_STATUS_OK) {
            riscv_interrupt_restore(saved);
            return KERNEL_FILES_STATUS_STATE;
        }
        if (wake_reason == KERNEL_WAIT_SIGNALLED) {
            kernel_signal_note_syscall_restart(kernel_task_current());
            riscv_interrupt_restore(saved);
            *linux_result = -KERNEL_ERESTARTSYS;
            return KERNEL_FILES_STATUS_OK;
        }
    }
    riscv_interrupt_restore(saved);

    access_status = kernel_copy_to_user(mm,
                                        user_buffer,
                                        staging,
                                        staged,
                                        &copied);
    if (access_status != KERNEL_UACCESS_STATUS_OK && copied == 0U) {
        files->record->statistics.read_failures++;
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }

    *linux_result = (int64_t)copied;
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
        return kernel_files_read_console(files, mm, user_buffer, count,
                                         linux_result);
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE) {
        enum kernel_pipe_status pipe_status = kernel_pipe_read(
            description->pipe,
            mm,
            user_buffer,
            count,
            description->open_flags,
            linux_result);

        if (pipe_status != KERNEL_PIPE_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        if (*linux_result >= 0) {
            files->record->statistics.bytes_read += (uint64_t)*linux_result;
        } else {
            files->record->statistics.read_failures++;
        }
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) ==
        KERNEL_OPEN_FILE_KIND_DIRECTORY) {
        files->record->statistics.read_failures++;
        *linux_result = -KERNEL_EISDIR;
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
    if (description == 0) {
        /* Every regular-file descriptor is read-only on the read-only root. */
        files->record->statistics.write_failures++;
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE) {
        enum kernel_pipe_status pipe_status = kernel_pipe_write(
            description->pipe,
            mm,
            user_buffer,
            count,
            description->open_flags,
            linux_result);

        if (pipe_status != KERNEL_PIPE_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        if (*linux_result >= 0) {
            files->record->statistics.bytes_written +=
                (uint64_t)*linux_result;
        } else {
            files->record->statistics.write_failures++;
        }
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_CONSOLE) {
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

enum kernel_files_status kernel_files_writev(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_iov,
    uint64_t iovcnt,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    unsigned char staging[KERNEL_FILES_WRITE_STAGING];
    uint64_t total = 0U;
    uint64_t index;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    files->record->statistics.write_calls++;
    description = lookup_description(files, fd);
    if (description == 0 ||
        (kernel_open_file_kind(description) !=
             KERNEL_OPEN_FILE_KIND_CONSOLE &&
         kernel_open_file_kind(description) != KERNEL_OPEN_FILE_KIND_PIPE)) {
        files->record->statistics.write_failures++;
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (iovcnt == 0U) {
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    }
    if (iovcnt > 1024U) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }

    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE) {
        for (index = 0U; index < iovcnt; index++) {
            struct kernel_uaccess_iovec iovec;
            size_t copied = 0U;
            int64_t written;
            enum kernel_uaccess_status access_status;
            enum kernel_pipe_status pipe_status;

            access_status = kernel_copy_from_user(mm,
                                                  &iovec,
                                                  user_iov +
                                                      index * sizeof(iovec),
                                                  sizeof(iovec),
                                                  &copied);
            if (access_status != KERNEL_UACCESS_STATUS_OK ||
                copied != sizeof(iovec)) {
                files->record->statistics.write_failures++;
                *linux_result = total != 0U ? (int64_t)total : -KERNEL_EFAULT;
                files->record->statistics.bytes_written += total;
                return KERNEL_FILES_STATUS_OK;
            }
            pipe_status = kernel_pipe_write(description->pipe,
                                             mm,
                                             iovec.base,
                                             iovec.length,
                                             description->open_flags,
                                             &written);
            if (pipe_status != KERNEL_PIPE_STATUS_OK) {
                return KERNEL_FILES_STATUS_STATE;
            }
            if (written < 0) {
                *linux_result = total != 0U ? (int64_t)total : written;
                files->record->statistics.bytes_written += total;
                if (total == 0U) {
                    files->record->statistics.write_failures++;
                }
                return KERNEL_FILES_STATUS_OK;
            }
            total += (uint64_t)written;
            if ((uint64_t)written != iovec.length) {
                *linux_result = (int64_t)total;
                files->record->statistics.bytes_written += total;
                return KERNEL_FILES_STATUS_OK;
            }
        }
        files->record->statistics.bytes_written += total;
        *linux_result = (int64_t)total;
        return KERNEL_FILES_STATUS_OK;
    }

    for (index = 0U; index < iovcnt; index++) {
        struct kernel_uaccess_iovec iovec;
        size_t copied = 0U;
        uint64_t offset = 0U;
        enum kernel_uaccess_status access_status;

        access_status = kernel_copy_from_user(mm,
                                              &iovec,
                                              user_iov +
                                                  index * sizeof(iovec),
                                              sizeof(iovec),
                                              &copied);
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(iovec)) {
            files->record->statistics.write_failures++;
            *linux_result = total != 0U ? (int64_t)total
                                        : -KERNEL_EFAULT;
            files->record->statistics.bytes_written += total;
            return KERNEL_FILES_STATUS_OK;
        }
        if (kernel_user_range_check(iovec.base, (size_t)iovec.length) !=
            KERNEL_UACCESS_STATUS_OK) {
            *linux_result = total != 0U ? (int64_t)total
                                        : -KERNEL_EFAULT;
            files->record->statistics.bytes_written += total;
            return KERNEL_FILES_STATUS_OK;
        }
        while (offset < iovec.length) {
            size_t chunk = iovec.length - offset;
            size_t emitted = 0U;
            size_t position;

            if (chunk > sizeof(staging)) {
                chunk = sizeof(staging);
            }
            access_status = kernel_copy_from_user(mm,
                                                  staging,
                                                  iovec.base + offset,
                                                  chunk,
                                                  &emitted);
            total += emitted;
            for (position = 0U; position < emitted; position++) {
                kernel_console_putc((char)staging[position]);
            }
            if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
                *linux_result = total != 0U ? (int64_t)total
                                            : -KERNEL_EFAULT;
                files->record->statistics.bytes_written += total;
                return KERNEL_FILES_STATUS_OK;
            }
            if (access_status != KERNEL_UACCESS_STATUS_OK ||
                emitted != chunk) {
                return KERNEL_FILES_STATUS_STATE;
            }
            offset += emitted;
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

static void fill_linux_stat(
    struct kernel_linux_stat *stat,
    const struct kernel_open_file_description *description)
{
    uint64_t size = kernel_open_file_size(description);

    memset(stat, 0, sizeof(*stat));
    if (kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_CONSOLE) {
        /* The console is the character device this kernel exposes. */
        stat->st_mode = KERNEL_VFS_S_IFCHR | UINT32_C(0000600);
        stat->st_rdev = UINT64_C(0x501);
    } else if (kernel_open_file_kind(description) ==
               KERNEL_OPEN_FILE_KIND_PIPE) {
        stat->st_mode = KERNEL_VFS_S_IFIFO | UINT32_C(0000600);
    } else {
        stat->st_mode = kernel_open_file_mode(description);
        stat->st_rdev = 0U;
        stat->st_ino = kernel_vfs_file_inode(&description->file);
        stat->st_blocks = (int64_t)((size + 511U) / 512U);
    }
    stat->st_dev = 0U;
    stat->st_nlink = 1U;
    stat->st_uid = 0U;
    stat->st_gid = 0U;
    stat->st_pad1 = 0U;
    stat->st_size = (int64_t)size;
    stat->st_blksize = (int32_t)BOAROS_PAGE_SIZE;
    stat->st_pad2 = 0;
    stat->st_atime = 0;
    stat->st_atime_nsec = 0;
    stat->st_mtime = 0;
    stat->st_mtime_nsec = 0;
    stat->st_ctime = 0;
    stat->st_ctime_nsec = 0;
    stat->st_pad4 = 0U;
    stat->st_pad5 = 0U;
}

static int copy_stat_to_user(struct kernel_mm *mm,
                             uint64_t user_buffer,
                             const struct kernel_linux_stat *stat,
                             int64_t *linux_result)
{
    size_t copied = 0U;
    enum kernel_uaccess_status access_status;

    access_status = kernel_copy_to_user(mm,
                                        user_buffer,
                                        stat,
                                        sizeof(*stat),
                                        &copied);
    if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
        *linux_result = -KERNEL_EFAULT;
        return 0;
    }
    if (access_status != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(*stat)) {
        return -1;
    }
    return 1;
}

enum kernel_files_status kernel_files_lseek(
    struct kernel_files *files,
    int64_t fd,
    int64_t offset,
    uint64_t whence,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    int64_t current;
    int64_t target;

    if (!kernel_files_is_live(files) || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    description = lookup_description(files, fd);
    if (description == 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) ==
            KERNEL_OPEN_FILE_KIND_CONSOLE ||
        kernel_open_file_kind(description) == KERNEL_OPEN_FILE_KIND_PIPE) {
        *linux_result = -KERNEL_ESPIPE;
        return KERNEL_FILES_STATUS_OK;
    }
    current = (int64_t)kernel_open_file_offset(description);
    switch (whence) {
    case KERNEL_FILES_SEEK_SET:
        target = offset;
        break;
    case KERNEL_FILES_SEEK_CUR:
        if ((offset > 0 && current > INT64_MAX - offset) ||
            (offset < 0 && current < INT64_MIN - offset)) {
            *linux_result = -KERNEL_EINVAL;
            return KERNEL_FILES_STATUS_OK;
        }
        target = current + offset;
        break;
    case KERNEL_FILES_SEEK_END:
        current = (int64_t)kernel_open_file_size(description);
        if ((offset > 0 && current > INT64_MAX - offset) ||
            (offset < 0 && current < INT64_MIN - offset)) {
            *linux_result = -KERNEL_EINVAL;
            return KERNEL_FILES_STATUS_OK;
        }
        target = current + offset;
        break;
    default:
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (target < 0) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_seek(description, (uint64_t)target) !=
        KERNEL_OPEN_FILE_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    *linux_result = target;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_fstat(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    struct kernel_linux_stat stat;
    int copy_result;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    description = lookup_description(files, fd);
    if (description == 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    fill_linux_stat(&stat, description);
    copy_result = copy_stat_to_user(mm, user_buffer, &stat, linux_result);
    if (copy_result < 0) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (copy_result == 0) {
        return KERNEL_FILES_STATUS_OK;
    }
    *linux_result = 0;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_fstatat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint64_t user_buffer,
    uint64_t flags,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description = 0;
    struct kernel_vfs_mount *mount;
    char *path;
    struct kernel_linux_stat stat;
    size_t path_length;
    enum kernel_heap_status heap_status;
    enum kernel_fs_context_status fs_status;
    enum kernel_open_file_status open_status;
    int copy_result;
    int result;

    if (!kernel_files_is_live(files) || !kernel_fs_context_is_live(fs) ||
        mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if ((flags & ~(KERNEL_FILES_AT_SYMLINK_NOFOLLOW |
                   KERNEL_FILES_AT_EMPTY_PATH)) != 0U) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    heap_status = kernel_heap_allocate(files->heap,
                                       KERNEL_FS_PATH_MAX,
                                       (void **)&path);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY) {
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    {
        enum kernel_uaccess_status access_status =
            kernel_copy_string_from_user(mm,
                                         path,
                                         user_path,
                                         KERNEL_FS_PATH_MAX,
                                         &path_length);

        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            (void)release_or_queue_allocation(files, path);
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_FILES_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK) {
            (void)release_or_queue_allocation(files, path);
            return KERNEL_FILES_STATUS_STATE;
        }
    }
    if (path_length == 0U) {
        if ((flags & KERNEL_FILES_AT_EMPTY_PATH) == 0U) {
            (void)release_or_queue_allocation(files, path);
            *linux_result = -KERNEL_ENOENT;
            return KERNEL_FILES_STATUS_OK;
        }
        description = lookup_description(files, dirfd);
        if (description == 0) {
            (void)release_or_queue_allocation(files, path);
            *linux_result = -KERNEL_EBADF;
            return KERNEL_FILES_STATUS_OK;
        }
        if (kernel_open_file_acquire(description) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
            (void)release_or_queue_allocation(files, path);
            return KERNEL_FILES_STATUS_STATE;
        }
    } else {
        fs_status = kernel_fs_context_resolve_kernel_path(fs,
                                                          dirfd,
                                                          path,
                                                          path_length,
                                                          path,
                                                          KERNEL_FS_PATH_MAX,
                                                          &mount,
                                                          &result);
        if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
            (void)release_or_queue_allocation(files, path);
            return KERNEL_FILES_STATUS_STATE;
        }
        if (result != 0) {
            (void)release_or_queue_allocation(files, path);
            *linux_result = result;
            return KERNEL_FILES_STATUS_OK;
        }
            open_status = kernel_open_file_create(files->heap,
                                              mount,
                                              path,
                                              &description,
                                              &result);
        if (open_status != KERNEL_OPEN_FILE_STATUS_OK) {
            (void)release_or_queue_allocation(files, path);
            return open_status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                       ? KERNEL_FILES_STATUS_NO_MEMORY
                       : KERNEL_FILES_STATUS_STATE;
        }
        if (result != 0) {
            if (kernel_open_file_release(&description) !=
                KERNEL_OPEN_FILE_STATUS_OK) {
                (void)release_or_queue_allocation(files, path);
                return KERNEL_FILES_STATUS_STATE;
            }
            (void)release_or_queue_allocation(files, path);
            *linux_result = result;
            return KERNEL_FILES_STATUS_OK;
        }
    }
    (void)release_or_queue_allocation(files, path);
    fill_linux_stat(&stat, description);
    copy_result = copy_stat_to_user(mm, user_buffer, &stat, linux_result);
    if (description != 0 &&
        kernel_open_file_release(&description) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (copy_result < 0) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (copy_result == 0) {
        return KERNEL_FILES_STATUS_OK;
    }
    *linux_result = 0;
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status install_dup(
    struct kernel_files *files,
    struct kernel_open_file_description *description,
    uint32_t newfd,
    uint32_t fd_flags,
    int64_t *linux_result)
{
    files->record->slots[newfd].description = description;
    files->record->slots[newfd].flags = fd_flags;
    files->record->statistics.current_open_fds++;
    if (files->record->statistics.current_open_fds >
        files->record->statistics.peak_open_fds) {
        files->record->statistics.peak_open_fds =
            files->record->statistics.current_open_fds;
    }
    if ((fd_flags & KERNEL_FILES_FD_CLOEXEC) != 0U) {
        files->record->statistics.close_on_exec_fds++;
    }
    *linux_result = newfd;
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status ensure_slot_capacity(
    struct kernel_files *files,
    uint32_t needed,
    int64_t *linux_result)
{
    uint32_t old_capacity = files->record->statistics.capacity;
    uint32_t new_capacity;
    uint32_t index;
    enum kernel_heap_status heap_status;
    struct kernel_file_slot *resized;

    *linux_result = 0;
    if (needed <= old_capacity) {
        return KERNEL_FILES_STATUS_OK;
    }
    if (needed > KERNEL_FILES_MAX_CAPACITY) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    new_capacity = old_capacity;
    while (new_capacity < needed) {
        new_capacity *= 2U;
    }
    heap_status = kernel_heap_resize(files->heap,
                                     files->record->slots,
                                     (size_t)new_capacity *
                                         sizeof(struct kernel_file_slot),
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
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status find_two_free_fds(
    struct kernel_files *files,
    uint32_t *first,
    uint32_t *second,
    int64_t *linux_result)
{
    uint32_t found = 0U;
    uint32_t capacity;
    uint32_t offset;
    enum kernel_files_status status;

    if (files == 0 || first == 0 || second == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    *linux_result = 0;
    capacity = files->record->statistics.capacity;
    for (offset = 0U; offset < capacity && found < 2U; offset++) {
        uint32_t index = (files->record->next_fd + offset) % capacity;

        if (files->record->slots[index].description == 0) {
            if (found == 0U) {
                *first = index;
            } else {
                *second = index;
            }
            found++;
        }
    }
    if (found == 2U) {
        return KERNEL_FILES_STATUS_OK;
    }
    if (capacity == KERNEL_FILES_MAX_CAPACITY) {
        *linux_result = -KERNEL_EMFILE;
        return KERNEL_FILES_STATUS_OK;
    }
    status = ensure_slot_capacity(files,
                                   capacity + (2U - found),
                                   linux_result);
    if (status != KERNEL_FILES_STATUS_OK || *linux_result != 0) {
        return status;
    }
    capacity = files->record->statistics.capacity;
    for (offset = 0U; offset < capacity && found < 2U; offset++) {
        uint32_t index = (files->record->next_fd + offset) % capacity;

        if (files->record->slots[index].description == 0) {
            if (found == 0U) {
                *first = index;
            } else {
                *second = index;
            }
            found++;
        }
    }
    return found == 2U ? KERNEL_FILES_STATUS_OK
                       : KERNEL_FILES_STATUS_STATE;
}

static enum kernel_files_status release_uninstalled_description(
    struct kernel_files *files,
    struct kernel_open_file_description **owner)
{
    enum kernel_open_file_status status;

    if (owner == 0 || *owner == 0) {
        return KERNEL_FILES_STATUS_OK;
    }
    status = kernel_open_file_release(owner);
    if (status == KERNEL_OPEN_FILE_STATUS_OK) {
        return KERNEL_FILES_STATUS_OK;
    }
    if (status == KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED &&
        *owner != 0) {
        queue_description(files, *owner);
        *owner = 0;
        return KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
    }
    return KERNEL_FILES_STATUS_STATE;
}

enum kernel_files_status kernel_files_pipe2(
    struct kernel_files *files,
    struct kernel_mm *mm,
    uint64_t user_pipefd,
    uint64_t flags,
    int64_t *linux_result)
{
    struct kernel_pipe *pipe = 0;
    struct kernel_open_file_description *read_description = 0;
    struct kernel_open_file_description *write_description = 0;
    uint32_t read_fd = 0U;
    uint32_t write_fd = 0U;
    uint32_t fd_flags;
    int64_t result;
    int32_t pair[2];
    size_t copied = 0U;
    enum kernel_pipe_status pipe_status;
    enum kernel_open_file_status open_status;
    enum kernel_files_status status;
    enum kernel_uaccess_status access_status;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if ((flags & ~(KERNEL_FILES_O_CLOEXEC | KERNEL_FILES_O_NONBLOCK)) !=
        0U) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_user_range_check(user_pipefd, sizeof(pair)) !=
        KERNEL_UACCESS_STATUS_OK) {
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }
    pipe_status = kernel_pipe_create(files->heap, &pipe);
    if (pipe_status != KERNEL_PIPE_STATUS_OK) {
        if (pipe_status == KERNEL_PIPE_STATUS_CLEANUP_REQUIRED &&
            pipe != 0) {
            queue_pipe_cleanup(files, pipe);
            return KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
        }
        *linux_result = pipe_status == KERNEL_PIPE_STATUS_NO_MEMORY
                            ? -KERNEL_ENOMEM
                            : -KERNEL_EIO;
        return KERNEL_FILES_STATUS_OK;
    }
    open_status = kernel_open_file_create_pipe(files->heap,
                                               pipe,
                                               KERNEL_PIPE_ENDPOINT_READ,
                                               flags,
                                               &read_description);
    if (open_status != KERNEL_OPEN_FILE_STATUS_OK) {
        status = release_uninstalled_description(files, &read_description);
        if (kernel_pipe_destroy_unowned(pipe) != KERNEL_PIPE_STATUS_OK) {
            queue_pipe_cleanup(files, pipe);
            status = KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
        }
        if (status != KERNEL_FILES_STATUS_OK) {
            return status;
        }
        *linux_result = open_status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                            ? -KERNEL_ENOMEM
                            : -KERNEL_EIO;
        return KERNEL_FILES_STATUS_OK;
    }
    open_status = kernel_open_file_create_pipe(files->heap,
                                               pipe,
                                               KERNEL_PIPE_ENDPOINT_WRITE,
                                               flags,
                                               &write_description);
    if (open_status != KERNEL_OPEN_FILE_STATUS_OK) {
        enum kernel_files_status write_cleanup_status;
        enum kernel_files_status read_cleanup_status;

        write_cleanup_status = release_uninstalled_description(
            files,
            &write_description);
        read_cleanup_status = release_uninstalled_description(
            files,
            &read_description);
        status = write_cleanup_status != KERNEL_FILES_STATUS_OK
                     ? write_cleanup_status
                     : read_cleanup_status;
        /* The first endpoint owns the last pipe reference here.  Its
         * release either destroys the pipe or queues the OFD to retry that
         * release; never touch pipe after a successful read cleanup. */
        *linux_result = open_status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                            ? -KERNEL_ENOMEM
                            : -KERNEL_EIO;
        return status;
    }
    status = find_two_free_fds(files, &read_fd, &write_fd, &result);
    if (status != KERNEL_FILES_STATUS_OK || result != 0) {
        enum kernel_files_status cleanup_status;

        cleanup_status = release_uninstalled_description(files,
                                                          &write_description);
        {
            enum kernel_files_status read_cleanup_status =
                release_uninstalled_description(files, &read_description);

            if (cleanup_status == KERNEL_FILES_STATUS_OK) {
                cleanup_status = read_cleanup_status;
            }
        }
        if (cleanup_status != KERNEL_FILES_STATUS_OK) {
            return cleanup_status;
        }
        *linux_result = result;
        return status;
    }

    fd_flags = (flags & KERNEL_FILES_O_CLOEXEC) != 0U
                   ? KERNEL_FILES_FD_CLOEXEC
                   : 0U;
    files->record->slots[read_fd].description = read_description;
    files->record->slots[read_fd].flags = fd_flags;
    files->record->slots[write_fd].description = write_description;
    files->record->slots[write_fd].flags = fd_flags;
    read_description = 0;
    write_description = 0;
    files->record->statistics.current_open_fds += 2U;
    if (files->record->statistics.current_open_fds >
        files->record->statistics.peak_open_fds) {
        files->record->statistics.peak_open_fds =
            files->record->statistics.current_open_fds;
    }
    if (fd_flags != 0U) {
        files->record->statistics.close_on_exec_fds += 2U;
    }
    files->record->next_fd = (write_fd + 1U <
                              files->record->statistics.capacity)
                                 ? write_fd + 1U
                                 : 0U;
    pair[0] = (int32_t)read_fd;
    pair[1] = (int32_t)write_fd;
    access_status = kernel_copy_to_user(mm,
                                        user_pipefd,
                                        pair,
                                        sizeof(pair),
                                        &copied);
    if (access_status != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(pair)) {
        (void)detach_fd(files, write_fd);
        (void)detach_fd(files, read_fd);
        (void)drain_file_cleanup(files);
        (void)drain_pipe_cleanup(files);
        (void)drain_allocation_cleanup(files);
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }
    *linux_result = 0;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_dup(
    struct kernel_files *files,
    int64_t oldfd,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    uint32_t fd;
    int result;
    enum kernel_files_status status;

    if (!kernel_files_is_live(files) || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    description = lookup_description(files, oldfd);
    if (description == 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    status = find_free_fd(files, &fd, &result);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    if (result != 0) {
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_acquire(description) !=
        KERNEL_OPEN_FILE_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    /* dup never carries CLOEXEC across; only F_DUPFD_CLOEXEC sets it. */
    return install_dup(files, description, fd, 0U, linux_result);
}

enum kernel_files_status kernel_files_dup3(
    struct kernel_files *files,
    int64_t oldfd,
    int64_t newfd,
    uint64_t flags,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    uint32_t fd_flags = 0U;
    enum kernel_files_status status;

    if (!kernel_files_is_live(files) || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (newfd < 0 || (uint64_t)newfd >= KERNEL_FILES_MAX_CAPACITY ||
        oldfd == newfd ||
        (flags & ~LINUX_O_CLOEXEC) != 0U) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    description = lookup_description(files, oldfd);
    if (description == 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    status = ensure_slot_capacity(files, (uint32_t)newfd + 1U, linux_result);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    if (*linux_result != 0) {
        return KERNEL_FILES_STATUS_OK;
    }
    if (lookup_description(files, newfd) != 0 &&
        detach_fd(files, (uint32_t)newfd) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (kernel_open_file_acquire(description) !=
        KERNEL_OPEN_FILE_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if ((flags & LINUX_O_CLOEXEC) != 0U) {
        fd_flags = KERNEL_FILES_FD_CLOEXEC;
    }
    return install_dup(files, description, (uint32_t)newfd, fd_flags,
                       linux_result);
}

enum kernel_files_status kernel_files_dup2(
    struct kernel_files *files,
    int64_t oldfd,
    int64_t newfd,
    int64_t *linux_result)
{
    if (!kernel_files_is_live(files) || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (oldfd == newfd) {
        if (lookup_description(files, oldfd) == 0) {
            *linux_result = -KERNEL_EBADF;
            return KERNEL_FILES_STATUS_OK;
        }
        *linux_result = newfd;
        return KERNEL_FILES_STATUS_OK;
    }
    if (newfd < 0 || (uint64_t)newfd >= KERNEL_FILES_MAX_CAPACITY) {
        /* dup2 reports an out-of-range target as EBADF, unlike dup3. */
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    return kernel_files_dup3(files, oldfd, newfd, 0U, linux_result);
}

enum kernel_files_status kernel_files_fcntl(
    struct kernel_files *files,
    int64_t fd,
    uint64_t command,
    uint64_t argument,
    int64_t *linux_result)
{
    struct kernel_file_slot *slot;
    struct kernel_open_file_description *description;
    uint32_t fd_flags;

    if (!kernel_files_is_live(files) || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (command == KERNEL_FILES_F_DUPFD ||
        command == KERNEL_FILES_F_DUPFD_CLOEXEC) {
        uint32_t index;

        if ((int64_t)argument < 0) {
            *linux_result = -KERNEL_EINVAL;
            return KERNEL_FILES_STATUS_OK;
        }
        description = lookup_description(files, fd);
        if (description == 0) {
            *linux_result = -KERNEL_EBADF;
            return KERNEL_FILES_STATUS_OK;
        }
        for (index = (uint32_t)argument;
             index < files->record->statistics.capacity;
             index++) {
            if (files->record->slots[index].description == 0) {
                break;
            }
        }
        if (index == files->record->statistics.capacity) {
            *linux_result = -KERNEL_EMFILE;
            return KERNEL_FILES_STATUS_OK;
        }
        if (kernel_open_file_acquire(description) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        fd_flags = command == KERNEL_FILES_F_DUPFD_CLOEXEC
                       ? KERNEL_FILES_FD_CLOEXEC
                       : 0U;
        return install_dup(files, description, index, fd_flags,
                           linux_result);
    }
    if (fd < 0 ||
        (uint64_t)fd >= files->record->statistics.capacity ||
        files->record->slots[fd].description == 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    slot = &files->record->slots[fd];
    switch (command) {
    case KERNEL_FILES_F_GETFD:
        /* The fd flag is FD_CLOEXEC, value 1, not the O_CLOEXEC bit. */
        *linux_result = (slot->flags & KERNEL_FILES_FD_CLOEXEC) != 0U
                            ? 1
                            : 0;
        return KERNEL_FILES_STATUS_OK;
    case KERNEL_FILES_F_SETFD:
        fd_flags = (argument & 1U) != 0U ? KERNEL_FILES_FD_CLOEXEC : 0U;
        if ((fd_flags & KERNEL_FILES_FD_CLOEXEC) != 0U &&
            (slot->flags & KERNEL_FILES_FD_CLOEXEC) == 0U) {
            files->record->statistics.close_on_exec_fds++;
        } else if ((fd_flags & KERNEL_FILES_FD_CLOEXEC) == 0U &&
                   (slot->flags & KERNEL_FILES_FD_CLOEXEC) != 0U) {
            files->record->statistics.close_on_exec_fds--;
        }
        slot->flags = fd_flags;
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    case KERNEL_FILES_F_GETFL:
        *linux_result = (int64_t)kernel_open_file_flags(slot->description);
        return KERNEL_FILES_STATUS_OK;
    case KERNEL_FILES_F_SETFL:
        /*
         * musl passes O_LARGEFILE with every F_SETFL request on 64-bit
         * targets.  It is an accepted, non-modifiable status bit; only
         * APPEND and NONBLOCK are changed here.
         */
        if ((argument & ~(LINUX_O_APPEND | LINUX_O_NONBLOCK |
                          LINUX_O_LARGEFILE)) != 0U) {
            *linux_result = -KERNEL_EINVAL;
            return KERNEL_FILES_STATUS_OK;
        }
        slot->description->open_flags =
            (slot->description->open_flags &
             (uint32_t)~(LINUX_O_APPEND | LINUX_O_NONBLOCK)) |
            (uint32_t)(argument & (LINUX_O_APPEND | LINUX_O_NONBLOCK));
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    default:
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
}

enum kernel_files_status kernel_files_getdents(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    uint64_t count,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    unsigned char record[19U + 256U];
    char name[256];
    uint64_t inode;
    uint8_t type;
    uint64_t request;
    uint64_t total = 0U;
    uint64_t index;
    int fill_result;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    description = lookup_description(files, fd);
    if (description == 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(description) !=
        KERNEL_OPEN_FILE_KIND_DIRECTORY) {
        *linux_result = -KERNEL_ENOTDIR;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_user_range_check(user_buffer, (size_t)count) !=
        KERNEL_UACCESS_STATUS_OK) {
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

    /* The descriptor offset counts the entries already emitted; each
     * call re-walks the directory and skips past them. */
    index = kernel_open_file_offset(description);
    while (total < request) {
        size_t name_length;
        size_t record_length;
        size_t copied = 0U;
        uint16_t reported_length;
        uint16_t little;
        uint64_t cookie;
        enum kernel_uaccess_status access_status;

        fill_result = kernel_vfs_dir_entry(&description->file,
                                           index,
                                           &inode,
                                           &type,
                                           name,
                                           sizeof(name));
        if (fill_result == 0) {
            break;
        }
        if (fill_result < 0) {
            *linux_result = fill_result;
            return KERNEL_FILES_STATUS_OK;
        }
        name_length = strlen(name);
        record_length = (19U + name_length + 1U + 7U) & ~(size_t)7U;
        if ((uint64_t)record_length > request - total) {
            /* Linux answers EINVAL when nothing fits at all. */
            *linux_result = total == 0U ? -KERNEL_EINVAL : (int64_t)total;
            files->record->statistics.bytes_read += total;
            return KERNEL_FILES_STATUS_OK;
        }
        reported_length = (uint16_t)record_length;
        little = 1;
        if (*(unsigned char *)&little) {
            /* Little-endian host byte order for the fixed fields. */
            memcpy(&record[0], &inode, sizeof(inode));
            cookie = index + 1U;
            memcpy(&record[8], &cookie, sizeof(cookie));
        } else {
            for (uint8_t byte = 0U; byte < 8U; byte++) {
                record[byte] =
                    (unsigned char)((inode >> (8U * byte)) & 0xffU);
                record[8U + byte] =
                    (unsigned char)(((index + 1U) >> (8U * byte)) &
                                    0xffU);
            }
        }
        memcpy(&record[16], &reported_length, sizeof(reported_length));
        record[18] = type;
        memcpy(&record[19], name, name_length);
        record[19U + name_length] = '\0';
        for (size_t pad = 19U + name_length + 1U; pad < record_length;
             pad++) {
            record[pad] = 0U;
        }

        access_status = kernel_copy_to_user(mm,
                                            user_buffer + total,
                                            record,
                                            record_length,
                                            &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT ||
            copied != record_length) {
            *linux_result = total == 0U ? -KERNEL_EFAULT : (int64_t)total;
            files->record->statistics.bytes_read += total;
            return KERNEL_FILES_STATUS_OK;
        }
        total += record_length;
        index++;
        if (kernel_open_file_seek(description, index) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
    }
    files->record->statistics.bytes_read += total;
    *linux_result = (int64_t)total;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_close(
    struct kernel_files *files,
    int64_t fd,
    int64_t *linux_result)
{
    struct kernel_open_file_description *description;
    int cleanup_required = 0;

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
    if (drain_file_cleanup(files) != KERNEL_FILES_STATUS_OK) {
        cleanup_required = 1;
    }
    if (drain_pipe_cleanup(files) != KERNEL_FILES_STATUS_OK) {
        cleanup_required = 1;
    }
    if (drain_allocation_cleanup(files) != KERNEL_FILES_STATUS_OK) {
        cleanup_required = 1;
    }
    if (cleanup_required != 0) {
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
    if (drain_pipe_cleanup(files) != KERNEL_FILES_STATUS_OK) {
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
    int cleanup_required = 0;

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
    if (drain_file_cleanup(files) != KERNEL_FILES_STATUS_OK) {
        cleanup_required = 1;
    }
    if (drain_pipe_cleanup(files) != KERNEL_FILES_STATUS_OK) {
        cleanup_required = 1;
    }
    if (drain_allocation_cleanup(files) != KERNEL_FILES_STATUS_OK) {
        cleanup_required = 1;
    }
    if (cleanup_required != 0) {
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
