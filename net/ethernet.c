#include <kernel/network.h>
#include <kernel/socket.h>
#include <kernel/socket_control.h>
#include <kernel/errno.h>
#include <kernel/cost.h>
#include <kernel/console.h>
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
#include "netif/ethernet.h"
#include <stddef.h>
#include <string.h>

struct network_rx {
    struct pbuf_custom custom;
    struct virtio_net_device *device;
    unsigned buffer;
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
static void release_owner(void *owner)
{
    kernel_socket_pbuf_free(owner);
}
static err_t submit_link_frame(struct netif *interface, struct pbuf *p)
{
    COST_SCOPE(output_cost, NET_TX_OUTPUT_TICKS);
    struct kernel_network *n = interface->state;
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
            int result = virtio_net_send_segments(n->device, segments,
                                                             count, p);
            if (result == 0) return ERR_OK;
            pbuf_free(p);
            if (result == -KERNEL_EAGAIN) { kernel_socket_network_blocked(); return ERR_MEM; }
            if (result != -KERNEL_ENOTSUP) return ERR_IF;
            /* 未协商 indirect：回退复制路径。 */
        }
    }
    int result = virtio_net_send_copy(n->device, p->tot_len, copy_pbuf, p);
    /* 复制回退耗尽的仍是 NIC 槽；只等协议池归还会漏掉真实 TX 完成。 */
    if (result == -KERNEL_EAGAIN) { kernel_socket_network_blocked(); return ERR_MEM; }
    return result >= 0 ? ERR_OK : result == -KERNEL_EMSGSIZE ? ERR_BUF : ERR_IF;
}
static err_t output_link_frame(struct netif *interface, struct pbuf *p)
{
    err_t result = submit_link_frame(interface, p);
    if (result == ERR_OK) kernel_socket_packet_ethernet(interface,p,NULL,p->tot_len,1);
    return result;
}
static err_t link_output(struct netif *interface, struct pbuf *p)
{
#if BOAROS_USER_SMP
    struct boaros_link_defer *defer = interface->boaros_link_defer;
    /* ARP probes, reinjected packets and fragments own other buffers and
     * still go through the synchronous link path. */
    if (defer && defer->packet == p && !defer->submit) {
        defer->submit = output_link_frame;
        return ERR_OK;
    }
#endif
    return output_link_frame(interface, p);
}
int kernel_network_send_frame(struct netif *interface, const void *frame, uint16_t size)
{
    /* The protocol owner keeps the Ethernet attachment and device alive. */
    if (interface->linkoutput != link_output) return -KERNEL_EOPNOTSUPP;
    struct kernel_network *n=interface->state;
    int result=virtio_net_send(n->device,frame,size);
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
    interface->hwaddr_len = 6; memcpy(interface->hwaddr, n->device->mac, 6);
    interface->mtu = 1500;
    interface->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET |
        NETIF_FLAG_IGMP | NETIF_FLAG_MLD6;
    interface->output = etharp_output; interface->output_ip6 = ipv6_output;
    interface->linkoutput = link_output;
    return ERR_OK;
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
    return udp_capacity_after(virtio_net_get_progress(n->device).loaned, 0, 0);
}
static struct kernel_wait_queue *progress(struct kernel_network *n)
{ return n->device ? &n->device->progress : &n->progress; }
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
    if (!n->device) return 0;
    struct virtio_net_progress state = virtio_net_get_progress(n->device);
    return generation != state.tx_capacity_generation || state.tx_done_count ||
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
        arch_time_read() + 5 * n->frequency;
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
static void input_frame(struct kernel_network *n, struct virtio_net_frame *frame)
{
    struct kernel_socket_protocol_scope irq; kernel_socket_protocol_enter(&irq);
    const unsigned char *data = frame->data;
    kernel_socket_packet_ethernet(&n->interface,NULL,data,(uint16_t)frame->size,0);
    int budgeted = (frame->size >= 34 && data[12] == 8 && data[13] == 0 && data[23] == 17) ||
        /* Extension-header traffic is conservatively budgeted too: it may
         * terminate in UDP. Direct TCP/ICMPv6 retain the control reserve. */
        (frame->size >= 54 && data[12] == 0x86 && data[13] == 0xdd &&
         data[20] != 6 && data[20] != 58);
    int address_ok = frame->size >= 14 &&
        ((!memcmp(data, n->device->mac, 6)) || (data[0] & 1));
    /* 预算检查包含即将持有的版本，不能先吃掉控制余量再等待完整重组。 */
    unsigned loaned = virtio_net_get_progress(n->device).loaned;
    unsigned loans = loaned < 32 ? 1 : 0;
    unsigned pool = loans ? 0 : (frame->size + PBUF_POOL_BUFSIZE - 1) / PBUF_POOL_BUFSIZE;
    if (!address_ok || (frame->size>=14 && data[12]==0x86 && data[13]==0xdd && n->interface.mtu<1280) ||
        (budgeted && !udp_capacity_after(loaned, loans, pool))) goto dropped;
    struct pbuf *p;
    if (!virtio_net_lend(n->device, frame->buffer)) {
        struct network_rx *rx = &n->rx[frame->buffer];
        rx->device = n->device; rx->buffer = frame->buffer; rx->custom.custom_free_function = rx_free;
        p = pbuf_alloced_custom(PBUF_RAW, (u16_t)frame->size, PBUF_REF, &rx->custom,
                               frame->data, 2048 - 16);
        if (!p) __builtin_trap();
    } else {
        p = pbuf_alloc(PBUF_RAW, (u16_t)frame->size, PBUF_POOL);
        if (!p) goto dropped;
        if (pbuf_take(p, frame->data, frame->size) != ERR_OK) __builtin_trap();
        n->device->statistics.copied_packets++; n->device->statistics.copied_bytes += frame->size;
        virtio_net_release(n->device, frame->buffer);
    }
    /* 成功input消耗或接纳pbuf；失败才由当前接收请求释放。 */
    if (n->interface.input(p, &n->interface) != ERR_OK) pbuf_free(p);
    kernel_socket_protocol_leave(&irq);
    return;
dropped:
    n->device->statistics.drops++;
    virtio_net_release(n->device, frame->buffer);
    kernel_socket_protocol_leave(&irq);
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
        unsigned handled = 0;
        int failed = virtio_net_service(d);
        virtio_net_tx_release(d, release_owner, 0);
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
            struct virtio_net_frame frame;
            while (handled < 8 && virtio_net_receive(d, &frame)) {
                input_frame(n, &frame); handled++;
            }
        }
        struct kernel_socket_service_result service = kernel_socket_service_owned(&scope,
            (struct kernel_socket_service_budget){8, 8, 1});
        if (virtio_net_service(d)) device_failed(n);
        /* 新TX_DONE由下轮开头在owner外归还；当前条件保证不会睡过完成。 */
        wait_progress(n, &scope, service, n->capacity_generation);
        arch_interrupt_restore(irq);
    }
}

int kernel_network_start(struct kernel_network **owner, struct kernel_heap *heap,
    struct virtio_net_device *device, uint64_t frequency)
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
    n->heap = heap; n->device = device; n->frequency = frequency; *owner = n;
    kernel_wait_queue_init(&n->progress);
    struct kernel_socket_protocol_scope saved; kernel_socket_protocol_enter(&saved);
    kernel_socket_network_initialize();
    if (device) {
        ip4_addr_t address = {lwip_htonl(BOAROS_NET_IPV4)}, mask = {lwip_htonl(BOAROS_NET_NETMASK)}, gateway = {0};
        if (!netif_add(&n->interface, &address, &mask, &gateway, n, interface_init, ethernet_input)) {
            kernel_socket_protocol_leave(&saved); (void)kernel_network_stop(owner); return -KERNEL_ENOMEM;
        }
        n->attached = 1;
        /* DAD publishes the MAC-derived address only after neighbor probing. */
        netif_create_ip6_linklocal_address(&n->interface, 1);
        netif_set_up(&n->interface);
        if (virtio_net_get_progress(device).link_up) netif_set_link_up(&n->interface);
        netif_set_default(&n->interface);
    }
    if (kernel_thread_create_joinable(device ? worker : timer_worker, n, &n->worker) != KERNEL_SCHEDULER_STATUS_OK) {
        kernel_socket_protocol_leave(&saved); (void)kernel_network_stop(owner); return -KERNEL_ENOMEM;
    }
    kernel_socket_network_hooks(timer_wake, progress(n), device ? udp_capacity : 0, n);
    kernel_socket_protocol_leave(&saved);
    if (device) {
        print_text("BoarOS: eth0 ready transport="); print_text(device->version == 1 ? "legacy" : "modern");
        print_text(" irq="); print_u64(device->irq_source); print_text(" MTU=1500 RX/TX=32 DMA-bytes=");
        print_u64(((uint64_t)BOAROS_PAGE_SIZE << device->queue_order) + 2 * 131072U);
        print_text(" control-bytes="); print_u64(sizeof(*n)); print_text("\n");
    }
    return 0;
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
    if (first_stop) kernel_socket_network_hooks(0, 0, 0, 0);
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
    kernel_socket_network_purge_orphans();
    if (n->attached) {
        struct kernel_socket_protocol_scope core; kernel_socket_protocol_enter(&core);
        ip4_reass_cleanup_netif(&n->interface);
        etharp_cleanup_netif(&n->interface);
        kernel_socket_packet_detach(&n->interface);
        netif_set_down(&n->interface); netif_remove(&n->interface); n->attached = 0;
        kernel_socket_protocol_leave(&core);
    }
    /* reset已确认DMA停止；worker已join，归还剩余TX owner。 */
    if (n->device) {
        virtio_net_tx_release(n->device, release_owner, 1);
        if (virtio_net_get_progress(n->device).loaned) { arch_interrupt_restore(irq); return -KERNEL_EBUSY; }
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
