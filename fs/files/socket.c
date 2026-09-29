#include "private.h"

#include <kernel/errno.h>
#include <kernel/open_file.h>
#include <kernel/socket.h>
#include <kernel/uaccess.h>

#include <stdint.h>

#define LINUX_SOCK_NONBLOCK 00004000U
#define LINUX_SOCK_CLOEXEC 02000000U
#define LINUX_O_RDWR 2U

static enum kernel_files_status install_socket(
    struct kernel_files *files, struct kernel_socket *socket,
    uint32_t fd, uint32_t flags, int64_t *linux_result)
{
    struct kernel_open_file_description *ofd = 0;
    enum kernel_open_file_status status =
        kernel_open_file_create_socket(files->heap, socket,
            LINUX_O_RDWR | (flags & LINUX_SOCK_NONBLOCK), &ofd);
    if (status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY) {
        kernel_socket_destroy(socket);
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (status != KERNEL_OPEN_FILE_STATUS_OK) {
        kernel_socket_destroy(socket);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (kernel_files_install_new_owned_at(files, fd,
        (flags & LINUX_SOCK_CLOEXEC) != 0U ? KERNEL_FILES_FD_CLOEXEC : 0U,
        &ofd) != KERNEL_FILES_STATUS_OK) {
        if (kernel_open_file_release(&ofd) != KERNEL_OPEN_FILE_STATUS_OK)
            __builtin_trap();
        return KERNEL_FILES_STATUS_STATE;
    }
    *linux_result = fd;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_socket_create(
    struct kernel_files *files, int type, uint32_t flags,
    int64_t *linux_result)
{
    struct kernel_socket *socket = 0;
    uint32_t fd;
    int find_result = 0;
    int result;
    enum kernel_files_status status;
    if (!kernel_files_is_live(files) || linux_result == 0)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    status = kernel_files_find_free_fd(files, &fd, &find_result);
    if (status != KERNEL_FILES_STATUS_OK) return status;
    if (find_result != 0) {
        *linux_result = find_result;
        return KERNEL_FILES_STATUS_OK;
    }
    result = kernel_socket_create(files->heap, type, &socket);
    if (result != 0) {
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    return install_socket(files, socket, fd, flags, linux_result);
}

enum kernel_files_status kernel_files_socket_accept(
    struct kernel_files *files, struct kernel_open_file_description *listener,
    uint32_t flags, uint32_t *peer_address, uint16_t *peer_port,
    int64_t *linux_result)
{
    struct kernel_socket *accepted = 0;
    uint32_t fd;
    int find_result = 0;
    int result;
    enum kernel_files_status status;
    if (!kernel_files_is_live(files) || listener == 0 ||
        peer_address == 0 || peer_port == 0 || linux_result == 0)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    status = kernel_files_find_free_fd(files, &fd, &find_result);
    if (status != KERNEL_FILES_STATUS_OK) return status;
    if (find_result != 0) {
        *linux_result = find_result;
        return KERNEL_FILES_STATUS_OK;
    }
    result = kernel_socket_accept(kernel_open_file_socket(listener), &accepted);
    if (result != 0) {
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    result = kernel_socket_getpeer(accepted, peer_address, peer_port);
    if (result != 0) {
        kernel_socket_destroy(accepted);
        *linux_result = result;
        return KERNEL_FILES_STATUS_OK;
    }
    return install_socket(files, accepted, fd, flags, linux_result);
}

enum kernel_files_status kernel_files_socketpair_create(
    struct kernel_files *files,
    struct kernel_mm *mm,
    int type,
    uint32_t flags,
    uint64_t user_sv,
    int64_t *linux_result)
{
    struct kernel_socket *sock0 = 0;
    struct kernel_socket *sock1 = 0;
    struct kernel_open_file_description *ofd0 = 0;
    struct kernel_open_file_description *ofd1 = 0;
    uint32_t fd0 = 0U;
    uint32_t fd1 = 0U;
    uint32_t fd_flags;
    int32_t pair[2];
    int64_t find_result = 0;
    size_t copied = 0U;
    int res;
    enum kernel_open_file_status open_status;
    enum kernel_files_status status;

    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (kernel_user_range_check(user_sv, sizeof(pair)) != KERNEL_UACCESS_STATUS_OK) {
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }

    res = kernel_socket_pair(files->heap, type, &sock0, &sock1);
    if (res != 0) {
        *linux_result = res;
        return KERNEL_FILES_STATUS_OK;
    }

    open_status = kernel_open_file_create_socket(
        files->heap, sock0, LINUX_O_RDWR | (flags & LINUX_SOCK_NONBLOCK), &ofd0);
    if (open_status != KERNEL_OPEN_FILE_STATUS_OK) {
        kernel_socket_destroy(sock1);
        kernel_socket_destroy(sock0);
        *linux_result = open_status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                            ? -KERNEL_ENOMEM
                            : -KERNEL_EIO;
        return KERNEL_FILES_STATUS_OK;
    }

    open_status = kernel_open_file_create_socket(
        files->heap, sock1, LINUX_O_RDWR | (flags & LINUX_SOCK_NONBLOCK), &ofd1);
    if (open_status != KERNEL_OPEN_FILE_STATUS_OK) {
        (void)kernel_files_release_uninstalled_description(files, &ofd0);
        kernel_socket_destroy(sock1);
        *linux_result = open_status == KERNEL_OPEN_FILE_STATUS_NO_MEMORY
                            ? -KERNEL_ENOMEM
                            : -KERNEL_EIO;
        return KERNEL_FILES_STATUS_OK;
    }

    status = kernel_files_find_two_free_fds(files, &fd0, &fd1, &find_result);
    if (status != KERNEL_FILES_STATUS_OK || find_result != 0) {
        (void)kernel_files_release_uninstalled_description(files, &ofd1);
        (void)kernel_files_release_uninstalled_description(files, &ofd0);
        *linux_result = find_result != 0 ? find_result : -KERNEL_EMFILE;
        return KERNEL_FILES_STATUS_OK;
    }

    pair[0] = (int32_t)fd0;
    pair[1] = (int32_t)fd1;
    if (kernel_copy_to_user(mm, user_sv, pair, sizeof(pair), &copied) != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(pair)) {
        (void)kernel_files_release_uninstalled_description(files, &ofd1);
        (void)kernel_files_release_uninstalled_description(files, &ofd0);
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }

    fd_flags = (flags & LINUX_SOCK_CLOEXEC) != 0U ? KERNEL_FILES_FD_CLOEXEC : 0U;
    status = kernel_files_install_new_owned_at(files, fd0, fd_flags, &ofd0);
    if (status != KERNEL_FILES_STATUS_OK) {
        (void)kernel_files_release_uninstalled_description(files, &ofd1);
        (void)kernel_files_release_uninstalled_description(files, &ofd0);
        return status;
    }
    status = kernel_files_install_new_owned_at(files, fd1, fd_flags, &ofd1);
    if (status != KERNEL_FILES_STATUS_OK) {
        int64_t dummy = 0;
        (void)kernel_files_close(files, (int64_t)fd0, &dummy);
        (void)kernel_files_release_uninstalled_description(files, &ofd1);
        return status;
    }

    *linux_result = 0;
    return KERNEL_FILES_STATUS_OK;
}
