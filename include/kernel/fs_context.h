#ifndef BOAROS_KERNEL_FS_CONTEXT_H
#define BOAROS_KERNEL_FS_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KERNEL_FS_PATH_MAX 4096U
#define KERNEL_FS_AT_FDCWD (-100)

struct kernel_heap;
struct kernel_mm;
struct kernel_fs_context_record;
struct kernel_vfs_mount;
struct kernel_vfs_path;
struct kernel_mount_namespace;
struct kernel_namespace;
struct kernel_task;
struct kernel_open_file_description;

enum kernel_fs_context_status {
    KERNEL_FS_CONTEXT_STATUS_OK = 0,
    KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT,
    KERNEL_FS_CONTEXT_STATUS_NO_MEMORY,
    KERNEL_FS_CONTEXT_STATUS_CLEANUP_REQUIRED,
    KERNEL_FS_CONTEXT_STATUS_STATE,
    KERNEL_FS_CONTEXT_STATUS_BUSY,
};

enum kernel_fs_context_state {
    KERNEL_FS_CONTEXT_EMPTY = 0,
    KERNEL_FS_CONTEXT_LIVE,
    KERNEL_FS_CONTEXT_MOVED,
    KERNEL_FS_CONTEXT_CLEANUP,
    KERNEL_FS_CONTEXT_RELEASED,
};

struct kernel_fs_context {
    struct kernel_heap *heap;
    struct kernel_fs_context_record *record;
    struct kernel_mount_namespace *namespace;
    enum kernel_fs_context_state state;
};

enum kernel_fs_context_status kernel_fs_context_create(
    struct kernel_fs_context *fs,
    struct kernel_vfs_mount *root_mount,
    struct kernel_heap *heap);

/* Share root/cwd/umask; each wrapper retains its own mount namespace owner. */
enum kernel_fs_context_status kernel_fs_context_acquire(
    struct kernel_fs_context *destination,
    const struct kernel_fs_context *source);

enum kernel_fs_context_status kernel_fs_context_fork(
    struct kernel_fs_context *destination,
    const struct kernel_fs_context *source);
int kernel_fs_context_fork_mount_namespace(struct kernel_fs_context *,
    const struct kernel_fs_context *);
/* Empty candidate on success means the existing FS context is exclusive. */
int kernel_fs_context_prepare_unshare(struct kernel_fs_context *,
    const struct kernel_fs_context *,int new_mount_namespace);

/* A proc reader owns both the namespace tree and its process-relative root. */
struct kernel_fs_mounts {
    struct kernel_mount_namespace *namespace;
    struct kernel_vfs_path *root, *top;
};
struct kernel_fs_mounts kernel_fs_context_mounts_snapshot(const struct kernel_fs_context *);
void kernel_fs_mounts_release(struct kernel_fs_mounts *);
struct kernel_vfs_mount *kernel_fs_context_mount_root(const struct kernel_fs_context *);
/* Prepare all fallible path/allocation work before publishing a combined
 * namespace change. Commit consumes the live candidate and cannot fail. */
int kernel_fs_context_prepare_namespace(struct kernel_fs_context *,
    const struct kernel_fs_context *,struct kernel_mount_namespace *,int allow_shared);
void kernel_fs_context_commit(struct kernel_fs_context *,struct kernel_fs_context *);
/* Publish only; the caller releases the returned old context after dropping
 * the task resource protection used for a combined namespace exchange. */
void kernel_fs_context_exchange(struct kernel_fs_context *,struct kernel_fs_context *,
    struct kernel_fs_context *old);
/* Combined pidfd setns retains the shared FS record and its umask. The
 * returned candidate owns the replaced paths and this caller's old namespace. */
void kernel_fs_context_exchange_paths_namespace(struct kernel_fs_context *,
    struct kernel_fs_context *,struct kernel_fs_context *old);
uint64_t kernel_fs_context_namespace_id(const struct kernel_fs_context *);
uint64_t kernel_mount_namespace_id(const struct kernel_mount_namespace *);
int kernel_mount_namespace_take_tree(struct kernel_vfs_path **,struct kernel_heap *,struct kernel_open_file_description **);
int kernel_mount_namespace_open_tree(struct kernel_vfs_path *,struct kernel_heap *,int,struct kernel_open_file_description **);
struct kernel_namespace *kernel_mount_namespace_base(struct kernel_mount_namespace *);
struct kernel_mount_namespace *kernel_mount_namespace_file(const struct kernel_open_file_description *);

enum kernel_fs_context_status kernel_fs_context_move(
    struct kernel_fs_context *destination,
    struct kernel_fs_context *source);

/* The caller owns this wrapper throughout the query. CLONE_FS shares the
 * record, not the wrapper; borrowing its mutable contents requires snapshot. */
static inline int kernel_fs_context_is_live(const struct kernel_fs_context *fs)
{
    return fs && fs->state == KERNEL_FS_CONTEXT_LIVE && fs->heap && fs->record && fs->namespace;
}

/* An owned pair is coherent with CLONE_FS cwd replacement and context move. */
struct kernel_fs_paths {
    struct kernel_vfs_path *root, *cwd;
    uint32_t umask;
};
struct kernel_fs_paths kernel_fs_context_snapshot(const struct kernel_fs_context *);
struct kernel_fs_paths kernel_fs_context_path_snapshot(const struct kernel_fs_context *, bool need_cwd);
void kernel_fs_paths_release(struct kernel_fs_paths *);
#define KERNEL_FS_PATHS(name, fs) \
    struct kernel_fs_paths name __attribute__((cleanup(kernel_fs_paths_release))) = \
        kernel_fs_context_snapshot(fs)
uint32_t kernel_fs_context_umask(const struct kernel_fs_context *fs);
int kernel_fs_context_exchange_umask(const struct kernel_fs_context *fs,
                                    uint32_t requested, uint32_t *previous);
int kernel_fs_context_set_cwd(const struct kernel_fs_context *fs,
                              struct kernel_vfs_path *path);
int kernel_fs_context_set_root(const struct kernel_fs_context *fs,
                               struct kernel_vfs_path *path);

/* Legacy kernel pathname adapter for an existing object (root boot).
 * User operations use owned root/cwd snapshots and VFS *at APIs directly.
 * path_length excludes NUL; normal path errors use linux_result. */
enum kernel_fs_context_status kernel_fs_context_resolve_kernel_path(
    const struct kernel_fs_context *fs,
    int64_t dirfd,
    const char *path,
    size_t path_length,
    char *buffer,
    size_t capacity,
    struct kernel_vfs_mount **mount,
    int *linux_result);

/* A normal path error is returned through linux_result. */
enum kernel_fs_context_status kernel_fs_context_resolve_user_path(
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    char *buffer,
    size_t capacity,
    struct kernel_vfs_mount **mount,
    int *linux_result);

enum kernel_fs_context_status kernel_fs_context_release(
    struct kernel_fs_context *fs);
/* 返回组外共享状态；安全共享时登记 exec 以阻止新的 CLONE_FS。 */
int kernel_fs_context_exec_begin(const struct kernel_fs_context *, struct kernel_task *);
void kernel_fs_context_exec_end(const struct kernel_fs_context *);

#endif
