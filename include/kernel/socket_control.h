#ifndef BOAROS_KERNEL_SOCKET_CONTROL_H
#define BOAROS_KERNEL_SOCKET_CONTROL_H
#include <stdint.h>
struct kernel_heap;
struct kernel_pid;
struct kernel_uid_account;
struct kernel_open_file_description;
struct kernel_socket;
#define KERNEL_SOCKET_SCM_MAX_FD 253U
/* The syscall owns this object until the first successful publication. A
 * received object owns all OFD references until installation or release. */
struct kernel_socket_control {
    struct kernel_heap *heap;
    struct kernel_pid *pid;
    struct kernel_uid_account *account;
    uint32_t uid, gid, count, capacity;
    uint32_t unix_count; /* Prepared graph edges; ordinary rights stay packet-owned. */
    unsigned has_credentials, inflight;
    struct kernel_socket_control *next; /* Detached cleanup/retry owner. */
    struct kernel_socket_control *receiver_next, **receiver_previous;
    struct kernel_socket *receiver;
    void *packet;
    struct kernel_open_file_description *files[];
};
enum kernel_socket_receive_state {
    KERNEL_SOCKET_RECEIVE_NONE,
    KERNEL_SOCKET_RECEIVE_STREAM_EMPTY,
    KERNEL_SOCKET_RECEIVE_RECORD,
    KERNEL_SOCKET_RECEIVE_STREAM_FAULT,
    KERNEL_SOCKET_RECEIVE_EOF,
};
struct kernel_socket_message {
    struct kernel_socket_control *control;
    struct kernel_socket_control *credentials; /* send snapshot retained across stream fragments */
    uint32_t flags;
    enum kernel_socket_receive_state receive_state;
};
int kernel_socket_control_allocate(struct kernel_heap *, uint32_t capacity,
    struct kernel_socket_control **owner);
void kernel_socket_control_release(struct kernel_socket_control **owner);
/* Called after an OFD refcount decrement; never accesses the released OFD. */
void kernel_socket_rights_changed(void);
void kernel_socket_rights_service(void);
/* Advance collection and retry retired owners now. Busy preserves every owner. */
int kernel_socket_rights_drain(void);
/* Bind socket queue ownership to its actual OFD; pending children use listener. */
void kernel_socket_attach_file(struct kernel_socket *,struct kernel_open_file_description *);
#endif
