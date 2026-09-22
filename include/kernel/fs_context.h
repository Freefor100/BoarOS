#ifndef BOAROS_KERNEL_FS_CONTEXT_H
#define BOAROS_KERNEL_FS_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

#define KERNEL_FS_PATH_MAX 4096U
#define KERNEL_FS_AT_FDCWD (-100)

struct kernel_heap;
struct kernel_mm;
struct kernel_fs_context_record;
struct kernel_vfs_mount;
struct kernel_vfs_path;

enum kernel_fs_context_status {
    KERNEL_FS_CONTEXT_STATUS_OK = 0,
    KERNEL_FS_CONTEXT_STATUS_INVALID_ARGUMENT,
    KERNEL_FS_CONTEXT_STATUS_NO_MEMORY,
    KERNEL_FS_CONTEXT_STATUS_CLEANUP_REQUIRED,
    KERNEL_FS_CONTEXT_STATUS_STATE,
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
    enum kernel_fs_context_state state;
};

enum kernel_fs_context_status kernel_fs_context_create(
    struct kernel_fs_context *fs,
    struct kernel_vfs_mount *root_mount,
    struct kernel_heap *heap);

/* Share the root and current-working-directory context. */
enum kernel_fs_context_status kernel_fs_context_acquire(
    struct kernel_fs_context *destination,
    const struct kernel_fs_context *source);

enum kernel_fs_context_status kernel_fs_context_fork(
    struct kernel_fs_context *destination,
    const struct kernel_fs_context *source);

enum kernel_fs_context_status kernel_fs_context_move(
    struct kernel_fs_context *destination,
    struct kernel_fs_context *source);

int kernel_fs_context_is_live(const struct kernel_fs_context *fs);

/* Borrowed identities remain live while this context is held. */
struct kernel_vfs_path *kernel_fs_context_root(const struct kernel_fs_context *fs);
struct kernel_vfs_path *kernel_fs_context_cwd(const struct kernel_fs_context *fs);
int kernel_fs_context_set_cwd(const struct kernel_fs_context *fs,
                              struct kernel_vfs_path *path);

/* Legacy kernel pathname adapter for an existing object (root boot).
 * User operations use borrowed root/cwd and VFS *at APIs directly.
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

#endif
