#include <kernel/socket.h>
#include "../fs/uaccess_iov_internal.h"

#include <arch/riscv/context.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/sync.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>

#include "lwip/init.h"
#include "boaros_lwip.h"
#include "lwip/memp.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"

#include <stddef.h>
#include <stdint.h>

#define SOCKET_DGRAM 2
#define SOCKET_STREAM 1
#define UDP_MAX_PAYLOAD 65507U
#define SOCKET_WRITE_RETRY_MS 250U

struct socket_packet {
    struct socket_packet *next;
    struct pbuf *payload;
    void *data;
    uint32_t length;
    struct kernel_socket_address address;
    uint16_t consumed;
};

struct socket_pending {
    struct socket_pending *next;
    struct kernel_socket *child;
};

struct socket_link { struct kernel_socket *next, *previous; uint64_t epoch; unsigned queued; };
struct kernel_socket {
    struct kernel_heap *heap;
    struct kernel_wait_queue wait;
    struct socket_packet *packets_head;
    struct socket_packet *packets_tail;
    struct socket_pending *pending_head;
    struct socket_pending *pending_tail;
    struct kernel_socket_read_request *read_request;
    struct kernel_socket *write_retry_next;
    struct kernel_socket *inet_next;
    struct socket_link work_links[4];
    struct kernel_socket *listener;
    unsigned work_flags, pending_live;
    uint8_t pending_ready, destroying;
    struct udp_pcb *udp;
    struct tcp_pcb *tcp;
    struct kernel_socket *peer;
    uint64_t receive_timeout_ns;
    uint64_t send_timeout_ns;
    uint32_t tx_limit;
    uint32_t tx_reserved;
    int pending_error;
    uint8_t reuseaddr;
    uint8_t keepalive;
    uint8_t nodelay;
    uint32_t rx_bytes;
    uint32_t rx_limit;
    int error;
    uint8_t type;
    uint8_t domain;
    uint8_t family;
    uint8_t v6only;
    uint8_t listening;
    uint8_t connecting;
    uint8_t connected;
    uint8_t peer_closed;
    uint8_t read_closed;
    uint8_t write_closed;
    struct kernel_socket_address saved_local;
    struct kernel_socket_address saved_peer;
    ip_addr_t udp_bind_ip;
    uint8_t explicit_bind;
    uint8_t explicit_port;
    uint8_t receive_retry;
    uint8_t write_blocked;
    uint32_t write_retry_ms;
};

static void address_export(const struct kernel_socket *socket,
                           const ip_addr_t *ip, uint16_t port,
                           struct kernel_socket_address *address)
{
    *address = (struct kernel_socket_address){.family = socket->family, .port = port};
    if (IP_IS_V6(ip)) {
        __builtin_memcpy(address->bytes, ip_2_ip6(ip)->addr, 16);
        address->scope = ip6_addr_zone(ip_2_ip6(ip));
    } else if (socket->family == KERNEL_SOCKET_AF_INET6 && !ip_addr_isany(ip)) {
        address->bytes[10] = address->bytes[11] = 0xff;
        __builtin_memcpy(address->bytes + 12, &ip_2_ip4(ip)->addr, 4);
    } else {
        __builtin_memcpy(address->bytes, &ip_2_ip4(ip)->addr, 4);
    }
}

static int address_import(const struct kernel_socket *socket,
                          const struct kernel_socket_address *address,
                          ip_addr_t *ip, int binding)
{
    if (address->family != socket->family) return -KERNEL_EAFNOSUPPORT;
    ip_addr_set_zero(ip);
    if (address->family == KERNEL_SOCKET_AF_INET) {
        IP_SET_TYPE(ip, IPADDR_TYPE_V4);
        __builtin_memcpy(&ip_2_ip4(ip)->addr, address->bytes, 4);
    } else {
        static const uint8_t mapped[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
        if (__builtin_memcmp(address->bytes, mapped, 12) == 0) {
            if (socket->v6only) return binding ? -KERNEL_EINVAL : -KERNEL_ENETUNREACH;
            IP_SET_TYPE(ip, IPADDR_TYPE_V4);
            __builtin_memcpy(&ip_2_ip4(ip)->addr, address->bytes + 12, 4);
        } else {
            IP_SET_TYPE(ip, IPADDR_TYPE_V6);
            __builtin_memcpy(ip_2_ip6(ip)->addr, address->bytes, 16);
            if (ip6_addr_has_scope(ip_2_ip6(ip), IP6_UNKNOWN)) {
                if (address->scope > 255U) return -KERNEL_ENODEV;
                ip6_addr_set_zone(ip_2_ip6(ip), (u8_t)address->scope);
            }
            if (binding && !socket->v6only && ip_addr_isany(ip))
                IP_SET_TYPE(ip, IPADDR_TYPE_ANY);
        }
    }
    return 0;
}

static struct kernel_socket *inet_sockets;
static uint8_t socket_initialized;
static uint8_t socket_timer_irq;
static uint8_t socket_timer_running;
static uint8_t socket_device_failed;
static void (*network_timer_wake)(void *);
static void (*network_work_wake)(void *);
static int (*network_udp_capacity)(void *);
static void *network_context;
static struct kernel_socket *write_retry_head;

enum { SOCKET_WORK_OUTPUT = 1, SOCKET_WORK_RECEIVE = 2, SOCKET_WORK_REAP = 4, SOCKET_WORK_LISTENER = 8 };
struct socket_work_queue { struct kernel_socket *head, *tail; unsigned slot; };
static struct socket_work_queue ready_work, pool_wait = {.slot = 1}, nic_wait = {.slot = 2}, receive_wait = {.slot = 3};
static uint64_t pool_generation, nic_generation, receive_generation;
static unsigned loopback_pending, nic_blocked, protocol_depth, service_cursor, capacity_cursor;
#if BOAROS_COST_DIAGNOSTICS
static unsigned polling_snapshot;
#endif
static struct kernel_io_context *protocol_owner;

uintptr_t kernel_socket_protocol_enter(void)
{
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_io_context *owner = kernel_io_context_current();
    if (protocol_depth && protocol_owner != owner) __builtin_trap();
    if (!protocol_depth) {
        protocol_owner = owner;
        if (owner->allocation_depth == UINT32_MAX) __builtin_trap();
        owner->allocation_depth++;
    }
    if (protocol_depth == UINT32_MAX) __builtin_trap();
    protocol_depth++;
    return irq;
}
void kernel_socket_protocol_leave(uintptr_t irq)
{
    if (!protocol_depth || protocol_owner != kernel_io_context_current()) __builtin_trap();
    if (!--protocol_depth) {
        if (!protocol_owner->allocation_depth) __builtin_trap();
        protocol_owner->allocation_depth--;
        protocol_owner = 0;
    }
    riscv_interrupt_restore(irq);
}
static void socket_pbuf_free(struct pbuf *payload)
{
    uintptr_t irq = kernel_socket_protocol_enter();
    pbuf_free(payload);
    kernel_socket_protocol_leave(irq);
}
static void work_remove(struct socket_work_queue *queue, struct kernel_socket *socket)
{
    struct socket_link *link = &socket->work_links[queue->slot];
    if (!link->queued) return;
    if (link->previous) link->previous->work_links[queue->slot].next = link->next;
    else queue->head = link->next;
    if (link->next) link->next->work_links[queue->slot].previous = link->previous;
    else queue->tail = link->previous;
    *link = (struct socket_link){0};
}
static void work_add(struct socket_work_queue *queue, struct kernel_socket *socket, uint64_t epoch)
{
    if (socket->destroying || socket->domain != KERNEL_SOCKET_DOMAIN_INET) return;
    struct socket_link *link = &socket->work_links[queue->slot];
    if (link->queued) return;
    *link = (struct socket_link){.previous = queue->tail, .queued = 1, .epoch = epoch};
    if (queue->tail) queue->tail->work_links[queue->slot].next = socket;
    else queue->head = socket;
    queue->tail = socket;
}
static void socket_schedule(struct kernel_socket *socket, unsigned flags)
{
    if (socket->destroying || socket->domain != KERNEL_SOCKET_DOMAIN_INET) return;
    socket->work_flags |= flags;
    work_add(&ready_work, socket, 0);
    if (network_work_wake) network_work_wake(network_context);
}
static void socket_pool_wait(struct kernel_socket *socket)
{
    work_remove(&pool_wait, socket);
    work_add(&pool_wait, socket, pool_generation);
}
static void receive_capacity(void)
{
    uintptr_t irq = kernel_socket_protocol_enter();
    receive_generation++;
    if (receive_wait.head && network_work_wake) network_work_wake(network_context);
    kernel_socket_protocol_leave(irq);
}
static void protocol_work_ready(void)
{
    loopback_pending = 1;
    if (network_work_wake) network_work_wake(network_context);
}
static void protocol_capacity(int pool)
{
    if (pool != -1 && pool != MEMP_TCP_SEG && pool != MEMP_PBUF && pool != MEMP_PBUF_POOL) return;
    pool_generation++;
    if (pool_wait.head && network_work_wake) network_work_wake(network_context);
}
static void protocol_input(struct tcp_pcb *pcb)
{
    if (pcb->callback_arg)
        socket_schedule(pcb->callback_arg, SOCKET_WORK_OUTPUT | SOCKET_WORK_REAP | SOCKET_WORK_LISTENER);
}
static void service_inline(void)
{
    /* 短路径仅在raw栈已经退出后执行；回调只发布下一批工作。 */
    if (!protocol_depth)
        (void)kernel_socket_service_pending((struct kernel_socket_service_budget){8, 8, 1});
}

/* Called with interrupts disabled: the list itself owns no socket reference.
 * A socket must remove its entry before its OFD frees it.  Entries stay
 * ordered by write_retry_ms so the earliest retry is the head. */
static void write_retry_disarm(struct kernel_socket *socket)
{
    struct kernel_socket **link;
    if (!socket->write_blocked) return;
    for (link = &write_retry_head; *link != 0;
         link = &(*link)->write_retry_next) {
        if (*link == socket) {
            *link = socket->write_retry_next;
            socket->write_retry_next = 0;
            socket->write_blocked = 0U;
            return;
        }
    }
    __builtin_trap();
}

static void write_retry_arm(struct kernel_socket *socket)
{
    socket->write_retry_ms = sys_now() + SOCKET_WRITE_RETRY_MS;
    write_retry_disarm(socket);
    struct kernel_socket **link = &write_retry_head;
    while (*link != 0 &&
           (int32_t)((*link)->write_retry_ms - socket->write_retry_ms) <= 0)
        link = &(*link)->write_retry_next;
    socket->write_retry_next = *link;
    *link = socket;
    socket->write_blocked = 1U;
}

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

static void retire_timewait(struct kernel_socket *socket)
{
    /* 输入通知只排队；raw输入返回后才检查最终状态，避免保留协议将释放的PCB。 */
    if (!socket->tcp || socket->listening || socket->tcp->state != TIME_WAIT) return;
    tcp_arg(socket->tcp, 0); tcp_err(socket->tcp, 0);
    tcp_recv(socket->tcp, 0); tcp_sent(socket->tcp, 0); tcp_poll(socket->tcp, 0, 0);
    socket->tcp->connected = 0;
    socket->tcp = 0; socket->peer_closed = 1; socket->receive_retry = 0;
    work_remove(&pool_wait, socket); work_remove(&nic_wait, socket); work_remove(&receive_wait, socket);
    write_retry_disarm(socket); wake_socket(socket);
}

void kernel_socket_expire_timers(void)
{
    if (network_timer_wake) { network_timer_wake(network_context); return; }
    /* 无 owner 的早期/停用窗口仍直接推进有界协议定时器。 */
    if (socket_initialized) {
        if (protocol_depth) return;
        uintptr_t irq = kernel_socket_protocol_enter();
        socket_timer_irq = socket_timer_running = 1;
        (void)sys_check_timeouts_budget(1);
        socket_timer_running = socket_timer_irq = 0;
        kernel_socket_protocol_leave(irq);
    }
}

static uint64_t socket_next_timer_deadline(void)
{
    uint64_t deadline = 0;
    uint64_t now;
    uint64_t delay;
    u32_t milliseconds;
    uintptr_t old_status = kernel_socket_protocol_enter();
    milliseconds = socket_initialized ? sys_timeouts_sleeptime()
                                      : SYS_TIMEOUTS_SLEEPTIME_INFINITE;
    u32_t current_ms = sys_now();
    if (write_retry_head != 0) {
        int32_t remaining =
            (int32_t)(write_retry_head->write_retry_ms - current_ms);
        u32_t retry = remaining <= 0 ? 1U : (u32_t)remaining;
        if (milliseconds == SYS_TIMEOUTS_SLEEPTIME_INFINITE ||
            retry < milliseconds) milliseconds = retry;
    }
    kernel_socket_protocol_leave(old_status);
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

static const ip_addr_t *local_ip(const struct kernel_socket *socket)
{
    if (socket->udp != 0) return &socket->udp->local_ip;
    return socket->tcp != 0 ? &socket->tcp->local_ip : 0;
}
static uint16_t local_port(const struct kernel_socket *socket)
{
    if (socket->udp != 0) return socket->udp->local_port;
    return socket->tcp != 0 ? socket->tcp->local_port : 0;
}
static int address_overlap(const ip_addr_t *a, const ip_addr_t *b)
{
    if (IP_IS_ANY_TYPE_VAL(*a) || IP_IS_ANY_TYPE_VAL(*b)) return 1;
    if (IP_GET_TYPE(a) != IP_GET_TYPE(b)) return 0;
    return ip_addr_isany(a) || ip_addr_isany(b) || ip_addr_cmp(a, b);
}
static int port_conflict(struct kernel_socket *socket, const ip_addr_t *ip,
                         uint16_t port, int listening)
{
    if (port == 0) return 0;
    /* lwIP 的 TCP bind 不检查 ANY 与纯 IPv6 的交集；补齐 Linux 双栈资格。 */
    COST_ADD(NETWORK_GLOBAL_SCANS, 1);
    for (struct kernel_socket *other = inet_sockets; other != 0; other = other->inet_next) {
        if (other == socket || other->type != socket->type ||
            local_port(other) != port || local_ip(other) == 0 ||
            !address_overlap(ip, local_ip(other))) continue;
        if (listening ? other->listening :
            !(socket->reuseaddr && other->reuseaddr && !other->listening))
            return -KERNEL_EADDRINUSE;
    }
    return 0;
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
    uint32_t charge = payload->tot_len ? payload->tot_len : 1U;
    /* 接收队列不能占尽共享协议堆：给下一条最大 UDP 和 TCP 控制报文留空间。 */
    if ((network_udp_capacity && !network_udp_capacity(network_context)) ||
        lwip_stats.mem.used > MEM_SIZE - (65536U + 4096U) ||
        charge > socket->rx_limit || socket->rx_bytes > socket->rx_limit - charge) {
        UDP_STATS_INC(udp.memerr);
        socket_pbuf_free(payload); return;
    }
    enum kernel_heap_status status =
        kernel_heap_allocate_zeroed(socket->heap, 1U, sizeof(*packet),
                                    (void **)&packet);
    if (status == KERNEL_HEAP_STATUS_EMPTY) {
        socket_pbuf_free(payload);
        return;
    }
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    packet->payload = payload;
    socket->rx_bytes += charge;
    address_export(socket, peer, port, &packet->address);
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
    socket_schedule(socket, SOCKET_WORK_OUTPUT | SOCKET_WORK_REAP);
    socket->connecting = 0U;
    socket->connected = error == ERR_OK;
    if (error == ERR_OK) {
        address_export(socket, &pcb->local_ip, pcb->local_port, &socket->saved_local);
        address_export(socket, &pcb->remote_ip, pcb->remote_port, &socket->saved_peer);
    }
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
    socket_schedule(socket, SOCKET_WORK_REAP);
    if (payload == 0) {
        socket->peer_closed = 1U;
        wake_socket(socket);
        return ERR_OK;
    }
    /* 拒收重试仍由协议持有 pbuf；IRQ 不重入被中断的全局堆操作。 */
    if (socket_timer_irq) {
        socket->receive_retry=1;socket_schedule(socket, SOCKET_WORK_RECEIVE);wake_socket(socket);return ERR_MEM;
    }
    if (error != ERR_OK) return error;
    if (payload->tot_len > socket->rx_limit ||
        socket->rx_bytes > socket->rx_limit - payload->tot_len) { socket->receive_retry = 1; return ERR_MEM; }
    status = kernel_heap_allocate_zeroed(socket->heap, 1U, sizeof(*packet),
                                         (void **)&packet);
    if (status == KERNEL_HEAP_STATUS_EMPTY) { socket->receive_retry = 1; work_remove(&receive_wait, socket); work_add(&receive_wait, socket, receive_generation); return ERR_MEM; }
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    packet->payload = payload;
    socket->rx_bytes += payload->tot_len;
    socket->receive_retry=0;
    work_remove(&receive_wait, socket);
    if (socket->packets_tail != 0) socket->packets_tail->next = packet;
    else socket->packets_head = packet;
    socket->packets_tail = packet;
    wake_socket(socket);
    return ERR_OK;
}

static err_t tcp_data_sent(void *context, struct tcp_pcb *pcb, u16_t length)
{
    struct kernel_socket *socket = context;
    (void)pcb;
    (void)length;
    socket_schedule(socket, SOCKET_WORK_OUTPUT | SOCKET_WORK_REAP);
    write_retry_disarm(socket);
    wake_socket(socket);
    return ERR_OK;
}

static void tcp_failed(void *context, err_t error)
{
    struct kernel_socket *socket = context;
    write_retry_disarm(socket);
    socket->receive_retry=0;
    work_remove(&pool_wait, socket); work_remove(&nic_wait, socket); work_remove(&receive_wait, socket);
    if (socket->listener && socket->pending_ready) {
        socket->pending_ready = 0;
        if (!socket->listener->pending_live) __builtin_trap();
        socket->listener->pending_live--;
        socket_schedule(socket->listener, SOCKET_WORK_LISTENER);
    }
    if (error == ERR_CLSD) {
        socket->tcp = 0; socket->peer_closed = 1; wake_socket(socket); return;
    }
    socket->error = socket_device_failed ? -KERNEL_EIO : error == ERR_RST && socket->connecting
        ? -KERNEL_ECONNREFUSED : error == ERR_ABRT && socket_timer_running
            ? -KERNEL_ETIMEDOUT : lwip_error(error);
    socket->pending_error = socket->error;
    socket->tcp = 0;
    socket->connecting = 0U;
    socket->connected = 0U;
    socket->peer_closed = 1U;
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
    child->listener = listener; child->pending_ready = 1; listener->pending_live++;
    child->heap = listener->heap;
    child->family = listener->family;
    child->v6only = listener->v6only;
    child->rx_limit = listener->rx_limit;
    child->tx_limit = listener->tx_limit;
    child->receive_timeout_ns = listener->receive_timeout_ns;
    child->send_timeout_ns = listener->send_timeout_ns;
    child->reuseaddr = listener->reuseaddr;
    child->keepalive = listener->keepalive;
    child->nodelay = listener->nodelay;
    child->type = SOCKET_STREAM;
    child->tcp = pcb;
    if (child->nodelay) tcp_nagle_disable(pcb);
    child->connected = 1U;
    address_export(child, &pcb->local_ip, pcb->local_port, &child->saved_local);
    address_export(child, &pcb->remote_ip, pcb->remote_port, &child->saved_peer);
    kernel_wait_queue_init(&child->wait);
    tcp_arg(pcb, child);
    tcp_err(pcb, tcp_failed);
    tcp_recv(pcb, tcp_data_received);
    tcp_sent(pcb, tcp_data_sent);
    child->inet_next = inet_sockets; inet_sockets = child;
    tcp_backlog_delayed(pcb);
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

int kernel_socket_create(struct kernel_heap *heap, int family, int type,
                         struct kernel_socket **owner)
{
    struct kernel_socket *socket = 0;
    uintptr_t old_status;

    if (heap == 0 || owner == 0 || *owner != 0 ||
        (type != SOCKET_DGRAM && type != SOCKET_STREAM) ||
        (family != KERNEL_SOCKET_AF_INET && family != KERNEL_SOCKET_AF_INET6)) {
        return -KERNEL_EINVAL;
    }
    enum kernel_heap_status status =
        kernel_heap_allocate_zeroed(heap, 1U, sizeof(*socket),
                                    (void **)&socket);
    if (status == KERNEL_HEAP_STATUS_EMPTY) {
        return -KERNEL_ENOMEM;
    }
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    old_status = kernel_socket_protocol_enter();
    kernel_socket_network_initialize();
    socket->heap = heap;
    socket->type = (uint8_t)type;
    socket->family = (uint8_t)family;
    socket->domain = KERNEL_SOCKET_DOMAIN_INET;
    socket->rx_limit = 65536U;
    socket->tx_limit = 65536U;
    kernel_wait_queue_init(&socket->wait);
    if (type == SOCKET_DGRAM) {
        socket->udp = udp_new_ip_type(family == KERNEL_SOCKET_AF_INET ?
                                      IPADDR_TYPE_V4 : IPADDR_TYPE_ANY);
        if (socket->udp != 0) udp_recv(socket->udp, udp_received, socket);
    } else {
        socket->tcp = tcp_new_ip_type(family == KERNEL_SOCKET_AF_INET ?
                                      IPADDR_TYPE_V4 : IPADDR_TYPE_ANY);
        if (socket->tcp != 0) {
            tcp_arg(socket->tcp, socket);
            tcp_err(socket->tcp, tcp_failed);
            tcp_recv(socket->tcp, tcp_data_received);
            tcp_sent(socket->tcp, tcp_data_sent);
        }
    }
    kernel_socket_protocol_leave(old_status);
    if ((type == SOCKET_DGRAM && socket->udp == 0) ||
        (type == SOCKET_STREAM && socket->tcp == 0)) {
        if (kernel_heap_release(heap, socket) != KERNEL_HEAP_STATUS_OK)
            __builtin_trap();
        return -KERNEL_ENOMEM;
    }
    old_status = kernel_socket_protocol_enter();
    socket->inet_next = inet_sockets; inet_sockets = socket;
    kernel_socket_protocol_leave(old_status);
    *owner = socket;
    return 0;
}

int kernel_socket_pair(struct kernel_heap *heap, int type,
                       struct kernel_socket **owner_a,
                       struct kernel_socket **owner_b)
{
    struct kernel_socket *sock_a = 0;
    struct kernel_socket *sock_b = 0;
    enum kernel_heap_status status;

    if (heap == 0 || owner_a == 0 || owner_b == 0 ||
        *owner_a != 0 || *owner_b != 0 ||
        (type != SOCKET_DGRAM && type != SOCKET_STREAM)) {
        return -KERNEL_EINVAL;
    }

    status = kernel_heap_allocate_zeroed(heap, 1U, sizeof(*sock_a), (void **)&sock_a);
    if (status == KERNEL_HEAP_STATUS_EMPTY) return -KERNEL_ENOMEM;
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();

    status = kernel_heap_allocate_zeroed(heap, 1U, sizeof(*sock_b), (void **)&sock_b);
    if (status != KERNEL_HEAP_STATUS_OK) {
        if (kernel_heap_release(heap, sock_a) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
        return status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
    }

    sock_a->heap = heap;
    sock_a->type = (uint8_t)type;
    sock_a->domain = KERNEL_SOCKET_DOMAIN_UNIX;
    sock_a->connected = 1U;
    sock_a->rx_limit = sock_a->tx_limit = 65536U;
    kernel_wait_queue_init(&sock_a->wait);

    sock_b->heap = heap;
    sock_b->type = (uint8_t)type;
    sock_b->domain = KERNEL_SOCKET_DOMAIN_UNIX;
    sock_b->connected = 1U;
    sock_b->rx_limit = sock_b->tx_limit = 65536U;
    kernel_wait_queue_init(&sock_b->wait);

    sock_a->peer = sock_b;
    sock_b->peer = sock_a;

    *owner_a = sock_a;
    *owner_b = sock_b;
    return 0;
}

void kernel_socket_destroy(struct kernel_socket *socket)
{
    if (socket && socket->tx_reserved) __builtin_trap();
    uintptr_t old_status;
    if (socket == 0) __builtin_trap();
    old_status = kernel_socket_protocol_enter();
    if (socket->read_request != 0) __builtin_trap();
    socket->destroying = 1;
    if (socket->listener && socket->pending_ready) {
        if (!socket->listener->pending_live) __builtin_trap();
        socket->listener->pending_live--;
    }
    socket->listener = 0; socket->pending_ready = 0;
    work_remove(&ready_work, socket); work_remove(&pool_wait, socket); work_remove(&nic_wait, socket); work_remove(&receive_wait, socket);
    write_retry_disarm(socket);
    if (socket->domain == KERNEL_SOCKET_DOMAIN_INET) {
        COST_ADD(NETWORK_GLOBAL_SCANS, 1);
        struct kernel_socket **link = &inet_sockets;
        while (*link != socket) { if (*link == 0) __builtin_trap(); link = &(*link)->inet_next; }
        *link = socket->inet_next; socket->inet_next = 0;
    }
    if (socket->domain == KERNEL_SOCKET_DOMAIN_UNIX) {
        struct kernel_socket *peer = socket->peer;
        if (peer != 0) {
            socket->peer = 0;
            peer->peer = 0;
            peer->peer_closed = 1U;
            wake_socket(peer);
        }
    }
    while (socket->packets_head != 0) {
        struct socket_packet *packet = socket->packets_head;
        socket->packets_head = packet->next;
        if (packet->payload != 0) {
            socket_pbuf_free(packet->payload);
        }
        if (packet->data != 0) {
            if (kernel_heap_release(socket->heap, packet->data) != KERNEL_HEAP_STATUS_OK)
                __builtin_trap();
        }
        if (kernel_heap_release(socket->heap, packet) != KERNEL_HEAP_STATUS_OK)
            __builtin_trap();
        receive_capacity();
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
    kernel_socket_protocol_leave(old_status);
    if (kernel_heap_release(socket->heap, socket) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    receive_capacity();
}

int kernel_socket_bind(struct kernel_socket *socket,
                       const struct kernel_socket_address *address)
{
    ip_addr_t local;
    if (socket->domain != KERNEL_SOCKET_DOMAIN_INET) return -KERNEL_EINVAL;
    if ((socket->udp != 0 && socket->udp->local_port != 0) ||
        (socket->tcp != 0 && socket->tcp->local_port != 0)) return -KERNEL_EINVAL;
    int result = address_import(socket, address, &local, 1);
    if (result != 0) return result;
    if (!ip_addr_isany(&local) &&
        !(IP_IS_V4(&local) ? ip4_addr_isloopback(ip_2_ip4(&local)) : ip6_addr_isloopback(ip_2_ip6(&local)))) {
        int found = 0;
        for (struct netif *n = netif_list; n; n = n->next)
            if (IP_IS_V4(&local) && ip4_addr_eq(ip_2_ip4(&local), netif_ip4_addr(n))) found = 1;
        if (!found) return -KERNEL_EADDRNOTAVAIL;
    }
    uintptr_t old_status = kernel_socket_protocol_enter();
    result = port_conflict(socket, &local, address->port, 0);
    if (result != 0) { kernel_socket_protocol_leave(old_status); return result; }
    err_t error = socket->type == SOCKET_DGRAM
        ? udp_bind(socket->udp, &local, address->port)
        : tcp_bind(socket->tcp, &local, address->port);
    if (error == ERR_OK) {
        socket->explicit_bind = 1;
        socket->explicit_port = address->port != 0;
        if (socket->udp != 0) ip_addr_copy(socket->udp_bind_ip, socket->udp->local_ip);
        if (IP_IS_V6(&local) && !ip_addr_isany(&local)) socket->v6only = 1;
    }
    kernel_socket_protocol_leave(old_status);
    return lwip_error(error);
}

int kernel_socket_getname(struct kernel_socket *socket,
                          struct kernel_socket_address *address)
{
    if (socket == 0 || address == 0) return -KERNEL_EINVAL;
    if (socket->domain == KERNEL_SOCKET_DOMAIN_UNIX) {
        *address = (struct kernel_socket_address){.family=1}; return 0;
    }
    if (socket->tcp == 0 && socket->connected && socket->type == SOCKET_STREAM) {
        *address = socket->saved_local; return 0;
    }
    if (socket->type == SOCKET_DGRAM) {
        address_export(socket, &socket->udp->local_ip, socket->udp->local_port, address);
    } else {
        if (socket->tcp == 0) return socket->error != 0 ? socket->error : -KERNEL_ENOTCONN;
        if (socket->listening) {
            struct tcp_pcb_listen *pcb = (struct tcp_pcb_listen *)socket->tcp;
            address_export(socket, &pcb->local_ip, pcb->local_port, address);
        } else {
            address_export(socket, &socket->tcp->local_ip, socket->tcp->local_port, address);
        }
    }
    return 0;
}

int kernel_socket_getpeer(struct kernel_socket *socket,
                          struct kernel_socket_address *address)
{
    if (socket == 0 || address == 0) return -KERNEL_EINVAL;
    if (!socket->connected) return -KERNEL_ENOTCONN;
    if (socket->domain == KERNEL_SOCKET_DOMAIN_UNIX) {
        *address = (struct kernel_socket_address){.family=1}; return 0;
    }
    if (socket->udp != 0)
        address_export(socket, &socket->udp->remote_ip, socket->udp->remote_port, address);
    else if (socket->tcp != 0)
        address_export(socket, &socket->tcp->remote_ip, socket->tcp->remote_port, address);
    else { *address = socket->saved_peer; }
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
    old_status = kernel_socket_protocol_enter();
    int conflict = port_conflict(socket, local_ip(socket), local_port(socket), 1);
    if (conflict != 0) { kernel_socket_protocol_leave(old_status); return conflict; }
    replacement = tcp_listen_with_backlog_and_err(socket->tcp,
        backlog < 1 ? 1U : backlog > 255 ? 255U : (u8_t)backlog, &error);
    if (replacement != 0) {
        socket->tcp = replacement;
        socket->listening = 1U;
        tcp_arg(replacement, socket);
        tcp_accept(replacement, socket_tcp_accepted);
    }
    kernel_socket_protocol_leave(old_status);
    return replacement != 0 ? 0 : lwip_error(error);
}

int kernel_socket_connect(struct kernel_socket *socket,
                          const struct kernel_socket_address *address, int nonblocking)
{
    ip_addr_t remote;
    err_t error;
    uintptr_t old_status;
    if (socket->udp != 0) {
        old_status = kernel_socket_protocol_enter();
        if (address->family == 0) {
            udp_disconnect(socket->udp); socket->connected = 0;
            if (socket->explicit_bind) ip_addr_copy(socket->udp->local_ip, socket->udp_bind_ip);
            else {
                ip_addr_set_zero(&socket->udp->local_ip);
                IP_SET_TYPE(&socket->udp->local_ip, socket->family == KERNEL_SOCKET_AF_INET ? IPADDR_TYPE_V4 :
                             socket->v6only ? IPADDR_TYPE_V6 : IPADDR_TYPE_ANY);
            }
            if (!socket->explicit_port) socket->udp->local_port = 0;
            error = ERR_OK;
        } else {
            int result = address_import(socket, address, &remote, 0);
            if (result != 0) { kernel_socket_protocol_leave(old_status); return result; }
            struct netif *route = ip_route(&socket->udp->local_ip, &remote);
            const ip_addr_t *source = route != 0 ? ip_netif_get_local_ip(route, &remote) : 0;
            if (source == 0) { kernel_socket_protocol_leave(old_status); return -KERNEL_ENETUNREACH; }
            error = udp_connect(socket->udp, &remote, address->port);
            if (error == ERR_OK) {
                socket->connected = 1;
                if (ip_addr_isany(&socket->udp->local_ip)) ip_addr_copy(socket->udp->local_ip, *source);
            }
        }
        kernel_socket_protocol_leave(old_status);
        return lwip_error(error);
    }
    if (socket->type != SOCKET_STREAM) return -KERNEL_EOPNOTSUPP;
    if (socket->tcp == 0) return socket->error != 0 ? socket->error
                                                     : -KERNEL_ENOTCONN;
    if (socket->listening) return -KERNEL_EINVAL;
    if (socket->connected) return -KERNEL_EISCONN;
    if (socket->connecting) return -KERNEL_EALREADY;
    int result = address_import(socket, address, &remote, 0);
    if (result != 0) return result;
    old_status = kernel_socket_protocol_enter();
    error = tcp_connect(socket->tcp, &remote, address->port, tcp_connected);
    if (error == ERR_OK) socket->connecting = 1U;
    kernel_socket_protocol_leave(old_status);
    if (error != ERR_OK) return lwip_error(error);
    /* NO_SYS has no network thread: the initiating syscall must deliver SYN. */
    service_inline();
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
    service_inline();
    old_status = kernel_socket_protocol_enter();
    purge_dead_pending(socket);
    entry = socket->pending_head;
    if (entry == 0) {
        kernel_socket_protocol_leave(old_status);
        return -KERNEL_EAGAIN;
    }
    socket->pending_head = entry->next;
    if (socket->pending_head == 0) socket->pending_tail = 0;
    if (!socket->pending_live || !entry->child->pending_ready) __builtin_trap();
    socket->pending_live--; entry->child->pending_ready = 0; entry->child->listener = 0;
    tcp_backlog_accepted(entry->child->tcp);
    *owner = entry->child;
    kernel_socket_protocol_leave(old_status);
    if (kernel_heap_release(socket->heap, entry) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    return 0;
}

int kernel_socket_accept_check(const struct kernel_socket *socket)
{
    if (socket->type != SOCKET_STREAM) return -KERNEL_EOPNOTSUPP;
    return socket->listening ? 0 : -KERNEL_EINVAL;
}

int kernel_socket_sendto(struct kernel_socket *socket, struct kernel_mm *mm,
                         uint64_t user_data, uint64_t size,
                         const struct kernel_socket_address *address)
{
    struct pbuf *payload;
    ip_addr_t remote;
    size_t copied = 0;
    err_t error;
    uintptr_t old_status;
    if (socket->type != SOCKET_DGRAM) return -KERNEL_EOPNOTSUPP;
    if (size > UDP_MAX_PAYLOAD) return -KERNEL_EMSGSIZE;
    int result = address_import(socket, address, &remote, 0);
    if (result != 0) return result;
    old_status = kernel_socket_protocol_enter();
    payload = pbuf_alloc(PBUF_TRANSPORT, (u16_t)size, PBUF_RAM);
    kernel_socket_protocol_leave(old_status);
    if (payload == 0) return -KERNEL_ENOMEM;
    if (size != 0 &&
        (kernel_copy_from_user(mm, payload->payload, user_data, (size_t)size,
                               &copied) != KERNEL_UACCESS_STATUS_OK ||
         copied != size)) {
        socket_pbuf_free(payload);
        return -KERNEL_EFAULT;
    }
    old_status = kernel_socket_protocol_enter();
    error = udp_sendto(socket->udp, payload, &remote, address->port);
    socket_pbuf_free(payload);
    kernel_socket_protocol_leave(old_status);
    if (error == ERR_OK) service_inline();
    return error == ERR_OK ? (int)size : lwip_error(error);
}

int kernel_socket_recvfrom(struct kernel_socket *socket, struct kernel_mm *mm,
                           uint64_t user_data, uint64_t size,
                           struct kernel_socket_address *address)
{
    struct socket_packet *packet;
    uint8_t chunk[256];
    size_t wanted;
    size_t done = 0;
    int user_fault = 0;
    uintptr_t old_status;
    if (socket->type != SOCKET_DGRAM) return -KERNEL_EOPNOTSUPP;
    service_inline();
    old_status = kernel_socket_protocol_enter();
    if (socket->read_request != 0) {
        kernel_socket_protocol_leave(old_status);
        return -KERNEL_EAGAIN;
    }
    packet = socket->packets_head;
    if (packet == 0) {
        kernel_socket_protocol_leave(old_status);
        return socket->read_closed ? 0 : -KERNEL_EAGAIN;
    }
    socket->packets_head = packet->next;
    if (socket->packets_head == 0) socket->packets_tail = 0;
    kernel_socket_protocol_leave(old_status);
    wanted = size < packet->payload->tot_len ? (size_t)size
                                             : packet->payload->tot_len;
    while (done < wanted) {
        size_t length = wanted - done;
        size_t copied = 0;
        if (length > sizeof(chunk)) length = sizeof(chunk);
        if (pbuf_copy_partial(packet->payload, chunk, (u16_t)length,
                              (u16_t)done) != length) __builtin_trap();
        enum kernel_uaccess_status access =
            kernel_copy_to_user(mm, user_data + done, chunk, length,
                                &copied);
        if (access != KERNEL_UACCESS_STATUS_OK) {
            if (access != KERNEL_UACCESS_STATUS_FAULT) __builtin_trap();
            user_fault = 1;
            break;
        }
        done += length;
    }
    *address = packet->address;
    old_status = kernel_socket_protocol_enter();
    uint32_t charge = packet->payload->tot_len ? packet->payload->tot_len : 1U;
    if (socket->rx_bytes < charge) __builtin_trap();
    socket->rx_bytes -= charge;
    socket_pbuf_free(packet->payload);
    kernel_socket_protocol_leave(old_status);
    if (kernel_heap_release(socket->heap, packet) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    return user_fault ? -KERNEL_EFAULT : (int)done;
}

/* 无队首数据时的状态；查询不消费错误，reservation入口负责交付。 */
static int socket_receive_empty(const struct kernel_socket *socket, int nonblocking)
{
    if (socket->pending_error) return socket->pending_error;
    if (socket->type == SOCKET_DGRAM && socket->read_closed && nonblocking)
        return -KERNEL_EAGAIN;
    if (socket->read_closed ||
        ((socket->type == SOCKET_STREAM || socket->domain == KERNEL_SOCKET_DOMAIN_UNIX) && socket->peer_closed))
        return 0;
    if (socket->domain == KERNEL_SOCKET_DOMAIN_INET && socket->type == SOCKET_STREAM &&
        !socket->connected && !socket->connecting)
        return -KERNEL_ENOTCONN;
    return -KERNEL_EAGAIN;
}

int kernel_socket_receive_ready(struct kernel_socket *socket)
{
    uintptr_t saved = kernel_socket_protocol_enter();
    int ready = socket->read_request == 0 &&
        (socket->listening || socket->packets_head != 0 || socket_receive_empty(socket, 0) != -KERNEL_EAGAIN);
    kernel_socket_protocol_leave(saved);
    return ready;
}

int kernel_socket_reserve_read(struct kernel_socket *socket,
                              struct kernel_task *task,
                              struct kernel_socket_read_request *request,
                              struct kernel_open_file_description **pin_owner,
                              uint32_t capacity, int nonblocking)
{
    struct socket_packet *packet;
    uint32_t available;
    uint32_t length;
    uintptr_t old_status;
    if (request == 0 || request->socket != 0 ||
        pin_owner == 0 || *pin_owner == 0 ||
        kernel_open_file_socket(*pin_owner) != socket)
        __builtin_trap();
    if (socket->listening) return -KERNEL_ENOTCONN;
    if (socket->domain == KERNEL_SOCKET_DOMAIN_INET) {
        service_inline();
    }
    old_status = kernel_socket_protocol_enter();
    if (socket->read_request != 0) {
        kernel_socket_protocol_leave(old_status);
        return -KERNEL_EAGAIN;
    }
    packet = socket->packets_head;
    if (packet == 0) {
        int result = socket_receive_empty(socket, nonblocking);
        if (socket->pending_error) socket->pending_error = 0;
        kernel_socket_protocol_leave(old_status);
        return result;
    }
    /* recv(0)检查连接/等待状态，但不取得或消费已有stream数据。 */
    if (capacity == 0U && socket->type == SOCKET_STREAM) {
        kernel_socket_protocol_leave(old_status);
        return 0;
    }
    available = packet->payload != 0 ? (uint32_t)(packet->payload->tot_len - packet->consumed)
                                     : (packet->length - packet->consumed);
    length = capacity < available ? capacity : available;
    request->socket = socket;
    request->task = task;
    request->packet = packet;
    request->bytes = length;
    request->datagram = socket->type == SOCKET_DGRAM;
    request->pin = *pin_owner;
    request->pin_owner = pin_owner;
    *pin_owner = 0;
    if (task != 0 &&
        kernel_task_socket_read_register(task, request) != KERNEL_TASK_STATUS_OK)
        __builtin_trap();
    socket->read_request = request;
    kernel_socket_protocol_leave(old_status);
    return (int)length;
}

void kernel_socket_read_info(const struct kernel_socket_read_request *request,
                             struct kernel_socket_address *address, uint32_t *length)
{
    const struct socket_packet *packet = request->packet;
    if (request->socket == 0 || packet == 0) __builtin_trap();
    if (address != 0) {
        if (request->socket->domain == KERNEL_SOCKET_DOMAIN_UNIX)
            *address = (struct kernel_socket_address){.family=1};
        else *address = packet->address;
    }
    if (length != 0) *length = packet->payload != 0 ? (uint32_t)(packet->payload->tot_len - packet->consumed)
                                                  : packet->length - packet->consumed;
}

void kernel_socket_copy_read(const struct kernel_socket_read_request *request,
                             uint32_t offset, void *buffer, uint32_t length)
{
    const struct socket_packet *packet = request->packet;
    if (request->socket == 0 || request->socket->read_request != request ||
        offset > request->bytes || length > request->bytes - offset)
        __builtin_trap();
    if (length == 0) return;
    if (packet->payload != 0) {
        if (pbuf_copy_partial(packet->payload, buffer, (u16_t)length,
                              packet->consumed + offset) != length)
            __builtin_trap();
    } else if (packet->data != 0) {
        __builtin_memcpy(buffer, (const char *)packet->data + packet->consumed + offset, length);
    } else {
        __builtin_trap();
    }
}

void kernel_socket_finish_read(struct kernel_socket_read_request *request,
                               int user_fault)
{
    uintptr_t old_status = kernel_socket_protocol_enter();
    struct kernel_socket *socket = request->socket;
    struct socket_packet *packet;
    uint32_t bytes = request->bytes;
    uint32_t available;
    if (socket == 0 || socket->read_request != request ||
        socket->packets_head != request->packet) __builtin_trap();
    packet = socket->packets_head;
    available = packet->payload != 0 ? (uint32_t)(packet->payload->tot_len - packet->consumed)
                                     : (packet->length - packet->consumed);
    if (bytes > available ||
        (socket->type == SOCKET_STREAM && bytes == 0U))
        __builtin_trap();
    /* TCP/STREAM advances only after the complete staged copy succeeds. A fault in
     * this skb leaves its bytes queued, even when usercopy wrote a prefix.
     * UDP drops the datagram on copy fault, matching udp_recvmsg. */
    if (socket->type == SOCKET_STREAM && user_fault) {
        socket->read_request = 0;
        if (request->task != 0 &&
            kernel_task_socket_read_clear(request->task, request) !=
                KERNEL_TASK_STATUS_OK) __builtin_trap();
        if (*request->pin_owner != 0 || request->pin == 0) __builtin_trap();
        *request->pin_owner = request->pin;
        request->pin = 0;
        request->socket = 0;
        wake_socket(socket);
        kernel_socket_protocol_leave(old_status);
        return;
    }
    if (socket->type == SOCKET_DGRAM || bytes == available) {
        socket->packets_head = packet->next;
        if (socket->packets_head == 0) socket->packets_tail = 0;
        if (packet->payload != 0) socket_pbuf_free(packet->payload);
        if (packet->data != 0) {
            if (kernel_heap_release(socket->heap, packet->data) != KERNEL_HEAP_STATUS_OK)
                __builtin_trap();
        }
        if (kernel_heap_release(socket->heap, packet) != KERNEL_HEAP_STATUS_OK)
            __builtin_trap();
        receive_capacity();
    } else {
        packet->consumed += (uint16_t)bytes;
    }
    {
        uint32_t charge = socket->type == SOCKET_DGRAM ? (available ? available : 1U) : bytes;
        if (socket->rx_bytes < charge) __builtin_trap();
        socket->rx_bytes -= charge;
        if (socket->peer != 0) {
            wake_socket(socket->peer);
        }
    }
    if (socket->type == SOCKET_STREAM && socket->tcp != 0) {
        tcp_recved(socket->tcp, (u16_t)bytes);
        socket_schedule(socket, SOCKET_WORK_RECEIVE | SOCKET_WORK_OUTPUT | SOCKET_WORK_REAP);
    }
    socket->read_request = 0;
    if (request->task != 0 &&
        kernel_task_socket_read_clear(request->task, request) !=
            KERNEL_TASK_STATUS_OK) __builtin_trap();
    if (*request->pin_owner != 0 || request->pin == 0) __builtin_trap();
    *request->pin_owner = request->pin;
    request->pin = 0;
    request->socket = 0;
    wake_socket(socket);
    kernel_socket_protocol_leave(old_status);
}

void kernel_socket_abort_read(struct kernel_socket_read_request *request)
{
    uintptr_t old_status = kernel_socket_protocol_enter();
    struct kernel_socket *socket;
    struct kernel_open_file_description *pin;
    if (request == 0 || request->socket == 0) __builtin_trap();
    socket = request->socket;
    if (socket->read_request != request ||
        socket->packets_head != request->packet ||
        (request->task != 0 &&
         kernel_task_socket_read_clear(request->task, request) !=
             KERNEL_TASK_STATUS_OK)) __builtin_trap();
    socket->read_request = 0;
    pin = request->pin;
    if (pin == 0 || *request->pin_owner != 0) __builtin_trap();
    request->pin = 0;
    request->socket = 0;
    wake_socket(socket);
    kernel_socket_protocol_leave(old_status);
    if (kernel_open_file_release(&pin) != KERNEL_OPEN_FILE_STATUS_OK)
        __builtin_trap();
}

static void free_write_packet(struct kernel_socket_write_request *request)
{
    struct socket_packet *packet = request->packet;
    if (!packet) return;
    if (packet->data && kernel_heap_release(request->socket->heap, packet->data) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
    if (kernel_heap_release(request->socket->heap, packet) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    request->packet = 0;
    receive_capacity();
}
static void clear_write_request(struct kernel_socket_write_request *request)
{
    kernel_socket_stream_cancel(request);
    free_write_packet(request);
    if (request->task && kernel_task_socket_write_clear(request->task, request) != KERNEL_TASK_STATUS_OK)
        __builtin_trap();
}
void kernel_socket_abort_write(struct kernel_socket_write_request *request)
{
    clear_write_request(request);
    struct kernel_open_file_description *pin = request->pin;
    request->pin = 0;
    if (kernel_open_file_release(&pin) != KERNEL_OPEN_FILE_STATUS_OK) __builtin_trap();
}
int kernel_socket_is_datagram(const struct kernel_socket *socket)
{ return socket && socket->type == SOCKET_DGRAM; }
static int socket_write_datagram_source(struct kernel_open_file_description **pin_owner,
    struct kernel_mm *mm, const struct kernel_uaccess_iovec *iov,
    size_t iov_count, uint64_t count, uint32_t flags,
    const struct kernel_socket_address *destination, const void *kernel_buffer)
{
    struct kernel_socket *socket = kernel_open_file_socket(*pin_owner);
    /* sendfile 的 UDP actor 在协议长度检查前解析缺失的 connected peer。 */
    if (kernel_buffer && socket->domain == KERNEL_SOCKET_DOMAIN_INET && !destination &&
        !socket->connected && count <= UINT16_MAX) return -KERNEL_EDESTADDRREQ;
    if (count > (socket->domain == KERNEL_SOCKET_DOMAIN_INET ? UDP_MAX_PAYLOAD : 65536U) ||
        count > socket->tx_limit) return -KERNEL_EMSGSIZE;
    if (socket->write_closed) {
        if (!(flags & KERNEL_SOCKET_MSG_NOSIGNAL) && kernel_task_current())
            (void)kernel_signal_send_task(kernel_task_current(), 13U, 0);
        return -KERNEL_EPIPE;
    }
    if (socket->domain == KERNEL_SOCKET_DOMAIN_INET && !destination && !socket->connected)
        return -KERNEL_EDESTADDRREQ;
    struct kernel_socket_write_request request = {
        .socket = kernel_open_file_socket(*pin_owner), .task = kernel_task_current(),
        .pin = *pin_owner, .pin_owner = pin_owner,
    };
    *pin_owner = 0;
    if (request.task && kernel_task_socket_write_register(request.task, &request) != KERNEL_TASK_STATUS_OK)
        __builtin_trap();
    int result = -KERNEL_ENOMEM;
    struct socket_packet *packet = 0;
    {
        KERNEL_NO_RECLAIM_IO;
        enum kernel_heap_status status = kernel_heap_allocate_zeroed(request.socket->heap,
            1, sizeof(*packet), (void **)&packet);
        if (status != KERNEL_HEAP_STATUS_OK) {
            if (status != KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
            goto out;
        }
        request.packet = packet;
        packet->length = (uint32_t)count;
        if (count) {
            status = kernel_heap_allocate(request.socket->heap, (size_t)count, &packet->data);
            if (status != KERNEL_HEAP_STATUS_OK) {
                if (status != KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
                goto out;
            }
        }
    }
    if (kernel_buffer) {
        if (count) __builtin_memcpy(packet->data, kernel_buffer, (size_t)count);
    } else {
        struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0, 0};
        size_t copied = 0;
        enum kernel_uaccess_status access = kernel_copy_from_user_iov(mm, &cursor,
            packet->data, (size_t)count, &copied);
        if (access == KERNEL_UACCESS_STATUS_FAULT) { result = -KERNEL_EFAULT; goto out; }
        if (access != KERNEL_UACCESS_STATUS_OK || copied != count) __builtin_trap();
    }
    if (request.socket->domain == KERNEL_SOCKET_DOMAIN_INET) {
        ip_addr_t remote;
        int imported = destination ? address_import(request.socket, destination, &remote, 0) : 0;
        if (imported != 0) { result = imported; goto out; }
        uintptr_t saved = kernel_socket_protocol_enter();
        struct pbuf *payload = pbuf_alloc(PBUF_TRANSPORT, (u16_t)count, PBUF_RAM);
        if (payload == 0) { kernel_socket_protocol_leave(saved); result = -KERNEL_ENOMEM; goto out; }
        if (count) __builtin_memcpy(payload->payload, packet->data, (size_t)count);
        err_t error = destination ? udp_sendto(request.socket->udp, payload, &remote, destination->port)
                                 : udp_send(request.socket->udp, payload);
        socket_pbuf_free(payload);
        kernel_socket_protocol_leave(saved);
        if (error == ERR_OK) service_inline();
        result = error == ERR_OK ? (int)count : lwip_error(error);
        goto out;
    }
    uintptr_t saved = riscv_interrupt_save();
    uint32_t charge = count ? (uint32_t)count : 1U;
    uint64_t deadline = 0;
    if (request.socket->send_timeout_ns) {
        uint64_t now = kernel_time_monotonic_ns(), timeout = request.socket->send_timeout_ns;
        uint64_t target = timeout > UINT64_MAX-now ? UINT64_MAX : now+timeout;
        enum kernel_time_status converted = kernel_time_deadline_from_monotonic(target,&deadline);
        if (converted != KERNEL_TIME_STATUS_OK) __builtin_trap();
    }
    for (;;) {
        struct kernel_socket *peer = request.socket->peer;
        if (!peer || request.socket->peer_closed || request.socket->write_closed || peer->read_closed) {
            result = -KERNEL_EPIPE;
            if (request.task && !(flags & KERNEL_SOCKET_MSG_NOSIGNAL))
                (void)kernel_signal_send_task(request.task, 13U, 0);
            break;
        }
        uint32_t capacity = peer->rx_bytes < peer->rx_limit ? peer->rx_limit - peer->rx_bytes : 0U;
        if (charge <= capacity) {
            /* 只有整条复制成功并有整条预算时才移动 packet owner。 */
            if (peer->packets_tail) peer->packets_tail->next = packet;
            else peer->packets_head = packet;
            peer->packets_tail = packet;
            peer->rx_bytes += charge;
            request.packet = 0;
            wake_socket(peer);
            result = (int)count;
            break;
        }
        if (flags & KERNEL_SOCKET_MSG_DONTWAIT) { result = -KERNEL_EAGAIN; break; }
        enum kernel_wait_wake_reason reason;
        if (kernel_scheduler_block_current(&request.socket->wait, deadline, 1, &reason) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
        if (reason == KERNEL_WAIT_TIMEOUT) { result = -KERNEL_EAGAIN; break; }
        if (reason == KERNEL_WAIT_SIGNALLED) {
            if (deadline) result = -KERNEL_EINTR;
            else { kernel_signal_note_syscall_restart(request.task); result = -KERNEL_ERESTARTSYS; }
            break;
        }
    }
    riscv_interrupt_restore(saved);
out:
    clear_write_request(&request);
    *pin_owner = request.pin;
    return result;
}

int kernel_socket_write_datagram(struct kernel_open_file_description **pin_owner,
    struct kernel_mm *mm, const struct kernel_uaccess_iovec *iov,
    size_t iov_count, uint64_t count, uint32_t flags,
    const struct kernel_socket_address *destination)
{
    return socket_write_datagram_source(pin_owner, mm, iov, iov_count, count,
                                        flags, destination, 0);
}

int kernel_socket_write_datagram_buffer(struct kernel_open_file_description **pin_owner,
    const void *buffer, uint64_t count, uint32_t flags)
{
    if (!buffer) return -KERNEL_EINVAL;
    return socket_write_datagram_source(pin_owner, 0, 0, 0, count, flags, 0, buffer);
}

int kernel_socket_discard_receive(const struct kernel_socket *socket, uint32_t flags)
{
    return socket && socket->domain == KERNEL_SOCKET_DOMAIN_INET &&
           socket->type == SOCKET_STREAM && (flags & KERNEL_SOCKET_MSG_TRUNC);
}

static struct kernel_socket_statistics socket_statistics;
void kernel_socket_get_statistics(struct kernel_socket_statistics *statistics)
{
    *statistics = socket_statistics;
}

#if BOAROS_COST_DIAGNOSTICS
void kernel_socket_protocol_snapshot(uint64_t values[KERNEL_SOCKET_PROTOCOL_VALUES])
{
    uintptr_t irq = kernel_socket_protocol_enter();
    const uint64_t snapshot[KERNEL_SOCKET_PROTOCOL_VALUES] = {
        socket_statistics.tcp_write_calls, socket_statistics.tcp_written_bytes,
        lwip_stats.tcp.xmit,lwip_stats.tcp.recv,lwip_stats.tcp.memerr,
        lwip_stats.udp.xmit,lwip_stats.udp.recv,lwip_stats.udp.memerr,
        lwip_stats.mem.used,lwip_stats.mem.max,
        (lwip_stats.memp[MEMP_TCP_PCB] ? lwip_stats.memp[MEMP_TCP_PCB]->used : 0),(lwip_stats.memp[MEMP_TCP_PCB_LISTEN] ? lwip_stats.memp[MEMP_TCP_PCB_LISTEN]->used : 0),
        (lwip_stats.memp[MEMP_TCP_SEG] ? lwip_stats.memp[MEMP_TCP_SEG]->used : 0),(lwip_stats.memp[MEMP_UDP_PCB] ? lwip_stats.memp[MEMP_UDP_PCB]->used : 0),
        (lwip_stats.memp[MEMP_PBUF] ? lwip_stats.memp[MEMP_PBUF]->used : 0),sizeof(STAT_COUNTER)*8U
    };
    __builtin_memcpy(values,snapshot,sizeof(snapshot));
    kernel_socket_protocol_leave(irq);
}
#endif

int kernel_socket_is_tcp(const struct kernel_socket *socket)
{ return socket && socket->domain == KERNEL_SOCKET_DOMAIN_INET && socket->type == SOCKET_STREAM; }

/* 调用者持raw资格；此处交付状态错误，不能先碰payload改变errno优先级。 */
static int tcp_write_state(struct kernel_socket *socket, uint32_t flags, int observe)
{
    if (socket->pending_error) {
        int result = socket->pending_error; if (observe) socket->pending_error = 0; return result;
    }
    if (socket->write_closed || (!socket->connected && !socket->connecting) || !socket->tcp) {
        if (observe && !(flags & KERNEL_SOCKET_MSG_NOSIGNAL) && kernel_task_current())
            (void)kernel_signal_send_task(kernel_task_current(), 13U, 0);
        return -KERNEL_EPIPE;
    }
    return socket->connecting ? -KERNEL_EAGAIN : 0;
}
static uint32_t tcp_admission_capacity(const struct kernel_socket *socket)
{
    if (!socket->tcp || socket->write_blocked || tcp_sndqueuelen(socket->tcp) >= TCP_SND_QUEUELEN) return 0;
    uint32_t available = tcp_sndbuf(socket->tcp);
    uint32_t outstanding = TCP_SND_BUF - available;
    uint32_t budget = outstanding < socket->tx_limit ? socket->tx_limit - outstanding : 0;
    if (available > budget) available = budget;
    return available > socket->tx_reserved ? available - socket->tx_reserved : 0;
}
void kernel_socket_stream_begin(struct kernel_socket_write_request *request,
    struct kernel_open_file_description **pin_owner)
{
    if (!request || !pin_owner || !kernel_socket_is_tcp(kernel_open_file_socket(*pin_owner))) __builtin_trap();
    *request = (struct kernel_socket_write_request){.socket = kernel_open_file_socket(*pin_owner),
        .task = kernel_task_current(), .pin = *pin_owner, .pin_owner = pin_owner};
    *pin_owner = 0;
    if (request->task && kernel_task_socket_write_register(request->task, request) != KERNEL_TASK_STATUS_OK)
        __builtin_trap();
}
int kernel_socket_stream_reserve(struct kernel_socket_write_request *request,
    uint32_t size, uint32_t flags)
{
    uintptr_t irq = kernel_socket_protocol_enter();
    if (!request->pin || request->reserved) __builtin_trap();
    int error = tcp_write_state(request->socket, flags, !request->progressed);
    uint32_t available = error ? 0 : tcp_admission_capacity(request->socket);
    if (!error && size) {
        if (size > available) size = available;
        if (!size) { error = -KERNEL_EAGAIN; COST_ADD(STREAM_ADMIT_BLOCKED, 1); }
        else { request->reserved = size; request->socket->tx_reserved += size; }
    }
    kernel_socket_protocol_leave(irq);
    return error ? error : (int)size;
}
static void stream_release_reservation(struct kernel_socket_write_request *request, int notify)
{
    if (!request->reserved) return;
    uintptr_t irq = kernel_socket_protocol_enter();
    if (request->socket->tx_reserved < request->reserved) __builtin_trap();
    request->socket->tx_reserved -= request->reserved; request->reserved = 0;
    if (notify) wake_socket(request->socket);
    kernel_socket_protocol_leave(irq);
}
void kernel_socket_stream_cancel(struct kernel_socket_write_request *request)
{ stream_release_reservation(request, 1); }
void kernel_socket_stream_finish(struct kernel_socket_write_request *request)
{
    kernel_socket_stream_cancel(request);
    if (request->task && kernel_task_socket_write_clear(request->task, request) != KERNEL_TASK_STATUS_OK)
        __builtin_trap();
    if (*request->pin_owner || !request->pin) __builtin_trap();
    *request->pin_owner = request->pin;
    request->pin = 0; request->socket = 0;
}
int kernel_socket_stream_commit(struct kernel_socket_write_request *request,
    const void *buffer, uint32_t size, uint32_t flags)
{
    uintptr_t irq = kernel_socket_protocol_enter();
    if (!size || size > request->reserved) __builtin_trap();
    /* 仅在同一个raw临界区内解除本请求预算并提交；其他writer仍看到剩余reservation。 */
    stream_release_reservation(request, 0);
    /* 已交付前缀优先：保留后来错误供下一次调用观察，也不为短成功发送SIGPIPE。 */
    int result = tcp_write_state(request->socket, flags, !request->progressed);
    if (!result) result = kernel_socket_write_buffer(request->socket, buffer, size, flags);
    if (result > 0) request->progressed = 1;
    if (result == -KERNEL_EAGAIN) COST_ADD(STREAM_PROTOCOL_BLOCKED, 1);
    if (tcp_admission_capacity(request->socket)) wake_socket(request->socket);
    kernel_socket_protocol_leave(irq);
    if (result > 0) service_inline();
    return result;
}

int kernel_socket_write_buffer(struct kernel_socket *socket,
                               const void *buffer, uint32_t size, uint32_t flags)
{
    uint32_t length;
    err_t error;
    uintptr_t old_status;

    if (socket->domain == KERNEL_SOCKET_DOMAIN_UNIX) {
        old_status = kernel_socket_protocol_enter();
        if (socket->peer == 0 || socket->write_closed || socket->peer->read_closed) {
            struct kernel_task *curr = kernel_task_current();
            kernel_socket_protocol_leave(old_status);
            if (curr != 0 && !(flags & KERNEL_SOCKET_MSG_NOSIGNAL)) {
                (void)kernel_signal_send_task(curr, 13U, 0);
            }
            return -KERNEL_EPIPE;
        }
        if (size == 0U && socket->type == SOCKET_STREAM) {
            kernel_socket_protocol_leave(old_status);
            return 0;
        }
        if (socket->type == SOCKET_DGRAM && size > 65536U) {
            kernel_socket_protocol_leave(old_status); return -KERNEL_EMSGSIZE;
        }
        struct kernel_socket *peer = socket->peer;
        if (peer->rx_bytes >= peer->rx_limit) {
            kernel_socket_protocol_leave(old_status);
            return -KERNEL_EAGAIN;
        }
        uint32_t to_write = size;
        if (socket->type == SOCKET_DGRAM && (size ? size : 1U) > peer->rx_limit - peer->rx_bytes) {
            kernel_socket_protocol_leave(old_status); return -KERNEL_EAGAIN;
        }
        if (to_write > peer->rx_limit - peer->rx_bytes) {
            to_write = peer->rx_limit - peer->rx_bytes;
        }
        struct socket_packet *packet = 0;
        void *data_buf = 0;
        enum kernel_heap_status heap_status;
        heap_status = kernel_heap_allocate(peer->heap, to_write ? (size_t)to_write : 1U, (void **)&data_buf);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            kernel_socket_protocol_leave(old_status);
            return heap_status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        }
        heap_status = kernel_heap_allocate_zeroed(peer->heap, 1U, sizeof(*packet), (void **)&packet);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            (void)kernel_heap_release(peer->heap, data_buf);
            kernel_socket_protocol_leave(old_status);
            return heap_status == KERNEL_HEAP_STATUS_EMPTY ? -KERNEL_ENOMEM : -KERNEL_EIO;
        }
        __builtin_memcpy(data_buf, buffer, to_write);
        packet->data = data_buf;
        packet->length = to_write;
        packet->consumed = 0U;

        if (peer->packets_tail != 0) {
            peer->packets_tail->next = packet;
        } else {
            peer->packets_head = packet;
        }
        peer->packets_tail = packet;
        peer->rx_bytes += to_write ? to_write : 1U;

        wake_socket(peer);
        kernel_socket_protocol_leave(old_status);
        return (int)to_write;
    }

    old_status = kernel_socket_protocol_enter();
    if (socket->type == SOCKET_DGRAM) {
        kernel_socket_protocol_leave(old_status); return -KERNEL_EDESTADDRREQ;
    }
    int state = tcp_write_state(socket, flags, 1);
    if (state || !size) { kernel_socket_protocol_leave(old_status); return state; }
    length = tcp_admission_capacity(socket);
    if (length > size) length = size;
    if (length > UINT16_MAX) length = UINT16_MAX;
    if (!length) { kernel_socket_protocol_leave(old_status); return -KERNEL_EAGAIN; }
    socket_statistics.tcp_write_calls++;
    error = tcp_write(socket->tcp, buffer, (u16_t)length, TCP_WRITE_FLAG_COPY);
    if (error == ERR_OK) {
        socket_statistics.tcp_written_bytes += length;
        write_retry_disarm(socket);
        /* tcp_write owns the copied bytes even if immediate output defers. */
        nic_blocked = 0;
        err_t output = tcp_output(socket->tcp);
        if (output == ERR_MEM) {
            if (nic_blocked) { work_remove(&nic_wait, socket); work_add(&nic_wait, socket, nic_generation); }
            else socket_pool_wait(socket);
        }
        socket_schedule(socket, SOCKET_WORK_REAP);
    } else if (error == ERR_MEM) {
        /* The global segment/pbuf pool can be full even while sndbuf remains.
         * Hide POLLOUT until an ACK or a bounded protocol-timer retry. */
        write_retry_arm(socket);
        socket_pool_wait(socket);
    }
    kernel_socket_protocol_leave(old_status);
    if (error == ERR_OK) service_inline();
    return error == ERR_OK ? (int)length
                           : error == ERR_MEM ? -KERNEL_EAGAIN
                                              : lwip_error(error);
}

int kernel_socket_shutdown(struct kernel_socket *socket, int how)
{
    if (how < 0 || how > 2) return -KERNEL_EINVAL;
    uintptr_t saved = kernel_socket_protocol_enter();
    int result = 0;
    if (how != 1) socket->read_closed = 1;
    if (how != 0 && !socket->write_closed) {
        socket->write_closed = 1;
        if (socket->tcp != 0 && socket->connected && !socket->listening) {
            /* 只关闭 raw TX；raw RX 关闭会丢 owner 或重置对端。FIN 排在已接受字节后。 */
            err_t error = tcp_shutdown(socket->tcp, 0, 1);
            if (error != ERR_OK) result = lwip_error(error);
        }
        if (socket->peer != 0 && socket->type == SOCKET_STREAM) {
            socket->peer->peer_closed = 1; wake_socket(socket->peer);
        }
    }
    if (!socket->connected) result = -KERNEL_ENOTCONN;
    wake_socket(socket);
    if (socket->peer != 0) wake_socket(socket->peer);
    socket_schedule(socket, SOCKET_WORK_REAP | SOCKET_WORK_OUTPUT);
    kernel_socket_protocol_leave(saved);
    service_inline();
    return result;
}

int kernel_socket_set_option(struct kernel_socket *socket,
                             enum kernel_socket_option option, int value)
{
    uintptr_t saved = kernel_socket_protocol_enter();
    int result = 0;
    struct ip_pcb *pcb = socket->udp != 0 ? (struct ip_pcb *)socket->udp : (struct ip_pcb *)socket->tcp;
    switch (option) {
    case KERNEL_SOCKET_REUSEADDR:
    case KERNEL_SOCKET_KEEPALIVE: {
        uint8_t flag = option == KERNEL_SOCKET_REUSEADDR ? SOF_REUSEADDR : SOF_KEEPALIVE;
        if (option == KERNEL_SOCKET_REUSEADDR) socket->reuseaddr = value != 0;
        else socket->keepalive = value != 0;
        if (pcb != 0) { if (value) ip_set_option(pcb, flag); else ip_reset_option(pcb, flag); }
        break;
    }
    case KERNEL_SOCKET_SNDBUF:
    case KERNEL_SOCKET_RCVBUF: {
        uint32_t limit = (uint32_t)value;
        uint32_t minimum = option == KERNEL_SOCKET_SNDBUF ? 4608U : 2304U;
        /* 固定 RV64 Linux 的倍增/下限，实际本地预算限制为 64 KiB。 */
        if (limit > 32768U) limit = 32768U;
        limit *= 2;
        if (limit < minimum) limit = minimum;
        if (option == KERNEL_SOCKET_SNDBUF) socket->tx_limit = limit;
        else {
            uint32_t previous = socket->rx_limit;
            socket->rx_limit = limit;
            /* UNIX发送者等待对端预算；peer仅在当前保护区内借用。 */
            if (limit > previous && socket->domain == KERNEL_SOCKET_DOMAIN_UNIX && socket->peer != 0)
                wake_socket(socket->peer);
        }
        wake_socket(socket);
        break;
    }
    case KERNEL_SOCKET_NODELAY:
        if (socket->domain != KERNEL_SOCKET_DOMAIN_INET || socket->type != SOCKET_STREAM) { result = -KERNEL_ENOPROTOOPT; break; }
        socket->nodelay = value != 0;
        if (socket->tcp != 0 && !socket->listening) {
            if (value) tcp_nagle_disable(socket->tcp); else tcp_nagle_enable(socket->tcp);
        }
        break;
    case KERNEL_SOCKET_V6ONLY:
        if (socket->family != KERNEL_SOCKET_AF_INET6) { result = -KERNEL_ENOPROTOOPT; break; }
        if ((socket->udp != 0 && socket->udp->local_port != 0) ||
            (socket->tcp != 0 && socket->tcp->local_port != 0)) { result = -KERNEL_EINVAL; break; }
        socket->v6only = value != 0;
        if (pcb != 0) {
            ip_addr_set_zero_ip6(&pcb->local_ip);
            if (!value) IP_SET_TYPE(&pcb->local_ip, IPADDR_TYPE_ANY);
        }
        break;
    default: result = -KERNEL_ENOPROTOOPT;
    }
    kernel_socket_protocol_leave(saved);
    return result;
}

int kernel_socket_get_option(struct kernel_socket *socket,
                             enum kernel_socket_option option, int *value)
{
    uintptr_t saved = kernel_socket_protocol_enter();
    int result = 0;
    switch (option) {
    case KERNEL_SOCKET_REUSEADDR: *value = socket->reuseaddr; break;
    case KERNEL_SOCKET_KEEPALIVE: *value = socket->keepalive; break;
    case KERNEL_SOCKET_SNDBUF: *value = (int)socket->tx_limit; break;
    case KERNEL_SOCKET_RCVBUF: *value = (int)socket->rx_limit; break;
    case KERNEL_SOCKET_TYPE: *value = socket->type; break;
    case KERNEL_SOCKET_ACCEPTCONN: *value = socket->listening; break;
    case KERNEL_SOCKET_ERROR: *value = -socket->pending_error; socket->pending_error = 0; break;
    case KERNEL_SOCKET_NODELAY:
        if (socket->domain != KERNEL_SOCKET_DOMAIN_INET || socket->type != SOCKET_STREAM) result = -KERNEL_ENOPROTOOPT;
        else *value = socket->nodelay;
        break;
    case KERNEL_SOCKET_MAXSEG:
        if (socket->tcp == 0) result = -KERNEL_ENOPROTOOPT;
        else *value = socket->listening ? TCP_MSS : tcp_mss(socket->tcp);
        break;
    case KERNEL_SOCKET_V6ONLY:
        if (socket->family != KERNEL_SOCKET_AF_INET6) result = -KERNEL_ENOPROTOOPT;
        else *value = socket->v6only;
        break;
    default: result = -KERNEL_ENOPROTOOPT;
    }
    kernel_socket_protocol_leave(saved);
    return result;
}
void kernel_socket_set_send_timeout(struct kernel_socket *socket, uint64_t nanoseconds)
{
    socket->send_timeout_ns = nanoseconds;
}
uint64_t kernel_socket_send_timeout(const struct kernel_socket *socket)
{
    return socket->send_timeout_ns;
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
    uintptr_t snapshot_irq = kernel_socket_protocol_enter();
    COST_ADD(NETWORK_POLL_CALLS, 1);
#if BOAROS_COST_DIAGNOSTICS
    polling_snapshot++;
#endif
    if (queue != 0) *queue = &socket->wait;

    if (socket->domain == KERNEL_SOCKET_DOMAIN_UNIX) {
        uintptr_t saved = kernel_socket_protocol_enter();
        if (socket->type == SOCKET_DGRAM) {
            if ((socket->packets_head != 0 || socket->read_closed) && socket->read_request == 0)
                events |= KERNEL_POLLIN | KERNEL_POLLRDNORM;
            if (socket->read_closed) events |= KERNEL_POLLRDHUP;
            if (socket->read_closed && socket->write_closed) events |= KERNEL_POLLHUP;
            if (socket->peer_closed) {
                events |= KERNEL_POLLIN | KERNEL_POLLRDNORM | KERNEL_POLLHUP;
            }
            if (socket->peer != 0 && !socket->peer->read_closed) {
                if (socket->peer->rx_bytes < socket->peer->rx_limit) {
                    events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM;
                }
            } else {
                events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM | KERNEL_POLLHUP;
            }
        } else {
            if ((socket->packets_head != 0 || socket->peer_closed || socket->read_closed) &&
                socket->read_request == 0)
                events |= KERNEL_POLLIN | KERNEL_POLLRDNORM;
            if (socket->peer_closed || socket->read_closed) events |= KERNEL_POLLRDHUP;
            if ((socket->peer_closed || socket->read_closed) && socket->write_closed) events |= KERNEL_POLLHUP;
            if (socket->peer == 0) events |= KERNEL_POLLHUP;
            if (socket->peer != 0 && !socket->peer->read_closed) {
                if (socket->peer->rx_bytes < socket->peer->rx_limit) {
                    events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM;
                }
            } else {
                events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM | KERNEL_POLLERR;
            }
        }
        kernel_socket_protocol_leave(saved);
#if BOAROS_COST_DIAGNOSTICS
        polling_snapshot--;
#endif
        kernel_socket_protocol_leave(snapshot_irq);
        return events;
    }

    if (socket->type == SOCKET_DGRAM) {
        if ((socket->packets_head != 0 || socket->read_closed) && socket->read_request == 0)
            events |= KERNEL_POLLIN | KERNEL_POLLRDNORM;
        events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM;
    } else if (socket->listening) {
        if (socket->pending_live != 0) events |= KERNEL_POLLIN | KERNEL_POLLRDNORM;
    } else if (socket->connected) {
        if (socket->peer_closed || socket->read_closed) events |= KERNEL_POLLRDHUP;
        if ((socket->peer_closed || socket->read_closed) && socket->write_closed) events |= KERNEL_POLLHUP;
        if (socket->write_closed) events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM;
        if ((socket->packets_head != 0 || socket->peer_closed || socket->read_closed) &&
            socket->read_request == 0)
            events |= KERNEL_POLLIN | KERNEL_POLLRDNORM;
        uintptr_t saved = kernel_socket_protocol_enter();
        if (tcp_admission_capacity(socket))
            events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM;
        kernel_socket_protocol_leave(saved);
    } else if (socket->peer_closed) {
        events |= KERNEL_POLLHUP | KERNEL_POLLOUT | KERNEL_POLLRDHUP | KERNEL_POLLIN;
    } else if (socket_receive_empty(socket, 0) == -KERNEL_ENOTCONN) {
        events |= KERNEL_POLLHUP | KERNEL_POLLOUT | KERNEL_POLLWRNORM;
    }
    if (socket->pending_error) events |= KERNEL_POLLERR;
#if BOAROS_COST_DIAGNOSTICS
    polling_snapshot--;
#endif
    kernel_socket_protocol_leave(snapshot_irq);
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
    old_status = kernel_socket_protocol_enter();
    if ((flags & UINT16_C(0x1)) != 0U) netif_set_up(loopback);
    else netif_set_down(loopback);
    kernel_socket_protocol_leave(old_status);
    return 0;
}

void kernel_socket_network_initialize(void)
{
    if (!socket_initialized) {
        const struct boaros_lwip_hooks hooks = {protocol_input, protocol_work_ready, protocol_capacity};
        boaros_lwip_set_hooks(&hooks);
        lwip_init(); socket_initialized = 1;
    }
}
void kernel_socket_network_hooks(void (*timer_wake)(void *), void (*work_wake)(void *),
    int (*udp_capacity)(void *), void *context)
{
    uintptr_t irq = kernel_socket_protocol_enter();
    network_timer_wake = timer_wake; network_work_wake = work_wake;
    network_udp_capacity = udp_capacity; network_context = context;
    kernel_socket_protocol_leave(irq);
}
void kernel_socket_network_capacity(void)
{
    uintptr_t irq = kernel_socket_protocol_enter();
    nic_generation++;
    if (nic_wait.head && network_work_wake) network_work_wake(network_context);
    kernel_socket_protocol_leave(irq);
}
void kernel_socket_network_blocked(void) { nic_blocked = 1; }
static int capacity_ready(const struct socket_work_queue *queue, uint64_t generation)
{ return queue->head && queue->head->work_links[queue->slot].epoch != generation; }

int kernel_socket_work_pending(void)
{
    if (!socket_initialized) return 0;
    uintptr_t irq = kernel_socket_protocol_enter();
    int runnable = ready_work.head || loopback_pending ||
        capacity_ready(&pool_wait, pool_generation) || capacity_ready(&nic_wait, nic_generation) ||
        capacity_ready(&receive_wait, receive_generation) || sys_timeouts_sleeptime() == 0 ||
        (write_retry_head && (int32_t)(sys_now() - write_retry_head->write_retry_ms) >= 0);
    kernel_socket_protocol_leave(irq);
    return runnable;
}

static int service_ready_socket(void)
{
    if (!ready_work.head) return 0;
    struct kernel_socket *socket = ready_work.head;
    unsigned flags = socket->work_flags; socket->work_flags = 0;
    work_remove(&ready_work, socket);
    if ((flags & SOCKET_WORK_LISTENER) && socket->listening) purge_dead_pending(socket);
    if ((flags & SOCKET_WORK_RECEIVE) && socket->receive_retry && socket->tcp && socket->tcp->refused_data)
        (void)tcp_process_refused_data(socket->tcp);
    retire_timewait(socket);
    if ((flags & SOCKET_WORK_OUTPUT) && socket->tcp && !socket->listening && socket->tcp->unsent) {
        nic_blocked = 0;
        err_t error = tcp_output(socket->tcp);
        if (error == ERR_MEM) {
            if (nic_blocked) { work_remove(&nic_wait, socket); work_add(&nic_wait, socket, nic_generation); }
            else socket_pool_wait(socket);
        }
    }
    return 1;
}
static int service_capacity(void)
{
    struct socket_work_queue *queues[] = {&pool_wait, &nic_wait, &receive_wait};
    uint64_t generations[] = {pool_generation, nic_generation, receive_generation};
    for (unsigned i = 0; i < 3; i++) {
        unsigned index = capacity_cursor;
        capacity_cursor = (capacity_cursor + 1) % 3;
        struct socket_work_queue *queue = queues[index];
        if (!capacity_ready(queue, generations[index])) continue;
        struct kernel_socket *socket = queue->head;
        work_remove(queue, socket);
        if (queue == &pool_wait) { write_retry_disarm(socket); wake_socket(socket); }
        socket_schedule(socket, SOCKET_WORK_REAP |
            (queue == &receive_wait ? SOCKET_WORK_RECEIVE : SOCKET_WORK_OUTPUT));
        return 1;
    }
    return 0;
}
static int service_retry_deadline(void)
{
    if (!write_retry_head || (int32_t)(sys_now() - write_retry_head->write_retry_ms) < 0) return 0;
    struct kernel_socket *socket = write_retry_head;
    write_retry_disarm(socket); work_remove(&pool_wait, socket);
    socket_schedule(socket, SOCKET_WORK_OUTPUT | SOCKET_WORK_RECEIVE | SOCKET_WORK_REAP);
    wake_socket(socket);
    return 1;
}

struct kernel_socket_service_result kernel_socket_service_pending(struct kernel_socket_service_budget budget)
{
    struct kernel_socket_service_result result = {0};
#if BOAROS_COST_DIAGNOSTICS
    if (polling_snapshot) COST_ADD(NETWORK_POLL_SERVICES, 1);
#endif
    if (!socket_initialized || protocol_depth) return result;
    uintptr_t irq = kernel_socket_protocol_enter();
    COST_ADD(NETWORK_SERVICE_CALLS, 1);
    loopback_pending = 0;
    for (struct netif *netif = netif_list; netif; netif = netif->next) {
        if (result.packets < budget.packets)
            result.packets += netif_poll_budget(netif, budget.packets - result.packets);
        if (netif->loop_first) loopback_pending = 1;
    }
    /* 转交容量、到期重试和实际socket工作共用预算；轮转避免持续流量饿死另一类。 */
    while (result.sockets < budget.sockets) {
        int advanced = 0;
        for (unsigned i = 0; i < 3; i++) {
            unsigned which = service_cursor;
            service_cursor = (service_cursor + 1) % 3;
            advanced = which == 0 ? service_ready_socket() :
                which == 1 ? service_capacity() : service_retry_deadline();
            if (advanced) break;
        }
        if (!advanced) break;
        result.sockets++;
    }
    socket_timer_running = 1;
    result.timers = sys_check_timeouts_budget(budget.timers);
    socket_timer_running = 0;
    result.runnable = kernel_socket_work_pending();
    result.next_deadline = socket_next_timer_deadline();
    COST_ADD(NETWORK_SOCKET_VISITS, result.sockets);
    COST_ADD(NETWORK_LOOP_PACKETS, result.packets);
    COST_ADD(NETWORK_TIMER_CALLBACKS, result.timers);
    kernel_socket_protocol_leave(irq);
    return result;
}

void kernel_socket_network_failed(uint32_t address)
{
    uintptr_t saved = kernel_socket_protocol_enter();
    socket_device_failed = 1;
    COST_ADD(NETWORK_GLOBAL_SCANS, 1);
    for (struct kernel_socket *s = inet_sockets; s; s = s->inet_next) {
        const ip_addr_t *local = local_ip(s);
        int affected = local && IP_IS_V4(local) && ip4_addr_get_u32(ip_2_ip4(local)) == address;
        if (!affected && s->udp && s->connected && IP_IS_V4(&s->udp->remote_ip)) {
            /* connected UDP可仍绑定ANY；在撤下carrier前按实际路由归因。 */
            struct netif *route = ip4_route_src(ip_2_ip4(&s->udp->local_ip), ip_2_ip4(&s->udp->remote_ip));
            affected = route && ip4_addr_get_u32(netif_ip4_addr(route)) == address;
        }
        if (!affected) continue;
        /* endpoint仍归OFD；设备故障与协议超时分开，排队数据先按原规则交付。 */
        s->error = s->pending_error = -KERNEL_EIO;
        if (s->tcp && !s->listening) tcp_abort(s->tcp);
        wake_socket(s);
    }
    socket_device_failed = 0;
    kernel_socket_protocol_leave(saved);
}
static void interface_snapshot(struct netif *n, struct kernel_socket_interface *s)
{
    *s = (struct kernel_socket_interface){0};
    int loop = n->name[0] == 'l' && n->name[1] == 'o';
    __builtin_memcpy(s->name, loop ? "lo" : "eth0", loop ? 3 : 5);
    s->index = netif_get_index(n); s->address = netif_ip4_addr(n)->addr;
    /* loopif的0表示不受L2 MTU限制；Linux逻辑MTU容纳完整IPv4长度范围。 */
    s->netmask = netif_ip4_netmask(n)->addr; s->mtu = loop ? 65536U : n->mtu;
    s->hardware_type = loop ? 772 : 1;
    if (netif_is_up(n)) s->flags |= 1;
    if (netif_is_up(n) && netif_is_link_up(n)) s->flags |= 0x40;
    s->flags |= loop ? 8 : 0x1002;
    if (!loop) __builtin_memcpy(s->mac, n->hwaddr, 6);
}
int kernel_socket_interface_index(uint32_t index, struct kernel_socket_interface *s)
{
    uintptr_t irq = kernel_socket_protocol_enter();
    for (struct netif *n = netif_list; n; n = n->next) if (netif_get_index(n) == index) {
        interface_snapshot(n, s); kernel_socket_protocol_leave(irq); return 0;
    }
    kernel_socket_protocol_leave(irq); return -KERNEL_ENODEV;
}
int kernel_socket_interface_get(const char name[16], struct kernel_socket_interface *s)
{
    uintptr_t irq = kernel_socket_protocol_enter();
    for (struct netif *n = netif_list; n; n = n->next) {
        int loop = n->name[0] == 'l' && n->name[1] == 'o';
        if (!__builtin_memcmp(name, loop ? "lo" : "eth0", loop ? 3 : 5)) {
            interface_snapshot(n, s); kernel_socket_protocol_leave(irq); return 0;
        }
    }
    kernel_socket_protocol_leave(irq); return -KERNEL_ENODEV;
}
int kernel_socket_interface_nth(uint32_t ordinal, struct kernel_socket_interface *s)
{
    uintptr_t irq = kernel_socket_protocol_enter(); unsigned previous = 0;
    for (uint32_t i = 0; i <= ordinal; i++) {
        struct netif *selected = 0;
        for (struct netif *n = netif_list; n; n = n->next)
            if (netif_get_index(n) > previous && (!selected || netif_get_index(n) < netif_get_index(selected))) selected = n;
        if (!selected) { kernel_socket_protocol_leave(irq); return -KERNEL_ENODEV; }
        previous = netif_get_index(selected);
        if (i == ordinal) { interface_snapshot(selected, s); kernel_socket_protocol_leave(irq); return 0; }
    }
    __builtin_trap();
}
int kernel_socket_interface_set_flags(const char name[16], uint16_t flags)
{
    uintptr_t irq = kernel_socket_protocol_enter();
    for (struct netif *n = netif_list; n; n = n->next) {
        int loop = n->name[0] == 'l' && n->name[1] == 'o';
        if (!__builtin_memcmp(name, loop ? "lo" : "eth0", loop ? 3 : 5)) {
            if (flags & 1) netif_set_up(n); else netif_set_down(n);
            kernel_socket_protocol_leave(irq); return 0;
        }
    }
    kernel_socket_protocol_leave(irq); return -KERNEL_ENODEV;
}
