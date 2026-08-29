#ifndef BOAROS_KERNEL_FS_CONTEXT_H
#define BOAROS_KERNEL_FS_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

#define KERNEL_FS_PATH_MAX 4096U
#define KERNEL_FS_AT_FDCWD (-100)

struct kernel_heap;
struct kernel_mm;
struct kernel_vfs_mount;

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
    struct kernel_vfs_mount *root_mount;
    char *cwd;
    enum kernel_fs_context_state state;
};

enum kernel_fs_context_status kernel_fs_context_create(
    struct kernel_fs_context *fs,
    struct kernel_vfs_mount *root_mount,
    struct kernel_heap *heap);

int kernel_fs_context_is_live(const struct kernel_fs_context *fs);

/* A normal path error is returned through linux_result. */
enum kernel_fs_context_status kernel_fs_context_resolve_user_path(
    const struct kernel_fs_context *fs,
    const struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    char *buffer,
    size_t capacity,
    struct kernel_vfs_mount **mount,
    int *linux_result);

enum kernel_fs_context_status kernel_fs_context_release(
    struct kernel_fs_context *fs);

#endif
