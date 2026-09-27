#include <kernel/socket.h>

#include <arch/riscv/context.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/scheduler.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>

#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"

#include <stddef.h>
#include <stdint.h>

#define SOCKET_DGRAM 2
#define SOCKET_STREAM 1
#define UDP_MAX_PAYLOAD 65507U

struct socket_packet {
    struct socket_packet *next;
    struct pbuf *payload;
    uint32_t address;
    uint16_t port;
    uint16_t consumed;
};

struct socket_pending {
    struct socket_pending *next;
    struct kernel_socket *child;
};

struct kernel_socket {
    struct kernel_heap *heap;
    struct kernel_wait_queue wait;
    struct socket_packet *packets_head;
    struct socket_packet *packets_tail;
    struct socket_pending *pending_head;
    struct socket_pending *pending_tail;
    struct udp_pcb *udp;
    struct tcp_pcb *tcp;
    uint64_t receive_timeout_ns;
    int error;
    uint8_t type;
    uint8_t listening;
    uint8_t connecting;
    uint8_t connected;
    uint8_t peer_closed;
};

static uint8_t socket_initialized;

static int lwip_error(err_t error)
{
    switch (error) {
    case ERR_OK: return 0;
    case ERR_MEM: return -KERNEL_ENOMEM;
    case ERR_USE: return -KERNEL_EADDRINUSE;
    case ERR_VAL: return -KERNEL_EINVAL;
    case ERR_RTE: return -KERNEL_ENETUNREACH;
    case ERR_INPROGRESS: return -KERNEL_EINPROGRESS;
    case ERR_WOULDBLOCK: return -KERNEL_EAGAIN;
    case ERR_ABRT: return -KERNEL_ECONNABORTED;
    case ERR_RST: return -KERNEL_ECONNRESET;
    case ERR_CONN: return -KERNEL_ENOTCONN;
    default: return -KERNEL_EIO;
    }
}

static void wake_socket(struct kernel_socket *socket)
{
    if (kernel_wait_queue_wake_all(&socket->wait) !=
        KERNEL_SCHEDULER_STATUS_OK) {
        __builtin_trap();
    }
}

static void poll_loopback(void)
{
    uintptr_t old_status = riscv_interrupt_save();
    if (socket_initialized) {
        sys_check_timeouts();
        netif_poll_all();
    }
    riscv_interrupt_restore(old_status);
}

uint64_t kernel_socket_next_timer_deadline(void)
{
    uint64_t deadline = 0;
    uint64_t now;
    uint64_t delay;
    u32_t milliseconds;
    uintptr_t old_status = riscv_interrupt_save();
    milliseconds = socket_initialized ? sys_timeouts_sleeptime()
                                      : SYS_TIMEOUTS_SLEEPTIME_INFINITE;
    riscv_interrupt_restore(old_status);
    if (milliseconds == SYS_TIMEOUTS_SLEEPTIME_INFINITE) return 0;
    now = kernel_time_monotonic_ns();
    delay = (uint64_t)(milliseconds == 0U ? 1U : milliseconds) *
            UINT64_C(1000000);
    if (kernel_time_deadline_from_monotonic(
            UINT64_MAX - now < delay ? UINT64_MAX : now + delay,
            &deadline) != KERNEL_TIME_STATUS_OK)
        return 0;
    return deadline;
}

static void purge_dead_pending(struct kernel_socket *listener)
{
    struct socket_pending *entry = listener->pending_head;
    struct socket_pending *previous = 0;
    while (entry != 0) {
        struct socket_pending *next = entry->next;
        if (entry->child->tcp == 0 || !entry->child->connected) {
            if (previous != 0) previous->next = next;
            else listener->pending_head = next;
            if (listener->pending_tail == entry)
                listener->pending_tail = previous;
            kernel_socket_destroy(entry->child);
            if (kernel_heap_release(listener->heap, entry) !=
                KERNEL_HEAP_STATUS_OK) __builtin_trap();
        } else {
            previous = entry;
        }
        entry = next;
    }
}

static void udp_received(void *context, struct udp_pcb *pcb,
                         struct pbuf *payload, const ip_addr_t *peer,
                         u16_t port)
{
    struct kernel_socket *socket = context;
    struct socket_packet *packet = 0;
    (void)pcb;
    enum kernel_heap_status status =
        kernel_heap_allocate_zeroed(socket->heap, 1U, sizeof(*packet),
                                    (void **)&packet);
    if (status == KERNEL_HEAP_STATUS_EMPTY) {
        pbuf_free(payload);
        return;
    }
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    packet->payload = payload;
    packet->address = ip4_addr_get_u32(ip_2_ip4(peer));
    packet->port = port;
    if (socket->packets_tail != 0) {
        socket->packets_tail->next = packet;
    } else {
        socket->packets_head = packet;
    }
    socket->packets_tail = packet;
    wake_socket(socket);
}

static err_t tcp_connected(void *context, struct tcp_pcb *pcb, err_t error)
{
    struct kernel_socket *socket = context;
    (void)pcb;
    socket->connecting = 0U;
    socket->connected = error == ERR_OK;
    socket->error = lwip_error(error);
    wake_socket(socket);
    return ERR_OK;
}

static err_t tcp_data_received(void *context, struct tcp_pcb *pcb,
                               struct pbuf *payload, err_t error)
{
    struct kernel_socket *socket = context;
    struct socket_packet *packet = 0;
    enum kernel_heap_status status;
    (void)pcb;
    if (payload == 0) {
        socket->peer_closed = 1U;
        wake_socket(socket);
        return ERR_OK;
    }
    if (error != ERR_OK) return error;
    status = kernel_heap_allocate_zeroed(socket->heap, 1U, sizeof(*packet),
                                         (void **)&packet);
    if (status == KERNEL_HEAP_STATUS_EMPTY) return ERR_MEM;
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    packet->payload = payload;
    if (socket->packets_tail != 0) socket->packets_tail->next = packet;
    else socket->packets_head = packet;
    socket->packets_tail = packet;
    wake_socket(socket);
    return ERR_OK;
}

static err_t tcp_data_sent(void *context, struct tcp_pcb *pcb, u16_t length)
{
    (void)pcb;
    (void)length;
    wake_socket((struct kernel_socket *)context);
    return ERR_OK;
}

static void tcp_failed(void *context, err_t error)
{
    struct kernel_socket *socket = context;
    socket->tcp = 0;
    socket->connecting = 0U;
    socket->connected = 0U;
    socket->error = lwip_error(error);
    wake_socket(socket);
}

static err_t socket_tcp_accepted(void *context, struct tcp_pcb *pcb,
                                 err_t error)
{
    struct kernel_socket *listener = context;
    struct kernel_socket *child = 0;
    struct socket_pending *entry = 0;

    enum kernel_heap_status status;
    if (error != ERR_OK || pcb == 0) {
        if (pcb != 0) tcp_abort(pcb);
        return ERR_ABRT;
    }
    status = kernel_heap_allocate_zeroed(listener->heap, 1U, sizeof(*child),
                                         (void **)&child);
    if (status == KERNEL_HEAP_STATUS_EMPTY) {
        if (pcb != 0) tcp_abort(pcb);
        return ERR_ABRT;
    }
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    status = kernel_heap_allocate_zeroed(listener->heap, 1U, sizeof(*entry),
                                         (void **)&entry);
    if (status == KERNEL_HEAP_STATUS_EMPTY) {
        if (kernel_heap_release(listener->heap, child) != KERNEL_HEAP_STATUS_OK)
            __builtin_trap();
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    child->heap = listener->heap;
    child->type = SOCKET_STREAM;
    child->tcp = pcb;
    child->connected = 1U;
    kernel_wait_queue_init(&child->wait);
    tcp_arg(pcb, child);
    tcp_err(pcb, tcp_failed);
    tcp_recv(pcb, tcp_data_received);
    tcp_sent(pcb, tcp_data_sent);
    entry->child = child;
    if (listener->pending_tail != 0) {
        listener->pending_tail->next = entry;
    } else {
        listener->pending_head = entry;
    }
    listener->pending_tail = entry;
    wake_socket(listener);
    return ERR_OK;
}

int kernel_socket_create(struct kernel_heap *heap, int type,
                         struct kernel_socket **owner)
{
    struct kernel_socket *socket = 0;
    uintptr_t old_status;

    if (heap == 0 || owner == 0 || *owner != 0 ||
        (type != SOCKET_DGRAM && type != SOCKET_STREAM)) {
        return -KERNEL_EINVAL;
    }
    enum kernel_heap_status status =
        kernel_heap_allocate_zeroed(heap, 1U, sizeof(*socket),
                                    (void **)&socket);
    if (status == KERNEL_HEAP_STATUS_EMPTY) {
        return -KERNEL_ENOMEM;
    }
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    old_status = riscv_interrupt_save();
    if (!socket_initialized) {
        lwip_init();
        socket_initialized = 1U;
    }
    socket->heap = heap;
    socket->type = (uint8_t)type;
    kernel_wait_queue_init(&socket->wait);
    if (type == SOCKET_DGRAM) {
        socket->udp = udp_new();
        if (socket->udp != 0) udp_recv(socket->udp, udp_received, socket);
    } else {
        socket->tcp = tcp_new();
        if (socket->tcp != 0) {
            tcp_arg(socket->tcp, socket);
            tcp_err(socket->tcp, tcp_failed);
            tcp_recv(socket->tcp, tcp_data_received);
            tcp_sent(socket->tcp, tcp_data_sent);
        }
    }
    riscv_interrupt_restore(old_status);
    if ((type == SOCKET_DGRAM && socket->udp == 0) ||
        (type == SOCKET_STREAM && socket->tcp == 0)) {
        if (kernel_heap_release(heap, socket) != KERNEL_HEAP_STATUS_OK)
            __builtin_trap();
        return -KERNEL_ENOMEM;
    }
    *owner = socket;
    return 0;
}

void kernel_socket_destroy(struct kernel_socket *socket)
{
    uintptr_t old_status;
    if (socket == 0) __builtin_trap();
    old_status = riscv_interrupt_save();
    while (socket->packets_head != 0) {
        struct socket_packet *packet = socket->packets_head;
        socket->packets_head = packet->next;
        pbuf_free(packet->payload);
        if (kernel_heap_release(socket->heap, packet) != KERNEL_HEAP_STATUS_OK)
            __builtin_trap();
    }
    while (socket->pending_head != 0) {
        struct socket_pending *entry = socket->pending_head;
        socket->pending_head = entry->next;
        kernel_socket_destroy(entry->child);
        if (kernel_heap_release(socket->heap, entry) != KERNEL_HEAP_STATUS_OK)
            __builtin_trap();
    }
    if (socket->udp != 0) udp_remove(socket->udp);
    if (socket->tcp != 0) {
        if (!socket->listening) {
            tcp_arg(socket->tcp, 0);
            tcp_err(socket->tcp, 0);
            tcp_recv(socket->tcp, 0);
            tcp_sent(socket->tcp, 0);
            tcp_poll(socket->tcp, 0, 0);
            socket->tcp->connected = 0;
        } else {
            tcp_arg(socket->tcp, 0);
            tcp_accept(socket->tcp, 0);
        }
        if (tcp_close(socket->tcp) != ERR_OK) tcp_abort(socket->tcp);
    }
    riscv_interrupt_restore(old_status);
    if (kernel_heap_release(socket->heap, socket) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
}

int kernel_socket_bind(struct kernel_socket *socket, uint32_t address,
                       uint16_t port)
{
    ip_addr_t local;
    err_t error;
    uintptr_t old_status = riscv_interrupt_save();
    ip4_addr_set_u32(ip_2_ip4(&local), address);
    if (socket->type == SOCKET_DGRAM) {
        error = udp_bind(socket->udp, &local, port);
    } else {
        error = tcp_bind(socket->tcp, &local, port);
    }
    riscv_interrupt_restore(old_status);
    return lwip_error(error);
}

int kernel_socket_getname(struct kernel_socket *socket, uint32_t *address,
                          uint16_t *port)
{
    if (socket == 0 || address == 0 || port == 0) return -KERNEL_EINVAL;
    if (socket->type == SOCKET_DGRAM) {
        *address = ip4_addr_get_u32(ip_2_ip4(&socket->udp->local_ip));
        *port = socket->udp->local_port;
    } else {
        if (socket->tcp == 0) return socket->error != 0 ? socket->error
                                                         : -KERNEL_ENOTCONN;
        if (socket->listening) {
            struct tcp_pcb_listen *pcb = (struct tcp_pcb_listen *)socket->tcp;
            *address = ip4_addr_get_u32(ip_2_ip4(&pcb->local_ip));
            *port = pcb->local_port;
        } else {
            *address = ip4_addr_get_u32(ip_2_ip4(&socket->tcp->local_ip));
            *port = socket->tcp->local_port;
        }
    }
    return 0;
}

int kernel_socket_getpeer(struct kernel_socket *socket, uint32_t *address,
                          uint16_t *port)
{
    if (socket == 0 || address == 0 || port == 0) return -KERNEL_EINVAL;
    if (socket->type != SOCKET_STREAM || !socket->connected ||
        socket->tcp == 0) return -KERNEL_ENOTCONN;
    *address = ip4_addr_get_u32(ip_2_ip4(&socket->tcp->remote_ip));
    *port = socket->tcp->remote_port;
    return 0;
}

int kernel_socket_listen(struct kernel_socket *socket, int backlog)
{
    err_t error = ERR_OK;
    struct tcp_pcb *replacement;
    uintptr_t old_status;
    if (socket->type != SOCKET_STREAM) return -KERNEL_EOPNOTSUPP;
    if (socket->tcp == 0) return -KERNEL_ENOTCONN;
    if (socket->listening) return 0;
    old_status = riscv_interrupt_save();
    replacement = tcp_listen_with_backlog_and_err(socket->tcp,
        backlog < 1 ? 1U : backlog > 255 ? 255U : (u8_t)backlog, &error);
    if (replacement != 0) {
        socket->tcp = replacement;
        socket->listening = 1U;
        tcp_arg(replacement, socket);
        tcp_accept(replacement, socket_tcp_accepted);
    }
    riscv_interrupt_restore(old_status);
    return replacement != 0 ? 0 : lwip_error(error);
}

int kernel_socket_connect(struct kernel_socket *socket, uint32_t address,
                          uint16_t port, int nonblocking)
{
    ip_addr_t remote;
    err_t error;
    uintptr_t old_status;
    if (socket->type != SOCKET_STREAM) return -KERNEL_EOPNOTSUPP;
    if (socket->tcp == 0) return socket->error != 0 ? socket->error
                                                     : -KERNEL_ENOTCONN;
    if (socket->listening) return -KERNEL_EINVAL;
    if (socket->connected) return -KERNEL_EISCONN;
    if (socket->connecting) return -KERNEL_EALREADY;
    ip4_addr_set_u32(ip_2_ip4(&remote), address);
    old_status = riscv_interrupt_save();
    error = tcp_connect(socket->tcp, &remote, port, tcp_connected);
    if (error == ERR_OK) socket->connecting = 1U;
    riscv_interrupt_restore(old_status);
    if (error != ERR_OK) return lwip_error(error);
    /* NO_SYS has no network thread: the initiating syscall must deliver SYN. */
    poll_loopback();
    if (nonblocking) return -KERNEL_EINPROGRESS;
    if (socket->error != 0) return socket->error;
    return socket->connected ? 0 : -KERNEL_EINPROGRESS;
}

int kernel_socket_connection_result(struct kernel_socket *socket)
{
    if (socket->error != 0) return socket->error;
    return socket->connected ? 0 : -KERNEL_EINPROGRESS;
}

int kernel_socket_accept(struct kernel_socket *socket,
                         struct kernel_socket **owner)
{
    struct socket_pending *entry;
    uintptr_t old_status;
    if (!socket->listening || owner == 0 || *owner != 0)
        return -KERNEL_EINVAL;
    poll_loopback();
    old_status = riscv_interrupt_save();
    purge_dead_pending(socket);
    entry = socket->pending_head;
    if (entry == 0) {
        riscv_interrupt_restore(old_status);
        return -KERNEL_EAGAIN;
    }
    socket->pending_head = entry->next;
    if (socket->pending_head == 0) socket->pending_tail = 0;
    tcp_accepted(socket->tcp);
    *owner = entry->child;
    riscv_interrupt_restore(old_status);
    if (kernel_heap_release(socket->heap, entry) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    return 0;
}

int kernel_socket_sendto(struct kernel_socket *socket, struct kernel_mm *mm,
                         uint64_t user_data, uint64_t size, uint32_t address,
                         uint16_t port)
{
    struct pbuf *payload;
    ip_addr_t remote;
    size_t copied = 0;
    err_t error;
    uintptr_t old_status;
    if (socket->type != SOCKET_DGRAM) return -KERNEL_EOPNOTSUPP;
    if (size > UDP_MAX_PAYLOAD) return -KERNEL_EMSGSIZE;
    old_status = riscv_interrupt_save();
    payload = pbuf_alloc(PBUF_TRANSPORT, (u16_t)size, PBUF_RAM);
    riscv_interrupt_restore(old_status);
    if (payload == 0) return -KERNEL_ENOMEM;
    if (size != 0 &&
        (kernel_copy_from_user(mm, payload->payload, user_data, (size_t)size,
                               &copied) != KERNEL_UACCESS_STATUS_OK ||
         copied != size)) {
        pbuf_free(payload);
        return -KERNEL_EFAULT;
    }
    ip4_addr_set_u32(ip_2_ip4(&remote), address);
    old_status = riscv_interrupt_save();
    error = udp_sendto(socket->udp, payload, &remote, port);
    pbuf_free(payload);
    if (error == ERR_OK) netif_poll_all();
    riscv_interrupt_restore(old_status);
    return error == ERR_OK ? (int)size : lwip_error(error);
}

int kernel_socket_recvfrom(struct kernel_socket *socket, struct kernel_mm *mm,
                           uint64_t user_data, uint64_t size,
                           uint32_t *address, uint16_t *port)
{
    struct socket_packet *packet;
    uint8_t chunk[256];
    size_t wanted;
    size_t done = 0;
    uintptr_t old_status;
    if (socket->type != SOCKET_DGRAM) return -KERNEL_EOPNOTSUPP;
    poll_loopback();
    old_status = riscv_interrupt_save();
    packet = socket->packets_head;
    if (packet == 0) {
        riscv_interrupt_restore(old_status);
        return -KERNEL_EAGAIN;
    }
    socket->packets_head = packet->next;
    if (socket->packets_head == 0) socket->packets_tail = 0;
    riscv_interrupt_restore(old_status);
    wanted = size < packet->payload->tot_len ? (size_t)size
                                             : packet->payload->tot_len;
    while (done < wanted) {
        size_t length = wanted - done;
        size_t copied = 0;
        if (length > sizeof(chunk)) length = sizeof(chunk);
        if (pbuf_copy_partial(packet->payload, chunk, (u16_t)length,
                              (u16_t)done) != length) __builtin_trap();
        if (kernel_copy_to_user(mm, user_data + done, chunk, length,
                                &copied) != KERNEL_UACCESS_STATUS_OK) {
            done += copied;
            break;
        }
        done += length;
    }
    *address = packet->address;
    *port = packet->port;
    old_status = riscv_interrupt_save();
    pbuf_free(packet->payload);
    riscv_interrupt_restore(old_status);
    if (kernel_heap_release(socket->heap, packet) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    return done != 0 || wanted == 0 ? (int)done : -KERNEL_EFAULT;
}

int kernel_socket_read_buffer(struct kernel_socket *socket, void *buffer,
                              uint32_t capacity)
{
    struct socket_packet *packet;
    uint32_t available;
    uint32_t length;
    uintptr_t old_status;
    if (capacity == 0U) return 0;
    if (socket->listening) return -KERNEL_ENOTCONN;
    poll_loopback();
    old_status = riscv_interrupt_save();
    packet = socket->packets_head;
    if (packet == 0) {
        int result = socket->type == SOCKET_STREAM && socket->peer_closed
                         ? 0 : socket->error != 0 ? socket->error
                                                 : -KERNEL_EAGAIN;
        riscv_interrupt_restore(old_status);
        return result;
    }
    available = packet->payload->tot_len - packet->consumed;
    length = capacity < available ? capacity : available;
    if (pbuf_copy_partial(packet->payload, buffer, (u16_t)length,
                          packet->consumed) != length)
        __builtin_trap();
    if (socket->type == SOCKET_DGRAM || length == available) {
        socket->packets_head = packet->next;
        if (socket->packets_head == 0) socket->packets_tail = 0;
        pbuf_free(packet->payload);
        if (kernel_heap_release(socket->heap, packet) != KERNEL_HEAP_STATUS_OK)
            __builtin_trap();
    } else {
        packet->consumed += (uint16_t)length;
    }
    if (socket->type == SOCKET_STREAM && socket->tcp != 0)
        tcp_recved(socket->tcp, (u16_t)length);
    riscv_interrupt_restore(old_status);
    return (int)length;
}

int kernel_socket_write_buffer(struct kernel_socket *socket,
                               const void *buffer, uint32_t size)
{
    uint32_t length;
    err_t error;
    uintptr_t old_status;
    if (socket->type == SOCKET_DGRAM) return -KERNEL_EDESTADDRREQ;
    if (!socket->connected || socket->tcp == 0)
        return socket->error != 0 ? socket->error : -KERNEL_ENOTCONN;
    if (size == 0U) return 0;
    old_status = riscv_interrupt_save();
    length = size < tcp_sndbuf(socket->tcp) ? size : tcp_sndbuf(socket->tcp);
    if (length > UINT16_MAX) length = UINT16_MAX;
    if (length == 0U) {
        riscv_interrupt_restore(old_status);
        return -KERNEL_EAGAIN;
    }
    error = tcp_write(socket->tcp, buffer, (u16_t)length, TCP_WRITE_FLAG_COPY);
    if (error == ERR_OK) {
        /* tcp_write owns the copied bytes even if immediate output defers. */
        (void)tcp_output(socket->tcp);
        netif_poll_all();
    }
    riscv_interrupt_restore(old_status);
    return error == ERR_OK ? (int)length
                           : error == ERR_MEM ? -KERNEL_EAGAIN
                                              : lwip_error(error);
}

void kernel_socket_set_receive_timeout(struct kernel_socket *socket,
                                       uint64_t nanoseconds)
{
    socket->receive_timeout_ns = nanoseconds;
}

uint64_t kernel_socket_receive_timeout(const struct kernel_socket *socket)
{
    return socket->receive_timeout_ns;
}

uint32_t kernel_socket_poll(struct kernel_socket *socket,
                            struct kernel_wait_queue **queue)
{
    uint32_t events = 0;
    if (queue != 0) *queue = &socket->wait;
    poll_loopback();
    if (socket->type == SOCKET_DGRAM) {
        if (socket->packets_head != 0) events |= KERNEL_POLLIN | KERNEL_POLLRDNORM;
        events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM;
    } else if (socket->listening) {
        uintptr_t old_status = riscv_interrupt_save();
        purge_dead_pending(socket);
        riscv_interrupt_restore(old_status);
        if (socket->pending_head != 0) events |= KERNEL_POLLIN | KERNEL_POLLRDNORM;
    } else if (socket->connected) {
        if (socket->packets_head != 0 || socket->peer_closed)
            events |= KERNEL_POLLIN | KERNEL_POLLRDNORM;
        if (socket->tcp != 0 && tcp_sndbuf(socket->tcp) != 0U)
            events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM;
    } else if (socket->error != 0) {
        events |= KERNEL_POLLERR | KERNEL_POLLHUP;
    }
    return events;
}

struct kernel_wait_queue *kernel_socket_wait_queue(struct kernel_socket *socket)
{
    return &socket->wait;
}

static int is_loopback_name(const char name[16])
{
    return name[0] == 'l' && name[1] == 'o' && name[2] == 0;
}

static struct netif *loopback_netif(void)
{
    struct netif *interface;
    NETIF_FOREACH(interface) {
        if (interface->name[0] == 'l' && interface->name[1] == 'o')
            return interface;
    }
    return 0;
}

int kernel_socket_loopback_flags(const char name[16], uint16_t *flags)
{
    struct netif *loopback;
    if (flags == 0 || !is_loopback_name(name)) return -KERNEL_ENODEV;
    loopback = loopback_netif();
    if (loopback == 0) return -KERNEL_ENODEV;
    *flags = UINT16_C(0x8); /* IFF_LOOPBACK */
    if (netif_is_up(loopback)) *flags |= UINT16_C(0x1);
    if (netif_is_link_up(loopback)) *flags |= UINT16_C(0x40);
    return 0;
}

int kernel_socket_set_loopback_flags(const char name[16], uint16_t flags)
{
    struct netif *loopback;
    uintptr_t old_status;
    if (!is_loopback_name(name)) return -KERNEL_ENODEV;
    loopback = loopback_netif();
    if (loopback == 0) return -KERNEL_ENODEV;
    old_status = riscv_interrupt_save();
    if ((flags & UINT16_C(0x1)) != 0U) netif_set_up(loopback);
    else netif_set_down(loopback);
    riscv_interrupt_restore(old_status);
    return 0;
}
