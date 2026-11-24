#include <kernel/network.h>
#include <kernel/socket.h>
#include <kernel/socket_control.h>
#include <kernel/errno.h>
#include <kernel/cost.h>
#include <kernel/console.h>
#include <kernel/igb.h>
#include <arch/context.h>
#include <arch/bus.h>
#include <arch/timer.h>
#include <kernel/virtio_net.h>
#include <kernel/page.h>
#include "net-config.h"
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "lwip/ip4_frag.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "boaros_lwip.h"
#include "netif/ethernet.h"
#include <stddef.h>
#include <string.h>

struct network_rx {
    struct pbuf_custom custom;
    struct virtio_net_device *device;
    unsigned buffer;
};
enum { IGB_PORT_BUFFERS = 64 };
struct network_igb_buffer {
    struct pbuf_custom header, payload;
    struct network_igb_area *area;
    uint8_t *head, *data;
    uint64_t physical, header_physical;
    struct kernel_network_igb_zc_buffer loan;
    const struct kernel_network_igb_zc_ops *loan_ops;
    void *provider;
    unsigned references;
};
struct network_igb_area {
    struct network_igb_buffer buffers[IGB_PORT_BUFFERS];
    struct kernel_network *network;
    struct netif secondary;
    uint64_t free_buffers, physical, header_physical;
    unsigned loans, order, header_order;
    const struct kernel_network_igb_zc_ops *ops;
    void *provider;
    bool isolated;
};
struct kernel_network {
    struct virtio_net_device *device;
    struct kernel_thread_join worker;
    struct kernel_wait_queue progress;
    uint64_t frequency;
    uint8_t stopping;
    struct netif interface;
    struct network_rx rx[64];
    struct kernel_heap *heap;
    uint64_t deadline, capacity_generation;
    uint8_t attached, failure_delivered;
    struct kernel_igb_rx *igb;
    struct network_igb_area *igb_area, *igb_zc;
    bool igb_zc_first, igb_zc_attached;
};
static void print_text(const char *s) { while (*s) kernel_console_putc(*s++); }
static void print_u64(uint64_t v)
{
    char digits[24]; unsigned n = 0;
    do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) kernel_console_putc(digits[--n]);
}
static void rx_free(struct pbuf *p)
{
    struct network_rx *r = (void *)p;
    virtio_net_release(r->device, r->buffer);
}
static int copy_pbuf(const void *context, void *destination, uint32_t length)
{ return pbuf_copy_partial(context, destination, (u16_t)length, 0) == length; }
static int copy_frame(const void *context, void *destination, uint32_t length)
{ memcpy(destination, context, length); return 1; }
static void free_tx_pbuf_direct(void *const *payloads, unsigned count)
{
    for (unsigned i = 0; i < count; i++) pbuf_free(payloads[i]);
}
static err_t submit_link_frame(struct netif *interface, struct pbuf *p, int more)
{
    COST_SCOPE(output_cost, NET_TX_OUTPUT_TICKS);
    struct kernel_network *n = interface->state;
    if (n->igb) {
        int error = kernel_igb_tx_copy(n->igb, p->tot_len, copy_pbuf, p, more);
        if (error == -KERNEL_EAGAIN) { kernel_socket_network_blocked(); return ERR_MEM; }
        return error ? ERR_IF : ERR_OK;
    }
    /* 零拷贝候选：整链为可 DMA 的 PBUF_RAM/POOL、段数受限且地址可换算。 */
    if (p->tot_len != 0 && p->tot_len <= 1514) {
        struct virtio_net_tx_segment segments[VIRTIO_NET_TX_SEGMENTS];
        unsigned count = 0;
        int eligible = 1;
        for (struct pbuf *q = p; q != 0; q = q->next) {
            uint64_t physical;
            if (count == VIRTIO_NET_TX_SEGMENTS || q->len == 0 ||
                PBUF_NEEDS_COPY(q) ||
                !arch_dma_image_address((uint64_t)(uintptr_t)q->payload, q->len, &physical)) {
                eligible = 0; break;
            }
            segments[count].physical_address = physical;
            segments[count].length = q->len;
            count++;
        }
        if (eligible && count != 0) {
            pbuf_ref(p);
            int result = virtio_net_send_segments(n->device, segments, count, p, more);
            /* The submitter can return an already completed slot even when
             * it is the serial protocol owner and the worker cannot run. */
            if (result == -KERNEL_EAGAIN &&
                virtio_net_tx_reclaim(n->device, free_tx_pbuf_direct))
                result = virtio_net_send_segments(n->device, segments, count, p, more);
            if (result == 0) return ERR_OK;
            pbuf_free(p);
            if (result == -KERNEL_EAGAIN) { kernel_socket_network_blocked(); return ERR_MEM; }
            if (result != -KERNEL_ENOTSUP) return ERR_IF;
            /* 未协商 indirect：回退复制路径。 */
        }
    }
    int result = virtio_net_send_copy(n->device, p->tot_len, copy_pbuf, p, more);
    if (result == -KERNEL_EAGAIN &&
        virtio_net_tx_reclaim(n->device, free_tx_pbuf_direct))
        result = virtio_net_send_copy(n->device, p->tot_len, copy_pbuf, p, more);
    /* 复制回退耗尽的仍是 NIC 槽；只等协议池归还会漏掉真实 TX 完成。 */
    if (result == -KERNEL_EAGAIN) { kernel_socket_network_blocked(); return ERR_MEM; }
    return result >= 0 ? ERR_OK : result == -KERNEL_EMSGSIZE ? ERR_BUF : ERR_IF;
}
static err_t output_link_frame(struct netif *interface, struct pbuf *p, int more)
{
    err_t result = submit_link_frame(interface, p, more);
    if (result == ERR_OK) kernel_socket_packet_ethernet(interface,p,NULL,p->tot_len,1);
    return result;
}
static void flush_link_frames(struct netif *interface)
{
    struct kernel_network *n = interface->state;
    if (n->igb) kernel_igb_tx_flush(n->igb);
    else virtio_net_tx_flush(n->device);
}
static err_t link_output(struct netif *interface, struct pbuf *p)
{
    struct boaros_link_defer *defer = interface->boaros_link_defer;
    /* ARP probes, reinjected packets and fragments own other buffers and
     * still go through the synchronous link path. */
    if (defer && defer->packet == p && !defer->submit) {
        defer->submit = output_link_frame;
        return ERR_OK;
    }
    return output_link_frame(interface, p, 0);
}
int kernel_network_send_frame(struct netif *interface, const void *frame, uint16_t size)
{
    /* The protocol owner keeps the Ethernet attachment and device alive. */
    if (interface->linkoutput != link_output) return -KERNEL_EOPNOTSUPP;
    struct kernel_network *n=interface->state;
    int result=n->igb ? kernel_igb_tx_copy(n->igb, size, copy_frame, frame, 0) :
        virtio_net_send(n->device,frame,size);
    if (result==-KERNEL_EAGAIN) {
        kernel_socket_network_blocked();
        return -KERNEL_ENOBUFS;
    }
    return result<0 ? result : 0;
}

static err_t ipv6_output(struct netif *interface,struct pbuf *p,const ip6_addr_t *destination)
{
    /* IPv6 cannot use a sub-1280 link. Check before neighbor queue admission,
     * otherwise an oversized unresolved datagram would falsely succeed. */
    if (interface->mtu<1280) return ERR_RTE;
    if (p->tot_len>netif_mtu6(interface)) return ERR_BUF;
    const struct ip6_hdr *header=p->payload;
    ip6_addr_t source;ip6_addr_copy_from_packed(source,header->src);
    /* Loopback is host-local even when an explicit device or /0 route would
     * otherwise select Ethernet. Enforce this for TCP and queued output too. */
    if(ip6_addr_isloopback(&source) || ip6_addr_isloopback(destination))return ERR_RTE;
    /* Unspecified source is only for DAD and RFC3590 MLD bootstrap. Ordinary
     * queued/application packets must retain an available local source. */
    if(ip6_addr_isany(&source)) {
        int bootstrap=IP6H_NEXTH(header)==58 && IP6H_HOPLIM(header)==255 &&
            p->tot_len>=64 && pbuf_get_at(p,40)==135 && !pbuf_get_at(p,41) &&
            ip6_addr_issolicitednode(destination);
        if(!bootstrap && IP6H_NEXTH(header)==0 && IP6H_HOPLIM(header)==1 && p->tot_len>=72) {
            unsigned char options[8];ip6_addr_t group;
            static const unsigned char mld_alert[8]={58,0,5,2,0,0,1,0};
            pbuf_copy_partial(p,options,8,40);pbuf_copy_partial(p,group.addr,16,56);ip6_addr_clear_zone(&group);
            unsigned type=pbuf_get_at(p,48);
            bootstrap=!memcmp(options,mld_alert,8) && !pbuf_get_at(p,49) &&
                ip6_addr_ismulticast(&group) && ip6_addr_multicast_scope(&group)>1 &&
                !ip6_addr_isallnodes_linklocal(&group) &&
                ((type==131 && ip6_addr_zoneless_eq(&group,destination)) ||
                 (type==132 && ip6_addr_isallrouters_linklocal(destination)));
        }
        if(!bootstrap)return ERR_RTE;
    } else {
        int valid=0;
        for (unsigned i=0;i<LWIP_IPV6_NUM_ADDRESSES;i++)
            if (ip6_addr_isvalid(netif_ip6_addr_state(interface,i)) &&
                ip6_addr_zoneless_eq(&source,netif_ip6_addr(interface,i))) {valid=1;break;}
        if (!valid) return ERR_RTE;
    }
    return ethip6_output(interface,p,destination);
}
static err_t interface_init(struct netif *interface)
{
    struct kernel_network *n = interface->state;
    interface->name[0] = 'e'; interface->name[1] = 't';
    interface->hwaddr_len = 6;
    if (n->igb) kernel_igb_mac_address(n->igb, interface->hwaddr);
    else memcpy(interface->hwaddr, n->device->mac, 6);
    interface->mtu = 1500;
    interface->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET |
        NETIF_FLAG_IGMP | NETIF_FLAG_MLD6;
    interface->output = etharp_output; interface->output_ip6 = ipv6_output;
    interface->linkoutput = link_output;
    interface->boaros_link_submit = output_link_frame;
    interface->boaros_link_flush = flush_link_frames;
    return ERR_OK;
}

static err_t igb_isolated_interface_init(struct netif *interface)
{
    err_t error = interface_init(interface);
    interface->name[0] = 'i';
    interface->name[1] = 'z';
    interface->hwaddr[0] |= 2U;
    interface->hwaddr[5] ^= 0x80U;
    interface->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return error;
}
static unsigned pool_used(void)
{ return lwip_stats.memp[MEMP_PBUF_POOL] ? MEMP_STATS_GET(used, MEMP_PBUF_POOL) : 0; }
static int udp_capacity_after(unsigned loaned, unsigned loans, unsigned pbufs)
{
    unsigned used = pool_used() + pbufs;
    uint64_t owned = STATS_GET(mem.used) + ((uint64_t)loaned + loans) * 2048 +
        (uint64_t)used * PBUF_POOL_BUFSIZE;
    return used <= PBUF_POOL_SIZE - 16 && owned <= MEM_SIZE - (65536U + 4096U);
}
static int udp_capacity(void *context)
{
    struct kernel_network *n = context;
    if (n->igb)
        return udp_capacity_after(__atomic_load_n(&n->igb_area->loans, __ATOMIC_RELAXED), 0, 0);
    return udp_capacity_after(virtio_net_loan_count(n->device), 0, 0);
}

struct pbuf *boaros_lwip_udp_copy_borrowed(struct pbuf *p)
{
    struct network_rx *rx=(void *)p;
    struct virtio_net_device *device=rx->device;
    unsigned bytes=LWIP_MEM_ALIGN_SIZE(PBUF_TRANSPORT)+p->tot_len;
    unsigned pool=(bytes+PBUF_POOL_BUFSIZE-1)/PBUF_POOL_BUFSIZE;
    if (!udp_capacity_after(virtio_net_loan_count(device),0,pool)) return NULL;
    struct pbuf *owned=pbuf_clone(PBUF_TRANSPORT,PBUF_POOL,p);
    if (owned) {
        owned->if_idx=p->if_idx;
        device->statistics.copied_packets++;
        device->statistics.copied_bytes+=p->tot_len;
    }
    return owned;
}

static int udp_frame_can_borrow(struct kernel_network *n, const struct virtio_net_frame *frame)
{
    const unsigned char *data=frame->data;
    /* Only local IPv4 UDP with an unfragmented, fixed-size IP header can
     * reach UDP synchronously. Raw IP observers retain the original path. */
    if (frame->size<42 || (data[0]&1) || data[12]!=8 || data[13]!=0 ||
        data[14]!=0x45 || (data[20]&0x3f) || data[21] || data[23]!=17 || data[30]>=224)
        return 0;
    uint32_t destination;
    memcpy(&destination,data+30,sizeof(destination));
    return destination==netif_ip4_addr(&n->interface)->addr && boaros_lwip_raw_input_idle();
}

static struct kernel_wait_queue *progress(struct kernel_network *n)
{
    if (n->igb) return kernel_igb_progress(n->igb);
    return n->device ? &n->device->progress : &n->progress;
}
static void timer_wake(void *context)
{
    struct kernel_network *n = context;
    uintptr_t irq = arch_interrupt_save();
    if (n->deadline && (int64_t)(arch_time_read() - n->deadline) >= 0)
        (void)kernel_wait_queue_wake_all(progress(n));
    arch_interrupt_restore(irq);
}
static int device_work_pending(struct kernel_network *n, uint64_t generation)
{
    if (n->igb)
        return kernel_igb_pending(n->igb,
            __atomic_load_n(&n->igb_area->free_buffers, __ATOMIC_ACQUIRE) != 0) ||
            (n->igb_zc->provider && kernel_igb_zc_pending(n->igb)) ||
            (kernel_igb_rx_link_up(n->igb) != !!netif_is_link_up(&n->interface));
    if (!n->device) return 0;
    struct virtio_net_progress state = virtio_net_get_progress(n->device);
    return state.polling || generation != state.tx_capacity_generation || state.tx_done_count ||
        state.rx_refill_pending || (state.failed ? !n->failure_delivered :
            state.ready_count || state.link_up != netif_is_link_up(&n->interface));
}
/* 本批服务的outer owner直接覆盖等待登记，避免向FIFO交接后立刻再次排队。 */
static void wait_progress(struct kernel_network *n, struct kernel_socket_protocol_scope *scope,
    struct kernel_socket_service_result service, uint64_t generation)
{
    (void)arch_interrupt_save();
    int runnable = kernel_socket_work_pending() || device_work_pending(n, generation);
    if (n->stopping || runnable) {
        kernel_socket_protocol_leave(scope);
        if (!n->stopping && kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        return;
    }
    n->deadline = service.next_deadline;
    uint64_t timeout = n->device ? virtio_net_watchdog_deadline(n->device) :
        arch_time_read() + (n->igb ? n->frequency /
            (kernel_igb_irq_available(n->igb) ? 20 : 1000) : 5 * n->frequency);
    if (!n->deadline || timeout < n->deadline) n->deadline = timeout;
    struct kernel_wait_token token = {0};
    enum kernel_wait_wake_reason reason;
    if (kernel_wait_prepare(progress(n), n->deadline, 0, &token) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    int sleep = !n->stopping && !kernel_socket_work_pending() && !device_work_pending(n, generation);
    /* 远端归还RX借用或报告故障也能在登记前发生；重查实际状态后才休眠。 */
    kernel_socket_protocol_leave(scope);
    if (sleep && kernel_wait_park(&token, &reason) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    if (kernel_wait_finish(&token) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
}
static int stopping(struct kernel_network *n)
{
    uintptr_t irq = arch_interrupt_save();
    int result = n->stopping;
    arch_interrupt_restore(irq);
    return result;
}
/* 无 NIC 的 timer-only owner：IRQ 只唤醒它，协议定时器在 worker 上下文推进。 */
static void timer_worker(void *context)
{
    struct kernel_network *n = context;
    while (!stopping(n)) {
        kernel_socket_rights_service();
        uintptr_t irq = arch_interrupt_save();
        struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
        struct kernel_socket_service_result service = kernel_socket_service_owned(&scope,
            (struct kernel_socket_service_budget){8, 8, 1});
        wait_progress(n, &scope, service, 0);
        arch_interrupt_restore(irq);
    }
}
static void deliver_pbuf(struct netif *interface, struct pbuf *p)
{
    /* A successful Ethernet input consumes or retains the packet chain. */
    if (interface->input(p, interface) != ERR_OK) pbuf_free(p);
}

static int budgeted_frame(const uint8_t *data, unsigned length)
{
    /* Extension-header traffic may terminate in UDP. Keep control reserve
     * available for direct TCP and ICMPv6. */
    return (length >= 34 && data[12] == 8 && data[13] == 0 && data[23] == 17) ||
        (length >= 54 && data[12] == 0x86 && data[13] == 0xdd &&
         data[20] != 6 && data[20] != 58);
}

static void igb_return_buffer(struct network_igb_buffer *slot)
{
    struct network_igb_area *area = slot->area;
    struct kernel_network *n = area->network;
    if (slot->loan_ops) {
        slot->loan_ops->put(slot->provider, slot->loan.cookie);
        slot->loan_ops = NULL;
    }
    uint64_t bit = UINT64_C(1) << (slot - area->buffers);
    if (!__atomic_fetch_or(&area->free_buffers, bit, __ATOMIC_RELEASE))
        (void)kernel_wait_queue_wake_all(progress(n));
}

static void igb_release_pbuf(struct network_igb_buffer *slot)
{
    if (__atomic_sub_fetch(&slot->references, 1, __ATOMIC_ACQ_REL)) return;
    struct network_igb_area *area = slot->area;
    igb_return_buffer(slot);
    __atomic_sub_fetch(&area->loans, 1, __ATOMIC_RELEASE);
}

static void igb_free_header(struct pbuf *p)
{ igb_release_pbuf((void *)p); }

static void igb_free_payload(struct pbuf *p)
{
    struct pbuf_custom *payload = (void *)p;
    igb_release_pbuf((void *)((uint8_t *)payload - offsetof(struct network_igb_buffer, payload)));
}

int kernel_network_igb_zc_payload(const struct pbuf *p, const void *provider,
    void **cookie, unsigned *offset)
{
    if (!(p->flags & PBUF_FLAG_IS_CUSTOM) ||
        ((const struct pbuf_custom *)p)->custom_free_function != igb_free_payload) return 0;
    const struct network_igb_buffer *slot =
        (const void *)((const uint8_t *)p - offsetof(struct network_igb_buffer, payload));
    if (!slot->loan_ops || slot->provider != provider) return 0;
    uintptr_t delta = (uintptr_t)p->payload - (uintptr_t)slot->loan.address;
    if (delta >= KERNEL_IGB_RX_PAYLOAD || p->len > KERNEL_IGB_RX_PAYLOAD - delta)
        return 0;
    *cookie = slot->loan.cookie;
    *offset = (unsigned)delta;
    return 1;
}

static unsigned igb_refill(struct network_igb_area *area)
{
    unsigned count = 0;
    struct kernel_network *n = area->network;
    unsigned space = area->isolated ? kernel_igb_zc_space(n->igb) : KERNEL_IGB_RX_DEPTH;
    uint64_t available = __atomic_load_n(&area->free_buffers, __ATOMIC_ACQUIRE);
    while (available && count < space) {
        struct kernel_igb_rx_buffer batch[KERNEL_NETWORK_RECEIVE_BATCH];
        uint64_t taken = 0;
        unsigned used = 0;
        for (unsigned i = 0; i < IGB_PORT_BUFFERS && used < KERNEL_NETWORK_RECEIVE_BATCH &&
             used + count < space; i++) {
            uint64_t bit = UINT64_C(1) << i;
            if (!(available & bit)) continue;
            struct network_igb_buffer *slot = &area->buffers[i];
            uint64_t physical = slot->physical;
            if (area->ops && !area->ops->take(area->provider, &slot->loan)) {
                slot->loan_ops = area->ops;
                slot->provider = area->provider;
                physical = slot->loan.physical;
            }
            batch[used++] = (struct kernel_igb_rx_buffer){
                physical, slot->header_physical, slot,
            };
            taken |= bit;
        }
        __atomic_fetch_and(&area->free_buffers, ~taken, __ATOMIC_ACQ_REL);
        unsigned posted = area->isolated ? kernel_igb_zc_post(n->igb, batch, used) :
            kernel_igb_rx_post(n->igb, batch, used);
        count += posted;
        if (posted < used) {
            for (unsigned i = posted; i < used; i++) {
                struct network_igb_buffer *slot = batch[i].owner;
                igb_return_buffer(slot);
            }
            break;
        }
        available &= ~taken;
    }
    return count;
}

static struct netif *igb_route_input(struct network_igb_area *area,
    const uint8_t *first, unsigned bytes)
{
    struct kernel_network *n = area->network;
    if (area->isolated) return &area->secondary;
    /* Broadcast stays on the main hardware queue. Route ARP control for the
     * configured secondary IP without lending its bytes to secondary DMA. */
    struct netif *secondary = &n->igb_zc->secondary;
    if (n->igb_zc->provider && netif_is_up(secondary) && bytes >= 42 &&
        first[12] == 8 && first[13] == 6 && first[14] == 0 && first[15] == 1 &&
        first[16] == 8 && first[17] == 0 && first[18] == 6 && first[19] == 4 &&
        netif_ip4_addr(secondary)->addr &&
        !memcmp(first + 38, &netif_ip4_addr(secondary)->addr, 4)) return secondary;
    return &n->interface;
}

static unsigned igb_receive_inline(struct network_igb_area *area, unsigned budget)
{
    struct kernel_network *n = area->network;
    unsigned received = 0;
    struct kernel_igb_rx_packet packet;
    while (received < budget && (area->isolated ? kernel_igb_zc_reap(n->igb, &packet, 1) :
           kernel_igb_rx_reap(n->igb, &packet, 1))) {
        received++;
        struct network_igb_buffer *slot = packet.owner;
        unsigned full = packet.header_bytes + packet.payload_bytes;
        unsigned first_bytes = packet.header_bytes ? packet.header_bytes : packet.payload_bytes;
        uint8_t *body = slot->loan_ops ? slot->loan.address : slot->data;
        const uint8_t *first = packet.header_bytes ? slot->head : body;
        unsigned loans = __atomic_load_n(&area->loans, __ATOMIC_RELAXED);
        /* The provider can modify its mapping. Only IPv4 TCP with the
         * complete protocol header in kernel DMA storage can attach the loan
         * directly. For other protocols parse an immutable kernel snapshot. */
        int provider_tcp = slot->loan_ops && packet.split && packet.header_bytes >= 54 &&
            first[12] == 8 && first[13] == 0 && first[14] == 0x45 &&
            !(first[20] & 0x3f) && !first[21] && first[23] == 6 &&
            (first[46] >> 4) >= 5 && 34U + 4U * (first[46] >> 4) <= first_bytes;
        int lend = slot->loan_ops ? provider_tcp : loans < KERNEL_IGB_RX_DEPTH;
        unsigned pool = lend ? 0 : (full + PBUF_POOL_BUFSIZE - 1) / PBUF_POOL_BUFSIZE;
        if (packet.error || first_bytes < 14 ||
            (budgeted_frame(first, first_bytes) && !udp_capacity_after(loans, lend, pool))) {
            igb_return_buffer(slot);
            continue;
        }
        struct netif *interface = igb_route_input(area, first, first_bytes);
        if (!lend) {
            struct pbuf *copy = pbuf_alloc(PBUF_RAW, full, PBUF_POOL);
            if (copy && ((packet.header_bytes &&
                         pbuf_take_at(copy, slot->head, packet.header_bytes, 0) != ERR_OK) ||
                        (packet.payload_bytes &&
                         pbuf_take_at(copy, body, packet.payload_bytes,
                             packet.header_bytes) != ERR_OK))) __builtin_trap();
            igb_return_buffer(slot);
            if (copy) {
                kernel_socket_packet_ethernet(interface, copy, NULL, full, 0);
                deliver_pbuf(interface, copy);
            }
            continue;
        }
        slot->references = !!packet.header_bytes + !!packet.payload_bytes;
        __atomic_add_fetch(&area->loans, 1, __ATOMIC_RELAXED);
        struct pbuf *head = NULL, *payload = NULL;
        if (packet.header_bytes) {
            slot->header.custom_free_function = igb_free_header;
            head = pbuf_alloced_custom(PBUF_RAW, packet.header_bytes, PBUF_REF,
                &slot->header, slot->head, KERNEL_IGB_RX_HEADER);
            if (!head) __builtin_trap();
        }
        if (packet.payload_bytes) {
            slot->payload.custom_free_function = igb_free_payload;
            payload = pbuf_alloced_custom(PBUF_RAW, packet.payload_bytes, PBUF_REF,
                &slot->payload, body, KERNEL_IGB_RX_PAYLOAD);
            if (!payload) __builtin_trap();
        }
        if (head && payload) pbuf_cat(head, payload);
        struct pbuf *p = head ? head : payload;
        kernel_socket_packet_ethernet(interface, p, NULL, full, 0);
        deliver_pbuf(interface, p);
    }
    igb_refill(area);
    return received;
}

static void input_frame_owned(struct kernel_network *n, const struct virtio_net_frame *frame)
{
    const unsigned char *data = frame->data;
    kernel_socket_packet_ethernet(&n->interface,NULL,data,(uint16_t)frame->size,0);
    int budgeted = budgeted_frame(data, frame->size);
    int address_ok = frame->size >= 14 &&
        ((!memcmp(data, n->device->mac, 6)) || (data[0] & 1));
    if (!address_ok || (data[12]==0x86 && data[13]==0xdd && n->interface.mtu<1280))
        goto dropped;
    if (budgeted) {
        /* Only this receive scope can add loans. Concurrent returns can
         * make this budget estimate conservative and need no device lock. */
        unsigned loaned = virtio_net_loan_count(n->device);
        unsigned loans = loaned < 32 ? 1 : 0;
        unsigned pool = loans ? 0 : (frame->size + PBUF_POOL_BUFSIZE - 1) / PBUF_POOL_BUFSIZE;
        if (!udp_capacity_after(loaned, loans, pool)) goto dropped;
    }
    struct pbuf *p;
    int lent=!virtio_net_lend(n->device,frame->buffer);
    int borrowed=!lent && budgeted && udp_frame_can_borrow(n,frame);
    if (lent || borrowed) {
        struct network_rx *rx = &n->rx[frame->buffer];
        rx->device = n->device; rx->buffer = frame->buffer; rx->custom.custom_free_function = rx_free;
        p = pbuf_alloced_custom(PBUF_RAW, (u16_t)frame->size, PBUF_REF, &rx->custom,
                               frame->data, 2048 - 16);
        if (!p) __builtin_trap();
        p->boaros_udp_rx_borrowed=borrowed;
    } else {
        p = pbuf_alloc(PBUF_RAW, (u16_t)frame->size, PBUF_POOL);
        if (!p) goto dropped;
        if (pbuf_take(p, frame->data, frame->size) != ERR_OK) __builtin_trap();
        n->device->statistics.copied_packets++; n->device->statistics.copied_bytes += frame->size;
        virtio_net_release(n->device, frame->buffer);
    }
#if BOAROS_USER_SMP
    if (frame->more_ready) p->flags |= PBUF_FLAG_BOAROS_RX_BACKLOG;
#endif
    deliver_pbuf(&n->interface, p);
    return;
dropped:
    n->device->statistics.drops++;
    virtio_net_release(n->device, frame->buffer);
}
/* A caller with the network protocol scope may consume the receive FIFO. */
static unsigned receive_inline(void *context)
{
    struct kernel_network *n = context;
    if (n->igb) {
        struct network_igb_area *first = n->igb_area, *second = n->igb_zc;
        if (!second->provider) return igb_receive_inline(first, KERNEL_NETWORK_RECEIVE_BATCH);
        if (n->igb_zc_first) { first = n->igb_zc; second = n->igb_area; }
        n->igb_zc_first = !n->igb_zc_first;
        unsigned received = igb_receive_inline(first, KERNEL_NETWORK_RECEIVE_BATCH);
        return received + igb_receive_inline(second, KERNEL_NETWORK_RECEIVE_BATCH - received);
    }
    struct virtio_net_frame frames[KERNEL_NETWORK_RECEIVE_BATCH];
    unsigned count = 0;
    int serial = !boaros_lwip_parallel_enabled();
    int batching = 0;
    while (count < KERNEL_NETWORK_RECEIVE_BATCH) {
        unsigned ready = virtio_net_receive_batch(n->device, frames,
            KERNEL_NETWORK_RECEIVE_BATCH - count);
        if (!ready) break;
        if (serial && !batching && frames[0].more_ready) {
            boaros_lwip_tcp_ack_batch_begin();
            batching = 1;
        }
        for (unsigned i = 0; i < ready; i++) {
            const struct virtio_net_frame *frame = &frames[i];
            const unsigned char *data = frame->data;
            /* Finish pending TCP work before any other link/network traffic.
             * TCP checks the tuple, original headers and sequence on admission. */
            if (batching && (frame->size < 54 || data[12] != 8 || data[13] || data[14] != 0x45 ||
                             (data[20] & 0x3f) || data[21] ||
                             data[23] != 6 || data[46] != 0x50 || (data[47] & ~8U)!=0x10))
                boaros_lwip_tcp_ack_batch_flush();
            input_frame_owned(n, frame);
        }
        count += ready;
        if (batching && !frames[ready-1].more_ready) {
            boaros_lwip_tcp_ack_batch_end();
            batching = 0;
        }
    }
    if (batching) boaros_lwip_tcp_ack_batch_end();
    return count;
}

static void device_failed(struct kernel_network *n)
{
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    if (!n->failure_delivered) {
        n->failure_delivered = 1;
        kernel_socket_network_failed(ip4_addr_get_u32(netif_ip4_addr(&n->interface)));
    }
    netif_set_link_down(&n->interface);
    kernel_socket_protocol_leave(&irq);
    /* 失败不等于DMA停止：设备仍可能读取已投递的TX描述符与payload，
     * 在途owner保留到stop()复位确认，与RX借用同一策略；已完成owner
     * 仍由worker的release(0)归还。 */
}
static void worker(void *context)
{
    struct kernel_network *n = context;
    struct virtio_net_device *d = n->device;
    while (!stopping(n)) {
        kernel_socket_rights_service();
        int failed = virtio_net_poll_service(d);
        virtio_net_tx_release(d, kernel_socket_pbuf_free_batch, 0);
        uintptr_t irq = arch_interrupt_save();
        struct kernel_socket_protocol_scope scope; kernel_socket_protocol_enter(&scope);
        struct virtio_net_progress state = virtio_net_get_progress(d);
        if (n->capacity_generation != state.tx_capacity_generation) {
            n->capacity_generation = state.tx_capacity_generation;
            kernel_socket_network_capacity();
        }
        if (failed || state.failed) {
            device_failed(n);
        } else {
            if (state.link_up) netif_set_link_up(&n->interface); else netif_set_link_down(&n->interface);
            receive_inline(n);
        }
        struct kernel_socket_service_result service = kernel_socket_service_owned(&scope,
            (struct kernel_socket_service_budget){8, 8, 1});
        if (virtio_net_service(d)) device_failed(n);
        /* Release completed payload references outside the protocol scope. */
        wait_progress(n, &scope, service, n->capacity_generation);
        arch_interrupt_restore(irq);
    }
}

static void igb_worker(void *context)
{
    struct kernel_network *n = context;
    while (!stopping(n)) {
        kernel_socket_rights_service();
        uintptr_t irq = arch_interrupt_save();
        struct kernel_socket_protocol_scope scope;
        kernel_socket_protocol_enter(&scope);
        if (kernel_igb_tx_reap(n->igb)) kernel_socket_network_capacity();
        if (kernel_igb_rx_link_up(n->igb)) {
            netif_set_link_up(&n->interface);
            if (n->igb_zc->provider) netif_set_link_up(&n->igb_zc->secondary);
        } else {
            netif_set_link_down(&n->interface);
            netif_set_link_down(&n->igb_zc->secondary);
        }
        receive_inline(n);
        struct kernel_socket_service_result service = kernel_socket_service_owned(&scope,
            (struct kernel_socket_service_budget){8, 8, 1});
        wait_progress(n, &scope, service, 0);
        arch_interrupt_restore(irq);
    }
}

static void igb_release_area(struct network_igb_area *area)
{
    struct kernel_network *n = area->network;
    if (area->physical && physical_page_release_order(n->heap->page_allocator,
        area->physical, area->order)) __builtin_trap();
    if (area->header_physical && physical_page_release_order(n->heap->page_allocator,
        area->header_physical, area->header_order)) __builtin_trap();
    kernel_heap_release(n->heap, area);
}

static int igb_prepare_area(struct kernel_network *n, struct network_igb_area **result, bool isolated)
{
    struct physical_page_allocator *allocator = n->heap->page_allocator;
    struct network_igb_area *area;
    if (kernel_heap_allocate_zeroed(n->heap, 1, sizeof(*area), (void **)&area))
        return -KERNEL_ENOMEM;
    area->network = n;
    area->isolated = isolated;
    while (BOAROS_PAGE_SIZE << area->order < IGB_PORT_BUFFERS * KERNEL_IGB_RX_PAYLOAD)
        area->order++;
    while (BOAROS_PAGE_SIZE << area->header_order < IGB_PORT_BUFFERS * KERNEL_IGB_RX_HEADER)
        area->header_order++;
    int error = -KERNEL_ENOMEM;
    if (physical_page_allocate_order(allocator, area->order, &area->physical) ||
        physical_page_allocate_order(allocator, area->header_order, &area->header_physical))
        goto fail;
    void *headers, *payload;
    error = -KERNEL_EIO;
    if (physical_page_map_owned(allocator, area->physical, &payload) ||
        physical_page_map_owned(allocator, area->header_physical, &headers))
        goto fail;
    for (unsigned i = 0; i < IGB_PORT_BUFFERS; i++) {
        struct network_igb_buffer *slot = &area->buffers[i];
        unsigned head = i * KERNEL_IGB_RX_HEADER;
        unsigned data = i * KERNEL_IGB_RX_PAYLOAD;
        slot->area = area;
        slot->head = (uint8_t *)headers + head;
        slot->data = (uint8_t *)payload + data;
        slot->physical = area->physical + data;
        slot->header_physical = area->header_physical + head;
    }
    area->free_buffers = UINT64_MAX;
    *result = area;
    return 0;
fail:
    igb_release_area(area);
    return error;
}

static int network_start(struct kernel_network **owner, struct kernel_heap *heap,
    struct virtio_net_device *device, struct kernel_igb_rx *igb, uint64_t frequency)
{
    if (!owner || *owner || !heap || !frequency || frequency > UINT64_MAX / 5) return -KERNEL_EINVAL;
    if (device) {
        struct virtio_net_progress state = virtio_net_get_progress(device);
        if (!state.configured || state.stopping || state.failed) return -KERNEL_EINVAL;
    }
    struct kernel_network *n = 0;
    enum kernel_heap_status allocation = kernel_heap_allocate_zeroed(heap, 1, sizeof(*n), (void **)&n);
    if (allocation == KERNEL_HEAP_STATUS_EMPTY) return -KERNEL_ENOMEM;
    if (allocation != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    n->heap = heap; n->device = device; n->igb = igb; n->frequency = frequency;
    if (igb) {
        int error = igb_prepare_area(n, &n->igb_area, false);
        if (!error) error = igb_prepare_area(n, &n->igb_zc, true);
        if (error) {
            if (n->igb_area) igb_release_area(n->igb_area);
            kernel_heap_release(heap, n);
            return error;
        }
    }
    *owner = n;
    kernel_wait_queue_init(&n->progress);
    struct kernel_socket_protocol_scope saved; kernel_socket_protocol_enter(&saved);
    kernel_socket_network_initialize();
    if (device || igb) {
        ip4_addr_t address = {lwip_htonl(BOAROS_NET_IPV4)}, mask = {lwip_htonl(BOAROS_NET_NETMASK)}, gateway = {0};
        if (!netif_add(&n->interface, &address, &mask, &gateway, n, interface_init, ethernet_input)) {
            kernel_socket_protocol_leave(&saved); (void)kernel_network_stop(owner); return -KERNEL_ENOMEM;
        }
        n->attached = 1;
        /* DAD publishes the MAC-derived address only after neighbor probing. */
        netif_create_ip6_linklocal_address(&n->interface, 1);
        netif_set_up(&n->interface);
        if (igb ? kernel_igb_rx_link_up(igb) : virtio_net_get_progress(device).link_up)
            netif_set_link_up(&n->interface);
        netif_set_default(&n->interface);
        if (igb) {
            struct netif *secondary = &n->igb_zc->secondary;
            const ip4_addr_t empty = {0};
            if (!netif_add(secondary, &empty, &empty, &empty, n,
                igb_isolated_interface_init, ethernet_input)) {
                kernel_socket_protocol_leave(&saved);
                (void)kernel_network_stop(owner);
                return -KERNEL_ENOMEM;
            }
            n->igb_zc_attached = true;
            igb_refill(n->igb_area);
        }
    }
    if (kernel_thread_create_joinable(igb ? igb_worker : device ? worker : timer_worker,
        n, &n->worker) != KERNEL_SCHEDULER_STATUS_OK) {
        kernel_socket_protocol_leave(&saved); (void)kernel_network_stop(owner); return -KERNEL_ENOMEM;
    }
    kernel_socket_network_hooks(timer_wake, progress(n), n->attached ? udp_capacity : 0,
                                n->attached ? receive_inline : 0, n);
    kernel_socket_protocol_leave(&saved);
    if (igb) {
        print_text("BoarOS: eth0 ready transport=igb RX/TX=32 IRQ=");
        print_u64(kernel_igb_irq_available(igb));
        print_text("\n");
    } else if (device) {
        print_text("BoarOS: eth0 ready transport="); print_text(device->version == 1 ? "legacy" : "modern");
        print_text(" irq="); print_u64(device->irq_source); print_text(" MTU=1500 RX/TX=32 DMA-bytes=");
        print_u64(((uint64_t)BOAROS_PAGE_SIZE << device->queue_order) + 2 * 131072U);
        print_text(" control-bytes="); print_u64(sizeof(*n)); print_text("\n");
    }
    return 0;
}

int kernel_network_start(struct kernel_network **owner, struct kernel_heap *heap,
    struct virtio_net_device *device, uint64_t frequency)
{ return network_start(owner, heap, device, NULL, frequency); }

int kernel_network_igb_start(struct kernel_network **owner, struct kernel_heap *heap,
    struct kernel_igb_rx *device, uint64_t frequency)
{ return network_start(owner, heap, NULL, device, frequency); }

static struct network_igb_area *igb_zc_by_index(uint32_t ifindex)
{
    if (ifindex > UINT8_MAX) return NULL;
    struct netif *interface = netif_get_by_index(ifindex);
    if (!interface || interface->linkoutput != link_output) return NULL;
    struct kernel_network *n = interface->state;
    if (!n->igb_zc || interface != &n->igb_zc->secondary || stopping(n)) return NULL;
    return n->igb_zc;
}

int kernel_network_igb_zc_attach(uint32_t ifindex, unsigned queue,
    const struct kernel_network_igb_zc_ops *ops, void *provider)
{
    if (queue != 1 || !ops || !ops->take || !ops->put || !provider) return -KERNEL_EINVAL;
    struct kernel_socket_protocol_scope scope;
    kernel_socket_protocol_enter(&scope);
    struct network_igb_area *area = igb_zc_by_index(ifindex);
    int error = -KERNEL_ENODEV;
    if (area) {
        error = area->provider ? -KERNEL_EBUSY :
            kernel_igb_zc_bind(area->network->igb, area->secondary.hwaddr);
        if (!error) {
            area->ops = ops;
            area->provider = provider;
            if (ops->set_attached) ops->set_attached(provider,1);
            igb_refill(area);
            if (kernel_igb_rx_link_up(area->network->igb))
                netif_set_link_up(&area->secondary);
            (void)kernel_wait_queue_wake_all(progress(area->network));
        }
    }
    kernel_socket_protocol_leave(&scope);
    return error;
}

static void igb_drain_area(struct network_igb_area *area)
{
    struct kernel_igb_rx *dev = area->network->igb;
    struct kernel_igb_rx_packet packet;
    while (area->isolated ? kernel_igb_zc_reap(dev, &packet, 1) :
        kernel_igb_rx_reap(dev, &packet, 1)) igb_return_buffer(packet.owner);
}

int kernel_network_igb_zc_detach(uint32_t ifindex, void *provider)
{
    struct kernel_socket_protocol_scope scope;
    kernel_socket_protocol_enter(&scope);
    struct network_igb_area *area = igb_zc_by_index(ifindex);
    int error = -KERNEL_ENODEV;
    if (area && area->provider == provider) {
        error = kernel_igb_zc_stop(area->network->igb);
        if (!error) {
            const struct kernel_network_igb_zc_ops *ops = area->ops;
            netif_set_link_down(&area->secondary);
            area->ops = NULL;
            area->provider = NULL;
            igb_drain_area(area);
            if (ops->set_attached) ops->set_attached(provider,0);
        }
    }
    kernel_socket_protocol_leave(&scope);
    return error;
}

void kernel_network_igb_zc_kick(uint32_t ifindex, void *provider)
{
    struct kernel_socket_protocol_scope scope;
    kernel_socket_protocol_enter(&scope);
    struct network_igb_area *area = igb_zc_by_index(ifindex);
    if (area && area->provider == provider) {
        igb_refill(area);
        (void)kernel_wait_queue_wake_all(progress(area->network));
    }
    kernel_socket_protocol_leave(&scope);
}

int kernel_network_stop(struct kernel_network **owner)
{
    if (!owner || !*owner) return 0;
    struct kernel_network *n = *owner;
    int rights=kernel_socket_rights_drain();
    if (rights) return rights;
    uintptr_t irq = arch_interrupt_save();
    int first_stop = !n->stopping;
    if (n->device) virtio_net_begin_stop(n->device);
    (void)kernel_wait_queue_wake_all(progress(n));
    kernel_assert_can_block();
    arch_interrupt_restore(ARCH_INTERRUPT_ENABLE_MASK);
    /* Close queued admission and finish pinned callers before joining the worker. */
    if (first_stop) kernel_socket_network_hooks(0, 0, 0, 0, 0);
    uintptr_t stopped_irq=arch_interrupt_save();
    n->stopping=1;(void)kernel_wait_queue_wake_all(progress(n));
    arch_interrupt_restore(stopped_irq);
    if (n->worker.task) kernel_thread_join(&n->worker);
#if BOAROS_USER_SMP
    kernel_socket_stop_tcp_owners();
#endif
    rights=kernel_socket_rights_drain();
    if (rights) {arch_interrupt_restore(irq);return rights;}
    if (n->device && virtio_net_quiesce(n->device)) { arch_interrupt_restore(irq); return -KERNEL_EIO; }
    if (n->igb) {
        int error = kernel_igb_rx_stop(n->igb);
        if (error) { arch_interrupt_restore(irq); return error; }
    }
    kernel_socket_network_purge_orphans();
    if (n->attached) {
        struct kernel_socket_protocol_scope core; kernel_socket_protocol_enter(&core);
        ip4_reass_cleanup_netif(&n->interface);
        etharp_cleanup_netif(&n->interface);
        kernel_socket_packet_detach(&n->interface);
        netif_set_down(&n->interface); netif_remove(&n->interface); n->attached = 0;
        if (n->igb_zc_attached) {
            struct netif *secondary = &n->igb_zc->secondary;
            ip4_reass_cleanup_netif(secondary);
            etharp_cleanup_netif(secondary);
            kernel_socket_packet_detach(secondary);
            netif_set_down(secondary);
            netif_remove(secondary);
            n->igb_zc_attached = false;
        }
        kernel_socket_protocol_leave(&core);
    }
    /* reset已确认DMA停止；worker已join，归还剩余TX owner。 */
    if (n->device) {
        virtio_net_tx_release(n->device, kernel_socket_pbuf_free_batch, 1);
        if (virtio_net_get_progress(n->device).loaned) { arch_interrupt_restore(irq); return -KERNEL_EBUSY; }
    }
    if (n->igb) {
        if (__atomic_load_n(&n->igb_area->loans, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&n->igb_zc->loans, __ATOMIC_ACQUIRE)) {
            arch_interrupt_restore(irq);
            return -KERNEL_EBUSY;
        }
        igb_drain_area(n->igb_area);
        igb_drain_area(n->igb_zc);
        igb_release_area(n->igb_area);
        igb_release_area(n->igb_zc);
    }
    if (n->device) {
        print_text("BoarOS: network final rx="); print_u64(n->device->statistics.rx_packets);
        print_text(" tx="); print_u64(n->device->statistics.tx_packets);
        print_text(" loan-packets="); print_u64(n->device->statistics.loan_packets);
        print_text(" loan-bytes="); print_u64(n->device->statistics.loan_bytes);
        print_text(" loan-peak="); print_u64(n->device->statistics.loan_peak);
        print_text(" copy-packets="); print_u64(n->device->statistics.copied_packets);
        print_text(" copy-bytes="); print_u64(n->device->statistics.copied_bytes);
        print_text(" tx-sg="); print_u64(n->device->statistics.tx_sg_packets);
        print_text(" tx-copy="); print_u64(n->device->statistics.tx_copy_packets);
        print_text(" drops="); print_u64(n->device->statistics.drops);
        print_text(" errors="); print_u64(n->device->statistics.errors);
#if BOAROS_COST_DIAGNOSTICS
        print_text(" tx-clock-hz="); print_u64(n->device->frequency);
        print_text(" tx-done-free-count="); print_u64(n->device->statistics.tx_done_free_count);
        print_text(" tx-done-free-ticks="); print_u64(n->device->statistics.tx_done_free_ticks);
        print_text(" tx-done-free-max="); print_u64(n->device->statistics.tx_done_free_max);
        print_text(" tx-free-post-count="); print_u64(n->device->statistics.tx_free_post_count);
        print_text(" tx-free-post-ticks="); print_u64(n->device->statistics.tx_free_post_ticks);
        print_text(" tx-free-post-max="); print_u64(n->device->statistics.tx_free_post_max);
        print_text(" tx-latency-overflow="); print_u64(n->device->statistics.tx_latency_overflow);
#endif
    }
    struct kernel_heap_statistics heap_statistics;
    kernel_heap_get_statistics(n->heap, &heap_statistics);
    print_text(" root-heap-peak-pages="); print_u64(heap_statistics.peak_pages); print_text("\n");
    kernel_heap_release(n->heap, n);
    *owner = 0; arch_interrupt_restore(irq); return 0;
}
