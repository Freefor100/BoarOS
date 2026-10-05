#include <arch/riscv/context.h>
#include <arch/riscv/plic.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/virtio_mmio_net.h>
#include <kernel/errno.h>
#include <kernel/console.h>
#include <stddef.h>
#include <string.h>

enum { RX_FREE, RX_POSTED, RX_READY, RX_CPU, RX_LOAN };
enum { TX_FREE, TX_PENDING, TX_POSTED, TX_DONE };
_Static_assert(RISCV_NET_TX_TABLE_OFFSET % 16U == 0U &&
               RISCV_NET_TX_TABLE_OFFSET +
                   (RISCV_NET_TX_SEGMENTS + 1U) * 16U <=
                   RISCV_NET_BUFFER_SIZE,
               "TX indirect table must fit 16-byte aligned in a slot");
struct net_descriptor { uint64_t address; uint32_t length; uint16_t flags, next; };
struct net_used { uint32_t id, length; };

/* MMIO边界独立于队列算法，宿主设备模型覆盖真实选择寄存器语义。 */
__attribute__((weak)) uint32_t riscv_virtio_net_read(volatile uint8_t *base, unsigned offset)
{ return *(volatile uint32_t *)(base + offset); }
__attribute__((weak)) void riscv_virtio_net_write(volatile uint8_t *base, unsigned offset, uint32_t value)
{ *(volatile uint32_t *)(base + offset) = value; }
static uint32_t rd(struct riscv_virtio_mmio_net *d, unsigned offset)
{ return riscv_virtio_net_read(d->mmio, offset); }
static void wr(struct riscv_virtio_mmio_net *d, unsigned offset, uint32_t value)
{ riscv_virtio_net_write(d->mmio, offset, value); }
static void barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static unsigned stride(const struct riscv_virtio_mmio_net *d) { return d->version == 1 ? 8192 : 4096; }
static unsigned used_offset(const struct riscv_virtio_mmio_net *d) { return d->version == 1 ? 4096 : 584; }
static unsigned header(const struct riscv_virtio_mmio_net *d) { return d->version == 1 ? 10 : 12; }
static unsigned padding(const struct riscv_virtio_mmio_net *d) { return 16 - header(d); }
static void *queue(struct riscv_virtio_mmio_net *d, unsigned q) { return (char *)d->queues + q * stride(d); }
static void *rx_data(struct riscv_virtio_mmio_net *d, unsigned b)
{ return (char *)d->rx_memory + b * RISCV_NET_BUFFER_SIZE + padding(d); }
static void *tx_data(struct riscv_virtio_mmio_net *d, unsigned b)
{ return (char *)d->tx_memory + b * RISCV_NET_BUFFER_SIZE + padding(d); }
static void wake(struct riscv_virtio_mmio_net *d)
{ if (kernel_wait_queue_wake_all(&d->progress) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap(); }
static unsigned free_head(uint32_t mask)
{ for (unsigned i = 0; i < 32; i++) if (!(mask & (UINT32_C(1) << i))) return i; return 32; }
static void publish(struct riscv_virtio_mmio_net *d, unsigned q, unsigned id)
{
    volatile uint16_t *avail = (void *)((char *)queue(d, q) + 512);
    avail[2 + d->available[q] % 32] = (uint16_t)id;
    barrier();
    avail[1] = ++d->available[q];
    barrier();
    wr(d, 0x50, q);
}
static void refill_rx(struct riscv_virtio_mmio_net *d)
{
    while (!d->failed && !d->stopping && d->configured && d->rx_count && d->rx_posted != UINT32_MAX) {
        unsigned id = free_head(d->rx_posted);
        unsigned b = d->rx_free[d->rx_head++ % 64];
        d->rx_count--;
        if (d->rx_state[b] != RX_FREE) __builtin_trap();
        d->rx_state[b] = RX_POSTED; d->rx_map[id] = (uint16_t)b;
        struct net_descriptor *desc = queue(d, 0);
        desc[id] = (struct net_descriptor){ d->rx_phys + b * 2048 + padding(d), 2048 - padding(d), 2, 0 };
        d->rx_posted |= UINT32_C(1) << id;
        publish(d, 0, id);
    }
}
static void flush_tx(struct riscv_virtio_mmio_net *d)
{
    while (!d->failed && !d->stopping && d->configured && d->tx_count && d->tx_posted != UINT32_MAX) {
        unsigned id = free_head(d->tx_posted);
        unsigned b = d->tx_pending[d->tx_head++ % 64];
        d->tx_count--;
        if (d->tx_state[b] != TX_PENDING) __builtin_trap();
        d->tx_state[b] = TX_POSTED; d->tx_map[id] = (uint16_t)b;
        struct net_descriptor *desc = queue(d, 1);
        if (d->tx_owner[b]) {
            /* 零拷贝：主描述符指向槽内 indirect 表（头+各段），不复制 payload。 */
            desc[id] = (struct net_descriptor){ d->tx_phys + b * 2048 +
                    RISCV_NET_TX_TABLE_OFFSET, d->tx_table[b], 4U, 0 };
        } else {
            /* TX长度保存在独立buffer记录，不能从上次同head的描述符继承。 */
            uint16_t length; memcpy(&length, (char *)d->tx_memory + b * 2048, sizeof(length));
            desc[id] = (struct net_descriptor){ d->tx_phys + b * 2048 + padding(d), length, 0, 0 };
        }
        d->tx_time[b] = riscv_time_read();
        d->tx_posted |= UINT32_C(1) << id;
        publish(d, 1, id);
    }
}
static void diagnostic_text(const char *p) { while (*p) kernel_console_putc(*p++); }
static void diagnostic_hex(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    diagnostic_text("0x");
    for (int shift = 60; shift >= 0; shift -= 4) kernel_console_putc(digits[(v >> shift) & 15]);
}
static unsigned posted_count(uint32_t bits) { unsigned n=0; while(bits) { bits &= bits-1; n++; } return n; }
static void fail(struct riscv_virtio_mmio_net *d, const char *reason, unsigned q, uint32_t id, uint32_t length)
{
    if (!d->failed) {
        d->statistics.errors++;
        diagnostic_text("BoarOS: VirtIO-net failed reason="); diagnostic_text(reason);
        diagnostic_text(" mmio="); diagnostic_hex((uintptr_t)d->mmio);
        diagnostic_text(" queue="); diagnostic_hex(q);
        diagnostic_text(" id="); diagnostic_hex(id);
        diagnostic_text(" length="); diagnostic_hex(length);
        diagnostic_text(" available="); diagnostic_hex(d->available[q]);
        diagnostic_text(" consumed="); diagnostic_hex(d->consumed[q]);
        diagnostic_text(" posted="); diagnostic_hex(q ? d->tx_posted : d->rx_posted);
        diagnostic_text("\n");
        /* 槽级快照：只读驱动自有数组与队列索引，不追描述符地址、不分配。 */
        volatile uint16_t *used_words = (void *)((unsigned char *)d->queues + used_offset(d));
        diagnostic_text("BoarOS: VirtIO-net slots version="); diagnostic_hex(d->version);
        diagnostic_text(" status="); diagnostic_hex(rd(d, 0x70));
        diagnostic_text(" irq="); diagnostic_hex(d->irq_source);
        diagnostic_text(" configured="); diagnostic_hex(d->configured);
        diagnostic_text(" link="); diagnostic_hex(d->link_up);
        diagnostic_text(" loaned="); diagnostic_hex(d->loaned);
        diagnostic_text(" ready="); diagnostic_hex(d->ready_count);
        diagnostic_text(" pending="); diagnostic_hex(d->tx_count);
        diagnostic_text(" done="); diagnostic_hex(d->tx_done_count);
        diagnostic_text(" used="); diagnostic_hex(used_words[1]);
        diagnostic_text(" avail-rx="); diagnostic_hex(d->available[0]);
        diagnostic_text(" avail-tx="); diagnostic_hex(d->available[1]);
        diagnostic_text("\n");
        for (unsigned b = 0; b < RISCV_NET_BUFFERS; b++) {
            if (d->rx_state[b] != RX_FREE) {
                diagnostic_text("BoarOS: VirtIO-net rx buffer="); diagnostic_hex(b);
                diagnostic_text(" state="); diagnostic_hex(d->rx_state[b]);
                diagnostic_text(" length="); diagnostic_hex(d->rx_length[b]);
                diagnostic_text("\n");
            }
        }
        for (unsigned b = 0; b < RISCV_NET_BUFFERS; b++) {
            if (d->tx_state[b] != TX_FREE) {
                diagnostic_text("BoarOS: VirtIO-net tx buffer="); diagnostic_hex(b);
                diagnostic_text(" state="); diagnostic_hex(d->tx_state[b]);
                diagnostic_text(" age="); diagnostic_hex(riscv_time_read() - d->tx_time[b]);
                diagnostic_text(" owner="); diagnostic_hex((uintptr_t)d->tx_owner[b]);
                diagnostic_text("\n");
            }
        }
    }
    d->failed = 1;
    wake(d);
}
static void check_status(struct riscv_virtio_mmio_net *d)
{
    if (d->configured && !d->failed && d->version == 2) {
        uint32_t status = rd(d, 0x70);
        /* 即使没有TX或配置IRQ丢失，NEEDS_RESET也不能继续发布DMA。 */
        if (status & 64U) fail(d, "needs-reset", 0, status, 0);
    }
}
static void harvest(struct riscv_virtio_mmio_net *d)
{
    if (!d->configured || d->failed) return;
    for (unsigned q = 0; q < 2; q++) {
        volatile uint16_t *used = (void *)((char *)queue(d, q) + used_offset(d));
        uint16_t index = used[1]; barrier();
        unsigned count = (uint16_t)(index - d->consumed[q]);
        uint32_t *posted = q ? &d->tx_posted : &d->rx_posted;
        if (count > posted_count(*posted)) { fail(d, "used-index", q, index, count); return; }
        while (d->consumed[q] != index) {
            volatile struct net_used *items = (void *)(used + 2);
            struct net_used e = items[d->consumed[q] % 32];
            if (e.id >= 32 || !(*posted & (UINT32_C(1) << e.id))) { fail(d, "head-owner", q, e.id, e.length); return; }
            unsigned b = q ? d->tx_map[e.id] : d->rx_map[e.id];
            if (!q && (e.length < header(d) || e.length > 2048 - padding(d) || d->rx_state[b] != RX_POSTED)) {
                fail(d, "rx-length-owner", q, e.id, e.length); return;
            }
            if (q && (e.length > 2048 || d->tx_state[b] != TX_POSTED)) { fail(d, "tx-length-owner", q, e.id, e.length); return; }
            if (!q) {
                const uint8_t *network_header = rx_data(d, b);
                /* 未协商checksum/GSO：不能把待补校验或合并包当作完整Ethernet帧。 */
                if (network_header[0] || network_header[1]) {
                    fail(d, "unnegotiated-offload", q, e.id, e.length); return;
                }
            }
            *posted &= ~(UINT32_C(1) << e.id); d->consumed[q]++;
            if (q) {
                if (d->tx_owner[b]) {
                    if (d->tx_done_count == RISCV_NET_BUFFERS) __builtin_trap();
                    d->tx_state[b] = TX_DONE;
                    d->tx_done[(d->tx_done_head + d->tx_done_count++) %
                               RISCV_NET_BUFFERS] = (uint16_t)b;
                } else { d->tx_state[b] = TX_FREE; d->tx_capacity_generation++; }
            }
            else {
                if (d->ready_count == 64) __builtin_trap();
                d->rx_state[b] = RX_READY; d->rx_length[b] = (uint16_t)(e.length - header(d));
                d->rx_ready[(d->ready_head + d->ready_count++) % 64] = (uint16_t)b;
                d->statistics.rx_packets++; d->statistics.rx_bytes += e.length - header(d);
            }
        }
    }
}
static void interrupt(void *owner)
{
    struct riscv_virtio_mmio_net *d = owner;
    uint32_t status = rd(d, 0x60);
    /* 先ack再收割；新的完成必须仍能留下通知。IRQ不发布新RX/TX。 */
    if (status) wr(d, 0x64, status);
    check_status(d);
    if (d->feature_low & (1U << 16)) d->link_up = (d->mmio[0x106] & 1) != 0;
    d->statistics.interrupts++; harvest(d); wake(d);
}
static int reset(struct riscv_virtio_mmio_net *d)
{
    wr(d, 0x70, 0); barrier();
    if (rd(d, 0x70)) return -KERNEL_EIO;
    d->configured = 0;
    return 0;
}
static int allocate(struct riscv_virtio_mmio_net *d, unsigned order, uint64_t *phys, void **data)
{
    enum physical_page_status status = physical_page_allocate_order(d->allocator, order, phys);
    if (status == PHYSICAL_PAGE_STATUS_EMPTY) return -KERNEL_ENOMEM;
    if (status != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    if (physical_page_resolve(d->allocator, *phys, data) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    memset(*data, 0, 4096U << order);
    return 0;
}
int riscv_virtio_mmio_net_init(struct riscv_virtio_mmio_net *d, volatile void *mmio,
    uint64_t size, struct physical_page_allocator *allocator, uint64_t frequency, uint32_t source)
{
    if (!d || d->mmio || !mmio || !allocator || !frequency || !source || size < 0x108) return -KERNEL_EINVAL;
    volatile uint8_t *base = mmio;
    uint32_t version = riscv_virtio_net_read(base, 4);
    if (riscv_virtio_net_read(base, 0) != UINT32_C(0x74726976) ||
        riscv_virtio_net_read(base, 8) != 1 || (version != 1 && version != 2)) return -KERNEL_ENODEV;
    d->mmio = base; d->allocator = allocator; d->frequency = frequency;
    d->irq_source = source; d->version = version; d->queue_order = version == 1 ? 2 : 1;
    kernel_wait_queue_init(&d->progress);
    int error = reset(d);
    if (error) return error;
    wr(d, 0x70, 1); wr(d, 0x70, 3); wr(d, 0x14, 0);
    uint32_t features = rd(d, 0x10);
    if (!(features & (1U << 5))) { error = -KERNEL_ENOTSUP; goto failed; }
    d->feature_low = features & ((1U << 5) | (1U << 16) | (1U << 28));
    wr(d, 0x24, 0); wr(d, 0x20, d->feature_low);
    if (version == 2) {
        wr(d, 0x14, 1);
        if (!(rd(d, 0x10) & 1)) { error = -KERNEL_ENOTSUP; goto failed; }
        wr(d, 0x24, 1); wr(d, 0x20, 1); wr(d, 0x70, 11);
        if (!(rd(d, 0x70) & 8)) { error = -KERNEL_EIO; goto failed; }
    } else wr(d, 0x28, 4096);
    unsigned attempts = 0;
    uint32_t generation;
    do {
        if (++attempts > 4) { error = -KERNEL_EIO; goto failed; }
        generation = version == 2 ? rd(d, 0xfc) : 0;
        for (unsigned i = 0; i < 6; i++) d->mac[i] = d->mmio[0x100 + i];
        barrier();
    } while (version == 2 && rd(d, 0xfc) != generation);
    if ((d->mac[0] & 1) || !memcmp(d->mac, "\0\0\0\0\0\0", 6)) { error = -KERNEL_EIO; goto failed; }
    d->link_up = !(d->feature_low & (1U << 16)) || (d->mmio[0x106] & 1);
    if ((error = allocate(d, d->queue_order, &d->queue_phys, &d->queues)) ||
        (error = allocate(d, 5, &d->rx_phys, &d->rx_memory)) ||
        (error = allocate(d, 5, &d->tx_phys, &d->tx_memory))) goto failed;
    for (unsigned q = 0; q < 2; q++) {
        wr(d, 0x30, q);
        if (rd(d, 0x34) < 32) { error = -KERNEL_ENOTSUP; goto failed; }
        wr(d, 0x38, 32);
        uint64_t p = d->queue_phys + q * stride(d);
        if (version == 1) {
            if ((p >> 12) > UINT32_MAX) { error = -KERNEL_ENOTSUP; goto failed; }
            wr(d, 0x3c, 4096); wr(d, 0x40, (uint32_t)(p >> 12));
        } else {
            wr(d, 0x80, (uint32_t)p); wr(d, 0x84, (uint32_t)(p >> 32));
            p += 512; wr(d, 0x90, (uint32_t)p); wr(d, 0x94, (uint32_t)(p >> 32));
            p = d->queue_phys + q * stride(d) + used_offset(d);
            wr(d, 0xa0, (uint32_t)p); wr(d, 0xa4, (uint32_t)(p >> 32)); wr(d, 0x44, 1);
        }
    }
    for (unsigned i = 0; i < 64; i++) d->rx_free[i] = (uint16_t)i;
    d->rx_count = 64;
    if (!riscv_plic_register(source, interrupt, d)) { error = -KERNEL_EIO; goto failed; }
    d->irq_registered = d->configured = 1;
    wr(d, 0x70, version == 2 ? 15 : 7);
    uintptr_t saved = riscv_interrupt_save(); refill_rx(d); riscv_interrupt_restore(saved);
    return 0;
failed:
    if (riscv_virtio_mmio_net_stop(d)) return -KERNEL_EIO;
    return error;
}
int riscv_virtio_mmio_net_service(struct riscv_virtio_mmio_net *d)
{
    uintptr_t saved = riscv_interrupt_save();
    check_status(d);
    harvest(d);
    if (!d->failed) {
        uint64_t now = riscv_time_read();
        for (unsigned i = 0; i < 32; i++) if (d->tx_posted & (UINT32_C(1) << i)) {
            unsigned b = d->tx_map[i];
            if (now - d->tx_time[b] >= d->frequency * 5) { fail(d, "tx-timeout", 1, i, 0); break; }
        }
    }
    if (!d->failed) { refill_rx(d); flush_tx(d); }
    int result = d->failed ? -KERNEL_EIO : 0;
    riscv_interrupt_restore(saved); return result;
}
int riscv_virtio_mmio_net_receive(struct riscv_virtio_mmio_net *d, struct riscv_net_frame *frame)
{
    uintptr_t saved = riscv_interrupt_save();
    if (!d->ready_count) { riscv_interrupt_restore(saved); return 0; }
    unsigned b = d->rx_ready[d->ready_head++ % 64]; d->ready_count--;
    if (d->rx_state[b] != RX_READY) __builtin_trap();
    d->rx_state[b] = RX_CPU;
    *frame = (struct riscv_net_frame){ (char *)rx_data(d, b) + header(d), d->rx_length[b], b };
    riscv_interrupt_restore(saved); return 1;
}
int riscv_virtio_mmio_net_lend(struct riscv_virtio_mmio_net *d, unsigned b)
{
    uintptr_t saved = riscv_interrupt_save();
    if (b >= 64 || d->rx_state[b] != RX_CPU) __builtin_trap();
    if (d->loaned == 32) { riscv_interrupt_restore(saved); return -KERNEL_EAGAIN; }
    d->rx_state[b] = RX_LOAN; d->loaned++;
    d->statistics.loan_packets++; d->statistics.loan_bytes += d->rx_length[b];
    if (d->loaned > d->statistics.loan_peak) d->statistics.loan_peak = d->loaned;
    riscv_interrupt_restore(saved); return 0;
}
void riscv_virtio_mmio_net_release(struct riscv_virtio_mmio_net *d, unsigned b)
{
    uintptr_t saved = riscv_interrupt_save();
    if (b >= 64 || (d->rx_state[b] != RX_CPU && d->rx_state[b] != RX_LOAN)) __builtin_trap();
    if (d->rx_state[b] == RX_LOAN) { if (!d->loaned) __builtin_trap(); d->loaned--; }
    d->rx_state[b] = RX_FREE;
    if (d->rx_count == 64) __builtin_trap();
    d->rx_free[(d->rx_head + d->rx_count++) % 64] = (uint16_t)b;
    wake(d); riscv_interrupt_restore(saved);
}
int riscv_virtio_mmio_net_send_copy(struct riscv_virtio_mmio_net *d, uint32_t size,
    int (*copy)(const void *, void *, uint32_t), const void *context)
{
    if (!copy || size > 1514) return -KERNEL_EMSGSIZE;
    uintptr_t saved = riscv_interrupt_save();
    check_status(d);
    if (d->failed) { riscv_interrupt_restore(saved); return -KERNEL_EIO; }
    if (d->stopping || !d->configured || !d->link_up) { riscv_interrupt_restore(saved); return -KERNEL_ENETDOWN; }
    harvest(d);
    unsigned b; for (b = 0; b < 64; b++) if (d->tx_state[b] == TX_FREE) break;
    if (b == 64 || d->failed) { riscv_interrupt_restore(saved); return d->failed ? -KERNEL_EIO : -KERNEL_EAGAIN; }
    memset(tx_data(d, b), 0, header(d));
    if (!copy(context, (char *)tx_data(d, b) + header(d), size)) {
        riscv_interrupt_restore(saved); return -KERNEL_EIO;
    }
    uint16_t length = (uint16_t)(size + header(d));
    memcpy((char *)d->tx_memory + b * 2048, &length, 2);
    d->tx_state[b] = TX_PENDING;
    d->tx_pending[(d->tx_head + d->tx_count++) % 64] = (uint16_t)b;
    d->statistics.tx_packets++; d->statistics.tx_bytes += size;
    d->statistics.tx_copy_packets++;
    flush_tx(d); wake(d);
    riscv_interrupt_restore(saved); return (int)size;
}
static int copy_data(const void *source, void *destination, uint32_t size)
{ memcpy(destination, source, size); return 1; }
int riscv_virtio_mmio_net_send_segments(struct riscv_virtio_mmio_net *d,
    const struct riscv_net_tx_segment *segments, unsigned count, void *owner)
{
    if (!segments || count == 0U || count > RISCV_NET_TX_SEGMENTS) return -KERNEL_EMSGSIZE;
    uint64_t total = 0;
    for (unsigned i = 0; i < count; i++) {
        if (segments[i].length == 0U) return -KERNEL_EMSGSIZE;
        total += segments[i].length;
    }
    if (total > 1514U) return -KERNEL_EMSGSIZE;
    uintptr_t saved = riscv_interrupt_save();
    if (!(d->feature_low & (1U << 28))) { riscv_interrupt_restore(saved); return -KERNEL_ENOTSUP; }
    check_status(d);
    if (d->failed) { riscv_interrupt_restore(saved); return -KERNEL_EIO; }
    if (d->stopping || !d->configured || !d->link_up) { riscv_interrupt_restore(saved); return -KERNEL_ENETDOWN; }
    harvest(d);
    unsigned b; for (b = 0; b < RISCV_NET_BUFFERS; b++) if (d->tx_state[b] == TX_FREE) break;
    if (b == RISCV_NET_BUFFERS || d->failed) {
        riscv_interrupt_restore(saved);
        return d->failed ? -KERNEL_EIO : -KERNEL_EAGAIN;
    }
    unsigned h = header(d);
    memset(tx_data(d, b), 0, h);
    struct net_descriptor *table = (void *)((char *)d->tx_memory +
        b * RISCV_NET_BUFFER_SIZE + RISCV_NET_TX_TABLE_OFFSET);
    table[0] = (struct net_descriptor){ d->tx_phys + b * RISCV_NET_BUFFER_SIZE +
        padding(d), h, 1U, 1U };
    for (unsigned i = 0; i < count; i++) {
        table[1 + i] = (struct net_descriptor){ segments[i].physical_address,
            segments[i].length, i + 1U < count ? 1U : 0U, (uint16_t)(i + 2U) };
    }
    d->tx_table[b] = (uint16_t)((count + 1U) * 16U);
    d->tx_owner[b] = owner;
    d->tx_state[b] = TX_PENDING;
    d->tx_pending[(d->tx_head + d->tx_count++) % RISCV_NET_BUFFERS] = (uint16_t)b;
    d->statistics.tx_packets++; d->statistics.tx_bytes += total;
    d->statistics.tx_sg_packets++;
    flush_tx(d); wake(d);
    riscv_interrupt_restore(saved);
    return 0;
}
unsigned riscv_virtio_mmio_net_tx_release(struct riscv_virtio_mmio_net *d,
    void (*release)(void *owner), int abandon)
{
    if (!d || !release) return 0;
    unsigned released = 0;
    uintptr_t saved = riscv_interrupt_save();
    if (abandon) {
        /* reset/stop 已确认 DMA 停止，归还所有仍持有的 owner。 */
        for (unsigned b = 0; b < RISCV_NET_BUFFERS; b++) {
            if (!d->tx_owner[b]) continue;
            release(d->tx_owner[b]);
            d->tx_owner[b] = 0;
            d->tx_state[b] = TX_FREE;
            d->tx_capacity_generation++;
            released++;
        }
        d->tx_head = 0; d->tx_count = 0; d->tx_done_head = 0; d->tx_done_count = 0;
    } else {
        while (d->tx_done_count) {
            unsigned b = d->tx_done[d->tx_done_head++ % RISCV_NET_BUFFERS];
            d->tx_done_count--;
            if (d->tx_state[b] != TX_DONE || !d->tx_owner[b]) __builtin_trap();
            release(d->tx_owner[b]);
            d->tx_owner[b] = 0;
            d->tx_state[b] = TX_FREE;
            d->tx_capacity_generation++;
            released++;
        }
    }
    riscv_interrupt_restore(saved);
    return released;
}
int riscv_virtio_mmio_net_send(struct riscv_virtio_mmio_net *d, const void *data, uint32_t size)
{
    if (!data) return -KERNEL_EINVAL;
    return riscv_virtio_mmio_net_send_copy(d, size, copy_data, data);
}
int riscv_virtio_mmio_net_stop(struct riscv_virtio_mmio_net *d)
{
    if (!d || !d->mmio) return 0;
    uintptr_t saved = riscv_interrupt_save(); d->stopping = 1;
    if (reset(d)) { riscv_interrupt_restore(saved); return -KERNEL_EIO; }
    if (d->irq_registered) { riscv_plic_unregister(d->irq_source, d); d->irq_registered = 0; }
    /* reset只结束设备DMA，不能撤销协议仍借用的内容。 */
    if (d->loaned) { riscv_interrupt_restore(saved); return -KERNEL_EBUSY; }
    for (unsigned i = 0; i < 64; i++) if (d->rx_state[i] == RX_CPU) {
        riscv_interrupt_restore(saved); return -KERNEL_EBUSY;
    }
    if (d->queue_phys && physical_page_release_order(d->allocator, d->queue_phys, d->queue_order) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    if (d->rx_phys && physical_page_release_order(d->allocator, d->rx_phys, 5) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    if (d->tx_phys && physical_page_release_order(d->allocator, d->tx_phys, 5) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    d->queue_phys = d->rx_phys = d->tx_phys = 0; d->queues = d->rx_memory = d->tx_memory = 0; d->mmio = 0;
    riscv_interrupt_restore(saved); return 0;
}
