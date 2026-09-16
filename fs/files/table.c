#include "private.h"
#include "../open_file_internal.h"
#include "../pipe_internal.h"

#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LINUX_O_APPEND UINT64_C(00002000)
#define LINUX_O_NONBLOCK UINT64_C(00004000)
#define LINUX_O_LARGEFILE UINT64_C(00100000)
#define LINUX_O_CLOEXEC UINT64_C(02000000)

static int empty_files(const struct kernel_files *files)
{
    return files->state == KERNEL_FILES_EMPTY && files->heap == 0 &&
           files->record == 0;
}

int kernel_files_is_live(const struct kernel_files *files)
{
    return files != 0 && files->state == KERNEL_FILES_LIVE &&
           files->heap != 0 && files->record != 0 &&
           files->record->references != 0U &&
           files->record->slots != 0 &&
           files->record->statistics.capacity >=
               KERNEL_FILES_INITIAL_CAPACITY;
}

static void finish_files(struct kernel_files *files,
                         enum kernel_files_state state)
{
    files->heap = 0;
    files->record = 0;
    files->nofile_limit = 0U;
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
        (void)kernel_heap_release(heap, record);
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_FILES_STATUS_NO_MEMORY
                   : KERNEL_FILES_STATUS_STATE;
    }
    record->slots = slots;
    record->statistics.capacity = capacity;
    files->heap = heap;
    files->record = record;
    files->nofile_limit = KERNEL_FILES_MAX_CAPACITY;
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

enum kernel_files_status kernel_files_acquire(
    struct kernel_files *destination,
    const struct kernel_files *source)
{
    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_files(destination) || !kernel_files_is_live(source) ||
        source->record->references == UINT32_MAX) {
        return KERNEL_FILES_STATUS_STATE;
    }
    source->record->references++;
    destination->heap = source->heap;
    destination->record = source->record;
    destination->nofile_limit = source->nofile_limit;
    destination->state = KERNEL_FILES_LIVE;
    return KERNEL_FILES_STATUS_OK;
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
    destination->nofile_limit = source->nofile_limit;
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

enum kernel_files_status kernel_files_release_allocation(
    struct kernel_files *files,
    void *pointer)
{
    (void)kernel_heap_release(files->heap, pointer);
    return KERNEL_FILES_STATUS_OK;
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

enum kernel_files_status kernel_files_drain_file_cleanup(
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

static enum kernel_files_status find_fd_from(
    struct kernel_files *files, uint32_t minimum, uint32_t *fd,
    int64_t *linux_result)
{
    uint32_t index = minimum;
    uint32_t upper = files->nofile_limit < KERNEL_FILES_MAX_CAPACITY
                         ? files->nofile_limit : KERNEL_FILES_MAX_CAPACITY;

    *linux_result = 0;
    while (index < upper) {
        enum kernel_files_status status =
            ensure_slot_capacity(files, index + 1U, linux_result);

        if (status != KERNEL_FILES_STATUS_OK || *linux_result != 0) {
            return status;
        }
        while (index < files->record->statistics.capacity && index < upper) {
            if (files->record->slots[index].description == 0) {
                *fd = index;
                return KERNEL_FILES_STATUS_OK;
            }
            index++;
        }
    }
    *linux_result = -KERNEL_EMFILE;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_find_free_fd(
    struct kernel_files *files, uint32_t *fd, int *linux_result)
{
    int64_t result;
    enum kernel_files_status status =
        find_fd_from(files, files->record->next_fd, fd, &result);

    *linux_result = (int)result;
    return status;
}

static enum kernel_files_status install_owned_at(
    struct kernel_files *files,
    uint32_t fd,
    uint32_t fd_flags,
    struct kernel_open_file_description **owner)
{
    struct kernel_file_slot *slot;

    if (!kernel_files_is_live(files) || owner == 0 || *owner == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (fd >= files->record->statistics.capacity ||
        (fd_flags & ~KERNEL_FILES_FD_CLOEXEC) != 0U) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    slot = &files->record->slots[fd];
    if (files->record->next_fd > files->record->statistics.capacity ||
        slot->description != 0 || slot->flags != 0U ||
        files->record->statistics.current_open_fds >=
            files->record->statistics.capacity ||
        files->record->statistics.close_on_exec_fds >
            files->record->statistics.current_open_fds) {
        return KERNEL_FILES_STATUS_STATE;
    }

    slot->description = *owner;
    slot->flags = fd_flags;
    files->record->statistics.current_open_fds++;
    if (files->record->statistics.current_open_fds >
        files->record->statistics.peak_open_fds) {
        files->record->statistics.peak_open_fds =
            files->record->statistics.current_open_fds;
    }
    if ((fd_flags & KERNEL_FILES_FD_CLOEXEC) != 0U) {
        files->record->statistics.close_on_exec_fds++;
    }
    if (fd == files->record->next_fd) {
        do {
            files->record->next_fd++;
        } while (files->record->next_fd <
                     files->record->statistics.capacity &&
                 files->record->slots[files->record->next_fd].description !=
                     0);
    }
    *owner = 0;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_install_new_owned_at(
    struct kernel_files *files,
    uint32_t fd,
    uint32_t fd_flags,
    struct kernel_open_file_description **owner)
{
    return install_owned_at(files, fd, fd_flags, owner);
}

enum kernel_files_status kernel_files_install_shared_acquired_at(
    struct kernel_files *files,
    uint32_t fd,
    uint32_t fd_flags,
    struct kernel_open_file_description **owner)
{
    return install_owned_at(files, fd, fd_flags, owner);
}

void kernel_files_queue_description(
    struct kernel_files *files,
    struct kernel_open_file_description *description)
{
    description->cleanup_next = files->record->cleanup_files;
    files->record->cleanup_files = description;
}

struct kernel_open_file_description *kernel_files_lookup_description(
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
    description = kernel_files_lookup_description(files, fd);
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
        kernel_files_lookup_description(files, fd) != 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    open_status = kernel_open_file_create_console(files->heap, &description);
    if (open_status != KERNEL_OPEN_FILE_STATUS_OK) {
        return open_status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                   ? KERNEL_FILES_STATUS_NO_MEMORY
                   : KERNEL_FILES_STATUS_STATE;
    }
    if (kernel_files_install_new_owned_at(files,
                                          (uint32_t)fd,
                                          0U,
                                          &description) !=
        KERNEL_FILES_STATUS_OK) {
        if (kernel_open_file_release(&description) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        return KERNEL_FILES_STATUS_STATE;
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
        kernel_files_queue_description(files, description);
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

static enum kernel_files_status close_fd(struct kernel_files *files,
                                         uint32_t fd)
{
    struct kernel_file_slot *slot = &files->record->slots[fd];
    struct kernel_open_file_description *description;
    enum kernel_open_file_status open_status;
    enum kernel_files_status cleanup_status;

    if (slot->description == 0) {
        return KERNEL_FILES_STATUS_STATE;
    }
    description = slot->description;
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

    open_status = kernel_open_file_detach(&description);
    if (open_status == KERNEL_OPEN_FILE_STATUS_OK) {
        return KERNEL_FILES_STATUS_OK;
    }
    if (open_status != KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED ||
        description == 0) {
        return KERNEL_FILES_STATUS_STATE;
    }
    cleanup_status = cleanup_description(files, description);
    if (cleanup_status == KERNEL_FILES_STATUS_OK) {
        return KERNEL_FILES_STATUS_OK;
    }
    if (cleanup_status != KERNEL_FILES_STATUS_CLEANUP_REQUIRED) {
        return KERNEL_FILES_STATUS_STATE;
    }
    kernel_files_queue_description(files, description);
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status install_dup(
    struct kernel_files *files,
    struct kernel_open_file_description **acquired_owner,
    uint32_t newfd,
    uint32_t fd_flags,
    int64_t *linux_result)
{
    enum kernel_files_status status =
        kernel_files_install_shared_acquired_at(files,
                                                newfd,
                                                fd_flags,
                                                acquired_owner);

    if (status != KERNEL_FILES_STATUS_OK) {
        enum kernel_open_file_status open_status =
            kernel_open_file_release(acquired_owner);

        return open_status == KERNEL_OPEN_FILE_STATUS_OK
                   ? status
                   : KERNEL_FILES_STATUS_STATE;
    }
    *linux_result = newfd;
    return KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status find_two_free_fds(
    struct kernel_files *files, uint32_t *first, uint32_t *second,
    int64_t *linux_result)
{
    enum kernel_files_status status =
        find_fd_from(files, files->record->next_fd, first, linux_result);

    if (status != KERNEL_FILES_STATUS_OK || *linux_result != 0) {
        return status;
    }
    return find_fd_from(files, *first + 1U, second, linux_result);
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
        kernel_files_queue_description(files, *owner);
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
        (void)kernel_pipe_destroy_unowned(pipe);
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
        /* The first endpoint owns the last pipe reference here.  Do not
         * touch the pipe after the read description is released. */
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
    status = kernel_files_install_new_owned_at(files,
                                               read_fd,
                                               fd_flags,
                                               &read_description);
    if (status != KERNEL_FILES_STATUS_OK) {
        (void)release_uninstalled_description(files, &write_description);
        (void)release_uninstalled_description(files, &read_description);
        return status;
    }
    status = kernel_files_install_new_owned_at(files,
                                               write_fd,
                                               fd_flags,
                                               &write_description);
    if (status != KERNEL_FILES_STATUS_OK) {
        (void)detach_fd(files, read_fd);
        (void)release_uninstalled_description(files, &write_description);
        return status;
    }
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
        (void)kernel_files_drain_file_cleanup(files);
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
    description = kernel_files_lookup_description(files, oldfd);
    if (description == 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    status = kernel_files_find_free_fd(files, &fd, &result);
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
    return install_dup(files, &description, fd, 0U, linux_result);
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
    if (oldfd == newfd || (flags & ~LINUX_O_CLOEXEC) != 0U) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (newfd < 0 || (uint64_t)newfd >= files->nofile_limit ||
        (uint64_t)newfd >= KERNEL_FILES_MAX_CAPACITY) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    description = kernel_files_lookup_description(files, oldfd);
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
    if (kernel_files_lookup_description(files, newfd) != 0 &&
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
    return install_dup(files, &description, (uint32_t)newfd, fd_flags,
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
        if (kernel_files_lookup_description(files, oldfd) == 0) {
            *linux_result = -KERNEL_EBADF;
            return KERNEL_FILES_STATUS_OK;
        }
        *linux_result = newfd;
        return KERNEL_FILES_STATUS_OK;
    }
    if (newfd < 0 || (uint64_t)newfd >= files->nofile_limit ||
        (uint64_t)newfd >= KERNEL_FILES_MAX_CAPACITY) {
        /* Both dup2 and dup3 reject an out-of-range target with EBADF. */
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

        if (argument >= files->nofile_limit ||
            argument >= KERNEL_FILES_MAX_CAPACITY) {
            *linux_result = -KERNEL_EINVAL;
            return KERNEL_FILES_STATUS_OK;
        }
        description = kernel_files_lookup_description(files, fd);
        if (description == 0) {
            *linux_result = -KERNEL_EBADF;
            return KERNEL_FILES_STATUS_OK;
        }
        enum kernel_files_status status =
            find_fd_from(files, (uint32_t)argument, &index, linux_result);

        if (status != KERNEL_FILES_STATUS_OK || *linux_result != 0) {
            return status;
        }
        if (kernel_open_file_acquire(description) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
            return KERNEL_FILES_STATUS_STATE;
        }
        fd_flags = command == KERNEL_FILES_F_DUPFD_CLOEXEC
                       ? KERNEL_FILES_FD_CLOEXEC
                       : 0U;
        return install_dup(files, &description, index, fd_flags,
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

enum kernel_files_status kernel_files_close(
    struct kernel_files *files,
    int64_t fd,
    int64_t *linux_result)
{
    if (!kernel_files_is_live(files) || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    files->record->statistics.close_calls++;
    if (kernel_files_lookup_description(files, fd) == 0) {
        files->record->statistics.close_failures++;
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (close_fd(files, (uint32_t)fd) != KERNEL_FILES_STATUS_OK) {
        return KERNEL_FILES_STATUS_STATE;
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
    if (kernel_files_drain_file_cleanup(files) != KERNEL_FILES_STATUS_OK) {
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
        files->record->references == 0U) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (files->state == KERNEL_FILES_LIVE &&
        files->record->references > 1U) {
        files->record->references--;
        finish_files(files, KERNEL_FILES_RELEASED);
        return KERNEL_FILES_STATUS_OK;
    }
    if (files->record->references != 1U) {
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
    if (kernel_files_drain_file_cleanup(files) != KERNEL_FILES_STATUS_OK) {
        cleanup_required = 1;
    }
    if (cleanup_required != 0) {
        return KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
    }
    if (files->record->slots != 0) {
        (void)kernel_heap_release(files->heap,
                                files->record->slots);
        files->record->slots = 0;
    }
    (void)kernel_heap_release(files->heap, files->record);
    finish_files(files, KERNEL_FILES_RELEASED);
    return KERNEL_FILES_STATUS_OK;
}
