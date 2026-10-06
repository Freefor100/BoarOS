#include <kernel/network.h>
#include <kernel/socket.h>
#include <kernel/errno.h>
#include <kernel/cost.h>
#include <kernel/console.h>
#include <arch/context.h>
#include <arch/riscv/direct_map.h>
#include <arch/riscv/memory_layout.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/virtio_mmio_net.h>
#include "net-config.h"
#include "lwip/etharp.h"
#include "lwip/ip4_frag.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "netif/ethernet.h"
#include <stddef.h>
#include <string.h>

struct network_rx {
    struct pbuf_custom custom;
    struct riscv_virtio_mmio_net *device;
    unsigned buffer;
};
struct kernel_network {
    struct riscv_virtio_mmio_net device;
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
    riscv_virtio_mmio_net_release(r->device, r->buffer);
}
static int copy_pbuf(const void *context, void *destination, uint32_t length)
{ return pbuf_copy_partial(context, destination, (u16_t)length, 0) == length; }
static void release_owner(void *owner)
{
    uintptr_t irq = kernel_socket_protocol_enter();
    pbuf_free((struct pbuf *)owner);
    kernel_socket_protocol_leave(irq);
}
static err_t link_output(struct netif *interface, struct pbuf *p)
{
    struct kernel_network *n = interface->state;
    /* 零拷贝候选：整链为可 DMA 的 PBUF_RAM/POOL、段数受限且地址可换算。 */
    if (p->tot_len != 0 && p->tot_len <= 1514) {
        struct riscv_net_tx_segment segments[RISCV_NET_TX_SEGMENTS];
        unsigned count = 0;
        int eligible = 1;
        for (struct pbuf *q = p; q != 0; q = q->next) {
            uint64_t physical;
            if (count == RISCV_NET_TX_SEGMENTS || q->len == 0 ||
                PBUF_NEEDS_COPY(q) ||
                riscv_image_va_to_pa((uint64_t)(uintptr_t)q->payload, q->len,
                                     &physical) != RISCV_DIRECT_MAP_STATUS_OK) {
                eligible = 0; break;
            }
            segments[count].physical_address = physical;
            segments[count].length = q->len;
            count++;
        }
        if (eligible && count != 0) {
            pbuf_ref(p);
            int result = riscv_virtio_mmio_net_send_segments(&n->device, segments,
                                                             count, p);
            if (result == 0) return ERR_OK;
            pbuf_free(p);
            if (result == -KERNEL_EAGAIN) { kernel_socket_network_blocked(); return ERR_MEM; }
            if (result != -KERNEL_ENOTSUP) return ERR_IF;
            /* 未协商 indirect：回退复制路径。 */
        }
    }
    int result = riscv_virtio_mmio_net_send_copy(&n->device, p->tot_len, copy_pbuf, p);
    /* 复制回退耗尽的仍是 NIC 槽；只等协议池归还会漏掉真实 TX 完成。 */
    if (result == -KERNEL_EAGAIN) { kernel_socket_network_blocked(); return ERR_MEM; }
    return result >= 0 ? ERR_OK : result == -KERNEL_EMSGSIZE ? ERR_BUF : ERR_IF;
}
static err_t interface_init(struct netif *interface)
{
    struct kernel_network *n = interface->state;
    interface->name[0] = 'e'; interface->name[1] = 't';
    interface->hwaddr_len = 6; memcpy(interface->hwaddr, n->device.mac, 6);
    interface->mtu = 1500;
    interface->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    interface->output = etharp_output; interface->linkoutput = link_output;
    return ERR_OK;
}
static unsigned pool_used(void)
{ return lwip_stats.memp[MEMP_PBUF_POOL] ? lwip_stats.memp[MEMP_PBUF_POOL]->used : 0; }
static int udp_capacity_after(struct kernel_network *n, unsigned loans, unsigned pbufs)
{
    unsigned used = pool_used() + pbufs;
    uint64_t owned = lwip_stats.mem.used + ((uint64_t)n->device.loaned + loans) * 2048 +
        (uint64_t)used * PBUF_POOL_BUFSIZE;
    return used <= PBUF_POOL_SIZE - 16 && owned <= MEM_SIZE - (65536U + 4096U);
}
static int udp_capacity(void *context) { return udp_capacity_after(context, 0, 0); }
static void work_wake(void *context)
{
    struct kernel_network *network = context;
    (void)kernel_wait_queue_wake_all(&network->device.progress);
}
static void release_completed(struct kernel_network *network)
{
    struct riscv_virtio_mmio_net *device = &network->device;
    riscv_virtio_mmio_net_tx_release(device, release_owner, 0);
    if (network->capacity_generation != device->tx_capacity_generation) {
        network->capacity_generation = device->tx_capacity_generation;
        kernel_socket_network_capacity();
    }
}
static void timer_wake(void *context)
{
    struct kernel_network *n = context;
    if (n->deadline && (int64_t)(riscv_time_read() - n->deadline) >= 0)
        (void)kernel_wait_queue_wake_all(&n->device.progress);
}
/* 无 NIC 的 timer-only owner：IRQ 只唤醒它，协议定时器在 worker 上下文推进。 */
static void timer_worker(void *context)
{
    struct kernel_network *n = context;
    for (;;) {
        uintptr_t irq = arch_interrupt_save();
        if (n->device.stopping) { arch_interrupt_restore(irq); return; }
        struct kernel_socket_service_result service = kernel_socket_service_pending(
            (struct kernel_socket_service_budget){8, 8, 1});
        if (service.runnable || kernel_socket_work_pending()) {
            arch_interrupt_restore(irq);
            irq = arch_interrupt_save();
            if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
            arch_interrupt_restore(irq); continue;
        }
        n->deadline = service.next_deadline;
        uint64_t timeout = riscv_time_read() + 5 * n->device.frequency;
        if (!n->deadline || timeout < n->deadline) n->deadline = timeout;
        COST_ADD(NETWORK_RUNNABLE_SLEEP, kernel_socket_work_pending() != 0);
        enum kernel_wait_wake_reason reason;
        if (kernel_scheduler_block_current(&n->device.progress, n->deadline, 0, &reason) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        arch_interrupt_restore(irq);
    }
}
static void input_frame(struct kernel_network *n, struct riscv_net_frame *frame)
{
    uintptr_t irq = kernel_socket_protocol_enter();
    const unsigned char *data = frame->data;
    int udp = frame->size >= 34 && data[12] == 8 && data[13] == 0 && data[23] == 17;
    int address_ok = frame->size >= 14 &&
        ((!memcmp(data, n->device.mac, 6)) || (data[0] & 1));
    /* 预算检查包含即将持有的版本，不能先吃掉控制余量再等待完整重组。 */
    unsigned loans = n->device.loaned < 32 ? 1 : 0;
    unsigned pool = loans ? 0 : (frame->size + PBUF_POOL_BUFSIZE - 1) / PBUF_POOL_BUFSIZE;
    if (!address_ok || (udp && !udp_capacity_after(n, loans, pool))) goto dropped;
    struct pbuf *p;
    if (!riscv_virtio_mmio_net_lend(&n->device, frame->buffer)) {
        struct network_rx *rx = &n->rx[frame->buffer];
        rx->device = &n->device; rx->buffer = frame->buffer; rx->custom.custom_free_function = rx_free;
        p = pbuf_alloced_custom(PBUF_RAW, (u16_t)frame->size, PBUF_REF, &rx->custom,
                               frame->data, 2048 - 16);
        if (!p) __builtin_trap();
    } else {
        p = pbuf_alloc(PBUF_RAW, (u16_t)frame->size, PBUF_POOL);
        if (!p) goto dropped;
        if (pbuf_take(p, frame->data, frame->size) != ERR_OK) __builtin_trap();
        n->device.statistics.copied_packets++; n->device.statistics.copied_bytes += frame->size;
        riscv_virtio_mmio_net_release(&n->device, frame->buffer);
    }
    /* 成功input消耗或接纳pbuf；失败才由当前接收请求释放。 */
    if (n->interface.input(p, &n->interface) != ERR_OK) pbuf_free(p);
    kernel_socket_protocol_leave(irq);
    return;
dropped:
    n->device.statistics.drops++;
    riscv_virtio_mmio_net_release(&n->device, frame->buffer);
    kernel_socket_protocol_leave(irq);
}
static void device_failed(struct kernel_network *n)
{
    uintptr_t irq = kernel_socket_protocol_enter();
    if (!n->failure_delivered) {
        n->failure_delivered = 1;
        kernel_socket_network_failed(ip4_addr_get_u32(netif_ip4_addr(&n->interface)));
    }
    netif_set_link_down(&n->interface);
    kernel_socket_protocol_leave(irq);
    /* 失败不等于DMA停止：设备仍可能读取已投递的TX描述符与payload，
     * 在途owner保留到stop()复位确认，与RX借用同一策略；已完成owner
     * 仍由worker的release(0)归还。 */
}
static void worker(void *context)
{
    struct kernel_network *n = context;
    struct riscv_virtio_mmio_net *d = &n->device;
    for (;;) {
        uintptr_t irq = arch_interrupt_save();
        if (d->stopping) { arch_interrupt_restore(irq); return; }
        unsigned handled = 0;
        int failed = riscv_virtio_mmio_net_service(d);
        /* RX和协议回调都可能发送；先归还完成owner及其槽位。 */
        release_completed(n);
        uint64_t capacity_generation = d->tx_capacity_generation;
        if (failed) {
            device_failed(n);
        } else {
            uintptr_t core = kernel_socket_protocol_enter();
            if (d->link_up) netif_set_link_up(&n->interface); else netif_set_link_down(&n->interface);
            kernel_socket_protocol_leave(core);
            struct riscv_net_frame frame;
            while (!d->failed && handled < 8 && riscv_virtio_mmio_net_receive(d, &frame)) {
                input_frame(n, &frame); handled++;
            }
        }
        /* 失败NIC不再收发，但共享的loopback与协议期限仍须独立进展。 */
        struct kernel_socket_service_result service = kernel_socket_service_pending(
            (struct kernel_socket_service_budget){8, 8, 1});
        if (riscv_virtio_mmio_net_service(d)) device_failed(n);
        release_completed(n);
        /* 最后一次收割也会归还复制路径的容量，不能只检查SG done链。 */
        if (service.runnable || kernel_socket_work_pending() || capacity_generation != d->tx_capacity_generation || (!d->failed && d->ready_count)) {
            /* 批次间给中断和其他任务机会，不持raw调用栈睡在设备credit上。 */
            arch_interrupt_restore(irq);
            irq = arch_interrupt_save();
            if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
            arch_interrupt_restore(irq); continue;
        }
        n->deadline = service.next_deadline;
        uint64_t timeout = riscv_time_read() + 5 * d->frequency;
        if (!n->deadline || timeout < n->deadline) n->deadline = timeout;
        COST_ADD(NETWORK_RUNNABLE_SLEEP, kernel_socket_work_pending() != 0);
        enum kernel_wait_wake_reason reason;
        if (kernel_scheduler_block_current(&d->progress, n->deadline, 0, &reason) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        arch_interrupt_restore(irq);
    }
}
int kernel_network_start(struct kernel_network **owner, struct kernel_heap *heap,
    const struct dtb_boot_info *boot, const struct dtb_irq_info *irq)
{
    if (!owner || *owner || !heap || !boot || !irq) return -KERNEL_EINVAL;
    for (unsigned i = 0; i < boot->virtio_mmio_count; i++) {
        volatile uint32_t *mmio = (void *)(uintptr_t)(RISCV_KERNEL_MMIO_BASE + boot->virtio_mmio[i].base);
        if (boot->virtio_mmio[i].size < 0x108 || mmio[0] != 0x74726976 || mmio[2] != 1) continue;
        uint32_t source = 0;
        for (unsigned j = 0; j < irq->route_count; j++) if (irq->routes[j].base == boot->virtio_mmio[i].base) source = irq->routes[j].source;
        if (!source) return -KERNEL_EIO;
        struct kernel_network *n = 0;
        enum kernel_heap_status allocation = kernel_heap_allocate_zeroed(heap, 1, sizeof(*n), (void **)&n);
        if (allocation == KERNEL_HEAP_STATUS_EMPTY) return -KERNEL_ENOMEM;
        if (allocation != KERNEL_HEAP_STATUS_OK) __builtin_trap();
        n->heap = heap; *owner = n;
        int error = riscv_virtio_mmio_net_init(&n->device, mmio, boot->virtio_mmio[i].size,
            heap->page_allocator, boot->timebase_frequency, source);
        if (error) { if (kernel_network_stop(owner)) return -KERNEL_EIO; return error; }
        uintptr_t saved = kernel_socket_protocol_enter();
        kernel_socket_network_initialize();
        ip4_addr_t address = {lwip_htonl(BOAROS_NET_IPV4)}, mask = {lwip_htonl(BOAROS_NET_NETMASK)}, gateway = {0};
        if (!netif_add(&n->interface, &address, &mask, &gateway, n, interface_init, ethernet_input)) {
            kernel_socket_protocol_leave(saved); (void)kernel_network_stop(owner); return -KERNEL_ENOMEM;
        }
        n->attached = 1;
        netif_set_up(&n->interface); if (n->device.link_up) netif_set_link_up(&n->interface);
        netif_set_default(&n->interface);
        if (kernel_thread_create_joinable(worker, n, &n->device.worker) != KERNEL_SCHEDULER_STATUS_OK) {
            kernel_socket_protocol_leave(saved); (void)kernel_network_stop(owner); return -KERNEL_ENOMEM;
        }
        kernel_socket_network_hooks(timer_wake, work_wake, udp_capacity, n);
        kernel_socket_protocol_leave(saved);
        print_text("BoarOS: eth0 ready transport="); print_text(n->device.version == 1 ? "legacy" : "modern");
        print_text(" irq="); print_u64(source); print_text(" MTU=1500 RX/TX=32 DMA-bytes=");
        print_u64((4096U << n->device.queue_order) + 2 * 131072U);
        print_text(" control-bytes="); print_u64(sizeof(*n)); print_text("\n");
        return 0;
    }
    /* 无 NIC 仍保留 timer-only owner：最后的 OFD 定时回收与 loopback 期限
     * 不再依赖另一个用户 syscall，也不在 IRQ 里跑协议回调。 */
    struct kernel_network *n = 0;
    if (kernel_heap_allocate_zeroed(heap, 1, sizeof(*n), (void **)&n) !=
        KERNEL_HEAP_STATUS_OK)
        return -KERNEL_ENOMEM;
    n->heap = heap;
    n->device.frequency = boot->timebase_frequency;
    kernel_wait_queue_init(&n->device.progress);
    if (kernel_thread_create_joinable(timer_worker, n, &n->device.worker) !=
        KERNEL_SCHEDULER_STATUS_OK) {
        (void)kernel_heap_release(heap, n);
        return -KERNEL_EIO;
    }
    kernel_socket_network_hooks(timer_wake, work_wake, 0, n);
    *owner = n;
    return 0;
}
int kernel_network_stop(struct kernel_network **owner)
{
    if (!owner || !*owner) return 0;
    struct kernel_network *n = *owner;
    uintptr_t irq = arch_interrupt_save();
    n->device.stopping = 1;
    (void)kernel_wait_queue_wake_all(&n->device.progress);
    if (n->device.worker.task) kernel_thread_join(&n->device.worker);
    if (n->attached) {
        uintptr_t core = kernel_socket_protocol_enter();
        ip4_reass_cleanup_netif(&n->interface);
        etharp_cleanup_netif(&n->interface);
        netif_set_down(&n->interface); netif_remove(&n->interface); n->attached = 0;
        kernel_socket_protocol_leave(core);
    }
    int error = riscv_virtio_mmio_net_stop(&n->device);
    if (error) { arch_interrupt_restore(irq); return error; }
    /* reset已确认DMA停止；worker已join，归还剩余TX owner。 */
    riscv_virtio_mmio_net_tx_release(&n->device, release_owner, 1);
    kernel_socket_network_hooks(0, 0, 0, 0);
    print_text("BoarOS: network final rx="); print_u64(n->device.statistics.rx_packets);
    print_text(" tx="); print_u64(n->device.statistics.tx_packets);
    print_text(" loan-packets="); print_u64(n->device.statistics.loan_packets);
    print_text(" loan-bytes="); print_u64(n->device.statistics.loan_bytes);
    print_text(" loan-peak="); print_u64(n->device.statistics.loan_peak);
    print_text(" copy-packets="); print_u64(n->device.statistics.copied_packets);
    print_text(" copy-bytes="); print_u64(n->device.statistics.copied_bytes);
    print_text(" tx-sg="); print_u64(n->device.statistics.tx_sg_packets);
    print_text(" tx-copy="); print_u64(n->device.statistics.tx_copy_packets);
    print_text(" drops="); print_u64(n->device.statistics.drops);
    print_text(" errors="); print_u64(n->device.statistics.errors);
#if BOAROS_COST_DIAGNOSTICS
    print_text(" tx-clock-hz="); print_u64(n->device.frequency);
    print_text(" tx-done-free-count="); print_u64(n->device.statistics.tx_done_free_count);
    print_text(" tx-done-free-ticks="); print_u64(n->device.statistics.tx_done_free_ticks);
    print_text(" tx-done-free-max="); print_u64(n->device.statistics.tx_done_free_max);
    print_text(" tx-free-post-count="); print_u64(n->device.statistics.tx_free_post_count);
    print_text(" tx-free-post-ticks="); print_u64(n->device.statistics.tx_free_post_ticks);
    print_text(" tx-free-post-max="); print_u64(n->device.statistics.tx_free_post_max);
    print_text(" tx-latency-overflow="); print_u64(n->device.statistics.tx_latency_overflow);
#endif
    struct kernel_heap_statistics heap_statistics;
    kernel_heap_get_statistics(n->heap, &heap_statistics);
    print_text(" root-heap-peak-pages="); print_u64(heap_statistics.peak_pages); print_text("\n");
    if (kernel_heap_release(n->heap, n) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    *owner = 0; arch_interrupt_restore(irq); return 0;
}
