#include "private.h"
#include "../open_file_internal.h"
#include "../record_lock.h"
#include "../vfs_internal.h"

#include <arch/riscv/context.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/open_file.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

#include <stdint.h>

/* RV64 asm-generic struct flock, including its four-byte trailing padding. */
struct linux_flock {
    int16_t type, whence;
    int64_t start, length;
    int32_t pid;
};
_Static_assert(sizeof(struct linux_flock) == 32, "RV64 flock ABI");

/* The fixed Linux baseline bounds POSIX deadlock-chain traversal at ten.
 * Only stack-live traditional wait requests participate. An edge is used
 * only while its blocking lock still exists; releases can wake a waiter
 * before that task has resumed to remove the edge. */
struct lock_wait_edge {
    struct lock_wait_edge *next;
    struct kernel_record_lock_state *state;
    const void *owner, *blocker;
    int64_t start, end;
    int16_t type;
};

static struct lock_wait_edge *waiting_locks;

static int deadlocked(const void *owner, const void *requester, int depth)
{
    if (depth >= 10) return 0;
    for (struct lock_wait_edge *edge = waiting_locks; edge;
         edge = edge->next) {
        if (edge->owner != owner) continue;
        struct kernel_record_lock_conflict current;
        kernel_record_lock_get(edge->state, edge->owner, 0,
                               edge->start, edge->end, edge->type, &current);
        if (current.kind || current.owner != edge->blocker) continue;
        if (edge->blocker == requester ||
            deadlocked(edge->blocker, requester, depth + 1)) return 1;
    }
    return 0;
}

static void remove_wait_edge(struct lock_wait_edge *edge)
{
    struct lock_wait_edge **cursor = &waiting_locks;
    while (*cursor && *cursor != edge) cursor = &(*cursor)->next;
    if (!*cursor) __builtin_trap();
    *cursor = edge->next;
}

static enum kernel_files_status drop_pin(
    struct kernel_files *files,
    struct kernel_open_file_description **file)
{
    enum kernel_open_file_status status = kernel_open_file_release(file);
    if (status == KERNEL_OPEN_FILE_STATUS_OK) return KERNEL_FILES_STATUS_OK;
    if (status == KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED && *file) {
        kernel_files_queue_description(files, *file);
        *file = 0;
        return KERNEL_FILES_STATUS_CLEANUP_REQUIRED;
    }
    return KERNEL_FILES_STATUS_STATE;
}

static int range_from_flock(const struct linux_flock *flock,
                            struct kernel_open_file_description *file,
                            int64_t *start, int64_t *end)
{
    int64_t base;
    if (flock->whence == 0) base = 0;
    else if (flock->whence == 1) {
        if (file->offset > INT64_MAX) return -KERNEL_EOVERFLOW;
        base = (int64_t)file->offset;
    } else if (flock->whence == 2) {
        uint64_t size = kernel_open_file_size(file);
        if (size > INT64_MAX) return -KERNEL_EOVERFLOW;
        base = (int64_t)size;
    } else return -KERNEL_EINVAL;
    if (__builtin_add_overflow(base, flock->start, start) || *start < 0)
        return -KERNEL_EINVAL;
    if (flock->length > 0) {
        if (__builtin_add_overflow(*start, flock->length - 1, end))
            return -KERNEL_EOVERFLOW;
    } else if (flock->length < 0) {
        int64_t lower;
        if (__builtin_add_overflow(*start, flock->length, &lower) ||
            lower < 0) return -KERNEL_EINVAL;
        *end = *start - 1;
        *start = lower;
    } else *end = INT64_MAX;
    return 0;
}

enum kernel_files_status kernel_files_fcntl_lock(
    struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_task *task, int64_t fd, uint64_t command,
    uint64_t user_flock, int64_t *linux_result)
{
    struct linux_flock flock;
    struct kernel_open_file_description *file = 0;
    struct kernel_vfs_node *node;
    struct kernel_record_lock_state *state;
    struct kernel_record_lock **owner_head;
    struct kernel_record_lock_conflict found;
    enum kernel_files_status status;
    enum kernel_uaccess_status access;
    kernel_pid_t tgid;
    size_t copied = 0;
    int64_t start, end;
    int result;
    uint8_t kind = command >= KERNEL_FILES_F_OFD_GETLK ? 1U : 0U;
    int query = command == KERNEL_FILES_F_GETLK ||
                command == KERNEL_FILES_F_OFD_GETLK;
    int wait = command == KERNEL_FILES_F_SETLKW ||
               command == KERNEL_FILES_F_OFD_SETLKW;
    if (!kernel_files_is_live(files) || !mm || !task || !linux_result)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    status = kernel_files_pin(files, fd, &file, linux_result);
    if (status != KERNEL_FILES_STATUS_OK || !file) return status;
    node = kernel_open_file_node(file);
    if (!node) { *linux_result = -KERNEL_EBADF; goto out; }
    access = kernel_copy_from_user(mm, &flock, user_flock,
                                    sizeof(flock), &copied);
    if (access == KERNEL_UACCESS_STATUS_FAULT) {
        *linux_result = -KERNEL_EFAULT;
        goto out;
    }
    if (access != KERNEL_UACCESS_STATUS_OK || copied != sizeof(flock)) {
        status = KERNEL_FILES_STATUS_STATE;
        goto out;
    }
    if (flock.type < 0 || flock.type > 2 ||
        (query && !kind && flock.type == 2) ||
        (kind && flock.pid != 0)) {
        *linux_result = -KERNEL_EINVAL;
        goto out;
    }
    result = range_from_flock(&flock, file, &start, &end);
    if (result) { *linux_result = result; goto out; }
    if (!query && flock.type == 0 && !kernel_open_file_readable(file)) {
        *linux_result = -KERNEL_EBADF;
        goto out;
    }
    if (!query && flock.type == 1 && (file->open_flags & 3U) == 0U) {
        *linux_result = -KERNEL_EBADF;
        goto out;
    }
    state = kernel_vfs_node_record_locks(node);
    owner_head = kind ? &file->record_locks : &files->record->record_locks;
    if (kernel_task_tgid(task, &tgid) != KERNEL_TASK_STATUS_OK) {
        status = KERNEL_FILES_STATUS_STATE;
        goto out;
    }
    uintptr_t irq = riscv_interrupt_save();
    if (query) {
        result = kernel_record_lock_get(state,
                      kind ? (const void *)file : (const void *)files->record,
                      kind, start, end, flock.type, &found);
    } else {
        for (;;) {
            result = kernel_record_lock_set(state, owner_head,
                          kind ? (const void *)file : (const void *)files->record,
                          kind, (int32_t)tgid, start, end, flock.type,
                          files->heap);
            if (result != -KERNEL_EAGAIN || !wait) break;
            const void *owner = kind ? (const void *)file :
                                       (const void *)files->record;
            kernel_record_lock_get(state, owner, kind, start, end,
                                   flock.type, &found);
            if (!kind && !found.kind && found.owner &&
                deadlocked(found.owner, owner, 0)) {
                result = -KERNEL_EDEADLK;
                break;
            }
            struct lock_wait_edge edge = { .next = waiting_locks,
                .state = state, .owner = owner, .blocker = found.owner,
                .start = start, .end = end, .type = flock.type };
            if (!kind) waiting_locks = &edge;
            enum kernel_wait_wake_reason reason;
            enum kernel_scheduler_status sleep_status =
                kernel_scheduler_block_current(&state->waiters, 0U, 1,
                                               &reason);
            if (!kind) remove_wait_edge(&edge);
            if (sleep_status != KERNEL_SCHEDULER_STATUS_OK) {
                riscv_interrupt_restore(irq);
                status = KERNEL_FILES_STATUS_STATE;
                goto out;
            }
            if (reason == KERNEL_WAIT_SIGNALLED) {
                kernel_signal_note_syscall_restart(task);
                result = -KERNEL_ERESTARTSYS;
                break;
            }
            if (!kind && kernel_files_lookup_description(files, fd) != file) {
                result = -KERNEL_EBADF;
                break;
            }
        }
    }
    riscv_interrupt_restore(irq);
    if (result) { *linux_result = result; goto out; }
    if (query) {
        flock.type = found.type;
        if (found.type != 2) {
            flock.whence = 0;
            flock.start = found.start;
            flock.length = found.end == INT64_MAX
                         ? 0 : found.end - found.start + 1;
            flock.pid = found.pid;
        }
        copied = 0;
        access = kernel_copy_to_user(mm, user_flock, &flock,
                                      sizeof(flock), &copied);
        if (access == KERNEL_UACCESS_STATUS_FAULT)
            *linux_result = -KERNEL_EFAULT;
        else if (access != KERNEL_UACCESS_STATUS_OK ||
                 copied != sizeof(flock)) {
            status = KERNEL_FILES_STATUS_STATE;
            goto out;
        }
        else *linux_result = 0;
    } else *linux_result = 0;
out:
    {
        enum kernel_files_status release_status = drop_pin(files, &file);
        return status == KERNEL_FILES_STATUS_STATE ? status : release_status;
    }
}
