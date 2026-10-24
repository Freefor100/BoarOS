#ifndef BOAROS_KERNEL_SOCKET_CALL_H
#define BOAROS_KERNEL_SOCKET_CALL_H

#include <kernel/socket_control.h>

struct kernel_files;
struct kernel_mm;
struct kernel_task;
struct kernel_uaccess_iov_snapshot;
/* 栈上请求在任务中登记；退出先结束收发借用，再归还这些调用期所有者。 */
struct kernel_socket_call {
    struct kernel_socket_message message;
    struct kernel_task *task;
    struct kernel_files *files;
    struct kernel_heap *heap;
    struct kernel_open_file_description **socket_owner;
    struct kernel_socket_address *address;
    void *import;
    struct kernel_uaccess_iov_snapshot *iov_snapshot;
    uint32_t reserved_fd;
    unsigned fd_reserved;
};
int kernel_socket_call_begin(struct kernel_socket_call *, struct kernel_files *, struct kernel_open_file_description **);
void kernel_socket_call_end(struct kernel_socket_call *);
void kernel_socket_call_abort(struct kernel_socket_call *);
int kernel_socket_control_import(struct kernel_socket_call *, struct kernel_mm *, uint64_t, uint64_t);
void kernel_socket_control_export(struct kernel_socket_call *, struct kernel_mm *,
    uint64_t, uint64_t, uint32_t, int, uint64_t *, uint32_t *);
#endif
