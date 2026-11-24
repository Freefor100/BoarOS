#include "private.h"

#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/io_uring.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

enum uring_file_opcode {
    URING_SYNC_FILE_RANGE = 8,
    URING_FALLOCATE = 17,
    URING_OPENAT = 18,
    URING_CLOSE = 19,
    URING_STATX = 21,
    URING_OPENAT2 = 28,
    URING_EPOLL_CTL = 29,
    URING_RENAMEAT = 35,
    URING_UNLINKAT = 36,
    URING_MKDIRAT = 37,
    URING_SYMLINKAT = 38,
    URING_LINKAT = 39,
    URING_FSETXATTR = 41,
    URING_SETXATTR = 42,
    URING_FGETXATTR = 43,
    URING_GETXATTR = 44,
    URING_FTRUNCATE = 55,
    URING_PIPE = 62,
};

int kernel_io_uring_extra_supported(unsigned opcode)
{
    switch (opcode) {
    case URING_SYNC_FILE_RANGE: case URING_FALLOCATE: case URING_OPENAT:
    case URING_CLOSE: case URING_STATX: case URING_OPENAT2: case URING_EPOLL_CTL:
    case URING_RENAMEAT: case URING_UNLINKAT: case URING_MKDIRAT:
    case URING_SYMLINKAT: case URING_LINKAT: case URING_FSETXATTR:
    case URING_SETXATTR: case URING_FGETXATTR: case URING_GETXATTR:
    case URING_FTRUNCATE: case URING_PIPE:
        return 1;
    default:
        return kernel_io_uring_socket_supported(opcode);
    }
}

int kernel_io_uring_extra_needs_file(unsigned opcode)
{
    return opcode == URING_SYNC_FILE_RANGE || opcode == URING_FALLOCATE ||
        opcode == URING_FTRUNCATE || opcode == URING_FSETXATTR ||
        opcode == URING_FGETXATTR || (kernel_io_uring_socket_supported(opcode) &&
            kernel_io_uring_socket_needs_file(opcode));
}

int kernel_io_uring_extra_validate(const struct uring_sqe *sqe)
{
    if ((sqe->flags & 1U) && !kernel_io_uring_extra_needs_file(sqe->opcode))
        return -KERNEL_EBADF; /* IOSQE_FIXED_FILE */
    if (kernel_io_uring_socket_supported(sqe->opcode))
        return kernel_io_uring_socket_validate(sqe);
    switch (sqe->opcode) {
    case URING_SYNC_FILE_RANGE:
        return (sqe->address || sqe->buffer || sqe->file_index) ? -KERNEL_EINVAL : 0;
    case URING_FALLOCATE:
        return (sqe->op_flags || sqe->buffer || sqe->file_index) ? -KERNEL_EINVAL : 0;
    case URING_FTRUNCATE:
        return (sqe->address || sqe->length || sqe->op_flags || sqe->buffer ||
            sqe->file_index || sqe->extra[0]) ? -KERNEL_EINVAL : 0;
    case URING_OPENAT: case URING_OPENAT2:
        return sqe->buffer ? -KERNEL_EINVAL : 0;
    case URING_CLOSE:
        if (sqe->offset || sqe->address || sqe->length || sqe->op_flags ||
            sqe->buffer || (sqe->file_index && sqe->fd)) return -KERNEL_EINVAL;
        return 0;
    case URING_UNLINKAT:
        if (sqe->offset || sqe->length || sqe->extra[0] || sqe->extra[1] ||
            (sqe->op_flags & ~0x200U)) return -KERNEL_EINVAL; /* AT_REMOVEDIR */
        break;
    case URING_MKDIRAT:
        if (sqe->offset || sqe->op_flags) return -KERNEL_EINVAL;
        break;
    case URING_SYMLINKAT:
        if (sqe->length || sqe->op_flags) return -KERNEL_EINVAL;
        break;
    case URING_PIPE:
        if (sqe->fd || sqe->offset || sqe->extra[0] ||
            (sqe->op_flags & ~(uint32_t)(0x80000 | 0x800 | 0x4000 | 0x80)))
            return -KERNEL_EINVAL;
        return sqe->file_index ? -KERNEL_EOPNOTSUPP : 0;
    case URING_FGETXATTR: case URING_GETXATTR:
        return sqe->op_flags ? -KERNEL_EINVAL : 0;
    case URING_FSETXATTR: case URING_SETXATTR:
        return sqe->op_flags & ~3U ? -KERNEL_EINVAL : 0;
    }
    return (sqe->buffer || sqe->file_index) ? -KERNEL_EINVAL : 0;
}

static enum kernel_files_status uring_epoll_ctl(struct kernel_files *files,
    struct kernel_mm *mm, const struct uring_sqe *sqe, int64_t *result)
{
    struct { uint32_t events, pad; uint64_t data; } event = {0};
    int32_t operation = (int32_t)sqe->length;
    size_t copied;
    if ((operation == 1 || operation == 3) &&
        kernel_copy_from_user(mm, &event, sqe->address, sizeof(event), &copied)) {
        *result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }
    return kernel_files_epoll_ctl(files, sqe->fd, operation, (int32_t)sqe->offset,
        event.events, event.data, result);
}

int64_t kernel_io_uring_extra_execute(struct kernel_files *files, struct kernel_mm *mm,
    const struct uring_sqe *sqe, struct kernel_open_file_description **owner)
{
    if (kernel_io_uring_socket_supported(sqe->opcode))
        return kernel_io_uring_socket_execute(files, mm, sqe, owner);
    struct kernel_open_file_description *file = *owner;
    int64_t result = 0;
    enum kernel_files_status status;
    switch (sqe->opcode) {
    case URING_SYNC_FILE_RANGE:
        return kernel_files_sync_range_owned(file, (int64_t)sqe->offset, sqe->length,
            sqe->op_flags);
    case URING_FALLOCATE:
        return kernel_files_fallocate_owned(file, sqe->length,
            (int64_t)sqe->offset, (int64_t)sqe->address);
    case URING_FTRUNCATE:
        if ((int64_t)sqe->offset < 0) return -KERNEL_EINVAL;
        status = kernel_files_truncate_owned(file, sqe->offset, &result);
        break;
    case URING_EPOLL_CTL:
        status = uring_epoll_ctl(files, mm, sqe, &result);
        break;
    case URING_CLOSE:
        status = kernel_files_uring_close(files, sqe->fd, &result);
        break;
    case URING_PIPE:
        status = kernel_files_pipe2(files, mm, sqe->address, sqe->op_flags, &result);
        break;
    default: {
        const struct kernel_fs_context *fs = NULL;
        if (!file && kernel_task_fs_context_borrow(kernel_task_current(), &fs))
            return -KERNEL_EBADF;
        switch (sqe->opcode) {
        case URING_OPENAT: case URING_OPENAT2: {
            uint64_t how[3] = {sqe->op_flags, sqe->length, 0};
            int strict = sqe->opcode == URING_OPENAT2;
            if (strict) {
                int error = syscall_copy_open_how(mm, sqe->offset, sqe->length, how);
                if (error) return error;
            }
            status = kernel_files_uring_open(files, fs, mm, sqe->fd, sqe->address,
                how, strict, sqe->file_index ? owner : NULL, &result);
            break;
        }
        case URING_STATX:
            status = kernel_files_statx(files, fs, mm, sqe->fd, sqe->address,
                sqe->op_flags, sqe->length, sqe->offset, &result);
            break;
        case URING_RENAMEAT:
            status = kernel_files_renameat(files, fs, mm, sqe->fd, sqe->address,
                (int32_t)sqe->length, sqe->offset, sqe->op_flags, &result);
            break;
        case URING_UNLINKAT:
            status = kernel_files_unlinkat(files, fs, mm, sqe->fd, sqe->address,
                sqe->op_flags, &result);
            break;
        case URING_MKDIRAT:
            status = kernel_files_mkdirat(files, fs, mm, sqe->fd, sqe->address,
                sqe->length, &result);
            break;
        case URING_SYMLINKAT:
            status = kernel_files_symlinkat(files, fs, mm, sqe->address, sqe->fd,
                sqe->offset, &result);
            break;
        case URING_LINKAT:
            status = kernel_files_linkat(files, fs, mm, sqe->fd, sqe->address,
                (int32_t)sqe->length, sqe->offset, sqe->op_flags, &result);
            break;
        case URING_FGETXATTR: case URING_GETXATTR:
        case URING_FSETXATTR: case URING_SETXATTR: {
            const struct kernel_files_xattr_request request = {
                .operation = (sqe->opcode == URING_FSETXATTR || sqe->opcode == URING_SETXATTR)
                    ? KERNEL_XATTR_SET : KERNEL_XATTR_GET,
                .by_fd = file != NULL,
                .fd = sqe->fd,
                .path = sqe->extra[0],
                .name = sqe->address,
                .value = sqe->offset,
                .size = sqe->length,
                .flags = sqe->op_flags,
            };
            status = kernel_files_xattr_owned(files, fs, mm, &request, file, &result);
            break;
        }
        default:
            return -KERNEL_EINVAL;
        }
        break;
    }
    }
    return status == KERNEL_FILES_STATUS_OK ? result : -KERNEL_EIO;
}
