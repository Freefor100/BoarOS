#include <kernel/credentials.h>
#include <kernel/socket.h>
#include "../fs/uaccess_iov_internal.h"
#include "socket_state.h"

#include <arch/context.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/fs_context.h>
#include <kernel/vfs.h>
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
    uint32_t length;
    struct kernel_socket_address address;
    uint32_t consumed;
    struct kernel_socket *unix_sender;
    uint32_t send_charge;
    unsigned char data[];
};

struct socket_pending {
    struct socket_pending *next;
    struct kernel_socket *child;
};

struct socket_link { struct kernel_socket *next, *previous; uint64_t epoch; unsigned queued; };
struct kernel_socket_name {
    struct kernel_heap *heap;
    uint32_t references;
    uint16_t length;
    struct kernel_vfs_path *path;
    unsigned char bytes[];
};
struct kernel_socket {
    struct kernel_heap *heap;
    struct kernel_wait_queue wait;
    struct socket_packet *packets_head;
    struct socket_packet *packets_tail;
    struct socket_packet *retired_head, *retired_tail;
    uint32_t receive_credit;
    struct socket_pending *pending_head;
    struct socket_pending *pending_tail;
    struct kernel_socket_read_request *read_request;
    struct kernel_socket_write_request *writes_head, *writes_tail;
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
    uint32_t tx_budget;
    uint32_t unix_packets;
    uint64_t unix_memory;
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
    struct kernel_socket_write_request *active_writes;
    uint32_t unix_references, unix_backlog, unix_queued, unix_reserved;
    struct kernel_socket_name *unix_name, *unix_peer_name;
    struct kernel_vfs_path *unix_path;
    struct kernel_socket *unix_hash_next;
    struct kernel_socket *unix_peers, *unix_peer_next, **unix_peer_previous;
    struct kernel_wait_node unix_space;
    unsigned unix_space_wait;
    struct kernel_socket_peercred peercred;
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
        /* Linux treats an IPv4 destination of INADDR_ANY as loopback. */
        if (!binding && ip4_addr_isany_val(*ip_2_ip4(ip)))
            IP4_ADDR(ip_2_ip4(ip), 127, 0, 0, 1);
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
static uint8_t socket_timer_running;
static uint8_t socket_device_failed;
static uint8_t network_stopping;
static void (*network_timer_wake)(void *);
static struct kernel_wait_queue *network_work_queue;
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

static struct kernel_mutex protocol_mutex;
static unsigned protocol_mutex_initialized;

static int protocol_enter(struct kernel_socket_protocol_scope *scope, int try_only, int local_only)
{
#if BOAROS_USER_SMP
    scope->service=(struct kernel_service_scope){0};
    if (!kernel_service_scope_try_local(&scope->service)) {
        if (local_only) return 0;
        kernel_service_scope_begin(&scope->service);
    }
#else
#if BOAROS_MAX_CPUS > 1
    if (local_only && kernel_cpu_current()->logical_id) return 0;
#else
    (void)local_only;
#endif
#endif
    kernel_assert_service_context();
    struct kernel_io_context *owner = kernel_io_context_current();
    if (protocol_owner == owner) {
        if (!protocol_depth || protocol_depth == UINT32_MAX) __builtin_trap();
        /* 外层mutex随任务保活；抢占者不能改变深度，嵌套借用保留当前IRQ状态。 */
        scope->outer = 0;
        protocol_depth++;
        return 1;
    }
    scope->interrupts=arch_interrupt_save();
    scope->guard=(struct kernel_lock_guard){0};
    if (!protocol_mutex_initialized) {
        kernel_mutex_init(&protocol_mutex, 25, (uintptr_t)&protocol_mutex);
        protocol_mutex_initialized = 1;
    }
    /* 竞争者的 guard 留在自己的调用栈；mutex 可调度等待其他协议 owner。 */
    if (try_only) {
        if (!kernel_mutex_try_lock(&protocol_mutex, &scope->guard)) {
#if BOAROS_USER_SMP
            kernel_service_scope_end(&scope->service);
#endif
            arch_interrupt_restore(scope->interrupts);
            return 0;
        }
    } else kernel_mutex_lock(&protocol_mutex, &scope->guard);
    if (protocol_depth || protocol_owner) __builtin_trap();
    scope->outer = 1;
    protocol_owner = owner;
    if (owner->allocation_depth == UINT32_MAX) __builtin_trap();
    owner->allocation_depth++;
    protocol_depth = 1;
    /* 协议内部允许定时器抢占；任何 raw 临界区都不能越过这个入口。 */
    kernel_assert_can_block();
    arch_interrupt_restore(ARCH_INTERRUPT_ENABLE_MASK);
    return 1;
}
void kernel_socket_protocol_enter(struct kernel_socket_protocol_scope *scope)
{ (void)protocol_enter(scope, 0, 0); }
void kernel_socket_protocol_leave(struct kernel_socket_protocol_scope *scope)
{
    if (!scope->outer) {
        if (protocol_depth <= 1 || protocol_owner != kernel_io_context_current()) __builtin_trap();
        protocol_depth--;
        return;
    }
    (void)arch_interrupt_save();
    if (protocol_depth != 1 || protocol_owner != kernel_io_context_current() ||
        !protocol_owner->allocation_depth) __builtin_trap();
    protocol_depth = 0;
    protocol_owner->allocation_depth--;
    protocol_owner = 0;
    kernel_lock_release(&scope->guard);
#if BOAROS_USER_SMP
    /* 先归还协议owner，再允许恢复用户CPU资格；不能带mutex等待迁移。 */
    kernel_service_scope_end(&scope->service);
#endif
    arch_interrupt_restore(scope->interrupts);
}
static void socket_pbuf_free(struct pbuf *payload)
{
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    pbuf_free(payload);
    kernel_socket_protocol_leave(&irq);
}
static void socket_work_ready_locked(void)
{
    struct socket_state_guard *state=socket_state_owner();
    if(!state->work_prepared && network_work_queue) {
        state->work_prepared=1;
        (void)kernel_wait_cursor_prepare(network_work_queue,&state->work);
    }
}
static int caller_owns_protocol(void)
{
#if BOAROS_MAX_CPUS > 1
    if(kernel_cpu_current()->logical_id)return 0;
#endif
    return protocol_owner==kernel_io_context_current() && protocol_depth;
}

static void work_remove(struct socket_work_queue *queue, struct kernel_socket *socket)
{
    SOCKET_STATE(irq);
    struct socket_link *link = &socket->work_links[queue->slot];
    if (!link->queued) { socket_state_release(&irq); return; }
    if (link->previous) link->previous->work_links[queue->slot].next = link->next;
    else queue->head = link->next;
    if (link->next) link->next->work_links[queue->slot].previous = link->previous;
    else queue->tail = link->previous;
    *link = (struct socket_link){0};
    socket_state_release(&irq);
}
/* 复用调用者已有的端点保护，一次状态修改只取得一次资格。 */
static void work_add_locked(struct socket_work_queue *queue, struct kernel_socket *socket, uint64_t epoch)
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
    SOCKET_STATE(irq);
    if (socket->destroying || socket->domain != KERNEL_SOCKET_DOMAIN_INET) { socket_state_release(&irq); return; }
    socket->work_flags |= flags;
    unsigned queued = socket->work_links[ready_work.slot].queued;
    work_add_locked(&ready_work, socket, 0);
    if (!queued) socket_work_ready_locked();
    socket_state_release(&irq);
}
static void socket_pool_wait(struct kernel_socket *socket)
{
    SOCKET_STATE(state);
    work_remove(&pool_wait, socket);
    work_add_locked(&pool_wait, socket, pool_generation);
}
static void receive_capacity(void)
{
    SOCKET_STATE(irq);
    receive_generation++;
    if (receive_wait.head) socket_work_ready_locked();
    socket_state_release(&irq);
}
static void protocol_work_ready(void)
{
    /* loopback队列只有协议owner生产/消费；已有待处理包时不重复通知。 */
    if(loopback_pending)return;
    loopback_pending=1;
    SOCKET_STATE(state);socket_work_ready_locked();
}
static void protocol_capacity(int pool)
{
    if (pool != -1 && pool != MEMP_TCP_SEG && pool != MEMP_PBUF && pool != MEMP_PBUF_POOL) return;
    /* 协议池及其等待链归CPU0协议owner；只在实际有等待者时触碰
     * 跨核端点状态与worker通知存储，真实归还仍逐次推进代次。 */
    pool_generation++;
    if(pool_wait.head) {SOCKET_STATE(state);socket_work_ready_locked();}
}
static void protocol_input(struct tcp_pcb *pcb)
{
    if (pcb->callback_arg)
        socket_schedule(pcb->callback_arg, SOCKET_WORK_OUTPUT | SOCKET_WORK_REAP | SOCKET_WORK_LISTENER);
}
static void service_inline(const struct kernel_socket_protocol_scope *scope)
{
    /* raw API已返回但outer资格尚未交接；同步回调借用scope时不重入服务。 */
    if (scope->outer)
        (void)kernel_socket_service_owned(scope, (struct kernel_socket_service_budget){8, 8, 1});
}

/* Protocol owner required: the list itself owns no socket reference.
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

static void tcp_publish_capacity_locked(struct kernel_socket *socket);

static void wake_socket(struct kernel_socket *socket)
{
    SOCKET_STATE(state);
    if(caller_owns_protocol() &&
        socket->domain==KERNEL_SOCKET_DOMAIN_INET && socket->type==SOCKET_STREAM)
        tcp_publish_capacity_locked(socket);
    socket_state_notify(&socket->wait);
}
/* UNIX names are immutable; input views borrow the syscall snapshot, while
 * queued sources and exported addresses retain only this one stored name. */
static struct kernel_socket *unix_names[64];
static uint32_t unix_autoname;
static void unix_name_get_locked(struct kernel_socket_name *name)
{ if(name) { if(!name->references || name->references==UINT32_MAX)__builtin_trap();name->references++; } }
static void unix_name_put(struct kernel_socket_name *name)
{
    if(!name)return;
    SOCKET_STATE(state);
    if(!name->references)__builtin_trap();
    int release=!--name->references;
    socket_state_release(&state);
    if(release) {if(name->path && kernel_vfs_path_release(&name->path))__builtin_trap();kernel_heap_release(name->heap,name);}
}
void kernel_socket_address_release(struct kernel_socket_address *address)
{
    if(address->family==KERNEL_SOCKET_AF_UNIX)unix_name_put(address->unix_owner);
    *address=(struct kernel_socket_address){0};
}
int kernel_socket_is_unix(const struct kernel_socket *socket)
{ return socket && socket->domain==KERNEL_SOCKET_DOMAIN_UNIX; }
static void unix_export_locked(struct kernel_socket_name *name,struct kernel_socket_address *address)
{
    *address=(struct kernel_socket_address){.family=KERNEL_SOCKET_AF_UNIX};
    if(name) {
        unix_name_get_locked(name);address->unix_path=name->bytes;
        address->unix_length=name->length;address->unix_owner=name;
    }
}
static void unix_get_locked(struct kernel_socket *socket)
{
    if(!socket->unix_references || socket->unix_references==UINT32_MAX)__builtin_trap();
    socket->unix_references++;
}
static int unix_put_locked(struct kernel_socket *socket)
{
    if(!socket->unix_references)__builtin_trap();
    int release=!--socket->unix_references;
    if(release && (socket->destroying!=2 || socket->unix_packets || socket->unix_memory || socket->unix_path || socket->unix_name))__builtin_trap();
    return release;
}
static void unix_put(struct kernel_socket *socket)
{
    if(!socket)return;
    SOCKET_STATE(state);
    int release=unix_put_locked(socket);
    socket_state_release(&state);
    if(release)kernel_heap_release(socket->heap,socket);
}
static struct kernel_socket_peercred unix_current_cred(void)
{
    const struct kernel_credentials *cred=kernel_credentials_current();
    kernel_pid_t pid=0;struct kernel_task *task=kernel_task_current();
    if(task && kernel_task_tgid(task,&pid)!=KERNEL_TASK_STATUS_OK)__builtin_trap();
    return (struct kernel_socket_peercred){.pid=(int32_t)pid,.uid=cred->euid,.gid=cred->egid};
}
int kernel_socket_peercred(struct kernel_socket *socket,struct kernel_socket_peercred *cred)
{ SOCKET_STATE(state);*cred=socket->peercred;return 0; }
static unsigned unix_name_hash(const unsigned char *name,unsigned length,unsigned type)
{
    uint32_t hash=type;
    for(unsigned i=0;i<length;i++)hash=(hash^name[i])*16777619U;
    return hash&63U;
}
static unsigned unix_path_hash(struct kernel_vfs_path *path)
{
    uintptr_t mount=(uintptr_t)kernel_vfs_path_mount(path);
    return (unsigned)((mount>>4)^kernel_vfs_path_inode(path))&63U;
}
static unsigned unix_socket_hash(struct kernel_socket *socket)
{
    return socket->unix_path?unix_path_hash(socket->unix_path):
        unix_name_hash(socket->unix_name->bytes,socket->unix_name->length,socket->type);
}
static int unix_same_path(struct kernel_vfs_path *a,struct kernel_vfs_path *b)
{ return a && b && kernel_vfs_path_mount(a)==kernel_vfs_path_mount(b) && kernel_vfs_path_inode(a)==kernel_vfs_path_inode(b); }
static struct kernel_socket *unix_find_locked(const struct kernel_socket_address *address,
    struct kernel_vfs_path *path,unsigned type)
{
    unsigned slot=path?unix_path_hash(path):unix_name_hash(address->unix_path,address->unix_length,type);
    for(struct kernel_socket *socket=unix_names[slot];socket;socket=socket->unix_hash_next) {
        if(socket->destroying)continue;
        if(path?unix_same_path(socket->unix_path,path):
            !socket->unix_path && socket->type==type && socket->unix_name->length==address->unix_length &&
            !__builtin_memcmp(socket->unix_name->bytes,address->unix_path,address->unix_length))return socket;
    }
    return NULL;
}
static int unix_path_resolve(const unsigned char *name,int create,struct kernel_vfs_path **owner)
{
    const struct kernel_fs_context *fs;
    struct kernel_task *task=kernel_task_current();
    if(!task || kernel_task_fs_context_borrow(task,&fs)!=KERNEL_TASK_STATUS_OK)return -KERNEL_ENOENT;
    KERNEL_FS_PATHS(paths,fs);
    return create?kernel_vfs_socket_bind_at(paths.cwd,paths.root,(const char *)name,0777U & ~paths.umask,owner):
        kernel_vfs_socket_lookup_at(paths.cwd,paths.root,(const char *)name,owner);
}
static int unix_address_validate(const struct kernel_socket_address *address,int binding)
{
    if(address->family!=KERNEL_SOCKET_AF_UNIX || address->unix_length>108 ||
        (!binding && !address->unix_length))return -KERNEL_EINVAL;
    return 0;
}
static int unix_lookup(struct kernel_socket *sender,const struct kernel_socket_address *address,struct kernel_socket **owner)
{
    int result=unix_address_validate(address,0);if(result)return result;
    struct kernel_vfs_path *path=NULL;
    unsigned char pathname[109];
    if(address->unix_path[0]) {
        __builtin_memcpy(pathname,address->unix_path,address->unix_length);pathname[address->unix_length]=0;
        result=unix_path_resolve(pathname,0,&path);if(result)return result;
    }
    SOCKET_STATE(state);
    struct kernel_socket *target=unix_find_locked(address,path,sender->type);
    result=!target?-KERNEL_ECONNREFUSED:target->type!=sender->type?-KERNEL_EPROTOTYPE:0;
    if(!result) {unix_get_locked(target);*owner=target;}
    socket_state_release(&state);
    if(path && kernel_vfs_path_release(&path))__builtin_trap();
    return result;
}
static int unix_bind(struct kernel_socket *socket,const struct kernel_socket_address *address)
{
    int result=unix_address_validate(address,1);if(result)return result;
    unsigned char auto_bytes[6]={0};
    struct kernel_socket_address normalized=*address;
    if(!address->unix_length) {
        static const char hex[]="0123456789abcdef";
        normalized.unix_path=auto_bytes;normalized.unix_length=6;
        unsigned attempts=0;
        for(;;) {
            SOCKET_STATE(state);uint32_t value=++unix_autoname;
            for(unsigned i=1;i<6;i++)auto_bytes[i]=(unsigned char)hex[(value>>((5-i)*4))&15];
            if(!unix_find_locked(&normalized,NULL,socket->type))break;
            if(++attempts==0x100000U)return -KERNEL_ENOSPC;
        }
    }
    size_t length=normalized.unix_length;
    if(normalized.unix_path[0]) {
        length=0;while(length<normalized.unix_length && normalized.unix_path[length])length++;
        length++;
    }
    struct kernel_socket_name *name=NULL;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(socket->heap,1,sizeof(*name)+length,(void **)&name);
    if(status!=KERNEL_HEAP_STATUS_OK)return status==KERNEL_HEAP_STATUS_EMPTY?-KERNEL_ENOMEM:-KERNEL_EIO;
    name->heap=socket->heap;name->references=1;name->length=(uint16_t)length;
    __builtin_memcpy(name->bytes,normalized.unix_path,length>normalized.unix_length?normalized.unix_length:length);
    struct kernel_vfs_path *path=NULL;
    { SOCKET_STATE(state);result=socket->unix_name?-KERNEL_EINVAL:0; }
    if(!result && name->bytes[0])result=unix_path_resolve(name->bytes,1,&path);
    if(!result) {
        normalized.unix_path=name->bytes;normalized.unix_length=name->length;
        SOCKET_STATE(state);
        result=socket->unix_name?-KERNEL_EINVAL:unix_find_locked(&normalized,path,socket->type)?-KERNEL_EADDRINUSE:0;
        if(!result) {
            name->path=path;socket->unix_name=name;socket->unix_path=path;socket->explicit_bind=1;name=NULL;path=NULL;
            unsigned slot=unix_socket_hash(socket);socket->unix_hash_next=unix_names[slot];unix_names[slot]=socket;
        }
    }
    if(path && kernel_vfs_path_release(&path))__builtin_trap();
    unix_name_put(name);
    return result==-KERNEL_EEXIST?-KERNEL_EADDRINUSE:result;
}
/* Peer links borrow no OFD: last user close still shuts the endpoint down.
 * Each connection owns peer storage, and its intrusive incoming edge lets
 * close disconnect exactly the users of that endpoint, including one-way DGRAM. */
static struct kernel_socket *unix_take_peer_locked(struct kernel_socket *socket)
{
    struct kernel_socket *old=socket->peer;
    if(!old)return NULL;
    if(!socket->unix_peer_previous || *socket->unix_peer_previous!=socket)__builtin_trap();
    *socket->unix_peer_previous=socket->unix_peer_next;
    if(socket->unix_peer_next)socket->unix_peer_next->unix_peer_previous=socket->unix_peer_previous;
    socket->unix_peer_previous=NULL;socket->unix_peer_next=NULL;socket->peer=NULL;socket->unix_space_wait=0;
    return old;
}
static void unix_link_peer_locked(struct kernel_socket *socket,struct kernel_socket *peer)
{
    if(socket->peer)__builtin_trap();
    unix_get_locked(peer);socket->peer=peer;socket->unix_peer_next=peer->unix_peers;
    socket->unix_peer_previous=&peer->unix_peers;
    if(socket->unix_peer_next)socket->unix_peer_next->unix_peer_previous=&socket->unix_peer_next;
    peer->unix_peers=socket;
}
static int unix_queue_full(const struct kernel_socket *sender,const struct kernel_socket *target)
{ return sender->type==SOCKET_DGRAM && target!=sender && target->peer!=sender && target->unix_queued+target->unix_reserved>10; }
static void unix_space_notify(struct kernel_wait_node *node,uint32_t reason)
{
    (void)reason;struct kernel_socket *sender=node->context;
    SOCKET_STATE(state);
    if(sender->unix_space_wait) {sender->unix_space_wait=0;wake_socket(sender);}
}
static void unix_watch_peer_locked(struct kernel_socket *socket)
{
    /* Reciprocal/self connections are exempt from the destination queue limit.
     * Their sender-budget changes already notify the owning endpoint. */
    if(socket->type==SOCKET_DGRAM && socket->peer && socket->peer!=socket &&
       socket->peer->peer!=socket) {
        kernel_wait_node_init_callback(&socket->unix_space,unix_space_notify,socket);
        kernel_wait_queue_add(&socket->peer->wait,&socket->unix_space);
    }
}
static void unix_unwatch_peer(struct kernel_socket *socket)
{ if(kernel_wait_node_remove_sync(&socket->unix_space)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap(); }
static int unix_wait_queue(struct kernel_socket *target,uint64_t deadline,uint32_t flags)
{
    if(flags&KERNEL_SOCKET_MSG_DONTWAIT)return -KERNEL_EAGAIN;
    struct kernel_wait_token token={0};enum kernel_wait_wake_reason reason=KERNEL_WAIT_WOKEN;
    if(kernel_wait_prepare(&target->wait,deadline,1,&token)!=KERNEL_SCHEDULER_STATUS_OK)return -KERNEL_EIO;
    /* Caller has the endpoint state lock through prepare; notifications can
     * precede park, but cannot pass the condition/registration boundary. */
    struct socket_state_guard *state=socket_state_owner();
    uintptr_t interrupts=socket_state_unlock_wait(state);
    int result=kernel_wait_park(&token,&reason)!=KERNEL_SCHEDULER_STATUS_OK?-KERNEL_EIO:0;
    if(kernel_wait_finish(&token)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
    arch_interrupt_restore(interrupts);
    if(result)return result;
    if(reason==KERNEL_WAIT_TIMEOUT)return -KERNEL_EAGAIN;
    if(reason==KERNEL_WAIT_SIGNALLED) {
        if(deadline)return -KERNEL_EINTR;
        kernel_signal_note_syscall_restart(kernel_task_current());return -KERNEL_ERESTARTSYS;
    }
    return 0;
}
static uint64_t unix_deadline(struct kernel_socket *socket)
{
    uint64_t timeout=kernel_socket_send_timeout(socket),deadline=0;
    if(timeout) {
        uint64_t now=kernel_time_monotonic_ns();
        if(kernel_time_deadline_from_monotonic(UINT64_MAX-now<timeout?UINT64_MAX:now+timeout,&deadline))return UINT64_MAX;
    }
    return deadline;
}
/* A nonempty sender packet set holds one storage reference until its last packet leaves. */
static void unix_uncharge(struct socket_packet *packet)
{
    struct kernel_socket *sender=packet->unix_sender;
    kernel_socket_address_release(&packet->address);
    if(!sender)return;
    SOCKET_STATE(irq);
    if(!sender->unix_packets || sender->unix_memory<packet->send_charge)__builtin_trap();
    sender->unix_memory-=packet->send_charge;sender->unix_packets--;
    packet->unix_sender=0;
    int release=!sender->unix_packets && unix_put_locked(sender);
    if(!sender->destroying)wake_socket(sender);
    socket_state_release(&irq);
    if(release)kernel_heap_release(sender->heap,sender);
}
static uint32_t unix_charge(uint32_t size)
{ return (uint32_t)sizeof(struct socket_packet)+size; }
static void unix_publish_owner(struct kernel_socket *sender,struct socket_packet *packet,uint32_t charge)
{
    if(packet->unix_sender || sender->unix_packets==UINT32_MAX || sender->unix_memory>UINT64_MAX-charge)__builtin_trap();
    packet->unix_sender=sender;packet->send_charge=charge;
    /* Per-packet bytes stay charged individually; the shared reference covers their owner set. */
    if(!sender->unix_packets)unix_get_locked(sender);
    sender->unix_packets++;sender->unix_memory+=charge;
}
static int unix_writable(const struct kernel_socket *sender)
{ return sender->unix_memory+sender->tx_reserved<=sender->tx_limit/4U; }


static void retire_timewait(struct kernel_socket *socket)
{
    struct tcp_pcb *pcb = socket->tcp;
    if (!pcb || socket->listening || pcb->state != TIME_WAIT) return;
    /* 释放 hook 也走这里：只清借用与回调，不重入 raw API 或销毁堆 owner。 */
    pcb->callback_arg = 0; pcb->errf = 0;
    pcb->recv = 0; pcb->sent = 0; pcb->poll = 0; pcb->pollinterval = 0;
    pcb->connected = 0;
    SOCKET_STATE(state);
    socket->tcp = 0; socket->peer_closed = 1; socket->receive_retry = 0;
    work_remove(&pool_wait, socket); work_remove(&nic_wait, socket); work_remove(&receive_wait, socket);
    write_retry_disarm(socket); wake_socket(socket);
}

void kernel_socket_expire_timers(void)
{
    SOCKET_STATE(irq);
    if (network_timer_wake) network_timer_wake(network_context);
    socket_state_release(&irq);
}

static uint64_t socket_next_timer_deadline(void)
{
    uint64_t deadline = 0;
    uint64_t now;
    uint64_t delay;
    u32_t milliseconds;
    struct kernel_socket_protocol_scope old_status; kernel_socket_protocol_enter(&old_status);
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
    kernel_socket_protocol_leave(&old_status);
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
            kernel_heap_release(listener->heap, entry);
        } else {
            previous = entry;
        }
        entry = next;
    }
}

/* 端点锁只发布队列和额度；载荷分配、复制及回收均在保护外完成。 */
static void packet_publish(struct kernel_socket *socket, struct socket_packet *packet, uint32_t charge)
{
    SOCKET_STATE(irq);
    if (socket->rx_bytes > UINT32_MAX - charge) __builtin_trap();
    socket->rx_bytes += charge;
    if (socket->packets_tail) socket->packets_tail->next = packet;
    else socket->packets_head = packet;
    socket->packets_tail = packet;
    wake_socket(socket);
    socket_state_release(&irq);
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
    int reject=(network_udp_capacity && !network_udp_capacity(network_context)) ||
        lwip_stats.mem.used > MEM_SIZE - (65536U + 4096U);
    { SOCKET_STATE(state);reject |= charge>socket->rx_limit || socket->rx_bytes>socket->rx_limit-charge; }
    if (reject) {
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
    address_export(socket, peer, port, &packet->address);
    packet_publish(socket, packet, charge);
}

static err_t tcp_connected(void *context, struct tcp_pcb *pcb, err_t error)
{
    struct kernel_socket *socket = context;
    (void)pcb;
    socket_schedule(socket, SOCKET_WORK_OUTPUT | SOCKET_WORK_REAP);
    SOCKET_STATE(irq);
    socket->connecting = 0U;
    socket->connected = error == ERR_OK;
    if (error == ERR_OK) {
        address_export(socket, &pcb->local_ip, pcb->local_port, &socket->saved_local);
        address_export(socket, &pcb->remote_ip, pcb->remote_port, &socket->saved_peer);
    }
    socket->error = lwip_error(error);
    wake_socket(socket);
    socket_state_release(&irq);
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
        SOCKET_STATE(state);socket->peer_closed = 1U;
        wake_socket(socket);
        return ERR_OK;
    }
    if (error != ERR_OK) return error;
    { SOCKET_STATE(state);
      if(payload->tot_len>socket->rx_limit || socket->rx_bytes>socket->rx_limit-payload->tot_len)
          {socket->receive_retry=1;return ERR_MEM;}
    }
    status = kernel_heap_allocate_zeroed(socket->heap, 1U, sizeof(*packet),
                                         (void **)&packet);
    if (status == KERNEL_HEAP_STATUS_EMPTY) { SOCKET_STATE(state);socket->receive_retry = 1; work_remove(&receive_wait, socket); work_add_locked(&receive_wait, socket, receive_generation); return ERR_MEM; }
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    packet->payload = payload;
    socket->receive_retry=0;
    work_remove(&receive_wait, socket);
    packet_publish(socket, packet, payload->tot_len);
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
        SOCKET_STATE(state);socket->pending_ready = 0;
        if (!socket->listener->pending_live) __builtin_trap();
        socket->listener->pending_live--;
        socket_schedule(socket->listener, SOCKET_WORK_LISTENER);
    }
    SOCKET_STATE(irq);
    if (error == ERR_CLSD) {
        socket->tcp = 0; socket->peer_closed = 1; wake_socket(socket); socket_state_release(&irq); return;
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
    socket_state_release(&irq);
}

static void protocol_timewait_free(struct tcp_pcb *pcb)
{
    /* raw 的 opaque 参数不一定是 socket；本层回调配对会在 destroy 前清除。 */
    if (!pcb->callback_arg || pcb->errf != tcp_failed) return;
    struct kernel_socket *socket = pcb->callback_arg;
    if (pcb->state != TIME_WAIT || socket->tcp != pcb || socket->destroying || socket->listening)
        __builtin_trap();
    retire_timewait(socket);
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
        kernel_heap_release(listener->heap, child);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    child->listener = listener; child->pending_ready = 1;
    child->heap = listener->heap;
    child->peercred=(struct kernel_socket_peercred){.uid=UINT32_MAX,.gid=UINT32_MAX};
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
    {SOCKET_STATE(state);tcp_publish_capacity_locked(child);}
    child->inet_next = inet_sockets; inet_sockets = child;
    tcp_backlog_delayed(pcb);
    entry->child = child;
    SOCKET_STATE(state);listener->pending_live++;
    if (listener->pending_tail != 0) {
        listener->pending_tail->next = entry;
    } else {
        listener->pending_head = entry;
    }
    listener->pending_tail = entry;
    wake_socket(listener);
    return ERR_OK;
}

static struct kernel_mutex unix_control;
static unsigned unix_control_initialized;
static void unix_control_acquire(struct kernel_lock_guard *guard)
{
    uintptr_t irq=arch_interrupt_save();
    if(!unix_control_initialized) {kernel_mutex_init(&unix_control,10,(uintptr_t)&unix_control);unix_control_initialized=1;}
    arch_interrupt_restore(irq);
    kernel_mutex_lock(&unix_control,guard);
}
static void unix_initialize(struct kernel_socket *socket,struct kernel_heap *heap,int type)
{
    socket->heap=heap;socket->type=(uint8_t)type;socket->family=KERNEL_SOCKET_AF_UNIX;
    socket->domain=KERNEL_SOCKET_DOMAIN_UNIX;socket->unix_references=1;
    socket->rx_limit=socket->tx_limit=212992;socket->peercred=(struct kernel_socket_peercred){.uid=UINT32_MAX,.gid=UINT32_MAX};
    kernel_wait_queue_init(&socket->wait);
}

int kernel_socket_create(struct kernel_heap *heap, int family, int type,
                         struct kernel_socket **owner)
{
    struct kernel_socket *socket = 0;
    struct kernel_socket_protocol_scope old_status;

    if (heap == 0 || owner == 0 || *owner != 0 ||
        (type != SOCKET_DGRAM && type != SOCKET_STREAM) ||
        (family != KERNEL_SOCKET_AF_UNIX && family != KERNEL_SOCKET_AF_INET && family != KERNEL_SOCKET_AF_INET6)) {
        return -KERNEL_EINVAL;
    }
    enum kernel_heap_status status =
        kernel_heap_allocate_zeroed(heap, 1U, sizeof(*socket),
                                    (void **)&socket);
    if (status == KERNEL_HEAP_STATUS_EMPTY) {
        return -KERNEL_ENOMEM;
    }
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    if(family==KERNEL_SOCKET_AF_UNIX) {unix_initialize(socket,heap,type);*owner=socket;return 0;}
    socket->peercred=(struct kernel_socket_peercred){.uid=UINT32_MAX,.gid=UINT32_MAX};
    kernel_socket_protocol_enter(&old_status);
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
    kernel_socket_protocol_leave(&old_status);
    if ((type == SOCKET_DGRAM && socket->udp == 0) ||
        (type == SOCKET_STREAM && socket->tcp == 0)) {
        kernel_heap_release(heap, socket);
        return -KERNEL_ENOMEM;
    }
    kernel_socket_protocol_enter(&old_status);
    socket->inet_next = inet_sockets; inet_sockets = socket;
    kernel_socket_protocol_leave(&old_status);
    *owner = socket;
    return 0;
}

int kernel_socket_pair(struct kernel_heap *heap,int type,
    struct kernel_socket **owner_a,struct kernel_socket **owner_b)
{
    if(!heap || !owner_a || !owner_b || *owner_a || *owner_b)return -KERNEL_EINVAL;
    struct kernel_socket *a=NULL,*b=NULL;
    int result=kernel_socket_create(heap,KERNEL_SOCKET_AF_UNIX,type,&a);
    if(result)return result;
    result=kernel_socket_create(heap,KERNEL_SOCKET_AF_UNIX,type,&b);
    if(result) {kernel_socket_destroy(a);return result;}
    struct kernel_socket_peercred cred=unix_current_cred();
    SOCKET_STATE(state);
    a->connected=b->connected=1;a->peercred=b->peercred=cred;
    unix_link_peer_locked(a,b);unix_link_peer_locked(b,a);
    unix_watch_peer_locked(a);unix_watch_peer_locked(b);
    *owner_a=a;*owner_b=b;return 0;
}

static void retire_received(struct kernel_socket *socket)
{
    SOCKET_STATE(irq);
    struct socket_packet *packets = socket->retired_head;
    uint32_t credit = socket->receive_credit;
    socket->retired_head = socket->retired_tail = 0;
    socket->receive_credit = 0;
    socket_state_release(&irq);
    int freed = packets != 0;
    while (packets) {
        struct socket_packet *next = packets->next;
        if (packets->payload) pbuf_free(packets->payload);
        kernel_heap_release(socket->heap, packets);
        packets = next;
    }
    if (freed) receive_capacity();
    if (socket->tcp && !socket->listening) while (credit) {
        u16_t part = credit > UINT16_MAX ? UINT16_MAX : (u16_t)credit;
        tcp_recved(socket->tcp, part); credit -= part;
    }
}

static void unix_destroy(struct kernel_socket *socket)
{
    KERNEL_LOCK_SCOPE(control);unix_control_acquire(&control);
    SOCKET_STATE(state);
    if(socket->read_request || socket->tx_reserved || socket->active_writes || socket->destroying)__builtin_trap();
    socket->destroying=1;
    struct kernel_socket_name *name=socket->unix_name,*peer_name=socket->unix_peer_name;
    socket->unix_peer_name=NULL;
    if(name && socket->explicit_bind) {
        struct kernel_socket **link=&unix_names[unix_socket_hash(socket)];
        while(*link!=socket) {if(!*link)__builtin_trap();link=&(*link)->unix_hash_next;}
        *link=socket->unix_hash_next;socket->unix_hash_next=NULL;
    }
    socket_state_release(&state);
    unix_unwatch_peer(socket);
    SOCKET_STATE(own);
    struct kernel_socket *peer=unix_take_peer_locked(socket);
    if(peer && socket->type==SOCKET_STREAM && socket->packets_head)peer->pending_error=-KERNEL_ECONNRESET;
    if(peer && !peer->destroying)wake_socket(peer);
    socket_state_release(&own);unix_put(peer);
    for(;;) {
        SOCKET_STATE(incoming);
        struct kernel_socket *sender=socket->unix_peers;
        if(!sender)break;
        unix_get_locked(sender);socket_state_release(&incoming);
        unix_unwatch_peer(sender);
        SOCKET_STATE(detach);
        struct kernel_socket *target=unix_take_peer_locked(sender);
        if(target!=socket)__builtin_trap();
        sender->peer_closed=1;
        if(socket->type==SOCKET_STREAM) {
            if(sender->unix_peer_name)__builtin_trap();
            sender->unix_peer_name=name;unix_name_get_locked(name);
            if(socket->packets_head)sender->pending_error=-KERNEL_ECONNRESET;
        }
        if(!sender->destroying)wake_socket(sender);
        socket_state_release(&detach);unix_put(target);unix_put(sender);
    }
    { SOCKET_STATE(names);socket->unix_name=NULL;socket->unix_path=NULL; }
    kernel_lock_release(&control);
    unix_name_put(name);unix_name_put(peer_name);
    while(socket->packets_head) {
        struct socket_packet *packet=socket->packets_head;socket->packets_head=packet->next;
        unix_uncharge(packet);kernel_heap_release(socket->heap,packet);
    }
    while(socket->pending_head) {
        struct socket_pending *entry=socket->pending_head;socket->pending_head=entry->next;
        entry->child->listener=NULL;entry->child->pending_ready=0;
        kernel_socket_destroy(entry->child);kernel_heap_release(socket->heap,entry);
    }
    uintptr_t irq=arch_interrupt_save();
    if(kernel_wait_queue_close(&socket->wait)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
    for(;;) {
        enum kernel_scheduler_status status=kernel_wait_queue_destroy(&socket->wait);
        if(status==KERNEL_SCHEDULER_STATUS_OK)break;
        if(status!=KERNEL_SCHEDULER_STATUS_BUSY)__builtin_trap();
        kernel_wait_backend_quiesce();
    }
    arch_interrupt_restore(irq);
    SOCKET_STATE(done);socket->destroying=2;socket_state_release(&done);
    unix_put(socket);
}

void kernel_socket_destroy(struct kernel_socket *socket)
{
    struct kernel_socket_protocol_scope old_status;
    if (socket == 0) __builtin_trap();
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {unix_destroy(socket);return;}
    kernel_socket_protocol_enter(&old_status);
    SOCKET_STATE(state);
    if (socket->read_request != 0 || socket->tx_reserved || socket->writes_head || socket->active_writes) __builtin_trap();
    socket->destroying = 1;
    if (socket->listener && socket->pending_ready) {
        if (!socket->listener->pending_live) __builtin_trap();
        socket->listener->pending_live--;
    }
    socket->listener = 0; socket->pending_ready = 0;
    socket_state_release(&state);
    work_remove(&ready_work, socket); work_remove(&pool_wait, socket); work_remove(&nic_wait, socket); work_remove(&receive_wait, socket);
    write_retry_disarm(socket);
    retire_received(socket);
    if (socket->domain == KERNEL_SOCKET_DOMAIN_INET) {
        COST_ADD(NETWORK_GLOBAL_SCANS, 1);
        struct kernel_socket **link = &inet_sockets;
        while (*link != socket) { if (*link == 0) __builtin_trap(); link = &(*link)->inet_next; }
        *link = socket->inet_next; socket->inet_next = 0;
    }
    while (socket->packets_head != 0) {
        struct socket_packet *packet = socket->packets_head;
        socket->packets_head = packet->next;
        if (packet->payload != 0) {
            socket_pbuf_free(packet->payload);
        }
        unix_uncharge(packet);
        kernel_heap_release(socket->heap, packet);
        receive_capacity();
    }
    while (socket->pending_head != 0) {
        struct socket_pending *entry = socket->pending_head;
        socket->pending_head = entry->next;
        kernel_socket_destroy(entry->child);
        kernel_heap_release(socket->heap, entry);
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
    uintptr_t wait_irq=arch_interrupt_save();
    if(kernel_wait_queue_close(&socket->wait)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
    for(;;) {
        enum kernel_scheduler_status status=kernel_wait_queue_destroy(&socket->wait);
        if(status==KERNEL_SCHEDULER_STATUS_OK)break;
        if(status!=KERNEL_SCHEDULER_STATUS_BUSY)__builtin_trap();
        kernel_wait_backend_quiesce();
    }
    arch_interrupt_restore(wait_irq);
    struct kernel_heap *heap = socket->heap;
    kernel_socket_protocol_leave(&old_status);
    kernel_heap_release(heap, socket);
    receive_capacity();
}

int kernel_socket_bind(struct kernel_socket *socket,
                       const struct kernel_socket_address *address)
{
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        KERNEL_LOCK_SCOPE(control);unix_control_acquire(&control);return unix_bind(socket,address);
    }
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    ip_addr_t local;
    if (socket->domain != KERNEL_SOCKET_DOMAIN_INET) { kernel_socket_protocol_leave(&scope); return -KERNEL_EINVAL; }
    int result = address_import(socket, address, &local, 1);
    if (result != 0) { kernel_socket_protocol_leave(&scope); return result; }
    int low_port_denied=address->port && address->port<1024 &&
        !kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_NET_BIND_SERVICE);
    if(socket->family==KERNEL_SOCKET_AF_INET6 && low_port_denied) {
        kernel_socket_protocol_leave(&scope);return -KERNEL_EACCES;
    }
    if (!ip_addr_isany(&local) &&
        !(IP_IS_V4(&local) ? ip4_addr_isloopback(ip_2_ip4(&local)) : ip6_addr_isloopback(ip_2_ip6(&local)))) {
        int found = 0;
        for (struct netif *n = netif_list; n; n = n->next)
            if (IP_IS_V4(&local) && ip4_addr_eq(ip_2_ip4(&local), netif_ip4_addr(n))) found = 1;
        if (!found) { kernel_socket_protocol_leave(&scope); return -KERNEL_EADDRNOTAVAIL; }
    }

    if(low_port_denied) {
        kernel_socket_protocol_leave(&scope);return -KERNEL_EACCES;
    }
    if ((socket->udp != 0 && socket->udp->local_port != 0) ||
        (socket->tcp != 0 && socket->tcp->local_port != 0)) { kernel_socket_protocol_leave(&scope); return -KERNEL_EINVAL; }
    result = port_conflict(socket, &local, address->port, 0);
    if (result != 0) { kernel_socket_protocol_leave(&scope); return result; }
    err_t error = socket->type == SOCKET_DGRAM
        ? udp_bind(socket->udp, &local, address->port)
        : tcp_bind(socket->tcp, &local, address->port);
    if (error == ERR_OK) {
        socket->explicit_bind = 1;
        socket->explicit_port = address->port != 0;
        if (socket->udp != 0) ip_addr_copy(socket->udp_bind_ip, socket->udp->local_ip);
        if (IP_IS_V6(&local) && !ip_addr_isany(&local)) socket->v6only = 1;
    }
    kernel_socket_protocol_leave(&scope);
    return lwip_error(error);
}

int kernel_socket_getname(struct kernel_socket *socket,
                          struct kernel_socket_address *address)
{
    if(socket && socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        SOCKET_STATE(state);unix_export_locked(socket->unix_name,address);return 0;
    }
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    if (socket == 0 || address == 0) { kernel_socket_protocol_leave(&scope); return -KERNEL_EINVAL; }
    if (socket->tcp == 0 && socket->connected && socket->type == SOCKET_STREAM) {
        *address = socket->saved_local;
        kernel_socket_protocol_leave(&scope); return 0;
    }
    if (socket->type == SOCKET_DGRAM) {
        address_export(socket, &socket->udp->local_ip, socket->udp->local_port, address);
    } else {
        if (socket->tcp == 0) { int result = socket->error != 0 ? socket->error : -KERNEL_ENOTCONN; kernel_socket_protocol_leave(&scope); return result; }
        if (socket->listening) {
            struct tcp_pcb_listen *pcb = (struct tcp_pcb_listen *)socket->tcp;
            address_export(socket, &pcb->local_ip, pcb->local_port, address);
        } else {
            address_export(socket, &socket->tcp->local_ip, socket->tcp->local_port, address);
        }
    }
    kernel_socket_protocol_leave(&scope);
    return 0;
}

int kernel_socket_getpeer(struct kernel_socket *socket,struct kernel_socket_address *address)
{
    if(!socket || !address)return -KERNEL_EINVAL;
    SOCKET_STATE(state);
    if(!socket->connected)return -KERNEL_ENOTCONN;
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        if(!socket->peer && socket->type!=SOCKET_STREAM)return -KERNEL_ENOTCONN;
        unix_export_locked(socket->peer?socket->peer->unix_name:socket->unix_peer_name,address);
    } else *address=socket->saved_peer;
    return 0;
}

static int unix_connect(struct kernel_socket *socket,const struct kernel_socket_address *address,int nonblocking)
{
    if(socket->type==SOCKET_DGRAM) {
        if(address->family && unix_address_validate(address,0))return -KERNEL_EINVAL;
        KERNEL_LOCK_SCOPE(control);unix_control_acquire(&control);
        struct kernel_socket *target=NULL;
        int result=address->family?unix_lookup(socket,address,&target):0;
        if(result)return result;
        { SOCKET_STATE(check);
          result=target && target->destroying?-KERNEL_ECONNREFUSED:
                 target && target->peer && target->peer!=socket?-KERNEL_EPERM:0;
        }
        if(result) {unix_put(target);return result;}
        unix_unwatch_peer(socket);
        /* Becoming reciprocal removes the target's former queue-space
         * subscription. Control owns topology; finish old callback borrows
         * outside the endpoint lock before publishing the new relation. */
        if(target && target!=socket && target->peer==socket)unix_unwatch_peer(target);
        SOCKET_STATE(state);
        struct kernel_socket *old=unix_take_peer_locked(socket);
        if(target) {unix_link_peer_locked(socket,target);unix_watch_peer_locked(socket);target->connected=1;}
        int notify_old=old && old!=target && old!=socket && old->peer==socket;
        int notify_target=target && target!=socket && target->peer==socket;
        if(notify_old)unix_watch_peer_locked(old);
        if(notify_target)target->unix_space_wait=0;
        struct socket_packet *discard=NULL;
        if(old && old!=target) {
            if(socket->read_request) {
                struct socket_packet *reserved=socket->packets_head;
                discard=reserved->next;reserved->next=NULL;socket->packets_tail=reserved;
                socket->rx_bytes=reserved->length?reserved->length:1;socket->unix_queued=1;
            } else {
                discard=socket->packets_head;socket->packets_head=socket->packets_tail=NULL;
                socket->rx_bytes=socket->unix_queued=0;
            }
            if(discard && !old->destroying && old->peer==socket)old->pending_error=-KERNEL_ECONNRESET;
        }
        socket->connected=target!=NULL;socket->peer_closed=0;socket->pending_error=0;
        wake_socket(socket);
        socket_state_release(&state);
        /* Each notification guard holds at most two queues. The peer refs
         * above survive this separate delivery of changed queue eligibility. */
        { SOCKET_STATE(changed);
          if(notify_old && !old->destroying)wake_socket(old);
          if(notify_target && !target->destroying)wake_socket(target);
        }
        while(discard) {struct socket_packet *next=discard->next;unix_uncharge(discard);kernel_heap_release(socket->heap,discard);discard=next;}
        unix_put(old);unix_put(target);return 0;
    }
    int result=unix_address_validate(address,0);if(result)return result;
    struct kernel_socket *child=NULL;
    result=kernel_socket_create(socket->heap,KERNEL_SOCKET_AF_UNIX,SOCKET_STREAM,&child);
    if(result)return result;
    struct socket_pending *entry=NULL;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(socket->heap,1,sizeof(*entry),(void **)&entry);
    if(status!=KERNEL_HEAP_STATUS_OK) {kernel_socket_destroy(child);return -KERNEL_ENOMEM;}
    uint64_t deadline=unix_deadline(socket);
    struct kernel_socket_peercred cred=unix_current_cred();
    for(;;) {
        struct kernel_socket *target=NULL;
        result=unix_lookup(socket,address,&target);if(result)break;
        SOCKET_STATE(state);
        result=socket->connected?-KERNEL_EISCONN:socket->listening?-KERNEL_EINVAL:
            target->destroying || !target->listening || target->read_closed?-KERNEL_ECONNREFUSED:0;
        if(!result && target->pending_live>target->unix_backlog) {
            result=unix_wait_queue(target,deadline,nonblocking?KERNEL_SOCKET_MSG_DONTWAIT:0);
            socket_state_release(&state);unix_put(target);
            if(!result)continue;
            break;
        }
        if(!result) {
            child->unix_name=target->unix_name;unix_name_get_locked(child->unix_name);
            child->unix_path=child->unix_name->path;
            child->peercred=cred;socket->peercred=target->peercred;
            child->connected=socket->connected=1;socket->peer_closed=0;
            unix_link_peer_locked(socket,child);unix_link_peer_locked(child,socket);
            child->listener=target;child->pending_ready=1;entry->child=child;
            if(target->pending_tail)target->pending_tail->next=entry;else target->pending_head=entry;
            target->pending_tail=entry;target->pending_live++;
            wake_socket(target);child=NULL;entry=NULL;
        }
        socket_state_release(&state);unix_put(target);break;
    }
    if(entry)kernel_heap_release(socket->heap,entry);
    if(child)kernel_socket_destroy(child);
    return result;
}

int kernel_socket_listen(struct kernel_socket *socket, int backlog)
{
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        struct kernel_socket_peercred cred=unix_current_cred();
        SOCKET_STATE(state);
        if(socket->type!=SOCKET_STREAM)return -KERNEL_EOPNOTSUPP;
        if(!socket->unix_name || socket->connected)return -KERNEL_EINVAL;
        socket->listening=1;socket->unix_backlog=(unsigned)backlog>4096U?4096U:(unsigned)backlog;
        socket->peercred=cred;wake_socket(socket);return 0;
    }
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    err_t error = ERR_OK;
    struct tcp_pcb *replacement;
    if (socket->type != SOCKET_STREAM) { kernel_socket_protocol_leave(&scope); return -KERNEL_EOPNOTSUPP; }
    if (socket->tcp == 0) { kernel_socket_protocol_leave(&scope); return -KERNEL_ENOTCONN; }
    if (socket->listening) { kernel_socket_protocol_leave(&scope); return 0; }
    int conflict = port_conflict(socket, local_ip(socket), local_port(socket), 1);
    if (conflict != 0) { kernel_socket_protocol_leave(&scope); return conflict; }
    replacement = tcp_listen_with_backlog_and_err(socket->tcp,
        backlog < 1 ? 1U : backlog > 255 ? 255U : (u8_t)backlog, &error);
    if (replacement != 0) {
        tcp_arg(replacement, socket);tcp_accept(replacement, socket_tcp_accepted);
        SOCKET_STATE(state);socket->tcp=replacement;socket->listening=1;
    }
    kernel_socket_protocol_leave(&scope);
    return replacement != 0 ? 0 : lwip_error(error);
}

int kernel_socket_connect(struct kernel_socket *socket,
                          const struct kernel_socket_address *address, int nonblocking)
{
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX)return unix_connect(socket,address,nonblocking);
    ip_addr_t remote;
    err_t error;
    struct kernel_socket_protocol_scope old_status;
    kernel_socket_protocol_enter(&old_status);
    if (socket->udp != 0) {
        if (address->family == 0) {
            udp_disconnect(socket->udp);
            { SOCKET_STATE(state);socket->connected=0; }
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
            if (result != 0) { kernel_socket_protocol_leave(&old_status); return result; }
            struct netif *route = ip_route(&socket->udp->local_ip, &remote);
            const ip_addr_t *source = route != 0 ? ip_netif_get_local_ip(route, &remote) : 0;
            if (source == 0) { kernel_socket_protocol_leave(&old_status); return -KERNEL_ENETUNREACH; }
            error = udp_connect(socket->udp, &remote, address->port);
            if (error == ERR_OK) {
                { SOCKET_STATE(state);socket->connected=1;
                  address_export(socket,&socket->udp->remote_ip,socket->udp->remote_port,&socket->saved_peer); }
                if (ip_addr_isany(&socket->udp->local_ip)) ip_addr_copy(socket->udp->local_ip, *source);
            }
        }
        kernel_socket_protocol_leave(&old_status);
        return lwip_error(error);
    }
    if (socket->type != SOCKET_STREAM) { int failure = (-KERNEL_EOPNOTSUPP); kernel_socket_protocol_leave(&old_status); return failure; }
    if (socket->tcp == 0) { int failure = (socket->error != 0 ? socket->error
                                                     : -KERNEL_ENOTCONN); kernel_socket_protocol_leave(&old_status); return failure; }
    if (socket->listening) { int failure = (-KERNEL_EINVAL); kernel_socket_protocol_leave(&old_status); return failure; }
    if (socket->connected) { int failure = (-KERNEL_EISCONN); kernel_socket_protocol_leave(&old_status); return failure; }
    if (socket->connecting) { int failure = (-KERNEL_EALREADY); kernel_socket_protocol_leave(&old_status); return failure; }
    int result = address_import(socket, address, &remote, 0);
    if (result != 0) { int failure = (result); kernel_socket_protocol_leave(&old_status); return failure; }
    error = tcp_connect(socket->tcp, &remote, address->port, tcp_connected);
    if(error==ERR_OK) {SOCKET_STATE(state);socket->connecting=1;}
    if (error == ERR_OK) service_inline(&old_status);
    int connected = socket->error ? socket->error : socket->connected ? 0 : -KERNEL_EINPROGRESS;
    kernel_socket_protocol_leave(&old_status);
    if (error != ERR_OK) return lwip_error(error);
    return nonblocking ? -KERNEL_EINPROGRESS : connected;
}

int kernel_socket_connection_result(struct kernel_socket *socket)
{
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {SOCKET_STATE(state);return socket->connected?0:-KERNEL_ECONNREFUSED;}
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    int result = socket->error ? socket->error : socket->connected ? 0 : -KERNEL_EINPROGRESS;
    kernel_socket_protocol_leave(&scope);
    return result;
}

int kernel_socket_accept(struct kernel_socket *socket,
                         struct kernel_socket **owner)
{
    struct socket_pending *entry;
    struct kernel_socket_protocol_scope old_status;
    if (owner == 0 || *owner != 0) return -KERNEL_EINVAL;
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        SOCKET_STATE(state);
        if(!socket->listening)return -KERNEL_EINVAL;
        entry=socket->pending_head;
        if(!entry)return -KERNEL_EAGAIN;
        socket->pending_head=entry->next;if(!socket->pending_head)socket->pending_tail=NULL;
        if(!socket->pending_live)__builtin_trap();
        socket->pending_live--;
        entry->child->listener=NULL;entry->child->pending_ready=0;*owner=entry->child;
        wake_socket(socket);socket_state_release(&state);kernel_heap_release(socket->heap,entry);return 0;
    }
    kernel_socket_protocol_enter(&old_status);
    service_inline(&old_status);
    { SOCKET_STATE(state);
      if (network_stopping) { socket_state_release(&state);
          kernel_socket_protocol_leave(&old_status); return -KERNEL_EIO; }
    }
    if (!socket->listening) { kernel_socket_protocol_leave(&old_status); return -KERNEL_EINVAL; }
    purge_dead_pending(socket);
    entry = socket->pending_head;
    if (entry == 0) {
        kernel_socket_protocol_leave(&old_status);
        return -KERNEL_EAGAIN;
    }
    socket->pending_head = entry->next;
    if (socket->pending_head == 0) socket->pending_tail = 0;
    { SOCKET_STATE(state);
      if(!socket->pending_live || !entry->child->pending_ready)__builtin_trap();
      socket->pending_live--;entry->child->pending_ready=0;entry->child->listener=0;
    }
    tcp_backlog_accepted(entry->child->tcp);
    *owner = entry->child;
    kernel_socket_protocol_leave(&old_status);
    kernel_heap_release(socket->heap, entry);
    return 0;
}

int kernel_socket_accept_check(const struct kernel_socket *socket)
{
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    if (socket->type != SOCKET_STREAM) { kernel_socket_protocol_leave(&scope); return -KERNEL_EOPNOTSUPP; }
    int result = socket->listening ? 0 : -KERNEL_EINVAL;
    kernel_socket_protocol_leave(&scope);
    return result;
}

int kernel_socket_wait_ready(struct kernel_socket *socket, uint32_t events,
    uint64_t target_ns, uint32_t flags)
{
    uint64_t deadline = 0;
    int timed = target_ns == 0;
    for (;;) {
        SOCKET_STATE(state);
        int ready = events == KERNEL_POLLIN && !socket->listening
            ? kernel_socket_receive_ready(socket)
            : (kernel_socket_poll(socket, 0) & (events | KERNEL_POLLERR | KERNEL_POLLHUP)) != 0;
        if (ready) return (events & KERNEL_POLLOUT) ? kernel_socket_unix_wait_error(socket) : 0;
        if (flags & KERNEL_SOCKET_MSG_DONTWAIT) return -KERNEL_EAGAIN;
        if (!timed) {
            enum kernel_time_status status = kernel_time_deadline_from_monotonic(target_ns, &deadline);
            if (status != KERNEL_TIME_STATUS_OK)
                return status == KERNEL_TIME_STATUS_DEADLINE_PASSED ? -KERNEL_EAGAIN : -KERNEL_EIO;
            timed = 1;
        }
        struct kernel_wait_token token;
        enum kernel_wait_wake_reason reason = KERNEL_WAIT_WOKEN;
        if (kernel_wait_prepare(&socket->wait, deadline, 1, &token) != KERNEL_SCHEDULER_STATUS_OK)
            return -KERNEL_EIO;
        /* State cannot change before registration; notification may precede park. */
        uintptr_t interrupts = socket_state_unlock_wait(&state);
        enum kernel_scheduler_status status = kernel_wait_park(&token, &reason);
        if (kernel_wait_finish(&token) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        arch_interrupt_restore(interrupts);
        if (status != KERNEL_SCHEDULER_STATUS_OK) return -KERNEL_EIO;
        if (reason == KERNEL_WAIT_TIMEOUT && target_ns && kernel_time_monotonic_ns() >= target_ns)
            return -KERNEL_EAGAIN;
        if (reason == KERNEL_WAIT_SIGNALLED) {
            if (target_ns) return -KERNEL_EINTR;
            kernel_signal_note_syscall_restart(kernel_task_current());
            return -KERNEL_ERESTARTSYS;
        }
    }
}

/* 无队首数据时的状态；查询不消费错误，reservation入口负责交付。 */
static int socket_receive_empty(const struct kernel_socket *socket, int nonblocking)
{
    if (socket->domain == KERNEL_SOCKET_DOMAIN_UNIX && socket->type == SOCKET_STREAM &&
        !socket->connected) return -KERNEL_EINVAL;
    if (socket->pending_error) return socket->pending_error;
    if (socket->domain == KERNEL_SOCKET_DOMAIN_INET && network_stopping) return -KERNEL_EIO;
    if (socket->type == SOCKET_DGRAM && socket->read_closed && nonblocking)
        return -KERNEL_EAGAIN;
    if (socket->read_closed ||
        (socket->type == SOCKET_STREAM && socket->peer_closed))
        return 0;
    if (socket->domain == KERNEL_SOCKET_DOMAIN_INET && socket->type == SOCKET_STREAM &&
        !socket->connected && !socket->connecting)
        return -KERNEL_ENOTCONN;
    return -KERNEL_EAGAIN;
}

int kernel_socket_receive_ready(struct kernel_socket *socket)
{
    SOCKET_STATE(saved);
    int ready = socket->read_request == 0 &&
        (socket->listening || socket->packets_head != 0 || socket_receive_empty(socket, 0) != -KERNEL_EAGAIN);
    socket_state_release(&saved);
    return ready;
}

static uint32_t packet_available(const struct socket_packet *packet)
{
    return packet->payload ? packet->payload->tot_len - packet->consumed : packet->length - packet->consumed;
}

int kernel_socket_reserve_read(struct kernel_socket *socket,
                              struct kernel_task *task,
                              struct kernel_socket_read_request *request,
                              struct kernel_open_file_description **pin_owner,
                              uint32_t capacity, int nonblocking)
{
    struct socket_packet *packet;
    uint32_t length = 0;
    struct socket_state_guard old_status __attribute__((cleanup(socket_state_release)))={0};
    if (request == 0 || request->socket != 0 ||
        pin_owner == 0 || *pin_owner == 0 ||
        kernel_open_file_socket(*pin_owner) != socket)
        __builtin_trap();
    socket_state_acquire(&old_status);
    if(socket->domain==KERNEL_SOCKET_DOMAIN_INET && network_stopping)return -KERNEL_EIO;
    if (socket->listening) {
        socket_state_release(&old_status);
        return socket->domain==KERNEL_SOCKET_DOMAIN_UNIX?-KERNEL_EINVAL:-KERNEL_ENOTCONN;
    }
    if (socket->read_request != 0) {
        socket_state_release(&old_status);
        return -KERNEL_EAGAIN;
    }
    packet = socket->packets_head;
    if (packet == 0) {
        int result = socket_receive_empty(socket, nonblocking);
        if (socket->pending_error) socket->pending_error = 0;
        socket_state_release(&old_status);
        return result;
    }
    /* recv(0)检查连接/等待状态，但不取得或消费已有stream数据。 */
    if (capacity == 0U && socket->type == SOCKET_STREAM) {
        socket_state_release(&old_status);
        return 0;
    }
    if (capacity > (uint32_t)__INT_MAX__) capacity = __INT_MAX__;
    uint32_t available = socket->type == SOCKET_DGRAM ? packet_available(packet) : socket->rx_bytes;
    length = available < capacity ? available : capacity;
    request->socket = socket;
    request->task = task;
    request->packet = packet;
    request->bytes = length;
    request->completed_bytes = 0;
    request->datagram = socket->type == SOCKET_DGRAM;
    request->pin = *pin_owner;
    request->pin_owner = pin_owner;
    *pin_owner = 0;
    if (task != 0 &&
        kernel_task_socket_read_register(task, request) != KERNEL_TASK_STATUS_OK)
        __builtin_trap();
    socket->read_request = request;
    socket_state_release(&old_status);
    return (int)length;
}

void kernel_socket_read_info(const struct kernel_socket_read_request *request,
                             struct kernel_socket_address *address, uint32_t *length)
{
    const struct socket_packet *packet = request->packet;
    if (request->socket == 0 || packet == 0) __builtin_trap();
    if (address != 0) {
        *address=packet->address;
        if(address->family==KERNEL_SOCKET_AF_UNIX) {
            SOCKET_STATE(state);unix_name_get_locked(address->unix_owner);
        }
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
    unsigned char *destination = buffer;
    while (length) {
        if (!packet) __builtin_trap();
        uint32_t available = packet_available(packet);
        if (offset >= available) { offset -= available; packet = packet->next; continue; }
        uint32_t take = available - offset < length ? available - offset : length;
        if (packet->payload) {
            if (pbuf_copy_partial(packet->payload, destination, (u16_t)take,
                                  packet->consumed + offset) != take) __builtin_trap();
        } else __builtin_memcpy(destination, packet->data + packet->consumed + offset, take);
        destination += take; length -= take; offset = 0; if(length)packet = packet->next;
    }
}

int kernel_socket_read_user(struct kernel_socket_read_request *request,
    struct kernel_mm *mm, struct kernel_uaccess_iov_cursor *cursor)
{
    const struct socket_packet *packet = request->packet;
    if (!request->socket || request->socket->read_request != request || !packet)
        __builtin_trap();
    uint32_t remaining = request->bytes;
    struct kernel_uaccess_write_cursor pages = {0};
    int result = 0;
    request->completed_bytes = 0;
    /* bytes固定本次借用的队列前缀；后来追加的packet不扩充本次用户复制。 */
    while (remaining) {
        if (!packet) __builtin_trap();
        uint32_t take = packet_available(packet);
        if (take > remaining) take = remaining;
        const struct pbuf *part = packet->payload;
        uint32_t offset = packet->consumed, pending = take;
        while (pending) {
            const void *source;
            size_t length = pending, copied = 0;
            if (part) {
                while (offset >= part->len) {
                    offset -= part->len; part = part->next;
                    if (!part) __builtin_trap();
                }
                if (length > (size_t)(part->len - offset)) length = part->len - offset;
                source = (const unsigned char *)part->payload + offset;
            } else {
                if (packet->payload) __builtin_trap();
                source = packet->data + offset;
            }
            enum kernel_uaccess_status status = kernel_copy_to_user_iov_cursor(mm, cursor, &pages, source, length, &copied);
            if (status == KERNEL_UACCESS_STATUS_FAULT) { result = -KERNEL_EFAULT; goto out; }
            if (status || copied != length) __builtin_trap();
            pending -= (uint32_t)length; offset += (uint32_t)length;
        }
        /* 故障packet即使写入用户前缀也不消费；此前完整成功的packet可提交短读。 */
        request->completed_bytes += take;
        remaining -= take; if(remaining)packet = packet->next;
    }
out:
    kernel_uaccess_write_cursor_finish(&pages);
    return result;
}

uint32_t kernel_socket_finish_read(struct kernel_socket_read_request *request, int user_fault)
{
    struct kernel_socket *socket = request->socket;
    if (!socket || socket->read_request != request || socket->packets_head != request->packet ||
        request->completed_bytes > request->bytes || (!request->datagram && !request->bytes)) __builtin_trap();
    uint32_t bytes = user_fault && !request->datagram ? request->completed_bytes : request->bytes;
    uint32_t remaining = bytes, charge = bytes;
    struct socket_packet *retired = 0, *tail = 0;
    if (request->datagram) {
        charge = packet_available(socket->packets_head);
        if (!charge) charge = 1;
    }
    /* The reservation owns this fixed prefix. Find its boundary with IRQs on;
     * an appender may change only the final next link, read below at detach. */
    struct socket_packet *packet = socket->packets_head;
    uint32_t partial = 0;
    while (remaining || request->datagram) {
        if (!packet) __builtin_trap();
        uint32_t available = packet_available(packet);
        uint32_t take = remaining < available ? remaining : available;
        if (request->datagram || take == available) tail = packet;
        else { partial = take; break; }
        remaining -= take;
        if (request->datagram || !remaining) break;
        packet = packet->next;
    }
    SOCKET_STATE(scope);
    if (tail) {
        retired = socket->packets_head;
        socket->packets_head = tail->next;
        tail->next = 0;
        if (!socket->packets_head) socket->packets_tail = 0;
    }
    if (partial) packet->consumed += partial;
    if (socket->rx_bytes < charge) __builtin_trap();
    socket->rx_bytes -= charge;
    if(request->datagram && socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        if(!socket->unix_queued)__builtin_trap();
        socket->unix_queued--;
    }
    if(charge && socket->type==SOCKET_STREAM && socket->peer)wake_socket(socket->peer);
    if (socket->domain == KERNEL_SOCKET_DOMAIN_INET) {
        if (socket->receive_credit > UINT32_MAX - bytes) __builtin_trap();
        if (!request->datagram) socket->receive_credit += bytes;
        if (retired) {
            if (socket->retired_tail) socket->retired_tail->next = retired;
            else socket->retired_head = retired;
            socket->retired_tail = tail;
            retired = 0;
        }
        socket_schedule(socket, SOCKET_WORK_RECEIVE | SOCKET_WORK_OUTPUT | SOCKET_WORK_REAP);
    }
    socket->read_request = 0;
    if (request->task && kernel_task_socket_read_clear(request->task, request) != KERNEL_TASK_STATUS_OK)
        __builtin_trap();
    if (*request->pin_owner || !request->pin) __builtin_trap();
    *request->pin_owner = request->pin;
    request->pin = 0;
    request->socket = 0;
    wake_socket(socket);
    uint32_t delivered = user_fault && request->datagram ? 0 : bytes;
    socket_state_release(&scope);
    int freed = retired != 0;
    while (retired) {
        struct socket_packet *next = retired->next;
        unix_uncharge(retired);
        kernel_heap_release(socket->heap, retired);
        retired = next;
    }
    if (freed) receive_capacity();
    return delivered;
}

void kernel_socket_abort_read(struct kernel_socket_read_request *request)
{
    SOCKET_STATE(old_status);
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
    socket_state_release(&old_status);
    if (kernel_open_file_release(&pin) != KERNEL_OPEN_FILE_STATUS_OK)
        __builtin_trap();
}

enum { SOCKET_WRITE_ACCEPTED=1U, SOCKET_WRITE_DEFERRED=2U };
enum socket_protocol_operation { SOCKET_PROTOCOL_STREAM, SOCKET_PROTOCOL_UDP_PREPARE,
    SOCKET_PROTOCOL_UDP_SEND, SOCKET_PROTOCOL_DISPOSE, SOCKET_PROTOCOL_FLUSH };
static int protocol_request(struct kernel_socket_write_request *,enum socket_protocol_operation);

static void socket_write_begin(struct kernel_socket_write_request *request,
    struct kernel_open_file_description **pin_owner,int hold_unix_peer)
{
    *request=(struct kernel_socket_write_request){.socket=kernel_open_file_socket(*pin_owner),
        .task=kernel_task_current(),.pin=*pin_owner,.pin_owner=pin_owner};
    *pin_owner=0;
    if(request->task && kernel_task_socket_write_register(request->task,request)!=KERNEL_TASK_STATUS_OK)__builtin_trap();
    SOCKET_STATE(state);
    struct kernel_socket *socket=request->socket;
    request->active_next=socket->active_writes;request->active_previous=&socket->active_writes;
    if(request->active_next)request->active_next->active_previous=&request->active_next;
    socket->active_writes=request;
    /* Close can detach the peer edge; borrow its storage in this same publication lock. */
    if(hold_unix_peer && socket->peer) {
        request->unix_destination=socket->peer;
        unix_get_locked(request->unix_destination);
    }
}
static void socket_write_end(struct kernel_socket_write_request *request)
{
    SOCKET_STATE(state);
    if(request->queued || !request->active_previous || *request->active_previous!=request)__builtin_trap();
    *request->active_previous=request->active_next;
    if(request->active_next)request->active_next->active_previous=request->active_previous;
    request->active_next=0;request->active_previous=0;
    if(request->task && kernel_task_socket_write_clear(request->task,request)!=KERNEL_TASK_STATUS_OK)__builtin_trap();
    struct kernel_socket *destination=request->unix_destination;
    request->unix_destination=NULL;
    int release=destination && unix_put_locked(destination);
    wake_socket(request->socket);
    struct kernel_heap *heap=release?destination->heap:NULL;
    socket_state_release(&state);
    if(release)kernel_heap_release(heap,destination);
}
static void tcp_flush_output(struct kernel_socket *socket);
static int tcp_write_buffer_locked(struct kernel_socket *socket,
    const void *buffer, uint32_t size, uint32_t flags, uint32_t reserved);

static void free_write_packet(struct kernel_socket_write_request *request)
{
    if (request->protocol_payload) {
        if(protocol_request(request,SOCKET_PROTOCOL_DISPOSE))__builtin_trap();
    }
    struct socket_packet *packet = request->packet;
    if (!packet) return;
    kernel_heap_release(request->socket->heap, packet);
    request->packet = 0;
    receive_capacity();
}
static void clear_write_request(struct kernel_socket_write_request *request)
{
    free_write_packet(request);
    if(request->unix_destination && request->reserved && request->socket->type==SOCKET_DGRAM) {
        SOCKET_STATE(state);
        if(!request->unix_destination->unix_reserved)__builtin_trap();
        request->unix_destination->unix_reserved--;
        if(!request->unix_destination->destroying)wake_socket(request->unix_destination);
    }
    kernel_socket_stream_cancel(request);
    if(request->progressed & SOCKET_WRITE_DEFERRED)
        (void)protocol_request(request,SOCKET_PROTOCOL_FLUSH);
    socket_write_end(request);
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
static int unix_peer_failure(struct kernel_socket *socket)
{
    if(socket->write_closed)return -KERNEL_EPIPE;
    if(socket->type==SOCKET_DGRAM && !socket->peer && socket->peer_closed) {
        if(socket->connected) {socket->connected=0;return -KERNEL_ECONNREFUSED;}
        return -KERNEL_ENOTCONN;
    }
    return socket->type==SOCKET_DGRAM?-KERNEL_ENOTCONN:-KERNEL_EPIPE;
}
int kernel_socket_unix_wait_error(struct kernel_socket *socket)
{
    SOCKET_STATE(irq);
    int error=socket->domain==KERNEL_SOCKET_DOMAIN_UNIX ? socket->pending_error : 0;
    if(error)socket->pending_error=0;
    socket_state_release(&irq);return error;
}
static int unix_reserve_packet(struct kernel_socket_write_request *request,uint32_t count,uint32_t flags)
{
    uint64_t deadline=(flags&KERNEL_SOCKET_MSG_DONTWAIT)?0:unix_deadline(request->socket);
    for(;;) {
        SOCKET_STATE(state);
        struct kernel_socket *sender=request->socket,*peer=request->unix_destination;
        int result=0;
        if(sender->write_closed)return -KERNEL_EPIPE;
        if(!peer || peer->destroying)
            return sender->type==SOCKET_DGRAM?-KERNEL_ECONNREFUSED:-KERNEL_EPIPE;
        if(peer->read_closed)return -KERNEL_EPIPE;
        if(sender->type==SOCKET_DGRAM && peer->peer && peer->peer!=sender)return -KERNEL_EPERM;
        struct kernel_socket *waiter=NULL;
        if(sender->unix_memory+sender->tx_reserved>=sender->tx_limit)waiter=sender;
        else if(unix_queue_full(sender,peer))waiter=peer;
        if(!waiter) {
            request->reserved=unix_charge(count);
            if(sender->tx_reserved>UINT32_MAX-request->reserved)__builtin_trap();
            sender->tx_reserved+=request->reserved;
            if(sender->type==SOCKET_DGRAM)peer->unix_reserved++;
            return 0;
        }
        result=unix_wait_queue(waiter,deadline,flags);
        if(result)return result;
    }
}
/* Publication has no user access. Keep its state guard off the stack while
 * a payload copy may recurse through an ELF/page-cache/block read fault. */
static __attribute__((noinline)) int unix_publish_packet(
    struct kernel_socket_write_request *request,uint32_t count)
{
    struct socket_packet *packet=request->packet;
    int result;
    SOCKET_STATE(endpoint);
    struct kernel_socket *peer=request->unix_destination;
    if(!peer || peer->destroying || request->socket->write_closed || peer->read_closed) {
        result=request->socket->type==SOCKET_DGRAM && peer && peer->destroying?-KERNEL_ECONNREFUSED:-KERNEL_EPIPE;
    } else if(request->socket->type==SOCKET_DGRAM && peer->peer && peer->peer!=request->socket) {
        result=-KERNEL_EPERM;
    } else {
        uint32_t charge=request->reserved;
        if(!charge || request->socket->tx_reserved<charge)__builtin_trap();
        request->socket->tx_reserved-=charge;request->reserved=0;
        if(request->socket->type==SOCKET_DGRAM) {
            if(!peer->unix_reserved)__builtin_trap();
            peer->unix_reserved--;peer->unix_queued++;
            if(request->socket->unix_name)unix_export_locked(request->socket->unix_name,&packet->address);
        }
        unix_publish_owner(request->socket,packet,charge);
        if(peer->packets_tail)peer->packets_tail->next=packet;else peer->packets_head=packet;
        peer->packets_tail=packet;
        uint32_t payload=count ? (uint32_t)count : 1U;
        if(peer->rx_bytes>UINT32_MAX-payload)__builtin_trap();
        peer->rx_bytes+=payload;request->packet=0;wake_socket(peer);result=(int)count;
    }
    return result;}

static int socket_write_packet_source(struct kernel_open_file_description **pin_owner,
    struct kernel_mm *mm, struct kernel_uaccess_iov_cursor *cursor,
    uint64_t count, uint32_t flags,
    const struct kernel_socket_address *destination, const void *kernel_buffer,
    int *user_fault)
{
    if (user_fault) *user_fault=0;
    struct kernel_socket *socket = kernel_open_file_socket(*pin_owner);
    {
        SOCKET_STATE(preflight);
        if(socket->type==SOCKET_STREAM) {
            if(socket->domain!=KERNEL_SOCKET_DOMAIN_UNIX)__builtin_trap();
            int error=(!socket->peer || socket->write_closed || socket->peer->read_closed)?unix_peer_failure(socket):0;
            if(error)return error;
            if(!count)return 0;
            uint32_t limit=socket->tx_limit/2U-64U;if(limit>32768U)limit=32768U;
            if(count>limit)count=limit;
        } else {
            if(kernel_buffer && socket->domain==KERNEL_SOCKET_DOMAIN_INET && !destination &&
                !socket->connected && count<=UINT16_MAX)return -KERNEL_EDESTADDRREQ;
            if(count>(socket->domain==KERNEL_SOCKET_DOMAIN_INET?UDP_MAX_PAYLOAD:socket->tx_limit-32U) ||
                count>socket->tx_limit)return -KERNEL_EMSGSIZE;
            if(socket->write_closed)return -KERNEL_EPIPE;
            if(socket->domain==KERNEL_SOCKET_DOMAIN_INET && !destination && !socket->connected)return -KERNEL_EDESTADDRREQ;
        }
    }
    int hold_unix_peer=socket->domain==KERNEL_SOCKET_DOMAIN_UNIX &&
        !(destination && socket->type==SOCKET_DGRAM);
    struct kernel_socket_write_request request={0};
    socket_write_begin(&request,pin_owner,hold_unix_peer);
    int result = -KERNEL_ENOMEM;
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        if(destination && socket->type==SOCKET_DGRAM) {
            result=unix_lookup(socket,destination,&request.unix_destination);if(result)goto out;
        } else {
            if(!request.unix_destination) {
                SOCKET_STATE(state);result=unix_peer_failure(socket);goto out;
            }
        }
    }
    if(request.socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        result=unix_reserve_packet(&request,(uint32_t)count,flags);
        if(result) {
            clear_write_request(&request);*pin_owner=request.pin;return result;
        }
    }
    result=-KERNEL_ENOMEM;
    struct socket_packet *packet = 0;
    void *data = 0;
    ip_addr_t remote;
    if (request.socket->domain == KERNEL_SOCKET_DOMAIN_INET) {
        request.destination=destination;request.protocol_address=&remote;request.payload_size=count;
        result=protocol_request(&request,SOCKET_PROTOCOL_UDP_PREPARE);
        if(result)goto out;
        data=((struct pbuf *)request.protocol_payload)->payload;
    } else {
        KERNEL_NO_RECLAIM_IO;
        enum kernel_heap_status status = kernel_heap_allocate_zeroed(request.socket->heap,
            1, sizeof(*packet)+(size_t)count, (void **)&packet);
        if (status != KERNEL_HEAP_STATUS_OK) {
            if (status != KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
            goto out;
        }
        request.packet = packet;
        packet->length = (uint32_t)count;
        data = packet->data;
    }
    if (kernel_buffer) {
        if (count) __builtin_memcpy(data, kernel_buffer, (size_t)count);
    } else {
        size_t copied = 0;
        enum kernel_uaccess_status access = kernel_copy_from_user_iov(mm, cursor,
            data, (size_t)count, &copied);
        if (access == KERNEL_UACCESS_STATUS_FAULT) {
            if (user_fault) *user_fault=1;
            if (request.socket->type != SOCKET_STREAM || !copied) {
                result = -KERNEL_EFAULT; goto out;
            }
            count=copied; packet->length=(uint32_t)copied;
        } else if (access != KERNEL_UACCESS_STATUS_OK || copied != count) __builtin_trap();
    }
    if(request.socket->domain==KERNEL_SOCKET_DOMAIN_INET) {
        result=protocol_request(&request,SOCKET_PROTOCOL_UDP_SEND);goto out;
    }
    result=unix_publish_packet(&request,(uint32_t)count);

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
    struct kernel_uaccess_iov_cursor cursor={iov,iov_count,0,0};
    return socket_write_packet_source(pin_owner, mm, &cursor, count,
                                        flags, destination, 0, 0);
}

int kernel_socket_write_packet_buffer(struct kernel_open_file_description **pin_owner,
    const void *buffer, uint64_t count, uint32_t flags)
{
    if (!buffer) return -KERNEL_EINVAL;
    return socket_write_packet_source(pin_owner, 0, 0, count, flags, 0, buffer, 0);
}

int kernel_socket_write_unix(struct kernel_open_file_description **pin_owner,
    struct kernel_mm *mm, struct kernel_uaccess_iov_cursor *cursor,
    uint64_t count, uint32_t flags, int *user_fault)
{
    return socket_write_packet_source(pin_owner,mm,cursor,count,flags,0,0,user_fault);
}

int kernel_socket_discard_receive(const struct kernel_socket *socket, uint32_t flags)
{
    return socket && socket->domain == KERNEL_SOCKET_DOMAIN_INET &&
           socket->type == SOCKET_STREAM && (flags & KERNEL_SOCKET_MSG_TRUNC);
}

static struct kernel_socket_statistics socket_statistics;
void kernel_socket_get_statistics(struct kernel_socket_statistics *statistics)
{
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    *statistics = socket_statistics;
    kernel_socket_protocol_leave(&scope);
}

#if BOAROS_COST_DIAGNOSTICS
void kernel_socket_protocol_snapshot(uint64_t values[KERNEL_SOCKET_PROTOCOL_VALUES])
{
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    const uint64_t snapshot[KERNEL_SOCKET_PROTOCOL_VALUES] = {
        socket_statistics.tcp_write_calls, socket_statistics.tcp_written_bytes,
        lwip_stats.tcp.xmit,lwip_stats.tcp.recv,lwip_stats.tcp.memerr,
        lwip_stats.udp.xmit,lwip_stats.udp.recv,lwip_stats.udp.memerr,
        lwip_stats.mem.used,lwip_stats.mem.max,
        (lwip_stats.memp[MEMP_TCP_PCB] ? lwip_stats.memp[MEMP_TCP_PCB]->used : 0),(lwip_stats.memp[MEMP_TCP_PCB_LISTEN] ? lwip_stats.memp[MEMP_TCP_PCB_LISTEN]->used : 0),
        (lwip_stats.memp[MEMP_TCP_SEG] ? lwip_stats.memp[MEMP_TCP_SEG]->used : 0),(lwip_stats.memp[MEMP_UDP_PCB] ? lwip_stats.memp[MEMP_UDP_PCB]->used : 0),
        (lwip_stats.memp[MEMP_PBUF] ? lwip_stats.memp[MEMP_PBUF]->used : 0),sizeof(STAT_COUNTER)*8U,
        (lwip_stats.memp[MEMP_TCP_SEG] ? lwip_stats.memp[MEMP_TCP_SEG]->max : 0),
        (lwip_stats.memp[MEMP_PBUF] ? lwip_stats.memp[MEMP_PBUF]->max : 0),
        (lwip_stats.memp[MEMP_PBUF_POOL] ? lwip_stats.memp[MEMP_PBUF_POOL]->used : 0),
        (lwip_stats.memp[MEMP_PBUF_POOL] ? lwip_stats.memp[MEMP_PBUF_POOL]->max : 0),
        (lwip_stats.memp[MEMP_TCP_PCB] ? lwip_stats.memp[MEMP_TCP_PCB]->max : 0),
        (lwip_stats.memp[MEMP_TCP_PCB_LISTEN] ? lwip_stats.memp[MEMP_TCP_PCB_LISTEN]->max : 0)
    };
    __builtin_memcpy(values,snapshot,sizeof(snapshot));
    kernel_socket_protocol_leave(&irq);
}
#endif

int kernel_socket_is_tcp(const struct kernel_socket *socket)
{ return socket && socket->domain == KERNEL_SOCKET_DOMAIN_INET && socket->type == SOCKET_STREAM; }

/* 调用者持raw资格；此处交付状态错误，不能先碰payload改变errno优先级。 */
static int tcp_write_state(struct kernel_socket *socket, uint32_t flags, int observe, struct kernel_task *task)
{
    SOCKET_STATE(irq);
    int result;
    if (socket->pending_error) {
        result = socket->pending_error;
        if (observe) socket->pending_error = 0;
        socket_state_release(&irq);
        return result;
    }
    if (network_stopping) result = -KERNEL_EIO;
    else if (socket->write_closed || (!socket->connected && !socket->connecting) || !socket->tcp)
        result = -KERNEL_EPIPE;
    else result = socket->connecting ? -KERNEL_EAGAIN : 0;
    socket_state_release(&irq);
    if (result == -KERNEL_EPIPE && observe && !(flags & KERNEL_SOCKET_MSG_NOSIGNAL) && task)
        (void)kernel_signal_send_task(task, 13U, 0);
    return result;
}

static uint32_t tcp_admission_capacity_locked(const struct kernel_socket *socket)
{
    uint32_t budget = socket->tx_budget;
    return budget > socket->tx_reserved ? budget - socket->tx_reserved : 0;
}
static uint32_t tcp_protocol_budget(const struct kernel_socket *socket)
{
    if (!socket->tcp || socket->listening || !socket->connected || socket->write_closed ||
        socket->write_blocked || tcp_sndqueuelen(socket->tcp) >= TCP_SND_QUEUELEN) return 0;
    uint32_t available = tcp_sndbuf(socket->tcp);
    uint32_t outstanding = TCP_SND_BUF - available;
    uint32_t budget = outstanding < socket->tx_limit ? socket->tx_limit - outstanding : 0;
    return available < budget ? available : budget;
}
static void tcp_publish_capacity_locked(struct kernel_socket *socket)
{
    socket->tx_budget = tcp_protocol_budget(socket);
}
static void tcp_flush_output(struct kernel_socket *socket)
{
    if (!socket->tcp || !socket->tcp->unsent) return;
    nic_blocked = 0;
    err_t output = tcp_output(socket->tcp);
    if (output == ERR_MEM) {
        if (nic_blocked) {
            SOCKET_STATE(state);
            work_remove(&nic_wait, socket);
            work_add_locked(&nic_wait, socket, nic_generation);
        } else socket_pool_wait(socket);
    }
}

void kernel_socket_stream_begin(struct kernel_socket_write_request *request,
    struct kernel_open_file_description **pin_owner)
{
    if (!request || !pin_owner || !kernel_socket_is_tcp(kernel_open_file_socket(*pin_owner))) __builtin_trap();
    socket_write_begin(request,pin_owner,0);
}
int kernel_socket_stream_reserve(struct kernel_socket_write_request *request,
    uint32_t size, uint32_t flags)
{
    SOCKET_STATE(irq);
    if (!request->pin || request->reserved) __builtin_trap();
    int error = tcp_write_state(request->socket, flags, !request->progressed, request->task);
    uint32_t available = error ? 0 : tcp_admission_capacity_locked(request->socket);
    if (!error && size) {
        if (size > available) size = available;
        if (!size) {
            socket_schedule(request->socket, SOCKET_WORK_OUTPUT);
            request->progressed &= (uint8_t)~SOCKET_WRITE_DEFERRED;
            error = -KERNEL_EAGAIN; COST_ADD(STREAM_ADMIT_BLOCKED, 1);
        }
        else { request->reserved = size; request->socket->tx_reserved += size; }
    }
    socket_state_release(&irq);
    return error ? error : (int)size;
}
static void stream_release_reservation(struct kernel_socket_write_request *request, int notify)
{
    if (!request->reserved) return;
    SOCKET_STATE(irq);
    if (request->socket->tx_reserved < request->reserved) __builtin_trap();
    request->socket->tx_reserved -= request->reserved; request->reserved = 0;
    if (notify) wake_socket(request->socket);
    socket_state_release(&irq);
}
void kernel_socket_stream_cancel(struct kernel_socket_write_request *request)
{ stream_release_reservation(request, 1); }
void kernel_socket_stream_finish(struct kernel_socket_write_request *request)
{
    int deferred=(request->progressed & SOCKET_WRITE_DEFERRED)!=0;
    stream_release_reservation(request, 1);
    if(deferred)(void)protocol_request(request,SOCKET_PROTOCOL_FLUSH);
    socket_write_end(request);
    if (*request->pin_owner || !request->pin) __builtin_trap();
    *request->pin_owner = request->pin;
    request->pin = 0;
    request->socket = 0;
}

static int stream_commit_owned(struct kernel_socket_write_request *request,
    const struct kernel_socket_write_segment *segments, unsigned count, uint32_t flags)
{
    if (!count || !request->reserved) __builtin_trap();
    uint32_t total = 0;
    int result = tcp_write_state(request->socket, flags, !request->progressed, request->task);
    for (unsigned i = 0; !result && i < count; i++) {
        uint32_t size = segments[i].size;
        if (!size || size > request->reserved - total) __builtin_trap();
        int accepted = tcp_write_buffer_locked(request->socket, segments[i].data,
            size, flags | (i + 1 < count ? KERNEL_SOCKET_IO_MORE : 0), request->reserved);
        if (accepted > 0) {
            total += (uint32_t)accepted;
            if ((uint32_t)accepted < size) break;
        } else result = accepted;
    }
    if (total) result = (int)total;
    SOCKET_STATE(endpoint);
    tcp_publish_capacity_locked(request->socket);
    stream_release_reservation(request, 0);
    if (result > 0) {
        request->progressed = SOCKET_WRITE_ACCEPTED |
            (((flags & KERNEL_SOCKET_IO_MORE) && tcp_admission_capacity_locked(request->socket))
                ? SOCKET_WRITE_DEFERRED : 0U);
    }
    socket_state_release(&endpoint);
    if (result == -KERNEL_EAGAIN) {
        tcp_flush_output(request->socket);
        request->progressed &= (uint8_t)~SOCKET_WRITE_DEFERRED;
    }
    if (result == -KERNEL_EAGAIN) COST_ADD(STREAM_PROTOCOL_BLOCKED, 1);
    return result;
}

static int execute_protocol_request(struct kernel_socket_write_request *request)
{
    COST_SCOPE(execution, NETWORK_PROTOCOL_EXEC_TICKS);
    struct kernel_socket *socket=request->socket;
    switch(request->operation) {
    case SOCKET_PROTOCOL_STREAM:
        return stream_commit_owned(request,request->segments,request->segment_count,request->flags);
    case SOCKET_PROTOCOL_UDP_PREPARE: {
        int error=request->destination?address_import(socket,request->destination,request->protocol_address,0):0;
        if(error)return error;
        request->protocol_payload=pbuf_alloc(PBUF_TRANSPORT,(u16_t)request->payload_size,PBUF_RAM);
        return request->protocol_payload?0:-KERNEL_ENOMEM;
    }
    case SOCKET_PROTOCOL_UDP_SEND: {
        struct pbuf *payload=request->protocol_payload;
        if(!payload)__builtin_trap();
        err_t error=request->destination?udp_sendto(socket->udp,payload,request->protocol_address,request->destination->port)
                                        :udp_send(socket->udp,payload);
        socket_pbuf_free(payload);request->protocol_payload=0;
        return error==ERR_OK?(int)request->payload_size:lwip_error(error);
    }
    case SOCKET_PROTOCOL_DISPOSE:
        if(!request->protocol_payload)__builtin_trap();
        socket_pbuf_free(request->protocol_payload);request->protocol_payload=0;return 0;
    case SOCKET_PROTOCOL_FLUSH:
        tcp_flush_output(socket);request->progressed &= (uint8_t)~SOCKET_WRITE_DEFERRED;return 0;
    }
    __builtin_trap();
}
static int protocol_request(struct kernel_socket_write_request *request,enum socket_protocol_operation operation)
{
    request->operation=operation;
    struct kernel_socket_protocol_scope protocol;
    if(protocol_enter(&protocol,network_work_queue!=0,1)) {
        int result;
        {SOCKET_STATE(state);result=network_stopping && operation<SOCKET_PROTOCOL_DISPOSE?-KERNEL_EIO:0;}
        if(!result)result=execute_protocol_request(request);
        request->segments=0;
        {SOCKET_STATE(state);
         if(operation==SOCKET_PROTOCOL_STREAM && tcp_admission_capacity_locked(request->socket))wake_socket(request->socket);}
        if(result>=0 && operation!=SOCKET_PROTOCOL_UDP_PREPARE && operation!=SOCKET_PROTOCOL_DISPOSE &&
            !(request->progressed & SOCKET_WRITE_DEFERRED))service_inline(&protocol);
        kernel_socket_protocol_leave(&protocol);return result;
    }
    SOCKET_STATE(state);
    if(!network_work_queue || (network_stopping && operation<SOCKET_PROTOCOL_DISPOSE)) {
        if(operation>=SOCKET_PROTOCOL_DISPOSE)__builtin_trap();
        request->segments=0;return -KERNEL_EIO;
    }
    struct kernel_socket *socket=request->socket;
    if(request->queued || request->next || !request->pin)__builtin_trap();
    COST_SCOPE(queued, NETWORK_PROTOCOL_QUEUE_TICKS);
    COST_ADD(NETWORK_PROTOCOL_QUEUED, 1);
#if BOAROS_COST_DIAGNOSTICS
    request->queued_ticks=kernel_cost_clock();
#endif
    request->queued=1;
    if(socket->writes_tail)socket->writes_tail->next=request;else socket->writes_head=request;
    /* 入队只发布待服务请求；STREAM/FLUSH 按实际窗口与 IO_MORE 决定输出。 */
    socket->writes_tail=request;socket_schedule(socket,SOCKET_WORK_REAP);
    while(request->queued) {
        COST_ADD(NETWORK_PROTOCOL_PARKS, 1);
        enum kernel_wait_wake_reason reason;
        if(kernel_wait_prepare(0,0,0,&request->completion)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
        uintptr_t interrupts=socket_state_unlock_wait(&state);
        if(kernel_wait_park(&request->completion,&reason)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
        if(kernel_wait_finish(&request->completion)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
        arch_interrupt_restore(interrupts);socket_state_acquire(&state);
    }
#if BOAROS_COST_DIAGNOSTICS
    COST_ADD(NETWORK_PROTOCOL_DISPATCH_TICKS,request->executing_ticks-request->queued_ticks);
    COST_ADD(NETWORK_PROTOCOL_RESUME_TICKS,kernel_cost_clock()-request->completed_ticks);
#endif
    return request->result;
}
int kernel_socket_stream_commitv(struct kernel_socket_write_request *request,
    const struct kernel_socket_write_segment *segments,unsigned count,uint32_t flags)
{
    if(request->segments || !segments || !count)__builtin_trap();
    request->segments=segments;request->segment_count=count;request->flags=flags;
    return protocol_request(request,SOCKET_PROTOCOL_STREAM);
}

int kernel_socket_stream_commit(struct kernel_socket_write_request *request,
    const void *buffer, uint32_t size, uint32_t flags)
{
    const struct kernel_socket_write_segment segment = {buffer, size};
    return kernel_socket_stream_commitv(request, &segment, 1, flags);
}

static int tcp_write_buffer_locked(struct kernel_socket *socket,
    const void *buffer, uint32_t size, uint32_t flags, uint32_t reserved)
{
    uint32_t length;
    err_t error;
    SOCKET_STATE(endpoint);
    uint32_t budget = tcp_protocol_budget(socket);
    if (socket->tx_reserved < reserved) __builtin_trap();
    uint32_t others = socket->tx_reserved - reserved;
    length = budget > others ? budget - others : 0;
    if (length > size) length = size;
    if (length > UINT16_MAX) length = UINT16_MAX;
    /* Core may be preempted inside tcp_write. Keep those bytes unavailable to
     * endpoint-only admission until the real PCB result is published. */
    socket->tx_budget = budget - length;
    socket_state_release(&endpoint);
    if (!length) return -KERNEL_EAGAIN;
    socket_statistics.tcp_write_calls++;
    error = tcp_write(socket->tcp, buffer, (u16_t)length, TCP_WRITE_FLAG_COPY |
        ((flags & KERNEL_SOCKET_IO_MORE) ? TCP_WRITE_FLAG_MORE : 0));
    if (error == ERR_OK) {
        socket_statistics.tcp_written_bytes += length;
        write_retry_disarm(socket);
        /* tcp_write owns the copied bytes even if immediate output defers. */
        /* 单次gather请求先填接纳窗口；末批、窗口满或finish退出时再发送。 */
        if (!(flags & KERNEL_SOCKET_IO_MORE) || tcp_protocol_budget(socket) <= others)
            tcp_flush_output(socket);
    } else if (error == ERR_MEM) {
        /* The global segment/pbuf pool can be full even while sndbuf remains.
         * Hide POLLOUT until an ACK or a bounded protocol-timer retry. */
        write_retry_arm(socket);
        socket_pool_wait(socket);
        tcp_flush_output(socket);
    }
    {SOCKET_STATE(state);tcp_publish_capacity_locked(socket);}
    return error == ERR_OK ? (int)length
                           : error == ERR_MEM ? -KERNEL_EAGAIN
                                              : lwip_error(error);
}

int kernel_socket_shutdown(struct kernel_socket *socket,int how)
{
    if(how<0 || how>2)return -KERNEL_EINVAL;
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        SOCKET_STATE(state);int result=socket->connected?0:-KERNEL_ENOTCONN;
        if(how!=1)socket->read_closed=1;
        if(how!=0) {socket->write_closed=1;if(socket->peer && socket->type==SOCKET_STREAM)socket->peer->peer_closed=1;}
        wake_socket(socket);if(socket->peer)wake_socket(socket->peer);return result;
    }
    struct kernel_socket_protocol_scope protocol;kernel_socket_protocol_enter(&protocol);
    SOCKET_STATE(state);
    int result=socket->connected?0:-KERNEL_ENOTCONN;
    int close_tx=how!=0 && !socket->write_closed;
    if(how!=1)socket->read_closed=1;
    if(close_tx) {
        socket->write_closed=1;
        if(socket->peer && socket->type==SOCKET_STREAM)socket->peer->peer_closed=1;
    }
    wake_socket(socket);if(socket->peer)wake_socket(socket->peer);
    socket_state_release(&state);
    if(close_tx && socket->tcp && socket->connected && !socket->listening) {
        err_t error=tcp_shutdown(socket->tcp,0,1);
        if(error!=ERR_OK)result=lwip_error(error);
    }
    socket_schedule(socket,SOCKET_WORK_REAP|SOCKET_WORK_OUTPUT);
    service_inline(&protocol);kernel_socket_protocol_leave(&protocol);return result;
}

int kernel_socket_set_option(struct kernel_socket *socket,
                             enum kernel_socket_option option, int value)
{
    struct kernel_socket_protocol_scope saved; kernel_socket_protocol_enter(&saved);
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
        /* UNIX按Linux倍增/下限和4MiB配置上限；INET保留现有协议预算。 */
        uint32_t maximum=socket->domain==KERNEL_SOCKET_DOMAIN_UNIX ? (4U<<20) : 32768U;
        if(limit>maximum)limit=maximum;
        limit *= 2;
        if (limit < minimum) limit = minimum;
        SOCKET_STATE(state);
        if (option == KERNEL_SOCKET_SNDBUF) socket->tx_limit = limit;
        else {
            socket->rx_limit = limit;
            /* UNIX接收缓存值可查询，但不能替发送者归还在途内存。 */
        }
        wake_socket(socket);
        break;
    }
    case KERNEL_SOCKET_NODELAY:
        if (socket->domain != KERNEL_SOCKET_DOMAIN_INET || socket->type != SOCKET_STREAM) { result = -KERNEL_ENOPROTOOPT; break; }
        socket->nodelay = value != 0;
        if (socket->tcp != 0 && !socket->listening) {
            if (value) {
                tcp_nagle_disable(socket->tcp);
                tcp_flush_output(socket);
            } else tcp_nagle_enable(socket->tcp);
        }
        break;
    case KERNEL_SOCKET_V6ONLY:
        if (socket->family != KERNEL_SOCKET_AF_INET6) { result = -KERNEL_ENOPROTOOPT; break; }
        if ((socket->udp != 0 && socket->udp->local_port != 0) ||
            (socket->tcp != 0 && socket->tcp->local_port != 0)) { result = -KERNEL_EINVAL; break; }
        {SOCKET_STATE(state);socket->v6only=value!=0;}
        if (pcb != 0) {
            ip_addr_set_zero_ip6(&pcb->local_ip);
            if (!value) IP_SET_TYPE(&pcb->local_ip, IPADDR_TYPE_ANY);
        }
        break;
    default: result = -KERNEL_ENOPROTOOPT;
    }
    kernel_socket_protocol_leave(&saved);
    return result;
}

int kernel_socket_get_option(struct kernel_socket *socket,
                             enum kernel_socket_option option, int *value)
{
    struct kernel_socket_protocol_scope saved; kernel_socket_protocol_enter(&saved);
    int result = 0;
    switch (option) {
    case KERNEL_SOCKET_REUSEADDR: *value = socket->reuseaddr; break;
    case KERNEL_SOCKET_KEEPALIVE: *value = socket->keepalive; break;
    case KERNEL_SOCKET_SNDBUF: *value = (int)socket->tx_limit; break;
    case KERNEL_SOCKET_RCVBUF: *value = (int)socket->rx_limit; break;
    case KERNEL_SOCKET_TYPE: *value = socket->type; break;
    case KERNEL_SOCKET_ACCEPTCONN: *value = socket->listening; break;
    case KERNEL_SOCKET_ERROR: {
        SOCKET_STATE(irq);
        *value = -socket->pending_error; socket->pending_error = 0;
        socket_state_release(&irq);
        break;
    }
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
    kernel_socket_protocol_leave(&saved);
    return result;
}
void kernel_socket_set_send_timeout(struct kernel_socket *socket, uint64_t nanoseconds)
{
    SOCKET_STATE(scope);
    socket->send_timeout_ns = nanoseconds;
    socket_state_release(&scope);
}
uint64_t kernel_socket_send_timeout(const struct kernel_socket *socket)
{
    SOCKET_STATE(scope);
    uint64_t timeout = socket->send_timeout_ns;
    socket_state_release(&scope);
    return timeout;
}

void kernel_socket_set_receive_timeout(struct kernel_socket *socket,
                                       uint64_t nanoseconds)
{
    SOCKET_STATE(scope);
    socket->receive_timeout_ns = nanoseconds;
    socket_state_release(&scope);
}

uint64_t kernel_socket_receive_timeout(const struct kernel_socket *socket)
{
    SOCKET_STATE(scope);
    uint64_t timeout = socket->receive_timeout_ns;
    socket_state_release(&scope);
    return timeout;
}

uint32_t kernel_socket_poll(struct kernel_socket *socket,
                            struct kernel_wait_queue **queue)
{
    uint32_t events = 0;
    SOCKET_STATE(snapshot_irq);
    COST_ADD(NETWORK_POLL_CALLS, 1);
#if BOAROS_COST_DIAGNOSTICS
    polling_snapshot++;
#endif
    if (queue != 0) *queue = &socket->wait;

    if (socket->domain == KERNEL_SOCKET_DOMAIN_UNIX) {
        SOCKET_STATE(saved);
        if(socket->pending_error)events|=KERNEL_POLLERR;
        if(socket->listening) {
            if(socket->pending_live)events|=KERNEL_POLLIN|KERNEL_POLLRDNORM;
        } else if (socket->type == SOCKET_DGRAM) {
            if ((socket->packets_head != 0 || socket->read_closed) && socket->read_request == 0)
                events |= KERNEL_POLLIN | KERNEL_POLLRDNORM;
            if (socket->read_closed) events |= KERNEL_POLLRDHUP;
            if (socket->read_closed && socket->write_closed) events |= KERNEL_POLLHUP;
            if (socket->peer != 0 && !socket->peer->read_closed) {
                if (unix_writable(socket) && !unix_queue_full(socket,socket->peer)) {
                    events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM;
                } else if(unix_queue_full(socket,socket->peer))socket->unix_space_wait=1;
            } else {
                events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM;
            }
        } else {
            if ((socket->packets_head != 0 || socket->peer_closed || socket->read_closed) &&
                socket->read_request == 0)
                events |= KERNEL_POLLIN | KERNEL_POLLRDNORM;
            if (socket->peer_closed || socket->read_closed) events |= KERNEL_POLLRDHUP;
            if ((socket->peer_closed || socket->read_closed) && socket->write_closed) events |= KERNEL_POLLHUP;
            if (socket->peer == 0) events |= KERNEL_POLLHUP;
            if (socket->peer != 0 && !socket->peer->read_closed) {
                if (unix_writable(socket)) {
                    events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM;
                }
            } else {
                events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM | KERNEL_POLLERR;
            }
        }
        socket_state_release(&saved);
#if BOAROS_COST_DIAGNOSTICS
        polling_snapshot--;
#endif
        socket_state_release(&snapshot_irq);
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
        SOCKET_STATE(saved);
        if (tcp_admission_capacity_locked(socket))
            events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM;
        socket_state_release(&saved);
    } else if (socket->peer_closed) {
        events |= KERNEL_POLLHUP | KERNEL_POLLOUT | KERNEL_POLLRDHUP | KERNEL_POLLIN;
    } else if (socket_receive_empty(socket, 0) == -KERNEL_ENOTCONN) {
        events |= KERNEL_POLLHUP | KERNEL_POLLOUT | KERNEL_POLLWRNORM;
    }
    if (socket->pending_error || network_stopping) events |= KERNEL_POLLERR;
#if BOAROS_COST_DIAGNOSTICS
    polling_snapshot--;
#endif
    socket_state_release(&snapshot_irq);
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
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    struct netif *loopback;
    if (flags == 0 || !is_loopback_name(name)) { kernel_socket_protocol_leave(&scope); return -KERNEL_ENODEV; }
    loopback = loopback_netif();
    if (loopback == 0) { kernel_socket_protocol_leave(&scope); return -KERNEL_ENODEV; }
    *flags = UINT16_C(0x8); /* IFF_LOOPBACK */
    if (netif_is_up(loopback)) *flags |= UINT16_C(0x1);
    if (netif_is_link_up(loopback)) *flags |= UINT16_C(0x40);
    kernel_socket_protocol_leave(&scope);
    return 0;
}

int kernel_socket_set_loopback_flags(const char name[16], uint16_t flags)
{
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    struct netif *loopback;
    if (!is_loopback_name(name)) { kernel_socket_protocol_leave(&scope); return -KERNEL_ENODEV; }
    loopback = loopback_netif();
    if (loopback == 0) { kernel_socket_protocol_leave(&scope); return -KERNEL_ENODEV; }
    if ((flags & UINT16_C(0x1)) != 0U) netif_set_up(loopback);
    else netif_set_down(loopback);
    kernel_socket_protocol_leave(&scope);
    return 0;
}

void kernel_socket_network_initialize(void)
{
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    if (!socket_initialized) {
        const struct boaros_lwip_hooks hooks = {
            .input = protocol_input, .work = protocol_work_ready, .capacity = protocol_capacity,
            .timewait_free = protocol_timewait_free
        };
        boaros_lwip_set_hooks(&hooks);
        lwip_init(); socket_initialized = 1;
    }
    kernel_socket_protocol_leave(&scope);
}
void kernel_socket_network_hooks(void (*timer_wake)(void *),struct kernel_wait_queue *work_queue,
    int (*udp_capacity)(void *),void *context)
{
    struct kernel_socket_protocol_scope protocol;kernel_socket_protocol_enter(&protocol);
    {SOCKET_STATE(state);network_stopping=work_queue==0;}
    if (!work_queue) for (struct kernel_socket *socket=inet_sockets; socket; socket=socket->inet_next) {
        SOCKET_STATE(state);
        /* Empty readers and control waiters have no active payload request to drain. */
        if (!socket->pending_error) socket->pending_error=-KERNEL_EIO;
        wake_socket(socket);
    }
    if(!work_queue)for(;;) {
        struct kernel_open_file_description *pin=0;
        struct kernel_wait_token token={0};uintptr_t interrupts=0;
        for(struct kernel_socket *socket=inet_sockets;socket;socket=socket->inet_next) {
            retire_received(socket);
            SOCKET_STATE(state);
            if(socket->active_writes)pin=socket->active_writes->pin;
            else if(socket->read_request)pin=socket->read_request->pin;
            if(!pin)continue;
            if(!protocol.outer || kernel_open_file_acquire(pin)!=KERNEL_OPEN_FILE_STATUS_OK)__builtin_trap();
            if(kernel_wait_prepare(&socket->wait,0,0,&token)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
            interrupts=socket_state_unlock_wait(&state);break;
        }
        if(!pin)break;
        /* Keep the existing worker alive while accepted callers return their real borrows. */
        protocol.interrupts=0;kernel_socket_protocol_leave(&protocol);
        enum kernel_wait_wake_reason reason;
        if(kernel_wait_park(&token,&reason)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
        if(kernel_wait_finish(&token)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
        arch_interrupt_restore(interrupts);
        if(kernel_open_file_release(&pin)!=KERNEL_OPEN_FILE_STATUS_OK)__builtin_trap();
        kernel_socket_protocol_enter(&protocol);
    }
    struct kernel_wait_queue *old_queue;
    {SOCKET_STATE(state);
     old_queue=network_work_queue;
     network_timer_wake=timer_wake;network_work_queue=work_queue;
     network_udp_capacity=udp_capacity;network_context=context;}
    kernel_socket_protocol_leave(&protocol);
    if (!work_queue && old_queue) for (;;) {
        int borrowed;
        {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);borrowed=old_queue->borrows!=0;}
        if(!borrowed)break;
        /* Hooks no longer admit cursors; their real queue borrows cover lock-free delivery. */
        kernel_wait_backend_quiesce();
    }
}
void kernel_socket_network_capacity(void)
{
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    SOCKET_STATE(state);nic_generation++;
    if(nic_wait.head)socket_work_ready_locked();
    socket_state_release(&state);
    kernel_socket_protocol_leave(&irq);
}
void kernel_socket_network_blocked(void) { nic_blocked = 1; }
static int capacity_ready(const struct socket_work_queue *queue, uint64_t generation)
{ return queue->head && queue->head->work_links[queue->slot].epoch != generation; }

int kernel_socket_work_pending(void)
{
    if (protocol_owner != kernel_io_context_current() || !protocol_depth) __builtin_trap();
    if (!socket_initialized) return 0;
    int timer_ready=sys_timeouts_sleeptime()==0;
    SOCKET_STATE(state);
    int runnable = ready_work.head || loopback_pending ||
        capacity_ready(&pool_wait, pool_generation) || capacity_ready(&nic_wait, nic_generation) ||
        capacity_ready(&receive_wait, receive_generation) || timer_ready ||
        (write_retry_head && (int32_t)(sys_now() - write_retry_head->write_retry_ms) >= 0);
    return runnable;
}

static void service_write(struct kernel_socket *socket)
{
    SOCKET_STATE(irq);
    struct kernel_socket_write_request *request = socket->writes_head;
    if (!request) { socket_state_release(&irq); return; }
    socket->writes_head = request->next;
    if (!socket->writes_head) socket->writes_tail = 0;
    request->next = 0;
    socket_state_release(&irq);
    int result;
    {SOCKET_STATE(state);result=network_stopping && request->operation<SOCKET_PROTOCOL_DISPOSE?-KERNEL_EIO:0;}
#if BOAROS_COST_DIAGNOSTICS
    request->executing_ticks=kernel_cost_clock();
#endif
    if(!result)result=execute_protocol_request(request);
#if BOAROS_COST_DIAGNOSTICS
    request->completed_ticks=kernel_cost_clock();
#endif
    socket_state_acquire(&irq);
    request->result = result;
    request->segments = 0;request->queued=0;
    if (socket->writes_head) socket_schedule(socket, SOCKET_WORK_REAP);
    /* 原调用者重新取得端点保护前保活请求；在发布结果的同一保护内
     * 定向通知，ACK/POLLOUT 不再唤醒尚未完成 COPY 的等待者。 */
    if(kernel_wait_notify(&request->completion,KERNEL_WAIT_WOKEN)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
    wake_socket(socket);
    socket_state_release(&irq);
}

static int service_ready_socket(void)
{
    SOCKET_STATE(irq);
    if (!ready_work.head) { socket_state_release(&irq); return 0; }
    struct kernel_socket *socket = ready_work.head;
    unsigned flags = socket->work_flags; socket->work_flags = 0;
    work_remove(&ready_work, socket);
    socket_state_release(&irq);
    retire_received(socket);
    if ((flags & SOCKET_WORK_LISTENER) && socket->listening) purge_dead_pending(socket);
    if ((flags & SOCKET_WORK_RECEIVE) && socket->receive_retry && socket->tcp && socket->tcp->refused_data)
        (void)tcp_process_refused_data(socket->tcp);
    retire_timewait(socket);
    if ((flags & SOCKET_WORK_OUTPUT) && socket->tcp && !socket->listening && socket->tcp->unsent) {
        nic_blocked = 0;
        err_t error = tcp_output(socket->tcp);
        if (error == ERR_MEM) {
            if (nic_blocked) { SOCKET_STATE(state);work_remove(&nic_wait, socket); work_add_locked(&nic_wait, socket, nic_generation); }
            else socket_pool_wait(socket);
        }
    }
    /* 已捕获的OUTPUT属于此前输入/重试；先完成它，再接纳新COPY，
     * 避免旧标记把新请求仍可合批的IO_MORE前缀提前送入协议。 */
    service_write(socket);
    SOCKET_STATE(state);
    uint32_t previous = tcp_admission_capacity_locked(socket);
    tcp_publish_capacity_locked(socket);
    if (tcp_admission_capacity_locked(socket) > previous) wake_socket(socket);
    return 1;
}
static int service_capacity(void)
{
    SOCKET_STATE(state);
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

static void service_loopback(struct kernel_socket_service_result *result, unsigned budget)
{
    for (struct netif *netif = netif_list; netif; netif = netif->next) {
        if (result->packets < budget)
            result->packets += netif_poll_budget(netif, budget - result->packets);
    }
    /* 输入可产生又被本批消费的ACK；发布实际剩余队列，避免空转一轮。 */
    loopback_pending = 0;
    for (struct netif *netif = netif_list; netif; netif = netif->next)
        if (netif->loop_first) loopback_pending = 1;
}

struct kernel_socket_service_result kernel_socket_service_owned(const struct kernel_socket_protocol_scope *scope,
    struct kernel_socket_service_budget budget)
{
    struct kernel_socket_service_result result = {0};
#if BOAROS_COST_DIAGNOSTICS
    if (polling_snapshot) COST_ADD(NETWORK_POLL_SERVICES, 1);
#endif
    if (!scope->outer || protocol_owner != kernel_io_context_current() || protocol_depth != 1) __builtin_trap();
    if (!socket_initialized) return result;
    COST_ADD(NETWORK_SERVICE_CALLS, 1);
    service_loopback(&result, budget.packets);
    /* 容量、到期重试和实际socket工作共用额度，沿原游标轮转。 */
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
    /* 先批量接收旧ACK归还额度；本批COPY/窗口更新产生了新包时，
     * 使用尚未用完的原包预算交付，避免无谓地再等worker一次调度。 */
    if (loopback_pending && result.packets < budget.packets)
        service_loopback(&result, budget.packets);
    socket_timer_running = 1;
    result.timers = sys_check_timeouts_budget(budget.timers);
    socket_timer_running = 0;
    result.runnable = kernel_socket_work_pending();
    result.next_deadline = socket_next_timer_deadline();
    COST_ADD(NETWORK_SOCKET_VISITS, result.sockets);
    COST_ADD(NETWORK_LOOP_PACKETS, result.packets);
    COST_ADD(NETWORK_TIMER_CALLBACKS, result.timers);
    return result;
}

struct kernel_socket_service_result kernel_socket_service_pending(struct kernel_socket_service_budget budget)
{
    if (protocol_owner == kernel_io_context_current()) return (struct kernel_socket_service_result){0};
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    struct kernel_socket_service_result result = kernel_socket_service_owned(&scope, budget);
    kernel_socket_protocol_leave(&scope);
    return result;
}

void kernel_socket_network_failed(uint32_t address)
{
    struct kernel_socket_protocol_scope saved; kernel_socket_protocol_enter(&saved);
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
        if (s->tcp && !s->listening) {
            /* tcp_failed publishes this device error exactly once. */
            tcp_abort(s->tcp);
        } else {
            SOCKET_STATE(endpoint);
            s->error = s->pending_error = -KERNEL_EIO;
            wake_socket(s);
            socket_state_release(&endpoint);
        }
    }
    socket_device_failed = 0;
    kernel_socket_protocol_leave(&saved);
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
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    for (struct netif *n = netif_list; n; n = n->next) if (netif_get_index(n) == index) {
        interface_snapshot(n, s); kernel_socket_protocol_leave(&irq); return 0;
    }
    kernel_socket_protocol_leave(&irq); return -KERNEL_ENODEV;
}
int kernel_socket_interface_get(const char name[16], struct kernel_socket_interface *s)
{
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    for (struct netif *n = netif_list; n; n = n->next) {
        int loop = n->name[0] == 'l' && n->name[1] == 'o';
        if (!__builtin_memcmp(name, loop ? "lo" : "eth0", loop ? 3 : 5)) {
            interface_snapshot(n, s); kernel_socket_protocol_leave(&irq); return 0;
        }
    }
    kernel_socket_protocol_leave(&irq); return -KERNEL_ENODEV;
}
int kernel_socket_interface_nth(uint32_t ordinal, struct kernel_socket_interface *s)
{
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq); unsigned previous = 0;
    for (uint32_t i = 0; i <= ordinal; i++) {
        struct netif *selected = 0;
        for (struct netif *n = netif_list; n; n = n->next)
            if (netif_get_index(n) > previous && (!selected || netif_get_index(n) < netif_get_index(selected))) selected = n;
        if (!selected) { kernel_socket_protocol_leave(&irq); return -KERNEL_ENODEV; }
        previous = netif_get_index(selected);
        if (i == ordinal) { interface_snapshot(selected, s); kernel_socket_protocol_leave(&irq); return 0; }
    }
    __builtin_trap();
}
int kernel_socket_interface_set_flags(const char name[16], uint16_t flags)
{
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    for (struct netif *n = netif_list; n; n = n->next) {
        int loop = n->name[0] == 'l' && n->name[1] == 'o';
        if (!__builtin_memcmp(name, loop ? "lo" : "eth0", loop ? 3 : 5)) {
            if (flags & 1) netif_set_up(n); else netif_set_down(n);
            kernel_socket_protocol_leave(&irq); return 0;
        }
    }
    kernel_socket_protocol_leave(&irq); return -KERNEL_ENODEV;
}
