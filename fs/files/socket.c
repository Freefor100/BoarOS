#include "private.h"

#include <kernel/errno.h>
#include <kernel/open_file.h>
#include <kernel/socket.h>

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
