#include <kernel/bpf.h>
#include <kernel/network.h>
#include <kernel/async_io.h>
#include <kernel/bpf_vm.h>
#include <kernel/credentials.h>
#include <kernel/io_context.h>
#include <kernel/socket.h>
#include <kernel/socket_control.h>
#include <kernel/socket_task.h>
#include <kernel/pid.h>
#include <kernel/tty_task.h>
#include "../fs/uaccess_iov_internal.h"
#include "socket_state.h"
#include "packet_ring.h"

#include <arch/context.h>
#include <arch/timer.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/fs_context.h>
#include <kernel/files.h>
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
#include "lwip/etharp.h"
#include "lwip/memp.h"
#include "lwip/ip.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/igmp.h"
#include "lwip/nd6.h"
#include "lwip/mld6.h"
#include "lwip/priv/nd6_priv.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/prot/udp.h"

#include <stddef.h>
#include <stdint.h>

#include "rtnetlink.h"

#define SOCKET_DGRAM 2
#define SOCKET_STREAM 1
#define SOCKET_SEQPACKET 5
#define SOCKET_RAW 3
#define NETLINK_COOKIE_SIZE 32U
#define PACKET_ETH_HEADER 14U
#define PACKET_ETH_ALL 3U
#define PACKET_HOST 0U
#define PACKET_BROADCAST 1U
#define PACKET_MULTICAST 2U
#define PACKET_OTHERHOST 3U
#define PACKET_OUTGOING 4U
#define UDP_MAX_PAYLOAD 65507U
#define UDP_RECEIVE_LIMIT (96U * 1024U)
#define RAW_MAX_PAYLOAD 65515U
#define SOCKET_WRITE_BATCH 4U
#define SOCKET_WRITE_RETRY_MS 250U

struct socket_packet {
    struct socket_packet *next;
    struct pbuf *payload;
    uint32_t length;
    struct kernel_socket_address address;
    uint32_t consumed;
    uint8_t loopback;
    struct kernel_socket *unix_sender;
    uint32_t send_charge;
    struct kernel_socket_control *control;
    unsigned char data[];
};

struct socket_pending {
    struct socket_pending *next;
    struct kernel_socket *child;
};

struct socket_udp_pending {
    struct pbuf *head, *tail;
    ip_addr_t remote;
    uint32_t bytes, charge;
    uint16_t port;
};

struct socket_packet_options {
    struct kernel_mutex configuration;
    struct packet_ring *ring;
    struct kernel_socket *timer_next, **timer_previous;
    uint32_t deadline, version, reserve;
    uint32_t packets, drops, freezes;
};

struct socket_link { struct kernel_socket *next, *previous; uint64_t epoch; unsigned queued; };
struct kernel_socket_name {
    struct kernel_heap *heap;
    uint32_t references;
    uint16_t length;
    struct kernel_vfs_path *path;
    unsigned char bytes[];
};
struct socket_ipv6_member {
    struct socket_ipv6_member *next;
    ip6_addr_t group;
    uint8_t index;
    uint8_t active; /* Owns a reference in the current IPv6 netif generation. */
};
struct socket_ipv4_member {
    struct socket_ipv4_member *next;
    ip4_addr_t group;
    uint32_t address;
    uint8_t index;
};
struct unix_credentials {
    struct kernel_pid *pid;
    uint32_t uid,gid;
};
static void unix_cred_release(struct unix_credentials *cred)
{
    if(cred->pid)kernel_task_identity_put(cred->pid);
    cred->pid=NULL;
}

struct kernel_socket {
    struct kernel_heap *heap;
    struct kernel_raw_lock endpoint_lock;
    struct kernel_bpf_program *filter;
    unsigned filter_locked;
    struct kernel_wait_queue wait;
    struct kernel_wait_queue async_events;
    struct socket_packet *packets_head;
    struct socket_packet *packets_tail;
    struct socket_packet *retired_head, *retired_tail;
    struct socket_packet *udp_spares;
    uint32_t receive_credit;
    uint64_t receive_sequence;
    struct socket_pending *pending_head;
    struct socket_pending *pending_tail;
    struct kernel_socket_read_request *read_request;
    struct kernel_socket_write_request *writes_head, *writes_tail;
    struct kernel_socket *write_retry_next;
    struct kernel_socket *inet_next, **inet_previous;
    struct socket_link work_links[4];
    struct kernel_socket *listener;
    unsigned work_flags, pending_live;
    uint8_t pending_ready, destroying;
    struct udp_pcb *udp;
    struct socket_udp_pending *udp_pending;
    struct raw_pcb *raw;
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
    uint32_t keep_idle, keep_intvl, keep_cnt;
    struct kernel_socket_tcp_info retired_info;
    uint32_t retired_info_ms;
    uint8_t nodelay, cork;
    uint8_t broadcast, ipv4_ttl, multicast_ttl, oobinline;
    uint8_t ipv4_multicast_loop, ipv4_multicast_all;
    struct socket_ipv4_member *ipv4_members;
    uint8_t ipv6_multicast_index, ipv6_multicast_hops, ipv6_multicast_loop, ipv6_multicast_all;
    struct socket_ipv6_member *ipv6_members;
    uint8_t bound_device, multicast_index;
    ip_addr_t multicast_source;
    uint32_t rx_bytes;
    uint32_t rx_limit;
    uint8_t udp_spare_count;
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
    struct unix_credentials peercred;
    struct kernel_socket_control *rights_head;
    struct kernel_open_file_description *gc_file;
    struct kernel_socket *gc_next, *gc_work;
    uint32_t gc_inflight;
    unsigned gc_mark, passcred;
    uint32_t netlink_references, record_memory, netlink_port;
    struct kernel_socket *netlink_next;
    struct kernel_socket *packet_next, **packet_previous;
    struct socket_packet_options *packet_options;
    uint32_t packet_index;
    uint16_t packet_protocol;
#if BOAROS_USER_SMP
    unsigned tcp_cpu;
    uint16_t tcp_prepare_mss;
    uint8_t udp_output_active, udp_rx_drained;
#endif
};

/* Link-level taps and their bind configuration share the IP qualification.
 * The atomic count avoids entering the protocol path when no tap is active. */
static struct kernel_socket *packet_taps;
static struct kernel_socket *packet_timer_head;
static unsigned packet_tap_count;

static int packet_taps_active(void)
{ return __atomic_load_n(&packet_tap_count,__ATOMIC_ACQUIRE)!=0; }

static int packet_hooked(const struct kernel_socket *socket)
{ return socket->packet_protocol && socket->packet_index!=UINT32_MAX; }

static void packet_tap_unlink(struct kernel_socket *socket)
{
    if (!socket->packet_previous) return;
    if (socket->packet_next) socket->packet_next->packet_previous=socket->packet_previous;
    *socket->packet_previous=socket->packet_next;
    socket->packet_previous=NULL;
    socket->packet_next=NULL;
    if (packet_hooked(socket)) __atomic_sub_fetch(&packet_tap_count,1U,__ATOMIC_RELEASE);
}

static void packet_tap_update(struct kernel_socket *socket,uint32_t index,uint16_t protocol)
{
    int was_hooked=packet_hooked(socket);
    socket->packet_index=index;
    socket->packet_protocol=protocol;
    if (!socket->packet_previous) {
        socket->packet_previous=&packet_taps;
        socket->packet_next=packet_taps;
        if (packet_taps) packet_taps->packet_previous=&socket->packet_next;
        packet_taps=socket;
    }
    if (!was_hooked && packet_hooked(socket)) __atomic_add_fetch(&packet_tap_count,1U,__ATOMIC_RELEASE);
    else if (was_hooked && !packet_hooked(socket)) __atomic_sub_fetch(&packet_tap_count,1U,__ATOMIC_RELEASE);
}
static struct netif *loopback_netif(void);
static void socket_work_ready_locked(struct socket_state_guard *state);

static void packet_timer_disarm(struct socket_packet_options *options)
{
    if (!options->timer_previous) return;
    *options->timer_previous = options->timer_next;
    if (options->timer_next)
        options->timer_next->packet_options->timer_previous = options->timer_previous;
    options->timer_previous = NULL;
    options->timer_next = NULL;
}

static void packet_timer_update(struct kernel_socket *socket)
{
    struct socket_packet_options *options = socket->packet_options;
    uint32_t deadline;
    if (!packet_ring_deadline(options->ring, &deadline)) {
        packet_timer_disarm(options);
        return;
    }
    if (options->timer_previous && options->deadline == deadline) return;
    packet_timer_disarm(options);
    struct kernel_socket **link = &packet_timer_head;
    while (*link && (int32_t)((*link)->packet_options->deadline - deadline) <= 0)
        link = &(*link)->packet_options->timer_next;
    options->deadline = deadline;
    options->timer_previous = link;
    options->timer_next = *link;
    if (*link) (*link)->packet_options->timer_previous = &options->timer_next;
    *link = socket;
    if (link == &packet_timer_head) {
        SOCKET_STATE(state);
        socket_work_ready_locked(state.owner);
    }
}

/* UNIX packet publication also changes its peer's quota, reference graph and
 * names. INET and netlink endpoint transactions touch only their own socket. */
static struct kernel_raw_lock *socket_endpoint_lock(struct kernel_socket *socket)
{
    return socket->domain==KERNEL_SOCKET_DOMAIN_UNIX ? &socket_state_lock : &socket->endpoint_lock;
}
#define SOCKET_ENDPOINT(name,socket) \
    struct socket_state_guard name __attribute__((cleanup(socket_state_release))); \
    name.owner=0; \
    socket_state_acquire_lock(&name,socket_endpoint_lock(socket))

/* Registration can outlive its FD. Its independent endpoint reference permits
 * close to revoke the queue and wake quota waiters without waiting for a pin. */
struct kernel_netlink_cookie {
    struct kernel_heap *heap;
    struct kernel_socket *socket;
    struct kernel_open_file_description *pin;
    struct socket_packet *packet;
    struct kernel_wait_token token;
    unsigned reserved;
};

/* A socket reference protects the pointer; an invocation takes its own program
 * reference so replacement never waits for packet execution under state locks. */
static int socket_filter_attached(const struct kernel_socket *socket)
{ return __atomic_load_n(&socket->filter,__ATOMIC_ACQUIRE)!=NULL; }

struct kernel_bpf_program *kernel_socket_filter_get(struct kernel_socket *socket)
{
    if (!socket_filter_attached(socket)) return NULL;
    SOCKET_ENDPOINT(state,socket);
    struct kernel_bpf_program *program=socket->filter;
    if(program)kernel_bpf_program_get(program);
    return program;
}
int kernel_socket_filter_locked(struct kernel_socket *socket)
{ SOCKET_ENDPOINT(state,socket);return socket->filter_locked; }
int kernel_socket_filter_lock(struct kernel_socket *socket,int value)
{
    SOCKET_ENDPOINT(state,socket);
    if(socket->filter_locked && !value)return -KERNEL_EPERM;
    socket->filter_locked=!!value;return 0;
}
int kernel_socket_filter_set(struct kernel_socket *socket,struct kernel_bpf_program *program)
{
    SOCKET_ENDPOINT(state,socket);
    if(socket->filter_locked)return -KERNEL_EPERM;
    if(!program && !socket->filter)return -KERNEL_ENOENT;
    struct kernel_bpf_program *old=socket->filter;
    __atomic_store_n(&socket->filter,program,__ATOMIC_RELEASE);
    socket_state_release(&state);
    if(old)kernel_bpf_program_put(old);
    return 0;
}
static uint32_t socket_filter_run(struct kernel_socket *socket,const struct kernel_bpf_packet *packet)
{
    struct kernel_bpf_program *program=kernel_socket_filter_get(socket);
    if(!program)return UINT32_MAX;
    uint32_t result=kernel_bpf_filter(program,packet);
    kernel_bpf_program_put(program);
    return result;
}
static void socket_filter_inherit_locked(struct kernel_socket *child,struct kernel_socket *parent)
{
    struct kernel_bpf_program *program=parent->filter;
    child->filter_locked=parent->filter_locked;
    if(program)kernel_bpf_program_get(program);
    __atomic_store_n(&child->filter,program,__ATOMIC_RELEASE);
}
struct socket_filter_packet {
    struct pbuf *packet;
    const unsigned char *network;
    uint16_t network_length;
};
static int socket_filter_read(void *object,int64_t offset,void *buffer,uint32_t length)
{
    struct socket_filter_packet *view=object;
    unsigned char *out=buffer;
    if(offset<0) {
        if(offset<-(int64_t)view->network_length)return -1;
        uint32_t count=(uint32_t)-offset;
        if(count>length)count=length;
        __builtin_memcpy(out,view->network+view->network_length+offset,count);
        out+=count;length-=count;offset+=count;
    }
    if(!length)return 0;
    if(offset<0 || (uint64_t)offset+length>view->packet->tot_len)return -1;
    return pbuf_copy_partial(view->packet,out,(u16_t)length,(u16_t)offset)==length?0:-1;
}
static int socket_ingress_filter(struct kernel_socket *socket,struct pbuf *p,struct netif *input,
    int ipv6,const void *network,uint16_t network_length,uint16_t header)
{
    if(!socket)return 1;
    struct kernel_bpf_program *program=kernel_socket_filter_get(socket);
    if (!program) return 1;
    struct socket_filter_packet view={p,network,network_length};
    uint16_t index=netif_get_index(input);
    struct kernel_bpf_packet packet={.object=&view,.length=p->tot_len,
        .protocol=ipv6?0x86dd:0x0800,.ifindex=index,.ingress_ifindex=index,
        .packet_type=(p->flags&PBUF_FLAG_LLBCAST)?1:(p->flags&PBUF_FLAG_LLMCAST)?2:0,
        .hardware_type=input->hwaddr_len?1:772,.payload_offset=header,
        .network_offset=network_length?-(int32_t)network_length:INT32_MIN,
        .link_offset=INT32_MIN,.read=socket_filter_read};
    uint32_t length=kernel_bpf_filter(program,&packet);
    kernel_bpf_program_put(program);
    if(!length)return 0;
    if(length<header)length=header;
    if(length<p->tot_len)pbuf_realloc(p,(u16_t)length);
    return 1;
}

static void socket_ipv4_members_release(struct kernel_socket *);
static void socket_ipv6_members_release(struct kernel_socket *);
static void udp_received(void *,struct udp_pcb *,struct pbuf *,const ip_addr_t *,u16_t);
static int udp_receive_fits_locked(const struct kernel_socket *, uint32_t);
static void tcp_failed(void *,err_t);
static void socket_tcp_info_retire(struct tcp_pcb *);
static err_t socket_tcp_accepted(void *,struct tcp_pcb *,err_t);

static struct kernel_socket *tcp_filter_socket(const struct tcp_pcb *pcb)
{
    /* Raw lwIP callers own their opaque arguments. Only our registered
     * callbacks establish a socket association; LISTEN PCBs are smaller. */
    if (pcb->state==LISTEN) {
        const struct tcp_pcb_listen *listener=(const struct tcp_pcb_listen *)pcb;
        return listener->accept==socket_tcp_accepted?listener->callback_arg:NULL;
    }
    if (pcb->errf==tcp_failed) return pcb->callback_arg;
    /* Before accept installs the child's callbacks, lwIP borrows the live
     * listener. Closing the listener clears this link on pending children. */
    if (pcb->state==SYN_RCVD && pcb->listener && pcb->listener->accept==socket_tcp_accepted)
        return pcb->listener->callback_arg;
    return NULL;
}
static int socket_udp_ingress_filter(struct udp_pcb *pcb,struct pbuf *p,struct netif *input,
    int ipv6,const void *network,uint16_t network_length,uint16_t header)
{
    struct kernel_socket *socket=pcb->recv==udp_received?pcb->recv_arg:NULL;
    if (!socket_ingress_filter(socket,p,input,ipv6,network,network_length,header)) return 0;
    if (p->boaros_udp_rx_borrowed && socket) {
        /* Filtering can shorten the packet. Only an admissible record
         * should be copied out of the transient receive slot. */
        uint32_t charge=p->tot_len>header?p->tot_len-header:1;
        KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(socket));
        if (!udp_receive_fits_locked(socket,charge)) {
            UDP_STATS_INC(udp.memerr);
            return 0;
        }
    }
    return 1;
}
static int socket_tcp_ingress_filter(struct tcp_pcb *pcb,struct pbuf *p,struct netif *input,
    int ipv6,const void *network,uint16_t network_length,uint16_t header)
{
    return socket_ingress_filter(tcp_filter_socket(pcb),p,input,ipv6,network,network_length,header);
}

/* Connection lifetime and record boundaries are independent properties. */
static int socket_connection_oriented(const struct kernel_socket *socket)
{ return socket->type == SOCKET_STREAM || socket->type == SOCKET_SEQPACKET; }
int kernel_socket_preserves_messages(const struct kernel_socket *socket)
{ return socket && (socket->type == SOCKET_DGRAM || socket->type == SOCKET_SEQPACKET ||
                    socket->type == SOCKET_RAW ||
                    socket->domain == KERNEL_SOCKET_DOMAIN_NETLINK); }
int kernel_socket_is_stream(const struct kernel_socket *socket)
{ return socket && socket->type == SOCKET_STREAM; }
int kernel_socket_is_seqpacket(const struct kernel_socket *socket)
{ return socket && socket->type == SOCKET_SEQPACKET; }
int kernel_socket_is_udp(const struct kernel_socket *socket)
{ return socket && socket->domain == KERNEL_SOCKET_DOMAIN_INET && socket->type == SOCKET_DGRAM; }
static int udp_has_pending(const struct kernel_socket *socket)
{ return __atomic_load_n(&socket->udp_pending,__ATOMIC_RELAXED)!=NULL; }
static int socket_prepare_send_locked(struct kernel_socket *socket)
{
    if(!kernel_socket_is_seqpacket(socket))return 0;
    int result=socket->pending_error;
    if(result)socket->pending_error=0;
    else if(!socket->connected)result=-KERNEL_ENOTCONN;
    return result;
}
int kernel_socket_prepare_send(struct kernel_socket *socket)
{
    if(!kernel_socket_is_seqpacket(socket))return 0;
    SOCKET_STATE(state);
    return socket_prepare_send_locked(socket);
}
int kernel_socket_prepare_receive(struct kernel_socket *socket)
{
    if(!kernel_socket_is_seqpacket(socket))return 0;
    SOCKET_STATE(state);
    return socket->connected?0:-KERNEL_ENOTCONN;
}

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

enum socket_address_use { SOCKET_ADDRESS_CONNECT, SOCKET_ADDRESS_BIND, SOCKET_ADDRESS_SEND };
static int address_import(const struct kernel_socket *socket,
                          const struct kernel_socket_address *address,
                          ip_addr_t *ip, enum socket_address_use use)
{
    int binding=use==SOCKET_ADDRESS_BIND;
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
                uint32_t index=address->scope ? address->scope : socket->bound_device;
                if (use==SOCKET_ADDRESS_SEND && socket->bound_device)index=socket->bound_device;
                if (use==SOCKET_ADDRESS_CONNECT && address->scope && socket->bound_device &&
                    address->scope!=socket->bound_device)return -KERNEL_EINVAL;
                if (use!=SOCKET_ADDRESS_SEND && ip6_addr_islinklocal(ip_2_ip6(ip)) && !index)
                    return -KERNEL_EINVAL;
                /* SEND preparation runs on the caller with only its endpoint
                 * lock. Keep this a value snapshot; owner-side routing checks
                 * interface existence after copying, under protocol ownership. */
                if (index>255U || (use!=SOCKET_ADDRESS_SEND && index && !netif_get_by_index((u8_t)index)))
                    return binding?-KERNEL_ENODEV:-KERNEL_ENETUNREACH;
                ip6_addr_set_zone(ip_2_ip6(ip), (u8_t)index);
            }
            if (binding && !socket->v6only && ip_addr_isany(ip))
                IP_SET_TYPE(ip, IPADDR_TYPE_ANY);
        }
    }
    return 0;
}

static struct kernel_socket *inet_sockets;
/* The CPU0 protocol owner enumerates this registry on bind, device failure
 * and stop. Unlink at final close uses the actual incoming registry link. */
static void inet_register(struct kernel_socket *socket)
{
    socket->inet_next=inet_sockets;
    socket->inet_previous=&inet_sockets;
    if (inet_sockets) inet_sockets->inet_previous=&socket->inet_next;
    inet_sockets=socket;
}
static uint8_t socket_initialized;
static uint8_t socket_timer_running;
static uint8_t socket_device_failed;
static uint8_t network_stopping;
static inline int network_is_stopping(void)
{ return __atomic_load_n(&network_stopping,__ATOMIC_RELAXED); }
static void (*network_timer_wake)(void *);
static struct kernel_wait_queue *network_work_queue;
static int (*network_udp_capacity)(void *);
static unsigned (*network_receive_inline)(void *);
static void *network_context;
static struct kernel_socket *write_retry_head;

enum { SOCKET_WORK_OUTPUT = 1, SOCKET_WORK_RECEIVE = 2, SOCKET_WORK_REAP = 4,
       SOCKET_WORK_LISTENER = 8, SOCKET_WORK_PROMOTE = 16, SOCKET_WORK_DEVICE_FAILED = 32,
       SOCKET_WORK_PUSH = 64 };
struct socket_work_queue { struct kernel_socket *head, *tail; unsigned slot; };
static struct socket_work_queue ready_work, pool_wait = {.slot = 1}, nic_wait = {.slot = 2}, receive_wait = {.slot = 3};
static uint64_t pool_generation, nic_generation, receive_generation;
/* Protected by socket_state_lock. The per-PCB marker belongs to its TCP
 * context. These masks carry readiness without holding a PCB pointer. */
static unsigned orphan_nic_wait, orphan_nic_ready;
#if BOAROS_USER_SMP
static unsigned pool_has_waiters, receive_has_waiters;
#endif
static unsigned loopback_pending, nic_blocked, protocol_depth, service_cursor, capacity_cursor;
#if BOAROS_COST_DIAGNOSTICS
static unsigned polling_snapshot[BOAROS_MAX_CPUS];
static inline unsigned *socket_poll_depth(void)
{
#if BOAROS_MAX_CPUS > 1
    return &polling_snapshot[kernel_cpu_current()->logical_id];
#else
    return polling_snapshot;
#endif
}
#endif
static struct kernel_io_context *protocol_owner;

static struct kernel_mutex protocol_mutex;
static unsigned protocol_mutex_initialized;
static struct kernel_io_context *protocol_current_owner(void);

#if BOAROS_USER_SMP
/* TCP packets enter this ring after IP has checked and removed its header.
 * Ownership of the pbuf (and any DMA loan) transfers with the ring entry. */
#define TCP_RX_RING 64U
#define TCP_RX_BATCH 4U
#define TCP_ROUTE_BUCKETS 64U
/* Two ordinary loopback connections can share one context. Spread new
 * work once there are already four active endpoints on its preferred CPU. */
#define TCP_OWNER_SPREAD_AT 4U
struct socket_tcp_input {
    struct pbuf *packet;
    struct netif *interface;
    ip_addr_t src, dst;
    const void *network;
    uint16_t network_length;
    uint8_t route_bucket;
#if BOAROS_COST_DIAGNOSTICS
    uint64_t queued_ticks;
#endif
};
struct socket_tcp_owner {
    struct kernel_mutex mutex;
    struct boaros_lwip_tcp_context context;
    /* Locally qualified lookup uses the same bucket number as dispatch. */
    struct tcp_pcb *input_buckets[TCP_ROUTE_BUCKETS];
    struct kernel_thread_join worker;
    struct kernel_wait_queue work;
    struct socket_work_queue ready;
    /* Ring producers and consumers do not share the endpoint lock. The
     * worker joins both locks when it prepares to sleep. */
    struct kernel_raw_lock ingress;
    struct socket_tcp_input ring[TCP_RX_RING];
    struct kernel_socket **ack_waiter;
    unsigned index, head, count, depth, nic_blocked, timewait_count;
    uint8_t timer_running, stopping, output_reserved, waiting;
};
static struct socket_tcp_owner tcp_owners[BOAROS_MAX_CPUS-1];
static unsigned tcp_owner_count;
/* lwIP owns the built-in lo for the entire boot. Its mutable eligibility and
 * the legacy queue's publication are serialized with direct ring admission. */
static struct netif *fast_loopback;
enum { LOOPBACK_AVAILABLE=1, LOOPBACK_LEGACY=2, LOOPBACK_GENERATION=4 };
static uint64_t fast_loopback_state;
static struct tcp_pcb *tcp_routes[TCP_ROUTE_BUCKETS];
static unsigned tcp_owner_active[BOAROS_MAX_CPUS];
static struct kernel_raw_lock tcp_route_lock = {
    .rank=KERNEL_RAW_RANK_OBJECT, .initialized=KERNEL_RAW_INITIALIZED
};

static struct socket_tcp_owner *current_tcp_owner(void)
{ return kernel_io_context_inline()->socket_tcp_owner; }

static struct socket_tcp_owner *tcp_owner_at(unsigned cpu)
{
    if (!cpu || cpu >= BOAROS_MAX_CPUS) __builtin_trap();
    return &tcp_owners[cpu-1];
}

static unsigned tcp_socket_cpu(const struct kernel_socket *socket)
{
    /* A lock on the selected context is required before touching its PCB.
     * Queue callers already hold the lock used to publish the CPU number. */
    return __atomic_load_n(&socket->tcp_cpu,__ATOMIC_RELAXED);
}

static int tcp_owner_take_waiting(struct socket_tcp_owner *owner)
{
    /* Both producers synchronize with sleep registration, each under its own
     * lock. Exactly one consumes the shared notification hint. */
    return __atomic_load_n(&owner->waiting,__ATOMIC_RELAXED) &&
        __atomic_exchange_n(&owner->waiting,0,__ATOMIC_RELAXED);
}
static int tcp_owner_stopping(const struct socket_tcp_owner *owner)
{ return __atomic_load_n(&owner->stopping,__ATOMIC_RELAXED); }

static int tcp_owner_lock(struct kernel_socket_protocol_scope *scope,
                          struct socket_tcp_owner *owner,int try_only)
{
    struct kernel_io_context *io=kernel_io_context_inline();
    *scope=(struct kernel_socket_protocol_scope){.tcp_owner=owner};
    if (io->socket_tcp_owner==owner) {
        owner->depth++;
        return 1;
    }
    if (io->socket_tcp_owner) __builtin_trap();
    scope->interrupts=arch_interrupt_save();
    if (try_only) {
        if (!kernel_mutex_try_lock(&owner->mutex,&scope->guard)) {
            arch_interrupt_restore(scope->interrupts);
            return 0;
        }
    } else {
        COST_SCOPE(acquire, NETWORK_TCP_LOCK_TICKS);
        kernel_mutex_lock(&owner->mutex,&scope->guard);
    }
    if (owner->depth || io->allocation_depth==UINT32_MAX) __builtin_trap();
    owner->depth=1;
    io->socket_tcp_owner=owner;
    io->allocation_depth++;
    scope->outer=1;
    kernel_assert_can_block();
    arch_interrupt_restore(ARCH_INTERRUPT_ENABLE_MASK);
    return 1;
}
#endif

static struct kernel_io_context *protocol_current_owner(void)
{
    return __atomic_load_n(&protocol_owner, __ATOMIC_RELAXED);
}

static inline int protocol_lock(struct kernel_socket_protocol_scope *scope, int try_only)
{
    struct kernel_io_context *owner = kernel_io_context_current();
    if (protocol_current_owner() == owner) {
        if (!protocol_depth || protocol_depth == UINT32_MAX) __builtin_trap();
        /* 外层mutex随任务保活；抢占者不能改变深度，嵌套借用保留当前IRQ状态。 */
        scope->outer = 0;
        protocol_depth++;
        return 1;
    }
    scope->interrupts=arch_interrupt_save();
    scope->guard=(struct kernel_lock_guard){0};
    if (!__atomic_load_n(&protocol_mutex_initialized, __ATOMIC_RELAXED)) {
        /* 首次入口在 CPU0；远端只在看见发布后进入。 */
        kernel_mutex_init(&protocol_mutex, 25, (uintptr_t)&protocol_mutex);
#if BOAROS_USER_SMP
        boaros_lwip_ip_init(kernel_cpu_get_boot_count());
#endif
        __atomic_store_n(&protocol_mutex_initialized, 1, __ATOMIC_RELEASE);
    }
    /* 竞争者的 guard 留在自己的调用栈；mutex 可调度等待其他协议 owner。 */
    if (try_only) {
        if (!kernel_mutex_try_lock(&protocol_mutex, &scope->guard)) return 0;
    } else kernel_mutex_lock(&protocol_mutex, &scope->guard);
    if (protocol_depth || protocol_current_owner()) __builtin_trap();
    scope->outer = 1;
    __atomic_store_n(&protocol_owner, owner, __ATOMIC_RELAXED);
    if (owner->allocation_depth == UINT32_MAX) __builtin_trap();
    owner->allocation_depth++;
    protocol_depth = 1;
    /* 协议内部允许定时器抢占；任何 raw 临界区都不能越过这个入口。 */
    kernel_assert_can_block();
    arch_interrupt_restore(ARCH_INTERRUPT_ENABLE_MASK);
    return 1;
}

static int protocol_enter(struct kernel_socket_protocol_scope *scope, int try_only, int local_only)
{
#if BOAROS_USER_SMP
    scope->tcp_owner=0;
    scope->ip_guard.lock=0;
    scope->service=(struct kernel_service_scope){0};
    /* Loopback packet taps can borrow a caller's UDP submission scope. */
    if (protocol_current_owner() == kernel_io_context_current()) return protocol_lock(scope, try_only);
    /* With only CPU 0 online, the protocol mutex suffices to keep the
     * caller in the right execution context across a preemption. */
    if (kernel_cpu_get_boot_count() > 1 && !kernel_service_scope_try_local(&scope->service)) {
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
    int entered=protocol_lock(scope,try_only);
    if (!entered) {
#if BOAROS_USER_SMP
        kernel_service_scope_end(&scope->service);
#endif
        arch_interrupt_restore(scope->interrupts);
    }
#if BOAROS_USER_SMP
    else if (scope->outer && boaros_lwip_parallel_enabled()) boaros_lwip_ip_lock(&scope->ip_guard);
#endif
    return entered;
}
void kernel_socket_protocol_enter(struct kernel_socket_protocol_scope *scope)
{ (void)protocol_enter(scope, 0, 0); }
void kernel_socket_protocol_leave(struct kernel_socket_protocol_scope *scope)
{
#if BOAROS_USER_SMP
    if (scope->tcp_owner) {
        struct socket_tcp_owner *owner=scope->tcp_owner;
        struct kernel_io_context *io=kernel_io_context_inline();
        if (io->socket_tcp_owner!=owner || !owner->depth) __builtin_trap();
        if (--owner->depth) return;
        if (!scope->outer || !io->allocation_depth) __builtin_trap();
        (void)arch_interrupt_save();
        io->socket_tcp_owner=0;
        io->allocation_depth--;
        kernel_lock_release(&scope->guard);
        arch_interrupt_restore(scope->interrupts);
        return;
    }
    if (scope->outer && scope->ip_guard.lock) boaros_lwip_ip_unlock(&scope->ip_guard);
#endif
    if (!scope->outer) {
        if (protocol_depth <= 1 || protocol_current_owner() != kernel_io_context_current()) __builtin_trap();
        protocol_depth--;
        return;
    }
    (void)arch_interrupt_save();
    struct kernel_io_context *owner=protocol_current_owner();
    if (protocol_depth != 1 || owner != kernel_io_context_current() ||
        !owner->allocation_depth) __builtin_trap();
    protocol_depth = 0;
    owner->allocation_depth--;
    __atomic_store_n(&protocol_owner, 0, __ATOMIC_RELAXED);
    kernel_lock_release(&scope->guard);
#if BOAROS_USER_SMP
    /* 先归还协议owner，再允许恢复用户CPU资格；不能带mutex等待迁移。 */
    kernel_service_scope_end(&scope->service);
#endif
    arch_interrupt_restore(scope->interrupts);
}
void kernel_socket_pbuf_free(struct pbuf *payload)
{
#if BOAROS_USER_SMP
    if (boaros_lwip_parallel_enabled()) {
        pbuf_free(payload);
        return;
    }
#endif
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    pbuf_free(payload);
    kernel_socket_protocol_leave(&irq);
}

void kernel_socket_pbuf_free_batch(void *const *payloads, unsigned count)
{
    struct kernel_socket_protocol_scope scope;
    int serial = !boaros_lwip_parallel_enabled();
    if (serial) kernel_socket_protocol_enter(&scope);
    for (unsigned i = 0; i < count; i++) pbuf_free(payloads[i]);
    if (serial) kernel_socket_protocol_leave(&scope);
}
/* The protocol owner exclusively edits the chain. Admission only samples
 * whether a destination has already been selected for the datagram. */
static struct pbuf *udp_pending_take(struct kernel_socket *socket)
{
    struct socket_udp_pending *pending=socket->udp_pending;
    if (!pending) return NULL;
    __atomic_store_n(&socket->udp_pending,NULL,__ATOMIC_RELAXED);
    struct pbuf *payload=pending->head;
    kernel_heap_release(socket->heap,pending);
    return payload;
}

static void udp_pending_discard(struct kernel_socket *socket)
{
    struct pbuf *payload=udp_pending_take(socket);
    if (payload) kernel_socket_pbuf_free(payload);
}
static void socket_protocol_enter(struct kernel_socket_protocol_scope *scope,
                                  const struct kernel_socket *socket)
{
#if BOAROS_USER_SMP
    if (!socket) { kernel_socket_protocol_enter(scope); return; }
    for (;;) {
        unsigned cpu=tcp_socket_cpu(socket);
        if (cpu) (void)tcp_owner_lock(scope,tcp_owner_at(cpu),0);
        else kernel_socket_protocol_enter(scope);
        if (tcp_socket_cpu(socket)==cpu) return;
        kernel_socket_protocol_leave(scope);
    }
#else
    (void)socket;
    kernel_socket_protocol_enter(scope);
#endif
}
#if BOAROS_USER_SMP
static void tcp_control_enter(struct kernel_socket_protocol_scope *base,
                              struct kernel_socket_protocol_scope *remote,unsigned cpu)
{
    if (!base->outer) __builtin_trap();
    boaros_lwip_ip_unlock(&base->ip_guard);
    (void)tcp_owner_lock(remote,tcp_owner_at(cpu),0);
}

static void tcp_control_leave(struct kernel_socket_protocol_scope *base,
                              struct kernel_socket_protocol_scope *remote)
{
    kernel_socket_protocol_leave(remote);
    boaros_lwip_ip_lock(&base->ip_guard);
}
#endif
static void socket_work_ready_locked(struct socket_state_guard *state)
{
    if(!state->work_prepared && network_work_queue) {
        state->work_prepared=1;
        (void)kernel_wait_cursor_prepare(network_work_queue,&state->work);
    }
}
static int caller_owns_protocol(void)
{
#if BOAROS_USER_SMP
    if (current_tcp_owner()) return 1;
#endif
    return protocol_current_owner()==kernel_io_context_current() && protocol_depth;
}

static unsigned orphan_nic_context(void)
{
#if BOAROS_USER_SMP
    struct socket_tcp_owner *owner = current_tcp_owner();
    if (owner) return owner->index;
#endif
    return 0;
}

static void orphan_nic_notify_locked(struct socket_state_guard *state, unsigned mask)
{
    if (mask & 1U) socket_work_ready_locked(state);
#if BOAROS_USER_SMP
    for (unsigned cpu=1; mask>>cpu; cpu++) {
        if (!(mask & (1U<<cpu))) continue;
        struct socket_tcp_owner *owner = tcp_owner_at(cpu);
        if (!tcp_owner_stopping(owner) && tcp_owner_take_waiting(owner))
            socket_state_notify(state,&owner->work);
    }
#endif
}

static void orphan_nic_register_locked(struct socket_state_guard *state,
    struct tcp_pcb *pcb, uint64_t generation)
{
    if (!pcb || network_stopping || socket_device_failed) return;
    unsigned mask = 1U << orphan_nic_context();
    pcb->boaros_nic_retry = 1;
    orphan_nic_wait |= mask;
    if (generation != nic_generation) {
        orphan_nic_ready |= mask;
        orphan_nic_notify_locked(state,mask);
    }
}

static struct socket_work_queue *socket_ready_queue(const struct kernel_socket *socket)
{
#if BOAROS_USER_SMP
    unsigned cpu=tcp_socket_cpu(socket);
    if (cpu) return &tcp_owner_at(cpu)->ready;
#else
    (void)socket;
#endif
    return &ready_work;
}

static void work_remove_locked(struct socket_work_queue *queue, struct kernel_socket *socket)
{
    struct socket_link *link = &socket->work_links[queue->slot];
    if (!link->queued) return;
    if (link->previous) link->previous->work_links[queue->slot].next = link->next;
    else queue->head = link->next;
    if (link->next) link->next->work_links[queue->slot].previous = link->previous;
    else queue->tail = link->previous;
    *link = (struct socket_link){0};
#if BOAROS_USER_SMP
    if (queue==&pool_wait && !queue->head) __atomic_store_n(&pool_has_waiters,0,__ATOMIC_RELEASE);
    if (queue==&receive_wait && !queue->head) __atomic_store_n(&receive_has_waiters,0,__ATOMIC_RELEASE);
#endif
}
/* Caller holds the shared work-list lock through membership publication. */
static void work_add_locked(struct socket_work_queue *queue, struct kernel_socket *socket, uint64_t epoch)
{
    if (socket->destroying || socket->domain != KERNEL_SOCKET_DOMAIN_INET) return;
    struct socket_link *link = &socket->work_links[queue->slot];
    if (link->queued) return;
    *link = (struct socket_link){.previous = queue->tail, .queued = 1, .epoch = epoch};
    if (queue->tail) queue->tail->work_links[queue->slot].next = socket;
    else queue->head = socket;
    queue->tail = socket;
#if BOAROS_USER_SMP
    if (queue==&pool_wait) __atomic_store_n(&pool_has_waiters,1,__ATOMIC_RELEASE);
    if (queue==&receive_wait) __atomic_store_n(&receive_has_waiters,1,__ATOMIC_RELEASE);
#endif
}
static void socket_schedule_locked(struct kernel_socket *socket,unsigned flags,
                                   struct socket_state_guard *notifications)
{
    if (socket->destroying || socket->domain != KERNEL_SOCKET_DOMAIN_INET) return;
#if BOAROS_USER_SMP
    unsigned cpu=tcp_socket_cpu(socket);
    if (cpu && tcp_owner_stopping(tcp_owner_at(cpu))) return;
#endif
    socket->work_flags |= flags;
#if BOAROS_USER_SMP
    if (socket->udp_output_active) return;
#endif
    struct socket_work_queue *queue=socket_ready_queue(socket);
    unsigned queued = socket->work_links[queue->slot].queued;
    work_add_locked(queue, socket, 0);
    if (!queued) {
#if BOAROS_USER_SMP
        if (cpu) {
            struct socket_tcp_owner *owner=tcp_owner_at(cpu);
            if (tcp_owner_take_waiting(owner)) socket_state_notify(notifications,&owner->work);
            return;
        }
#endif
        socket_work_ready_locked(notifications);
    }
}
static void socket_schedule(struct kernel_socket *socket, unsigned flags)
{
    SOCKET_STATE(state);
    socket_schedule_locked(socket,flags,state.owner);
}
#if BOAROS_USER_SMP
static int udp4_output_socket(const struct kernel_socket *socket)
{
    return socket->family == KERNEL_SOCKET_AF_INET && socket->type == SOCKET_DGRAM;
}

static void udp4_forget_path(struct kernel_socket *socket)
{
    if (!udp4_output_socket(socket) || !boaros_lwip_parallel_enabled()) return;
    KERNEL_IRQ_LOCK_SCOPE(state,&socket_state_lock);
    socket->udp->boaros_path4.ip.generation = 0;
}

static int udp4_begin_output_locked(struct kernel_socket_write_request *request)
{
    /* The same qualification covers queued, synchronous and resolved sends.
     * Admission must not pass a request already published on the work list. */
    struct kernel_socket *socket=request->socket;
    if (socket->udp_output_active || socket->writes_head) return 0;
    socket->udp_output_active=1;
    request->udp_output_owned=1;
    return 1;
}

static void udp4_end_output_locked(struct kernel_socket_write_request *request,
    struct socket_state_guard *state)
{
    struct kernel_socket *socket=request->socket;
    socket->udp_output_active=0;
    request->udp_output_owned=0;
    if (socket->writes_head || socket->work_flags) socket_schedule_locked(socket,0,state);
}

static void udp4_end_output(struct kernel_socket_write_request *request)
{
    SOCKET_STATE(state);
    udp4_end_output_locked(request,state.owner);
}
#endif

static void socket_pool_wait(struct kernel_socket *socket)
{
    SOCKET_STATE(state);
    work_remove_locked(&pool_wait, socket);
    work_add_locked(&pool_wait,socket,__atomic_load_n(&pool_generation,__ATOMIC_RELAXED));
}
static void receive_capacity(void)
{
#if BOAROS_USER_SMP
    if (!__atomic_load_n(&receive_has_waiters,__ATOMIC_ACQUIRE)) return;
#endif
    SOCKET_STATE(irq);
    if (receive_wait.head) {
        receive_generation++;
        socket_work_ready_locked(irq.owner);
    }
}
static void protocol_work_ready(void)
{
    /* loopback队列只有协议owner生产/消费；已有待处理包时不重复通知。 */
    if(loopback_pending)return;
    loopback_pending=1;
    SOCKET_STATE(state);socket_work_ready_locked(state.owner);
}
static void protocol_capacity(int pool)
{
    if (pool != -1 && pool != MEMP_TCP_SEG && pool != MEMP_PBUF && pool != MEMP_PBUF_POOL) return;
    /* 协议池及其等待链归协议 owner；只在实际有等待者时触碰
     * 跨核端点状态与worker通知存储，真实归还仍逐次推进代次。 */
#if BOAROS_USER_SMP
    if (boaros_lwip_parallel_enabled()) {
        __atomic_fetch_add(&pool_generation,1,__ATOMIC_RELAXED);
        if (!__atomic_load_n(&pool_has_waiters,__ATOMIC_ACQUIRE)) return;
        SOCKET_STATE(state);
        if (pool_wait.head) socket_work_ready_locked(state.owner);
        return;
    }
#endif
    pool_generation++;
    if (pool_wait.head) { SOCKET_STATE(state);socket_work_ready_locked(state.owner); }
}
static void service_inline(struct kernel_socket_protocol_scope *scope)
{
    /* raw API已返回但outer资格尚未交接；同步回调借用scope时不重入服务。 */
    if (scope->outer
#if BOAROS_USER_SMP
        && !scope->tcp_owner
#endif
        )
        (void)kernel_socket_service_owned(scope, (struct kernel_socket_service_budget){8, 8, 1});
}

/* The list itself owns no socket reference.
 * A socket must remove its entry before its OFD frees it.  Entries stay
 * ordered by write_retry_ms so the earliest retry is the head. */
static void write_retry_disarm(struct kernel_socket *socket)
{
    /* Only this PCB's protocol owner arms a retry. A released pool wait or
     * deadline can clear the flag concurrently; recheck it under the list lock. */
    if (!__atomic_load_n(&socket->write_blocked,__ATOMIC_RELAXED)) return;
#if BOAROS_USER_SMP
    struct socket_state_guard state __attribute__((cleanup(socket_state_release)))={.owner=0};
    if (boaros_lwip_parallel_enabled()) socket_state_acquire(&state);
#endif
    struct kernel_socket **link;
    if (!socket->write_blocked) return;
    for (link = &write_retry_head; *link != 0;
         link = &(*link)->write_retry_next) {
        if (*link == socket) {
            *link = socket->write_retry_next;
            socket->write_retry_next = 0;
            __atomic_store_n(&socket->write_blocked,0,__ATOMIC_RELAXED);
            return;
        }
    }
    __builtin_trap();
}

static void write_retry_arm(struct kernel_socket *socket)
{
#if BOAROS_USER_SMP
    struct socket_state_guard state __attribute__((cleanup(socket_state_release)))={.owner=0};
    if (boaros_lwip_parallel_enabled()) socket_state_acquire(&state);
#endif
    write_retry_disarm(socket);
    socket->write_retry_ms = sys_now() + SOCKET_WRITE_RETRY_MS;
    struct kernel_socket **link = &write_retry_head;
    while (*link != 0 &&
           (int32_t)((*link)->write_retry_ms - socket->write_retry_ms) <= 0)
        link = &(*link)->write_retry_next;
    socket->write_retry_next = *link;
    *link = socket;
    __atomic_store_n(&socket->write_blocked,1,__ATOMIC_RELAXED);
#if BOAROS_USER_SMP
    if (link==&write_retry_head && tcp_socket_cpu(socket)) socket_work_ready_locked(state.owner);
#endif
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
static uint32_t tcp_admission_capacity_locked(const struct kernel_socket *socket);

static void wake_socket(struct kernel_socket *socket)
{
    struct socket_state_guard *state=socket_state_owner(socket_endpoint_lock(socket));
    if(socket->domain==KERNEL_SOCKET_DOMAIN_INET && socket->type==SOCKET_STREAM &&
       caller_owns_protocol()) {
#if BOAROS_USER_SMP
        /* CPU0 can wake shared-pool waiters; only the socket's own
         * protocol context may refresh capacity from its PCB. */
        unsigned cpu=tcp_socket_cpu(socket);
        if (current_tcp_owner()==(cpu?tcp_owner_at(cpu):NULL))
#endif
            tcp_publish_capacity_locked(socket);
    }
    socket_state_notify(state,&socket->wait);
}
static void socket_async(struct kernel_socket *socket,unsigned reason)
{ socket_state_async(socket_state_owner(socket_endpoint_lock(socket)),&socket->async_events,reason); }
static void socket_async_close(struct kernel_socket *socket)
{
    if (kernel_wait_queue_close(&socket->async_events)) __builtin_trap();
    for (;;) {
        enum kernel_scheduler_status status=kernel_wait_queue_destroy(&socket->async_events);
        if (!status) break;
        if (status!=KERNEL_SCHEDULER_STATUS_BUSY) __builtin_trap();
        kernel_wait_backend_quiesce();
    }
}
/* UNIX reference edges share one sleepable lock. Packet ownership, UID quota
 * and cleanup owners use socket state, including controls without graph edges.
 * No OFD final put or user access occurs in either lock domain. */
static struct kernel_mutex rights_mutex;
static unsigned rights_initialized, rights_dirty, rights_active, rights_retry_pending;
static unsigned rights_releasing;
static uint64_t rights_unix_inflight, rights_queued;
static uint32_t rights_retry_ms;
static struct kernel_socket *unix_sockets;
static struct kernel_socket_control *rights_retired;
static void rights_acquire(struct kernel_lock_guard *guard)
{
    { KERNEL_IRQ_LOCK_SCOPE(state,&socket_state_lock);
      if(!rights_initialized) {kernel_mutex_init(&rights_mutex,11,(uintptr_t)&rights_mutex);rights_initialized=1;} }
    kernel_mutex_lock(&rights_mutex,guard);
}
static void rights_activity_update(void)
{
    SOCKET_STATE(state);
    unsigned active=rights_unix_inflight!=0 || rights_retired!=NULL || rights_releasing;
    __atomic_store_n(&rights_active,active!=0,__ATOMIC_RELEASE);
    if(!active)rights_dirty=0;
}
void kernel_socket_rights_changed(void)
{
    if(!__atomic_load_n(&rights_unix_inflight,__ATOMIC_ACQUIRE))return;
    SOCKET_STATE(state);
    if(!rights_dirty) {rights_dirty=1;socket_work_ready_locked(state.owner);}
}
int kernel_socket_control_allocate(struct kernel_heap *heap,uint32_t capacity,
    struct kernel_socket_control **owner)
{
    if(*owner || capacity>KERNEL_SOCKET_SCM_MAX_FD)return -KERNEL_EINVAL;
    KERNEL_NO_RECLAIM_IO;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(heap,1,
        sizeof(**owner)+capacity*sizeof((*owner)->files[0]),(void **)owner);
    if(status==KERNEL_HEAP_STATUS_EMPTY)return -KERNEL_ENOMEM;
    if(status!=KERNEL_HEAP_STATUS_OK)__builtin_trap();
    (*owner)->heap=heap;(*owner)->capacity=capacity;return 0;
}
static int control_release_files(struct kernel_socket_control *control)
{
    int pending=0;
    for(unsigned i=0;i<control->count;i++)if(control->files[i]) {
        enum kernel_open_file_status status=kernel_open_file_release(&control->files[i]);
        if(status==KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED)pending=1;
        else if(status!=KERNEL_OPEN_FILE_STATUS_OK)__builtin_trap();
    }
    return pending;
}
static void control_free(struct kernel_socket_control *control)
{
    if(control->pid)kernel_task_identity_put(control->pid);
    if(control->account)kernel_uid_account_put(control->account);
    kernel_heap_release(control->heap,control);
}
void kernel_socket_control_release(struct kernel_socket_control **owner)
{
    struct kernel_socket_control *control=*owner;if(!control)return;
    if(control->inflight || control->receiver_previous)__builtin_trap();
    unsigned first=0;
    while(first<control->count && !control->files[first])first++;
    if(first==control->count) {*owner=NULL;control_free(control);return;}
    /* The last put may sleep outside the graph lock. Shutdown must still see
     * that real cleanup owner until it is freed or returned to the retry list. */
    {SOCKET_STATE(state);
     if(rights_releasing==UINT32_MAX)__builtin_trap();
     rights_releasing++;rights_activity_update();}
    *owner=NULL;
    int pending=control_release_files(control);
    if(!pending)control_free(control);
    SOCKET_STATE(state);
    if(!rights_releasing)__builtin_trap();
    rights_releasing--;
    if(pending) {
        control->next=rights_retired;rights_retired=control;
        rights_retry_pending=1;rights_retry_ms=sys_now()+SOCKET_WRITE_RETRY_MS;socket_work_ready_locked(state.owner);
    }
    rights_activity_update();
}
static void control_unqueue(struct kernel_socket_control *control)
{
    if(!control || !control->inflight)return;
    SOCKET_STATE(state);
    if(!control->account || control->account->unix_inflight<control->count || !rights_queued)__builtin_trap();
    control->account->unix_inflight-=control->count;rights_queued--;control->inflight=0;
    if(!control->unix_count)return;
    if(!control->receiver_previous || *control->receiver_previous!=control)__builtin_trap();
    *control->receiver_previous=control->receiver_next;
    if(control->receiver_next)control->receiver_next->receiver_previous=control->receiver_previous;
    control->receiver_next=NULL;control->receiver_previous=NULL;control->receiver=NULL;control->packet=NULL;
    for(unsigned i=0;i<control->count;i++) {
        struct kernel_socket *socket=kernel_open_file_socket(control->files[i]);
        if(!kernel_socket_is_unix(socket))continue;
        if(!socket->gc_inflight)__builtin_trap();
        socket->gc_inflight--;
    }
    if(rights_unix_inflight<control->unix_count)__builtin_trap();
    __atomic_store_n(&rights_unix_inflight,rights_unix_inflight-control->unix_count,__ATOMIC_RELEASE);
    rights_activity_update();kernel_socket_rights_changed();
}
static void control_queue(struct kernel_socket_control *control,struct kernel_socket *receiver,struct socket_packet *packet)
{
    if(!control || !control->count)return;
    socket_state_owner(&socket_state_lock);
    if(control->inflight || !control->account || rights_queued==UINT64_MAX ||
        control->account->unix_inflight>UINT64_MAX-control->count)__builtin_trap();
    control->inflight=1;rights_queued++;control->account->unix_inflight+=control->count;
    if(!control->unix_count)return;
    control->receiver=receiver;control->packet=packet;
    control->receiver_next=receiver->rights_head;control->receiver_previous=&receiver->rights_head;
    if(receiver->rights_head)receiver->rights_head->receiver_previous=&control->receiver_next;
    receiver->rights_head=control;
    for(unsigned i=0;i<control->count;i++) {
        struct kernel_socket *socket=kernel_open_file_socket(control->files[i]);
        if(kernel_socket_is_unix(socket))socket->gc_inflight++;
    }
    __atomic_store_n(&rights_unix_inflight,rights_unix_inflight+control->unix_count,__ATOMIC_RELEASE);
    rights_activity_update();kernel_socket_rights_changed();
}
static int control_clone(struct kernel_socket_control *source,struct kernel_socket_control **owner,int rights)
{
    if(!source)return 0;
    int result=kernel_socket_control_allocate(source->heap,rights?source->count:0,owner);if(result)return result;
    struct kernel_socket_control *copy=*owner;
    copy->uid=source->uid;copy->gid=source->gid;copy->has_credentials=source->has_credentials;
    if(source->pid) {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);kernel_pid_get(source->pid);copy->pid=source->pid;}
    for(unsigned i=0;rights && i<source->count;i++) {
        if(kernel_open_file_acquire(source->files[i])!=KERNEL_OPEN_FILE_STATUS_OK)__builtin_trap();
        copy->files[copy->count++]=source->files[i];
    }
    return 0;
}
void kernel_socket_attach_file(struct kernel_socket *socket,struct kernel_open_file_description *file)
{
    if(!kernel_socket_is_unix(socket))return;
    KERNEL_LOCK_SCOPE(guard);rights_acquire(&guard);
    SOCKET_STATE(state);
    if(socket->gc_file && socket->gc_file!=file)__builtin_trap();
    socket->gc_file=file;
}
static int control_prepare(struct kernel_socket *socket,int credentials,
    struct kernel_socket_control **owner)
{
    struct kernel_socket_control *control=*owner;
    if(!control && !credentials)return 0;
    if(!control) {int error=kernel_socket_control_allocate(socket->heap,0,owner);if(error)return error;control=*owner;}
    const struct kernel_credentials *cred=kernel_credentials_current();
    if(credentials && !control->has_credentials) {
        int error=kernel_task_identity_get(0,&control->pid);if(error)return error;
        control->uid=cred->uid;control->gid=cred->gid;control->has_credentials=1;
    }
    control->unix_count=0;
    for(unsigned i=0;i<control->count;i++)
        if(kernel_socket_is_unix(kernel_open_file_socket(control->files[i])))control->unix_count++;
    if(control->count && !control->account) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        control->account=kernel_uid_account_get_locked(cred->account);
    }
    return 0;
}
static int control_limit(struct kernel_socket_control *control)
{
    if(!control || !control->count)return 0;
    const struct kernel_credentials *cred=kernel_credentials_current();
    if(kernel_credentials_capable(cred,KERNEL_CAP_SYS_RESOURCE) || kernel_credentials_capable(cred,KERNEL_CAP_SYS_ADMIN))return 0;
    struct kernel_rlimit64 limit;
    if(kernel_task_get_rlimit(kernel_task_current(),KERNEL_RLIMIT_NOFILE,&limit)!=KERNEL_TASK_STATUS_OK)__builtin_trap();
    return control->account->unix_inflight>limit.current?-KERNEL_ETOOMANYREFS:0;
}
static void rights_mark(struct kernel_socket *socket,struct kernel_socket **work)
{
    if(!socket || socket->gc_mark)return;
    socket->gc_mark=1;socket->gc_work=*work;*work=socket;
}
void kernel_socket_rights_service(void)
{
    if(!__atomic_load_n(&rights_active,__ATOMIC_ACQUIRE))return;
    struct kernel_socket_control *discard=NULL,*retired=NULL;
    KERNEL_LOCK_SCOPE(guard);
    int collect;
    unsigned releasing;
retry_graph:
    {SOCKET_STATE(state);
     if(rights_dirty && rights_unix_inflight && !guard.lock) {
         socket_state_release(&state);rights_acquire(&guard);goto retry_graph;
     }
     int retry=rights_retry_pending && (int32_t)(sys_now()-rights_retry_ms)>=0;
     collect=rights_dirty && rights_unix_inflight!=0;
     rights_dirty=0;
     if(!collect && !retry)return;
     if(retry) {
         retired=rights_retired;rights_retired=NULL;rights_retry_pending=0;
     }
     /* drain也用短锁：摘链前先登记整批owner，不能出现空链假成功。 */
     releasing=collect || retired!=NULL;
     if(releasing) {if(rights_releasing==UINT32_MAX)__builtin_trap();rights_releasing++;}
    }
    /* 图链只保存含UNIX引用的控制对象；普通FD由报文队列保持所有权。 */
    if(collect) {
        struct kernel_socket *work=NULL;
        for(struct kernel_socket *s=unix_sockets;s;s=s->gc_next)s->gc_mark=0;
        for(struct kernel_socket *s=unix_sockets;s;s=s->gc_next)
            if(s->gc_file && kernel_open_file_references(s->gc_file)>s->gc_inflight)rights_mark(s,&work);
        for(;;) {
            while(work) {
                struct kernel_socket *s=work;work=s->gc_work;s->gc_work=NULL;
                for(struct socket_pending *p=s->pending_head;p;p=p->next)rights_mark(p->child,&work);
                for(struct kernel_socket_control *c=s->rights_head;c;c=c->receiver_next)
                    for(unsigned i=0;i<c->count;i++) {
                        struct kernel_socket *target=kernel_open_file_socket(c->files[i]);
                        if(kernel_socket_is_unix(target))rights_mark(target,&work);
                    }
            }
            /* Acquiring a queued descriptor or accepting a child is serialized
             * here. Recheck external owners before detaching candidate edges. */
            for(struct kernel_socket *s=unix_sockets;s;s=s->gc_next)
                if(!s->gc_mark && s->gc_file && kernel_open_file_references(s->gc_file)>s->gc_inflight)
                    rights_mark(s,&work);
            if(!work)break;
        }
        for(struct kernel_socket *s=unix_sockets;s;s=s->gc_next)if(!s->gc_mark)
            while(s->rights_head) {
                struct kernel_socket_control *control=s->rights_head;
                {SOCKET_STATE(state);
                 struct socket_packet *packet=control->packet;
                 if(packet->control!=control)__builtin_trap();
                 packet->control=NULL;}
                control_unqueue(control);control->next=discard;discard=control;
            }
    }
    rights_activity_update();
    kernel_lock_scope_release(&guard);
    while(discard) {struct kernel_socket_control *next=discard->next;discard->next=NULL;kernel_socket_control_release(&discard);discard=next;}
    while(retired) {struct kernel_socket_control *next=retired->next;retired->next=NULL;kernel_socket_control_release(&retired);retired=next;}
    if(releasing) {
        SOCKET_STATE(state);
        if(rights_releasing<releasing)__builtin_trap();
        rights_releasing-=releasing;rights_activity_update();
    }
}

int kernel_socket_rights_drain(void)
{
    {SOCKET_STATE(state);
     if(!rights_queued && !rights_retired)return rights_releasing?-KERNEL_EBUSY:0;
     rights_dirty=rights_unix_inflight!=0;
     if(rights_retired) {rights_retry_pending=1;rights_retry_ms=sys_now();}}
    kernel_socket_rights_service();
    SOCKET_STATE(state);
    return rights_queued || rights_retired || rights_releasing?-KERNEL_EBUSY:0;
}

/* UNIX names are immutable; input views borrow the syscall snapshot, while
 * queued sources and exported addresses retain only this one stored name. */
static struct kernel_socket *unix_names[64];
static uint32_t unix_autoname;
static void unix_name_get_locked(struct kernel_socket_name *name)
{ if(name) { if(!name->references || name->references==UINT32_MAX)__builtin_trap();name->references++; } }
static int unix_name_put_locked(struct kernel_socket_name *name)
{
    if(!name)return 0;
    if(!name->references)__builtin_trap();
    return !--name->references;
}
static void unix_name_free(struct kernel_socket_name *name)
{
    if(!name)return;
    if(name->path && kernel_vfs_path_release(&name->path))__builtin_trap();
    kernel_heap_release(name->heap,name);
}
static void unix_name_put(struct kernel_socket_name *name)
{
    if(!name)return;
    KERNEL_IRQ_LOCK_SCOPE(state,&socket_state_lock);
    int release=unix_name_put_locked(name);
    kernel_irq_lock_release(&state);
    if(release)unix_name_free(name);
}
void kernel_socket_address_release(struct kernel_socket_address *address)
{
    if(address->family==KERNEL_SOCKET_AF_UNIX)unix_name_put(address->unix_owner);
    *address=(struct kernel_socket_address){0};
}
int kernel_socket_is_unix(const struct kernel_socket *socket)
{ return socket && socket->domain==KERNEL_SOCKET_DOMAIN_UNIX; }
int kernel_socket_is_netlink(const struct kernel_socket *socket)
{ return socket && socket->domain==KERNEL_SOCKET_DOMAIN_NETLINK; }
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
    if(release && (socket->destroying!=2 || socket->unix_packets || socket->unix_memory))__builtin_trap();
    return release;
}
static void unix_storage_free(struct kernel_socket *socket)
{
    unix_name_put(socket->unix_name);
    unix_cred_release(&socket->peercred);
    kernel_heap_release(socket->heap,socket);
}
static void unix_put(struct kernel_socket *socket)
{
    if(!socket)return;
    KERNEL_IRQ_LOCK_SCOPE(state,&socket_state_lock);
    int release=unix_put_locked(socket);
    kernel_irq_lock_release(&state);
    if(release)unix_storage_free(socket);
}
static struct unix_credentials unix_current_cred(void)
{
    const struct kernel_credentials *cred=kernel_credentials_current();
    struct kernel_pid *pid=NULL;
    if(kernel_task_current() && kernel_task_identity_get(0,&pid))__builtin_trap();
    return (struct unix_credentials){.pid=pid,.uid=cred->euid,.gid=cred->egid};
}
int kernel_socket_peercred(struct kernel_socket *socket,struct kernel_socket_peercred *cred)
{
    SOCKET_ENDPOINT(state,socket);
    *cred=(struct kernel_socket_peercred){.pid=kernel_task_identity_user_number(socket->peercred.pid),
        .uid=socket->peercred.uid,.gid=socket->peercred.gid};
    return 0;
}
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
    return create?kernel_vfs_socket_bind_at(paths.cwd,paths.root,(const char *)name,0777U,owner,paths.umask):
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
    KERNEL_IRQ_LOCK_SCOPE(state,&socket_state_lock);
    struct kernel_socket *target=unix_find_locked(address,path,sender->type);
    result=!target?-KERNEL_ECONNREFUSED:target->type!=sender->type?-KERNEL_EPROTOTYPE:0;
    if(!result) {unix_get_locked(target);*owner=target;}
    kernel_irq_lock_release(&state);
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
static int unix_writable(const struct kernel_socket *sender);
static void unix_space_notify(struct kernel_wait_node *node,uint32_t reason,uint64_t event)
{ (void)event;
    (void)reason;struct kernel_socket *sender=node->context;
    SOCKET_STATE(state);
    if(sender->unix_space_wait) {
        sender->unix_space_wait=0;wake_socket(sender);
        if (sender->peer && !unix_queue_full(sender,sender->peer) && unix_writable(sender))
            socket_async(sender,KERNEL_ASYNC_POLL_OUT);
    }
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
static int unix_wait_queue(struct kernel_socket *target,uint64_t deadline,uint32_t flags,struct kernel_lock_guard *gc)
{
    if(flags&KERNEL_SOCKET_MSG_DONTWAIT)return -KERNEL_EAGAIN;
    struct kernel_wait_token token={0};enum kernel_wait_wake_reason reason=KERNEL_WAIT_WOKEN;
    if(kernel_wait_prepare(&target->wait,deadline,1,&token)!=KERNEL_SCHEDULER_STATUS_OK)return -KERNEL_EIO;
    /* Caller has the endpoint state lock through prepare; notifications can
     * precede park, but cannot pass the condition/registration boundary. */
    struct socket_state_guard *state=socket_state_owner(&socket_state_lock);
    uintptr_t interrupts=socket_state_unlock_wait(state);
    if(gc)kernel_lock_release(gc);
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
static struct kernel_socket *unix_uncharge_locked(struct socket_packet *packet)
{
    struct kernel_socket *sender=packet->unix_sender;
    if(!sender)return NULL;
    if(!sender->unix_packets || sender->unix_memory<packet->send_charge)__builtin_trap();
    sender->unix_memory-=packet->send_charge;sender->unix_packets--;
    packet->unix_sender=0;
    int release=!sender->unix_packets && unix_put_locked(sender);
    if(!sender->destroying) {
        wake_socket(sender);
        if (unix_writable(sender)) socket_async(sender,KERNEL_ASYNC_POLL_OUT);
    }
    return release?sender:NULL;
}
static void unix_uncharge(struct socket_packet *packet)
{
    if(!packet->unix_sender)return;
    SOCKET_STATE(irq);
    struct kernel_socket *sender=unix_uncharge_locked(packet);
    socket_state_release(&irq);
    if(sender)unix_storage_free(sender);
}
static uint32_t packet_charge(uint32_t size)
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
    socket_tcp_info_retire(pcb);
    pcb->callback_arg = 0; pcb->errf = 0;
    pcb->recv = 0; pcb->sent = 0; pcb->poll = 0; pcb->pollinterval = 0;
    pcb->connected = 0;
    { SOCKET_STATE(state);
      work_remove_locked(&pool_wait, socket); work_remove_locked(&nic_wait, socket); work_remove_locked(&receive_wait, socket);
      write_retry_disarm(socket);socket->receive_retry=0; }
    SOCKET_ENDPOINT(state,socket);
    socket->tcp = 0; socket->peer_closed = 1;
    wake_socket(socket);socket_async(socket,KERNEL_ASYNC_POLL_HUP);
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
    u32_t cork_delay = boaros_lwip_tcp_cork_delay(current_ms);
    if (cork_delay < milliseconds) milliseconds = cork_delay;
    if (packet_timer_head) {
        int32_t remaining = (int32_t)(packet_timer_head->packet_options->deadline - current_ms);
        u32_t packet_delay = remaining <= 0 ? 1U : (u32_t)remaining;
        if (packet_delay < milliseconds) milliseconds = packet_delay;
    }
    {SOCKET_STATE(state);if(rights_retry_pending) {
        int32_t remaining=(int32_t)(rights_retry_ms-current_ms);
        u32_t retry=remaining<=0?1U:(u32_t)remaining;
        if(milliseconds==SYS_TIMEOUTS_SLEEPTIME_INFINITE || retry<milliseconds)milliseconds=retry;
    }}
    { SOCKET_STATE(state);
    if (write_retry_head != 0) {
        int32_t remaining =
            (int32_t)(write_retry_head->write_retry_ms - current_ms);
        u32_t retry = remaining <= 0 ? 1U : (u32_t)remaining;
        if (milliseconds == SYS_TIMEOUTS_SLEEPTIME_INFINITE ||
            retry < milliseconds) milliseconds = retry;
    }}
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
    if (socket->raw != 0) return &socket->raw->local_ip;
    return socket->tcp != 0 ? &socket->tcp->local_ip : 0;
}
static struct ip_pcb *socket_ip_pcb(struct kernel_socket *socket)
{
    if(socket->raw)return (struct ip_pcb *)socket->raw;
    return socket->udp?(struct ip_pcb *)socket->udp:(struct ip_pcb *)socket->tcp;
}

static struct netif *ipv4_multicast_route(const ip4_addr_t *group)
{
    struct netif *selected=ip4_route(group);
    if (selected) return selected;
    /* BoarOS has no separate multicast route table. A configured Ethernet
     * network can transmit to an on-link group without a unicast gateway. */
    for (struct netif *n=netif_list;n;n=n->next) {
        if (!(n->flags&NETIF_FLAG_IGMP) || !netif_is_up(n) || !netif_is_link_up(n) ||
            ip4_addr_isany(netif_ip4_addr(n))) continue;
        if (!selected || (n->hwaddr_len && !selected->hwaddr_len)) selected=n;
    }
    return selected;
}

/* Called with the shared IP/protocol qualification. Explicit interface choice
 * and broadcast permission apply equally to connected UDP and raw sends. */
static int socket_ip_route(struct kernel_socket *socket,const ip_addr_t *remote,
                         struct netif **selected,const ip_addr_t **source)
{
    struct ip_pcb *pcb=socket_ip_pcb(socket);
    struct netif *interface=NULL;
    int multicast=IP_IS_V4(remote) && ip_addr_ismulticast(remote);
    if(pcb->netif_idx)interface=netif_get_by_index(pcb->netif_idx);
    else if(multicast && socket->multicast_index)
        interface=netif_get_by_index(socket->multicast_index);
    else if(multicast) interface=ipv4_multicast_route(ip_2_ip4(remote));
    else if(IP_IS_V6(remote) && ip_addr_ismulticast(remote) && socket->ipv6_multicast_index)
        interface=netif_get_by_index(socket->ipv6_multicast_index);
    else if(IP_IS_V4(remote) && ip4_addr_get_u32(ip_2_ip4(remote))==PP_HTONL(0xffffffffU)) {
        for(struct netif *n=netif_list;n;n=n->next)
            if(n->hwaddr_len && netif_is_up(n) && netif_is_link_up(n) &&
               !ip4_addr_isany_val(*netif_ip4_addr(n)) &&
               (!interface || netif_get_index(n)<netif_get_index(interface)))interface=n;
    } else interface=ip_route(&pcb->local_ip,remote);
    if(!interface || !netif_is_up(interface) || !netif_is_link_up(interface))return -KERNEL_ENETUNREACH;
    if(IP_IS_V4(remote) && ip_addr_isbroadcast(remote,interface) && !socket->broadcast)
        return -KERNEL_EACCES;
    const ip_addr_t *local=&pcb->local_ip;
    if(ip_addr_isany(local) || ip_addr_ismulticast(local)) {
        if(multicast && !ip_addr_isany(&socket->multicast_source))local=&socket->multicast_source;
        else local=ip_netif_get_local_ip(interface,remote);
    }
    if(!local)return -KERNEL_ENETUNREACH;
    if(IP_IS_V6(remote)) {
        const ip6_addr_t *dst=ip_2_ip6(remote),*src=ip_2_ip6(local);
        if(ip6_addr_has_zone(dst) && !ip6_addr_test_zone(dst,interface))return -KERNEL_ENETUNREACH;
        if(interface->hwaddr_len && (ip6_addr_isloopback(src) || ip6_addr_isloopback(dst)))
            return -KERNEL_ENETUNREACH;
        if(!ip6_addr_ismulticast(dst) && !ip6_addr_islinklocal(dst) &&
           ip6_addr_isloopback(src) && !ip6_addr_isloopback(dst))
            return -KERNEL_ENETUNREACH;
        int valid=0;
        for(unsigned i=0;i<LWIP_IPV6_NUM_ADDRESSES;i++)
            if(ip6_addr_isvalid(netif_ip6_addr_state(interface,i)) &&
               ip6_addr_zoneless_eq(src,netif_ip6_addr(interface,i)))valid=1;
        if(!valid)return -KERNEL_ENETUNREACH;
        if(!ip6_addr_ismulticast(dst) && !ip6_addr_has_scope(dst,IP6_UNKNOWN) &&
           boaros_lwip_ip6_onlink(interface,dst)<0 && ip6_addr_isany(&interface->ip6_gateway) && !nd6_default_gateway(interface))
            return -KERNEL_ENETUNREACH;
    }
    if(IP_IS_V6(remote) && ip6_addr_islinklocal(ip_2_ip6(remote)) &&
        (!IP_IS_V6(local) || ip6_addr_isloopback(ip_2_ip6(local))))return -KERNEL_ENETUNREACH;
    if(IP_IS_V4(local) && !ip_addr_isany(local)) {
        if(ip_addr_ismulticast(local) || ip_2_ip4(local)->addr==PP_HTONL(0xffffffffU))return -KERNEL_EINVAL;
        /* An indexed IP_MULTICAST_IF may retain any requested address, but
         * ordinary send still requires a current local source. Lookup may
         * find another interface; Linux permits that, including 127/8 aliases. */
        int found=0;
        for(struct netif *n=netif_list;n;n=n->next)
            if(netif_ip4_addr(n)->addr==ip_2_ip4(local)->addr ||
               (ip4_addr_isloopback(ip_2_ip4(local)) && ip4_addr_isloopback(netif_ip4_addr(n)))) {found=1;break;}
        if(!found)return -KERNEL_ENETUNREACH;
    }
    *selected=interface;*source=local;return 0;
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
#if BOAROS_USER_SMP
static uint32_t tcp_address_hash(const ip_addr_t *ip)
{
    if (IP_IS_V4(ip)) return ip4_addr_get_u32(ip_2_ip4(ip));
    const ip6_addr_t *v6=ip_2_ip6(ip);
    return v6->addr[0]^v6->addr[1]^v6->addr[2]^v6->addr[3];
}

static uint32_t tcp_flow_hash(const ip_addr_t *a,unsigned ap,const ip_addr_t *b,unsigned bp)
{
    /* Reverse-direction packets must land on the same CPU. */
    uint32_t h=(tcp_address_hash(a)+ap*UINT32_C(0x9e3779b9))^
               (tcp_address_hash(b)+bp*UINT32_C(0x9e3779b9));
    h^=h>>16;
    h*=UINT32_C(0x85ebca6b);
    return h^(h>>13);
}

static uint32_t tcp_pcb_hash(const struct tcp_pcb *pcb)
{ return tcp_flow_hash(&pcb->local_ip,pcb->local_port,&pcb->remote_ip,pcb->remote_port); }

static unsigned tcp_route(const ip_addr_t *src,uint16_t sport,const ip_addr_t *dst,
                          uint16_t dport,const struct netif *input,uint8_t *route_bucket,
                          int *established)
{
    unsigned bucket=tcp_flow_hash(src,sport,dst,dport)%TCP_ROUTE_BUCKETS;
    *route_bucket=(uint8_t)bucket;
    KERNEL_RAW_SCOPE(guard,&tcp_route_lock);
    for (const struct tcp_pcb *p=tcp_routes[bucket];p;p=p->boaros_route_next)
        if (p->local_port==dport && p->remote_port==sport &&
            ip_addr_cmp(&p->local_ip,dst) && ip_addr_cmp(&p->remote_ip,src) &&
            (p->netif_idx==NETIF_NO_INDEX || p->netif_idx==netif_get_index(input))) {
            *established = tcp_get_state(p) == ESTABLISHED;
            return p->boaros_owner;
        }
    return 0;
}

static unsigned tcp_packet_cpu(struct pbuf *p,const struct netif *input,
                               const ip_addr_t *src,const ip_addr_t *dst,uint8_t *route_bucket,
                               int *established)
{
    if (p->len<TCP_HLEN) return 0;
    const struct tcp_hdr *hdr=p->payload;
    return tcp_route(src,lwip_ntohs(hdr->src),dst,lwip_ntohs(hdr->dest),input,route_bucket,established);
}

static int tcp_queue_input_locked(struct socket_tcp_owner *owner,struct pbuf *p,struct netif *input,
                                  const ip_addr_t *src,const ip_addr_t *dst,uint8_t bucket,
                                  struct kernel_wait_cursor *wake)
{
    unsigned count=owner->count;
    if (tcp_owner_stopping(owner) || count+owner->output_reserved>=TCP_RX_RING) return 0;
    unsigned slot=(owner->head+count)%TCP_RX_RING;
    owner->ring[slot]=(struct socket_tcp_input){.packet=p,.interface=input,.src=*src,.dst=*dst,
                                               .route_bucket=bucket};
#if BOAROS_COST_DIAGNOSTICS
    owner->ring[slot].queued_ticks=kernel_cost_clock();
    COST_ADD(TCP_INPUT_QUEUE_DEPTH,count+1);
#endif
    __atomic_store_n(&owner->count,count+1,__ATOMIC_RELAXED);
    if (!count && tcp_owner_take_waiting(owner))
        (void)kernel_wait_cursor_prepare(&owner->work,wake);
    return 1;
}

static void tcp_mark_legacy_loopback(struct netif *netif)
{
    if (netif!=fast_loopback) return;
    /* The IP guard excludes the drain. Mark before lwIP links this packet. */
    uint64_t state=__atomic_load_n(&fast_loopback_state,__ATOMIC_RELAXED);
    if (!(state&LOOPBACK_LEGACY))
        __atomic_store_n(&fast_loopback_state,state|LOOPBACK_LEGACY,__ATOMIC_RELEASE);
}

static void tcp_owner_receive(struct socket_tcp_owner *owner,const struct socket_tcp_input *input)
{
    COST_SCOPE(input_cost, TCP_INPUT_PROCESS_TICKS);
    boaros_lwip_tcp_input_packet(input->packet,input->interface,&input->src,&input->dst,
                                input->network,input->network_length,
                                &owner->input_buckets[input->route_bucket]);
}

static void tcp_prepare_ack_wake(struct kernel_socket *socket, struct kernel_wait_cursor *wake)
{
    if (!socket) return;
    KERNEL_IRQ_LOCK_SCOPE(guard, socket_endpoint_lock(socket));
    (void)kernel_wait_cursor_prepare(&socket->wait, wake);
}

static void tcp_wake_ack_waiters(struct kernel_wait_cursor *wake)
{
    if (!wake->queue) return;
    uintptr_t interrupts = arch_interrupt_save();
    kernel_wait_cursor_wake(wake, 1);
    arch_interrupt_restore(interrupts);
}

static int tcp_try_inline_ack(struct socket_tcp_owner *owner, const struct socket_tcp_input *input)
{
    /* Established ACKs can use an idle shard's qualification on the
     * receiving CPU. Do not wait for a busy owner from the shared IP path. */
    struct kernel_lock_guard *ip = boaros_lwip_ip_drop_current();
    if (!ip) return 0;
    struct kernel_socket_protocol_scope scope;
    struct kernel_wait_cursor wake = {0};
    int ordered = 0;
    if (tcp_owner_lock(&scope, owner, 1)) {
        {
            KERNEL_RAW_SCOPE(ingress, &owner->ingress);
            ordered = !owner->count && !owner->output_reserved && !tcp_owner_stopping(owner);
        }
        if (ordered) {
            struct kernel_socket *ack_socket = NULL;
            owner->ack_waiter = &ack_socket;
            tcp_owner_receive(owner, input);
            owner->ack_waiter = NULL;
            tcp_prepare_ack_wake(ack_socket, &wake);
        }
        kernel_socket_protocol_leave(&scope);
    }
    tcp_wake_ack_waiters(&wake);
    boaros_lwip_ip_lock(ip);
    return ordered;
}

static int tcp_dispatch(struct pbuf *p,struct netif *input,const ip_addr_t *src,const ip_addr_t *dst)
{
    if (!__atomic_load_n(&tcp_owner_count,__ATOMIC_ACQUIRE)) return 0;
    uint8_t bucket;
    int established;
    unsigned cpu=tcp_packet_cpu(p,input,src,dst,&bucket,&established);
    if (!cpu) return 0;
    const struct tcp_hdr *header = p->payload;
    int ack_only = TCPH_FLAGS(header) == TCP_ACK && TCPH_HDRLEN_BYTES(header) == p->tot_len;
    struct socket_tcp_owner *owner=tcp_owner_at(cpu);
    /* Leave pending Ethernet frames to the receiving CPU and let the owner
     * merge notifications while serving its own pending acknowledgements. */
    if (established && ack_only && input->hwaddr_len &&
        !(p->flags & PBUF_FLAG_BOAROS_RX_BACKLOG) &&
        __atomic_load_n(&owner->waiting, __ATOMIC_RELAXED)) {
        struct socket_tcp_input packet = {.packet=p, .interface=input, .src=*src, .dst=*dst,
            .route_bucket=bucket, .network_length=ip_current_header_tot_len()};
        packet.network = (const unsigned char *)ip_next_header_ptr() - packet.network_length;
        if (tcp_try_inline_ack(owner, &packet)) {
            COST_ADD(TCP_INPUT_INLINE_ACK, 1);
            return 1;
        }
        COST_ADD(TCP_INPUT_INLINE_MISS, 1);
    }
    int queued;
    struct kernel_wait_cursor wake={0};
    { KERNEL_RAW_SCOPE(ingress,&owner->ingress); queued=tcp_queue_input_locked(owner,p,input,src,dst,bucket,&wake);
      if(queued) {
          struct socket_tcp_input *entry=&owner->ring[(owner->head+owner->count-1)%TCP_RX_RING];
          entry->network=((const unsigned char *)ip_next_header_ptr()-ip_current_header_tot_len());entry->network_length=ip_current_header_tot_len();
      } }
    if (queued) {
        COST_ADD(TCP_INPUT_QUEUED, 1);
#if BOAROS_COST_DIAGNOSTICS
        if (ack_only) COST_ADD(TCP_INPUT_ACK_ONLY, 1);
#endif
    }
    if (wake.queue) {
        COST_ADD(TCP_INPUT_WAKE, 1);
        kernel_wait_cursor_wake(&wake,1);
    }
    if (!queued) pbuf_free(p);
    return 1;
}

static int tcp_local_output(const struct tcp_pcb *pcb,struct pbuf *p,struct netif *netif,
                             const ip_addr_t *src,const ip_addr_t *dst,int *result)
{
    /* The headerless shortcut cannot supply packet sockets with an IP header. */
    if (packet_taps_active()) return 0;
    if (!pcb || !pcb->boaros_owner || netif!=fast_loopback ||
        !__atomic_load_n(&tcp_owner_count,__ATOMIC_ACQUIRE)) return 0;
    struct socket_tcp_owner *owner=current_tcp_owner();
    if (!owner || owner->index!=pcb->boaros_owner) return 0;
    /* Ordinary data and ACK use the PCB's exact tuple. Both peers live
     * under this owner, which also clears the link before either is freed. */
    const struct tcp_pcb *peer=pcb->boaros_loopback_peer;
    if (!peer || src!=&pcb->local_ip || dst!=&pcb->remote_ip || p->len<TCP_HLEN ||
        (peer->netif_idx!=NETIF_NO_INDEX && peer->netif_idx!=netif_get_index(netif))) return 0;
    /* Filtered packets need the real IP header, including negative-offset
     * loads. Use normal IP output instead of the headerless local shortcut. */
    struct kernel_socket *receiver=tcp_filter_socket(peer);
    if (receiver && socket_filter_attached(receiver)) return 0;
    const struct tcp_hdr *tcp=p->payload;
    if (tcp->src!=lwip_htons(pcb->local_port) || tcp->dest!=lwip_htons(pcb->remote_port)) return 0;
    uint64_t state;
    {
        KERNEL_RAW_SCOPE(ingress,&owner->ingress);
        state=__atomic_load_n(&fast_loopback_state,__ATOMIC_ACQUIRE);
        if ((state&(LOOPBACK_AVAILABLE|LOOPBACK_LEGACY))!=LOOPBACK_AVAILABLE ||
            tcp_owner_stopping(owner) || network_is_stopping() ||
            owner->count==TCP_RX_RING) return 0;
        if (owner->output_reserved) __builtin_trap();
        owner->output_reserved=1;
    }
    /* The peer shares this owner, so newer legacy output cannot pass the
     * reservation. RX has its own header and borrows only the TX payload. */
    u16_t header=TCPH_HDRLEN_BYTES((const struct tcp_hdr *)p->payload);
    if (header<TCP_HLEN || header>p->len) __builtin_trap();
    struct pbuf *input=pbuf_clone_shared(PBUF_RAW,p,header);
    err_t error=input?ERR_OK:ERR_MEM;
    if (input) input->if_idx=netif_get_index(netif);
    struct kernel_wait_cursor wake={0};
    {
        KERNEL_RAW_SCOPE(ingress,&owner->ingress);
        if (!owner->output_reserved) __builtin_trap();
        owner->output_reserved=0;
        if (error==ERR_OK) {
            uint64_t current=__atomic_load_n(&fast_loopback_state,__ATOMIC_ACQUIRE);
            if (tcp_owner_stopping(owner) || network_is_stopping()) error=ERR_IF;
            else if ((current&~UINT64_C(2))!=(state&~UINT64_C(2))) error=ERR_RTE;
            else if (!tcp_queue_input_locked(owner,input,netif,src,dst,peer->boaros_route_bucket,&wake)) __builtin_trap();
            else input=0;
        }
    }
    if (wake.queue) kernel_wait_cursor_wake(&wake,1);
    if (input) pbuf_free(input);
    *result=error;
    return 1;
}

static void tcp_pcb_forget(struct tcp_pcb *pcb)
{
    if (!pcb->boaros_owner) return;
    struct socket_tcp_owner *owner=current_tcp_owner();
    if (!owner || owner->index!=pcb->boaros_owner) __builtin_trap();
    unsigned bucket=pcb->boaros_route_bucket;
    struct tcp_pcb **input_link=&owner->input_buckets[bucket];
    while (*input_link!=pcb) {
        if (!*input_link) __builtin_trap();
        input_link=&(*input_link)->boaros_input_next;
    }
    *input_link=pcb->boaros_input_next;
    pcb->boaros_input_next=0;
    KERNEL_RAW_SCOPE(guard,&tcp_route_lock);
    struct tcp_pcb **link=&tcp_routes[bucket];
    while (*link!=pcb) {
        if (!*link) __builtin_trap();
        link=&(*link)->boaros_route_next;
    }
    *link=pcb->boaros_route_next;
    pcb->boaros_route_next=0;
    if (tcp_get_state(pcb) == TIME_WAIT)
        __atomic_sub_fetch(&owner->timewait_count, 1, __ATOMIC_RELAXED);
    else tcp_owner_active[pcb->boaros_owner]--;
    if (pcb->boaros_loopback_peer) {
        struct tcp_pcb *peer=pcb->boaros_loopback_peer;
        if (peer->boaros_owner!=pcb->boaros_owner || peer->boaros_loopback_peer!=pcb)
            __builtin_trap();
        peer->boaros_loopback_peer=0;
        pcb->boaros_loopback_peer=0;
    }
    pcb->boaros_owner=0;
}

/* TIME_WAIT keeps the tuple routed, but no longer competes for data work. */
static void tcp_pcb_timewait(struct tcp_pcb *pcb)
{
    if (!pcb->boaros_owner) return;
    KERNEL_RAW_SCOPE(guard,&tcp_route_lock);
    tcp_owner_active[pcb->boaros_owner]--;
    __atomic_add_fetch(&tcp_owner_at(pcb->boaros_owner)->timewait_count, 1, __ATOMIC_RELAXED);
}

static int tcp_reclaim_remote_timewait(void)
{
    unsigned count = __atomic_load_n(&tcp_owner_count, __ATOMIC_ACQUIRE);
    if (!count || current_tcp_owner() || protocol_current_owner() != kernel_io_context_current())
        return 0;
    int reclaimed = 0;
    struct kernel_lock_guard *ip = NULL;
    for (unsigned cpu = 1; cpu <= count && !reclaimed; ++cpu) {
        struct socket_tcp_owner *owner = tcp_owner_at(cpu);
        if (!__atomic_load_n(&owner->timewait_count, __ATOMIC_RELAXED)) continue;
        /* The default TCP mutex still owns the listener and the input packet.
         * Drop IP before acquiring a shard, in the same order as promotion. */
        if (!ip && !(ip = boaros_lwip_ip_drop_current())) return 0;
        struct kernel_socket_protocol_scope scope;
        (void)tcp_owner_lock(&scope, owner, 0);
        if (!tcp_owner_stopping(owner)) reclaimed = boaros_lwip_tcp_reclaim_timewait();
        kernel_socket_protocol_leave(&scope);
    }
    if (ip) boaros_lwip_ip_lock(ip);
    return reclaimed;
}

/* Promotion is serialized by the default TCP protocol mutex. Look up a
 * published owner before taking its mutex, keeping the route lock innermost. */
static unsigned tcp_pick_owner(const struct tcp_pcb *pcb,unsigned count,uint32_t hash)
{
    if (count==1) return 1;
    KERNEL_RAW_SCOPE(guard,&tcp_route_lock);
    for (const struct tcp_pcb *p=tcp_routes[hash%TCP_ROUTE_BUCKETS];p;p=p->boaros_route_next) {
        if (p->local_port==pcb->local_port && p->remote_port==pcb->remote_port &&
            ip_addr_cmp(&p->local_ip,&pcb->local_ip) && ip_addr_cmp(&p->remote_ip,&pcb->remote_ip))
            return p->boaros_owner;
        if (p->local_port==pcb->remote_port && p->remote_port==pcb->local_port &&
            ip_addr_cmp(&p->local_ip,&pcb->remote_ip) &&
            ip_addr_cmp(&p->remote_ip,&pcb->local_ip))
            return p->boaros_owner;
    }
    unsigned chosen=1+hash%count;
    if (tcp_owner_active[chosen]<TCP_OWNER_SPREAD_AT) return chosen;
    for (unsigned cpu=1;cpu<=count && tcp_owner_active[chosen];cpu++)
        if (tcp_owner_active[cpu]<tcp_owner_active[chosen]) chosen=cpu;
    return chosen;
}

/* Both tuples have the same owner. Re-read the actual peer after acquiring
 * that owner's mutex, since the previous peer may have retired meanwhile. */
static void tcp_link_loopback_peer(struct tcp_pcb *pcb,unsigned bucket)
{
    if (!pcb->boaros_loopback) return;
    for (struct tcp_pcb *peer=tcp_routes[bucket];peer;peer=peer->boaros_route_next) {
        if (!peer->boaros_loopback || peer->local_port!=pcb->remote_port ||
            peer->remote_port!=pcb->local_port || !ip_addr_cmp(&peer->local_ip,&pcb->remote_ip) ||
            !ip_addr_cmp(&peer->remote_ip,&pcb->local_ip) ||
            (peer->netif_idx && pcb->netif_idx && peer->netif_idx!=pcb->netif_idx)) continue;
        if (peer->boaros_owner!=pcb->boaros_owner) __builtin_trap();
        if (!peer->boaros_loopback_peer) {
            peer->boaros_loopback_peer=pcb;
            pcb->boaros_loopback_peer=peer;
        }
        return;
    }
}

static int tcp_route_conflict(const struct tcp_pcb *pcb,const ip_addr_t *local,
                             uint16_t port,const ip_addr_t *remote,uint16_t remote_port)
{
    if (!__atomic_load_n(&tcp_owner_count,__ATOMIC_ACQUIRE)) return 0;
    KERNEL_RAW_SCOPE(guard,&tcp_route_lock);
    unsigned first=remote?tcp_flow_hash(local,port,remote,remote_port)%TCP_ROUTE_BUCKETS:0;
    unsigned end=remote?first+1:TCP_ROUTE_BUCKETS;
    for (unsigned i=first;i<end;i++)
        for (const struct tcp_pcb *p=tcp_routes[i];p;p=p->boaros_route_next) {
            if (p==pcb || p->local_port!=port ||
                (pcb->netif_idx && p->netif_idx && pcb->netif_idx!=p->netif_idx)) continue;
            if (!local) return 1;
            if (remote) {
                if (p->remote_port==remote_port && ip_addr_cmp(&p->local_ip,local) &&
                    ip_addr_cmp(&p->remote_ip,remote)) return 1;
            } else if (address_overlap(local,&p->local_ip) &&
                       !(ip_get_option(pcb,SOF_REUSEADDR) &&
                         ((__atomic_load_n(&p->so_options,__ATOMIC_RELAXED)&SOF_REUSEADDR) ||
                          tcp_get_state(p)==TIME_WAIT))) return 1;
        }
    return 0;
}

static struct boaros_lwip_tcp_context *tcp_context(void)
{
    struct socket_tcp_owner *owner=current_tcp_owner();
    return owner?&owner->context:0;
}

/* CPU0 finishes the current input/accept callback before changing ownership.
 * CPU0's protocol lock excludes ingress, and no secondary takes it while
 * holding its TCP lock. Drop the shared IP guard while taking that lock. */
static int tcp_promote(struct kernel_socket *socket,struct kernel_socket_protocol_scope *scope)
{
    unsigned count=__atomic_load_n(&tcp_owner_count,__ATOMIC_ACQUIRE);
    struct tcp_pcb *pcb=socket->tcp;
    if (!count || !scope->outer || !pcb ||
        (pcb->state!=ESTABLISHED && pcb->state!=CLOSE_WAIT) || tcp_socket_cpu(socket)) return 0;
    uint32_t hash=tcp_pcb_hash(pcb);
    unsigned cpu=tcp_pick_owner(pcb,count,hash);
    struct socket_tcp_owner *owner=tcp_owner_at(cpu);
    boaros_lwip_ip_unlock(&scope->ip_guard);
    struct kernel_lock_guard guard={0};
    kernel_mutex_lock(&owner->mutex,&guard);
    int moved=boaros_lwip_tcp_context_move_active(&boaros_lwip_tcp_default_context,&owner->context,pcb);
    if (moved) {
        /* The receiver acquires the owner mutex before using this bucket.
         * The queued packet carries its number, never a reference to a PCB. */
        unsigned bucket=hash%TCP_ROUTE_BUCKETS;
        pcb->boaros_route_bucket=(uint8_t)bucket;
        pcb->boaros_input_next=owner->input_buckets[bucket];
        owner->input_buckets[bucket]=pcb;
        /* A published route never owns a pointer past tcp_free(). */
        { KERNEL_RAW_SCOPE(route_guard,&tcp_route_lock);
          pcb->boaros_owner=(uint8_t)cpu;
          tcp_link_loopback_peer(pcb,bucket);
          pcb->boaros_route_next=tcp_routes[bucket];
          tcp_routes[bucket]=pcb;
          tcp_owner_active[cpu]++; }
        SOCKET_STATE(state);
        int queued=socket->work_links[ready_work.slot].queued;
        if (queued) work_remove_locked(&ready_work,socket);
        __atomic_store_n(&socket->tcp_prepare_mss,boaros_lwip_tcp_detached_mss(pcb),__ATOMIC_RELAXED);
        __atomic_store_n(&socket->tcp_cpu,cpu,__ATOMIC_RELEASE);
        if (queued) socket_schedule_locked(socket,0,state.owner);
        else if (tcp_owner_take_waiting(owner)) socket_state_notify(state.owner,&owner->work);
    }
    kernel_lock_release(&guard);
    boaros_lwip_ip_lock(&scope->ip_guard);
    return moved;
}
#endif
static int port_conflict(struct kernel_socket *socket, const ip_addr_t *ip,
                         uint16_t port, int listening)
{
    /* udp_bind already checks this port against all UDP PCBs, including
     * wildcard address families and both sockets' SO_REUSEADDR flags. */
    if (port == 0 || socket->type==SOCKET_DGRAM) return 0;
    /* lwIP 的 TCP bind 不检查 ANY 与纯 IPv6 的交集；补齐 Linux 双栈资格。 */
    COST_ADD(NETWORK_GLOBAL_SCANS, 1);
    for (struct kernel_socket *other = inet_sockets; other != 0; other = other->inet_next) {
#if BOAROS_USER_SMP
        if (tcp_socket_cpu(other)) continue; /* Remote TCP tuples are checked by tcp_bind(). */
#endif
        if (other == socket || other->type != socket->type ||
            (socket->bound_device && other->bound_device && socket->bound_device!=other->bound_device) ||
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
    /* Only CPU0 owns embryonic PCBs. Detach failed children from the FIFO
     * before their destruction can run callbacks or release the protocol scope. */
    struct socket_pending *dead=NULL;
    {
        SOCKET_ENDPOINT(state,listener);
        struct socket_pending **link=&listener->pending_head;
        struct socket_pending *last=NULL;
        while (*link) {
            struct socket_pending *entry=*link;
            if (!entry->child->pending_ready) {
                *link=entry->next;
                entry->next=dead;dead=entry;
            } else {
                last=entry;link=&entry->next;
            }
        }
        listener->pending_tail=last;
    }
    while (dead) {
        struct socket_pending *entry=dead;dead=entry->next;
        kernel_socket_destroy(entry->child);
        kernel_heap_release(listener->heap,entry);
    }
}

/* 端点锁只发布队列和额度；载荷分配、复制及回收均在保护外完成。 */
static void packet_publish_locked(struct kernel_socket *socket, struct socket_packet *packet, uint32_t charge)
{
    if (socket->rx_bytes > UINT32_MAX - charge) __builtin_trap();
    socket->rx_bytes += charge;
    if (socket->packets_tail) socket->packets_tail->next = packet;
    else socket->packets_head = packet;
    socket->packets_tail = packet;
    socket_state_notify(socket_state_owner(socket_endpoint_lock(socket)),&socket->wait);
    socket_async(socket,KERNEL_ASYNC_POLL_IN);
}

static void packet_publish(struct kernel_socket *socket, struct socket_packet *packet, uint32_t charge)
{
    SOCKET_ENDPOINT(state,socket);
    packet_publish_locked(socket,packet,charge);
}

struct packet_frame {
    const struct pbuf *pbuf;
    const unsigned char *bytes;
    const unsigned char *prefix;
    uint32_t length, start;
};

static int packet_frame_read(void *object,int64_t offset,void *buffer,uint32_t length)
{
    struct packet_frame *view=object;
    offset+=view->start;
    if (offset<0 || (uint64_t)offset+length>view->length) return -1;
    unsigned char *out=buffer;
    if (view->prefix) {
        if (offset<PACKET_ETH_HEADER) {
            unsigned take=PACKET_ETH_HEADER-(unsigned)offset;
            if (take>length) take=length;
            __builtin_memcpy(out,view->prefix+offset,take);
            out+=take;length-=take;offset+=take;
        }
        offset-=PACKET_ETH_HEADER;
    }
    if (!length) return 0;
    if (view->pbuf)
        return pbuf_copy_partial(view->pbuf,out,(u16_t)length,(u16_t)offset)==length ? 0 : -1;
    __builtin_memcpy(out,view->bytes+offset,length);
    return 0;
}

static void packet_tap(struct netif *interface,struct packet_frame *view,unsigned type,
    struct kernel_socket *sender)
{
    unsigned char header[PACKET_ETH_HEADER];
    view->start=0;
    if (packet_frame_read(view,0,header,sizeof(header))) return;
    uint16_t protocol=(uint16_t)header[12]<<8|header[13];
    uint32_t index=netif_get_index(interface);
    uint16_t hardware=view->prefix || interface==loopback_netif() ? 772 : 1;
    for (struct kernel_socket *socket=packet_taps;socket;socket=socket->packet_next) {
        if (!packet_hooked(socket) || socket==sender ||
            (socket->packet_index && socket->packet_index!=index)) continue;
        if (socket->packet_protocol!=PACKET_ETH_ALL &&
            (type==PACKET_OUTGOING || socket->packet_protocol!=protocol)) continue;
        view->start=socket->type==SOCKET_RAW ? 0 : PACKET_ETH_HEADER;
        uint32_t length=view->length-view->start;
        struct kernel_bpf_packet filter={
            .object=view,.length=length,.protocol=protocol,.packet_type=type,
            .ifindex=index,.ingress_ifindex=type==PACKET_OUTGOING ? 0 : index,
            .hardware_type=hardware,.network_offset=PACKET_ETH_HEADER-view->start,
            .link_offset=-(int32_t)view->start,.payload_offset=PACKET_ETH_HEADER-view->start,
            .read=packet_frame_read
        };
        uint32_t kept=socket_filter_run(socket,&filter);
        if (!kept) continue;
        uint32_t original_length=length;
        if (kept<length) length=kept;
        struct kernel_socket_address address={
            .family=KERNEL_SOCKET_AF_PACKET,.port=protocol,.scope=index,.link_type=hardware,
            .link_packet_type=(uint8_t)type,.link_length=6,.link_size=20
        };
        __builtin_memcpy(address.link_address,header+6,6);
        struct socket_packet_options *options=socket->packet_options;
        if (options->ring) {
            unsigned events=packet_ring_receive(options->ring,&address,original_length,length,
                PACKET_ETH_HEADER-view->start,packet_frame_read,view,sys_now(),
                kernel_time_realtime_ns());
            if (events & PACKET_RING_RECEIVED) options->packets++;
            else options->drops++;
            if (events & PACKET_RING_FROZEN) options->freezes++;
            packet_timer_update(socket);
            if (events & PACKET_RING_PUBLISHED) {
                SOCKET_ENDPOINT(state,socket);
                wake_socket(socket);
                socket_async(socket,KERNEL_ASYNC_POLL_IN);
            }
            continue;
        }
        uint32_t charge=packet_charge(length);
        {
            SOCKET_ENDPOINT(state,socket);
            if (charge>socket->rx_limit || socket->record_memory>socket->rx_limit-charge) {
                options->drops++;
                continue;
            }
        }
        /* The frame's original owner may recycle it as soon as the tap returns. */
        struct socket_packet *packet=NULL;
        if (kernel_heap_allocate_zeroed(socket->heap,1,charge,(void **)&packet)) {
            options->drops++;
            continue;
        }
        packet->length=length;
        packet->address=address;
        if (packet_frame_read(view,0,packet->data,length)) __builtin_trap();
        SOCKET_ENDPOINT(state,socket);
        if (charge<=socket->rx_limit && socket->record_memory<=socket->rx_limit-charge) {
            socket->record_memory+=charge;
            packet_publish_locked(socket,packet,length ? length : 1);
            options->packets++;
            packet=NULL;
        }
        socket_state_release(&state);
        if (packet) {
            options->drops++;
            kernel_heap_release(socket->heap,packet);
        }
    }
}

static void packet_capture_ethernet(struct netif *interface,const struct pbuf *pbuf,
    const void *bytes,uint16_t size,int outgoing)
{
    KERNEL_NO_RECLAIM_IO;
    struct packet_frame view={.pbuf=pbuf,.bytes=bytes,.length=size};
    unsigned type=PACKET_OUTGOING;
    if (!outgoing) {
        const unsigned char *dest=bytes;
        static const unsigned char broadcast[6]={255,255,255,255,255,255};
        type=!__builtin_memcmp(dest,broadcast,6) ? PACKET_BROADCAST :
            (dest[0]&1U) ? PACKET_MULTICAST :
            __builtin_memcmp(dest,interface->hwaddr,6) ? PACKET_OTHERHOST : PACKET_HOST;
    }
    packet_tap(interface,&view,type,NULL);
}

void kernel_socket_packet_ethernet(struct netif *interface,const struct pbuf *pbuf,
    const void *bytes,uint16_t size,int outgoing)
{
    if (!packet_taps_active() || size<PACKET_ETH_HEADER) return;
#if BOAROS_USER_SMP
    if (boaros_lwip_parallel_enabled()) {
        BOAROS_LWIP_IP_SCOPE(ip);
        packet_capture_ethernet(interface,pbuf,bytes,size,outgoing);
        return;
    }
#endif
    struct kernel_socket_protocol_scope protocol;
    kernel_socket_protocol_enter(&protocol);
    packet_capture_ethernet(interface,pbuf,bytes,size,outgoing);
    kernel_socket_protocol_leave(&protocol);
}

void kernel_socket_packet_loopback(struct netif *interface,const struct pbuf *pbuf)
{
    if (!packet_taps_active()) return;
    unsigned char version=pbuf_get_at(pbuf,0)>>4;
    if (version!=4 && version!=6) return;
    unsigned char header[PACKET_ETH_HEADER]={0};
    header[12]=version==4 ? 0x08 : 0x86;
    header[13]=version==4 ? 0 : 0xdd;
    struct kernel_socket_protocol_scope protocol;
    kernel_socket_protocol_enter(&protocol);
    KERNEL_NO_RECLAIM_IO;
    /* IP delivered locally uses the loopback device in the socket address. */
    struct netif *loop=loopback_netif();
    if (loop) interface=loop;
    struct packet_frame view={.pbuf=pbuf,.prefix=header,.length=pbuf->tot_len+sizeof(header)};
    packet_tap(interface,&view,PACKET_OUTGOING,NULL);
    packet_tap(interface,&view,PACKET_HOST,NULL);
    kernel_socket_protocol_leave(&protocol);
}

void kernel_socket_packet_detach(struct netif *interface)
{
    /* Called before netif_remove under the IP qualification; this index can
     * no longer be used for either filtering or sending on the old interface. */
    uint32_t index=netif_get_index(interface);
    for (struct kernel_socket *socket=packet_taps;socket;socket=socket->packet_next) {
        if (socket->packet_index!=index) continue;
        packet_tap_update(socket,UINT32_MAX,socket->packet_protocol);
        SOCKET_ENDPOINT(state,socket);
        socket->pending_error=-KERNEL_ENODEV;
        wake_socket(socket);
        socket_async(socket,KERNEL_ASYNC_POLL_ERR);
    }
}

static int udp_receive_capacity(void)
{
    /* The NIC callback includes protocol heap, pool buffers and RX loans.
     * Without a NIC, reserve space in the protocol heap for control traffic. */
    if (network_udp_capacity) return network_udp_capacity(network_context);
    return STATS_GET(mem.used) <= MEM_SIZE - (65536U + 4096U);
}

static struct socket_packet *udp_packet_allocate(struct kernel_socket *socket)
{
    struct socket_packet *packet;
    enum kernel_heap_status status = kernel_heap_allocate(socket->heap,
        sizeof(*packet), (void **)&packet);
    if (status == KERNEL_HEAP_STATUS_EMPTY) return NULL;
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    return packet;
}

static int ipv4_multicast_accept(const struct kernel_socket *socket,struct netif *input,
    const ip4_addr_t *group)
{
    if (socket->ipv4_multicast_all) return 1;
    for (const struct socket_ipv4_member *m=socket->ipv4_members;m;m=m->next)
        if (m->index==netif_get_index(input) && ip4_addr_eq(&m->group,group)) return 1;
    return 0;
}

static int udp_receive_fits_locked(const struct kernel_socket *socket, uint32_t charge)
{
    return charge<=socket->rx_limit && socket->rx_bytes<=socket->rx_limit-charge;
}

static void udp_received(void *context, struct udp_pcb *pcb,
                         struct pbuf *payload, const ip_addr_t *peer,
                         u16_t port)
{
    struct kernel_socket *socket = context;
    (void)pcb;
    uint32_t charge = payload->tot_len ? payload->tot_len : 1U;
    if (udp_receive_capacity()) {
        SOCKET_ENDPOINT(state,socket);
        if (udp_receive_fits_locked(socket,charge)) {
            struct socket_packet *packet=socket->udp_spares;
            if (packet) {
                socket->udp_spares=packet->next;
                socket->udp_spare_count--;
            } else {
                socket_state_release(&state);
                packet=udp_packet_allocate(socket);
                if (!packet) {
                    kernel_socket_pbuf_free(payload);
                    return;
                }
                socket_state_acquire_lock(&state,socket_endpoint_lock(socket));
                if (!udp_receive_fits_locked(socket,charge)) {
                    socket_state_release(&state);
                    kernel_heap_release(socket->heap,packet);
                    goto drop;
                }
            }
            struct netif *input=boaros_lwip_parallel_enabled()?ip_current_input_netif():NULL;
            *packet=(struct socket_packet){
                .payload=payload, .loopback=input && input==netif_get_loopif(),
            };
            address_export(socket,peer,port,&packet->address);
            packet_publish_locked(socket,packet,charge);
            return;
        }
    }
drop:
    UDP_STATS_INC(udp.memerr);
    kernel_socket_pbuf_free(payload);
}

/* UDP multicast keeps one immutable pbuf including its UDP header. Each
 * queue owns a reference, a separate logical end (BPF snap length), and offset.
 * Publishing one recipient never mutates storage another CPU may be reading. */
static int socket_udp_multicast_receive(struct udp_pcb *pcb,struct pbuf *p,struct netif *input,
    const void *network,uint16_t network_length,uint16_t port)
{
    if(pcb->recv!=udp_received)return 0;
    struct kernel_socket *socket=pcb->recv_arg;
    int ipv6=ip_current_is_v6();
    int member;
    if(ipv6) {
        member=socket->ipv6_multicast_all;
        for(struct socket_ipv6_member *m=socket->ipv6_members;!member && m;m=m->next)
            if(m->active && m->index==netif_get_index(input) &&
                ip6_addr_zoneless_eq(&m->group,ip6_current_dest_addr()))member=1;
    } else member=ipv4_multicast_accept(socket,input,ip4_current_dest_addr());
    if(!member)return 1;
    struct socket_filter_packet view={p,network,network_length};
    struct kernel_bpf_packet filter={.object=&view,.length=p->tot_len,.protocol=ipv6?0x86dd:0x0800,
        .ifindex=netif_get_index(input),.ingress_ifindex=netif_get_index(input),.packet_type=2,
        .hardware_type=input->hwaddr_len?1:772,.payload_offset=8,
        .network_offset=network_length?-(int32_t)network_length:INT32_MIN,
        .link_offset=INT32_MIN,.read=socket_filter_read};
    uint32_t kept=socket_filter_run(socket,&filter);
    if(!kept)return 1;
    if(kept<8)kept=8;
    if(kept>p->tot_len)kept=p->tot_len;
    uint32_t charge=kept>8?kept-8:1;
    if (!udp_receive_capacity()) return 1;
    struct socket_packet *packet = udp_packet_allocate(socket);
    if (!packet) return 1;
    pbuf_ref(p);
    *packet=(struct socket_packet){.length=kept,.consumed=8,.payload=p};
    address_export(socket,ip_current_src_addr(),port,&packet->address);
    {SOCKET_ENDPOINT(state,socket);
      if(charge<=socket->rx_limit && socket->rx_bytes<=socket->rx_limit-charge) {
        packet_publish_locked(socket,packet,charge);return 1;
      }}
    kernel_socket_pbuf_free(p);
    kernel_heap_release(socket->heap,packet);
    return 1;
}

/* Raw input precedes ICMP processing, which can strip headers and rewrite an
 * echo request in place. Queue an independent version and leave the original
 * with IP so other raw endpoints and the ICMP responder still receive it. */
static u8_t raw_received(void *context,struct raw_pcb *pcb,struct pbuf *input,
                         const ip_addr_t *peer)
{
    struct kernel_socket *socket=context;
    (void)pcb;
    struct netif *interface=ip_current_input_netif();
    if(ip4_addr_ismulticast(ip4_current_dest_addr()) &&
        !ipv4_multicast_accept(socket,interface,ip4_current_dest_addr()))return 0;
    struct socket_filter_packet view={.packet=input};
    struct kernel_bpf_packet filter={.object=&view,.length=input->tot_len,
        .protocol=0x0800,.ifindex=netif_get_index(interface),
        .ingress_ifindex=netif_get_index(interface),
        .packet_type=(input->flags&PBUF_FLAG_LLBCAST)?1:(input->flags&PBUF_FLAG_LLMCAST)?2:0,
        .hardware_type=interface->hwaddr_len?1:772,
        .payload_offset=ip_current_header_tot_len()+8U,.network_offset=0,
        .link_offset=INT32_MIN,.read=socket_filter_read};
    uint32_t kept=socket_filter_run(socket,&filter);
    if(!kept)return 0;
    if(kept>input->tot_len)kept=input->tot_len;
    uint32_t charge=kept?kept:1U;
    { SOCKET_ENDPOINT(state,socket);
      if(socket->destroying || socket->read_closed || charge>socket->rx_limit ||
         socket->rx_bytes>socket->rx_limit-charge)return 0; }
    if((network_udp_capacity && !network_udp_capacity(network_context)) ||
       STATS_GET(mem.used)>MEM_SIZE-(65536U+4096U))return 0;
    struct socket_packet *packet=NULL;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(socket->heap,1,
        sizeof(*packet),(void **)&packet);
    if(status==KERNEL_HEAP_STATUS_EMPTY)return 0;
    if(status!=KERNEL_HEAP_STATUS_OK)__builtin_trap();
    struct pbuf *copy=pbuf_alloc(PBUF_RAW,(u16_t)kept,PBUF_RAM);
    if(!copy) {kernel_heap_release(socket->heap,packet);return 0;}
    if(pbuf_copy_partial(input,copy->payload,(u16_t)kept,0)!=kept)__builtin_trap();
    packet->payload=copy;
    address_export(socket,peer,0,&packet->address);
    { SOCKET_ENDPOINT(state,socket);
      if(!socket->destroying && !socket->read_closed && charge<=socket->rx_limit &&
         socket->rx_bytes<=socket->rx_limit-charge) {
          packet_publish_locked(socket,packet,charge);return 0;
      } }
    kernel_socket_pbuf_free(copy);kernel_heap_release(socket->heap,packet);
    return 0;
}

static err_t tcp_connected(void *context, struct tcp_pcb *pcb, err_t error)
{
    struct kernel_socket *socket = context;
    (void)pcb;
    socket_schedule(socket, SOCKET_WORK_OUTPUT | SOCKET_WORK_REAP |
                    (error==ERR_OK?SOCKET_WORK_PROMOTE:0));
    SOCKET_ENDPOINT(irq,socket);
    socket->connecting = 0U;
    socket->connected = error == ERR_OK;
    if (error == ERR_OK) {
        address_export(socket, &pcb->local_ip, pcb->local_port, &socket->saved_local);
        address_export(socket, &pcb->remote_ip, pcb->remote_port, &socket->saved_peer);
    }
    socket->error = lwip_error(error);
    wake_socket(socket);
    socket_async(socket,error==ERR_OK?KERNEL_ASYNC_POLL_OUT:KERNEL_ASYNC_POLL_ERR);
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
    if (payload == 0) {
        SOCKET_ENDPOINT(state,socket);socket->peer_closed = 1U;
        wake_socket(socket);
        socket_async(socket,socket->write_closed?KERNEL_ASYNC_POLL_HUP:KERNEL_ASYNC_POLL_IN);
        return ERR_OK;
    }
    if (error != ERR_OK) return error;
    /* TCP already charged the advertised receive credit, including bytes
     * promised before SO_RCVBUF was lowered. Only real memory can fail here. */
    status = kernel_heap_allocate_zeroed(socket->heap, 1U, sizeof(*packet),
                                         (void **)&packet);
    if (status == KERNEL_HEAP_STATUS_EMPTY) {
        {
            KERNEL_IRQ_LOCK_SCOPE(state,&socket_state_lock);
            socket->receive_retry=1;
            work_remove_locked(&receive_wait, socket);
            work_add_locked(&receive_wait, socket, receive_generation);
        }
#if BOAROS_USER_SMP
        /* A reader may have returned a packet before seeing this waiter.
         * Recheck the heap after publication, with the work-list lock free. */
        status = kernel_heap_allocate_zeroed(socket->heap, 1U, sizeof(*packet), (void **)&packet);
        if (status == KERNEL_HEAP_STATUS_EMPTY) return ERR_MEM;
#else
        return ERR_MEM;
#endif
    }
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    packet->payload = payload;
    if (socket->receive_retry) {
        KERNEL_IRQ_LOCK_SCOPE(state,&socket_state_lock);
        socket->receive_retry=0;
        work_remove_locked(&receive_wait, socket);
    }
    packet_publish(socket,packet,payload->tot_len);
    return ERR_OK;
}

static err_t tcp_data_sent(void *context, struct tcp_pcb *pcb, u16_t length)
{
    struct kernel_socket *socket = context;
    (void)pcb;
    (void)length;
    COST_ADD(TCP_ACK_CREDIT_EVENTS, 1);
    write_retry_disarm(socket);
    SOCKET_ENDPOINT(state,socket);
    uint32_t previous=tcp_admission_capacity_locked(socket);
    tcp_publish_capacity_locked(socket);
    if(tcp_admission_capacity_locked(socket)>previous) {
        COST_ADD(TCP_ACK_PUBLICATIONS, 1);
        COST_ADD(TCP_ACK_PUBLISHED_BYTES, tcp_admission_capacity_locked(socket)-previous);
#if BOAROS_COST_DIAGNOSTICS
        if (kernel_wait_queue_registrations(&socket->wait))
            COST_ADD(TCP_ACK_WAITER_PRESENT, 1);
#endif
#if BOAROS_USER_SMP
        struct socket_tcp_owner *owner = current_tcp_owner();
        struct kernel_socket **pending = owner ? owner->ack_waiter : NULL;
        if (pending && (!*pending || *pending == socket)) {
            if (*pending) COST_ADD(TCP_ACK_COALESCED, 1);
            else if (kernel_wait_queue_registrations(&socket->wait)) *pending = socket;
        } else
#endif
            socket_state_notify(state.owner,&socket->wait);
        socket_async(socket,KERNEL_ASYNC_POLL_OUT);
    }
    return ERR_OK;
}

static void tcp_failed(void *context, err_t error)
{
    struct kernel_socket *socket = context;
    { SOCKET_STATE(state);
      write_retry_disarm(socket);
      socket->receive_retry=0;
      work_remove_locked(&pool_wait,socket);work_remove_locked(&nic_wait,socket);work_remove_locked(&receive_wait,socket); }
    if (socket->listener && socket->pending_ready) {
        KERNEL_IRQ_LOCK_SCOPE(backlog,socket_endpoint_lock(socket->listener));
        socket->pending_ready = 0;
        if (!socket->listener->pending_live) __builtin_trap();
        socket->listener->pending_live--;
        kernel_irq_lock_release(&backlog);
        socket_schedule(socket->listener,SOCKET_WORK_LISTENER);
    }
    SOCKET_ENDPOINT(irq,socket);
    if (error == ERR_CLSD) {
        socket->tcp = 0; socket->peer_closed = 1; wake_socket(socket);socket_async(socket,KERNEL_ASYNC_POLL_HUP); socket_state_release(&irq); return;
    }
    int timer,device;
#if BOAROS_USER_SMP
    struct socket_tcp_owner *owner=current_tcp_owner();
    if (owner) {
        timer=owner->timer_running;
        device=socket->pending_error==-KERNEL_EIO;
    } else
#endif
    { timer=socket_timer_running; device=socket_device_failed; }
    socket->error = device ? -KERNEL_EIO : error == ERR_RST && socket->connecting
        ? -KERNEL_ECONNREFUSED : error == ERR_ABRT && timer
            ? -KERNEL_ETIMEDOUT : lwip_error(error);
    socket->pending_error = socket->error;
    socket->tcp = 0;
    socket->connecting = 0U;
    socket->connected = 0U;
    socket->peer_closed = 1U;
    wake_socket(socket);
    socket_async(socket,KERNEL_ASYNC_POLL_ERR);
    socket_async(socket,KERNEL_ASYNC_POLL_HUP);
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
    kernel_raw_lock_init(&child->endpoint_lock,KERNEL_RAW_RANK_OBJECT-1);
    { KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(listener));
      socket_filter_inherit_locked(child,listener);
      child->receive_timeout_ns=listener->receive_timeout_ns;
      child->send_timeout_ns=listener->send_timeout_ns;
      child->oobinline=listener->oobinline; }
    child->listener = listener; child->pending_ready = 1;
    child->heap = listener->heap;
    child->peercred=(struct unix_credentials){.uid=UINT32_MAX,.gid=UINT32_MAX};
    child->family = listener->family;
    child->v6only = listener->v6only;
    child->rx_limit = listener->rx_limit;
    child->tx_limit = listener->tx_limit;
    child->reuseaddr = listener->reuseaddr;
    child->keepalive = listener->keepalive;
    child->keep_idle = listener->keep_idle;
    child->keep_intvl = listener->keep_intvl;
    child->keep_cnt = listener->keep_cnt;
    pcb->keep_idle = child->keep_idle;
    pcb->keep_intvl = child->keep_intvl;
    pcb->keep_cnt = child->keep_cnt;
    child->nodelay = listener->nodelay;
    child->cork = listener->cork;
    child->broadcast=listener->broadcast;child->ipv4_ttl=listener->ipv4_ttl;
    child->multicast_ttl=listener->multicast_ttl;child->bound_device=pcb->netif_idx;
    child->ipv4_multicast_loop=listener->ipv4_multicast_loop;
    child->ipv4_multicast_all=listener->ipv4_multicast_all;
    child->multicast_index=listener->multicast_index;
    ip_addr_copy(child->multicast_source,listener->multicast_source);
    pcb->ttl=IP_IS_V4(&pcb->remote_ip)?child->ipv4_ttl:64;
    child->type = SOCKET_STREAM;
    child->tcp = pcb;
    if (child->nodelay) tcp_nagle_disable(pcb);
    pcb->boaros_cork = child->cork;
    child->connected = 1U;
    address_export(child, &pcb->local_ip, pcb->local_port, &child->saved_local);
    address_export(child, &pcb->remote_ip, pcb->remote_port, &child->saved_peer);
    kernel_wait_queue_init(&child->wait);kernel_wait_queue_init(&child->async_events);
    tcp_arg(pcb, child);
    tcp_err(pcb, tcp_failed);
    tcp_recv(pcb, tcp_data_received);
    tcp_sent(pcb, tcp_data_sent);
    tcp_publish_capacity_locked(child);
    inet_register(child);
    tcp_backlog_delayed(pcb);
    entry->child = child;
    SOCKET_ENDPOINT(state,listener);listener->pending_live++;
    if (listener->pending_tail != 0) {
        listener->pending_tail->next = entry;
    } else {
        listener->pending_head = entry;
    }
    listener->pending_tail = entry;
    wake_socket(listener);
    socket_async(listener,KERNEL_ASYNC_POLL_IN);
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
static int unix_autobind_if_needed(struct kernel_socket *socket)
{
    int bind;
    {SOCKET_STATE(state);bind=socket->passcred && !socket->unix_name;}
    if(!bind)return 0;
    KERNEL_LOCK_SCOPE(control);unix_control_acquire(&control);
    {SOCKET_STATE(state);if(socket->unix_name)return 0;}
    struct kernel_socket_address address={.family=KERNEL_SOCKET_AF_UNIX};
    return unix_bind(socket,&address);
}
static void unix_initialize(struct kernel_socket *socket,struct kernel_heap *heap,int type)
{
    socket->heap=heap;socket->type=(uint8_t)type;socket->family=KERNEL_SOCKET_AF_UNIX;
    socket->domain=KERNEL_SOCKET_DOMAIN_UNIX;socket->unix_references=1;
    {KERNEL_LOCK_SCOPE(gc);rights_acquire(&gc);socket->gc_next=unix_sockets;unix_sockets=socket;}
    socket->rx_limit=socket->tx_limit=212992;socket->peercred=(struct unix_credentials){.uid=UINT32_MAX,.gid=UINT32_MAX};
    kernel_wait_queue_init(&socket->wait);kernel_wait_queue_init(&socket->async_events);
}

int kernel_socket_create(struct kernel_heap *heap, int family, int type, int protocol,
                         struct kernel_socket **owner)
{
    struct kernel_socket *socket = 0;
    struct kernel_socket_protocol_scope old_status;

    if (heap == 0 || owner == 0 || *owner != 0 ||
        (type != SOCKET_DGRAM && type != SOCKET_STREAM &&
         !(type == SOCKET_RAW && (family == KERNEL_SOCKET_AF_NETLINK || family == KERNEL_SOCKET_AF_PACKET ||
             (family==KERNEL_SOCKET_AF_INET && protocol==1))) &&
         !(type == SOCKET_SEQPACKET && family == KERNEL_SOCKET_AF_UNIX)) ||
        ((family == KERNEL_SOCKET_AF_NETLINK || family == KERNEL_SOCKET_AF_PACKET) &&
         type != SOCKET_RAW && type != SOCKET_DGRAM) ||
        (family != KERNEL_SOCKET_AF_UNIX && family != KERNEL_SOCKET_AF_INET &&
         family != KERNEL_SOCKET_AF_INET6 && family != KERNEL_SOCKET_AF_NETLINK &&
         family != KERNEL_SOCKET_AF_PACKET)) {
        return -KERNEL_EINVAL;
    }
    enum kernel_heap_status status =
        kernel_heap_allocate_zeroed(heap, 1U, sizeof(*socket),
                                    (void **)&socket);
    if (status == KERNEL_HEAP_STATUS_EMPTY) {
        return -KERNEL_ENOMEM;
    }
    if (status != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    kernel_raw_lock_init(&socket->endpoint_lock,KERNEL_RAW_RANK_OBJECT-1);
    if(family==KERNEL_SOCKET_AF_UNIX) {unix_initialize(socket,heap,type);*owner=socket;return 0;}
    if (family==KERNEL_SOCKET_AF_NETLINK || family==KERNEL_SOCKET_AF_PACKET) {
        socket->heap=heap;socket->type=(uint8_t)type;socket->family=(uint8_t)family;
        socket->domain=family==KERNEL_SOCKET_AF_NETLINK ?
            KERNEL_SOCKET_DOMAIN_NETLINK : KERNEL_SOCKET_DOMAIN_PACKET;
        if (family==KERNEL_SOCKET_AF_PACKET) {
            struct socket_packet_options *options;
            if (kernel_heap_allocate_zeroed(heap,1,sizeof(*options),(void **)&options)) {
                kernel_heap_release(heap,socket);
                return -KERNEL_ENOMEM;
            }
            kernel_mutex_init(&options->configuration,12,(uintptr_t)options);
            socket->packet_options=options;
        }
        socket->netlink_references=1;
        socket->rx_limit=socket->tx_limit=212992;
        socket->peercred=(struct unix_credentials){.uid=UINT32_MAX,.gid=UINT32_MAX};
        kernel_wait_queue_init(&socket->wait);kernel_wait_queue_init(&socket->async_events);
        if (socket->domain==KERNEL_SOCKET_DOMAIN_PACKET) {
            kernel_socket_protocol_enter(&old_status);
            kernel_socket_network_initialize();
            packet_tap_update(socket,0,lwip_ntohs((uint16_t)protocol));
            kernel_socket_protocol_leave(&old_status);
        }
        *owner=socket;return 0;
    }
    socket->peercred=(struct unix_credentials){.uid=UINT32_MAX,.gid=UINT32_MAX};
    kernel_socket_protocol_enter(&old_status);
    kernel_socket_network_initialize();
    socket->heap = heap;
    socket->type = (uint8_t)type;
    socket->family = (uint8_t)family;
    socket->domain = KERNEL_SOCKET_DOMAIN_INET;
    socket->rx_limit = type==SOCKET_DGRAM ? UDP_RECEIVE_LIMIT : 65536U;
    socket->tx_limit = 65536U;
    socket->ipv4_ttl=64;socket->multicast_ttl=1;
    socket->ipv6_multicast_hops=1;socket->ipv6_multicast_loop=1;socket->ipv6_multicast_all=1;
    socket->ipv4_multicast_loop=1;socket->ipv4_multicast_all=1;
    kernel_wait_queue_init(&socket->wait);kernel_wait_queue_init(&socket->async_events);
    if(type==SOCKET_RAW) {
        socket->raw=raw_new_ip_type(IPADDR_TYPE_V4,(u8_t)protocol);
        if(socket->raw) {socket->raw->ttl=64;raw_set_multicast_ttl(socket->raw,1);raw_recv(socket->raw,raw_received,socket);}
    } else if (type == SOCKET_DGRAM) {
        socket->udp = udp_new_ip_type(family == KERNEL_SOCKET_AF_INET ?
                                      IPADDR_TYPE_V4 : IPADDR_TYPE_ANY);
        if (socket->udp != 0) {socket->udp->ttl=64;udp_set_multicast_ttl(socket->udp,1);udp_recv(socket->udp, udp_received, socket);}
    } else {
        socket->tcp = tcp_new_ip_type(family == KERNEL_SOCKET_AF_INET ?
                                      IPADDR_TYPE_V4 : IPADDR_TYPE_ANY);
        if (socket->tcp != 0) {
            socket->tcp->ttl=64;
            socket->keep_idle=socket->tcp->keep_idle;
            socket->keep_intvl=socket->tcp->keep_intvl;
            socket->keep_cnt=socket->tcp->keep_cnt;
            tcp_arg(socket->tcp, socket);
            tcp_err(socket->tcp, tcp_failed);
            tcp_recv(socket->tcp, tcp_data_received);
            tcp_sent(socket->tcp, tcp_data_sent);
        }
    }
    if ((type == SOCKET_DGRAM && socket->udp == 0) ||
        (type == SOCKET_RAW && socket->raw == 0) ||
        (type == SOCKET_STREAM && socket->tcp == 0)) {
        kernel_socket_protocol_leave(&old_status);
        kernel_heap_release(heap, socket);
        return -KERNEL_ENOMEM;
    }
    inet_register(socket);
    kernel_socket_protocol_leave(&old_status);
    *owner = socket;
    return 0;
}

int kernel_socket_pair(struct kernel_heap *heap,int type,
    struct kernel_socket **owner_a,struct kernel_socket **owner_b)
{
    if(!heap || !owner_a || !owner_b || *owner_a || *owner_b)return -KERNEL_EINVAL;
    struct kernel_socket *a=NULL,*b=NULL;
    int result=kernel_socket_create(heap,KERNEL_SOCKET_AF_UNIX,type, 0,&a);
    if(result)return result;
    result=kernel_socket_create(heap,KERNEL_SOCKET_AF_UNIX,type, 0,&b);
    if(result) {kernel_socket_destroy(a);return result;}
    struct unix_credentials cred=unix_current_cred();
    if(cred.pid)kernel_task_identity_retain(cred.pid);
    SOCKET_STATE(state);
    a->connected=b->connected=1;a->peercred=b->peercred=cred;
    unix_link_peer_locked(a,b);unix_link_peer_locked(b,a);
    unix_watch_peer_locked(a);unix_watch_peer_locked(b);
    *owner_a=a;*owner_b=b;return 0;
}

static void udp_recycle_locked(struct kernel_socket *socket, struct socket_packet *packet)
{
    packet->payload=NULL;
    packet->next=socket->udp_spares;
    socket->udp_spares=packet;
    socket->udp_spare_count++;
}

static void release_inet_packets(struct kernel_socket *socket, struct socket_packet *packets)
{
    if (!packets) return;
    struct socket_packet *spares=NULL;
    unsigned remaining=socket->udp && !socket->destroying?KERNEL_NETWORK_RECEIVE_BATCH:0;
    do {
        struct socket_packet *next = packets->next;
        if (packets->payload) pbuf_free(packets->payload);
        if (remaining) {
            remaining--;
            packets->next=spares;
            spares=packets;
        } else kernel_heap_release(socket->heap,packets);
        packets = next;
    } while (packets);
    if (spares) {
        SOCKET_ENDPOINT(state,socket);
        while (spares && socket->udp_spare_count<KERNEL_NETWORK_RECEIVE_BATCH) {
            struct socket_packet *packet=spares;
            spares=packet->next;
            udp_recycle_locked(socket,packet);
        }
    }
    while (spares) {
        struct socket_packet *next=spares->next;
        kernel_heap_release(socket->heap,spares);
        spares=next;
    }
    receive_capacity();
}

static void retire_received(struct kernel_socket *socket)
{
    KERNEL_IRQ_LOCK_SCOPE(irq,socket_endpoint_lock(socket));
    struct socket_packet *packets = socket->retired_head;
    uint32_t credit = socket->receive_credit;
    socket->retired_head = socket->retired_tail = 0;
    socket->receive_credit = 0;
    kernel_irq_lock_release(&irq);
    release_inet_packets(socket, packets);
    if (socket->tcp && !socket->listening) while (credit) {
        u16_t part = credit > UINT16_MAX ? UINT16_MAX : (u16_t)credit;
        tcp_recved(socket->tcp, part); credit -= part;
    }
}

/* A pending child can be destroyed recursively. Keep the UNIX graph and
 * notification frames off the stack of the other families' shutdown paths. */
static __attribute__((noinline)) void unix_destroy(struct kernel_socket *socket)
{
    KERNEL_LOCK_SCOPE(control);unix_control_acquire(&control);
    KERNEL_LOCK_SCOPE(gc);rights_acquire(&gc);
    KERNEL_IRQ_LOCK_SCOPE(state,&socket_state_lock);
    if(socket->read_request || socket->tx_reserved || socket->active_writes || socket->destroying)__builtin_trap();
    socket->destroying=1;
    struct kernel_socket **gc_link=&unix_sockets;
    while(*gc_link!=socket) {if(!*gc_link)__builtin_trap();gc_link=&(*gc_link)->gc_next;}
    *gc_link=socket->gc_next;socket->gc_next=NULL;socket->gc_file=NULL;
    struct kernel_socket_name *name=socket->unix_name,*peer_name=socket->unix_peer_name;
    socket->unix_peer_name=NULL;
    if(name && socket->explicit_bind) {
        struct kernel_socket **link=&unix_names[unix_socket_hash(socket)];
        while(*link!=socket) {if(!*link)__builtin_trap();link=&(*link)->unix_hash_next;}
        *link=socket->unix_hash_next;socket->unix_hash_next=NULL;
    }
    kernel_irq_lock_release(&state);
    for(struct socket_packet *p=socket->packets_head;p;p=p->next)control_unqueue(p->control);
    unix_unwatch_peer(socket);
    SOCKET_STATE(own);
    struct kernel_socket *peer=unix_take_peer_locked(socket);
    if(peer && socket_connection_oriented(socket) && (socket->packets_head || socket->listener))peer->pending_error=-KERNEL_ECONNRESET;
    /* The reciprocal-edge removal below publishes the peer's async hangup. */
    if(peer && !peer->destroying)wake_socket(peer);
    socket_state_release(&own);unix_put(peer);
    for(;;) {
        KERNEL_IRQ_LOCK_SCOPE(incoming,&socket_state_lock);
        struct kernel_socket *sender=socket->unix_peers;
        if(!sender)break;
        unix_get_locked(sender);kernel_irq_lock_release(&incoming);
        unix_unwatch_peer(sender);
        SOCKET_STATE(detach);
        struct kernel_socket *target=unix_take_peer_locked(sender);
        if(target!=socket)__builtin_trap();
        sender->peer_closed=1;
        if(socket_connection_oriented(socket)) {
            if(sender->unix_peer_name)__builtin_trap();
            sender->unix_peer_name=name;unix_name_get_locked(name);
            if(socket->packets_head || socket->listener)sender->pending_error=-KERNEL_ECONNRESET;
            sender->read_closed=sender->write_closed=1;
        }
        if(!sender->destroying) {
            wake_socket(sender);
            if (socket_connection_oriented(socket)) socket_async(sender,KERNEL_ASYNC_POLL_HUP);
        }
        socket_state_release(&detach);unix_put(target);unix_put(sender);
    }
    kernel_lock_release(&gc);
    kernel_lock_release(&control);
    unix_name_put(peer_name);
    while(socket->packets_head) {
        struct socket_packet *packet=socket->packets_head;socket->packets_head=packet->next;
        unix_uncharge(packet);kernel_socket_control_release(&packet->control);kernel_heap_release(socket->heap,packet);
    }
    while(socket->pending_head) {
        struct socket_pending *entry=socket->pending_head;socket->pending_head=entry->next;
        /* The queued child still records its embryonic connection at close. */
        entry->child->pending_ready=0;
        kernel_socket_destroy(entry->child);kernel_heap_release(socket->heap,entry);
    }
    uintptr_t irq=arch_interrupt_save();
    socket_async_close(socket);
    if(kernel_wait_queue_close(&socket->wait)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
    for(;;) {
        enum kernel_scheduler_status status=kernel_wait_queue_destroy(&socket->wait);
        if(status==KERNEL_SCHEDULER_STATUS_OK)break;
        if(status!=KERNEL_SCHEDULER_STATUS_BUSY)__builtin_trap();
        kernel_wait_backend_quiesce();
    }
    arch_interrupt_restore(irq);
    KERNEL_IRQ_LOCK_SCOPE(done,&socket_state_lock);socket->destroying=2;kernel_irq_lock_release(&done);
    unix_put(socket);
}

/* Binding is control-plane only. Registry ownership never nests endpoint locks. */
static struct kernel_socket *netlink_ports;
static uint32_t netlink_next_port=0x80000000U;
static int netlink_bind(struct kernel_socket *socket,uint32_t port,int automatic)
{
    kernel_pid_t pid=0;
    if(!port && kernel_task_current())(void)kernel_task_user_tgid(kernel_task_current(),&pid);
    uint32_t preferred=(uint32_t)pid;
    SOCKET_STATE(state);
    if(socket->netlink_port)return automatic || !port || socket->netlink_port==port?0:-KERNEL_EINVAL;
    uint32_t selected=port?port:preferred;
    for(;;) {
        int used=!selected;
        for(struct kernel_socket *p=netlink_ports;p;p=p->netlink_next)
            if(p->netlink_port==selected) {used=1;break;}
        if(!used)break;
        if(port)return -KERNEL_EADDRINUSE;
        selected=netlink_next_port++;
        if(!netlink_next_port)netlink_next_port=0x80000000U;
    }
    socket->netlink_port=selected;socket->netlink_next=netlink_ports;netlink_ports=socket;
    return 0;
}
static void netlink_unbind(struct kernel_socket *socket)
{
    SOCKET_STATE(state);
    struct kernel_socket **p=&netlink_ports;
    while(*p && *p!=socket)p=&(*p)->netlink_next;
    if(*p)*p=socket->netlink_next;
}
static int netlink_put_locked(struct kernel_socket *socket)
{
    if(!socket->netlink_references)__builtin_trap();
    int release=!--socket->netlink_references;
    if(release && (!socket->destroying || socket->record_memory))__builtin_trap();
    return release;
}
static void netlink_storage_free(struct kernel_socket *socket)
{
    uintptr_t interrupts=arch_interrupt_save();
    for(;;) {
        enum kernel_scheduler_status status=kernel_wait_queue_destroy(&socket->wait);
        if(status==KERNEL_SCHEDULER_STATUS_OK)break;
        if(status!=KERNEL_SCHEDULER_STATUS_BUSY)__builtin_trap();
        kernel_wait_backend_quiesce();
    }
    arch_interrupt_restore(interrupts);
    kernel_heap_release(socket->heap,socket);
}

int kernel_socket_packet_option_set(struct kernel_socket *socket, unsigned option, int value)
{
    struct socket_packet_options *options=socket->packet_options;
    if (!options) return -KERNEL_ENOPROTOOPT;
    if (option==KERNEL_PACKET_VERSION) {
        if (value<0 || value>(int)PACKET_VERSION_V3) return -KERNEL_EINVAL;
    } else if (option==KERNEL_PACKET_RESERVE) {
        /* Leave room for link offsets in the packet header's uint16_t fields. */
        if (value<0 || value>UINT16_MAX-96) return -KERNEL_EINVAL;
    } else return -KERNEL_ENOPROTOOPT;
    KERNEL_LOCK_SCOPE(configuration);
    kernel_mutex_lock(&options->configuration,&configuration);
    if (options->ring) return -KERNEL_EBUSY;
    if (option==KERNEL_PACKET_VERSION) options->version=(uint32_t)value;
    else options->reserve=(uint32_t)value;
    return 0;
}

int kernel_socket_packet_option_get(struct kernel_socket *socket, unsigned option, uint32_t selection,
    uint32_t values[3], unsigned *count)
{
    struct socket_packet_options *options=socket->packet_options;
    if (!options) return -KERNEL_ENOPROTOOPT;
    /* PACKET_STATISTICS samples and clears the counts under the packet producer
     * qualification. Start the service before taking the configuration lock. */
    KERNEL_SERVICE_SCOPE_IF(service,option==KERNEL_PACKET_STATISTICS);
    KERNEL_LOCK_SCOPE(configuration);
    kernel_mutex_lock(&options->configuration,&configuration);
    *count=1;
    switch (option) {
    case KERNEL_PACKET_VERSION: values[0]=options->version;return 0;
    case KERNEL_PACKET_RESERVE: values[0]=options->reserve;return 0;
    case KERNEL_PACKET_HDRLEN:
        if (selection>PACKET_VERSION_V3) return -KERNEL_EINVAL;
        values[0]=selection==PACKET_VERSION_V3 ? PACKET_V3_HEADER_LENGTH : 52U;
        return 0;
    case KERNEL_PACKET_STATISTICS: {
        struct kernel_socket_protocol_scope protocol;
        kernel_socket_protocol_enter(&protocol);
        values[0]=options->packets+options->drops;
        values[1]=options->drops;
        values[2]=options->freezes;
        options->packets=options->drops=options->freezes=0;
        kernel_socket_protocol_leave(&protocol);
        *count=options->version==PACKET_VERSION_V3 ? 3 : 2;
        return 0;
    }
    default: return -KERNEL_ENOPROTOOPT;
    }
}

int kernel_socket_packet_ring_configure(struct kernel_socket *socket,
    const struct kernel_socket_packet_ring_request *request)
{
    struct socket_packet_options *options=socket->packet_options;
    if (!options) return -KERNEL_ENOPROTOOPT;
    KERNEL_SERVICE_SCOPE(service);
    KERNEL_LOCK_SCOPE(configuration);
    kernel_mutex_lock(&options->configuration,&configuration);
    struct packet_ring *old=options->ring;
    if (old && (request->block_count ||
        __atomic_load_n(packet_ring_map_count(old),__ATOMIC_ACQUIRE))) return -KERNEL_EBUSY;
    if (options->version!=PACKET_VERSION_V3) return -KERNEL_EOPNOTSUPP;
    struct packet_ring *new_ring=NULL;
    if (request->block_count) {
        int error=packet_ring_create(socket->heap,request,options->reserve,&new_ring);
        if (error) return error;
    } else if (request->frame_count) return -KERNEL_EINVAL;

    struct kernel_socket_protocol_scope protocol;
    kernel_socket_protocol_enter(&protocol);
    SOCKET_ENDPOINT(state,socket);
    if (socket->read_request) {
        socket_state_release(&state);
        kernel_socket_protocol_leave(&protocol);
        packet_ring_destroy(new_ring);
        return -KERNEL_EBUSY;
    }
    struct socket_packet *packet=socket->packets_head;
    socket->packets_head=socket->packets_tail=NULL;
    socket->rx_bytes=socket->record_memory=0;
    options->ring=new_ring;
    packet_timer_disarm(options);
    socket_state_release(&state);
    kernel_socket_protocol_leave(&protocol);
    kernel_lock_release(&configuration);
    packet_ring_destroy(old);
    while (packet) {
        struct socket_packet *next=packet->next;
        kernel_heap_release(socket->heap,packet);
        packet=next;
    }
    return 0;
}

int kernel_socket_packet_ring_mmap_begin(struct kernel_socket *socket, uint64_t offset,
    uint64_t aligned_length, struct kernel_lock_guard *guard, uint64_t **mapped_bytes)
{
    struct socket_packet_options *options=socket->packet_options;
    if (!options) return -KERNEL_ENODEV;
    kernel_mutex_lock(&options->configuration,guard);
    struct packet_ring *ring=options->ring;
    if (!ring || offset || aligned_length!=packet_ring_bytes(ring)) {
        kernel_lock_release(guard);
        return -KERNEL_EINVAL;
    }
    *mapped_bytes=packet_ring_map_count(ring);
    return 0;
}

int kernel_socket_packet_ring_mmap_page(struct kernel_socket *socket, uint64_t offset,
    uint64_t *physical)
{
    struct socket_packet_options *options=socket->packet_options;
    if (!options) return -KERNEL_ENODEV;
    /* The caller holds the configure guard until mapping publication. A live
     * mapping's charge and file reference also prevent ring replacement. */
    struct packet_ring *ring=options->ring;
    return ring ? packet_ring_page(ring,offset,physical) : -KERNEL_EINVAL;
}

static void record_endpoint_destroy(struct kernel_socket *socket)
{
    struct socket_packet_options *packet_options=NULL;
    if (socket->domain==KERNEL_SOCKET_DOMAIN_NETLINK) netlink_unbind(socket);
    else {
        struct kernel_socket_protocol_scope protocol;
        kernel_socket_protocol_enter(&protocol);
        packet_tap_unlink(socket);
        SOCKET_ENDPOINT(state,socket);
        packet_options=socket->packet_options;
        packet_timer_disarm(packet_options);
        socket->packet_options=NULL;
        socket_state_release(&state);
        kernel_socket_protocol_leave(&protocol);
    }
    SOCKET_ENDPOINT(state,socket);
    if(socket->destroying || socket->read_request || socket->active_writes)__builtin_trap();
    socket->destroying=1;
    struct socket_packet *packets=socket->packets_head;
    /* Pending registrations keep their own charges and storage references. */
    uint64_t queued_memory=0;
    for(struct socket_packet *p=packets;p;p=p->next)queued_memory+=packet_charge(p->length);
    if(socket->record_memory<queued_memory)__builtin_trap();
    socket->record_memory-=(uint32_t)queued_memory;
    socket->packets_head=socket->packets_tail=NULL;socket->rx_bytes=0;
    socket_state_release(&state);
    if (packet_options) {
        packet_ring_destroy(packet_options->ring);
        kernel_heap_release(socket->heap,packet_options);
    }
    while(packets) {
        struct socket_packet *next=packets->next;
        kernel_heap_release(socket->heap,packets);packets=next;
    }
    /* Retain the file's storage reference through all close callbacks. Queue
     * storage is destroyed by the last real user, after its wait has ended. */
    socket_async_close(socket);
    if(kernel_wait_queue_close(&socket->wait)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
    SOCKET_ENDPOINT(done,socket);int release=netlink_put_locked(socket);socket_state_release(&done);
    if(release)netlink_storage_free(socket);
}

void kernel_socket_destroy(struct kernel_socket *socket)
{
    struct kernel_socket_protocol_scope old_status;
    if (socket == 0) __builtin_trap();
    { KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(socket));
      struct kernel_bpf_program *program=socket->filter;
      __atomic_store_n(&socket->filter,NULL,__ATOMIC_RELEASE);
      kernel_irq_lock_release(&state);if(program)kernel_bpf_program_put(program); }
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {unix_destroy(socket);return;}
    if (socket->domain==KERNEL_SOCKET_DOMAIN_NETLINK || socket->domain==KERNEL_SOCKET_DOMAIN_PACKET) {
        record_endpoint_destroy(socket);return;
    }
    kernel_socket_protocol_enter(&old_status);
#if BOAROS_USER_SMP
    struct kernel_socket_protocol_scope remote;
    unsigned cpu=tcp_socket_cpu(socket);
    if (cpu) tcp_control_enter(&old_status,&remote,cpu);
#endif
    KERNEL_IRQ_LOCK_SCOPE(state,&socket_state_lock);
    if (socket->read_request != 0 || socket->tx_reserved || socket->writes_head || socket->active_writes) __builtin_trap();
    socket->destroying = 1;
    kernel_irq_lock_release(&state);
    if (socket->listener && socket->pending_ready) {
        KERNEL_IRQ_LOCK_SCOPE(backlog,socket_endpoint_lock(socket->listener));
        if (!socket->listener->pending_live) __builtin_trap();
        socket->listener->pending_live--;
    }
    socket->listener = 0; socket->pending_ready = 0;
    { SOCKET_STATE(work);
      work_remove_locked(socket_ready_queue(socket),socket);work_remove_locked(&pool_wait,socket);
      const struct socket_link *retry=&socket->work_links[nic_wait.slot];
      if (retry->queued) orphan_nic_register_locked(work.owner,socket->tcp,retry->epoch);
      work_remove_locked(&nic_wait,socket);work_remove_locked(&receive_wait,socket); }
    write_retry_disarm(socket);
    if (socket->ipv4_members) {
        /* During remote TCP close the shared IP lock is dropped to acquire
         * the TCP owner. Restore IP protection for interface state changes. */
#if BOAROS_USER_SMP
        if (cpu) boaros_lwip_ip_lock(&old_status.ip_guard);
#endif
        socket_ipv4_members_release(socket);
#if BOAROS_USER_SMP
        if (cpu) boaros_lwip_ip_unlock(&old_status.ip_guard);
#endif
    }
    socket_ipv6_members_release(socket);
    retire_received(socket);
    if (!socket->inet_previous || *socket->inet_previous!=socket) __builtin_trap();
    *socket->inet_previous=socket->inet_next;
    if (socket->inet_next) socket->inet_next->inet_previous=socket->inet_previous;
    socket->inet_next=NULL;socket->inet_previous=NULL;
    while (socket->packets_head != 0) {
        struct socket_packet *packet = socket->packets_head;
        socket->packets_head = packet->next;
        if (packet->payload != 0) {
            kernel_socket_pbuf_free(packet->payload);
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
    if (socket->udp != 0) {
        while (socket->udp_spares) {
            struct socket_packet *packet=socket->udp_spares;
            socket->udp_spares=packet->next;
            kernel_heap_release(socket->heap,packet);
        }
        udp_pending_discard(socket);
        udp_remove(socket->udp);
    }
    if (socket->raw != 0) {raw_recv(socket->raw,NULL,NULL);raw_remove(socket->raw);socket->raw=NULL;}
    if (socket->tcp != 0) {
        if (!socket->listening) {
            boaros_lwip_tcp_push(socket->tcp);
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
    socket_async_close(socket);
    if(kernel_wait_queue_close(&socket->wait)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
    for(;;) {
        enum kernel_scheduler_status status=kernel_wait_queue_destroy(&socket->wait);
        if(status==KERNEL_SCHEDULER_STATUS_OK)break;
        if(status!=KERNEL_SCHEDULER_STATUS_BUSY)__builtin_trap();
        kernel_wait_backend_quiesce();
    }
    arch_interrupt_restore(wait_irq);
    struct kernel_heap *heap = socket->heap;
#if BOAROS_USER_SMP
    if (cpu) tcp_control_leave(&old_status,&remote);
#endif
    kernel_socket_protocol_leave(&old_status);
    kernel_heap_release(heap, socket);
    receive_capacity();
}

int kernel_socket_bind(struct kernel_socket *socket,
                       const struct kernel_socket_address *address)
{
    if (socket->domain==KERNEL_SOCKET_DOMAIN_PACKET) {
        if (!address || address->family!=KERNEL_SOCKET_AF_PACKET) return -KERNEL_EINVAL;
        struct kernel_socket_protocol_scope protocol;
        kernel_socket_protocol_enter(&protocol);
        uint32_t index=address->scope;
        int error=index && (index>255 || !netif_get_by_index((uint8_t)index)) ? -KERNEL_ENODEV : 0;
        if (!error) packet_tap_update(socket,index,address->port ? address->port : socket->packet_protocol);
        kernel_socket_protocol_leave(&protocol);
        return error;
    }
    if(socket->domain==KERNEL_SOCKET_DOMAIN_NETLINK) {
        if(!address || address->family!=KERNEL_SOCKET_AF_NETLINK)return -KERNEL_EINVAL;
        uint32_t groups;__builtin_memcpy(&groups,address->bytes,4);
        if(groups)return -KERNEL_EOPNOTSUPP;
        return netlink_bind(socket,address->scope,0);
    }
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        KERNEL_LOCK_SCOPE(control);unix_control_acquire(&control);return unix_bind(socket,address);
    }
    struct kernel_socket_protocol_scope scope; socket_protocol_enter(&scope,socket);
    ip_addr_t local;
    if (socket->domain != KERNEL_SOCKET_DOMAIN_INET) { kernel_socket_protocol_leave(&scope); return -KERNEL_EINVAL; }
    int result = address_import(socket, address, &local, SOCKET_ADDRESS_BIND);
    if (result != 0) { kernel_socket_protocol_leave(&scope); return result; }
    if ((socket->udp != 0 && socket->udp->local_port != 0) ||
        (socket->tcp != 0 && socket->tcp->local_port != 0)) { kernel_socket_protocol_leave(&scope); return -KERNEL_EINVAL; }
    if (IP_IS_V6(&local) && ip6_addr_islinklocal(ip_2_ip6(&local))) {
        /* Like IPv6 bind, an explicit address scope selects the bound device;
         * connect checks conflicts, while sendto retains an existing device. */
        SOCKET_ENDPOINT(state,socket);socket->bound_device=ip6_addr_zone(ip_2_ip6(&local));
        socket_ip_pcb(socket)->netif_idx=socket->bound_device;
    }
    int low_port_denied=!socket->raw && address->port && address->port<1024 &&
        !kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_NET_BIND_SERVICE);
    if(socket->family==KERNEL_SOCKET_AF_INET6 && low_port_denied) {
        kernel_socket_protocol_leave(&scope);return -KERNEL_EACCES;
    }
    if (!ip_addr_isany(&local) &&
        !(socket->type==SOCKET_DGRAM && IP_IS_V6(&local) && ip6_addr_ismulticast(ip_2_ip6(&local))) &&
        !(IP_IS_V4(&local) ? ip4_addr_isloopback(ip_2_ip4(&local)) : ip6_addr_isloopback(ip_2_ip6(&local)))) {
        int found = 0;
        for (struct netif *n = netif_list; n; n = n->next) {
            if (IP_IS_V4(&local) && ip4_addr_eq(ip_2_ip4(&local), netif_ip4_addr(n))) found = 1;
            if (IP_IS_V6(&local)) for (unsigned i=0;i<LWIP_IPV6_NUM_ADDRESSES;i++)
                if (ip6_addr_isvalid(netif_ip6_addr_state(n,i)) &&
                    ip6_addr_eq(ip_2_ip6(&local),netif_ip6_addr(n,i))) found=1;
        }
        if (!found) { kernel_socket_protocol_leave(&scope); return -KERNEL_EADDRNOTAVAIL; }
    }

    if(low_port_denied) {
        kernel_socket_protocol_leave(&scope);return -KERNEL_EACCES;
    }
    if(socket->raw) {
        if(socket->connected) {kernel_socket_protocol_leave(&scope);return -KERNEL_EINVAL;}
        err_t error=raw_bind(socket->raw,&local);
        if(!error) {socket->explicit_bind=1;ip_addr_copy(socket->udp_bind_ip,local);}
        kernel_socket_protocol_leave(&scope);return lwip_error(error);
    }
    result = port_conflict(socket, &local, address->port, 0);
    if (result != 0) { kernel_socket_protocol_leave(&scope); return result; }
    err_t error = socket->type == SOCKET_DGRAM
        ? udp_bind(socket->udp, &local, address->port)
        : tcp_bind(socket->tcp, &local, address->port);
    if (error == ERR_OK) {
        socket->explicit_bind = 1;
        socket->explicit_port = address->port != 0;
        if (socket->udp != 0) ip_addr_copy(socket->udp_bind_ip, socket->udp->local_ip);
        if (IP_IS_V6(&local) && !ip_addr_isany(&local)) { KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(socket));socket->v6only = 1; }
    }
    kernel_socket_protocol_leave(&scope);
    return lwip_error(error);
}

int kernel_socket_getname(struct kernel_socket *socket,
                          struct kernel_socket_address *address)
{
    if (socket && socket->domain==KERNEL_SOCKET_DOMAIN_PACKET) {
        if (!address) return -KERNEL_EINVAL;
        struct kernel_socket_protocol_scope protocol;
        kernel_socket_protocol_enter(&protocol);
        *address=(struct kernel_socket_address){.family=KERNEL_SOCKET_AF_PACKET,
            .scope=socket->packet_index,.port=socket->packet_protocol,.link_size=12};
        struct netif *interface=socket->packet_index && socket->packet_index<=255 ?
            netif_get_by_index((uint8_t)socket->packet_index) : NULL;
        if (interface) {
            address->link_length=6;
            address->link_size=18;
            address->link_type=interface==loopback_netif() ? 772 : 1;
            if (interface->hwaddr_len==6) __builtin_memcpy(address->link_address,interface->hwaddr,6);
        }
        kernel_socket_protocol_leave(&protocol);
        return 0;
    }
    if(socket && socket->domain==KERNEL_SOCKET_DOMAIN_NETLINK) {
        if(!address)return -KERNEL_EINVAL;
        SOCKET_STATE(state);
        *address=(struct kernel_socket_address){.family=KERNEL_SOCKET_AF_NETLINK,.scope=socket->netlink_port};return 0;
    }
    if(socket && socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        SOCKET_STATE(state);unix_export_locked(socket->unix_name,address);return 0;
    }
    struct kernel_socket_protocol_scope scope; socket_protocol_enter(&scope,socket);
    if (socket == 0 || address == 0) { kernel_socket_protocol_leave(&scope); return -KERNEL_EINVAL; }
    if (socket->tcp == 0 && socket->connected && socket->type == SOCKET_STREAM) {
        *address = socket->saved_local;
        kernel_socket_protocol_leave(&scope); return 0;
    }
    if(socket->raw) {
        address_export(socket,&socket->raw->local_ip,socket->raw->protocol,address);
    } else if (socket->type == SOCKET_DGRAM) {
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
    if (socket->domain==KERNEL_SOCKET_DOMAIN_PACKET) return -KERNEL_EOPNOTSUPP;
    if(socket->domain==KERNEL_SOCKET_DOMAIN_NETLINK) {
        *address=(struct kernel_socket_address){.family=KERNEL_SOCKET_AF_NETLINK};return 0;
    }
    KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(socket));
    if(!socket->connected)return -KERNEL_ENOTCONN;
    if(socket->type==SOCKET_RAW && socket->domain==KERNEL_SOCKET_DOMAIN_INET &&
       !socket->saved_peer.port)return -KERNEL_ENOTCONN;
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        if(!socket->peer && !socket_connection_oriented(socket))return -KERNEL_ENOTCONN;
        unix_export_locked(socket->peer?socket->peer->unix_name:socket->unix_peer_name,address);
    } else *address=socket->saved_peer;
    return 0;
}

/* Path lookup can fault and pending-child rollback can close sockets. Keep
 * these frames separate from the PCB control path in kernel_socket_connect. */
static __attribute__((noinline)) int unix_connect(struct kernel_socket *socket,const struct kernel_socket_address *address,int nonblocking)
{
    int autobind=unix_autobind_if_needed(socket);if(autobind)return autobind;
    if(socket->type==SOCKET_DGRAM) {
        if(address->family && unix_address_validate(address,0))return -KERNEL_EINVAL;
        KERNEL_LOCK_SCOPE(control);unix_control_acquire(&control);
        struct kernel_socket *target=NULL;
        int result=address->family?unix_lookup(socket,address,&target):0;
        if(result)return result;
        { KERNEL_IRQ_LOCK_SCOPE(check,&socket_state_lock);
          result=target && target->destroying?-KERNEL_ECONNREFUSED:
                 target && target->peer && target->peer!=socket?-KERNEL_EPERM:0;
        }
        if(result) {unix_put(target);return result;}
        unix_unwatch_peer(socket);
        /* Becoming reciprocal removes the target's former queue-space
         * subscription. Control owns topology; finish old callback borrows
         * outside the endpoint lock before publishing the new relation. */
        if(target && target!=socket && target->peer==socket)unix_unwatch_peer(target);
        KERNEL_LOCK_SCOPE(gc);rights_acquire(&gc);
        SOCKET_STATE(state);
        struct kernel_socket *old=unix_take_peer_locked(socket);
        if(target) {unix_link_peer_locked(socket,target);unix_watch_peer_locked(socket);target->connected=1;}
        int notify_old=old && old!=target && old!=socket && old->peer==socket;
        int notify_target=target && target!=socket && target->peer==socket;
        if(notify_old)unix_watch_peer_locked(old);
        if(notify_target)target->unix_space_wait=0;
        struct socket_packet *discard=NULL;int report_error=0;
        if(old && old!=target) {
            if(socket->read_request) {
                struct socket_packet *reserved=socket->packets_head;
                discard=reserved->next;reserved->next=NULL;socket->packets_tail=reserved;
                socket->rx_bytes=reserved->length?reserved->length:1;socket->unix_queued=1;
            } else {
                discard=socket->packets_head;socket->packets_head=socket->packets_tail=NULL;
                socket->rx_bytes=socket->unix_queued=0;
            }
            if(discard && !old->destroying && old->peer==socket) {
                old->pending_error=-KERNEL_ECONNRESET;report_error=1;
            }
        }
        socket->connected=target!=NULL;socket->peer_closed=0;socket->pending_error=0;
        wake_socket(socket);
        socket_state_release(&state);
        /* Each notification guard holds at most two queues. The peer refs
         * above survive this separate delivery of changed queue eligibility. */
        { SOCKET_STATE(changed);
          if(notify_old && !old->destroying) {
              wake_socket(old);if(report_error)socket_async(old,KERNEL_ASYNC_POLL_ERR);
          }
          if(notify_target && !target->destroying)wake_socket(target);
        }
        for(struct socket_packet *p=discard;p;p=p->next)control_unqueue(p->control);
        kernel_lock_release(&gc);kernel_lock_release(&control);
        while(discard) {struct socket_packet *next=discard->next;unix_uncharge(discard);kernel_socket_control_release(&discard->control);kernel_heap_release(socket->heap,discard);discard=next;}
        unix_put(old);unix_put(target);return 0;
    }
    int result=unix_address_validate(address,0);if(result)return result;
    struct kernel_socket *target=NULL;
    /* Hold the resolved listener across allocation, then recheck its live state. */
    result=unix_lookup(socket,address,&target);if(result)return result;
    struct kernel_socket *child=NULL;
    result=kernel_socket_create(socket->heap,KERNEL_SOCKET_AF_UNIX,socket->type, 0,&child);
    if(result) {unix_put(target);return result;}
    struct socket_pending *entry=NULL;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(socket->heap,1,sizeof(*entry),(void **)&entry);
    if(status!=KERNEL_HEAP_STATUS_OK) {unix_put(target);kernel_socket_destroy(child);return -KERNEL_ENOMEM;}
    uint64_t deadline=unix_deadline(socket);
    struct unix_credentials cred __attribute__((cleanup(unix_cred_release)))=unix_current_cred();
    struct unix_credentials retired __attribute__((cleanup(unix_cred_release)))={0};
    for(;;) {
        KERNEL_LOCK_SCOPE(gc);rights_acquire(&gc);
        SOCKET_STATE(state);
        result=socket->connected?-KERNEL_EISCONN:socket->listening?-KERNEL_EINVAL:
            target->destroying || !target->listening || target->read_closed?-KERNEL_ECONNREFUSED:0;
        if(!result && target->pending_live>target->unix_backlog) {
            result=unix_wait_queue(target,deadline,nonblocking?KERNEL_SOCKET_MSG_DONTWAIT:0,&gc);
            socket_state_release(&state);unix_put(target);target=NULL;
            if(!result)result=unix_lookup(socket,address,&target);
            if(!result)continue;
            break;
        }
        if(!result) {
            child->unix_name=target->unix_name;unix_name_get_locked(child->unix_name);
            child->unix_path=child->unix_name->path;
            child->passcred=target->passcred;
            child->peercred=cred;cred.pid=NULL;
            retired=socket->peercred;socket->peercred=target->peercred;
            if(socket->peercred.pid)kernel_task_identity_retain(socket->peercred.pid);
            child->connected=socket->connected=1;socket->peer_closed=0;
            unix_link_peer_locked(socket,child);unix_link_peer_locked(child,socket);
            child->listener=target;child->pending_ready=1;entry->child=child;
            if(target->pending_tail)target->pending_tail->next=entry;else target->pending_head=entry;
            target->pending_tail=entry;target->pending_live++;
            wake_socket(target);socket_async(target,KERNEL_ASYNC_POLL_IN);child=NULL;entry=NULL;
        }
        socket_state_release(&state);unix_put(target);break;
    }
    if(entry)kernel_heap_release(socket->heap,entry);
    if(child)kernel_socket_destroy(child);
    return result;
}

int kernel_socket_listen(struct kernel_socket *socket, int backlog)
{
    if (socket->domain==KERNEL_SOCKET_DOMAIN_NETLINK || socket->domain==KERNEL_SOCKET_DOMAIN_PACKET)
        return -KERNEL_EOPNOTSUPP;
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        struct unix_credentials cred __attribute__((cleanup(unix_cred_release)))=unix_current_cred();
        struct unix_credentials retired __attribute__((cleanup(unix_cred_release)))={0};
        SOCKET_STATE(state);
        if(!socket_connection_oriented(socket))return -KERNEL_EOPNOTSUPP;
        if(!socket->unix_name || socket->connected)return -KERNEL_EINVAL;
        socket->listening=1;socket->unix_backlog=(unsigned)backlog>4096U?4096U:(unsigned)backlog;
        retired=socket->peercred;socket->peercred=cred;cred.pid=NULL;wake_socket(socket);return 0;
    }
    struct kernel_socket_protocol_scope scope; socket_protocol_enter(&scope,socket);
    err_t error = ERR_OK;
    struct tcp_pcb *replacement;
    if (!socket_connection_oriented(socket)) { kernel_socket_protocol_leave(&scope); return -KERNEL_EOPNOTSUPP; }
    if (socket->tcp == 0) { kernel_socket_protocol_leave(&scope); return -KERNEL_ENOTCONN; }
    if (socket->listening) { kernel_socket_protocol_leave(&scope); return 0; }
    int conflict = port_conflict(socket, local_ip(socket), local_port(socket), 1);
    if (conflict != 0) { kernel_socket_protocol_leave(&scope); return conflict; }
    replacement = tcp_listen_with_backlog_and_err(socket->tcp,
        backlog < 1 ? 1U : backlog > 255 ? 255U : (u8_t)backlog, &error);
    if (replacement != 0) {
        tcp_arg(replacement, socket);tcp_accept(replacement, socket_tcp_accepted);
        SOCKET_ENDPOINT(state,socket);socket->tcp=replacement;socket->listening=1;
    }
    kernel_socket_protocol_leave(&scope);
    return replacement != 0 ? 0 : lwip_error(error);
}

int kernel_socket_connect(struct kernel_socket *socket,
                          const struct kernel_socket_address *address, int nonblocking)
{
    if (socket->domain==KERNEL_SOCKET_DOMAIN_NETLINK || socket->domain==KERNEL_SOCKET_DOMAIN_PACKET)
        return -KERNEL_EOPNOTSUPP;
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX)return unix_connect(socket,address,nonblocking);
    ip_addr_t remote;
    err_t error;
    struct kernel_socket_protocol_scope old_status;
    socket_protocol_enter(&old_status,socket);
    if(socket->raw) {
        if(address->family==0) {
            raw_disconnect(socket->raw);
            if(socket->explicit_bind)ip_addr_copy(socket->raw->local_ip,socket->udp_bind_ip);
            else ip_addr_set_zero_ip4(&socket->raw->local_ip);
            {SOCKET_ENDPOINT(state,socket);socket->connected=0;}
            kernel_socket_protocol_leave(&old_status);return 0;
        }
        int result=address_import(socket,address,&remote,SOCKET_ADDRESS_CONNECT);
        struct netif *route=NULL;const ip_addr_t *source=NULL;
        if(!result)result=socket_ip_route(socket,&remote,&route,&source);
        if(result) {kernel_socket_protocol_leave(&old_status);return result;}
        error=raw_connect(socket->raw,&remote);
        if(!error) {
            if(ip_addr_isany(&socket->raw->local_ip))ip_addr_copy(socket->raw->local_ip,*source);
            SOCKET_ENDPOINT(state,socket);socket->connected=1;
            address_export(socket,&remote,address->port,&socket->saved_peer);
        }
        kernel_socket_protocol_leave(&old_status);return lwip_error(error);
    }
    if (socket->udp != 0) {
#if BOAROS_USER_SMP
        udp4_forget_path(socket);
#endif
        if (address->family == 0) {
            udp_disconnect(socket->udp);
            { KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(socket));socket->connected=0; }
            if (socket->explicit_bind) ip_addr_copy(socket->udp->local_ip, socket->udp_bind_ip);
            else {
                ip_addr_set_zero(&socket->udp->local_ip);
                IP_SET_TYPE(&socket->udp->local_ip, socket->family == KERNEL_SOCKET_AF_INET ? IPADDR_TYPE_V4 :
                             socket->v6only ? IPADDR_TYPE_V6 : IPADDR_TYPE_ANY);
            }
            if (!socket->explicit_port) boaros_lwip_udp_release_port(socket->udp);
            error = ERR_OK;
        } else {
            int result = address_import(socket, address, &remote, SOCKET_ADDRESS_CONNECT);
            if (result != 0) { kernel_socket_protocol_leave(&old_status); return result; }
            struct netif *route=NULL;const ip_addr_t *source=NULL;
            result=socket_ip_route(socket,&remote,&route,&source);
            if(result) {kernel_socket_protocol_leave(&old_status);return result;}
            error = udp_connect(socket->udp, &remote, address->port);
            if (error == ERR_OK) {
                { KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(socket));socket->connected=1;
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
    int result = address_import(socket, address, &remote, SOCKET_ADDRESS_CONNECT);
    if (result != 0) { int failure = (result); kernel_socket_protocol_leave(&old_status); return failure; }
    if (IP_IS_V6(&remote)) {
        struct netif *route=NULL;const ip_addr_t *source=NULL;
        result=socket_ip_route(socket,&remote,&route,&source);
        if (result) {kernel_socket_protocol_leave(&old_status);return result;}
    }
    socket->tcp->ttl=IP_IS_V4(&remote)?socket->ipv4_ttl:64;
    error = tcp_connect(socket->tcp, &remote, address->port, tcp_connected);
    if(error==ERR_OK) {KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(socket));socket->connecting=1;}
    if (error == ERR_OK) service_inline(&old_status);
    int connected = socket->error ? socket->error : socket->connected ? 0 : -KERNEL_EINPROGRESS;
    kernel_socket_protocol_leave(&old_status);
    if (error != ERR_OK) return lwip_error(error);
    return nonblocking ? -KERNEL_EINPROGRESS : connected;
}

int kernel_socket_connection_result(struct kernel_socket *socket)
{
    KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(socket));
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX)return socket->connected?0:-KERNEL_ECONNREFUSED;
    return socket->error ? socket->error : socket->connected ? 0 : -KERNEL_EINPROGRESS;
}

int kernel_socket_accept(struct kernel_socket *socket,
                         struct kernel_socket **owner, struct kernel_open_file_description *file)
{
    struct socket_pending *entry;
    struct kernel_socket_protocol_scope old_status;
    if (owner == 0 || *owner != 0) return -KERNEL_EINVAL;
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        KERNEL_LOCK_SCOPE(gc);rights_acquire(&gc);
        SOCKET_STATE(state);
        if(!socket->listening)return -KERNEL_EINVAL;
        entry=socket->pending_head;
        if(!entry)return -KERNEL_EAGAIN;
        socket->pending_head=entry->next;if(!socket->pending_head)socket->pending_tail=NULL;
        if(!socket->pending_live)__builtin_trap();
        socket->pending_live--;
        entry->child->gc_file=file;
        entry->child->listener=NULL;entry->child->pending_ready=0;*owner=entry->child;
        wake_socket(socket);socket_state_release(&state);kernel_heap_release(socket->heap,entry);return 0;
    }
    kernel_socket_protocol_enter(&old_status);
    service_inline(&old_status);
    if (network_is_stopping()) {
        kernel_socket_protocol_leave(&old_status);return -KERNEL_EIO;
    }
    if (!socket->listening) { kernel_socket_protocol_leave(&old_status); return -KERNEL_EINVAL; }
    for (;;) {
        SOCKET_ENDPOINT(state,socket);
        entry=socket->pending_head;
        if (!entry) {
            socket_state_release(&state);
            kernel_socket_protocol_leave(&old_status);return -KERNEL_EAGAIN;
        }
        socket->pending_head=entry->next;
        if (!socket->pending_head) socket->pending_tail=NULL;
        if (entry->child->pending_ready) {
            if (!socket->pending_live) __builtin_trap();
            socket->pending_live--;entry->child->pending_ready=0;entry->child->listener=NULL;
            break;
        }
        socket_state_release(&state);
        kernel_socket_destroy(entry->child);
        kernel_heap_release(socket->heap,entry);
    }
    tcp_backlog_accepted(entry->child->tcp);
#if BOAROS_USER_SMP
    (void)tcp_promote(entry->child,&old_status);
#endif
    *owner = entry->child;
    kernel_socket_protocol_leave(&old_status);
    kernel_heap_release(socket->heap, entry);
    return 0;
}

int kernel_socket_accept_check(struct kernel_socket *socket)
{
    if (!socket_connection_oriented(socket)) return -KERNEL_EOPNOTSUPP;
    KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(socket));
    return socket->listening ? 0 : -KERNEL_EINVAL;
}

static int socket_receive_empty(const struct kernel_socket *, int);
#if BOAROS_USER_SMP
static void tcp_owner_receive_before_wait(struct socket_tcp_owner *owner);
#endif

static void socket_receive_before_wait(struct kernel_socket *socket)
{
    struct kernel_socket_protocol_scope scope;
    if (!protocol_enter(&scope, 1, 1)) return;
    if (scope.outer && !network_is_stopping() && network_receive_inline) {
        for (unsigned batch = 0; batch < 4; batch++) {
            if (network_receive_inline(network_context) < KERNEL_NETWORK_RECEIVE_BATCH) break;
            SOCKET_ENDPOINT(state,socket);
            if (tcp_admission_capacity_locked(socket) || socket->pending_error || socket->write_closed)
                break;
        }
    }
    kernel_socket_protocol_leave(&scope);
}

static int socket_wait_ready(struct kernel_socket *socket, uint32_t events,
    uint64_t target_ns, uint32_t flags, struct kernel_socket_read_request *request)
{
    uint64_t deadline = 0;
    int timed = target_ns == 0;
    int assisted = 0;
    for (;;) {
        SOCKET_ENDPOINT(state,socket);
        int ready = events == KERNEL_POLLIN && !socket->listening
            ? kernel_socket_receive_ready(socket)
            : (kernel_socket_poll(socket, 0) & (events | KERNEL_POLLERR | KERNEL_POLLHUP)) != 0;
        if (request && request->peek_offset) {
            /* Already peeked bytes are still POLLIN, but cannot satisfy this
             * continuation. A consuming reader invalidates the peek cursor. */
            ready = !socket->read_request &&
                (socket->receive_sequence != request->peek_sequence ||
                 socket->rx_bytes > request->peek_offset ||
                 socket_receive_empty(socket, 0) != -KERNEL_EAGAIN);
        }
        if (ready) return (events & KERNEL_POLLOUT) ? kernel_socket_unix_wait_error(socket) : 0;
        if (flags & KERNEL_SOCKET_MSG_DONTWAIT) return -KERNEL_EAGAIN;
        if (!timed) {
            enum kernel_time_status status = kernel_time_deadline_from_monotonic(target_ns, &deadline);
            if (status != KERNEL_TIME_STATUS_OK)
                return status == KERNEL_TIME_STATUS_DEADLINE_PASSED ? -KERNEL_EAGAIN : -KERNEL_EIO;
            timed = 1;
        }
#if BOAROS_USER_SMP
        unsigned cpu=tcp_socket_cpu(socket);
        if (!assisted && cpu && (events != KERNEL_POLLIN || !socket->read_request)) {
            struct socket_tcp_owner *owner=tcp_owner_at(cpu);
            /* A hint only. The consumer rechecks the ring under ingress. */
            if (__atomic_load_n(&owner->count,__ATOMIC_RELAXED)) {
                assisted=1;
                socket_state_release(&state);
                tcp_owner_receive_before_wait(owner);
                continue;
            }
        }
#endif
        /* On one CPU the writer can process queued acknowledgments before
         * registering to wait. Bound that work even if other flows keep
         * producing input; the worker owns any remainder. */
        if (!assisted && (events & KERNEL_POLLOUT) && kernel_socket_is_tcp(socket) &&
            !boaros_lwip_parallel_enabled()) {
            assisted = 1;
            socket_state_release(&state);
            socket_receive_before_wait(socket);
            continue;
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
            if (target_ns || (request && request->progressed)) return -KERNEL_EINTR;
            kernel_signal_note_syscall_restart(kernel_task_current());
            return -KERNEL_ERESTARTSYS;
        }
    }
}

int kernel_socket_wait_ready(struct kernel_socket *socket, uint32_t events,
    uint64_t target_ns, uint32_t flags)
{
    return socket_wait_ready(socket, events, target_ns, flags, NULL);
}

int kernel_socket_wait_read(struct kernel_socket *socket,
    struct kernel_socket_read_request *request, uint64_t target_ns, uint32_t flags)
{
    return socket_wait_ready(socket, KERNEL_POLLIN, target_ns, flags, request);
}

/* 无队首数据时的状态；查询不消费错误，reservation入口负责交付。 */
static int socket_receive_empty(const struct kernel_socket *socket, int nonblocking)
{
    if (socket->domain == KERNEL_SOCKET_DOMAIN_UNIX && socket_connection_oriented(socket) &&
        !socket->connected) return socket->type==SOCKET_SEQPACKET?-KERNEL_ENOTCONN:-KERNEL_EINVAL;
    if (socket->pending_error) return socket->pending_error;
    if (socket->domain == KERNEL_SOCKET_DOMAIN_INET && network_is_stopping()) return -KERNEL_EIO;
    if (kernel_socket_preserves_messages(socket) && !socket_connection_oriented(socket) &&
        socket->read_closed && nonblocking)
        return -KERNEL_EAGAIN;
    if (socket->read_closed ||
        (socket_connection_oriented(socket) && socket->peer_closed))
        return 0;
    if (socket->domain == KERNEL_SOCKET_DOMAIN_INET && socket->type == SOCKET_STREAM &&
        !socket->connected && !socket->connecting)
        return -KERNEL_ENOTCONN;
    return -KERNEL_EAGAIN;
}

int kernel_socket_receive_ready(struct kernel_socket *socket)
{
    SOCKET_ENDPOINT(saved,socket);
    int ready = socket->read_request == 0 &&
        (socket->listening || socket->packets_head != 0 || socket_receive_empty(socket, 0) != -KERNEL_EAGAIN);
    socket_state_release(&saved);
    return ready;
}

static uint32_t packet_available(const struct socket_packet *packet)
{
    return (packet->payload && !packet->length?packet->payload->tot_len:packet->length)-packet->consumed;
}

int kernel_socket_reserve_read(struct kernel_socket *socket,
                              struct kernel_task *task,
                              struct kernel_socket_read_request *request,
                              struct kernel_open_file_description **pin_owner,
                              uint32_t capacity, int nonblocking, struct kernel_socket_message *message)
{
    struct socket_packet *packet;
    uint32_t length = 0;
    /* acquire只检查lock并随后填满guard；无UNIX图边不清零未使用的锁记录。 */
    struct kernel_lock_guard gc __attribute__((cleanup(kernel_lock_scope_release)));gc.lock=0;
    struct socket_state_guard old_status __attribute__((cleanup(socket_state_release)))={0};
    if (request == 0 || request->socket != 0 ||
        pin_owner == 0 || *pin_owner == 0 ||
        kernel_open_file_socket(*pin_owner) != socket)
        __builtin_trap();
retry:
    socket_state_acquire_lock(&old_status,socket_endpoint_lock(socket));
    if(socket->domain==KERNEL_SOCKET_DOMAIN_INET && network_is_stopping())return -KERNEL_EIO;
    if (socket->listening) {
        socket_state_release(&old_status);
        return socket->domain==KERNEL_SOCKET_DOMAIN_UNIX && socket->type==SOCKET_STREAM?-KERNEL_EINVAL:-KERNEL_ENOTCONN;
    }
    if (socket->read_request != 0) {
        if(message && socket->domain==KERNEL_SOCKET_DOMAIN_UNIX && socket->type==SOCKET_STREAM && socket->connected)
            message->receive_state=KERNEL_SOCKET_RECEIVE_STREAM_EMPTY;
        socket_state_release(&old_status);
        return -KERNEL_EAGAIN;
    }
    int continuing_peek = request->waitall && message &&
        (message->flags & KERNEL_SOCKET_MSG_PEEK);
    if (!continuing_peek || socket->receive_sequence != request->peek_sequence)
        request->peek_offset = 0;
    request->peek_sequence = socket->receive_sequence;
    packet = socket->packets_head;
    if (packet == 0 || (continuing_peek && request->peek_offset == socket->rx_bytes)) {
        if(message && socket->domain==KERNEL_SOCKET_DOMAIN_UNIX && socket->type==SOCKET_STREAM && socket->connected)
            message->receive_state=KERNEL_SOCKET_RECEIVE_STREAM_EMPTY;
        int result = socket_receive_empty(socket, nonblocking);
        if(message && !result && socket->type==SOCKET_SEQPACKET)message->receive_state=KERNEL_SOCKET_RECEIVE_EOF;
        if (socket->pending_error && !request->progressed) socket->pending_error = 0;
        socket_state_release(&old_status);
        return result;
    }
    /* UNIX recv(0)仍检查队首控制消息；INET保留原来的空范围处理。 */
    if (capacity == 0U && socket->type == SOCKET_STREAM && socket->domain!=KERNEL_SOCKET_DOMAIN_UNIX) {
        socket_state_release(&old_status);
        return 0;
    }
    if (capacity > (uint32_t)__INT_MAX__) capacity = __INT_MAX__;
    uint32_t available = kernel_socket_preserves_messages(socket) ? packet_available(packet) : socket->rx_bytes;
    if (continuing_peek) available -= request->peek_offset;
    struct socket_packet *control_packet=packet;
    uint32_t control_offset=0;
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX && socket->type==SOCKET_STREAM) {
        available=0;
        struct kernel_socket_control *first=packet->control;
        for(struct socket_packet *p=packet;p && available<capacity;p=p->next) {
            struct kernel_socket_control *c=p->control;
            if(p!=packet && socket->passcred &&
                ((first && first->has_credentials)!=(c && c->has_credentials) ||
                 (first && c && first->has_credentials &&
                  (first->pid!=c->pid || first->uid!=c->uid || first->gid!=c->gid))))break;
            uint32_t offset=available;available+=packet_available(p);
            if(c && c->count) {control_packet=p;control_offset=offset;break;}
        }
    }
    struct kernel_socket_control *source=control_packet->control;
    unsigned rights=source && source->unix_count;
    if(rights && !gc.lock) {
        socket_state_release(&old_status);rights_acquire(&gc);goto retry;
    }
    length = available < capacity ? available : capacity;
    request->rights=rights;
    request->peek_control=NULL;
    request->message=message;
    if(message && socket->domain==KERNEL_SOCKET_DOMAIN_UNIX)
        message->receive_state=KERNEL_SOCKET_RECEIVE_RECORD;
    request->socket = socket;
    request->task = task;
    request->packet = packet;
    request->bytes = length;
    request->completed_bytes = 0;
    request->datagram = kernel_socket_preserves_messages(socket);
    request->pin = *pin_owner;
    request->pin_owner = pin_owner;
    *pin_owner = 0;
    if (task != 0 &&
        kernel_task_socket_read_register(task, request) != KERNEL_TASK_STATUS_OK)
        __builtin_trap();
    socket->read_request = request;
    socket_state_release(&old_status);
    if(source) {
        struct kernel_socket_control *copy=NULL;
        int peek=message && (message->flags&KERNEL_SOCKET_MSG_PEEK);
        int preserve=!request->datagram && source->count && source->has_credentials &&
            length>=control_offset && length-control_offset<packet_available(control_packet);
        int error=0;
        if(peek || (message && source->has_credentials) || preserve)
            error=control_clone(source,&copy,peek);
        if(error) {
            socket_state_acquire_lock(&old_status,socket_endpoint_lock(socket));socket->read_request=NULL;wake_socket(socket);
            if(task && kernel_task_socket_read_clear(task,request)!=KERNEL_TASK_STATUS_OK)__builtin_trap();
            *pin_owner=request->pin;request->pin=NULL;request->socket=NULL;socket_state_release(&old_status);return error;
        }
        request->peek_control=copy;
    }
    return (int)length;
}

void kernel_socket_read_info(const struct kernel_socket_read_request *request,
                             struct kernel_socket_address *address, uint32_t *length)
{
    const struct socket_packet *packet = request->packet;
    if (request->socket == 0 || packet == 0) __builtin_trap();
    if (address != 0) {
        if(request->socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
            /* The packet's sender reference retains its current stable name,
             * including a bind after send and logical close before receive. */
            SOCKET_STATE(state);
            if(!packet->unix_sender)__builtin_trap();
            if(packet->unix_sender->unix_name)unix_export_locked(packet->unix_sender->unix_name,address);
            else *address=(struct kernel_socket_address){0};
        } else *address=packet->address;
    }
    if (length != 0) *length = packet_available(packet);
}

void kernel_socket_copy_read(const struct kernel_socket_read_request *request,
                             uint32_t offset, void *buffer, uint32_t length)
{
    const struct socket_packet *packet = request->packet;
    if (request->socket == 0 || request->socket->read_request != request ||
        offset > request->bytes || length > request->bytes - offset)
        __builtin_trap();
    offset += request->peek_offset;
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
    uint32_t remaining = request->bytes, skip = request->peek_offset;
    while (skip && packet && skip >= packet_available(packet)) {
        skip -= packet_available(packet); packet = packet->next;
    }
    struct kernel_uaccess_write_cursor pages = {0};
    int result = 0;
    request->completed_bytes = 0;
    /* bytes固定本次借用的队列前缀；后来追加的packet不扩充本次用户复制。 */
    while (remaining) {
        if (!packet) __builtin_trap();
        uint32_t take = packet_available(packet) - skip;
        if (take > remaining) take = remaining;
        const struct pbuf *part = packet->payload;
        uint32_t offset = packet->consumed + skip, pending = take;
        skip = 0;
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
        request->completed_bytes > request->bytes ||
        (!request->datagram && !request->bytes && socket->domain!=KERNEL_SOCKET_DOMAIN_UNIX)) __builtin_trap();
    struct kernel_lock_guard gc __attribute__((cleanup(kernel_lock_scope_release)));gc.lock=0;
    if(request->rights)rights_acquire(&gc);
    struct kernel_socket_message *message=request->message;
    int unix_stream=socket->domain==KERNEL_SOCKET_DOMAIN_UNIX && !request->datagram;
    int zero_stream=unix_stream && !request->bytes && !user_fault;
    if(message && unix_stream)message->receive_state=user_fault?
        KERNEL_SOCKET_RECEIVE_STREAM_FAULT:KERNEL_SOCKET_RECEIVE_RECORD;
    int peek=message && (message->flags&KERNEL_SOCKET_MSG_PEEK);
    struct kernel_socket_control *received=request->peek_control,*discard_control=NULL;
    request->peek_control=NULL;
    uint32_t bytes = user_fault && !request->datagram ? request->completed_bytes : request->bytes;
    uint32_t remaining = bytes, charge = bytes;
    struct socket_packet *retired = 0, *tail = 0;
    struct kernel_socket *released_sender = NULL;
    if (request->datagram) {
        charge = packet_available(socket->packets_head);
        if (!charge) charge = 1;
    }
    /* The reservation owns this fixed prefix. Find its boundary with IRQs on;
     * an appender may change only the final next link, read below at detach. */
    struct socket_packet *packet = socket->packets_head;
    uint32_t partial = 0;
    while (!peek && (remaining || request->datagram || zero_stream)) {
        if (!packet) __builtin_trap();
        uint32_t available = packet_available(packet);
        uint32_t take = remaining < available ? remaining : available;
        if((take || request->datagram || zero_stream) && packet->control && packet->control->count) {
            struct kernel_socket_control *c=packet->control;control_unqueue(c);
            packet->control=received;received=c;
        }
        if (request->datagram || take == available) tail = packet;
        else { partial = take; break; }
        remaining -= take;
        if (request->datagram || !remaining) break;
        packet = packet->next;
    }
    if(peek)charge=0;
    SOCKET_ENDPOINT(scope,socket);
    if (tail) {
        retired = socket->packets_head;
        socket->packets_head = tail->next;
        tail->next = 0;
        if (!socket->packets_head) socket->packets_tail = 0;
    }
    if (partial) packet->consumed += partial;
    if (socket->rx_bytes < charge) __builtin_trap();
    socket->rx_bytes -= charge;
#if BOAROS_USER_SMP
    if (retired && request->datagram && udp4_output_socket(socket) && !retired->loopback)
        socket->udp_rx_drained = !socket->packets_head;
#endif
    if (kernel_socket_is_tcp(socket)) socket->receive_sequence += charge;
    if (!peek && request->datagram && (socket->domain==KERNEL_SOCKET_DOMAIN_NETLINK ||
        socket->domain==KERNEL_SOCKET_DOMAIN_PACKET)) {
        uint32_t memory=packet_charge(retired->length);
        if(socket->record_memory<memory)__builtin_trap();
        socket->record_memory-=memory;
    }
    if(!peek && request->datagram && socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        if(!socket->unix_queued)__builtin_trap();
        socket->unix_queued--;
        /* 单个报文出队与其发送额度/源名引用一起提交；对象实际销毁仍在锁外。 */
        released_sender=unix_uncharge_locked(retired);
    }
    if(charge && socket->type==SOCKET_STREAM && socket->peer)wake_socket(socket->peer);
    if (socket->domain == KERNEL_SOCKET_DOMAIN_INET) {
        int needs_reap=!socket->receive_credit && !socket->retired_head;
        if (socket->receive_credit > UINT32_MAX - bytes) __builtin_trap();
        if (!peek && !request->datagram) socket->receive_credit += bytes;
        if (retired && (!boaros_lwip_parallel_enabled() ||
            (request->datagram && !retired->loopback && !socket->packets_head))) {
            if (socket->retired_tail) socket->retired_tail->next = retired;
            else socket->retired_head = retired;
            socket->retired_tail = tail;
            retired = 0;
        }
        /* Only consumption owns work here. tcp_recved sends its own window
         * update; a peek, failed copy or zero-byte read creates no credit. */
        if (needs_reap && (socket->receive_credit || socket->retired_head)) {
            /* Publish the first retirement before clearing the request. Stop
             * drains both under the endpoint lock; callbacks must see neither lock. */
            KERNEL_IRQ_LOCK_SCOPE(work,&socket_state_lock);
            unsigned flags=SOCKET_WORK_REAP | (socket->receive_retry?SOCKET_WORK_RECEIVE:0);
            socket_schedule_locked(socket,flags,scope.owner);
        }
        if (retired) {
            /* During a UDP burst the reader returns buffers while more
             * datagrams are queued. Keep the request visible to network stop
             * until the underlying buffers have actually returned. */
            struct pbuf *reuse_payload=NULL;
            if (socket->udp && socket->udp_spare_count<KERNEL_NETWORK_RECEIVE_BATCH) {
                reuse_payload=retired->payload;
                udp_recycle_locked(socket,retired);
                retired=NULL;
            }
            socket_state_release(&scope);
            if (reuse_payload) {
                pbuf_free(reuse_payload);
                receive_capacity();
            } else release_inet_packets(socket,retired);
            retired=NULL;
            socket_state_acquire_lock(&scope,socket_endpoint_lock(socket));
        }
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
    if (request->waitall && delivered) {
        request->progressed = 1;
        if (peek) request->peek_offset += delivered;
    }
    socket_state_release(&scope);
    /* STREAM在载荷actor失败前已经捕获凭据；零字节recv也可接收rights。 */
    if(received && (!message || (!unix_stream &&
        ((user_fault && (peek || request->datagram || !bytes)) || (!bytes && !request->datagram))))) {
        discard_control=received;received=NULL;
    }
    if(message) {if(message->control)__builtin_trap();message->control=received;}
    /* The consumed-prefix walk detached every rights owner before retirement. */
    kernel_lock_scope_release(&gc);
    if(discard_control)kernel_socket_control_release(&discard_control);
    if(released_sender)unix_storage_free(released_sender);
    int freed = retired != 0;
    while (retired) {
        struct socket_packet *next = retired->next;
        unix_uncharge(retired);
        if(retired->control)kernel_socket_control_release(&retired->control);
        kernel_heap_release(socket->heap, retired);
        retired = next;
    }
    if (freed) receive_capacity();
    return delivered;
}

void kernel_socket_abort_read(struct kernel_socket_read_request *request)
{
    struct kernel_socket *socket;
    struct kernel_open_file_description *pin;
    if (request == 0 || request->socket == 0) __builtin_trap();
    socket = request->socket;
    SOCKET_ENDPOINT(old_status,socket);
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
    struct kernel_socket_control *control=request->peek_control;request->peek_control=NULL;
    kernel_socket_control_release(&control);
    if (kernel_open_file_release(&pin) != KERNEL_OPEN_FILE_STATUS_OK)
        __builtin_trap();
}

enum { SOCKET_WRITE_ACCEPTED=1U, SOCKET_WRITE_DEFERRED=2U, SOCKET_WRITE_LOOPBACK_INLINE=4U };
enum socket_protocol_operation { SOCKET_PROTOCOL_STREAM, SOCKET_PROTOCOL_DATAGRAM_PREPARE,
    SOCKET_PROTOCOL_DATAGRAM_SEND, SOCKET_PROTOCOL_DATAGRAM_CANCEL,
    SOCKET_PROTOCOL_DISPOSE, SOCKET_PROTOCOL_FLUSH };
static int protocol_request(struct kernel_socket_write_request *,enum socket_protocol_operation);

static void socket_link_active_write(struct kernel_socket_write_request *request)
{
    struct kernel_socket *socket=request->socket;
    request->active_next=socket->active_writes;request->active_previous=&socket->active_writes;
    if(request->active_next)request->active_next->active_previous=&request->active_next;
    socket->active_writes=request;
}

static void socket_write_begin(struct kernel_socket_write_request *request,
    struct kernel_open_file_description **pin_owner,int hold_unix_peer)
{
    *request=(struct kernel_socket_write_request){.socket=kernel_open_file_socket(*pin_owner),
        .task=kernel_task_current(),.pin=*pin_owner,.pin_owner=pin_owner};
    *pin_owner=0;
    if(request->task && kernel_task_socket_write_register(request->task,request)!=KERNEL_TASK_STATUS_OK)__builtin_trap();
    struct kernel_socket *socket=request->socket;
    SOCKET_ENDPOINT(state,socket);
    socket_link_active_write(request);
    /* Close can detach the peer edge; borrow its storage in this same publication lock. */
    if(hold_unix_peer && socket->peer) {
        request->unix_destination=socket->peer;
        unix_get_locked(request->unix_destination);
    }
}
static void socket_unlink_active_write(struct kernel_socket_write_request *request)
{
    if(request->queued || !request->active_previous || *request->active_previous!=request)__builtin_trap();
    *request->active_previous=request->active_next;
    if(request->active_next)request->active_next->active_previous=request->active_previous;
    request->active_next=0;request->active_previous=0;
    if(request->task && kernel_task_socket_write_clear(request->task,request)!=KERNEL_TASK_STATUS_OK)__builtin_trap();
}
static void socket_write_end(struct kernel_socket_write_request *request)
{
    SOCKET_ENDPOINT(state,request->socket);
    socket_unlink_active_write(request);
    struct kernel_socket *destination=request->unix_destination;
    request->unix_destination=NULL;
    int release=destination && unix_put_locked(destination);
    /* 请求摘除不改变正常就绪状态；停止网络才等待活动链归还。
     * OFD pin仍保证最后关闭晚于本请求解除登记。 */
    if(network_is_stopping() && request->socket->domain==KERNEL_SOCKET_DOMAIN_INET)wake_socket(request->socket);
    socket_state_release(&state);
    if(release)unix_storage_free(destination);
}
static void socket_tcp_output(struct tcp_pcb *pcb);
static void tcp_flush_output(struct kernel_socket *socket);

#if BOAROS_USER_SMP
static void discard_tcp_prepared(struct kernel_socket_write_request *request)
{
    struct tcp_seg *segments=request->tcp_prepared;
    request->tcp_prepared=0;
    request->tcp_prepare_size=0;
    request->tcp_prepare_failed=0;
    if (segments) tcp_segs_free(segments);
}
#endif

static void free_write_packet(struct kernel_socket_write_request *request)
{
    if (request->protocol_payload) {
        if (boaros_lwip_parallel_enabled()) {
            pbuf_free(request->protocol_payload);request->protocol_payload=NULL;
        } else if(protocol_request(request,SOCKET_PROTOCOL_DISPOSE))__builtin_trap();
    }
    struct socket_packet *packet = request->packet;
    if (!packet) return;
    kernel_heap_release(request->socket->heap, packet);
    request->packet = 0;
    receive_capacity();
}
static void clear_write_request(struct kernel_socket_write_request *request)
{
#if BOAROS_USER_SMP
    discard_tcp_prepared(request);
#endif
    free_write_packet(request);
#if BOAROS_USER_SMP
    if (request->udp_output_owned) udp4_end_output(request);
#endif
    if(request->unix_receive_reserved) {
        SOCKET_STATE(state);
        if(!request->unix_destination->unix_reserved)__builtin_trap();
        request->unix_destination->unix_reserved--;request->unix_receive_reserved=0;
        if(!request->unix_destination->destroying)wake_socket(request->unix_destination);
    }
    if(request->socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        if(request->reserved) {
            SOCKET_STATE(state);
            if(request->socket->tx_reserved<request->reserved)__builtin_trap();
            request->socket->tx_reserved-=request->reserved;request->reserved=0;
            /* UNIX reservations charge sender memory, not TCP tx_budget.
             * Return that credit before notifying writers and pollers. */
            wake_socket(request->socket);
            if (unix_writable(request->socket)) socket_async(request->socket,KERNEL_ASYNC_POLL_OUT);
        }
    }else kernel_socket_stream_cancel(request);
    if(request->progressed & SOCKET_WRITE_DEFERRED)
        (void)protocol_request(request,SOCKET_PROTOCOL_FLUSH);
    socket_write_end(request);
    if(request->automatic_control)kernel_socket_control_release(&request->automatic_control);
}
void kernel_socket_abort_write(struct kernel_socket_write_request *request)
{
    clear_write_request(request);
    struct kernel_open_file_description *pin = request->pin;
    request->pin = 0;
    if (kernel_open_file_release(&pin) != KERNEL_OPEN_FILE_STATUS_OK) __builtin_trap();
}

static void netlink_cookie_finish(struct kernel_netlink_cookie **owner,unsigned reason)
{
    if(!owner || !*owner)return;
    struct kernel_netlink_cookie *cookie=*owner;*owner=NULL;
    if(cookie->token.task) {
        if(cookie->token.task==kernel_wait_current_task()) {
            if(kernel_wait_finish(&cookie->token)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
        } else {
            /* Forced exit drains the task's token before aborting I/O. The
             * reaper must not finish a token belonging to another task. */
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            struct kernel_wait_record *record=kernel_wait_record_of(cookie->token.task);
            if(kernel_wait_record_phase(record)!=KERNEL_WAIT_FINISHED || record->nodes || kernel_wait_record_borrows(record))__builtin_trap();
            cookie->token=(struct kernel_wait_token){0};
        }
    }
    if(cookie->pin && kernel_open_file_release(&cookie->pin)!=KERNEL_OPEN_FILE_STATUS_OK)__builtin_trap();
    struct kernel_socket *socket=cookie->socket;
    if(socket) {
        SOCKET_ENDPOINT(state,socket);
        int publish=cookie->reserved && reason && !socket->destroying;
        if(publish) {
            cookie->packet->data[NETLINK_COOKIE_SIZE-1]=(unsigned char)reason;
            packet_publish(socket,cookie->packet,NETLINK_COOKIE_SIZE);
            cookie->packet=NULL;
        } else if(cookie->reserved) {
            uint32_t charge=packet_charge(NETLINK_COOKIE_SIZE);
            if(socket->record_memory<charge)__builtin_trap();
            socket->record_memory-=charge;
            if(!socket->destroying)wake_socket(socket);
        }
        int release=netlink_put_locked(socket);
        socket_state_release(&state);
        if(cookie->packet)kernel_heap_release(socket->heap,cookie->packet);
        if(release)netlink_storage_free(socket);
    }
    kernel_heap_release(cookie->heap,cookie);
}
int kernel_socket_mqueue_cookie_prepare(struct kernel_task *task,int64_t fd,
    const unsigned char bytes[NETLINK_COOKIE_SIZE],struct kernel_netlink_cookie **owner)
{
    if(!owner || *owner || !bytes)__builtin_trap();
    struct kernel_files *files=NULL;
    enum kernel_task_status task_status=kernel_task_files_borrow(task,&files);
    if(task_status==KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE)return -KERNEL_EBADF;
    if(task_status!=KERNEL_TASK_STATUS_OK)__builtin_trap();
    for(;;) {
        struct kernel_netlink_cookie *cookie=NULL;
        enum kernel_heap_status allocated=kernel_heap_allocate_zeroed(files->heap,1,sizeof(*cookie),(void **)&cookie);
        if(allocated!=KERNEL_HEAP_STATUS_OK) {
            if(allocated!=KERNEL_HEAP_STATUS_EMPTY)__builtin_trap();
            return -KERNEL_ENOMEM;
        }
        cookie->heap=files->heap;*owner=cookie;
        int64_t result;
        if(kernel_files_pin(files,fd,&cookie->pin,&result)!=KERNEL_FILES_STATUS_OK)__builtin_trap();
        if(result) {netlink_cookie_finish(owner,0);return (int)result;}
        struct kernel_socket *socket=kernel_open_file_socket(cookie->pin);
        if(!socket || socket->domain!=KERNEL_SOCKET_DOMAIN_NETLINK) {
            result=socket?-KERNEL_EINVAL:-KERNEL_ENOTSOCK;
            netlink_cookie_finish(owner,0);return (int)result;
        }
        {
            SOCKET_ENDPOINT(state,socket);
            if(socket->netlink_references==UINT32_MAX)__builtin_trap();
            socket->netlink_references++;cookie->socket=socket;
        }
        allocated=kernel_heap_allocate_zeroed(socket->heap,1,
            sizeof(struct socket_packet)+NETLINK_COOKIE_SIZE,(void **)&cookie->packet);
        if(allocated!=KERNEL_HEAP_STATUS_OK) {
            if(allocated!=KERNEL_HEAP_STATUS_EMPTY)__builtin_trap();
            netlink_cookie_finish(owner,0);return -KERNEL_ENOMEM;
        }
        cookie->packet->length=NETLINK_COOKIE_SIZE;
        cookie->packet->address.family=KERNEL_SOCKET_AF_NETLINK;
        __builtin_memcpy(cookie->packet->data,bytes,NETLINK_COOKIE_SIZE);
        /* A quota wait must not keep the last OFD alive: close revokes the
         * endpoint, while this cookie keeps only its storage alive. */
        if(kernel_open_file_release(&cookie->pin)!=KERNEL_OPEN_FILE_STATUS_OK)__builtin_trap();
        SOCKET_ENDPOINT(state,socket);
        if(socket->destroying) {
            socket_state_release(&state);netlink_cookie_finish(owner,0);continue;
        }
        uint32_t charge=packet_charge(NETLINK_COOKIE_SIZE);
        if(!socket->record_memory || (charge<=socket->rx_limit &&
            socket->record_memory<=socket->rx_limit-charge)) {
            socket->record_memory+=charge;cookie->reserved=1;return 0;
        }
        if(kernel_wait_prepare(&socket->wait,0,1,&cookie->token)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
        uintptr_t interrupts=socket_state_unlock_wait(&state);
        enum kernel_wait_wake_reason reason=KERNEL_WAIT_WOKEN;
        result=kernel_wait_park(&cookie->token,&reason)==KERNEL_SCHEDULER_STATUS_OK?0:-KERNEL_EIO;
        if(kernel_wait_finish(&cookie->token)!=KERNEL_SCHEDULER_STATUS_OK)__builtin_trap();
        arch_interrupt_restore(interrupts);
        netlink_cookie_finish(owner,0);
        if(result)return (int)result;
        if(reason==KERNEL_WAIT_SIGNALLED)return -KERNEL_ERESTARTSYS;
        /* Lookup again after wake: a closed or reused descriptor is observable. */
    }
}
void kernel_socket_mqueue_cookie_send(struct kernel_netlink_cookie **owner,unsigned reason)
{
    if(reason!=1 && reason!=2)__builtin_trap();
    netlink_cookie_finish(owner,reason);
}
void kernel_socket_mqueue_cookie_cancel(struct kernel_netlink_cookie **owner)
{ netlink_cookie_finish(owner,0); }
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
    if (socket->domain!=KERNEL_SOCKET_DOMAIN_UNIX) return 0;
    SOCKET_STATE(irq);
    int error=socket->pending_error;
    if(error)socket->pending_error=0;
    socket_state_release(&irq);return error;
}
static int unix_reserve_packet(struct kernel_socket_write_request *request,uint32_t count,uint32_t flags,int *credentials)
{
    uint64_t deadline=(flags&KERNEL_SOCKET_MSG_DONTWAIT)?0:unix_deadline(request->socket);
    for(;;) {
        SOCKET_STATE(state);
        struct kernel_socket *sender=request->socket,*peer=request->unix_destination;
        int result=0;
        if(sender->write_closed)return -KERNEL_EPIPE;
        if(!peer || peer->destroying)
            return sender->type==SOCKET_DGRAM?-KERNEL_ECONNREFUSED:-KERNEL_EPIPE;
        if(!kernel_socket_preserves_messages(sender) && peer->read_closed)return -KERNEL_EPIPE;
        struct kernel_socket *waiter=NULL;
        if(sender->unix_memory+sender->tx_reserved>=sender->tx_limit)waiter=sender;
        if(!waiter) {
            request->reserved=packet_charge(count);
            if(sender->tx_reserved>UINT32_MAX-request->reserved)__builtin_trap();
            sender->tx_reserved+=request->reserved;
            /* 同一接纳快照读取两端选项；真实凭据仍在锁外、载荷复制前取得。 */
            *credentials=sender->passcred || peer->passcred || peer->listener!=NULL;
            return 0;
        }
        result=unix_wait_queue(waiter,deadline,flags,NULL);
        if(result)return result;
    }
}
/* Sender memory is reserved before copying. Receiver queue admission follows
 * filtering: a dropped datagram succeeds even when the peer queue is full. */
static int unix_reserve_receive(struct kernel_socket_write_request *request,uint32_t flags)
{
    uint64_t deadline=(flags&KERNEL_SOCKET_MSG_DONTWAIT)?0:unix_deadline(request->socket);
    for(;;) {
        SOCKET_STATE(state);
        struct kernel_socket *sender=request->socket,*peer=request->unix_destination;
        if(sender->write_closed)return -KERNEL_EPIPE;
        if(peer->destroying)return sender->type==SOCKET_DGRAM?-KERNEL_ECONNREFUSED:-KERNEL_EPIPE;
        if(peer->read_closed)return -KERNEL_EPIPE;
        if(sender->type==SOCKET_DGRAM && peer->peer && peer->peer!=sender)return -KERNEL_EPERM;
        if(!unix_queue_full(sender,peer)) {
            peer->unix_reserved++;request->unix_receive_reserved=1;return 0;
        }
        int result=unix_wait_queue(peer,deadline,flags,NULL);
        if(result)return result;
    }
}
/* Publication has no user access. Keep its state guard off the stack while
 * a payload copy may recurse through an ELF/page-cache/block read fault. */
static int unix_filter_read(void *object,int64_t offset,void *buffer,uint32_t length)
{
    struct socket_packet *packet=object;
    if(offset<0 || (uint64_t)offset+length>packet->length)return -1;
    __builtin_memcpy(buffer,packet->data+offset,length);return 0;
}
static __attribute__((noinline)) int unix_publish_packet(
    struct kernel_socket_write_request *request,uint32_t count,struct kernel_socket_control **control)
{
    struct socket_packet *packet=request->packet;
    struct kernel_lock_guard gc __attribute__((cleanup(kernel_lock_scope_release)));gc.lock=0;
    int result=0;
    if(*control && (*control)->unix_count)rights_acquire(&gc);
    SOCKET_STATE(endpoint);
    result=control_limit(*control);if(result)return result;
    struct kernel_socket *peer=request->unix_destination;
    if(!peer || peer->destroying || request->socket->write_closed || peer->read_closed) {
        result=request->socket->type==SOCKET_DGRAM && peer && peer->destroying?-KERNEL_ECONNREFUSED:-KERNEL_EPIPE;
    } else if(request->socket->type==SOCKET_DGRAM && peer->peer && peer->peer!=request->socket) {
        result=-KERNEL_EPERM;
    } else {
        uint32_t charge=request->reserved;
        if(!charge || request->socket->tx_reserved<charge)__builtin_trap();
        request->socket->tx_reserved-=charge;request->reserved=0;
        if(kernel_socket_preserves_messages(request->socket)) {
            if(!request->unix_receive_reserved || !peer->unix_reserved)__builtin_trap();
            peer->unix_reserved--;request->unix_receive_reserved=0;peer->unix_queued++;
        }
        if(*control) {
            packet->control=*control;*control=NULL;
            if(packet->control->count)control_queue(packet->control,peer,packet);
        }
        unix_publish_owner(request->socket,packet,charge);
        if(peer->packets_tail)peer->packets_tail->next=packet;else peer->packets_head=packet;
        peer->packets_tail=packet;
        uint32_t payload=packet->length ? packet->length : 1U;
        if(peer->rx_bytes>UINT32_MAX-payload)__builtin_trap();
        peer->rx_bytes+=payload;request->packet=0;wake_socket(peer);socket_async(peer,KERNEL_ASYNC_POLL_IN);result=(int)count;
        /* 发布已经归还全部reservation；同一次提交撤销request和peer借用。 */
        socket_write_end(request);
    }
    return result;}

static int datagram_prepare_payload(struct kernel_socket_write_request *request)
{
    struct kernel_socket *socket=request->socket;
    ip_addr_t remote;
    if(request->destination) {
        int error;
        { KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(socket));error=address_import(socket,request->destination,&remote,SOCKET_ADDRESS_SEND); }
        if(error)return error;
        /* Hold the admitted address independently of later socket option
         * changes and of the syscall's temporary name snapshot. */
        request->inet_destination.family=IP_IS_V6(&remote)?KERNEL_SOCKET_AF_INET6:KERNEL_SOCKET_AF_INET;
        if(IP_IS_V6(&remote)) {
            __builtin_memcpy(request->inet_destination.bytes,ip_2_ip6(&remote)->addr,16);
            request->inet_destination.scope=ip6_addr_zone(ip_2_ip6(&remote));
        } else __builtin_memcpy(request->inet_destination.bytes,&ip_2_ip4(&remote)->addr,4);
    }
    request->protocol_payload=pbuf_alloc(socket->raw?PBUF_IP:PBUF_TRANSPORT,
        (u16_t)request->payload_size,PBUF_RAM);
    return request->protocol_payload?0:-KERNEL_ENOMEM;
}

/* Admission consumes borrowed UNIX names before a payload copy can fault.
 * Only the request, final payload and INET address remain live afterwards. */
int kernel_socket_packet_prepare(struct kernel_socket_write_request *request,
    struct kernel_open_file_description **pin_owner,uint64_t count,uint32_t flags,
    const struct kernel_socket_address *destination,int kernel_source,
    struct kernel_socket_message *message)
{
    if(!request || !pin_owner || !*pin_owner)__builtin_trap();
    struct kernel_socket *socket=kernel_open_file_socket(*pin_owner);
    if(socket->domain==KERNEL_SOCKET_DOMAIN_NETLINK) {
        if(destination) {
            uint32_t groups;__builtin_memcpy(&groups,destination->bytes,4);
            if(destination->family!=KERNEL_SOCKET_AF_NETLINK)return -KERNEL_EINVAL;
            if(destination->scope || groups)return -KERNEL_EPERM;
        }
        if(count>4096)return -KERNEL_EMSGSIZE;
        if(message && (message->control || message->credentials))return -KERNEL_EINVAL;
        int error=netlink_bind(socket,0,1);if(error)return error;
    }
    struct kernel_socket_address packet_destination={0};
    unsigned link_header=0;
    if (socket->domain==KERNEL_SOCKET_DOMAIN_PACKET) {
        if (count > UINT16_MAX-PACKET_ETH_HEADER) return -KERNEL_EMSGSIZE;
        if (destination) {
            packet_destination=*destination;
            if ((unsigned)packet_destination.link_length+12U>packet_destination.link_size)
                return -KERNEL_EINVAL;
        } else {
            struct kernel_socket_protocol_scope protocol;
            kernel_socket_protocol_enter(&protocol);
            packet_destination.scope=socket->packet_index;
            packet_destination.port=socket->packet_protocol;
            kernel_socket_protocol_leave(&protocol);
        }
        if (socket->type==SOCKET_DGRAM) link_header=PACKET_ETH_HEADER;
        else if (count<PACKET_ETH_HEADER) return -KERNEL_EINVAL;
    }
    struct kernel_socket_control **control=message?&message->control:&request->automatic_control;
    int result=0,credentials=0;
    int hold_unix_peer=socket->domain==KERNEL_SOCKET_DOMAIN_UNIX &&
        !(destination && socket->type==SOCKET_DGRAM);
    {
        SOCKET_ENDPOINT(preflight,socket);
        int prepared=socket_prepare_send_locked(socket);if(prepared)return prepared;
        if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX && socket->passcred && !socket->unix_name) {
            socket_state_release(&preflight);
            int error=unix_autobind_if_needed(socket);if(error)return error;
            socket_state_acquire_lock(&preflight,socket_endpoint_lock(socket));
        }
        if(socket->type==SOCKET_STREAM) {
            if(socket->domain!=KERNEL_SOCKET_DOMAIN_UNIX)__builtin_trap();
            int error=(!socket->peer || socket->write_closed || socket->peer->read_closed)?unix_peer_failure(socket):0;
            if(error)return error;
            if(!count)return 0;
            /* 连同实际packet头限制分配，避免载荷只差数个字节却占双倍buddy块。 */
            uint32_t limit=socket->tx_limit/2U;if(limit>32768U)limit=32768U;
            limit-=(uint32_t)sizeof(struct socket_packet);
            if(count>limit)count=limit;
        } else {
            int missing_destination=socket->domain==KERNEL_SOCKET_DOMAIN_INET && !destination &&
                !socket->connected && !udp_has_pending(socket);
            if(kernel_source && missing_destination && count<=UINT16_MAX)return -KERNEL_EDESTADDRREQ;
            if(count>(socket->domain==KERNEL_SOCKET_DOMAIN_INET?
                (socket->type==SOCKET_RAW?RAW_MAX_PAYLOAD:UDP_MAX_PAYLOAD):socket->tx_limit-32U) ||
                count>socket->tx_limit)return -KERNEL_EMSGSIZE;
            if(socket->write_closed)return -KERNEL_EPIPE;
            if(missing_destination)return -KERNEL_EDESTADDRREQ;
        }
        if(socket->domain==KERNEL_SOCKET_DOMAIN_INET) {
            /* The caller may reuse its import storage for this request. */
            struct kernel_socket_address inet_destination;
            if(destination)inet_destination=*destination;
            socket_write_begin(request,pin_owner,0);
            if(destination) {
                request->inet_destination=inet_destination;
                request->destination=&request->inet_destination;
            }
            request->payload_size=count;request->flags=flags;
#if BOAROS_USER_SMP
            if (udp4_output_socket(socket)) {
                request->udp_rx_drained=socket->udp_rx_drained;
                socket->udp_rx_drained=0;
            }
#endif
        } else if (socket->domain==KERNEL_SOCKET_DOMAIN_NETLINK || socket->domain==KERNEL_SOCKET_DOMAIN_PACKET) {
            socket_write_begin(request,pin_owner,0);
            request->payload_size=count;request->flags=flags;
            if (socket->domain==KERNEL_SOCKET_DOMAIN_PACKET)
                request->inet_destination=packet_destination;
        } else if(hold_unix_peer) {
            /* Connected admission, peer borrowing and quota share this state. */
            socket_write_begin(request,pin_owner,1);
            request->payload_size=count;request->flags=flags;
            result=request->unix_destination?unix_reserve_packet(request,(uint32_t)count,flags,&credentials):unix_peer_failure(socket);
        }
    }
    if(result)goto out;
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX && !hold_unix_peer) {
        /* Resolve the borrowed name before request construction overwrites
         * import metadata. The outer OFD pin remains owned by the caller. */
        struct kernel_socket *peer=NULL;
        result=unix_lookup(socket,destination,&peer);if(result)return result;
        SOCKET_STATE(admission);
        socket_write_begin(request,pin_owner,0);
        request->unix_destination=peer;
        request->payload_size=count;request->flags=flags;
        result=unix_reserve_packet(request,(uint32_t)count,flags,&credentials);
        if(result)goto out;
    }
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        if(message && !*control && message->credentials) {
            result=control_clone(message->credentials,control,0);if(result)goto out;
        }
        if(*control || credentials) {result=control_prepare(socket,credentials,control);if(result)goto out;}
        if(message && *control && (*control)->has_credentials && !message->credentials) {
            result=control_clone(*control,&message->credentials,0);if(result)goto out;
        }
    }
    result=-KERNEL_ENOMEM;
    struct socket_packet *packet = 0;
    if (request->socket->domain == KERNEL_SOCKET_DOMAIN_INET) {
        if (boaros_lwip_parallel_enabled()) {
            result=network_is_stopping()?
                -KERNEL_EIO:datagram_prepare_payload(request);
        } else result=protocol_request(request,SOCKET_PROTOCOL_DATAGRAM_PREPARE);
        if(result) {
            if (result==-KERNEL_ENOMEM && kernel_socket_is_udp(socket) && udp_has_pending(socket))
                (void)protocol_request(request,SOCKET_PROTOCOL_DATAGRAM_CANCEL);
            goto out;
        }
    } else {
        KERNEL_NO_RECLAIM_IO;
        enum kernel_heap_status status = kernel_heap_allocate_zeroed(request->socket->heap,
            1, sizeof(*packet)+(size_t)count+link_header, (void **)&packet);
        if (status != KERNEL_HEAP_STATUS_OK) {
            if (status != KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
            goto out;
        }
        request->packet = packet;
        packet->length = (uint32_t)count;
    }
    return 0;
out:
    clear_write_request(request);
    *pin_owner=request->pin;request->pin=NULL;request->socket=NULL;
    return result;
}

/* Filtering and queue admission follow copying. Their lock guards must not
 * remain on the stack while a user page faults into filesystem I/O. */
static __attribute__((noinline)) int socket_finish_packet(
    struct kernel_socket_write_request *request,uint32_t count,
    struct kernel_socket_control **control)
{
    struct kernel_socket *socket=request->socket;
    struct socket_packet *packet=request->packet;
    uint32_t flags=request->flags;
    int result=0;
    if(kernel_socket_preserves_messages(socket)) {
        struct kernel_bpf_packet view={.object=packet,.length=packet->length,
            .network_offset=0,.link_offset=0,.read=unix_filter_read};
        uint32_t kept=socket_filter_run(request->unix_destination,&view);
        if(!kept) {clear_write_request(request);return (int)count;}
        if(kept<packet->length)packet->length=kept;
        result=unix_reserve_receive(request,flags);if(result)return result;
    }
    result=unix_publish_packet(request,(uint32_t)count,control);
    return result;
}

#define ROUTE_SNAPSHOT_MAX 16U
#define ROUTE_RESPONSE_MAX 16384U
static int route_snapshot(struct kernel_socket_interface *,size_t *);
static int netlink_send(struct kernel_socket_write_request *request)
{
    struct kernel_socket *socket=request->socket;
    if(request->payload_size<sizeof(struct route_header))return (int)request->payload_size;
    size_t count=0;
    int result;
    struct socket_packet *packet=NULL;
    const size_t snapshot_offset=(sizeof(*packet)+ROUTE_RESPONSE_MAX+
        _Alignof(struct kernel_socket_interface)-1)&~(size_t)(_Alignof(struct kernel_socket_interface)-1);
    KERNEL_NO_RECLAIM_IO;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(socket->heap,1,
        snapshot_offset+sizeof(struct kernel_socket_interface)*ROUTE_SNAPSHOT_MAX,(void **)&packet);
    if(status!=KERNEL_HEAP_STATUS_OK) {
        if(status!=KERNEL_HEAP_STATUS_EMPTY)__builtin_trap();
        return -KERNEL_ENOMEM;
    }
    struct kernel_socket_interface *interfaces=(void *)((unsigned char *)packet+snapshot_offset);
    result=route_snapshot(interfaces,&count);
    if(result){kernel_heap_release(socket->heap,packet);return result;}
    struct route_output out={.data=packet->data,.capacity=ROUTE_RESPONSE_MAX,.port=socket->netlink_port};
    result=route_encode(&out,((struct socket_packet *)request->packet)->data,request->payload_size,interfaces,count);
    if(!result && out.used) {
        /* Queue only exact-sized storage: quota tracks the allocation, not
         * a small payload retained in the maximum-size encoding workspace. */
        struct socket_packet *exact=NULL;
        status=kernel_heap_allocate_zeroed(socket->heap,1,sizeof(*exact)+out.used,(void **)&exact);
        if(status!=KERNEL_HEAP_STATUS_OK) {
            if(status!=KERNEL_HEAP_STATUS_EMPTY)__builtin_trap();
            kernel_heap_release(socket->heap,packet);return -KERNEL_ENOMEM;
        }
        __builtin_memcpy(exact->data,packet->data,out.used);
        kernel_heap_release(socket->heap,packet);packet=exact;
        packet->length=(uint32_t)out.used;
        packet->address.family=KERNEL_SOCKET_AF_NETLINK;
        uint32_t charge=packet_charge(packet->length);
        SOCKET_ENDPOINT(state,socket);
        if(charge>socket->rx_limit || socket->record_memory>socket->rx_limit-charge)result=-KERNEL_ENOBUFS;
        else {
            socket->record_memory+=charge;
            packet_publish_locked(socket,packet,packet->length);packet=NULL;
        }
    }
    if(packet)kernel_heap_release(socket->heap,packet);
    return result?result:(int)request->payload_size;
}

static int packet_send_frame(struct kernel_socket_write_request *request)
{
    struct kernel_socket *socket=request->socket;
    struct socket_packet *packet=request->packet;
    struct kernel_socket_address *address=&request->inet_destination;
    struct kernel_socket_protocol_scope protocol;
    kernel_socket_protocol_enter(&protocol);
    struct netif *interface=address->scope && address->scope<=255 ?
        netif_get_by_index((uint8_t)address->scope) : NULL;
    int result=-KERNEL_ENXIO;
    if (!interface) goto Out;
    if (!netif_is_up(interface)) { result=-KERNEL_ENETDOWN;goto Out; }
    struct netif *loop=loopback_netif();
    uint32_t bytes=packet->length;
    if (socket->type==SOCKET_DGRAM) {
        if (interface!=loop) {
            if (!address->link_size) { result=-KERNEL_EINVAL;goto Out; }
            __builtin_memcpy(packet->data,address->link_address,6);
            __builtin_memcpy(packet->data+6,interface->hwaddr,6);
        }
        packet->data[12]=(unsigned char)(address->port>>8);
        packet->data[13]=(unsigned char)address->port;
        bytes+=PACKET_ETH_HEADER;
    }
    if (interface!=loop && bytes>interface->mtu+PACKET_ETH_HEADER) {
        result=-KERNEL_EMSGSIZE;goto Out;
    }
    struct pbuf *payload=NULL;
    unsigned ether_type=(unsigned)packet->data[12]<<8|packet->data[13];
    if (interface==loop && ((ether_type==0x0800 && bytes>=PACKET_ETH_HEADER+20) ||
        (ether_type==0x86dd && bytes>=PACKET_ETH_HEADER+40))) {
        payload=pbuf_alloc(PBUF_RAW,(u16_t)(bytes-PACKET_ETH_HEADER),PBUF_RAM);
        if (!payload) { result=-KERNEL_ENOBUFS;goto Out; }
        __builtin_memcpy(payload->payload,packet->data+PACKET_ETH_HEADER,bytes-PACKET_ETH_HEADER);
        payload->if_idx=netif_get_index(interface);
    }
    result=interface==loop ? 0 : kernel_network_send_frame(interface,packet->data,(uint16_t)bytes);
    if (!result) {
        KERNEL_NO_RECLAIM_IO;
        struct packet_frame view={.bytes=packet->data,.length=bytes};
        packet_tap(interface,&view,PACKET_OUTGOING,socket);
        if (interface==loop) packet_tap(interface,&view,PACKET_HOST,NULL);
        result=(int)packet->length;
    }
    if (payload && (result<0 || ip_input(payload,interface)!=ERR_OK)) pbuf_free(payload);
Out:
    kernel_socket_protocol_leave(&protocol);
    return result;
}

int kernel_socket_packet_write(struct kernel_socket_write_request *request,
    struct kernel_mm *mm,struct kernel_uaccess_iov_cursor *cursor,
    const void *kernel_buffer,int *user_fault,struct kernel_socket_message *message)
{
    if(!request || !request->socket || !request->active_previous)__builtin_trap();
    if(user_fault)*user_fault=0;
    struct kernel_socket_control **control=message?&message->control:&request->automatic_control;
    struct socket_packet *packet=request->packet;
    uint64_t count=request->payload_size;
    void *data=request->socket->domain==KERNEL_SOCKET_DOMAIN_INET
        ?((struct pbuf *)request->protocol_payload)->payload:packet->data;
    if (request->socket->domain==KERNEL_SOCKET_DOMAIN_PACKET && request->socket->type==SOCKET_DGRAM)
        data=packet->data+PACKET_ETH_HEADER;
    int result=0;
    if (kernel_buffer) {
        if (count) __builtin_memcpy(data, kernel_buffer, (size_t)count);
    } else {
        size_t copied = 0;
        enum kernel_uaccess_status access = kernel_copy_from_user_iov(mm, cursor,
            data, (size_t)count, &copied);
        if (access == KERNEL_UACCESS_STATUS_FAULT) {
            if (user_fault) *user_fault=1;
            if (request->socket->type != SOCKET_STREAM || !copied) {
                if (kernel_socket_is_udp(request->socket) && udp_has_pending(request->socket))
                    (void)protocol_request(request,SOCKET_PROTOCOL_DATAGRAM_CANCEL);
                result = -KERNEL_EFAULT; goto out;
            }
            count=copied; packet->length=(uint32_t)copied;
        } else if (access != KERNEL_UACCESS_STATUS_OK || copied != count) __builtin_trap();
    }
    if(request->socket->domain==KERNEL_SOCKET_DOMAIN_INET) {
        result=protocol_request(request,SOCKET_PROTOCOL_DATAGRAM_SEND);goto out;
    }
    if(request->socket->domain==KERNEL_SOCKET_DOMAIN_NETLINK) {
        result=netlink_send(request);goto out;
    }
    if (request->socket->domain==KERNEL_SOCKET_DOMAIN_PACKET) {
        result=packet_send_frame(request);goto out;
    }
    result=socket_finish_packet(request,(uint32_t)count,control);
    if(result<0)goto out;
    *request->pin_owner=request->pin;request->pin=NULL;request->socket=NULL;
    return result;
out:
    clear_write_request(request);
    *request->pin_owner=request->pin;request->pin=NULL;request->socket=NULL;
    return result;
}

static int socket_write_packet_source(struct kernel_open_file_description **pin_owner,
    struct kernel_mm *mm,struct kernel_uaccess_iov_cursor *cursor,
    uint64_t count,uint32_t flags,const struct kernel_socket_address *destination,
    const void *kernel_buffer,int *user_fault,struct kernel_socket_message *message)
{
    struct kernel_socket_write_request request={0};
    if(user_fault)*user_fault=0;
    int result=kernel_socket_packet_prepare(&request,pin_owner,count,flags,destination,
        kernel_buffer!=NULL,message);
    if(result || !request.socket)return result;
    return kernel_socket_packet_write(&request,mm,cursor,kernel_buffer,user_fault,message);
}

int kernel_socket_write_datagram(struct kernel_open_file_description **pin_owner,
    struct kernel_mm *mm, const struct kernel_uaccess_iovec *iov,
    size_t iov_count, uint64_t count, uint32_t flags,
    const struct kernel_socket_address *destination, struct kernel_socket_message *message)
{
    struct kernel_uaccess_iov_cursor cursor={iov,iov_count,0,0};
    return socket_write_packet_source(pin_owner, mm, &cursor, count,
                                        flags, destination, 0, 0, message);
}

int kernel_socket_write_packet_buffer(struct kernel_open_file_description **pin_owner,
    const void *buffer, uint64_t count, uint32_t flags)
{
    if (!buffer) return -KERNEL_EINVAL;
    return socket_write_packet_source(pin_owner, 0, 0, count, flags, 0, buffer, 0, NULL);
}

int kernel_socket_write_unix(struct kernel_open_file_description **pin_owner,
    struct kernel_mm *mm, struct kernel_uaccess_iov_cursor *cursor,
    uint64_t count, uint32_t flags, int *user_fault, struct kernel_socket_message *message)
{
    return socket_write_packet_source(pin_owner,mm,cursor,count,flags,0,0,user_fault,message);
}

int kernel_socket_discard_receive(const struct kernel_socket *socket, uint32_t flags)
{
    return socket && socket->domain == KERNEL_SOCKET_DOMAIN_INET &&
           socket->type == SOCKET_STREAM && (flags & KERNEL_SOCKET_MSG_TRUNC);
}

static struct kernel_socket_statistics socket_statistics;
void kernel_socket_get_statistics(struct kernel_socket_statistics *statistics)
{
    statistics->tcp_write_calls=__atomic_load_n(&socket_statistics.tcp_write_calls,__ATOMIC_RELAXED);
    statistics->tcp_written_bytes=__atomic_load_n(&socket_statistics.tcp_written_bytes,__ATOMIC_RELAXED);
}

#if BOAROS_COST_DIAGNOSTICS
void kernel_socket_protocol_snapshot(uint64_t values[KERNEL_SOCKET_PROTOCOL_VALUES])
{
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    const uint64_t snapshot[KERNEL_SOCKET_PROTOCOL_VALUES] = {
        __atomic_load_n(&socket_statistics.tcp_write_calls,__ATOMIC_RELAXED),
        __atomic_load_n(&socket_statistics.tcp_written_bytes,__ATOMIC_RELAXED),
        STATS_GET(tcp.xmit),STATS_GET(tcp.recv),STATS_GET(tcp.memerr),
        STATS_GET(udp.xmit),STATS_GET(udp.recv),STATS_GET(udp.memerr),
        STATS_GET(mem.used),STATS_GET(mem.max),
        (lwip_stats.memp[MEMP_TCP_PCB] ? MEMP_STATS_GET(used,MEMP_TCP_PCB) : 0),
        (lwip_stats.memp[MEMP_TCP_PCB_LISTEN] ? MEMP_STATS_GET(used,MEMP_TCP_PCB_LISTEN) : 0),
        (lwip_stats.memp[MEMP_TCP_SEG] ? MEMP_STATS_GET(used,MEMP_TCP_SEG) : 0),
        (lwip_stats.memp[MEMP_UDP_PCB] ? MEMP_STATS_GET(used,MEMP_UDP_PCB) : 0),
        (lwip_stats.memp[MEMP_PBUF] ? MEMP_STATS_GET(used,MEMP_PBUF) : 0),sizeof(STAT_COUNTER)*8U,
        (lwip_stats.memp[MEMP_TCP_SEG] ? MEMP_STATS_GET(max,MEMP_TCP_SEG) : 0),
        (lwip_stats.memp[MEMP_PBUF] ? MEMP_STATS_GET(max,MEMP_PBUF) : 0),
        (lwip_stats.memp[MEMP_PBUF_POOL] ? MEMP_STATS_GET(used,MEMP_PBUF_POOL) : 0),
        (lwip_stats.memp[MEMP_PBUF_POOL] ? MEMP_STATS_GET(max,MEMP_PBUF_POOL) : 0),
        (lwip_stats.memp[MEMP_TCP_PCB] ? MEMP_STATS_GET(max,MEMP_TCP_PCB) : 0),
        (lwip_stats.memp[MEMP_TCP_PCB_LISTEN] ? MEMP_STATS_GET(max,MEMP_TCP_PCB_LISTEN) : 0)
    };
    __builtin_memcpy(values,snapshot,sizeof(snapshot));
    kernel_socket_protocol_leave(&irq);
}
#endif

int kernel_socket_is_tcp(const struct kernel_socket *socket)
{ return socket && socket->domain == KERNEL_SOCKET_DOMAIN_INET && socket->type == SOCKET_STREAM; }

int kernel_socket_tcp_receive_control(struct kernel_socket *socket, uint32_t flags)
{
    /* The TCP endpoint has neither a queued asynchronous error nor urgent data. */
    if (flags & KERNEL_SOCKET_MSG_ERRQUEUE) return -KERNEL_EAGAIN;
    SOCKET_ENDPOINT(state,socket);
    return socket->listening ? -KERNEL_ENOTCONN : -KERNEL_EINVAL;
}

/* 调用者持raw资格；此处交付状态错误，不能先碰payload改变errno优先级。 */
static int tcp_write_state(struct kernel_socket *socket, uint32_t flags, int observe, struct kernel_task *task)
{
    int result;
    if (socket->pending_error) {
        result = socket->pending_error;
        if (observe) socket->pending_error = 0;
        return result;
    }
    if (network_is_stopping()) result = -KERNEL_EIO;
    else if (socket->write_closed || (!socket->connected && !socket->connecting) || !socket->tcp)
        result = -KERNEL_EPIPE;
    else result = socket->connecting ? -KERNEL_EAGAIN : 0;
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
        __atomic_load_n(&socket->write_blocked,__ATOMIC_RELAXED) || tcp_sndqueuelen(socket->tcp) >= TCP_SND_QUEUELEN) return 0;
    uint32_t available = tcp_sndbuf(socket->tcp);
    uint32_t outstanding = tcp_sndbuf_limit(socket->tcp) - available;
    uint32_t budget = outstanding < socket->tx_limit ? socket->tx_limit - outstanding : 0;
    return available < budget ? available : budget;
}
static void tcp_publish_capacity_locked(struct kernel_socket *socket)
{
    socket->tx_budget = tcp_protocol_budget(socket);
#if BOAROS_USER_SMP
    if (tcp_socket_cpu(socket))
        __atomic_store_n(&socket->tcp_prepare_mss,socket->tcp && !socket->listening?
            boaros_lwip_tcp_detached_mss(socket->tcp):0,__ATOMIC_RELAXED);
#endif
}
static void socket_tcp_output(struct tcp_pcb *pcb)
{
    struct kernel_socket *socket=pcb->errf==tcp_failed?pcb->callback_arg:NULL;
    unsigned *blocked=&nic_blocked;
#if BOAROS_USER_SMP
    struct socket_tcp_owner *owner=current_tcp_owner();
    if (owner) blocked=&owner->nic_blocked;
    uint64_t capacity = owner || !socket ? __atomic_load_n(&nic_generation,__ATOMIC_RELAXED) : 0;
#else
    uint64_t capacity = !socket ? __atomic_load_n(&nic_generation,__ATOMIC_RELAXED) : 0;
#endif
    *blocked=0;
    if (!socket) pcb->boaros_nic_retry=0;
    unsigned was_corked = pcb->boaros_cork_pending;
    err_t output = tcp_output(pcb);
    if (!socket) {
        if (output==ERR_MEM && *blocked) {
            SOCKET_STATE(state);
            orphan_nic_register_locked(state.owner,pcb,capacity);
        }
        return;
    }
    /* A direct caller may have armed a deadline while the worker was asleep. */
    if (!was_corked && pcb->boaros_cork_pending) socket_schedule(socket, 0);
    if (pcb->state==TIME_WAIT) { retire_timewait(socket); return; }
    if (output == ERR_MEM) {
        if (*blocked) {
            SOCKET_STATE(state);
            work_remove_locked(&nic_wait, socket);
#if BOAROS_USER_SMP
            if (owner) {
                /* A completion can now be published before the failed
                 * submitter acquires the work lock to register its retry. */
                work_add_locked(&nic_wait, socket, capacity);
                if (capacity != nic_generation) socket_work_ready_locked(state.owner);
            } else
#endif
                work_add_locked(&nic_wait, socket, nic_generation);
        } else socket_pool_wait(socket);
    }
}

static void tcp_flush_output(struct kernel_socket *socket)
{
    if (socket->tcp && (socket->tcp->unsent || (socket->tcp->flags&TF_ACK_NOW)))
        socket_tcp_output(socket->tcp);
    else COST_ADD(TCP_EMPTY_OUTPUT_REQUESTS, 1);
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
    unsigned schedule_output=0;
    KERNEL_IRQ_LOCK_SCOPE(irq,socket_endpoint_lock(request->socket));
    if (!request->pin || request->reserved) __builtin_trap();
    int error = tcp_write_state(request->socket, flags, !request->progressed, request->task);
    uint32_t available = error ? 0 : tcp_admission_capacity_locked(request->socket);
    if (!error && size) {
        if (size > available) size = available;
        if (!size) {
            /* A full send budget on a shard can mean only unacknowledged
             * bytes. Its real output is covered by the committing request,
             * pending work, or the NIC/pool retry queues. A deferred prefix
             * still needs a push before its writer waits for credit. */
            int require_output = request->progressed & SOCKET_WRITE_DEFERRED;
            if (!request->socket->tx_budget
#if BOAROS_USER_SMP
                && !tcp_socket_cpu(request->socket)
#endif
                ) require_output = 1;
            if (require_output)
                schedule_output=SOCKET_WORK_OUTPUT |
                    (request->progressed ? SOCKET_WORK_PUSH : 0);
            request->progressed &= (uint8_t)~SOCKET_WRITE_DEFERRED;
            error = -KERNEL_EAGAIN; COST_ADD(STREAM_ADMIT_BLOCKED, 1);
        }
        else {
            request->reserved = size;
            request->socket->tx_reserved += size;
#if BOAROS_USER_SMP
            request->tcp_prepare_mss=__atomic_load_n(&request->socket->tcp_prepare_mss,__ATOMIC_RELAXED);
#endif
        }
    }
    kernel_irq_lock_release(&irq);
    if (schedule_output) {
        COST_ADD(TCP_ADMIT_OUTPUT_REQUESTS, 1);
        socket_schedule(request->socket,schedule_output);
    }
    return error ? error : (int)size;
}
void kernel_socket_stream_cancel(struct kernel_socket_write_request *request)
{
    if (!request->reserved) return;
    SOCKET_ENDPOINT(irq,request->socket);
    if (request->socket->tx_reserved < request->reserved) __builtin_trap();
    uint32_t previous=tcp_admission_capacity_locked(request->socket);
    request->socket->tx_reserved -= request->reserved; request->reserved = 0;
    if (tcp_admission_capacity_locked(request->socket)>previous)
        socket_state_notify(irq.owner,&request->socket->wait);
    socket_state_release(&irq);
}
void kernel_socket_stream_finish(struct kernel_socket_write_request *request)
{
    int deferred=(request->progressed & SOCKET_WRITE_DEFERRED)!=0;
    kernel_socket_stream_cancel(request);
    if(deferred)(void)protocol_request(request,SOCKET_PROTOCOL_FLUSH);
    socket_write_end(request);
    if (*request->pin_owner || !request->pin) __builtin_trap();
    *request->pin_owner = request->pin;
    request->pin = 0;
    request->socket = 0;
}

static void complete_protocol_request_locked(struct kernel_socket_write_request *request)
{
#if BOAROS_COST_DIAGNOSTICS
    request->completed_ticks=kernel_cost_clock();
#endif
    request->segments=0;
    request->queued=0;
    /* FIFO and synchronous completion share the work queue lock. */
    if(kernel_wait_notify(&request->completion,KERNEL_WAIT_WOKEN)!=KERNEL_SCHEDULER_STATUS_OK)
        __builtin_trap();
}

static int stream_copy_owned(struct kernel_socket_write_request *request,
    uint32_t completed_reservations, int *flush)
{
    const struct kernel_socket_write_segment *segments=request->segments;
    unsigned count=request->segment_count;
    uint32_t flags=request->flags;
    uint32_t prepared=0;
#if BOAROS_USER_SMP
    prepared=request->tcp_prepare_size;
#endif
    if ((!prepared && !count) || !request->reserved) __builtin_trap();
    struct kernel_socket *socket = request->socket;
    uint32_t total = 0, available = 0;
    int result;
    { KERNEL_IRQ_LOCK_SCOPE(endpoint,socket_endpoint_lock(socket));
      result = tcp_write_state(socket, flags, !request->progressed, request->task);
      if (socket->tx_reserved < (uint64_t)completed_reservations + request->reserved) __builtin_trap();
      uint32_t others = socket->tx_reserved - completed_reservations - request->reserved;
      uint32_t budget = result ? 0 : tcp_protocol_budget(socket);
      available = budget > others ? budget - others : 0;
    }
    if (!result) {
        if (!prepared)
            for (unsigned i = 0; i < count; i++) {
                uint32_t size = segments[i].size;
                if (!size || size > request->reserved - prepared) __builtin_trap();
                prepared += size;
            }
        uint32_t length = available < prepared ? available : prepared;
        if (length > UINT16_MAX) length = UINT16_MAX;
        if (!length) result = -KERNEL_EAGAIN;
        else {
            /* Pins cover the whole vector. lwIP copies directly into final
             * segments and publishes only the prefix it could prepare. */
            u16_t accepted = 0;
            u8_t write_flags = TCP_WRITE_FLAG_COPY;
            if ((flags & (KERNEL_SOCKET_IO_MORE | KERNEL_SOCKET_MSG_MORE)) || length < prepared)
                write_flags |= TCP_WRITE_FLAG_MORE;
            __atomic_fetch_add(&socket_statistics.tcp_write_calls,1,__ATOMIC_RELAXED);
            err_t error;
#if BOAROS_USER_SMP
            if (request->tcp_prepare_size) {
                if (request->tcp_prepare_failed) write_flags|=TCP_WRITE_FLAG_MORE;
                error=request->tcp_prepared?boaros_lwip_tcp_publishv(socket->tcp,
                    &request->tcp_prepared,(u16_t)length,write_flags,&accepted):ERR_MEM;
                if (error==ERR_OK && request->tcp_prepare_failed) error=ERR_MEM;
            } else
#endif
            {
                error=boaros_lwip_tcp_writev(socket->tcp,segments,count,
                    (u16_t)length,write_flags,&accepted);
                if (error==ERR_OK && accepted!=length) __builtin_trap();
            }
            if (accepted) {
                __atomic_fetch_add(&socket_statistics.tcp_written_bytes,accepted,__ATOMIC_RELAXED);
                write_retry_disarm(socket);
                total = accepted;
                socket->tcp->boaros_output_more = !!(flags & KERNEL_SOCKET_MSG_MORE);
                if (accepted < prepared) {
                    boaros_lwip_tcp_push(socket->tcp);
                    *flush = 1;
                }
            }
            if (error == ERR_MEM) {
                /* Pool exhaustion still suppresses POLLOUT until release. */
                write_retry_arm(socket);
                socket_pool_wait(socket);
                result = -KERNEL_EAGAIN; *flush = 1;
                if (total || request->progressed) boaros_lwip_tcp_push(socket->tcp);
            } else result = error==ERR_WOULDBLOCK?-KERNEL_EAGAIN:lwip_error(error);
        }
    }
    if (total) result = (int)total;
    if (result == -KERNEL_EAGAIN) {
        if (request->progressed && socket->tcp) boaros_lwip_tcp_push(socket->tcp);
        *flush = 1;
        request->progressed &= (uint8_t)~SOCKET_WRITE_DEFERRED;
        COST_ADD(STREAM_PROTOCOL_BLOCKED, 1);
    }
    return result;
}

static void stream_commit_owned(struct kernel_socket_write_request **requests, unsigned count, int defer_output)
{
    COST_SCOPE(execution, NETWORK_PROTOCOL_EXEC_TICKS);
    struct kernel_socket *socket=requests[0]->socket;
    uint32_t reserved=0;
    int flush=0;
    /* 预约在输出前仍计入远端可接纳额度；协议 owner 读实际 PCB 额度时，
     * 已处理请求的预约不再重复扣减。 */
    for(unsigned i=0;i<count;i++) {
        struct kernel_socket_write_request *request=requests[i];
#if BOAROS_COST_DIAGNOSTICS
        if(request->queued)request->executing_ticks=kernel_cost_clock();
#endif
        request->result=stream_copy_owned(request,reserved,&flush);
        reserved+=request->reserved;
    }
    {
        SOCKET_ENDPOINT(endpoint,socket);
        uint32_t previous=tcp_admission_capacity_locked(socket);
        tcp_publish_capacity_locked(socket);
        if(socket->tx_reserved<reserved)__builtin_trap();
        socket->tx_reserved-=reserved;
        uint32_t available=tcp_admission_capacity_locked(socket);
        /* An IO_MORE prefix runs alone. Use the credit visible to its next
         * reservation at publication to decide whether it can defer output. */
        for (unsigned i=0;i<count;i++) {
            struct kernel_socket_write_request *request=requests[i];
            request->reserved=0;
            if (request->result<=0) continue;
            request->progressed=SOCKET_WRITE_ACCEPTED;
            if ((request->flags & KERNEL_SOCKET_IO_MORE) && available)
                request->progressed|=SOCKET_WRITE_DEFERRED;
            else flush=1;
        }
        /* COPY 消耗与预约归还共同决定额度变化，只有净增加才唤醒额度等待者。 */
        if(available>previous) {
            socket_state_notify(endpoint.owner,&socket->wait);socket_async(socket,KERNEL_ASYNC_POLL_OUT);
        }
    }
    if(requests[0]->queued) {
        KERNEL_IRQ_LOCK_SCOPE(queue_state,&socket_state_lock);
        for(unsigned i=0;i<count;i++) complete_protocol_request_locked(requests[i]);
    }
    if(flush && defer_output)socket_schedule(socket,SOCKET_WORK_OUTPUT);
    /* caller可立即归还请求栈；出包只借用协议scope固定的socket，不再访问requests。
     * 同一次服务仍完成原输出预算，最后close须等这份协议资格归还。 */
    if(flush && !defer_output)tcp_flush_output(socket);
}

/* Each send already owns a copied PBUF_RAM. Keep the private buffers in
 * order and fill their aggregate lengths once, when the datagram leaves. */
static int udp_pending_append(struct kernel_socket_write_request *request,
                              const ip_addr_t *remote,uint16_t port)
{
    struct kernel_socket *socket=request->socket;
    struct socket_udp_pending *pending=socket->udp_pending;
    struct pbuf *part=request->protocol_payload;
    uint32_t limit;
    { SOCKET_ENDPOINT(state,socket);limit=socket->tx_limit; }
    uint32_t charge=LWIP_MEM_ALIGN_SIZE(sizeof(*part))+LWIP_MEM_ALIGN_SIZE(PBUF_TRANSPORT)+
        LWIP_MEM_ALIGN_SIZE(part->len);
    if (pending && (pending->bytes+part->len>UDP_MAX_PAYLOAD || pending->bytes+part->len>limit))
        return -KERNEL_EMSGSIZE;
    /* Bound the memory held by many tiny sends as well as the wire size. */
    if (pending && part->len && pending->charge+charge>2U*limit) return -KERNEL_ENOBUFS;
    if (!pending) {
        enum kernel_heap_status status=kernel_heap_allocate_zeroed(socket->heap,1,sizeof(*pending),
            (void **)&pending);
        if (status==KERNEL_HEAP_STATUS_EMPTY) return -KERNEL_ENOMEM;
        if (status!=KERNEL_HEAP_STATUS_OK) __builtin_trap();
        pending->remote=*remote;
        pending->port=port;
        pending->head=part;
        __atomic_store_n(&socket->udp_pending,pending,__ATOMIC_RELAXED);
    } else if (!part->len) {
        kernel_socket_pbuf_free(part);
        request->protocol_payload=NULL;
        return 0;
    } else pending->tail->next=part;
    request->protocol_payload=NULL;
    pending->tail=part;
    pending->bytes+=part->len;
    pending->charge+=charge;
    return 0;
}

enum { DATAGRAM_REQUIRES_WORKER = INT32_MIN };

#if BOAROS_USER_SMP
static void udp4_remember_path(struct kernel_socket *socket, struct netif *interface,
    const ip_addr_t *src,const ip_addr_t *dst,uint16_t port)
{
    struct udp_pcb *pcb=socket->udp;
    const struct boaros_udp4_path *current=&pcb->boaros_path4;
    if (current->ip.generation==boaros_lwip_ipv4_path_generation() &&
        current->ip.interface==interface && current->remote_port==port &&
        ip4_addr_eq(&current->ip.source,ip_2_ip4(src)) &&
        ip4_addr_eq(&current->ip.destination,ip_2_ip4(dst))) return;
    struct boaros_udp4_path resolved={0};
    boaros_ipv4_path_resolve(&resolved.ip,interface,ip_2_ip4(src),ip_2_ip4(dst),pcb->netif_idx);
    if (!resolved.ip.generation) return;
    resolved.local_port=pcb->local_port;
    resolved.remote_port=port;
    resolved.ttl=socket->ipv4_ttl;
    resolved.tos=pcb->tos;
    resolved.no_checksum=!!(pcb->flags & UDP_FLAGS_NOCHKSUM);
    resolved.matches_peer=socket->connected && port==pcb->remote_port &&
        ip_addr_cmp(dst,&pcb->remote_ip);
    KERNEL_IRQ_LOCK_SCOPE(state,&socket_state_lock);
    pcb->boaros_path4=resolved;
}
#endif

static int datagram_send_owned(struct kernel_socket_write_request *request, int caller_only)
{
    struct kernel_socket *socket=request->socket;
    struct socket_udp_pending *pending=socket->udp_pending;
    struct pbuf *payload=request->protocol_payload;
    if (!payload) __builtin_trap();
    ip_addr_t remote={0};
    uint16_t port=request->destination?request->destination->port:socket->udp?socket->udp->remote_port:0;
    if (pending) {
        remote=pending->remote;
        port=pending->port;
    } else if (request->destination && request->destination->family==KERNEL_SOCKET_AF_INET) {
        IP_SET_TYPE(&remote,IPADDR_TYPE_V4);
        __builtin_memcpy(&ip_2_ip4(&remote)->addr,request->destination->bytes,4);
    } else if (request->destination) {
        if (request->destination->family!=KERNEL_SOCKET_AF_INET6) __builtin_trap();
        IP_SET_TYPE(&remote,IPADDR_TYPE_V6);
        __builtin_memcpy(ip_2_ip6(&remote)->addr,request->destination->bytes,16);
        ip6_addr_set_zone(ip_2_ip6(&remote),(u8_t)request->destination->scope);
    } else ip_addr_copy(remote,socket_ip_pcb(socket)->remote_ip);

    int failure=0;
    if (socket->udp) {
        if (socket->write_closed) failure=-KERNEL_EPIPE;
        else if (!pending && !request->destination && !socket->connected) failure=-KERNEL_EDESTADDRREQ;
        else if (!port) failure=-KERNEL_EINVAL;
    }
    struct netif *route=NULL;
    const ip_addr_t *source=NULL;
    if (!failure) failure=socket_ip_route(socket,&remote,&route,&source);
    if (caller_only && (failure || !IP_IS_V4(&remote) || ip_addr_ismulticast(&remote) ||
        ip_addr_isbroadcast(&remote, route) ||
        (route != netif_get_loopif() && (route->output != etharp_output || !route->boaros_link_submit))))
        return DATAGRAM_REQUIRES_WORKER;
    uint32_t length=request->payload_size+(pending?pending->bytes:0);
    if (!failure && IP_IS_V6(&remote) && route->mtu &&
        length+48U>nd6_get_destination_mtu(ip_2_ip6(&remote),route)) failure=-KERNEL_EMSGSIZE;

    int more=!!(request->flags&KERNEL_SOCKET_MSG_MORE);
    if (!failure && socket->udp && (pending || more)) {
        if (!socket->udp->local_port)
            failure=lwip_error(udp_bind(socket->udp,&socket->udp->local_ip,0));
        if (!failure) failure=udp_pending_append(request,&remote,port);
        if (!failure && more) {
            request->progressed=SOCKET_WRITE_DEFERRED;
            return (int)request->payload_size;
        }
        if (!failure) {
            payload=udp_pending_take(socket);
            for (struct pbuf *part=payload;part;part=part->next) {
                part->tot_len=(u16_t)length;
                length-=part->len;
            }
        }
    }
    if (failure) udp_pending_discard(socket);
    else {
        struct ip_pcb *pcb=socket_ip_pcb(socket);
        pcb->ttl=IP_IS_V4(&remote)?socket->ipv4_ttl:64;
        if (socket->udp) {
            udp_set_multicast_ttl(socket->udp,IP_IS_V4(&remote)?socket->multicast_ttl:socket->ipv6_multicast_hops);
            if (IP_IS_V4(&remote)?socket->ipv4_multicast_loop:socket->ipv6_multicast_loop)
                socket->udp->flags|=UDP_FLAGS_MULTICAST_LOOP;
            else socket->udp->flags&=(uint8_t)~UDP_FLAGS_MULTICAST_LOOP;
            if (route==netif_get_loopif() && !payload->next && !ip_addr_ismulticast(&remote)) {
                /* The outer synchronous send can finish its own input.
                 * Queued requests share the worker's packet budget. */
                payload->boaros_udp_loopback=protocol_depth==1 && !request->queued?
                    BOAROS_UDP_LOOPBACK_TRANSFER_INLINE:BOAROS_UDP_LOOPBACK_TRANSFER;
            }
        }
        if (IP_IS_V4(&remote)) payload->flags|=PBUF_FLAG_IP4_LOCAL_DF;
        failure=lwip_error(socket->raw?raw_sendto_if_src(socket->raw,payload,&remote,route,source):
            udp_sendto_if_src(socket->udp,payload,&remote,port,route,source));
#if BOAROS_USER_SMP
        if (!failure && !more && !pending && udp4_output_socket(socket) &&
            route->boaros_link_submit && boaros_lwip_parallel_enabled())
            udp4_remember_path(socket,route,source,&remote,port);
#endif
    }
    /* Transfer the sole reference after output has returned and reset the
     * netif hints. The input may free the payload before returning. */
    request->protocol_payload=NULL;
    if (payload->boaros_udp_loopback==BOAROS_UDP_LOOPBACK_INLINE) {
        COST_ADD(NETWORK_LOOP_PACKETS, 1);
        boaros_lwip_udp_loopback_input(route,payload);
        /* An ICMP reply may have put new work on a loopback queue. */
        if (!loopback_pending) request->progressed|=SOCKET_WRITE_LOOPBACK_INLINE;
    } else if (payload->boaros_udp_loopback!=BOAROS_UDP_LOOPBACK_QUEUED)
        kernel_socket_pbuf_free(payload);
    return failure?failure:(int)request->payload_size;
}

static int execute_protocol_request(struct kernel_socket_write_request *request)
{
    if(request->operation==SOCKET_PROTOCOL_STREAM) {
        stream_commit_owned(&request,1,0);
        return request->result;
    }
    COST_SCOPE(execution, NETWORK_PROTOCOL_EXEC_TICKS);
    struct kernel_socket *socket=request->socket;
    switch(request->operation) {
    case SOCKET_PROTOCOL_DATAGRAM_PREPARE: return datagram_prepare_payload(request);
    case SOCKET_PROTOCOL_DATAGRAM_SEND: return datagram_send_owned(request,0);
    case SOCKET_PROTOCOL_DATAGRAM_CANCEL: udp_pending_discard(socket);return 0;
    case SOCKET_PROTOCOL_DISPOSE:
        if(!request->protocol_payload)__builtin_trap();
        kernel_socket_pbuf_free(request->protocol_payload);request->protocol_payload=0;return 0;
    case SOCKET_PROTOCOL_FLUSH:
        tcp_flush_output(socket);request->progressed &= (uint8_t)~SOCKET_WRITE_DEFERRED;return 0;
    }
    __builtin_trap();
}

#if BOAROS_USER_SMP
static int udp4_try_resolved(struct kernel_socket_write_request *request)
{
    struct kernel_socket *socket=request->socket;
    struct boaros_udp4_path route;
    {
        KERNEL_IRQ_LOCK_SCOPE(state,&socket_state_lock);
        const struct boaros_udp4_path *stored=&socket->udp->boaros_path4;
        if (!stored->ip.generation || !network_work_queue || network_stopping ||
            request->payload_size+UDP_HLEN+IP_HLEN>stored->ip.mtu || udp_has_pending(socket) ||
            stored->ip.generation!=boaros_lwip_ipv4_path_generation())
            return DATAGRAM_REQUIRES_WORKER;
        if (request->destination) {
            uint32_t address;
            __builtin_memcpy(&address,request->destination->bytes,sizeof(address));
            if (request->destination->family!=KERNEL_SOCKET_AF_INET ||
                address!=stored->ip.destination.addr || request->destination->port!=stored->remote_port)
                return DATAGRAM_REQUIRES_WORKER;
        } else if (!stored->matches_peer) return DATAGRAM_REQUIRES_WORKER;
        if (!udp4_begin_output_locked(request)) return DATAGRAM_REQUIRES_WORKER;
        route=*stored;
    }
    int result=DATAGRAM_REQUIRES_WORKER;
    if (route.ip.generation==boaros_lwip_ipv4_path_generation()) {
        struct pbuf *payload=request->protocol_payload;
        int error=lwip_error(boaros_udp4_output_resolved(&route,payload));
        request->protocol_payload=NULL;
        kernel_socket_pbuf_free(payload);
        result=error ? error : (int)request->payload_size;
    }
    udp4_end_output(request);
    return result;
}

static int datagram_try_caller(struct kernel_socket_write_request *request)
{
    struct kernel_socket *socket = request->socket;
    if (!udp4_output_socket(socket) || (request->flags & KERNEL_SOCKET_MSG_MORE) ||
        !boaros_lwip_parallel_enabled())
        return DATAGRAM_REQUIRES_WORKER;

    /* Keep the response to an isolated receive on the normal path. An
     * outbound burst or a reader with packets left can use the snapshot. */
    if (!request->udp_rx_drained) {
        int cached=udp4_try_resolved(request);
        if (cached!=DATAGRAM_REQUIRES_WORKER) return cached;
    }
    if (!kernel_cpu_current()->logical_id ||
        !__atomic_load_n(&protocol_mutex_initialized,__ATOMIC_ACQUIRE))
        return DATAGRAM_REQUIRES_WORKER;

    struct kernel_socket_protocol_scope scope = {0};
    if (!protocol_lock(&scope, 1)) {
        arch_interrupt_restore(scope.interrupts);
        return DATAGRAM_REQUIRES_WORKER;
    }
    int result = DATAGRAM_REQUIRES_WORKER;
    if (scope.outer && boaros_lwip_ip_try_lock(&scope.ip_guard)) {
        int admit;
        {
            KERNEL_IRQ_LOCK_SCOPE(state, &socket_state_lock);
            admit = network_work_queue && !network_stopping && !socket->udp_pending &&
                udp4_begin_output_locked(request);
        }
        /* Only this transaction runs on the caller. Queued input or other
         * work published by output remains the network worker's responsibility. */
        if (admit) {
            COST_SCOPE(execution, NETWORK_PROTOCOL_EXEC_TICKS);
            result = datagram_send_owned(request, 1);
            udp4_end_output(request);
        }
    }
    kernel_socket_protocol_leave(&scope);
    return result;
}
#endif

static int protocol_request(struct kernel_socket_write_request *request,enum socket_protocol_operation operation)
{
    request->operation=operation;
    struct kernel_socket_protocol_scope protocol;
#if BOAROS_COST_DIAGNOSTICS
    int owner_locked = 0;
#endif
#if BOAROS_USER_SMP
    if (operation == SOCKET_PROTOCOL_DATAGRAM_SEND) {
        int result = datagram_try_caller(request);
        if (result != DATAGRAM_REQUIRES_WORKER) return result;
    }
    unsigned cpu=tcp_socket_cpu(request->socket);
    if (cpu) {
        struct socket_tcp_owner *owner=tcp_owner_at(cpu);
        if ((operation==SOCKET_PROTOCOL_STREAM || kernel_cpu_current()->logical_id==cpu) &&
            tcp_owner_lock(&protocol,owner,1)) {
#if BOAROS_COST_DIAGNOSTICS
            owner_locked = 1;
#endif
            int admit;
            { KERNEL_IRQ_LOCK_SCOPE(state,&socket_state_lock);admit=network_work_queue && !network_stopping &&
                !tcp_owner_stopping(owner) && tcp_socket_cpu(request->socket)==cpu &&
                !request->socket->writes_head &&
                !((request->flags & KERNEL_SOCKET_MSG_MORE) &&
                  (request->socket->work_flags & SOCKET_WORK_OUTPUT)); }
            if (admit) {
                if (operation == SOCKET_PROTOCOL_STREAM) COST_ADD(TCP_STREAM_INLINE, 1);
                int result;
                if (operation==SOCKET_PROTOCOL_STREAM && kernel_cpu_current()->logical_id!=cpu) {
                    /* The shard lock owns the COPY transaction, not the CPU.
                     * Queue only output work; no request or user pin escapes. */
                    stream_commit_owned(&request,1,1);
                    result=request->result;
                } else result=execute_protocol_request(request);
                request->segments=0;
                kernel_socket_protocol_leave(&protocol);
                return result;
            }
            kernel_socket_protocol_leave(&protocol);
        }
        goto queue;
    }
#endif
#if BOAROS_MAX_CPUS > 1
    if (operation==SOCKET_PROTOCOL_STREAM && kernel_cpu_current()->logical_id &&
        __atomic_load_n(&protocol_mutex_initialized,__ATOMIC_ACQUIRE)
#if BOAROS_USER_SMP
        && !__atomic_load_n(&tcp_owner_count,__ATOMIC_ACQUIRE)
#endif
        ) {
#if BOAROS_USER_SMP
        protocol=(struct kernel_socket_protocol_scope){0};
#endif
        if (protocol_lock(&protocol,1)) {
            int admit;
            /* 已排队的同流写入不能被越过。COPY 不进入网卡或输入路径；
             * 仅把需要的输出交给原 worker，发送者无须等它返还源码页。 */
            { KERNEL_IRQ_LOCK_SCOPE(state,&socket_state_lock);admit=network_work_queue && !network_stopping &&
                                        !request->socket->writes_head &&
                !((request->flags & KERNEL_SOCKET_MSG_MORE) &&
                  (request->socket->work_flags & SOCKET_WORK_OUTPUT)); }
            if (admit) {
                stream_commit_owned(&request,1,1);
                request->segments=0;
            }
            kernel_socket_protocol_leave(&protocol);
            if (admit) return request->result;
        } else arch_interrupt_restore(protocol.interrupts);
    }
#endif
    if(protocol_enter(&protocol,network_work_queue!=0,1)) {
#if BOAROS_USER_SMP
        if (tcp_socket_cpu(request->socket)) {
            kernel_socket_protocol_leave(&protocol);
            goto queue;
        }
#endif
        int result;
        result=network_is_stopping() && operation<SOCKET_PROTOCOL_DISPOSE?-KERNEL_EIO:0;
        if (!result && operation==SOCKET_PROTOCOL_STREAM &&
            (request->flags & KERNEL_SOCKET_MSG_MORE)) {
            /* A remote COPY can have returned before CPU0 emits it. Complete
             * that older output decision before installing this send's hint. */
            unsigned pending;
            { SOCKET_STATE(state);
              pending=request->socket->work_flags & (SOCKET_WORK_OUTPUT|SOCKET_WORK_PUSH);
              request->socket->work_flags &= ~pending; }
            if ((pending & SOCKET_WORK_PUSH) && request->socket->tcp)
                boaros_lwip_tcp_push(request->socket->tcp);
            if (pending & SOCKET_WORK_OUTPUT) tcp_flush_output(request->socket);
        }
#if BOAROS_USER_SMP
        int udp_output=!result && operation==SOCKET_PROTOCOL_DATAGRAM_SEND &&
            udp4_output_socket(request->socket) && boaros_lwip_parallel_enabled();
        if (udp_output) {
            int claimed;
            {
                KERNEL_IRQ_LOCK_SCOPE(state,&socket_state_lock);
                claimed=udp4_begin_output_locked(request);
            }
            if (!claimed) {
                kernel_socket_protocol_leave(&protocol);
                goto queue;
            }
        }
#endif
        if(!result)result=execute_protocol_request(request);
#if BOAROS_USER_SMP
        if (udp_output) udp4_end_output(request);
#endif
        request->segments=0;
        if(result>=0 && operation!=SOCKET_PROTOCOL_DATAGRAM_PREPARE &&
            operation!=SOCKET_PROTOCOL_DATAGRAM_CANCEL && operation!=SOCKET_PROTOCOL_DISPOSE &&
            !(request->progressed & (SOCKET_WRITE_DEFERRED|SOCKET_WRITE_LOOPBACK_INLINE)))
            service_inline(&protocol);
        kernel_socket_protocol_leave(&protocol);return result;
    }
#if BOAROS_USER_SMP
queue:;
#endif
    SOCKET_STATE(state);
    int stopped=network_stopping && operation<SOCKET_PROTOCOL_DISPOSE;
#if BOAROS_USER_SMP
    unsigned queued_cpu=tcp_socket_cpu(request->socket);
    if(queued_cpu && tcp_owner_at(queued_cpu)->stopping) stopped=1;
#endif
    if(!network_work_queue || stopped) {
        if(operation>=SOCKET_PROTOCOL_DISPOSE)__builtin_trap();
        request->segments=0;return -KERNEL_EIO;
    }
    struct kernel_socket *socket=request->socket;
    if(request->queued || request->next || !request->pin)__builtin_trap();
    COST_SCOPE(queued, NETWORK_PROTOCOL_QUEUE_TICKS);
    COST_ADD(NETWORK_PROTOCOL_QUEUED, 1);
#if BOAROS_COST_DIAGNOSTICS
    if (operation == SOCKET_PROTOCOL_STREAM)
        kernel_cost_add(owner_locked ? COST_TCP_STREAM_QUEUED_ORDER : COST_TCP_STREAM_QUEUED_BUSY, 1);
    else if (operation == SOCKET_PROTOCOL_FLUSH) COST_ADD(TCP_FLUSH_QUEUED, 1);
    request->queued_ticks=kernel_cost_clock();
#endif
    request->queued=1;
    if(socket->writes_tail)socket->writes_tail->next=request;else socket->writes_head=request;
    /* 入队只发布待服务请求；STREAM/FLUSH 按实际窗口与 IO_MORE 决定输出。 */
    socket->writes_tail=request;socket_schedule(socket,0);
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

#if BOAROS_USER_SMP
int kernel_socket_stream_has_shard(const struct kernel_socket *socket)
{ return tcp_socket_cpu(socket)!=0; }

int kernel_socket_stream_preparev(struct kernel_socket_write_request *request,
    const struct kernel_socket_write_segment *segments,unsigned count,uint32_t size)
{
    unsigned mss=request->tcp_prepare_mss;
    /* Keep short writes in the normal path, where they can extend a tail. */
    if (!mss || size<2*mss || size>UINT16_MAX) return 0;
    if (!segments || !count || size>request->reserved || request->tcp_prepare_size ||
        request->tcp_prepared || request->segments) __builtin_trap();
    request->tcp_prepare_size=(uint16_t)size;
#if BOAROS_COST_DIAGNOSTICS
    struct kernel_cost_tag preparing=kernel_cost_capture();
    uint64_t started=preparing.epoch?kernel_cost_clock():0;
#endif
    err_t error=boaros_lwip_tcp_preparev(segments,count,(u16_t)size,(u16_t)mss,
        &request->tcp_prepared);
#if BOAROS_COST_DIAGNOSTICS
    if (preparing.epoch)
        kernel_cost_add_tag(preparing,COST_NETWORK_PREPARE_TICKS,kernel_cost_clock()-started);
#endif
    if (error!=ERR_OK && error!=ERR_MEM) __builtin_trap();
    request->tcp_prepare_failed=error==ERR_MEM;
    return 1;
}

int kernel_socket_stream_publish(struct kernel_socket_write_request *request,uint32_t flags)
{
    if (!request->tcp_prepare_size) __builtin_trap();
    request->flags=flags;
    int result=protocol_request(request,SOCKET_PROTOCOL_STREAM);
    discard_tcp_prepared(request);
    return result;
}
#endif
int kernel_socket_stream_commit(struct kernel_socket_write_request *request,
    const void *buffer, uint32_t size, uint32_t flags)
{
    const struct kernel_socket_write_segment segment = {buffer, size};
    return kernel_socket_stream_commitv(request, &segment, 1, flags);
}

int kernel_socket_shutdown(struct kernel_socket *socket,int how)
{
    if(how<0 || how>2)return -KERNEL_EINVAL;
    if (socket->domain==KERNEL_SOCKET_DOMAIN_NETLINK || socket->domain==KERNEL_SOCKET_DOMAIN_PACKET)
        return -KERNEL_EOPNOTSUPP;
    if(socket->domain==KERNEL_SOCKET_DOMAIN_UNIX) {
        SOCKET_STATE(state);
        if(how!=1)socket->read_closed=1;
        if(how!=0)socket->write_closed=1;
        if(socket->peer && socket_connection_oriented(socket)) {
            if(how!=1)socket->peer->write_closed=1;
            if(how!=0)socket->peer->peer_closed=socket->peer->read_closed=1;
        }
        wake_socket(socket);
        if(socket->peer) {
            wake_socket(socket->peer);
            if (socket_connection_oriented(socket) && how!=0)
                socket_async(socket->peer,how==2?KERNEL_ASYNC_POLL_HUP:KERNEL_ASYNC_POLL_IN);
        }
        return 0;
    }
    struct kernel_socket_protocol_scope protocol;socket_protocol_enter(&protocol,socket);
    SOCKET_ENDPOINT(state,socket);
    int result=socket->connected?0:-KERNEL_ENOTCONN;
    int close_tx=how!=0 && !socket->write_closed;
    if(how!=1)socket->read_closed=1;
    if(close_tx) {
#if BOAROS_USER_SMP
        udp4_forget_path(socket);
#endif
        socket->write_closed=1;
        if(socket->peer && socket_connection_oriented(socket))socket->peer->peer_closed=1;
    }
    wake_socket(socket);if(socket->peer)wake_socket(socket->peer);
    socket_state_release(&state);
    if(close_tx && socket->tcp && socket->connected && !socket->listening) {
        boaros_lwip_tcp_push(socket->tcp);
        err_t error=tcp_shutdown(socket->tcp,0,1);
        if(error!=ERR_OK)result=lwip_error(error);
    }
    socket_schedule(socket,SOCKET_WORK_REAP|SOCKET_WORK_OUTPUT);
    service_inline(&protocol);kernel_socket_protocol_leave(&protocol);return result;
}

int kernel_socket_set_option(struct kernel_socket *socket,
                             enum kernel_socket_option option, int value)
{
    if (option==KERNEL_SOCKET_KEEPIDLE || option==KERNEL_SOCKET_KEEPINTVL ||
        option==KERNEL_SOCKET_KEEPCNT) {
        if (!kernel_socket_is_tcp(socket)) return -KERNEL_ENOPROTOOPT;
        if (value<1 || value>(option==KERNEL_SOCKET_KEEPCNT?127:32767)) return -KERNEL_EINVAL;
        struct kernel_socket_protocol_scope protocol;socket_protocol_enter(&protocol,socket);
        uint32_t converted=option==KERNEL_SOCKET_KEEPCNT?(uint32_t)value:(uint32_t)value*1000U;
        /* Listen PCBs are smaller than active PCBs. Retain policy on the endpoint
         * and install it on accepted children, never access an active-only field. */
        uint32_t *policy=option==KERNEL_SOCKET_KEEPIDLE?&socket->keep_idle:
            option==KERNEL_SOCKET_KEEPINTVL?&socket->keep_intvl:&socket->keep_cnt;
        { SOCKET_ENDPOINT(state,socket); *policy=converted; }
        if (socket->tcp && !socket->listening) {
            if(option==KERNEL_SOCKET_KEEPIDLE)socket->tcp->keep_idle=converted;
            else if(option==KERNEL_SOCKET_KEEPINTVL)socket->tcp->keep_intvl=converted;
            else socket->tcp->keep_cnt=converted;
        }
        /* lwIP checks elapsed idle time at its existing slow timer; no new
         * per-socket timer/worker, and no restart of the last-activity clock. */
        kernel_socket_protocol_leave(&protocol);return 0;
    }
    if (option == KERNEL_SOCKET_PASSCRED || option == KERNEL_SOCKET_OOBINLINE) {
        SOCKET_ENDPOINT(state,socket);
        if (option == KERNEL_SOCKET_PASSCRED) socket->passcred = value != 0;
        else socket->oobinline = value != 0;
        return 0;
    }
    int force = option==KERNEL_SOCKET_SNDBUFFORCE;
    int send = force || option==KERNEL_SOCKET_SNDBUF;
    if (send || option==KERNEL_SOCKET_RCVBUF) {
        if (force && !kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_NET_ADMIN))
            return -KERNEL_EPERM;
        struct kernel_socket_protocol_scope protocol;
        int tcp=kernel_socket_is_tcp(socket);
        if (tcp) socket_protocol_enter(&protocol,socket);
        uint32_t limit = force && value<0 ? 0U : (uint32_t)value;
        uint32_t minimum = send ? 4608U : 2304U;
        /* UNIX按Linux倍增/下限和4MiB配置上限；INET保留现有协议预算。 */
        uint32_t maximum=force ? INT32_MAX/2U :
            socket->domain!=KERNEL_SOCKET_DOMAIN_INET ? (4U<<20) : 32768U;
        if (!send && kernel_socket_is_udp(socket)) maximum=UDP_RECEIVE_LIMIT/2U;
        if(limit>maximum)limit=maximum;
        limit *= 2;
        if (limit < minimum) limit = minimum;
        SOCKET_ENDPOINT(state,socket);
        if (send) socket->tx_limit = limit;
        else {
            socket->rx_limit = limit;
            /* UNIX接收缓存值可查询，但不能替发送者归还在途内存。 */
        }
        wake_socket(socket);
        socket_state_release(&state);
        if (option==KERNEL_SOCKET_RCVBUF && tcp && socket->tcp)
            tcp_set_receive_limit(socket->tcp,limit);
        if (tcp) kernel_socket_protocol_leave(&protocol);
        return 0;
    }
    struct kernel_socket_protocol_scope saved;
    if(socket->domain==KERNEL_SOCKET_DOMAIN_INET)socket_protocol_enter(&saved,socket);
    int result = 0;
    struct ip_pcb *pcb = socket_ip_pcb(socket);
    switch (option) {
    case KERNEL_SOCKET_REUSEADDR:
    case KERNEL_SOCKET_KEEPALIVE: {
        uint8_t flag = option == KERNEL_SOCKET_REUSEADDR ? SOF_REUSEADDR : SOF_KEEPALIVE;
        { KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(socket));
          if (option == KERNEL_SOCKET_REUSEADDR) socket->reuseaddr = value != 0;
          else socket->keepalive = value != 0; }
        if (pcb != 0) { if (value) ip_set_option(pcb, flag); else ip_reset_option(pcb, flag); }
        break;
    }
    case KERNEL_SOCKET_BROADCAST:
        {SOCKET_ENDPOINT(state,socket);socket->broadcast=value!=0;}
        if(pcb) {if(value)ip_set_option(pcb,SOF_BROADCAST);else ip_reset_option(pcb,SOF_BROADCAST);}
        break;
    case KERNEL_SOCKET_IPV4_TTL:
        if(socket->domain!=KERNEL_SOCKET_DOMAIN_INET) {result=-KERNEL_ENOPROTOOPT;break;}
        if(value==-1)value=64;
        if(value<1 || value>255) {result=-KERNEL_EINVAL;break;}
#if BOAROS_USER_SMP
        udp4_forget_path(socket);
#endif
        {SOCKET_ENDPOINT(state,socket);socket->ipv4_ttl=(uint8_t)value;}
        if(pcb && IP_IS_V4(&pcb->local_ip))pcb->ttl=(u8_t)value;
        break;
    case KERNEL_SOCKET_IPV6_MULTICAST_HOPS:
    case KERNEL_SOCKET_IPV6_MULTICAST_LOOP:
    case KERNEL_SOCKET_IPV6_MULTICAST_ALL:
        if(socket->family!=KERNEL_SOCKET_AF_INET6) {result=-KERNEL_ENOPROTOOPT;break;}
        if(option==KERNEL_SOCKET_IPV6_MULTICAST_HOPS && socket->type==SOCKET_STREAM) {result=-KERNEL_ENOPROTOOPT;break;}
        if(option==KERNEL_SOCKET_IPV6_MULTICAST_HOPS && (value< -1 || value>255)) {result=-KERNEL_EINVAL;break;}
        if(option==KERNEL_SOCKET_IPV6_MULTICAST_LOOP && value!=0 && value!=1) {result=-KERNEL_EINVAL;break;}
        {SOCKET_ENDPOINT(state,socket);
          if(option==KERNEL_SOCKET_IPV6_MULTICAST_HOPS)socket->ipv6_multicast_hops=value<0?1:(uint8_t)value;
          else if(option==KERNEL_SOCKET_IPV6_MULTICAST_LOOP)socket->ipv6_multicast_loop=!!value;
          else socket->ipv6_multicast_all=!!value;}
        break;
    case KERNEL_SOCKET_MULTICAST_TTL:
        if(socket->domain!=KERNEL_SOCKET_DOMAIN_INET) {result=-KERNEL_ENOPROTOOPT;break;}
        if(socket->type==SOCKET_STREAM) {result=-KERNEL_EINVAL;break;}
        if(value==-1)value=1;
        if(value<0 || value>255) {result=-KERNEL_EINVAL;break;}
        {SOCKET_ENDPOINT(state,socket);socket->multicast_ttl=(uint8_t)value;}
        if(socket->raw)raw_set_multicast_ttl(socket->raw,(u8_t)value);
        if(socket->udp)udp_set_multicast_ttl(socket->udp,(u8_t)value);
        break;
    case KERNEL_SOCKET_IPV4_MULTICAST_LOOP:
    case KERNEL_SOCKET_IPV4_MULTICAST_ALL:
        if (socket->domain!=KERNEL_SOCKET_DOMAIN_INET) {result=-KERNEL_ENOPROTOOPT;break;}
        if (option==KERNEL_SOCKET_IPV4_MULTICAST_ALL && value!=0 && value!=1) {
            result=-KERNEL_EINVAL; break;
        }
        { SOCKET_ENDPOINT(state,socket);
          if(option==KERNEL_SOCKET_IPV4_MULTICAST_LOOP)socket->ipv4_multicast_loop=!!value;
          else socket->ipv4_multicast_all=!!value; }
        break;
    case KERNEL_SOCKET_CORK:
        if (!kernel_socket_is_tcp(socket)) { result = -KERNEL_ENOPROTOOPT; break; }
        { SOCKET_ENDPOINT(state,socket);socket->cork = value != 0; }
        if (socket->tcp && !socket->listening) {
            socket->tcp->boaros_cork = value != 0;
            if (!value) {
                socket->tcp->boaros_output_more = 0;
                boaros_lwip_tcp_cork_cancel(socket->tcp);
                /* Uncork alone restores normal Minshall. NODELAY additionally
                 * requests a push, including bytes blocked by the old window. */
                if (socket->nodelay) boaros_lwip_tcp_push(socket->tcp);
                tcp_flush_output(socket);
            }
        }
        break;
    case KERNEL_SOCKET_NODELAY:
        if (socket->domain != KERNEL_SOCKET_DOMAIN_INET || socket->type != SOCKET_STREAM) { result = -KERNEL_ENOPROTOOPT; break; }
        { KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(socket));socket->nodelay = value != 0; }
        if (socket->tcp != 0 && !socket->listening) {
            if (value) {
                tcp_nagle_disable(socket->tcp);
                boaros_lwip_tcp_push(socket->tcp);
                tcp_flush_output(socket);
            } else tcp_nagle_enable(socket->tcp);
        }
        break;
    case KERNEL_SOCKET_V6ONLY:
        if (socket->family != KERNEL_SOCKET_AF_INET6) { result = -KERNEL_ENOPROTOOPT; break; }
        if ((socket->udp != 0 && socket->udp->local_port != 0) ||
            (socket->tcp != 0 && socket->tcp->local_port != 0)) { result = -KERNEL_EINVAL; break; }
        { KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(socket));socket->v6only=value!=0; }
        if (pcb != 0) {
            ip_addr_set_zero_ip6(&pcb->local_ip);
            if (!value) IP_SET_TYPE(&pcb->local_ip, IPADDR_TYPE_ANY);
        }
        break;
    default: result = -KERNEL_ENOPROTOOPT;
    }
    if(socket->domain==KERNEL_SOCKET_DOMAIN_INET)kernel_socket_protocol_leave(&saved);
    return result;
}

/* Called only while the PCB's protocol owner is held. Queue-derived statistics
 * are sampled on query/retirement, never by walking queues on normal packets. */
static void tcp_info_snapshot(const struct tcp_pcb *pcb, struct kernel_socket_tcp_info *info)
{
    static const uint8_t states[]={7,10,2,3,1,4,5,8,9,11,6};
    *info=(struct kernel_socket_tcp_info){.pacing_rate=UINT64_MAX,.max_pacing_rate=UINT64_MAX};
    if((unsigned)pcb->state>=sizeof(states))__builtin_trap();
    info->state=states[pcb->state];
    if(pcb->state==LISTEN)return;
    info->ca_state=(pcb->flags&TF_RTO)?4:(pcb->flags&TF_INFR)?3:pcb->dupacks?1:0;
    info->retransmits=pcb->boaros_rto_retries;
    info->backoff=pcb->persist_backoff?(uint8_t)(pcb->persist_backoff-1):pcb->boaros_rto_backoff;
    info->probes=pcb->persist_backoff?pcb->persist_probe:pcb->keep_cnt_sent;
#if LWIP_TCP_TIMESTAMPS
    if(pcb->flags&TF_TIMESTAMP)info->options|=1;
#endif
#if LWIP_TCP_SACK_OUT
    if(pcb->flags&TF_SACK)info->options|=2;
#endif
#if LWIP_WND_SCALE
    if(pcb->flags&TF_WND_SCALE) {
        info->options|=4;info->scales=(uint8_t)(pcb->snd_scale|(pcb->rcv_scale<<4));
    }
#endif
    info->rto_us=(uint32_t)pcb->rto*TCP_SLOW_INTERVAL*1000U;
    info->ato_us=TCP_FAST_INTERVAL*1000U;
    info->snd_mss=tcp_mss(pcb);info->rcv_mss=pcb->boaros_rcv_mss;
    for(const struct tcp_seg *seg=pcb->unacked;seg;seg=seg->next) {
        info->unacked++;if(seg->flags&TF_SEG_RETRANSMITTED)info->retrans++;
        if(seg->flags&TF_SEG_LOST)info->lost++;
    }
    for(const struct tcp_seg *seg=pcb->unsent;seg;seg=seg->next) {
        info->notsent_bytes+=seg->len-seg->boaros_sent_bytes;
        if(seg->flags&TF_SEG_SENT) {
            info->unacked++;if(seg->flags&TF_SEG_LOST)info->lost++;
        }
    }
    info->last_data_sent=(tcp_ticks-pcb->boaros_last_sent)*TCP_SLOW_INTERVAL;
    info->last_data_recv=(tcp_ticks-pcb->boaros_last_recv)*TCP_SLOW_INTERVAL;
    info->last_ack_recv=(tcp_ticks-pcb->boaros_last_ack)*TCP_SLOW_INTERVAL;
    info->pmtu=pcb->boaros_pmtu;
    if(IP_IS_V6(&pcb->remote_ip)) {
        unsigned overhead=60;
#if LWIP_TCP_TIMESTAMPS
        if(pcb->flags&TF_TIMESTAMP)overhead+=12;
#endif
        if(info->pmtu>overhead && info->snd_mss>info->pmtu-overhead)
            info->snd_mss=info->pmtu-overhead;
    }
    info->rcv_ssthresh=info->rcv_space=pcb->boaros_rcv_capacity;
    info->rtt_us=(uint32_t)pcb->sa*TCP_SLOW_INTERVAL*1000U/8U;
    info->rttvar_us=(uint32_t)pcb->sv*TCP_SLOW_INTERVAL*1000U/4U;
    if(info->snd_mss) {
        info->snd_ssthresh=pcb->ssthresh/info->snd_mss;
        info->snd_cwnd=pcb->cwnd/info->snd_mss;
    }
    info->advmss=pcb->boaros_advmss;info->reordering=3;
    info->total_retrans=pcb->boaros_total_retrans;
    info->bytes_acked=pcb->boaros_bytes_acked;info->bytes_received=pcb->boaros_bytes_received;
    info->segs_in=pcb->boaros_segs_in;info->segs_out=pcb->boaros_segs_out;
    info->data_segs_in=pcb->boaros_data_in;info->data_segs_out=pcb->boaros_data_out;
    info->bytes_sent=pcb->boaros_bytes_sent;info->bytes_retrans=pcb->boaros_bytes_retrans;
    info->min_rtt_us=pcb->boaros_min_rtt;info->delivered=pcb->boaros_delivered;
    info->rcv_ooopack=pcb->boaros_ooopack;info->snd_wnd=pcb->snd_wnd;
    /* No rate/chronograph sampling, ECN/DSACK processing, or sender reorder
     * detection is enabled; their fields remain zero, not measured results. */
    /* No SACK scoreboard, FACK, or receive-side RTT estimator in this stack.
     * last_ack_sent is also unrecorded (Linux keeps this ABI field zero). */
}
static void socket_tcp_info_retire(struct tcp_pcb *pcb)
{
    if(pcb->errf!=tcp_failed || !pcb->callback_arg)return;
    struct kernel_socket *socket=pcb->callback_arg;
    tcp_info_snapshot(pcb,&socket->retired_info);
    socket->retired_info.state=7;
    socket->retired_info_ms=sys_now();
}
int kernel_socket_tcp_info(struct kernel_socket *socket, struct kernel_socket_tcp_info *info)
{
    if(!kernel_socket_is_tcp(socket))return -KERNEL_ENOPROTOOPT;
    _Static_assert(sizeof(*info)==232,"Linux TCP_INFO through snd_wnd");
    _Static_assert(offsetof(struct kernel_socket_tcp_info,total_retrans)==100,"Linux total_retrans offset");
    _Static_assert(offsetof(struct kernel_socket_tcp_info,snd_wnd)==228,"Linux snd_wnd offset");
    struct kernel_socket_protocol_scope protocol;socket_protocol_enter(&protocol,socket);
    if(socket->tcp) {
        tcp_info_snapshot(socket->tcp,info);
        if(socket->listening) {
            info->unacked=socket->pending_live;
            info->sacked=((struct tcp_pcb_listen *)socket->tcp)->backlog;
        }
    } else {
        *info=socket->retired_info;info->state=7;
        uint32_t elapsed=sys_now()-socket->retired_info_ms;
        info->last_data_sent+=elapsed;info->last_data_recv+=elapsed;info->last_ack_recv+=elapsed;
    }
    kernel_socket_protocol_leave(&protocol);return 0;
}

int kernel_socket_get_option(struct kernel_socket *socket,
                             enum kernel_socket_option option, int *value)
{
    if (option==KERNEL_SOCKET_KEEPIDLE || option==KERNEL_SOCKET_KEEPINTVL ||
        option==KERNEL_SOCKET_KEEPCNT) {
        if (!kernel_socket_is_tcp(socket)) return -KERNEL_ENOPROTOOPT;
        SOCKET_ENDPOINT(state,socket);
        uint32_t policy=option==KERNEL_SOCKET_KEEPIDLE?socket->keep_idle:
            option==KERNEL_SOCKET_KEEPINTVL?socket->keep_intvl:socket->keep_cnt;
        *value=(int)(option==KERNEL_SOCKET_KEEPCNT?policy:policy/1000U);
        return 0;
    }
    if (option==KERNEL_SOCKET_TYPE) { *value=socket->type;return 0; }
    if (option==KERNEL_SOCKET_MAXSEG) {
        if(!kernel_socket_is_tcp(socket))return -KERNEL_ENOPROTOOPT;
        struct kernel_socket_protocol_scope protocol;socket_protocol_enter(&protocol,socket);
        int result=socket->tcp?0:-KERNEL_ENOPROTOOPT;
        if(!result)*value=socket->listening?TCP_MSS:tcp_mss(socket->tcp);
        kernel_socket_protocol_leave(&protocol);return result;
    }
    SOCKET_ENDPOINT(state,socket);
    int result=0;
    switch (option) {
    case KERNEL_SOCKET_REUSEADDR: *value = socket->reuseaddr; break;
    case KERNEL_SOCKET_KEEPALIVE: *value = socket->keepalive; break;
    case KERNEL_SOCKET_BROADCAST: *value=socket->broadcast;break;
    case KERNEL_SOCKET_IPV4_TTL:
        if(socket->domain!=KERNEL_SOCKET_DOMAIN_INET)result=-KERNEL_ENOPROTOOPT;
        else *value=socket->ipv4_ttl;
        break;
    case KERNEL_SOCKET_IPV6_MULTICAST_HOPS:
    case KERNEL_SOCKET_IPV6_MULTICAST_LOOP:
    case KERNEL_SOCKET_IPV6_MULTICAST_ALL:
        if(socket->family!=KERNEL_SOCKET_AF_INET6)result=-KERNEL_ENOPROTOOPT;
        else *value=option==KERNEL_SOCKET_IPV6_MULTICAST_HOPS?socket->ipv6_multicast_hops:
            option==KERNEL_SOCKET_IPV6_MULTICAST_LOOP?socket->ipv6_multicast_loop:socket->ipv6_multicast_all;
        break;
    case KERNEL_SOCKET_MULTICAST_TTL:
        if(socket->domain!=KERNEL_SOCKET_DOMAIN_INET)result=-KERNEL_ENOPROTOOPT;
        else *value=socket->multicast_ttl;
        break;
    case KERNEL_SOCKET_IPV4_MULTICAST_LOOP:
    case KERNEL_SOCKET_IPV4_MULTICAST_ALL:
        if(socket->domain!=KERNEL_SOCKET_DOMAIN_INET)result=-KERNEL_ENOPROTOOPT;
        else *value=option==KERNEL_SOCKET_IPV4_MULTICAST_LOOP?
            socket->ipv4_multicast_loop:socket->ipv4_multicast_all;
        break;
    case KERNEL_SOCKET_SNDBUF: *value = (int)socket->tx_limit; break;
    case KERNEL_SOCKET_RCVBUF: *value = (int)socket->rx_limit; break;
    case KERNEL_SOCKET_PASSCRED: *value = socket->passcred; break;
    case KERNEL_SOCKET_OOBINLINE: *value = socket->oobinline; break;
    case KERNEL_SOCKET_ACCEPTCONN: *value = socket->listening; break;
    case KERNEL_SOCKET_ERROR: {
        *value = -socket->pending_error; socket->pending_error = 0;
        break;
    }
    case KERNEL_SOCKET_CORK:
        if (!kernel_socket_is_tcp(socket)) result = -KERNEL_ENOPROTOOPT;
        else *value = socket->cork;
        break;
    case KERNEL_SOCKET_NODELAY:
        if (socket->domain != KERNEL_SOCKET_DOMAIN_INET || socket->type != SOCKET_STREAM) result = -KERNEL_ENOPROTOOPT;
        else *value = socket->nodelay;
        break;
    case KERNEL_SOCKET_V6ONLY:
        if (socket->family != KERNEL_SOCKET_AF_INET6) result = -KERNEL_ENOPROTOOPT;
        else *value = socket->v6only;
        break;
    default: result = -KERNEL_ENOPROTOOPT;
    }
    return result;
}

int kernel_socket_set_bound_device(struct kernel_socket *socket,const char *name)
{
    struct kernel_socket_protocol_scope base;kernel_socket_protocol_enter(&base);
    struct netif *chosen=NULL;
    if(name[0]) {
        for(struct netif *n=netif_list;n;n=n->next) {
            int loop=n->name[0]=='l' && n->name[1]=='o';
            if(!__builtin_strcmp(name,loop?"lo":"eth0")) {chosen=n;break;}
        }
        if(!chosen) {kernel_socket_protocol_leave(&base);return -KERNEL_ENODEV;}
    }
#if BOAROS_USER_SMP
    struct kernel_socket_protocol_scope remote;
    unsigned cpu=tcp_socket_cpu(socket);
    if(cpu)tcp_control_enter(&base,&remote,cpu);
#endif
    int result=0;
    { SOCKET_ENDPOINT(state,socket);
      if(socket->bound_device &&
         !kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_NET_RAW))result=-KERNEL_EPERM;
      else {
#if BOAROS_USER_SMP
          udp4_forget_path(socket);
#endif
          socket->bound_device=chosen?netif_get_index(chosen):0;
      } }
    if(!result) {
        struct ip_pcb *pcb=socket_ip_pcb(socket);
#if BOAROS_USER_SMP
        /* The ingress route index reads netif_idx under this same short lock;
         * the PCB itself remains protected by its actual protocol owner. */
        KERNEL_RAW_SCOPE(route_guard,&tcp_route_lock);
#endif
        if(pcb)pcb->netif_idx=socket->bound_device;
    }
#if BOAROS_USER_SMP
    if(cpu)tcp_control_leave(&base,&remote);
#endif
    kernel_socket_protocol_leave(&base);return result;
}

int kernel_socket_get_bound_device(struct kernel_socket *socket,char name[16],uint32_t *length)
{
    unsigned index;
    {SOCKET_ENDPOINT(state,socket);index=socket->bound_device;}
    if(!index) {*length=0;return 0;}
    if(*length<16)return -KERNEL_EINVAL;
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    struct netif *interface=netif_get_by_index((u8_t)index);
    int result=-KERNEL_ENODEV;
    if(interface) {
        int loop=interface->name[0]=='l' && interface->name[1]=='o';
        *length=loop?3:5;__builtin_memcpy(name,loop?"lo":"eth0",*length);result=0;
    }
    kernel_socket_protocol_leave(&scope);return result;
}

static int ipv4_all_hosts(const ip4_addr_t *group)
{ return group->addr==PP_HTONL(0xe0000001U); }

static void ipv4_member_drop(struct kernel_socket *socket, struct socket_ipv4_member *member)
{
    struct netif *n=netif_get_by_index(member->index);
    if(n && !ipv4_all_hosts(&member->group)) (void)igmp_leavegroup_netif(n,&member->group);
    kernel_heap_release(socket->heap,member);
}

static void socket_ipv4_members_release(struct kernel_socket *socket)
{
    while(socket->ipv4_members) {
        struct socket_ipv4_member *member=socket->ipv4_members;
        socket->ipv4_members=member->next;
        ipv4_member_drop(socket,member);
    }
}

int kernel_socket_ipv4_membership(struct kernel_socket *socket,uint32_t group_address,
    uint32_t address,uint32_t index,int remove)
{
    if(socket->domain!=KERNEL_SOCKET_DOMAIN_INET)return -KERNEL_ENOPROTOOPT;
    ip4_addr_t group={group_address};
    if(!remove && !ip4_addr_ismulticast(&group))return -KERNEL_EINVAL;
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    struct netif *n=NULL;
    if(index) {
        if(index<=UINT8_MAX)n=netif_get_by_index((uint8_t)index);
    } else if(address) {
        ip4_addr_t local={address};
        for(struct netif *candidate=netif_list;candidate;candidate=candidate->next)
            if(address==netif_ip4_addr(candidate)->addr ||
                (ip4_addr_isloopback(&local) && ip4_addr_isloopback(netif_ip4_addr(candidate)))) {
                n=candidate;break;
            }
    } else n=ipv4_multicast_route(&group);
    if(n)index=netif_get_index(n);
    int result=0;
    if((!remove || (!index && !address)) && (!n || !(n->flags&NETIF_FLAG_IGMP))) {
        result=-KERNEL_ENODEV;goto out;
    }
    struct socket_ipv4_member **link=&socket->ipv4_members;
    unsigned count=0;
    for(;*link;link=&(*link)->next) {
        struct socket_ipv4_member *m=*link;
        count++;
        if(!ip4_addr_eq(&m->group,&group) || (index?index!=m->index:address!=m->address))continue;
        if(!remove) {result=-KERNEL_EADDRINUSE;goto out;}
        *link=m->next;
        ipv4_member_drop(socket,m);
        goto out;
    }
    if(remove) {result=-KERNEL_EADDRNOTAVAIL;goto out;}
    if(count==20) {result=-KERNEL_ENOBUFS;goto out;}
    struct igmp_group *existing=igmp_lookfor_group(n,&group);
    if(existing && existing->use==UINT8_MAX && !ipv4_all_hosts(&group)) {
        result=-KERNEL_ENOBUFS;goto out;
    }
    struct socket_ipv4_member *m=NULL;
    if(kernel_heap_allocate_zeroed(socket->heap,1,sizeof(*m),(void **)&m)!=KERNEL_HEAP_STATUS_OK) {
        result=-KERNEL_ENOBUFS;goto out;
    }
    err_t error=ipv4_all_hosts(&group)?ERR_OK:igmp_joingroup_netif(n,&group);
    if(error!=ERR_OK) {
        kernel_heap_release(socket->heap,m);
        result=error==ERR_MEM?-KERNEL_ENOBUFS:lwip_error(error);
        goto out;
    }
    m->group=group;m->address=address;m->index=(uint8_t)index;
    m->next=socket->ipv4_members;socket->ipv4_members=m;
out:
    kernel_socket_protocol_leave(&scope);return result;
}

static void socket_ipv6_members_release(struct kernel_socket *socket)
{
    while(socket->ipv6_members) {
        struct socket_ipv6_member *m=socket->ipv6_members;socket->ipv6_members=m->next;
        struct netif *n=netif_get_by_index(m->index);
        if(n && m->active)mld6_leavegroup_netif_force(n,&m->group);
        kernel_heap_release(socket->heap,m);
    }
}
static void socket_netif_removed(struct netif *n)
{
    /* lwIP owns teardown of the device groups. Remove socket subscriptions
     * before reuse of the numeric interface index. */
    for(struct kernel_socket *s=inet_sockets;s;s=s->inet_next) {
        struct socket_ipv4_member **ipv4=&s->ipv4_members;
        while(*ipv4) {
            struct socket_ipv4_member *m=*ipv4;
            if(m->index!=netif_get_index(n)){ipv4=&m->next;continue;}
            *ipv4=m->next;kernel_heap_release(s->heap,m);
        }
        struct socket_ipv6_member **link=&s->ipv6_members;
        while(*link) {
            struct socket_ipv6_member *m=*link;
            if(m->index!=netif_get_index(n)){link=&m->next;continue;}
            *link=m->next;kernel_heap_release(s->heap,m);
        }
    }
}
int kernel_socket_ipv6_membership(struct kernel_socket *socket,const uint8_t bytes[16],uint32_t index,int remove)
{
    if(socket->family!=KERNEL_SOCKET_AF_INET6)return -KERNEL_ENOPROTOOPT;
    if(socket->type!=SOCKET_DGRAM)return -KERNEL_EPROTO;
    ip6_addr_t group;__builtin_memcpy(group.addr,bytes,16);ip6_addr_clear_zone(&group);
    if(!ip6_addr_ismulticast(&group))return -KERNEL_EINVAL;
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    struct socket_ipv6_member **link=&socket->ipv6_members;unsigned count=0;int result=0;
    for(;*link;link=&(*link)->next) {
        struct socket_ipv6_member *m=*link;count++;
        if((!index || index==m->index) && ip6_addr_zoneless_eq(&group,&m->group)) {
            if(!remove){result=-KERNEL_EADDRINUSE;goto out;}
            struct netif *n=netif_get_by_index(m->index);
            err_t error=n && m->active?mld6_leavegroup_netif(n,&m->group):ERR_OK;
            if(error!=ERR_OK){result=lwip_error(error);goto out;}
            *link=m->next;kernel_heap_release(socket->heap,m);goto out;
        }
    }
    if(remove){result=-KERNEL_EADDRNOTAVAIL;goto out;}
    if(index>UINT8_MAX){result=-KERNEL_ENODEV;goto out;}
    struct netif *n=index?netif_get_by_index((uint8_t)index):ip6_route(IP6_ADDR_ANY6,&group);
    if(!n){result=-KERNEL_ENODEV;goto out;}
    if(n->hwaddr_len && n->mtu<1280){result=-KERNEL_ENODEV;goto out;}
    if(count>=32){result=-KERNEL_ENOBUFS;goto out;}
    ip6_addr_assign_zone(&group,IP6_MULTICAST,n);
    if(!mld6_lookfor_group(n,&group)) {
        unsigned groups=0;
        for(struct netif *p=netif_list;p;p=p->next)
            for(struct mld_group *g=netif_mld6_data(p);g;g=g->next)groups++;
        /* Leave room for solicited-node address groups, including concurrent
         * DAD. This is admission control, never an eviction of another owner. */
        if(groups>=48){result=-KERNEL_ENOBUFS;goto out;}
    }
    struct socket_ipv6_member *m=NULL;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(socket->heap,1,sizeof(*m),(void **)&m);
    if(status!=KERNEL_HEAP_STATUS_OK){result=-KERNEL_ENOBUFS;goto out;}
    err_t error=mld6_joingroup_netif(n,&group);
    if(error!=ERR_OK){kernel_heap_release(socket->heap,m);result=lwip_error(error);goto out;}
    m->group=group;m->index=netif_get_index(n);m->active=1;m->next=socket->ipv6_members;socket->ipv6_members=m;
out:
    kernel_socket_protocol_leave(&scope);return result;
}
int kernel_socket_set_ipv6_multicast_interface(struct kernel_socket *socket,uint32_t index)
{
    if(socket->family!=KERNEL_SOCKET_AF_INET6 || socket->type==SOCKET_STREAM)return -KERNEL_ENOPROTOOPT;
    if(index>UINT8_MAX)return -KERNEL_ENODEV;
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    int result=0;
    if(index && !netif_get_by_index((uint8_t)index))result=-KERNEL_ENODEV;
    else {SOCKET_ENDPOINT(state,socket);
      if(index && socket->bound_device && index!=socket->bound_device)result=-KERNEL_EINVAL;
      else socket->ipv6_multicast_index=(uint8_t)index;}
    kernel_socket_protocol_leave(&scope);return result;
}
int kernel_socket_get_ipv6_multicast_interface(struct kernel_socket *socket,uint32_t *index)
{
    if(socket->family!=KERNEL_SOCKET_AF_INET6)return -KERNEL_ENOPROTOOPT;
    SOCKET_ENDPOINT(state,socket);*index=socket->ipv6_multicast_index;return 0;
}

int kernel_socket_set_multicast_interface(struct kernel_socket *socket,uint32_t index,uint32_t address)
{
    if(socket->domain!=KERNEL_SOCKET_DOMAIN_INET)return -KERNEL_ENOPROTOOPT;
    if(socket->type==SOCKET_STREAM)return -KERNEL_EINVAL;
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    struct netif *interface=NULL;
    if(index && index<=255U)interface=netif_get_by_index((u8_t)index);
    else if(!index && address)for(struct netif *n=netif_list;n;n=n->next)
        if(netif_ip4_addr(n)->addr==address ||
           ((lwip_ntohl(address)>>24)==127 && n->name[0]=='l' && n->name[1]=='o')) {interface=n;break;}
    int result=0;
    if((index || address) && !interface)result=-KERNEL_EADDRNOTAVAIL;
    if(!result && interface && socket->bound_device && socket->bound_device!=netif_get_index(interface))result=-KERNEL_EINVAL;
    if(!result) {
        uint8_t selected=interface?netif_get_index(interface):0;
        {SOCKET_ENDPOINT(state,socket);socket->multicast_index=selected;
          ip_addr_set_zero_ip4(&socket->multicast_source);ip_2_ip4(&socket->multicast_source)->addr=address;}
        if(socket->raw)raw_set_multicast_netif_index(socket->raw,selected);
        if(socket->udp) {
            ip4_addr_t source={address};udp_set_multicast_netif_index(socket->udp,selected);
            udp_set_multicast_netif_addr(socket->udp,&source);
        }
    }
    kernel_socket_protocol_leave(&scope);return result;
}

int kernel_socket_get_multicast_interface(struct kernel_socket *socket,uint32_t *address)
{
    if(socket->domain!=KERNEL_SOCKET_DOMAIN_INET)return -KERNEL_ENOPROTOOPT;
    SOCKET_ENDPOINT(state,socket);*address=ip_2_ip4(&socket->multicast_source)->addr;return 0;
}
void kernel_socket_save_error(struct kernel_socket *socket, int error)
{
    SOCKET_ENDPOINT(scope,socket);
    socket->pending_error=error;
    socket_state_release(&scope);
}
void kernel_socket_set_send_timeout(struct kernel_socket *socket, uint64_t nanoseconds)
{
    SOCKET_ENDPOINT(scope,socket);
    socket->send_timeout_ns = nanoseconds;
    socket_state_release(&scope);
}
uint64_t kernel_socket_send_timeout(struct kernel_socket *socket)
{
    SOCKET_ENDPOINT(scope,socket);
    uint64_t timeout = socket->send_timeout_ns;
    socket_state_release(&scope);
    return timeout;
}

void kernel_socket_set_receive_timeout(struct kernel_socket *socket,
                                       uint64_t nanoseconds)
{
    SOCKET_ENDPOINT(scope,socket);
    socket->receive_timeout_ns = nanoseconds;
    socket_state_release(&scope);
}

uint64_t kernel_socket_receive_timeout(struct kernel_socket *socket)
{
    SOCKET_ENDPOINT(scope,socket);
    uint64_t timeout = socket->receive_timeout_ns;
    socket_state_release(&scope);
    return timeout;
}

uint32_t kernel_socket_poll(struct kernel_socket *socket,
                            struct kernel_wait_queue **queue)
{
    uint32_t events = 0;
    SOCKET_ENDPOINT(snapshot_irq,socket);
    COST_ADD(NETWORK_POLL_CALLS, 1);
#if BOAROS_COST_DIAGNOSTICS
    unsigned *polling=socket_poll_depth();
    (*polling)++;
#endif
    if (queue != 0) *queue = &socket->wait;

    if (socket->domain == KERNEL_SOCKET_DOMAIN_UNIX) {
        if(socket->pending_error)events|=KERNEL_POLLERR;
        if(socket_connection_oriented(socket)) {
            if((socket->packets_head || socket->read_closed || socket->pending_live) && !socket->read_request)
                events|=KERNEL_POLLIN|KERNEL_POLLRDNORM;
            if(socket->read_closed)events|=KERNEL_POLLRDHUP;
            if((socket->read_closed && socket->write_closed) || (!socket->connected && !socket->listening))
                events|=KERNEL_POLLHUP;
            if(!socket->listening && unix_writable(socket))events|=KERNEL_POLLOUT|KERNEL_POLLWRNORM|KERNEL_POLLWRBAND;
        } else {
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
        }
#if BOAROS_COST_DIAGNOSTICS
        (*polling)--;
#endif
        socket_state_release(&snapshot_irq);
        return events;
    }

    if (kernel_socket_preserves_messages(socket)) {
        if ((socket->packets_head != 0 || socket->read_closed) && socket->read_request == 0)
            events |= KERNEL_POLLIN | KERNEL_POLLRDNORM;
        if (socket->packet_options && socket->packet_options->ring &&
            packet_ring_poll(socket->packet_options->ring))
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
        if (tcp_admission_capacity_locked(socket))
            events |= KERNEL_POLLOUT | KERNEL_POLLWRNORM;
    } else if (socket->peer_closed) {
        events |= KERNEL_POLLHUP | KERNEL_POLLOUT | KERNEL_POLLRDHUP | KERNEL_POLLIN;
    } else if (socket_receive_empty(socket, 0) == -KERNEL_ENOTCONN) {
        events |= KERNEL_POLLHUP | KERNEL_POLLOUT | KERNEL_POLLWRNORM;
    }
    if (socket->pending_error || network_is_stopping()) events |= KERNEL_POLLERR;
#if BOAROS_COST_DIAGNOSTICS
    (*polling)--;
#endif
    socket_state_release(&snapshot_irq);
    return events;
}

struct kernel_wait_queue *kernel_socket_wait_queue(struct kernel_socket *socket)
{
    return &socket->wait;
}
struct kernel_wait_queue *kernel_socket_async_source(struct kernel_socket *socket)
{ return &socket->async_events; }

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

#if BOAROS_USER_SMP
static void fast_loopback_revoke(struct netif *netif)
{
    if (netif!=fast_loopback) return;
    uint64_t state=__atomic_load_n(&fast_loopback_state,__ATOMIC_RELAXED);
    if (state>UINT64_MAX-LOOPBACK_GENERATION) __builtin_trap();
    __atomic_store_n(&fast_loopback_state,(state+LOOPBACK_GENERATION)&~UINT64_C(1),__ATOMIC_RELEASE);
    /* A direct publisher may already have sampled the old configuration.
     * Drain its publication before changing the interface outside this lock. */
    unsigned count=__atomic_load_n(&tcp_owner_count,__ATOMIC_ACQUIRE);
    for (unsigned cpu=1;cpu<=count;cpu++) {
        KERNEL_RAW_SCOPE(ingress,&tcp_owner_at(cpu)->ingress);
    }
}

static void fast_loopback_refresh(struct netif *netif)
{
    if (!netif || netif!=fast_loopback) return;
    unsigned available=netif_is_up(netif) && netif_is_link_up(netif) && !netif->mtu &&
        netif_ip4_addr(netif)->addr==lwip_htonl(UINT32_C(0x7f000001)) &&
        netif_ip4_netmask(netif)->addr==lwip_htonl(UINT32_C(0xff000000)) &&
        ip6_addr_isloopback(netif_ip6_addr(netif,0)) && ip6_addr_isvalid(netif_ip6_addr_state(netif,0));
    uint64_t state=__atomic_load_n(&fast_loopback_state,__ATOMIC_RELAXED);
    __atomic_store_n(&fast_loopback_state,(state&~UINT64_C(1))|available,__ATOMIC_RELEASE);
}
#endif

static void ipv6_withdraw(struct netif *);
static void socket_set_interface_up(struct netif *netif,int up)
{
    if (!!netif_is_up(netif)==!!up) return;
#if BOAROS_USER_SMP
    fast_loopback_revoke(netif);
#endif
    if (netif->hwaddr_len) {
        if(!up) {
            ipv6_withdraw(netif);
            __builtin_memset(netif->ip6_configured,0,sizeof(netif->ip6_configured));
            ip6_addr_set_zero(&netif->ip6_gateway);
        }
        if (!up) {
            nd6_cleanup_netif(netif);
            netif_ip6_addr_set_state(netif,0,IP6_ADDR_INVALID);
        } else if (netif->mtu>=1280 && ip6_addr_isinvalid(netif_ip6_addr_state(netif,0))) {
            netif_create_ip6_linklocal_address(netif,1);
        }
    }
    if (up) netif_set_up(netif); else netif_set_down(netif);
#if BOAROS_USER_SMP
    fast_loopback_refresh(netif);
#endif
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
    socket_set_interface_up(loopback,flags & UINT16_C(0x1));
    kernel_socket_protocol_leave(&scope);
    return 0;
}

void kernel_socket_network_initialize(void)
{
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    if (!socket_initialized) {
        const struct boaros_lwip_hooks hooks = {
            .tcp_output = socket_tcp_output, .work = protocol_work_ready, .capacity = protocol_capacity,
            .timewait_free = protocol_timewait_free, .tcp_info_retire = socket_tcp_info_retire,
            .udp_ingress_filter = socket_udp_ingress_filter, .tcp_ingress_filter = socket_tcp_ingress_filter,
            .udp_multicast_receive=socket_udp_multicast_receive,.netif_removed=socket_netif_removed,
#if BOAROS_USER_SMP
            .tcp_context = tcp_context, .tcp_dispatch = tcp_dispatch,
            .pcb_forget = tcp_pcb_forget, .pcb_timewait = tcp_pcb_timewait,
            .reclaim_remote_timewait = tcp_reclaim_remote_timewait,
            .tcp_conflict = tcp_route_conflict,
            .tcp_local_output = tcp_local_output, .loopback_publish = tcp_mark_legacy_loopback,
#endif
        };
        boaros_lwip_set_hooks(&hooks);
        lwip_init();
#if BOAROS_USER_SMP
        if (boaros_lwip_parallel_enabled()) {
            fast_loopback=loopback_netif();
            fast_loopback_refresh(fast_loopback);
        }
#endif
        socket_initialized = 1;
    }
    kernel_socket_protocol_leave(&scope);
}
void kernel_socket_network_hooks(void (*timer_wake)(void *),struct kernel_wait_queue *work_queue,
    int (*udp_capacity)(void *),unsigned (*receive_inline)(void *),void *context)
{
    struct kernel_socket_protocol_scope protocol;kernel_socket_protocol_enter(&protocol);
    {SOCKET_STATE(state);__atomic_store_n(&network_stopping,work_queue==0,__ATOMIC_RELAXED);}
    if (!work_queue) for (struct kernel_socket *socket=inet_sockets; socket; socket=socket->inet_next) {
        SOCKET_ENDPOINT(state,socket);
        /* Empty readers and control waiters have no active payload request to drain. */
        if (!socket->pending_error) socket->pending_error=-KERNEL_EIO;
        wake_socket(socket);
        socket_async(socket,KERNEL_ASYNC_POLL_ERR);
    }
    if(!work_queue)for(;;) {
        struct kernel_open_file_description *pin=0;
        struct kernel_wait_token token={0};uintptr_t interrupts=0;
        for(struct kernel_socket *socket=inet_sockets;socket;socket=socket->inet_next) {
#if BOAROS_USER_SMP
            struct kernel_socket_protocol_scope remote;
            unsigned cpu=tcp_socket_cpu(socket);
            if (cpu) tcp_control_enter(&protocol,&remote,cpu);
#endif
            retire_received(socket);
#if BOAROS_USER_SMP
            if (cpu) tcp_control_leave(&protocol,&remote);
#endif
            SOCKET_ENDPOINT(state,socket);
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
     network_udp_capacity=udp_capacity;network_receive_inline=receive_inline;network_context=context;}
    kernel_socket_protocol_leave(&protocol);
    if (!work_queue && old_queue) for (;;) {
        int borrowed;
        {KERNEL_IRQ_LOCK_SCOPE(guard,&old_queue->lock);borrowed=old_queue->borrows!=0;}
        if(!borrowed)break;
        /* Hooks no longer admit cursors; their real queue borrows cover lock-free delivery. */
        kernel_wait_backend_quiesce();
    }
}
void kernel_socket_network_capacity(void)
{
    SOCKET_STATE(state);
    __atomic_store_n(&nic_generation,nic_generation+1,__ATOMIC_RELAXED);
    orphan_nic_ready |= orphan_nic_wait;
    orphan_nic_notify_locked(state.owner,orphan_nic_ready | (nic_wait.head != NULL));
}
void kernel_socket_network_blocked(void)
{
#if BOAROS_USER_SMP
    struct socket_tcp_owner *owner=current_tcp_owner();
    if (owner) { owner->nic_blocked=1; return; }
    /* A resolved UDP send reports a full queue to its own caller. */
    if (protocol_current_owner()!=kernel_io_context_current()) return;
#endif
    nic_blocked=1;
}
static int capacity_ready(const struct socket_work_queue *queue, uint64_t generation)
{ return queue->head && queue->head->work_links[queue->slot].epoch != generation; }

int kernel_socket_work_pending(void)
{
    if (protocol_current_owner() != kernel_io_context_current() || !protocol_depth) __builtin_trap();
    if (!socket_initialized) return 0;
    int timer_ready=sys_timeouts_sleeptime()==0 || boaros_lwip_tcp_cork_ready() ||
        (packet_timer_head && (int32_t)(sys_now()-packet_timer_head->packet_options->deadline)>=0);
    SOCKET_STATE(state);
    int runnable = rights_dirty || (rights_retry_pending && (int32_t)(sys_now()-rights_retry_ms)>=0) || ready_work.head || loopback_pending || (orphan_nic_ready & 1U) ||
        capacity_ready(&pool_wait,__atomic_load_n(&pool_generation,__ATOMIC_RELAXED)) || capacity_ready(&nic_wait, nic_generation) ||
        capacity_ready(&receive_wait, receive_generation) || timer_ready ||
        (write_retry_head && (int32_t)(sys_now() - write_retry_head->write_retry_ms) >= 0);
    return runnable;
}

static unsigned service_orphan_nic(unsigned cpu)
{
    unsigned mask = 1U << cpu;
    {
        SOCKET_STATE(state);
        if (!(orphan_nic_ready & mask)) return 0;
        orphan_nic_ready &= ~mask;
        orphan_nic_wait &= ~mask;
    }
    /* The caller holds this context's protocol lock. The PCB pool bounds
     * this pass. No PCB is borrowed by a different context or a work queue. */
    for (struct tcp_pcb *pcb=tcp_active_pcbs;pcb;pcb=pcb->next)
        if (pcb->boaros_nic_retry) socket_tcp_output(pcb);
    return 1;
}

static unsigned service_ready_socket(unsigned budget,struct socket_work_queue *queue,
                                     struct kernel_socket_protocol_scope *scope)
{
    struct kernel_socket_write_request *requests[SOCKET_WRITE_BATCH];
    unsigned count=0;
    KERNEL_IRQ_LOCK_SCOPE(irq,&socket_state_lock);
    if (!queue->head) { kernel_irq_lock_release(&irq); return 0; }
    struct kernel_socket *socket = queue->head;
#if BOAROS_USER_SMP
    if (socket->udp_output_active) {
        work_remove_locked(queue,socket);
        kernel_irq_lock_release(&irq);
        return 1;
    }
    if (queue==&ready_work && (socket->work_flags&SOCKET_WORK_PROMOTE)) {
        socket->work_flags &= ~SOCKET_WORK_PROMOTE;
        kernel_irq_lock_release(&irq);
        if (tcp_promote(socket,scope)) return 1;
        kernel_irq_lock_acquire(&socket_state_lock,&irq);
    }
#else
    (void)scope;
#endif
    unsigned flags = socket->work_flags; socket->work_flags = 0;
    work_remove_locked(queue, socket);
    int stopping=network_stopping;
    while(count<budget && count<SOCKET_WRITE_BATCH && socket->writes_head) {
        struct kernel_socket_write_request *request=socket->writes_head;
        if(count && (stopping || request->operation!=SOCKET_PROTOCOL_STREAM ||
                     (request->flags & (KERNEL_SOCKET_IO_MORE|KERNEL_SOCKET_MSG_MORE))))break;
        socket->writes_head=request->next;
        request->next=0;
        requests[count++]=request;
        /* IO_MORE 的前缀不随其他请求的正常末批提前输出。 */
        if(request->operation!=SOCKET_PROTOCOL_STREAM || (request->flags & (KERNEL_SOCKET_IO_MORE|KERNEL_SOCKET_MSG_MORE)))break;
    }
    /* 就绪链在同一保护内摘除并重新入队，已有的工作通知仍覆盖余项。 */
#if BOAROS_USER_SMP
    int udp_output=count && requests[0]->operation==SOCKET_PROTOCOL_DATAGRAM_SEND &&
        udp4_output_socket(socket) && boaros_lwip_parallel_enabled();
    if (udp_output) {
        socket->udp_output_active=1;
        requests[0]->udp_output_owned=1;
    }
    if (socket->writes_head && !udp_output) work_add_locked(queue,socket,0);
#else
    if (socket->writes_head) work_add_locked(queue,socket,0);
#endif
    if (!socket->writes_head) socket->writes_tail=0;
    kernel_irq_lock_release(&irq);
#if BOAROS_USER_SMP
    if ((flags&SOCKET_WORK_DEVICE_FAILED) && socket->tcp) {
        { KERNEL_IRQ_LOCK_SCOPE(state,socket_endpoint_lock(socket));socket->pending_error=-KERNEL_EIO; }
        tcp_abort(socket->tcp);
    }
#endif
    if (flags & SOCKET_WORK_REAP) retire_received(socket);
    if ((flags & SOCKET_WORK_LISTENER) && socket->listening) purge_dead_pending(socket);
    if ((flags & SOCKET_WORK_RECEIVE) && socket->receive_retry && socket->tcp && socket->tcp->refused_data)
        (void)tcp_process_refused_data(socket->tcp);
    if (flags) retire_timewait(socket);
    /* 已捕获的OUTPUT属于此前输入/重试；先完成它，再接纳新COPY，
     * 避免旧标记把新请求仍可合批的IO_MORE前缀提前送入协议。 */
    if ((flags & SOCKET_WORK_PUSH) && socket->tcp && !socket->listening)
        boaros_lwip_tcp_push(socket->tcp);
    if ((flags & SOCKET_WORK_OUTPUT) && !socket->listening)tcp_flush_output(socket);
    if(count && requests[0]->operation==SOCKET_PROTOCOL_STREAM && !stopping) {
        stream_commit_owned(requests,count,0);
        return count;
    }
    struct kernel_socket_write_request *request=count?requests[0]:NULL;
    if(request) {
#if BOAROS_COST_DIAGNOSTICS
        request->executing_ticks=kernel_cost_clock();
#endif
        request->result=stopping && request->operation<SOCKET_PROTOCOL_DISPOSE?
            -KERNEL_EIO:execute_protocol_request(request);
    }
    if((flags || request) && kernel_socket_is_tcp(socket)) {
        SOCKET_ENDPOINT(state,socket);
        uint32_t previous=tcp_admission_capacity_locked(socket);
        tcp_publish_capacity_locked(socket);
        if(tcp_admission_capacity_locked(socket)>previous) {
            socket_state_notify(state.owner,&socket->wait);socket_async(socket,KERNEL_ASYNC_POLL_OUT);
        }
    }
    if (request) {
        SOCKET_STATE(queue_state);
#if BOAROS_USER_SMP
        if (udp_output) udp4_end_output_locked(request,queue_state.owner);
#endif
        complete_protocol_request_locked(request);
    }
    return 1;
}
#if BOAROS_USER_SMP
static unsigned tcp_owner_take_input(struct socket_tcp_owner *owner,
                                     struct socket_tcp_input *input,unsigned limit)
{
    KERNEL_RAW_SCOPE(ingress,&owner->ingress);
    unsigned count=owner->count<limit?owner->count:limit;
    if (!count) return 0;
    /* Copy the claimed entries before returning their slots to producers. */
    for (unsigned i=0;i<count;i++) input[i]=owner->ring[(owner->head+i)%TCP_RX_RING];
    owner->head=(owner->head+count)%TCP_RX_RING;
    __atomic_store_n(&owner->count,owner->count-count,__ATOMIC_RELAXED);
    return count;
}

static unsigned tcp_owner_receive_batch(struct socket_tcp_owner *owner,unsigned limit,int stopping
#if BOAROS_COST_DIAGNOSTICS
                                        ,uint64_t locking,uint64_t locked
#endif
                                       )
{
    struct socket_tcp_input input[TCP_RX_BATCH];
    unsigned count=tcp_owner_take_input(owner,input,limit);
    struct netif *ack_interface=NULL;
    /* The connection owner already holds this prefix. Do not wait for
     * subsequent input or retain the interface beyond this protocol turn. */
    if (count>1 && !stopping) owner->context.input_ack_batch=&ack_interface;
    for (unsigned i=0;i<count;i++) {
#if BOAROS_COST_DIAGNOSTICS
        kernel_cost_tcp_input_dequeue(input[i].queued_ticks,locking,locked);
#endif
        if (!stopping) tcp_owner_receive(owner,&input[i]);
        else pbuf_free(input[i].packet);
    }
    owner->context.input_ack_batch=NULL;
    if (ack_interface) ack_interface->boaros_link_flush(ack_interface);
    return count;
}

static void tcp_owner_receive_before_wait(struct socket_tcp_owner *owner)
{
    if (caller_owns_protocol()) return;
    struct kernel_socket_protocol_scope scope;
#if BOAROS_COST_DIAGNOSTICS
    uint64_t locking=kernel_cost_clock();
#endif
    if (!tcp_owner_lock(&scope,owner,1)) return;
    struct kernel_wait_cursor wake={0};
    if (!tcp_owner_stopping(owner) && !network_is_stopping()) {
        /* Process one FIFO prefix under the shard lock. The worker retains
         * responsibility for the suffix, output requests and timers. */
        struct kernel_socket *ack_socket=NULL;
        owner->ack_waiter=&ack_socket;
        unsigned count=tcp_owner_receive_batch(owner,TCP_RX_BATCH,0
#if BOAROS_COST_DIAGNOSTICS
                                               ,locking,kernel_cost_clock()
#endif
                                              );
        owner->ack_waiter=NULL;
        if (count) COST_ADD(TCP_INPUT_WAIT_ASSIST,count);
        tcp_prepare_ack_wake(ack_socket,&wake);
    }
    kernel_socket_protocol_leave(&scope);
    tcp_wake_ack_waiters(&wake);
}

static uint64_t tcp_owner_deadline(void)
{
    uint32_t now_ms=sys_now();
    uint32_t delay=boaros_lwip_tcp_timers_delay(now_ms);
    uint32_t cork_delay=boaros_lwip_tcp_cork_delay(now_ms);
    if(cork_delay<delay)delay=cork_delay;
    uint64_t deadline=0;
    if (delay==UINT32_MAX) return 0;
    if (!delay) delay=1;
    uint64_t now=kernel_time_monotonic_ns(), ns=(uint64_t)delay*1000000;
    if (kernel_time_deadline_from_monotonic(now>UINT64_MAX-ns?UINT64_MAX:now+ns,&deadline)
        !=KERNEL_TIME_STATUS_OK) return arch_time_read()+1;
    return deadline;
}

static void tcp_owner_worker(void *arg)
{
    struct socket_tcp_owner *owner=arg;
    if (kernel_cpu_current()->logical_id!=owner->index) __builtin_trap();
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_current()->wait_flags |= KERNEL_COST_TCP_WORKER;
#endif
    for (;;) {
        struct kernel_socket_protocol_scope scope;
#if BOAROS_COST_DIAGNOSTICS
        uint64_t locking = kernel_cost_clock();
#endif
        (void)tcp_owner_lock(&scope,owner,0);
#if BOAROS_COST_DIAGNOSTICS
        uint64_t locked = kernel_cost_clock();
#endif
        int stopping;
        __atomic_store_n(&owner->waiting,0,__ATOMIC_RELAXED);
        stopping=tcp_owner_stopping(owner);
        struct kernel_socket *ack_socket = NULL;
        owner->ack_waiter = &ack_socket;
        for (unsigned remaining=16;remaining;) {
            unsigned limit=remaining<TCP_RX_BATCH?remaining:TCP_RX_BATCH;
            unsigned count=tcp_owner_receive_batch(owner,limit,stopping
#if BOAROS_COST_DIAGNOSTICS
                                                   ,locking,locked
#endif
                                                  );
            if (!count) break;
            remaining-=count;
        }
        owner->ack_waiter = NULL;
        struct kernel_wait_cursor ack_wake = {0};
        /* Include any consumer that registered since earlier ACKs in the batch. */
        tcp_prepare_ack_wake(ack_socket, &ack_wake);
        if (!stopping) service_orphan_nic(owner->index);
        for (unsigned i=0;i<8 && service_ready_socket(4,&owner->ready,&scope);i++) {}
        if (!stopping) {
            owner->timer_running=1;
            (void)boaros_lwip_tcp_cork_due(8);
            (void)boaros_lwip_tcp_timers_due(sys_now());
            owner->timer_running=0;
        }
        struct kernel_wait_token token={0};
        uint64_t deadline=tcp_owner_deadline();
        int done=0;
        {
            SOCKET_STATE(state);
            KERNEL_RAW_SCOPE(ingress,&owner->ingress);
            if (!owner->count && !owner->ready.head && !(orphan_nic_ready & (1U<<owner->index))) {
                done=tcp_owner_stopping(owner);
                if (!done) {
                    if (kernel_wait_prepare(&owner->work,deadline,0,&token)) __builtin_trap();
                    __atomic_store_n(&owner->waiting,1,__ATOMIC_RELAXED);
                }
            }
        }
        kernel_socket_protocol_leave(&scope);
        tcp_wake_ack_waiters(&ack_wake);
        if (done) return;
        if (token.task) {
            enum kernel_wait_wake_reason reason;
            if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
        } else {
            uintptr_t irq=arch_interrupt_save();
            if (kernel_scheduler_yield_current()) __builtin_trap();
            arch_interrupt_restore(irq);
        }
    }
}

static void tcp_owner_close_admission(struct socket_tcp_owner *owner)
{
    /* Queue stop and ready-list admission must still be ordered together. */
    SOCKET_STATE(state);
    KERNEL_RAW_SCOPE(ingress,&owner->ingress);
    __atomic_store_n(&owner->stopping,1,__ATOMIC_RELAXED);
    orphan_nic_wait &= ~(1U<<owner->index);
    orphan_nic_ready &= ~(1U<<owner->index);
}

static void tcp_owner_stop(struct socket_tcp_owner *owner)
{
    if (kernel_wait_queue_close(&owner->work)) __builtin_trap();
    kernel_thread_join(&owner->worker);
    for (;;) {
        enum kernel_scheduler_status status=kernel_wait_queue_destroy(&owner->work);
        if (status==KERNEL_SCHEDULER_STATUS_OK) break;
        if (status!=KERNEL_SCHEDULER_STATUS_BUSY) __builtin_trap();
        kernel_wait_backend_quiesce();
    }
}

int kernel_socket_start_tcp_owners(void)
{
    unsigned cpus=kernel_cpu_get_boot_count();
    if (cpus<2) return 0;
    if (tcp_owner_count) return -KERNEL_EBUSY;
    for (unsigned cpu=1;cpu<cpus;cpu++) {
        struct socket_tcp_owner *owner=tcp_owner_at(cpu);
        owner->index=cpu;
        kernel_raw_lock_init(&owner->ingress,KERNEL_RAW_RANK_OBJECT+1);
        kernel_wait_queue_init(&owner->work);
        kernel_mutex_init(&owner->mutex,26,(uintptr_t)&owner->mutex);
        boaros_lwip_tcp_context_init(&owner->context);
        if (kernel_thread_create_joinable_worker(tcp_owner_worker,owner,&owner->worker,cpu)) {
            if (kernel_wait_queue_close(&owner->work) || kernel_wait_queue_destroy(&owner->work)) __builtin_trap();
            for (unsigned i=1;i<cpu;i++) {
                struct socket_tcp_owner *started=tcp_owner_at(i);
                tcp_owner_close_admission(started);
                tcp_owner_stop(started);
            }
            return -KERNEL_ENOMEM;
        }
    }
    __atomic_store_n(&tcp_owner_count,cpus-1,__ATOMIC_RELEASE);
    return 0;
}

void kernel_socket_stop_tcp_owners(void)
{
    unsigned count=__atomic_exchange_n(&tcp_owner_count,0,__ATOMIC_ACQ_REL);
    for (unsigned i=1;i<=count;i++) tcp_owner_close_admission(tcp_owner_at(i));
    for (unsigned i=1;i<=count;i++) {
        struct socket_tcp_owner *owner=tcp_owner_at(i);
        tcp_owner_stop(owner);
        struct kernel_socket_protocol_scope scope;
        (void)tcp_owner_lock(&scope,owner,0);
        struct tcp_pcb *lists[]={tcp_active_pcbs,tcp_bound_pcbs,tcp_tw_pcbs};
        for (unsigned n=0;n<sizeof(lists)/sizeof(*lists);n++)
            for (struct tcp_pcb *pcb=lists[n],*next;pcb;pcb=next) {
                next=pcb->next;
                if (!pcb->callback_arg && !pcb->errf) tcp_abandon(pcb,0);
            }
        kernel_socket_protocol_leave(&scope);
    }
}
#endif
static int service_capacity(void)
{
    SOCKET_STATE(state);
    if (orphan_nic_ready & 1U) {
        socket_state_release(&state);
        return service_orphan_nic(0);
    }
    struct socket_work_queue *queues[] = {&pool_wait, &nic_wait, &receive_wait};
    uint64_t generations[] = {__atomic_load_n(&pool_generation,__ATOMIC_RELAXED),nic_generation,receive_generation};
    for (unsigned i = 0; i < 3; i++) {
        unsigned index = capacity_cursor;
        capacity_cursor = (capacity_cursor + 1) % 3;
        struct socket_work_queue *queue = queues[index];
        if (!capacity_ready(queue, generations[index])) continue;
        struct kernel_socket *socket = queue->head;
        work_remove_locked(queue, socket);
        if (queue == &nic_wait) COST_ADD(TCP_NIC_RETRY, 1);
        if (queue == &pool_wait) write_retry_disarm(socket);
        socket_schedule(socket, SOCKET_WORK_REAP |
            (queue == &receive_wait ? SOCKET_WORK_RECEIVE : SOCKET_WORK_OUTPUT));
        socket_state_release(&state);
        if (queue == &pool_wait) { SOCKET_ENDPOINT(endpoint,socket);wake_socket(socket);socket_async(socket,KERNEL_ASYNC_POLL_OUT); }
        return 1;
    }
    return 0;
}
static int service_retry_deadline(void)
{
    SOCKET_STATE(state);
    if (!write_retry_head || (int32_t)(sys_now() - write_retry_head->write_retry_ms) < 0) return 0;
    struct kernel_socket *socket = write_retry_head;
    write_retry_disarm(socket); work_remove_locked(&pool_wait, socket);
    socket_schedule(socket, SOCKET_WORK_OUTPUT | SOCKET_WORK_RECEIVE | SOCKET_WORK_REAP);
    socket_state_release(&state);
    SOCKET_ENDPOINT(endpoint,socket);
    wake_socket(socket);
    socket_async(socket,KERNEL_ASYNC_POLL_OUT);
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
#if BOAROS_USER_SMP
    /* All legacy entries have been delivered to their owners before clearing. */
    uint64_t state=__atomic_load_n(&fast_loopback_state,__ATOMIC_RELAXED);
    if ((state&LOOPBACK_LEGACY) && !fast_loopback->loop_first)
        __atomic_store_n(&fast_loopback_state,state&~UINT64_C(2),__ATOMIC_RELEASE);
#endif
}

static unsigned service_packet_ring_timers(unsigned budget)
{
    if (!budget || !packet_timer_head) return 0;
    uint32_t now=sys_now();
    unsigned retired=0;
    while (retired<budget && packet_timer_head) {
        struct kernel_socket *socket=packet_timer_head;
        struct socket_packet_options *options=socket->packet_options;
        if ((int32_t)(now-options->deadline)<0) break;
        packet_timer_disarm(options);
        if (!packet_ring_retire(options->ring,now)) __builtin_trap();
        SOCKET_ENDPOINT(state,socket);
        wake_socket(socket);
        socket_async(socket,KERNEL_ASYNC_POLL_IN);
        retired++;
    }
    return retired;
}

struct kernel_socket_service_result kernel_socket_service_owned(struct kernel_socket_protocol_scope *scope,
    struct kernel_socket_service_budget budget)
{
    struct kernel_socket_service_result result = {0};
#if BOAROS_COST_DIAGNOSTICS
    unsigned visits=0;
    if (*socket_poll_depth()) COST_ADD(NETWORK_POLL_SERVICES, 1);
#endif
    if (!scope->outer || protocol_current_owner() != kernel_io_context_current() || protocol_depth != 1) __builtin_trap();
    if (!socket_initialized) return result;
    COST_ADD(NETWORK_SERVICE_CALLS, 1);
    service_loopback(&result, budget.packets);
    /* 容量、到期重试和实际socket工作共用额度，沿原游标轮转。 */
    while (result.sockets < budget.sockets) {
        unsigned advanced = 0;
        for (unsigned i = 0; i < 3; i++) {
            unsigned which = service_cursor;
            service_cursor = (service_cursor + 1) % 3;
            if (which == 0) advanced=service_ready_socket(budget.sockets-result.sockets,&ready_work,
                                                        scope);
            else advanced=which == 1 ? service_capacity() : service_retry_deadline();
            if (advanced) break;
        }
        if (!advanced) break;
        result.sockets+=advanced;
#if BOAROS_COST_DIAGNOSTICS
        visits++;
#endif
    }
    /* 先批量接收旧ACK归还额度；本批COPY/窗口更新产生了新包时，
     * 使用尚未用完的原包预算交付，避免无谓地再等worker一次调度。 */
    if (loopback_pending && result.packets < budget.packets)
        service_loopback(&result, budget.packets);
    socket_timer_running = 1;
    result.timers = service_packet_ring_timers(budget.timers);
    result.timers += boaros_lwip_tcp_cork_due(budget.timers-result.timers);
    result.timers += sys_check_timeouts_budget(budget.timers-result.timers);
    socket_timer_running = 0;
    result.runnable = kernel_socket_work_pending();
    result.next_deadline = socket_next_timer_deadline();
    COST_ADD(NETWORK_SOCKET_VISITS, visits);
    COST_ADD(NETWORK_LOOP_PACKETS, result.packets);
    COST_ADD(NETWORK_TIMER_CALLBACKS, result.timers);
    return result;
}

struct kernel_socket_service_result kernel_socket_service_pending(struct kernel_socket_service_budget budget)
{
    if (protocol_current_owner() == kernel_io_context_current()) return (struct kernel_socket_service_result){0};
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    struct kernel_socket_service_result result = kernel_socket_service_owned(&scope, budget);
    kernel_socket_protocol_leave(&scope);
    return result;
}

void kernel_socket_network_purge_orphans(void)
{
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    { SOCKET_STATE(state); orphan_nic_wait &= ~1U; orphan_nic_ready &= ~1U; }
    struct tcp_pcb *lists[] = {tcp_active_pcbs, tcp_bound_pcbs, tcp_tw_pcbs};
    for (unsigned i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        for (struct tcp_pcb *pcb = lists[i], *next; pcb; pcb = next) {
            next = pcb->next;
            /* 最后一个 socket 已摘除回调；停服后不会再由协议定时器归还。 */
            if (!pcb->callback_arg && !pcb->errf) tcp_abandon(pcb, 0);
        }
    }
    /* A last FIN/ACK can have queued loopback traffic before the workers
     * stopped. Release it before detaching the interface. */
    while (loopback_pending) {
        struct kernel_socket_service_result drained={0};
        service_loopback(&drained,16);
    }
    kernel_socket_protocol_leave(&scope);
}

void kernel_socket_network_failed(uint32_t address)
{
    struct kernel_socket_protocol_scope saved; kernel_socket_protocol_enter(&saved);
    struct netif *failed=NULL;
    for (struct netif *n=netif_list;n;n=n->next)
        if (n->hwaddr_len && netif_ip4_addr(n)->addr==address) {failed=n;break;}
    socket_device_failed = 1;
    COST_ADD(NETWORK_GLOBAL_SCANS, 1);
    for (struct kernel_socket *s = inet_sockets; s; s = s->inet_next) {
#if BOAROS_USER_SMP
        if (tcp_socket_cpu(s)) {
            uint32_t local_address;
            static const uint8_t mapped[12]={0,0,0,0,0,0,0,0,0,0,0xff,0xff};
            unsigned offset=s->saved_local.family==KERNEL_SOCKET_AF_INET?0:12;
            if (!offset || !__builtin_memcmp(s->saved_local.bytes,mapped,sizeof(mapped))) {
                __builtin_memcpy(&local_address,s->saved_local.bytes+offset,sizeof(local_address));
                if (local_address==address) socket_schedule(s,SOCKET_WORK_DEVICE_FAILED);
            } else if (failed && s->saved_local.family==KERNEL_SOCKET_AF_INET6) {
                for(unsigned i=0;i<LWIP_IPV6_NUM_ADDRESSES;i++)
                    if((i==0 || failed->ip6_configured[i] || !ip6_addr_isinvalid(netif_ip6_addr_state(failed,i))) && !ip6_addr_isany(netif_ip6_addr(failed,i)) &&
                       !__builtin_memcmp(s->saved_local.bytes,netif_ip6_addr(failed,i)->addr,16) &&
                       (!s->saved_local.scope || s->saved_local.scope==netif_get_index(failed))) {
                        socket_schedule(s,SOCKET_WORK_DEVICE_FAILED);break;
                    }
            }
            continue;
        }
#endif
        const ip_addr_t *local = local_ip(s);
        int affected = local && IP_IS_V4(local) && ip4_addr_get_u32(ip_2_ip4(local)) == address;
        if (!affected && local && IP_IS_V6(local) && !ip_addr_isany(local) && failed)
            for(unsigned i=0;i<LWIP_IPV6_NUM_ADDRESSES;i++)
                if((i==0 || failed->ip6_configured[i] || !ip6_addr_isinvalid(netif_ip6_addr_state(failed,i))) && ip6_addr_eq(ip_2_ip6(local),netif_ip6_addr(failed,i))){affected=1;break;}
        if (!affected && s->udp && s->connected) {
            /* Connected UDP may still bind ANY; classify before carrier removal. */
            struct netif *route = ip_route(&s->udp->local_ip,&s->udp->remote_ip);
            affected = route && (route==failed ||
                (IP_IS_V4(&s->udp->remote_ip) && netif_ip4_addr(route)->addr==address));
        }
        if (!affected) continue;
        /* endpoint仍归OFD；设备故障与协议超时分开，排队数据先按原规则交付。 */
        if (s->tcp && !s->listening) {
            /* tcp_failed publishes this device error exactly once. */
            tcp_abort(s->tcp);
        } else {
            SOCKET_ENDPOINT(endpoint,s);
            s->error = s->pending_error = -KERNEL_EIO;
            wake_socket(s);
            socket_async(s,KERNEL_ASYNC_POLL_ERR);
            socket_state_release(&endpoint);
        }
    }
    socket_device_failed = 0;
    kernel_socket_protocol_leave(&saved);
}
int kernel_socket_family(const struct kernel_socket *s) {return s->family;}
static void ipv6_withdraw(struct netif *n)
{
    for(unsigned i=1;i<LWIP_IPV6_NUM_ADDRESSES;i++)
        if(!ip6_addr_isinvalid(netif_ip6_addr_state(n,i)))netif_ip6_addr_set_state_preserve_pcbs(n,i,IP6_ADDR_INVALID);
}
int kernel_socket_ipv6_address(uint32_t index,const uint8_t bytes[16],uint32_t prefix,int remove)
{
    if(prefix>128)return -KERNEL_EINVAL;
    if(!index || index>UINT8_MAX)return -KERNEL_ENODEV;
    ip6_addr_t address;__builtin_memcpy(address.addr,bytes,16);ip6_addr_clear_zone(&address);
    /* Link-local provisioning remains owned by DAD startup; multicast, mapped
     * and loopback configuration need different semantics, never fake success. */
    if(ip6_addr_isany(&address) || ip6_addr_ismulticast(&address) ||
       ip6_addr_islinklocal(&address) || ip6_addr_isloopback(&address) ||
       ip6_addr_isipv4mappedipv6(&address))return -KERNEL_EINVAL;
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    struct netif *n=netif_get_by_index(index);int result=-KERNEL_ENODEV;
    if(n) {
        if(!n->hwaddr_len)result=-KERNEL_EOPNOTSUPP;
        else if(n->mtu<1280 && !remove)result=-KERNEL_EINVAL;
        else {
            int found=-1,free_slot=-1;
            for(unsigned i=1;i<LWIP_IPV6_NUM_ADDRESSES;i++) {
                if(!ip6_addr_isinvalid(netif_ip6_addr_state(n,i)) && ip6_addr_zoneless_eq(&address,netif_ip6_addr(n,i)))found=(int)i;
                if(i<=BOAROS_LWIP_STATIC_IP6_SLOTS && !n->ip6_configured[i] && free_slot<0)free_slot=(int)i;
            }
            if(remove) {
                result=-KERNEL_EADDRNOTAVAIL;
                if(found>=0 && n->ip6_prefix[found]==prefix) {
                    nd6_invalidate_netif_routes(n);n->ip6_configured[found]=0;
                    netif_ip6_addr_set_state_preserve_pcbs(n,found,IP6_ADDR_INVALID);
                    ip6_addr_t zero={0};netif_ip6_addr_set(n,found,&zero);
                    result=0;
                }
            } else if(found>=0)result=-KERNEL_EEXIST;
            else if(free_slot<0)result=-KERNEL_ENOBUFS;
            else {
                nd6_invalidate_netif_routes(n);
                n->ip6_prefix[free_slot]=(uint8_t)prefix;n->ip6_configured[free_slot]=1;
                netif_ip6_addr_set(n,free_slot,&address);
                netif_ip6_addr_set_valid_life(n,free_slot,IP6_ADDR_LIFE_STATIC);
                netif_ip6_addr_set_pref_life(n,free_slot,IP6_ADDR_LIFE_STATIC);
                netif_ip6_addr_set_state(n,free_slot,IP6_ADDR_TENTATIVE);
                result=0;
            }
        }
    }
    kernel_socket_protocol_leave(&scope);return result;
}
int kernel_socket_ipv6_gateway(uint32_t index,const uint8_t bytes[16],int remove)
{
    if(index>UINT8_MAX)return -KERNEL_ENODEV;
    ip6_addr_t gateway;__builtin_memcpy(gateway.addr,bytes,16);ip6_addr_clear_zone(&gateway);
    if(!remove && !index && ip6_addr_islinklocal(&gateway))return -KERNEL_EINVAL;
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    struct netif *n=index?netif_get_by_index(index):NULL;int result=remove?-KERNEL_ESRCH:-KERNEL_ENETUNREACH;
    if(index && !n)result=-KERNEL_ENODEV;
    else {
        if(!n)for(struct netif *p=netif_list;p;p=p->next) {
            if(remove && !ip6_addr_isany(&p->ip6_gateway)){n=p;break;}
            if(!remove && !ip6_addr_islinklocal(&gateway) && boaros_lwip_ip6_onlink(p,&gateway)>=0){n=p;break;}
        }
        if(n) {
            ip6_addr_assign_zone(&gateway,IP6_UNICAST,n);
            if(remove) {
                if(ip6_addr_isany(&n->ip6_gateway) || (remove==2 && !ip6_addr_eq(&gateway,&n->ip6_gateway)))result=-KERNEL_ESRCH;
                else {nd6_invalidate_netif_routes(n);ip6_addr_set_zero(&n->ip6_gateway);result=0;}
            } else if(!netif_is_up(n))result=-KERNEL_ENETDOWN;
            else if(!n->hwaddr_len || n->mtu<1280 || ip6_addr_isany(&gateway) ||
                ip6_addr_ismulticast(&gateway) || ip6_addr_isloopback(&gateway) ||
                ip6_addr_isipv4mappedipv6(&gateway) || boaros_lwip_ip6_onlink(n,&gateway)<0)result=-KERNEL_ENETUNREACH;
            else {
                result=0;
                for(struct netif *p=netif_list;p;p=p->next)
                    if(!ip6_addr_isany(&p->ip6_gateway))result=-KERNEL_EEXIST;
                for(unsigned i=0;i<LWIP_IPV6_NUM_ADDRESSES;i++)
                    if(ip6_addr_zoneless_eq(&gateway,netif_ip6_addr(n,i)))result=-KERNEL_EINVAL;
                if(!result){nd6_invalidate_netif_routes(n);ip6_addr_copy(n->ip6_gateway,gateway);}
            }
        }
    }
    kernel_socket_protocol_leave(&scope);return result;
}
static void interface_snapshot(struct netif *n, struct kernel_socket_interface *s)
{
    *s = (struct kernel_socket_interface){0};
    int loop = n->name[0] == 'l' && n->name[1] == 'o';
    __builtin_memcpy(s->name, loop ? "lo" : "eth0", loop ? 3 : 5);
    s->index = netif_get_index(n); s->address = netif_ip4_addr(n)->addr;
    /* loopif的0表示不受L2 MTU限制；Linux逻辑MTU容纳完整IPv4长度范围。 */
    s->netmask = netif_ip4_netmask(n)->addr; s->mtu = loop && n->mtu == 0 ? 65536U : n->mtu;
    s->gateway = netif_ip4_gw(n)->addr;
    s->default_route = n == netif_default && s->gateway != 0;
    unsigned state=netif_ip6_addr_state(n,0);
    s->ipv6_present=!ip6_addr_isinvalid(state);
    if (s->ipv6_present) {
        __builtin_memcpy(s->ipv6_address,netif_ip6_addr(n,0)->addr,16);
        s->ipv6_flags=128 | ((ip6_addr_istentative(state) || ip6_addr_isduplicated(state))?64:0) |
            (ip6_addr_isduplicated(state)?8:0) | (ip6_addr_isdeprecated(state)?32:0);
    }
    _Static_assert(LWIP_IPV6_NUM_ADDRESSES==5,"snapshot address capacity");
    _Static_assert(LWIP_ND6_NUM_PREFIXES==5 && LWIP_ND6_NUM_ROUTERS==3,"snapshot route capacity");
    for(unsigned i=1;i<LWIP_IPV6_NUM_ADDRESSES;i++) {
        unsigned st=netif_ip6_addr_state(n,i);
        if(ip6_addr_isinvalid(st) && !n->ip6_configured[i])continue;
        struct kernel_socket_ipv6_address *a=&s->ipv6_addresses[i-1];
        a->present=1;
        __builtin_memcpy(a->address,netif_ip6_addr(n,i)->addr,16);
        a->prefix=n->ip6_prefix[i];
        a->flags=(netif_ip6_addr_isstatic(n,i)?128:0) | (!ip6_addr_isvalid(st)?64:0) |
            (ip6_addr_isduplicated(st)?8:0) | (ip6_addr_isdeprecated(st)?32:0);
        a->valid=netif_ip6_addr_isstatic(n,i)?UINT32_MAX:netif_ip6_addr_valid_life(n,i);
        a->preferred=netif_ip6_addr_isstatic(n,i)?UINT32_MAX:netif_ip6_addr_pref_life(n,i);
        a->created=n->ip6_created[i];a->updated=n->ip6_updated[i];
    }
    for(unsigned i=0;i<LWIP_ND6_NUM_PREFIXES;i++) {
        const struct nd6_prefix_list_entry *p=&prefix_list[i];
        if(p->netif!=n || !p->invalidation_timer)continue;
        s->ipv6_prefixes[i].present=1;s->ipv6_prefixes[i].prefix=p->prefix_length;
        s->ipv6_prefixes[i].lifetime=p->invalidation_timer;
        __builtin_memcpy(s->ipv6_prefixes[i].address,p->prefix.addr,16);
    }
    for(unsigned i=0;i<LWIP_ND6_NUM_ROUTERS;i++) {
        const struct nd6_router_list_entry *r=&default_router_list[i];
        if(!r->neighbor_entry || r->neighbor_entry->netif!=n || !r->invalidation_timer)continue;
        s->ipv6_routers[i].present=1;s->ipv6_routers[i].lifetime=r->invalidation_timer;
        unsigned pref=(r->flags>>3)&3;
        s->ipv6_routers[i].preference=pref==2?0:pref;
        __builtin_memcpy(s->ipv6_routers[i].address,r->neighbor_entry->next_hop_address.addr,16);
    }
    __builtin_memcpy(s->ipv6_gateway,n->ip6_gateway.addr,16);
    s->hardware_type = loop ? 772 : 1;
    s->link_up = !!netif_is_link_up(n);
    if (netif_is_up(n)) s->flags |= 1;
    if (netif_is_up(n) && netif_is_link_up(n)) s->flags |= 0x40;
    s->flags |= loop ? 8 : 0x1002;
    if (!loop) __builtin_memcpy(s->mac, n->hwaddr, 6);
}
static int route_snapshot(struct kernel_socket_interface *interfaces,size_t *count)
{
    KERNEL_SERVICE_SCOPE(service);
    kernel_socket_network_initialize();
    struct kernel_socket_protocol_scope scope;kernel_socket_protocol_enter(&scope);
    for(struct netif *n=netif_list;n;n=n->next) {
        if(*count==ROUTE_SNAPSHOT_MAX) {kernel_socket_protocol_leave(&scope);return -KERNEL_ENOBUFS;}
        interface_snapshot(n,&interfaces[(*count)++]);
    }
    kernel_socket_protocol_leave(&scope);return 0;
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
            socket_set_interface_up(n,flags & 1);
            kernel_socket_protocol_leave(&irq); return 0;
        }
    }
    kernel_socket_protocol_leave(&irq); return -KERNEL_ENODEV;
}
int kernel_socket_interface_set_ipv4(const char name[16], uint32_t command, uint32_t value)
{
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    int result = -KERNEL_ENODEV;
    for (struct netif *n = netif_list; n; n = n->next) {
        int loop = n->name[0] == 'l' && n->name[1] == 'o';
        if (__builtin_memcmp(name, loop ? "lo" : "eth0", loop ? 3 : 5)) continue;
        ip4_addr_t address = {value};
        uint32_t host = lwip_ntohl(value);
        result = 0;
        if (command == 0x8916) {
            if ((host >> 28) == 14) { result = -KERNEL_EINVAL; break; }
            if (value != netif_ip4_addr(n)->addr) {
                uint32_t mask = (host >> 24) == 0 || host == UINT32_MAX ? 0 :
                    (host >> 31) == 0 ? 0xff000000U :
                    (host >> 30) == 2 ? 0xffff0000U :
                    (host >> 29) == 6 ? 0xffffff00U : UINT32_MAX;
                ip4_addr_t netmask = {lwip_htonl(mask)};
#if BOAROS_USER_SMP
                fast_loopback_revoke(n);
#endif
                netif_set_netmask(n, &netmask);
                netif_set_ipaddr_preserve_pcbs(n, &address);
#if BOAROS_USER_SMP
                fast_loopback_refresh(n);
#endif
            }
        } else {
            uint32_t inverted = ~host;
            if (netif_ip4_addr(n)->addr == 0) { result = -KERNEL_EADDRNOTAVAIL; break; }
            if ((inverted & (inverted + 1U)) != 0) { result = -KERNEL_EINVAL; break; }
            if (netif_ip4_netmask(n)->addr!=value) {
#if BOAROS_USER_SMP
                fast_loopback_revoke(n);
#endif
                netif_set_netmask(n, &address);
#if BOAROS_USER_SMP
                fast_loopback_refresh(n);
#endif
            }
        }
        if (netif_default == n && netif_ip4_gw(n)->addr &&
            (!netif_ip4_addr(n)->addr ||
             !ip4_addr_net_eq(netif_ip4_gw(n), netif_ip4_addr(n), netif_ip4_netmask(n)))) {
            ip4_addr_t empty = {0};
            netif_set_gw(n, &empty);
            netif_set_default(0);
        }
        break;
    }
    kernel_socket_protocol_leave(&scope); return result;
}
int kernel_socket_interface_set_mtu(const char name[16], int32_t mtu)
{
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    int result = -KERNEL_ENODEV;
    for (struct netif *n = netif_list; n; n = n->next) {
        int loop = n->name[0] == 'l' && n->name[1] == 'o';
        if (__builtin_memcmp(name, loop ? "lo" : "eth0", loop ? 3 : 5)) continue;
        if (mtu < 68 || mtu > (loop ? 65536 : 1500)) result = -KERNEL_EINVAL;
        else {
            uint16_t next=mtu==65536?0:(uint16_t)mtu;
            if (n->mtu!=next) {
#if BOAROS_USER_SMP
                fast_loopback_revoke(n);
                boaros_lwip_ipv4_path_changed();
#endif
                n->mtu=next;
                if (!loop) {
                    /* Configuration changes invalidate both queued ND packets
                     * and destination PMTU, including a later MTU increase. */
                    if(next<1280)nd6_cleanup_netif(n);else nd6_invalidate_netif_routes(n);
#if LWIP_ND6_ALLOW_RA_UPDATES
                    n->mtu6=next;
#endif
                    if (next<1280) {
                        ipv6_withdraw(n);
                        __builtin_memset(n->ip6_configured,0,sizeof(n->ip6_configured));
                        ip6_addr_set_zero(&n->ip6_gateway);
                        netif_ip6_addr_set_state(n,0,IP6_ADDR_INVALID);
                        /* IPv6 device teardown, unlike ordinary link DOWN:
                         * retain socket records, but retire their aggregate
                         * references. A later old LEAVE must not consume a new
                         * socket's membership after IPv6 is recreated. */
                        for(struct kernel_socket *s=inet_sockets;s;s=s->inet_next)
                            for(struct socket_ipv6_member *m=s->ipv6_members;m;m=m->next)
                                if(m->index==netif_get_index(n))m->active=0;
                        mld6_stop(n);
                        __builtin_memset(n->ip6_mld_joined,0,sizeof(n->ip6_mld_joined));
                    }
                    if (next>=1280 && ip6_addr_isinvalid(netif_ip6_addr_state(n,0))) {
                        netif_create_ip6_linklocal_address(n,1);
                        nd6_restart_netif(n);
                    }
                }
#if BOAROS_USER_SMP
                fast_loopback_refresh(n);
#endif
            }
            result=0;
        }
        break;
    }
    kernel_socket_protocol_leave(&scope); return result;
}
int kernel_socket_default_route(const char *name, uint32_t gateway, int remove)
{
    struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
    struct netif *chosen = 0;
    ip4_addr_t address = {gateway};
    int result = -KERNEL_ENETUNREACH;
    if (remove && !name && !gateway) chosen = netif_default;
    for (struct netif *n = netif_list; n; n = n->next) {
        if (chosen) break;
        int loop = n->name[0] == 'l' && n->name[1] == 'o';
        if (name && __builtin_memcmp(name, loop ? "lo" : "eth0", loop ? 3 : 5)) continue;
        if (!name && gateway && !ip4_addr_net_eq(&address, netif_ip4_addr(n), netif_ip4_netmask(n)))
            continue;
        chosen = n;
        break;
    }
    if (!chosen) result = name ? -KERNEL_ENODEV : -KERNEL_ENETUNREACH;
    else if (!remove && (!netif_is_up(chosen) || netif_ip4_addr(chosen)->addr == 0 ||
             !ip4_addr_net_eq(&address, netif_ip4_addr(chosen), netif_ip4_netmask(chosen))))
        result = -KERNEL_ENETUNREACH;
    else if (remove) {
        if (netif_default != chosen || !netif_ip4_gw(chosen)->addr ||
            (gateway && netif_ip4_gw(chosen)->addr != gateway))
            result = -KERNEL_ESRCH;
        else {
            ip4_addr_t empty = {0}; netif_set_gw(chosen, &empty);
            netif_set_default(0); result = 0;
        }
    } else if (netif_default && netif_ip4_gw(netif_default)->addr != 0)
        result = -KERNEL_EEXIST;
    else {
        netif_set_gw(chosen, &address); netif_set_default(chosen); result = 0;
    }
    kernel_socket_protocol_leave(&scope); return result;
}
