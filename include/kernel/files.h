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
struct kernel_task;

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
    uint64_t write_calls;
    uint64_t write_failures;
    uint64_t close_calls;
    uint64_t close_failures;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t read_chunks;
    uint32_t current_open_fds;
    uint32_t peak_open_fds;
    uint32_t close_on_exec_fds;
    uint32_t capacity;
};

#define KERNEL_FILES_SEEK_SET UINT64_C(0)
#define KERNEL_FILES_SEEK_CUR UINT64_C(1)
#define KERNEL_FILES_SEEK_END UINT64_C(2)
#define KERNEL_FILES_AT_SYMLINK_NOFOLLOW UINT64_C(0x100)
#define KERNEL_FILES_AT_REMOVEDIR UINT64_C(0x200)
#define KERNEL_FILES_AT_EMPTY_PATH UINT64_C(0x1000)
#define KERNEL_FILES_F_DUPFD UINT64_C(0)
#define KERNEL_FILES_F_GETFD UINT64_C(1)
#define KERNEL_FILES_F_SETFD UINT64_C(2)
#define KERNEL_FILES_F_GETFL UINT64_C(3)
#define KERNEL_FILES_F_SETFL UINT64_C(4)
#define KERNEL_FILES_F_DUPFD_CLOEXEC UINT64_C(1030)
#define KERNEL_FILES_O_APPEND UINT64_C(00002000)
#define KERNEL_FILES_O_NONBLOCK UINT64_C(00004000)
#define KERNEL_FILES_O_LARGEFILE UINT64_C(00100000)
#define KERNEL_FILES_O_CLOEXEC UINT64_C(02000000)

/* Linux asm-generic struct stat as the riscv64 ABI defines it. */
struct kernel_linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t st_pad1;
    int64_t st_size;
    int32_t st_blksize;
    int32_t st_pad2;
    int64_t st_blocks;
    int64_t st_atime;
    int64_t st_atime_nsec;
    int64_t st_mtime;
    int64_t st_mtime_nsec;
    int64_t st_ctime;
    int64_t st_ctime_nsec;
    uint32_t st_pad4;
    uint32_t st_pad5;
};

_Static_assert(sizeof(struct kernel_linux_stat) == 128U,
               "Linux struct stat ABI size must remain 128 bytes");

struct kernel_files {
    struct kernel_heap *heap;
    struct kernel_files_record *record;
    uint32_t nofile_limit;
    enum kernel_files_state state;
};

enum kernel_files_status kernel_files_create(
    struct kernel_files *files,
    struct kernel_heap *heap);

/* Share the complete descriptor table and its cleanup ownership. */
enum kernel_files_status kernel_files_acquire(
    struct kernel_files *destination,
    const struct kernel_files *source);

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
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint64_t flags,
    uint64_t mode,
    int64_t *linux_result);

enum kernel_files_status kernel_files_symlinkat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    uint64_t user_target,
    int64_t dirfd,
    uint64_t user_linkpath,
    int64_t *linux_result);

enum kernel_files_status kernel_files_readlinkat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint64_t user_buffer,
    uint64_t size,
    int64_t *linux_result);

enum kernel_files_status kernel_files_mkdirat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint32_t mode,
    int64_t *linux_result);

enum kernel_files_status kernel_files_unlinkat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint32_t flags,
    int64_t *linux_result);

/* Creates a read/write pipe pair and copies the two descriptors to userland. */
enum kernel_files_status kernel_files_pipe2(
    struct kernel_files *files,
    struct kernel_mm *mm,
    uint64_t user_pipefd,
    uint64_t flags,
    int64_t *linux_result);

enum kernel_files_status kernel_files_read(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    uint64_t count,
    int64_t *linux_result);

/* pread64 reads without changing the shared open-file description offset. */
enum kernel_files_status kernel_files_pread(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    uint64_t count,
    int64_t offset,
    int64_t *linux_result);

enum kernel_files_status kernel_files_write(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    uint64_t count,
    int64_t *linux_result);

struct kernel_uaccess_iovec {
    uint64_t base;
    uint64_t length;
};

enum kernel_files_status kernel_files_readv(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_iov,
    uint64_t iovcnt,
    int64_t *linux_result);

enum kernel_files_status kernel_files_writev(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_iov,
    uint64_t iovcnt,
    int64_t *linux_result);

/* Install the console description at an empty fixed slot for PID 1 stdio. */
enum kernel_files_status kernel_files_open_console(
    struct kernel_files *files,
    int64_t fd,
    int64_t *linux_result);

enum kernel_files_status kernel_files_lseek(
    struct kernel_files *files,
    int64_t fd,
    int64_t offset,
    uint64_t whence,
    int64_t *linux_result);

enum kernel_files_status kernel_files_ftruncate(
    struct kernel_files *files,
    int64_t fd,
    uint64_t length,
    int64_t *linux_result);

enum kernel_files_status kernel_files_fstat(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    int64_t *linux_result);

/* newfstatat: AT_FDCWD or absolute paths, plus AT_EMPTY_PATH on a fd. */
enum kernel_files_status kernel_files_fstatat(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm,
    int64_t dirfd,
    uint64_t user_path,
    uint64_t user_buffer,
    uint64_t flags,
    int64_t *linux_result);

enum kernel_files_status kernel_files_getdents(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int64_t fd,
    uint64_t user_buffer,
    uint64_t count,
    int64_t *linux_result);

enum kernel_files_status kernel_files_dup(
    struct kernel_files *files,
    int64_t oldfd,
    int64_t *linux_result);

/* dup3 semantics: oldfd == newfd is EINVAL, flags accept O_CLOEXEC. */
enum kernel_files_status kernel_files_dup3(
    struct kernel_files *files,
    int64_t oldfd,
    int64_t newfd,
    uint64_t flags,
    int64_t *linux_result);

/* dup2 semantics: oldfd == newfd returns newfd without closing it. */
enum kernel_files_status kernel_files_dup2(
    struct kernel_files *files,
    int64_t oldfd,
    int64_t newfd,
    int64_t *linux_result);

enum kernel_files_status kernel_files_fcntl(
    struct kernel_files *files,
    int64_t fd,
    uint64_t command,
    uint64_t argument,
    int64_t *linux_result);

enum kernel_files_status kernel_files_close(
    struct kernel_files *files,
    int64_t fd,
    int64_t *linux_result);

/* Success returns an owned OFD reference independent of the fd slot. */
enum kernel_files_status kernel_files_pin(
    struct kernel_files *files,
    int64_t fd,
    struct kernel_open_file_description **owner,
    int64_t *linux_result);

/* All marked descriptors become unreachable even when cleanup must retry. */
enum kernel_files_status kernel_files_close_on_exec(
    struct kernel_files *files);

void kernel_files_get_statistics(
    const struct kernel_files *files,
    struct kernel_files_statistics *statistics);

enum kernel_files_status kernel_files_release(
    struct kernel_files *files);

enum kernel_files_status kernel_files_drain_file_cleanup(
    struct kernel_files *files);

enum kernel_files_status kernel_files_ppoll(
    struct kernel_files *files,
    struct kernel_mm *mm,
    struct kernel_task *task,
    uint64_t user_fds,
    uint64_t nfds,
    uint64_t user_timeout,
    uint64_t user_sigmask,
    size_t sigsetsize,
    int64_t *linux_result);

enum kernel_files_status kernel_files_pselect6(
    struct kernel_files *files,
    struct kernel_mm *mm,
    struct kernel_task *task,
    int64_t nfds,
    uint64_t user_readfds,
    uint64_t user_writefds,
    uint64_t user_exceptfds,
    uint64_t user_timeout,
    uint64_t user_sigdata,
    int64_t *linux_result);

enum kernel_files_status kernel_files_epoll_create1(
    struct kernel_files *files,
    uint32_t flags,
    int64_t *linux_result);

enum kernel_files_status kernel_files_epoll_ctl(
    struct kernel_files *files,
    int64_t epfd,
    int32_t op,
    int64_t fd,
    uint32_t events,
    uint64_t data,
    int64_t *linux_result);

enum kernel_files_status kernel_files_epoll_pwait(
    struct kernel_files *files,
    struct kernel_mm *mm,
    struct kernel_task *task,
    int64_t epfd,
    uint64_t user_events,
    int32_t maxevents,
    int32_t timeout,
    uint64_t user_sigmask,
    size_t sigsetsize,
    int64_t *linux_result);

#endif
