#ifndef BOAROS_KERNEL_FILES_H
#define BOAROS_KERNEL_FILES_H

#include <stddef.h>
#include <stdint.h>

struct kernel_file_slot;
struct kernel_files_record;
struct kernel_fs_context;
struct kernel_heap;
struct kernel_mm;
struct kernel_open_file_description;

enum kernel_files_status {
    KERNEL_FILES_STATUS_OK = 0,
    KERNEL_FILES_STATUS_INVALID_ARGUMENT,
    KERNEL_FILES_STATUS_NO_MEMORY,
    KERNEL_FILES_STATUS_CLEANUP_REQUIRED,
    KERNEL_FILES_STATUS_STATE,
};

enum kernel_files_state {
    KERNEL_FILES_EMPTY = 0,
    KERNEL_FILES_LIVE,
    KERNEL_FILES_MOVED,
    KERNEL_FILES_CLEANUP,
    KERNEL_FILES_RELEASED,
};

struct kernel_files_statistics {
    uint64_t open_calls;
    uint64_t open_failures;
    uint64_t read_calls;
    uint64_t read_failures;
    uint64_t close_calls;
    uint64_t close_failures;
    uint64_t bytes_read;
    uint64_t read_chunks;
    uint32_t current_open_fds;
    uint32_t peak_open_fds;
    uint32_t close_on_exec_fds;
    uint32_t capacity;
};

struct kernel_files {
    struct kernel_heap *heap;
    struct kernel_files_record *record;
    enum kernel_files_state state;
};

enum kernel_files_status kernel_files_create(
    struct kernel_files *files,
    struct kernel_heap *heap);

/* Copy descriptor slots while sharing their open-file descriptions. */
enum kernel_files_status kernel_files_fork(
    struct kernel_files *destination,
    const struct kernel_files *source);

enum kernel_files_status kernel_files_move(
    struct kernel_files *destination,
    struct kernel_files *source);

int kernel_files_is_live(const struct kernel_files *files);

/* Normal Linux ABI results, including negative errno, use linux_result. */
enum kernel_files_status kernel_files_openat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    const struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint64_t flags,
    uint64_t mode,
    int64_t *linux_result);

enum kernel_files_status kernel_files_read(
    struct kernel_files *files,
    const struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    uint64_t count,
    int64_t *linux_result);

enum kernel_files_status kernel_files_close(
    struct kernel_files *files,
    int64_t fd,
    int64_t *linux_result);

/* All marked descriptors become unreachable even when cleanup must retry. */
enum kernel_files_status kernel_files_close_on_exec(
    struct kernel_files *files);

void kernel_files_get_statistics(
    const struct kernel_files *files,
    struct kernel_files_statistics *statistics);

enum kernel_files_status kernel_files_release(
    struct kernel_files *files);

#endif
