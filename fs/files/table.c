#include <kernel/scheduler.h>
#include "private.h"
#include "../open_file_internal.h"
#include "../pipe_internal.h"
#include "../record_lock.h"
#include <kernel/mqueue.h>
#include <kernel/task.h>
#include "../vfs_internal.h"
#include "../char_device_internal.h"
#include "../block_file_internal.h"

#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/irq.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/uaccess.h>
#include <kernel/task.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LINUX_O_APPEND UINT64_C(00002000)
#define LINUX_O_ACCMODE UINT64_C(00000003)
#define LINUX_O_NONBLOCK UINT64_C(00004000)
#define LINUX_O_LARGEFILE UINT64_C(00100000)
#define LINUX_O_CLOEXEC UINT64_C(02000000)

enum kernel_files_status kernel_files_ioctl(
    struct kernel_files *files, struct kernel_mm *mm, int64_t fd,
    uint64_t command, uint64_t argument, int64_t *linux_result)
{
    KERNEL_SERVICE_SCOPE(service);
    struct kernel_open_file_description *description = 0;
    if (!kernel_files_is_live(files) || !linux_result)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    enum kernel_files_status status = kernel_files_pin_data(files, fd, &description,
                                                       linux_result);
    if (status != KERNEL_FILES_STATUS_OK || *linux_result) return status;
    *linux_result = description->block ? kernel_block_file_ioctl(files, &description, mm, command, argument) :
        description->device && description->device->ioctl
        ? description->device->ioctl(description->device_instance, kernel_task_current(), files, &description, mm, command, argument) : -KERNEL_ENOTTY;
    enum kernel_open_file_status release = kernel_open_file_release(&description);
    if (release == KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED && description) {
        kernel_files_queue_description(files, description);
        return KERNEL_FILES_STATUS_OK;
    }
    return release == KERNEL_OPEN_FILE_STATUS_OK ? KERNEL_FILES_STATUS_OK
                                                : KERNEL_FILES_STATUS_STATE;
}

static int empty_files(const struct kernel_files *files)
{
    return files->state == KERNEL_FILES_EMPTY && files->heap == 0 &&
           files->record == 0;
}

int kernel_files_is_live(const struct kernel_files *files)
{
    if (!files || files->state != KERNEL_FILES_LIVE || !files->heap || !files->record)
        return 0;
    return 1;
}
static struct kernel_open_file_description *lookup_locked(
    const struct kernel_files *files, int64_t fd)
{
    return fd >= 0 && (uint64_t)fd < files->record->statistics.capacity
        ? files->record->slots[fd].description : 0;
}
int kernel_files_next_open_fd(const struct kernel_files *files, int after, int *fd)
{
    if (!kernel_files_is_live(files) || !fd || after < -1) return -KERNEL_EINVAL;
    KERNEL_FILES_SCOPE(guard, files);
    for (uint32_t i = (uint32_t)(after + 1); i < files->record->statistics.capacity; i++)
        if (files->record->slots[i].description) { *fd = (int)i; return 0; }
    return -KERNEL_ENOENT;
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
    kernel_raw_lock_init(&record->metadata_lock, KERNEL_RAW_RANK_OBJECT);
    heap_status = kernel_heap_allocate_zeroed(heap,
                                              capacity,
                                              sizeof(*slots),
                                              (void **)&slots);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        kernel_heap_release(heap, record);
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

static int description_in_domain(const struct kernel_files *files,
    const struct kernel_open_file_description *file)
{
    if (!file) return 1;
    if (file->heap != files->heap || file->ep_items || file->record_locks ||
        file->device_opened || file->generated_ready) return 0;
    if (file->kind == KERNEL_OPEN_FILE_KIND_REGULAR)
        return !kernel_open_file_check_io_domain(file, files->heap, files->heap->page_allocator);
    return file->kind == KERNEL_OPEN_FILE_KIND_PIPE && file->pipe &&
        file->pipe->heap == files->heap && !file->pipe->fifo_node &&
        file->pipe->allocator == files->heap->page_allocator && !file->file.private_data && !kernel_pipe_check_task_domain(file->pipe);
}

/* 只检查受控任务的实际消费者；普通表、独立OFD与别名使用同一套同步。 */
enum kernel_files_status kernel_files_check_task_domain(
    const struct kernel_files *files, const struct kernel_mm *mm)
{
    kernel_assert_service_context();
    if (!kernel_files_is_live(files) || kernel_mm_check_files_domain(mm, files->heap))
        return KERNEL_FILES_STATUS_STATE;
    uint32_t capacity;
    { KERNEL_FILES_SCOPE(guard, files); capacity = files->record->statistics.capacity; }
    struct kernel_open_file_description **owners;
    if (kernel_heap_allocate_zeroed(files->heap, capacity, sizeof(*owners), (void **)&owners))
        return KERNEL_FILES_STATUS_NO_MEMORY;
    enum kernel_files_status status = KERNEL_FILES_STATUS_OK;
    {
        KERNEL_FILES_SCOPE(guard, files);
        if (capacity != files->record->statistics.capacity || files->record->record_locks ||
            files->record->retired) status = KERNEL_FILES_STATUS_STATE;
        else for (uint32_t i = 0; i < capacity; i++) {
            struct kernel_open_file_description *file = files->record->slots[i].description;
            if (files->record->slots[i].flags & KERNEL_FILES_FD_RESERVED) {
                status = KERNEL_FILES_STATUS_STATE;
                break;
            }
            if (file && kernel_open_file_acquire(file)) __builtin_trap();
            owners[i] = file;
        }
    }
    for (uint32_t i = 0; i < capacity; i++) {
        if (!description_in_domain(files, owners[i])) status = KERNEL_FILES_STATUS_STATE;
        if (owners[i] && kernel_open_file_release(&owners[i])) __builtin_trap();
    }
    kernel_heap_release(files->heap, owners);
    return status;
}

enum kernel_files_status kernel_files_acquire(
    struct kernel_files *destination,
    const struct kernel_files *source)
{
    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_files(destination) || !kernel_files_is_live(source) ||
        source->record == 0) {
        return KERNEL_FILES_STATUS_STATE;
    }
    KERNEL_FILES_SCOPE(guard, source);
    if (!source->record->references || source->record->references == UINT32_MAX)
        return KERNEL_FILES_STATUS_STATE;
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
    kernel_assert_service_context();
    uint32_t index;
    enum kernel_files_status status;

    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_files(destination) || !kernel_files_is_live(source)) {
        return KERNEL_FILES_STATUS_STATE;
    }
Retry:
    ;
    uint32_t capacity;
    { KERNEL_FILES_SCOPE(guard,source);
      capacity=source->record->statistics.capacity; }
    status = create_files(destination,
                          source->heap,
                          capacity);
    if (status != KERNEL_FILES_STATUS_OK) {
        return status;
    }
    destination->record->next_fd = 0;
    destination->nofile_limit = source->nofile_limit;
    struct kernel_raw_guard source_guard={0};
    kernel_raw_lock_acquire(&source->record->metadata_lock,&source_guard);
    if (source->record->statistics.capacity!=capacity) {
        kernel_raw_lock_release(&source_guard);
        if (kernel_files_release(destination)) __builtin_trap();
        *destination=(struct kernel_files){0};
        goto Retry;
    }
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
        if (description) destination->record->slots[index] = source->record->slots[index];
    }
    if (index == source->record->statistics.capacity) {
        destination->record->statistics.peak_open_fds =
            destination->record->statistics.current_open_fds;
        kernel_raw_lock_release(&source_guard);
        return KERNEL_FILES_STATUS_OK;
    }
    kernel_raw_lock_release(&source_guard);
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

void kernel_files_release_allocation(
    struct kernel_files *files,
    void *pointer)
{
    kernel_heap_release(files->heap, pointer);
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
    if (!__atomic_load_n(&files->record->cleanup_files,__ATOMIC_ACQUIRE)) return KERNEL_FILES_STATUS_OK;
    struct kernel_open_file_description *pending;
    { KERNEL_FILES_SCOPE(guard, files);
      pending=files->record->cleanup_files; files->record->cleanup_files=0; }
    struct kernel_open_file_description *failed = 0;
    while (pending) {
        struct kernel_open_file_description *description = pending;
        pending = description->cleanup_next;
        description->cleanup_next = 0;
        enum kernel_files_status status = cleanup_description(files, description);
        if (status != KERNEL_FILES_STATUS_OK) {
            if (status != KERNEL_FILES_STATUS_CLEANUP_REQUIRED) __builtin_trap();
            description->cleanup_next = failed;
            failed = description;
        }
    }
    int error = failed != 0;
    while (failed) {
        struct kernel_open_file_description *description = failed;
        failed = description->cleanup_next;
        description->cleanup_next = 0;
        kernel_files_queue_description(files, description);
    }
    return error ? KERNEL_FILES_STATUS_CLEANUP_REQUIRED : KERNEL_FILES_STATUS_OK;
}

static enum kernel_files_status ensure_slot_capacity(
    struct kernel_files *files, uint32_t needed, int64_t *linux_result)
{
    *linux_result = 0;
    if (needed > KERNEL_FILES_MAX_CAPACITY) { *linux_result=-KERNEL_EBADF; return KERNEL_FILES_STATUS_OK; }
    uint32_t capacity;
    { KERNEL_FILES_SCOPE(guard, files); capacity=files->record->statistics.capacity; }
    if (needed <= capacity) return KERNEL_FILES_STATUS_OK;
    while (capacity < needed) capacity *= 2;
    struct kernel_file_slot *candidate=0, *old=0;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(files->heap,
        capacity, sizeof(*candidate), (void **)&candidate);
    if (status == KERNEL_HEAP_STATUS_EMPTY) { *linux_result=-KERNEL_ENOMEM; return KERNEL_FILES_STATUS_OK; }
    if (status != KERNEL_HEAP_STATUS_OK) return KERNEL_FILES_STATUS_STATE;
    {
        KERNEL_FILES_SCOPE(guard, files);
        uint32_t current=files->record->statistics.capacity;
        if (needed > current) {
            if (current >= capacity || current > KERNEL_FILES_MAX_CAPACITY/2) __builtin_trap();
            /* 最大旧数组512槽，即8KiB；发布后旧数组只有本调用者拥有。 */
            memcpy(candidate, files->record->slots, current * sizeof(*candidate));
            old=files->record->slots;
            files->record->slots=candidate;
            files->record->statistics.capacity=capacity;
            candidate=0;
        }
    }
    if (old) kernel_heap_release(files->heap, old);
    if (candidate) kernel_heap_release(files->heap, candidate);
    return KERNEL_FILES_STATUS_OK;
}
static uint32_t find_empty_locked(const struct kernel_files *files, uint32_t minimum)
{
    uint32_t upper=files->nofile_limit < files->record->statistics.capacity
        ? files->nofile_limit : files->record->statistics.capacity;
    for (uint32_t i=minimum; i<upper; i++)
        if (!files->record->slots[i].description && !files->record->slots[i].flags) return i;
    return upper;
}
/* 找到编号时取得预留owner，直到安装或取消，close/dup不能接管它。 */
static enum kernel_files_status find_fd_from(
    struct kernel_files *files, uint32_t minimum, uint32_t *fd, int64_t *linux_result, int hint)
{
    *linux_result=0;
    for (;;) {
        uint32_t needed;
        {
            KERNEL_FILES_SCOPE(guard, files);
            uint32_t index=find_empty_locked(files, hint ? files->record->next_fd : minimum);
            if (index < files->record->statistics.capacity && index < files->nofile_limit) {
                files->record->slots[index].flags=KERNEL_FILES_FD_RESERVED;
                if (index==files->record->next_fd) {
                    do { files->record->next_fd++; }
                    while (files->record->next_fd<files->record->statistics.capacity &&
                        (files->record->slots[files->record->next_fd].description ||
                         files->record->slots[files->record->next_fd].flags));
                }
                *fd=index;
                return KERNEL_FILES_STATUS_OK;
            }
            if (index >= files->nofile_limit || index >= KERNEL_FILES_MAX_CAPACITY) {
                *linux_result=-KERNEL_EMFILE; return KERNEL_FILES_STATUS_OK;
            }
            needed=index+1;
        }
        enum kernel_files_status status=ensure_slot_capacity(files, needed, linux_result);
        if (status || *linux_result) return status;
    }
}
enum kernel_files_status kernel_files_find_free_fd(
    struct kernel_files *files, uint32_t *fd, int *linux_result)
{
    if (!kernel_files_is_live(files) || !fd || !linux_result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    int64_t result;
    enum kernel_files_status status=find_fd_from(files, 0, fd, &result, 1);
    *linux_result=(int)result;
    return status;
}

static enum kernel_files_status install_owned_locked(
    struct kernel_files *files,
    uint32_t fd,
    uint32_t fd_flags,
    struct kernel_open_file_description **owner)
{
    struct kernel_file_slot *slot;

    if (owner == 0 || *owner == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (fd >= files->record->statistics.capacity ||
        (fd_flags & ~KERNEL_FILES_FD_CLOEXEC) != 0U) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    slot = &files->record->slots[fd];
    if (files->record->next_fd > files->record->statistics.capacity ||
        slot->description != 0 || (slot->flags != 0U && slot->flags != KERNEL_FILES_FD_RESERVED) ||
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
                 (files->record->slots[files->record->next_fd].description != 0 ||
                  files->record->slots[files->record->next_fd].flags != 0));
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
    if (!kernel_files_is_live(files)) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    KERNEL_FILES_SCOPE(guard, files);
    return install_owned_locked(files, fd, fd_flags, owner);
}

enum kernel_files_status kernel_files_install_shared_acquired_at(
    struct kernel_files *files,
    uint32_t fd,
    uint32_t fd_flags,
    struct kernel_open_file_description **owner)
{
    if (!kernel_files_is_live(files)) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    KERNEL_FILES_SCOPE(guard, files);
    return install_owned_locked(files, fd, fd_flags, owner);
}

enum kernel_files_status kernel_files_install_pair_reserved(
    struct kernel_files_fd_reservation *first, struct kernel_files_fd_reservation *second,
    uint32_t flags, struct kernel_open_file_description **a,
    struct kernel_open_file_description **b)
{
    if (!first || !second || (flags & ~KERNEL_FILES_FD_CLOEXEC) ||
        !first->files || first->files!=second->files || first->fd==second->fd ||
        !a || !*a || !b || !*b) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    struct kernel_files *files=first->files;
    KERNEL_FILES_SCOPE(guard, files);
    if (first->fd>=files->record->statistics.capacity || second->fd>=files->record->statistics.capacity ||
        files->record->slots[first->fd].flags!=KERNEL_FILES_FD_RESERVED ||
        files->record->slots[second->fd].flags!=KERNEL_FILES_FD_RESERVED) __builtin_trap();
    if (install_owned_locked(files,first->fd,flags,a) ||
        install_owned_locked(files,second->fd,flags,b)) __builtin_trap();
    first->files=second->files=0;
    return KERNEL_FILES_STATUS_OK;
}

void kernel_files_queue_description(
    struct kernel_files *files,
    struct kernel_open_file_description *description)
{
    KERNEL_FILES_SCOPE(guard, files);
    description->cleanup_next = files->record->cleanup_files;
    files->record->cleanup_files = description;
}

void kernel_files_cancel_reservation(struct kernel_files_fd_reservation *reservation)
{
    if (!reservation->files) return;
    KERNEL_FILES_SCOPE(guard, reservation->files);
    struct kernel_files_record *record = reservation->files->record;
    if (record->slots[reservation->fd].description ||
        record->slots[reservation->fd].flags != KERNEL_FILES_FD_RESERVED) __builtin_trap();
    record->slots[reservation->fd].flags = 0;
    if (reservation->fd < record->next_fd) record->next_fd = reservation->fd;
    reservation->files = 0;
}

void kernel_files_pin_guard_release(struct kernel_files_pin_guard *guard)
{
    if (!guard->description) return;
    enum kernel_open_file_status status = kernel_open_file_release(&guard->description);
    if (status == KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED && guard->description) {
        kernel_files_queue_description(guard->files, guard->description);
        guard->description = 0;
    } else if (status != KERNEL_OPEN_FILE_STATUS_OK) __builtin_trap();
}
struct kernel_open_file_description *kernel_files_hold_fd(
    struct kernel_files *files, int64_t fd, struct kernel_files_pin_guard *guard)
{
    if (guard->description) __builtin_trap();
    KERNEL_FILES_SCOPE(guard_lock, files);
    struct kernel_open_file_description *description = lookup_locked(files, fd);
    if (description && kernel_open_file_acquire(description) != KERNEL_OPEN_FILE_STATUS_OK) __builtin_trap();
    *guard = (struct kernel_files_pin_guard){files, description};
    return description;
}
void kernel_files_path_guard_release(struct kernel_vfs_path **path)
{
    if (*path && kernel_vfs_path_release(path) != 0) __builtin_trap();
}

int kernel_files_fd_matches(struct kernel_files *files, int64_t fd,
    const struct kernel_open_file_description *description)
{
    if (!kernel_files_is_live(files)) return 0;
    KERNEL_FILES_SCOPE(guard, files);
    return lookup_locked(files, fd)==description;
}
enum kernel_files_status kernel_files_pin_data(
    struct kernel_files *files, int64_t fd,
    struct kernel_open_file_description **description, int64_t *result)
{
    enum kernel_files_status status = kernel_files_pin(files, fd, description, result);
    if (status != KERNEL_FILES_STATUS_OK || *result || !*description) return status;
    if (kernel_open_file_kind(*description) == KERNEL_OPEN_FILE_KIND_PATH) {
        if (kernel_open_file_release(description) != KERNEL_OPEN_FILE_STATUS_OK) __builtin_trap();
        *result = -KERNEL_EBADF;
    }
    return status;
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
    KERNEL_FILES_SCOPE(guard, files);
    description = lookup_locked(files, fd);
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
        !kernel_files_fd_matches(files, fd, 0)) {
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

enum kernel_files_status kernel_files_open_boot_console(
    struct kernel_files *files, struct kernel_vfs_path *root,
    int64_t fd, int64_t *linux_result)
{
    if (!kernel_files_is_live(files) || !root || !linux_result)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (fd < 0 || (uint64_t)fd >= files->record->statistics.capacity ||
        !kernel_files_fd_matches(files, fd, 0)) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    struct kernel_vfs_stat stat;
    int result = kernel_vfs_stat_at(root, root, "/dev/console", 1, &stat);
    if (result == -KERNEL_ENOENT)
        return kernel_files_open_console(files, fd, linux_result);
    if (result) {
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    if ((stat.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFCHR ||
        stat.rdev != UINT64_C(0x501)) {
        *linux_result = -KERNEL_ENXIO;
        return KERNEL_FILES_STATUS_OK;
    }
    struct kernel_open_file_description *description = 0;
    enum kernel_open_file_status open_status = kernel_open_file_create_at(
        files->heap, root, root, "/dev/console", &(const struct kernel_vfs_open_request){.flags=KERNEL_VFS_OPEN_FOLLOW}, &description, NULL, &result);
    if (open_status != KERNEL_OPEN_FILE_STATUS_OK)
        return open_status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY
            ? KERNEL_FILES_STATUS_NO_MEMORY : KERNEL_FILES_STATUS_STATE;
    if (result) {
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    /* 启动前已有根盘 inode；标准 fd 持有它，proc 链接才有真实路径身份。 */
    description->device = kernel_char_device_lookup(stat.rdev);
    if (!description->device) __builtin_trap();
    description->kind = description->device->kind;
    description->open_flags = 2U;
    result = kernel_char_device_open(description, description->open_flags);
    if (result) {
        kernel_files_queue_description(files, description);
        (void)kernel_files_drain_file_cleanup(files);
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_files_install_new_owned_at(files, (uint32_t)fd, 0U,
                                          &description) != KERNEL_FILES_STATUS_OK) {
        kernel_files_queue_description(files, description);
        (void)kernel_files_drain_file_cleanup(files);
        return KERNEL_FILES_STATUS_STATE;
    }
    *linux_result = 0;
    return KERNEL_FILES_STATUS_OK;
}

/* 锁内摘下槽owner；所有后端、record lock和末引用处理留在锁外。 */
static struct kernel_open_file_description *detach_fd_locked(
    struct kernel_files *files, uint32_t fd)
{
    struct kernel_file_slot *slot = &files->record->slots[fd];
    struct kernel_open_file_description *description = slot->description;
    if (!description) return 0;
    slot->description = 0;
    if (slot->flags & KERNEL_FILES_FD_CLOEXEC) {
        if (!files->record->statistics.close_on_exec_fds) __builtin_trap();
        files->record->statistics.close_on_exec_fds--;
    }
    slot->flags = 0;
    if (!files->record->statistics.current_open_fds) __builtin_trap();
    files->record->statistics.current_open_fds--;
    if (fd < files->record->next_fd) files->record->next_fd = fd;
    return description;
}
static enum kernel_files_status release_detached(
    struct kernel_files *files, struct kernel_open_file_description *description,
    int cleanup, struct kernel_task *closing)
{
    /* FD摘除才撤通知；瞬时OFD借用的归还不会关闭登记。 */
    if(description->kind==KERNEL_OPEN_FILE_KIND_MQUEUE)
        kernel_mqueue_close_fd(description,closing);
    struct kernel_vfs_node *node = kernel_open_file_node(description);
    int has_locks=node && __atomic_load_n(&files->record->record_locks,__ATOMIC_ACQUIRE);
    KERNEL_SERVICE_SCOPE_IF(service, has_locks);
    if (has_locks)
        kernel_record_lock_release(kernel_vfs_node_record_locks(node),
                                   &files->record->record_locks, files->heap);
    enum kernel_open_file_status status = kernel_open_file_release(&description);
    if (status == KERNEL_OPEN_FILE_STATUS_OK) return KERNEL_FILES_STATUS_OK;
    if (status != KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED || !description)
        return KERNEL_FILES_STATUS_STATE;
    if (cleanup) {
        enum kernel_files_status result = cleanup_description(files, description);
        if (result == KERNEL_FILES_STATUS_OK) return result;
        if (result != KERNEL_FILES_STATUS_CLEANUP_REQUIRED) return KERNEL_FILES_STATUS_STATE;
    }
    kernel_files_queue_description(files, description);
    return KERNEL_FILES_STATUS_OK;
}
static enum kernel_files_status dup_from(struct kernel_files *files,
    int64_t oldfd, uint32_t minimum, uint32_t flags, int64_t *result)
{
    for (;;) {
        uint32_t needed;
        {
            KERNEL_FILES_SCOPE(guard, files);
            struct kernel_open_file_description *source=lookup_locked(files, oldfd);
            if (!source) { *result=-KERNEL_EBADF; return KERNEL_FILES_STATUS_OK; }
            uint32_t fd=find_empty_locked(files, minimum);
            if (fd < files->record->statistics.capacity && fd < files->nofile_limit) {
                if (kernel_open_file_acquire(source)) __builtin_trap();
                enum kernel_files_status status=install_owned_locked(files, fd, flags, &source);
                if (status) __builtin_trap();
                *result=fd; return KERNEL_FILES_STATUS_OK;
            }
            if (fd >= files->nofile_limit || fd >= KERNEL_FILES_MAX_CAPACITY) {
                *result=-KERNEL_EMFILE; return KERNEL_FILES_STATUS_OK;
            }
            needed=fd+1;
        }
        enum kernel_files_status status=ensure_slot_capacity(files, needed, result);
        if (status || *result) return status;
    }
}

enum kernel_files_status kernel_files_find_two_free_fds(
    struct kernel_files *files, uint32_t *first, uint32_t *second, int64_t *result)
{
    int value;
    enum kernel_files_status status=kernel_files_find_free_fd(files, first, &value);
    *result=value;
    if (status || value) return status;
    status=kernel_files_find_free_fd(files, second, &value);
    *result=value;
    if (status || *result) {
        struct kernel_files_fd_reservation reservation={files,*first};
        kernel_files_cancel_reservation(&reservation);
    }
    return status;
}

enum kernel_files_status kernel_files_release_uninstalled_description(
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
        status = kernel_files_release_uninstalled_description(files, &read_description);
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

        write_cleanup_status = kernel_files_release_uninstalled_description(
            files,
            &write_description);
        read_cleanup_status = kernel_files_release_uninstalled_description(
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
    status = kernel_files_find_two_free_fds(files, &read_fd, &write_fd, &result);
    if (status != KERNEL_FILES_STATUS_OK || result != 0) {
        enum kernel_files_status cleanup_status;

        cleanup_status = kernel_files_release_uninstalled_description(files,
                                                          &write_description);
        {
            enum kernel_files_status read_cleanup_status =
                kernel_files_release_uninstalled_description(files, &read_description);

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

    struct kernel_files_fd_reservation read_res __attribute__((cleanup(kernel_files_cancel_reservation)))={files,read_fd};
    struct kernel_files_fd_reservation write_res __attribute__((cleanup(kernel_files_cancel_reservation)))={files,write_fd};
    fd_flags = flags & KERNEL_FILES_O_CLOEXEC ? KERNEL_FILES_FD_CLOEXEC : 0;
    pair[0]=(int32_t)read_fd; pair[1]=(int32_t)write_fd;
    access_status=kernel_copy_to_user(mm,user_pipefd,pair,sizeof(pair),&copied);
    if (access_status!=KERNEL_UACCESS_STATUS_OK || copied!=sizeof(pair)) {
        kernel_files_release_uninstalled_description(files,&write_description);
        kernel_files_release_uninstalled_description(files,&read_description);
        *linux_result=-KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }
    status=kernel_files_install_pair_reserved(&read_res,&write_res,fd_flags,
        &read_description,&write_description);
    if (status) __builtin_trap();
    *linux_result = 0;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_dup(
    struct kernel_files *files, int64_t oldfd, int64_t *result)
{
    if (!kernel_files_is_live(files) || !result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    return dup_from(files, oldfd, 0, 0, result);
}
enum kernel_files_status kernel_files_dup3(
    struct kernel_files *files, int64_t oldfd, int64_t newfd, uint64_t flags, int64_t *result)
{
    if (!kernel_files_is_live(files) || !result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (oldfd == newfd || (flags & ~LINUX_O_CLOEXEC)) { *result=-KERNEL_EINVAL; return KERNEL_FILES_STATUS_OK; }
    if (newfd<0 || (uint64_t)newfd>=files->nofile_limit || (uint64_t)newfd>=KERNEL_FILES_MAX_CAPACITY) {
        *result=-KERNEL_EBADF; return KERNEL_FILES_STATUS_OK;
    }
    { KERNEL_FILES_SCOPE(guard, files);
      if (!lookup_locked(files, oldfd)) { *result=-KERNEL_EBADF; return KERNEL_FILES_STATUS_OK; } }
    enum kernel_files_status status=ensure_slot_capacity(files, (uint32_t)newfd+1, result);
    if (status || *result) return status;
    struct kernel_open_file_description *replaced;
    {
        KERNEL_FILES_SCOPE(guard, files);
        struct kernel_open_file_description *source=lookup_locked(files, oldfd);
        if (!source) { *result=-KERNEL_EBADF; return KERNEL_FILES_STATUS_OK; }
        if (files->record->slots[newfd].flags & KERNEL_FILES_FD_RESERVED) {
            *result=-KERNEL_EBUSY; return KERNEL_FILES_STATUS_OK;
        }
        if (kernel_open_file_acquire(source)) __builtin_trap();
        replaced=detach_fd_locked(files, (uint32_t)newfd);
        if (install_owned_locked(files, (uint32_t)newfd,
            flags & LINUX_O_CLOEXEC ? KERNEL_FILES_FD_CLOEXEC : 0, &source)) __builtin_trap();
    }
    if (replaced && release_detached(files, replaced, 0, kernel_task_current())) return KERNEL_FILES_STATUS_STATE;
    *result=newfd; return KERNEL_FILES_STATUS_OK;
}
enum kernel_files_status kernel_files_dup2(
    struct kernel_files *files, int64_t oldfd, int64_t newfd, int64_t *result)
{
    if (!kernel_files_is_live(files) || !result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (oldfd==newfd) {
        KERNEL_FILES_SCOPE(guard, files);
        *result=lookup_locked(files, oldfd) ? newfd : -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    return kernel_files_dup3(files, oldfd, newfd, 0, result);
}
enum kernel_files_status kernel_files_fcntl(
    struct kernel_files *files, int64_t fd, uint64_t command, uint64_t argument, int64_t *result)
{
    if (!kernel_files_is_live(files) || !result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (command==KERNEL_FILES_F_DUPFD || command==KERNEL_FILES_F_DUPFD_CLOEXEC) {
        if (argument>=files->nofile_limit || argument>=KERNEL_FILES_MAX_CAPACITY) {
            *result=-KERNEL_EINVAL; return KERNEL_FILES_STATUS_OK;
        }
        return dup_from(files, fd, (uint32_t)argument,
            command==KERNEL_FILES_F_DUPFD_CLOEXEC ? KERNEL_FILES_FD_CLOEXEC : 0, result);
    }
    if(command==1031U || command==1032U) {
        KERNEL_FILES_PIN_SCOPE(pin);
        struct kernel_open_file_description *file=kernel_files_hold_fd(files,fd,&pin);
        if(!file || file->kind!=KERNEL_OPEN_FILE_KIND_PIPE) {*result=-KERNEL_EBADF;return 0;}
        *result=kernel_pipe_capacity(file->pipe,(int)command,argument);
        return 0;
    }
    KERNEL_FILES_SCOPE(guard, files);
    struct kernel_open_file_description *source=lookup_locked(files, fd);
    if (!source) { *result=-KERNEL_EBADF; return KERNEL_FILES_STATUS_OK; }
    struct kernel_file_slot *slot=&files->record->slots[fd];
    if (source->kind==KERNEL_OPEN_FILE_KIND_PATH && command!=KERNEL_FILES_F_GETFD &&
        command!=KERNEL_FILES_F_SETFD && command!=KERNEL_FILES_F_GETFL) {
        *result=-KERNEL_EBADF; return KERNEL_FILES_STATUS_OK;
    }
    switch (command) {
    case KERNEL_FILES_F_GETFD: *result=!!(slot->flags & KERNEL_FILES_FD_CLOEXEC); break;
    case KERNEL_FILES_F_SETFD: {
        uint32_t flags=argument & 1 ? KERNEL_FILES_FD_CLOEXEC : 0;
        if (flags && !slot->flags) files->record->statistics.close_on_exec_fds++;
        if (!flags && slot->flags) files->record->statistics.close_on_exec_fds--;
        slot->flags=flags; *result=0; break;
    }
    case KERNEL_FILES_F_GETFL: *result=kernel_open_file_flags(source); break;
    case KERNEL_FILES_F_SETFL:
        /* Immutable/open-only bits are ignored, not rejected. Direct I/O and
         * no-atime remain unsupported mutable modes, as at open. */
        if (argument & (00040000U | 01000000U))
            *result=-KERNEL_EINVAL;
        else { kernel_open_file_set_status_flags(source, (uint32_t)argument); *result=0; }
        break;
    default: *result=-KERNEL_EINVAL; break;
    }
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_close(
    struct kernel_files *files, int64_t fd, int64_t *linux_result)
{
    if (!kernel_files_is_live(files) || !linux_result)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    struct kernel_open_file_description *description;
    {
        KERNEL_FILES_SCOPE(guard, files);
        KERNEL_FILES_NOTE(files, close_calls, 1);
        description = lookup_locked(files, fd);
        if (!description) {
            KERNEL_FILES_NOTE(files, close_failures, 1);
            *linux_result = -KERNEL_EBADF;
            return KERNEL_FILES_STATUS_OK;
        }
        description = detach_fd_locked(files, (uint32_t)fd);
    }
    *linux_result = 0;
    return release_detached(files, description, 1, kernel_task_current());
}

/* 一段最多16槽；取得的OFD owner覆盖锁外释放。 */
static enum kernel_files_status detach_batch(struct kernel_files *files, uint32_t *cursor,
    int cloexec, struct kernel_task *closing)
{
    struct kernel_open_file_description *batch[16]; unsigned count=0;
    {
        KERNEL_FILES_SCOPE(guard, files);
        uint32_t end=*cursor+16;
        if (end>files->record->statistics.capacity) end=files->record->statistics.capacity;
        for (; *cursor<end; (*cursor)++) {
            struct kernel_file_slot *slot=&files->record->slots[*cursor];
            if (slot->description && (!cloexec || (slot->flags & KERNEL_FILES_FD_CLOEXEC)))
                batch[count++]=detach_fd_locked(files, *cursor);
        }
    }
    for (unsigned i=0; i<count; i++) if (release_detached(files, batch[i], 0, closing)) __builtin_trap();
    return KERNEL_FILES_STATUS_OK;
}
enum kernel_files_status kernel_files_close_on_exec(struct kernel_files *files)
{
    if (!kernel_files_is_live(files)) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    kernel_assert_service_context();
    uint32_t capacity;
    { KERNEL_FILES_SCOPE(guard, files); capacity=files->record->statistics.capacity; }
    uint32_t cursor=0;
    while (cursor<capacity) detach_batch(files, &cursor, 1, kernel_task_current());
    return kernel_files_drain_file_cleanup(files);
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
    KERNEL_FILES_SCOPE(guard, files);
    const struct kernel_files_statistics *source=&files->record->statistics;
    const uint64_t *counters=(const void *)source;
    uint64_t *output=(void *)statistics;
    for (size_t i=0;i<offsetof(struct kernel_files_statistics,current_open_fds)/sizeof(*counters);i++)
        output[i]=__atomic_load_n(&counters[i],__ATOMIC_RELAXED);
    statistics->current_open_fds=source->current_open_fds;
    statistics->peak_open_fds=source->peak_open_fds;
    statistics->close_on_exec_fds=source->close_on_exec_fds;
    statistics->capacity=source->capacity;
}

enum kernel_files_status kernel_files_release_for_task(struct kernel_files *files,struct kernel_task *closing)
{
    if (!files) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if ((files->state!=KERNEL_FILES_LIVE && files->state!=KERNEL_FILES_CLEANUP) ||
        !files->heap || !files->record) return KERNEL_FILES_STATUS_STATE;
    uint32_t capacity;
    {
        KERNEL_FILES_SCOPE(guard, files);
        struct kernel_files_record *record=files->record;
        if (files->state==KERNEL_FILES_LIVE) {
            if (!record->references || record->retired) __builtin_trap();
            if (--record->references) {
                finish_files(files, KERNEL_FILES_RELEASED);
                return KERNEL_FILES_STATUS_OK;
            }
            record->retired=1;
            files->state=KERNEL_FILES_CLEANUP;
        } else if (record->references || !record->retired) __builtin_trap();
        capacity=record->statistics.capacity;
    }
    uint32_t cursor=0;
    while (cursor<capacity) detach_batch(files, &cursor, 0, closing);
    enum kernel_files_status status=kernel_files_drain_file_cleanup(files);
    if (status) return status;
    kernel_heap_release(files->heap, files->record->slots);
    kernel_heap_release(files->heap, files->record);
    finish_files(files, KERNEL_FILES_RELEASED);
    return KERNEL_FILES_STATUS_OK;
}

/* Most callers close their own table; stopped-task teardown supplies its owner. */
enum kernel_files_status kernel_files_release(struct kernel_files *files)
{ return kernel_files_release_for_task(files,kernel_task_current()); }
