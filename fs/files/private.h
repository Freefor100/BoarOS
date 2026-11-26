#ifndef BOAROS_FS_FILES_PRIVATE_H
#define BOAROS_FS_FILES_PRIVATE_H

#include <kernel/files.h>
#include <kernel/rlimit.h>
#include <kernel/raw_lock.h>

#include <stdint.h>

struct kernel_pipe;
struct kernel_record_lock;
struct kernel_vfs_path;
struct kernel_fs_paths;
struct kernel_task_io_buffer;
struct kernel_vfs_size_limit;
void kernel_files_size_limit_notify(const struct kernel_vfs_size_limit *);
struct kernel_files_pin_guard {
    struct kernel_files *files;
    struct kernel_open_file_description *description;
};
void kernel_files_pin_guard_release(struct kernel_files_pin_guard *guard);
struct kernel_open_file_description *kernel_files_hold_fd(
    struct kernel_files *files, int64_t fd, struct kernel_files_pin_guard *guard);
#define KERNEL_FILES_PIN_SCOPE(name) struct kernel_files_pin_guard name __attribute__((cleanup(kernel_files_pin_guard_release))) = {0}
void kernel_files_path_guard_release(struct kernel_vfs_path **path);
#define KERNEL_FILES_PATH_SCOPE(name) struct kernel_vfs_path *name __attribute__((cleanup(kernel_files_path_guard_release))) = 0

int kernel_files_path_start(struct kernel_files *files,
                            const struct kernel_fs_paths *snapshot,
                            int64_t dirfd, const char *path,
                            struct kernel_vfs_path **start);
int kernel_files_borrow_lookup_start(struct kernel_files *, const struct kernel_fs_paths *,
    int64_t dirfd, const char *, struct kernel_vfs_path **start, struct kernel_vfs_path **owner);
void kernel_files_path_buffer_release(struct kernel_task_io_buffer *);
enum kernel_files_status kernel_files_copy_user_path(struct kernel_mm *, uint64_t,
    struct kernel_task_io_buffer *, int *result);

#define KERNEL_FILES_INITIAL_CAPACITY 32U
#define KERNEL_FILES_MAX_CAPACITY ((uint32_t)KERNEL_RLIMIT_NOFILE_CAP)
#define KERNEL_FILES_INLINE_BITMAP_CAPACITY 512U
#define KERNEL_FILES_BITMAP_WORDS(capacity) (((capacity) + 63U) / 64U)
#define KERNEL_FILES_FD_CLOEXEC UINT32_C(1)
#define KERNEL_FILES_FD_RESERVED UINT32_C(2)
struct kernel_files_fd_reservation { struct kernel_files *files; uint32_t fd; };
void kernel_files_cancel_reserved_fd(struct kernel_files_fd_reservation *reservation);
static inline void kernel_files_cancel_reservation(struct kernel_files_fd_reservation *reservation)
{
    if (reservation->files) kernel_files_cancel_reserved_fd(reservation);
}
#define KERNEL_FILES_RESERVATION_SCOPE(name) \
    struct kernel_files_fd_reservation name __attribute__((cleanup(kernel_files_cancel_reservation))) = {0}
enum kernel_files_status kernel_files_install_pair_reserved(
    struct kernel_files_fd_reservation *, struct kernel_files_fd_reservation *,
    uint32_t flags, struct kernel_open_file_description **,
    struct kernel_open_file_description **);
enum kernel_files_status kernel_files_install_owned_locked(
    struct kernel_files *, uint32_t, uint32_t, struct kernel_open_file_description **);

enum kernel_files_status kernel_files_release_io(struct kernel_files *,
    struct kernel_open_file_description **, enum kernel_files_status);
enum kernel_files_status kernel_files_pipe_result(int64_t bytes,unsigned broken);
int kernel_files_description_writable(const struct kernel_open_file_description *);

struct kernel_file_slot {
    struct kernel_open_file_description *description;
    uint32_t flags;
};

struct kernel_files_record {
    struct kernel_file_slot *slots;
    _Atomic(struct kernel_open_file_description *) cleanup_files;
    struct kernel_record_lock *record_locks;
    struct kernel_files_statistics statistics;
    uint32_t references;
    /* Lowest free slot, or capacity when full. */
    uint32_t next_fd;
    struct kernel_raw_lock metadata_lock;
    uint8_t retired;
    /* Under metadata_lock, a bit covers an installed description or a reservation. */
    uint64_t *occupied;
    uint64_t inline_occupied[KERNEL_FILES_BITMAP_WORDS(KERNEL_FILES_INLINE_BITMAP_CAPACITY)];
};
#if BOAROS_COST_DIAGNOSTICS
/* 同级对象锁很多；单列实际 FD 表争用，复用 raw guard 的计时。 */
static inline void kernel_files_scope_release(struct kernel_raw_guard *guard)
{
    if (!guard->lock) return;
    struct kernel_cost_tag tag=guard->cost;
    uint64_t wait=guard->wait_ticks;
    uint64_t hold=tag.epoch ? kernel_cost_clock()-guard->hold_start : 0;
    kernel_raw_lock_release(guard);
    if (tag.epoch) {
        kernel_cost_add_tag(tag,COST_FD_TABLE_WAIT,wait);
        kernel_cost_add_tag(tag,COST_FD_TABLE_HOLD,hold);
    }
}
#define KERNEL_FILES_SCOPE(name, files) \
    struct kernel_raw_guard name __attribute__((cleanup(kernel_files_scope_release))) = {0}; \
    kernel_raw_lock_acquire(&(files)->record->metadata_lock,&name)
#else
#define KERNEL_FILES_SCOPE(name, files) \
    KERNEL_RAW_SCOPE(name, &(files)->record->metadata_lock)
#endif
#define KERNEL_FILES_NOTE(files, field, value) \
    __atomic_fetch_add(&(files)->record->statistics.field, (uint64_t)(value), __ATOMIC_RELAXED)
int kernel_files_fd_matches(struct kernel_files *, int64_t,
    const struct kernel_open_file_description *);
#define KERNEL_POLL_STACK_CAPACITY 4U
int kernel_files_scan_select(struct kernel_files *,unsigned nfds,const uint64_t *readfds,
    const uint64_t *writefds,const uint64_t *exceptfds,unsigned *count,int *ready,
    struct kernel_open_file_description **small_snapshot);

/* 成功find取得独占预留；调用者随后安装或取消，不能遗弃编号owner。 */
enum kernel_files_status kernel_files_find_free_fd(
    struct kernel_files *files,
    uint32_t *fd,
    int *linux_result);

enum kernel_files_status kernel_files_find_two_free_fds(
    struct kernel_files *files, uint32_t *first, uint32_t *second,
    int64_t *linux_result);

enum kernel_files_status kernel_files_release_uninstalled_description(
    struct kernel_files *files,
    struct kernel_open_file_description **owner);

/* Success consumes *owner into an empty fd slot.  The new-owned variant is
 * for an OFD created by the caller; the shared-acquired variant is for a
 * reference the caller already acquired.  Neither helper changes refcounts. */
enum kernel_files_status kernel_files_install_new_owned_at(
    struct kernel_files *files,
    uint32_t fd,
    uint32_t fd_flags,
    struct kernel_open_file_description **owner);

enum kernel_files_status kernel_files_install_shared_acquired_at(
    struct kernel_files *files,
    uint32_t fd,
    uint32_t fd_flags,
    struct kernel_open_file_description **owner);

void kernel_files_release_allocation(
    struct kernel_files *files,
    void *pointer);

void kernel_files_queue_description(
    struct kernel_files *files,
    struct kernel_open_file_description *description);

enum kernel_files_status kernel_files_drain_file_cleanup(
    struct kernel_files *files);

int kernel_files_open_pty_peer(struct kernel_files *files, struct kernel_task *caller,
    struct kernel_vfs_path *path, uint32_t flags);

#endif
