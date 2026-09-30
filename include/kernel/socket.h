#ifndef BOAROS_KERNEL_SOCKET_H
#define BOAROS_KERNEL_SOCKET_H

#include <stdint.h>
#include <stddef.h>

struct kernel_uaccess_iovec;
struct kernel_heap;
struct kernel_mm;
struct kernel_socket;
struct kernel_wait_queue;
struct kernel_task;
struct kernel_open_file_description;

/* Stack-live syscall reservation, registered on its task for forced-exit
 * cleanup before the task's OFD pin is released. */
struct kernel_socket_read_request {
    struct kernel_socket *socket;
    struct kernel_task *task;
    void *packet;
    uint32_t bytes;
    int datagram;
    struct kernel_open_file_description *pin;
    struct kernel_open_file_description **pin_owner;
};

struct kernel_socket_write_request {
    struct kernel_socket *socket;
    struct kernel_task *task;
    void *packet;
    struct kernel_open_file_description *pin;
    struct kernel_open_file_description **pin_owner;
};
int kernel_socket_is_unix_datagram(const struct kernel_socket *socket);
int kernel_socket_write_datagram(struct kernel_open_file_description **pin_owner,
    struct kernel_mm *mm, const struct kernel_uaccess_iovec *iov,
    size_t iov_count, uint64_t count, int nonblocking);
void kernel_socket_abort_write(struct kernel_socket_write_request *request);

struct kernel_socket_statistics {
    uint64_t tcp_write_calls;
    uint64_t tcp_written_bytes;
};
void kernel_socket_get_statistics(struct kernel_socket_statistics *statistics);

#define KERNEL_SOCKET_DOMAIN_INET 0U
#define KERNEL_SOCKET_DOMAIN_UNIX 1U

/* IPv4 addresses use the Linux sockaddr_in/network-byte-order representation. */
int kernel_socket_create(struct kernel_heap *heap, int type,
                         struct kernel_socket **owner);
int kernel_socket_pair(struct kernel_heap *heap, int type,
                       struct kernel_socket **owner_a,
                       struct kernel_socket **owner_b);
void kernel_socket_destroy(struct kernel_socket *socket);
int kernel_socket_bind(struct kernel_socket *socket, uint32_t address,
                       uint16_t port);
int kernel_socket_getname(struct kernel_socket *socket, uint32_t *address,
                          uint16_t *port);
int kernel_socket_getpeer(struct kernel_socket *socket, uint32_t *address,
                          uint16_t *port);
int kernel_socket_listen(struct kernel_socket *socket, int backlog);
int kernel_socket_connect(struct kernel_socket *socket, uint32_t address,
                          uint16_t port, int nonblocking);
int kernel_socket_connection_result(struct kernel_socket *socket);
int kernel_socket_accept(struct kernel_socket *socket,
                         struct kernel_socket **owner);
int kernel_socket_accept_check(const struct kernel_socket *socket);
int kernel_socket_sendto(struct kernel_socket *socket, struct kernel_mm *mm,
                         uint64_t user_data, uint64_t size, uint32_t address,
                         uint16_t port);
int kernel_socket_recvfrom(struct kernel_socket *socket, struct kernel_mm *mm,
                           uint64_t user_data, uint64_t size,
                           uint32_t *address, uint16_t *port);
int kernel_socket_reserve_read(struct kernel_socket *socket,
                              struct kernel_task *task,
                              struct kernel_socket_read_request *request,
                              struct kernel_open_file_description **pin_owner,
                              uint32_t capacity);
void kernel_socket_copy_read(const struct kernel_socket_read_request *request,
                             uint32_t offset, void *buffer, uint32_t length);
void kernel_socket_finish_read(struct kernel_socket_read_request *request,
                               int user_fault);
void kernel_socket_abort_read(struct kernel_socket_read_request *request);
int kernel_socket_write_buffer(struct kernel_socket *socket,
                               const void *buffer, uint32_t size);
void kernel_socket_set_receive_timeout(struct kernel_socket *socket,
                                       uint64_t nanoseconds);
uint64_t kernel_socket_receive_timeout(const struct kernel_socket *socket);
uint32_t kernel_socket_poll(struct kernel_socket *socket,
                            struct kernel_wait_queue **queue);
struct kernel_wait_queue *kernel_socket_wait_queue(struct kernel_socket *socket);
uint64_t kernel_socket_next_timer_deadline(void);
int kernel_socket_loopback_flags(const char name[16], uint16_t *flags);
int kernel_socket_set_loopback_flags(const char name[16], uint16_t flags);

#endif
