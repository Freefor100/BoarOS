#include <arch/context.h>
#include <arch/bus.h>
#include <arch/timer.h>
#include <kernel/virtio_net.h>
#include <kernel/page.h>
#include <kernel/errno.h>
#include <kernel/console.h>
#include <stddef.h>
#include <string.h>

enum { RX_FREE, RX_POSTED, RX_READY, RX_CPU, RX_LOAN };
enum { TX_FREE, TX_PENDING, TX_POSTED, TX_DONE, TX_FILLING };
_Static_assert(VIRTIO_NET_TX_TABLE_OFFSET % 16U == 0U &&
               VIRTIO_NET_TX_TABLE_OFFSET +
                   (VIRTIO_NET_TX_SEGMENTS + 1U) * 16U <=
                   VIRTIO_NET_BUFFER_SIZE,
               "TX indirect table must fit 16-byte aligned in a slot");
static uint32_t rd(struct virtio_net_device *d, enum virtio_register reg)
{ return d->transport.ops.read(d->transport.context, reg); }
static unsigned stride(const struct virtio_net_device *d)
{ return d->version == 1 ? 2 * BOAROS_PAGE_SIZE : 4096; }
static unsigned used_offset(const struct virtio_net_device *d)
{ return d->version == 1 ? BOAROS_PAGE_SIZE : 584; }
static unsigned header(const struct virtio_net_device *d) { return d->version == 1 ? 10 : 12; }
static unsigned padding(const struct virtio_net_device *d) { return 16 - header(d); }
static void *queue(struct virtio_net_device *d, unsigned q) { return (char *)d->queues + q * stride(d); }
static void *rx_data(struct virtio_net_device *d, unsigned b)
{ return (char *)d->rx_memory + b * VIRTIO_NET_BUFFER_SIZE + padding(d); }
static void *tx_data(struct virtio_net_device *d, unsigned b)
{ return (char *)d->tx_memory + b * VIRTIO_NET_BUFFER_SIZE + padding(d); }
static void wake(struct virtio_net_device *d)
{
    /* Wait traversal stays on this CPU; the device state lock has already been returned. */
    uintptr_t irq = arch_interrupt_save();
    if (kernel_wait_queue_wake_all(&d->progress) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    arch_interrupt_restore(irq);
}
static unsigned free_head(uint32_t mask)
{ for (unsigned i = 0; i < 32; i++) if (!(mask & (UINT32_C(1) << i))) return i; return 32; }
static void enqueue(struct virtio_net_device *d, unsigned q, unsigned id, unsigned b)
{
    void *token = q ? (void *)&d->tx_state[b] : (void *)&d->rx_state[b];
    if (virtio_split_enqueue(&d->split[q], &d->transport, id, token,
        q ? 0 : header(d), q ? 2048 : 2048 - padding(d)) != VIRTIO_OK) __builtin_trap();
}
static int rx_refill_pending(const struct virtio_net_device *d)
{ return !d->failed && !d->stopping && d->configured && d->rx_count && d->rx_posted != UINT32_MAX; }
static void refill_rx(struct virtio_net_device *d)
{
    unsigned submitted = 0;
    while (rx_refill_pending(d)) {
        unsigned id = free_head(d->rx_posted);
        unsigned b = d->rx_free[d->rx_head++ % 64];
        d->rx_count--;
        if (d->rx_state[b] != RX_FREE) __builtin_trap();
        d->rx_state[b] = RX_POSTED; d->rx_map[id] = (uint16_t)b;
        struct virtio_descriptor *desc = queue(d, 0);
        desc[id] = (struct virtio_descriptor){ d->rx_phys + b * 2048 + padding(d), 2048 - padding(d), 2, 0 };
        d->rx_posted |= UINT32_C(1) << id;
        enqueue(d, 0, id, b); submitted++;
    }
    if (submitted && virtio_split_notify(&d->split[0], &d->transport, 0) != VIRTIO_OK) __builtin_trap();
}
#if BOAROS_COST_DIAGNOSTICS
static void latency_sample(struct virtio_net_device *d, uint64_t elapsed,
    uint64_t *count, uint64_t *total, uint64_t *maximum)
{
    if (*count == UINT64_MAX || UINT64_MAX - *total < elapsed) {
        d->statistics.tx_latency_overflow = 1;
        return;
    }
    (*count)++; *total += elapsed;
    if (elapsed > *maximum) *maximum = elapsed;
}
static void note_tx_free(struct virtio_net_device *d, unsigned b, uint64_t now)
{
    uint64_t bit = UINT64_C(1) << b;
    if (d->tx_done_valid & bit) {
        latency_sample(d, now - d->tx_done_time[b],
            &d->statistics.tx_done_free_count, &d->statistics.tx_done_free_ticks,
            &d->statistics.tx_done_free_max);
        d->tx_done_valid &= ~bit;
    }
    d->tx_free_time[b] = now; d->tx_free_valid |= bit;
}
#endif
static void flush_tx(struct virtio_net_device *d)
{
    unsigned submitted = 0;
    while (!d->failed && !d->stopping && d->configured && d->tx_count && d->tx_posted != UINT32_MAX) {
        unsigned id = free_head(d->tx_posted);
        unsigned b = d->tx_pending[d->tx_head++ % 64];
        d->tx_count--;
        if (d->tx_state[b] != TX_PENDING) __builtin_trap();
        d->tx_state[b] = TX_POSTED; d->tx_map[id] = (uint16_t)b;
        struct virtio_descriptor *desc = queue(d, 1);
        if (d->tx_owner[b]) {
            /* 零拷贝：主描述符指向槽内 indirect 表（头+各段），不复制 payload。 */
            desc[id] = (struct virtio_descriptor){ d->tx_phys + b * 2048 +
                    VIRTIO_NET_TX_TABLE_OFFSET, d->tx_table[b], 4U, 0 };
        } else {
            /* TX长度保存在独立buffer记录，不能从上次同head的描述符继承。 */
            uint16_t length; memcpy(&length, (char *)d->tx_memory + b * 2048, sizeof(length));
            desc[id] = (struct virtio_descriptor){ d->tx_phys + b * 2048 + padding(d), length, 0, 0 };
        }
        d->tx_time[b] = arch_time_read();
#if BOAROS_COST_DIAGNOSTICS
        if (d->tx_free_valid & (UINT64_C(1) << b)) {
            latency_sample(d, d->tx_time[b] - d->tx_free_time[b],
                &d->statistics.tx_free_post_count, &d->statistics.tx_free_post_ticks,
                &d->statistics.tx_free_post_max);
            d->tx_free_valid &= ~(UINT64_C(1) << b);
        }
#endif
        d->tx_posted |= UINT32_C(1) << id;
        enqueue(d, 1, id, b); submitted++;
    }
    if (submitted && virtio_split_notify(&d->split[1], &d->transport, 1) != VIRTIO_OK) __builtin_trap();
}
static void diagnostic_text(const char *p) { while (*p) kernel_console_putc(*p++); }
static void diagnostic_hex(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    diagnostic_text("0x");
    for (int shift = 60; shift >= 0; shift -= 4) kernel_console_putc(digits[(v >> shift) & 15]);
}
static unsigned posted_count(uint32_t bits) { unsigned n=0; while(bits) { bits &= bits-1; n++; } return n; }
static void fail(struct virtio_net_device *d, const char *reason, unsigned q, uint32_t id, uint32_t length)
{
    if (d->failed) return;
    d->statistics.errors++;
    d->failure_reason = reason; d->failure_queue = q;
    d->failure_id = id; d->failure_length = length;
    d->failed = 1;
}
/* The CPU0 worker prints the failure. A remote submitter only latches it
 * and wakes the worker; neither the raw lock nor its IRQ handler prints. */
static void report_failure(struct virtio_net_device *d)
{
    const char *reason;
    struct {
        uintptr_t context;
        uint32_t q, id, length, transport, version, status, irq, posted;
        uint16_t available, consumed, loaned, ready, pending, done, used, avail_rx, avail_tx;
        uint8_t configured, link;
    } report;
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
        reason = d->failure_reason;
        if (!reason) return;
        d->failure_reason = 0;
        unsigned q = d->failure_queue;
        volatile uint16_t *used_words = (void *)((unsigned char *)d->queues + used_offset(d));
        report.context = (uintptr_t)d->transport.context;
        report.q = q; report.id = d->failure_id; report.length = d->failure_length;
        report.transport = d->transport.kind; report.version = d->version;
        report.status = rd(d, VIRTIO_REG_STATUS); report.irq = d->irq_source;
        report.posted = q ? d->tx_posted : d->rx_posted;
        report.available = d->split[q].next_available; report.consumed = d->split[q].last_used;
        report.loaned = d->loaned; report.ready = d->ready_count; report.pending = d->tx_count;
        report.done = d->tx_done_count; report.used = used_words[1];
        report.avail_rx = d->split[0].next_available; report.avail_tx = d->split[1].next_available;
        report.configured = d->configured; report.link = d->link_up;
    }
    diagnostic_text("BoarOS: VirtIO-net failed reason="); diagnostic_text(reason);
    diagnostic_text(" transport="); diagnostic_hex(report.transport);
    diagnostic_text(" context="); diagnostic_hex(report.context);
    diagnostic_text(" queue="); diagnostic_hex(report.q);
    diagnostic_text(" id="); diagnostic_hex(report.id);
    diagnostic_text(" length="); diagnostic_hex(report.length);
    diagnostic_text(" available="); diagnostic_hex(report.available);
    diagnostic_text(" consumed="); diagnostic_hex(report.consumed);
    diagnostic_text(" posted="); diagnostic_hex(report.posted);
    diagnostic_text("\n");
    diagnostic_text("BoarOS: VirtIO-net slots version="); diagnostic_hex(report.version);
    diagnostic_text(" status="); diagnostic_hex(report.status);
    diagnostic_text(" irq="); diagnostic_hex(report.irq);
    diagnostic_text(" configured="); diagnostic_hex(report.configured);
    diagnostic_text(" link="); diagnostic_hex(report.link);
    diagnostic_text(" loaned="); diagnostic_hex(report.loaned);
    diagnostic_text(" ready="); diagnostic_hex(report.ready);
    diagnostic_text(" pending="); diagnostic_hex(report.pending);
    diagnostic_text(" done="); diagnostic_hex(report.done);
    diagnostic_text(" used="); diagnostic_hex(report.used);
    diagnostic_text(" avail-rx="); diagnostic_hex(report.avail_rx);
    diagnostic_text(" avail-tx="); diagnostic_hex(report.avail_tx);
    diagnostic_text("\n");
    uint64_t now = arch_time_read();
    for (unsigned b = 0; b < VIRTIO_NET_BUFFERS; b++) {
        unsigned rx, tx, length;
        uintptr_t owner;
        uint64_t time;
        {
            KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
            rx = d->rx_state[b]; tx = d->tx_state[b]; length = d->rx_length[b];
            owner = (uintptr_t)d->tx_owner[b]; time = d->tx_time[b];
        }
        if (rx != RX_FREE) {
            diagnostic_text("BoarOS: VirtIO-net rx buffer="); diagnostic_hex(b);
            diagnostic_text(" state="); diagnostic_hex(rx);
            diagnostic_text(" length="); diagnostic_hex(length);
            diagnostic_text("\n");
        }
        if (tx != TX_FREE) {
            diagnostic_text("BoarOS: VirtIO-net tx buffer="); diagnostic_hex(b);
            diagnostic_text(" state="); diagnostic_hex(tx);
            diagnostic_text(" age="); diagnostic_hex(now - time);
            diagnostic_text(" owner="); diagnostic_hex(owner);
            diagnostic_text("\n");
        }
    }
}
static void check_status(struct virtio_net_device *d)
{
    if (d->configured && !d->failed && d->version == 2) {
        uint32_t status = rd(d, VIRTIO_REG_STATUS);
        /* 即使没有TX或配置IRQ丢失，NEEDS_RESET也不能继续发布DMA。 */
        if (status & 64U) fail(d, "needs-reset", 0, status, 0);
    }
}
static int harvest(struct virtio_net_device *d)
{
    if (!d->configured || d->failed) return 0;
    int progress = 0;
    for (unsigned q = 0; q < 2; q++) {
        volatile uint16_t *used = (void *)((char *)queue(d, q) + used_offset(d));
        uint16_t index = used[1]; arch_dma_barrier();
        unsigned count = (uint16_t)(index - d->split[q].last_used);
        uint32_t *posted = q ? &d->tx_posted : &d->rx_posted;
        if (count > posted_count(*posted)) { fail(d, "used-index", q, index, count); return 1; }
        progress |= count != 0;
        while (d->split[q].last_used != index) {
            struct virtio_completion e;
            enum virtio_status status = virtio_split_take(&d->split[q], &e);
            if (status != VIRTIO_OK) {
                struct virtio_queue_fault *f = &d->split[q].fault;
                const char *reason = f->reason == VIRTIO_QUEUE_BAD_LENGTH ?
                    (q ? "tx-length-owner" : "rx-length-owner") : "head-owner";
                fail(d, reason, q, f->head, f->length); return 1;
            }
            unsigned b = q ? d->tx_map[e.head] : d->rx_map[e.head];
            void *token = q ? (void *)&d->tx_state[b] : (void *)&d->rx_state[b];
            if (e.token != token || !( *posted & (UINT32_C(1) << e.head)) ||
                (q ? d->tx_state[b] != TX_POSTED : d->rx_state[b] != RX_POSTED)) __builtin_trap();
            if (!q) {
                const uint8_t *network_header = rx_data(d, b);
                /* 未协商checksum/GSO：不能把待补校验或合并包当作完整Ethernet帧。 */
                if (network_header[0] || network_header[1]) {
                    fail(d, "unnegotiated-offload", q, e.head, e.length); return 1;
                }
            }
            *posted &= ~(UINT32_C(1) << e.head);
            if (q) {
#if BOAROS_COST_DIAGNOSTICS
                d->tx_done_time[b] = arch_time_read();
                d->tx_done_valid |= UINT64_C(1) << b;
#endif
                if (d->tx_owner[b]) {
                    if (d->tx_done_count == VIRTIO_NET_BUFFERS) __builtin_trap();
                    d->tx_state[b] = TX_DONE;
                    d->tx_done[(d->tx_done_head + d->tx_done_count++) %
                               VIRTIO_NET_BUFFERS] = (uint16_t)b;
                } else {
                    d->tx_state[b] = TX_FREE; d->tx_capacity_generation++;
#if BOAROS_COST_DIAGNOSTICS
                    note_tx_free(d, b, d->tx_done_time[b]);
#endif
                }
            }
            else {
                if (d->ready_count == 64) __builtin_trap();
                d->rx_state[b] = RX_READY; d->rx_length[b] = (uint16_t)(e.length - header(d));
                d->rx_ready[(d->ready_head + d->ready_count++) % 64] = (uint16_t)b;
                d->statistics.rx_packets++; d->statistics.rx_bytes += e.length - header(d);
            }
        }
    }
    return progress;
}
static int link_read(struct virtio_net_device *d)
{
    if (!(d->feature_low & (1U << 16))) { d->link_up = 1; return 0; }
    uint32_t value;
    if (virtio_transport_config_read(&d->transport, 6, 2, &value) != VIRTIO_OK) return -KERNEL_EIO;
    d->link_up = (value & 1) != 0; return 0;
}
static void interrupt(void *owner)
{
    struct virtio_net_device *d = owner;
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
        /* 先ack再收割；新的完成必须仍能留下通知。IRQ不发布新RX/TX。 */
        d->transport.ops.ack_interrupt(d->transport.context);
        arch_io_barrier(); check_status(d);
        if (d->configured && link_read(d)) fail(d, "configuration", 0, 0, 0);
        d->statistics.interrupts++; harvest(d);
    }
    wake(d);
}
static int reset_locked(struct virtio_net_device *d)
{
    if (virtio_transport_reset(&d->transport) != VIRTIO_OK) return -KERNEL_EIO;
    d->configured = 0;
    for (unsigned q = 0; q < 2; q++) if (d->split[q].size &&
        virtio_split_reset(&d->split[q], &d->transport) != VIRTIO_OK) __builtin_trap();
    return 0;
}
static int allocate(struct virtio_net_device *d, unsigned order, uint64_t *phys, void **data)
{
    enum physical_page_status status = physical_page_allocate_order(d->allocator, order, phys);
    if (status == PHYSICAL_PAGE_STATUS_EMPTY) return -KERNEL_ENOMEM;
    if (status != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    if (physical_page_resolve(d->allocator, *phys, data) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    memset(*data, 0, (size_t)BOAROS_PAGE_SIZE << order);
    return 0;
}
int virtio_net_init(struct virtio_net_device *d, const struct virtio_transport *transport,
    struct physical_page_allocator *allocator, uint64_t frequency, uint32_t source)
{
    if (!d || d->transport.context || !transport || !transport->context || !allocator ||
        !frequency || frequency > UINT64_MAX / 5 || !source || !transport->ops.read ||
        !transport->ops.write || !transport->ops.config_read || !transport->ops.ack_interrupt ||
        !transport->ops.register_irq || !transport->ops.unregister_irq) return -KERNEL_EINVAL;
    if (transport->device_id != 1 || (transport->version != 1 && transport->version != 2) ||
        transport->config_size < 6) return -KERNEL_ENODEV;
    kernel_raw_lock_init(&d->state, KERNEL_RAW_RANK_OBJECT);
    d->transport = *transport; d->allocator = allocator; d->frequency = frequency;
    d->irq_source = source; d->version = transport->version;
    d->queue_order = d->version == 1 ? 2 : (BOAROS_PAGE_SHIFT < 13 ? 13 - BOAROS_PAGE_SHIFT : 0);
    d->buffer_order = 17 - BOAROS_PAGE_SHIFT;
    kernel_wait_queue_init(&d->progress);
    uint64_t features;
    enum virtio_status status = virtio_transport_begin(&d->transport,
        (UINT64_C(1) << 5) | (UINT64_C(1) << 16) | (UINT64_C(1) << 28), UINT64_C(1) << 5, &features);
    int error = status == VIRTIO_UNSUPPORTED ? -KERNEL_ENOTSUP : -KERNEL_EIO;
    if (status != VIRTIO_OK) goto failed;
    d->feature_low = (uint32_t)features;
    unsigned attempts = 0;
    uint32_t generation;
    do {
        if (++attempts > 4) { error = -KERNEL_EIO; goto failed; }
        generation = d->version == 2 ? rd(d, VIRTIO_REG_CONFIG_GENERATION) : 0;
        for (unsigned i = 0; i < 6; i++) {
            uint32_t value;
            if (virtio_transport_config_read(&d->transport, i, 1, &value) != VIRTIO_OK) goto failed;
            d->mac[i] = (uint8_t)value;
        }
        arch_dma_barrier();
    } while (d->version == 2 && rd(d, VIRTIO_REG_CONFIG_GENERATION) != generation);
    if ((d->mac[0] & 1) || !memcmp(d->mac, "\0\0\0\0\0\0", 6)) { error = -KERNEL_EIO; goto failed; }
    if (link_read(d)) goto failed;
    if ((error = allocate(d, d->queue_order, &d->queue_phys, &d->queues)) ||
        (error = allocate(d, d->buffer_order, &d->rx_phys, &d->rx_memory)) ||
        (error = allocate(d, d->buffer_order, &d->tx_phys, &d->tx_memory))) goto failed;
    for (unsigned q = 0; q < 2; q++) {
        if (virtio_split_initialize(&d->split[q], queue(d, q), stride(d), 32, 512, used_offset(d)) != VIRTIO_OK) __builtin_trap();
        status = virtio_transport_queue(&d->transport, q, 32, d->queue_phys + q * stride(d),
            512, used_offset(d), BOAROS_PAGE_SIZE);
        if (status != VIRTIO_OK) { error = status == VIRTIO_UNSUPPORTED ? -KERNEL_ENOTSUP : -KERNEL_EIO; goto failed; }
    }
    for (unsigned i = 0; i < 64; i++) d->rx_free[i] = (uint16_t)i;
    d->rx_count = 64;
    if (!d->transport.ops.register_irq(d->transport.context, source, interrupt, d)) { error = -KERNEL_EIO; goto failed; }
    d->irq_registered = 1;
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
        if (virtio_transport_start(&d->transport) != VIRTIO_OK) { error = -KERNEL_EIO; goto failed; }
        d->configured = 1;
        refill_rx(d);
    }
    return 0;
failed:
    if (virtio_net_stop(d)) return -KERNEL_EIO;
    return error;
}
struct virtio_net_progress virtio_net_get_progress(struct virtio_net_device *d)
{
    if (!d->state.initialized) return (struct virtio_net_progress){0};
    KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
    return (struct virtio_net_progress){
        .tx_capacity_generation = d->tx_capacity_generation,
        .tx_done_count = d->tx_done_count, .ready_count = d->ready_count, .loaned = d->loaned,
        .configured = d->configured, .link_up = d->link_up, .stopping = d->stopping,
        .failed = d->failed, .rx_refill_pending = rx_refill_pending(d)
    };
}
int virtio_net_service(struct virtio_net_device *d)
{
    int result, notify;
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
        int failed = d->failed;
        check_status(d);
        harvest(d);
        if (!d->failed) {
            uint64_t now = arch_time_read();
            for (unsigned i = 0; i < 32; i++) if (d->tx_posted & (UINT32_C(1) << i)) {
                unsigned b = d->tx_map[i];
                if (now - d->tx_time[b] >= d->frequency * 5) { fail(d, "tx-timeout", 1, i, 0); break; }
            }
        }
        if (!d->failed) { refill_rx(d); flush_tx(d); }
        result = d->failed ? -KERNEL_EIO : 0;
        notify = !failed && d->failed;
    }
    if (notify) wake(d);
    if (result) report_failure(d);
    return result;
}
int virtio_net_receive(struct virtio_net_device *d, struct virtio_net_frame *frame)
{
    KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
    if (!d->ready_count || d->stopping || d->failed) return 0;
    unsigned b = d->rx_ready[d->ready_head++ % 64]; d->ready_count--;
    if (d->rx_state[b] != RX_READY) __builtin_trap();
    d->rx_state[b] = RX_CPU;
    *frame = (struct virtio_net_frame){ (char *)rx_data(d, b) + header(d), d->rx_length[b], b };
    return 1;
}
int virtio_net_lend(struct virtio_net_device *d, unsigned b)
{
    KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
    if (b >= 64 || d->rx_state[b] != RX_CPU) __builtin_trap();
    if (d->loaned == 32) return -KERNEL_EAGAIN;
    d->rx_state[b] = RX_LOAN; d->loaned++;
    d->statistics.loan_packets++; d->statistics.loan_bytes += d->rx_length[b];
    if (d->loaned > d->statistics.loan_peak) d->statistics.loan_peak = d->loaned;
    return 0;
}
void virtio_net_release(struct virtio_net_device *d, unsigned b)
{
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
        if (b >= 64 || (d->rx_state[b] != RX_CPU && d->rx_state[b] != RX_LOAN)) __builtin_trap();
        if (d->rx_state[b] == RX_LOAN) { if (!d->loaned) __builtin_trap(); d->loaned--; }
        d->rx_state[b] = RX_FREE;
        if (d->rx_count == 64) __builtin_trap();
        d->rx_free[(d->rx_head + d->rx_count++) % 64] = (uint16_t)b;
    }
    wake(d);
}
static int tx_status(const struct virtio_net_device *d)
{
    if (d->failed) return -KERNEL_EIO;
    return d->stopping || !d->configured || !d->link_up ? -KERNEL_ENETDOWN : 0;
}
static int tx_slot(struct virtio_net_device *d, int *notify)
{
    int failed = d->failed;
    check_status(d);
    *notify = !failed && d->failed;
    int status = tx_status(d);
    if (status) return status;
    *notify = harvest(d);
    if (d->failed) return -KERNEL_EIO;
    for (unsigned b = 0; b < VIRTIO_NET_BUFFERS; b++)
        if (d->tx_state[b] == TX_FREE) return (int)b;
    return -KERNEL_EAGAIN;
}
static void tx_publish(struct virtio_net_device *d, unsigned b, uint32_t size)
{
    d->tx_state[b] = TX_PENDING;
    d->tx_pending[(d->tx_head + d->tx_count++) % VIRTIO_NET_BUFFERS] = (uint16_t)b;
    d->statistics.tx_packets++; d->statistics.tx_bytes += size;
    flush_tx(d);
}
int virtio_net_send_copy(struct virtio_net_device *d, uint32_t size,
    int (*copy)(const void *, void *, uint32_t), const void *context)
{
    if (!copy || size > 1514) return -KERNEL_EMSGSIZE;
    int slot, notify;
    char *buffer = 0;
    unsigned h = 0;
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
        slot = tx_slot(d, &notify);
        if (slot >= 0) {
            d->tx_state[slot] = TX_FILLING;
            buffer = tx_data(d, (unsigned)slot);
            h = header(d);
        }
    }
    if (notify) wake(d);
    if (slot < 0) return slot;
    /* The slot stays off the DMA ring and pins its storage until this copier
     * publishes or cancels; a concurrent reset does not free that storage. */
    memset(buffer, 0, h);
    int copied = copy(context, buffer + h, size);
    int result;
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
        if (d->tx_state[slot] != TX_FILLING) __builtin_trap();
        check_status(d);
        result = copied ? tx_status(d) : -KERNEL_EIO;
        if (result) {
            d->tx_state[slot] = TX_FREE;
            d->tx_capacity_generation++;
        } else {
            uint16_t length = (uint16_t)(size + h);
            memcpy((char *)d->tx_memory + slot * VIRTIO_NET_BUFFER_SIZE, &length, 2);
            d->statistics.tx_copy_packets++;
            tx_publish(d, (unsigned)slot, size);
            result = (int)size;
        }
    }
    wake(d);
    return result;
}
static int copy_data(const void *source, void *destination, uint32_t size)
{ memcpy(destination, source, size); return 1; }
int virtio_net_send_segments(struct virtio_net_device *d,
    const struct virtio_net_tx_segment *segments, unsigned count, void *owner)
{
    if (!segments || count == 0U || count > VIRTIO_NET_TX_SEGMENTS) return -KERNEL_EMSGSIZE;
    uint64_t total = 0;
    for (unsigned i = 0; i < count; i++) {
        if (segments[i].length == 0U) return -KERNEL_EMSGSIZE;
        total += segments[i].length;
    }
    if (total > 1514U) return -KERNEL_EMSGSIZE;
    int slot, notify;
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
        if (!(d->feature_low & (1U << 28))) return -KERNEL_ENOTSUP;
        slot = tx_slot(d, &notify);
        if (slot >= 0) {
            unsigned b = (unsigned)slot;
            unsigned h = header(d);
            memset(tx_data(d, b), 0, h);
            struct virtio_descriptor *table = (void *)((char *)d->tx_memory +
                b * VIRTIO_NET_BUFFER_SIZE + VIRTIO_NET_TX_TABLE_OFFSET);
            table[0] = (struct virtio_descriptor){ d->tx_phys + b * VIRTIO_NET_BUFFER_SIZE +
                padding(d), h, 1U, 1U };
            for (unsigned i = 0; i < count; i++) {
                table[1 + i] = (struct virtio_descriptor){ segments[i].physical_address,
                    segments[i].length, i + 1U < count ? 1U : 0U, (uint16_t)(i + 2U) };
            }
            d->tx_table[b] = (uint16_t)((count + 1U) * 16U);
            d->tx_owner[b] = owner;
            d->statistics.tx_sg_packets++;
            tx_publish(d, b, (uint32_t)total);
        }
    }
    if (slot >= 0 || notify) wake(d);
    return slot >= 0 ? 0 : slot;
}
unsigned virtio_net_tx_release(struct virtio_net_device *d,
    void (*release)(void *owner), int abandon)
{
    if (!d || !release) return 0;
    void *owners[VIRTIO_NET_BUFFERS];
    unsigned released = 0;
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
        if (abandon && (!d->transport.quiescent || d->transport.started)) __builtin_trap();
        unsigned next = 0;
        while (abandon ? next < VIRTIO_NET_BUFFERS : d->tx_done_count != 0) {
            unsigned b;
            if (abandon) {
                b = next++;
                if (!d->tx_owner[b]) continue;
            } else {
                b = d->tx_done[d->tx_done_head++ % VIRTIO_NET_BUFFERS];
                d->tx_done_count--;
                if (d->tx_state[b] != TX_DONE || !d->tx_owner[b]) __builtin_trap();
            }
            /* 先归还槽位与capacity，再在IRQ外释放已摘下的独立owner。 */
            owners[released++] = d->tx_owner[b];
            d->tx_owner[b] = 0; d->tx_state[b] = TX_FREE;
            d->tx_capacity_generation++;
#if BOAROS_COST_DIAGNOSTICS
            note_tx_free(d, b, arch_time_read());
#endif
        }
        if (abandon) d->tx_head = d->tx_count = d->tx_done_head = d->tx_done_count = 0;
    }
    for (unsigned i = 0; i < released; i++) release(owners[i]);
    return released;
}

int virtio_net_send(struct virtio_net_device *d, const void *data, uint32_t size)
{
    if (!data) return -KERNEL_EINVAL;
    return virtio_net_send_copy(d, size, copy_data, data);
}
void virtio_net_begin_stop(struct virtio_net_device *d)
{
    KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
    d->stopping = 1;
}
int virtio_net_quiesce(struct virtio_net_device *d)
{
    if (!d || !d->transport.context) return 0;
    report_failure(d);
    KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
    d->stopping = 1;
    if (reset_locked(d)) return -KERNEL_EIO;
    if (d->irq_registered) {
        d->transport.ops.unregister_irq(d->transport.context, d->irq_source, d);
        d->irq_registered = 0;
    }
    return 0;
}
int virtio_net_stop(struct virtio_net_device *d)
{
    if (!d || !d->transport.context) return 0;
    if (virtio_net_quiesce(d)) return -KERNEL_EIO;
    uint64_t queue_phys, rx_phys, tx_phys;
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &d->state);
        /* DMA stop does not revoke a CPU copy, RX lease or protocol reference. */
        if (d->loaned) return -KERNEL_EBUSY;
        for (unsigned i = 0; i < 64; i++)
            if (d->rx_state[i] == RX_CPU || d->tx_state[i] == TX_FILLING || d->tx_owner[i])
                return -KERNEL_EBUSY;
        queue_phys = d->queue_phys; rx_phys = d->rx_phys; tx_phys = d->tx_phys;
        d->queue_phys = d->rx_phys = d->tx_phys = 0;
        d->queues = d->rx_memory = d->tx_memory = 0;
        memset(d->split, 0, sizeof(d->split)); d->transport.context = 0;
    }
    if (queue_phys && physical_page_release_order(d->allocator, queue_phys, d->queue_order) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    if (rx_phys && physical_page_release_order(d->allocator, rx_phys, d->buffer_order) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    if (tx_phys && physical_page_release_order(d->allocator, tx_phys, d->buffer_order) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    return 0;
}
