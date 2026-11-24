#include <kernel/igb.h>
#include <arch/bus.h>
#include <arch/timer.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/pci.h>
#include <kernel/physical_page.h>
#include <kernel/raw_lock.h>
#include <kernel/scheduler.h>
#include <stdbool.h>
#include <string.h>

enum {
    IGB_CTRL = 0x0000, IGB_STATUS = 0x0008, IGB_MDIC = 0x20,
    IGB_ICR = 0x00c0, IGB_IMS = 0x00d0, IGB_IMC = 0x00d8,
    IGB_RCTL = 0x0100, IGB_TCTL = 0x0400, IGB_RFCTL = 0x5008,
    IGB_MTA = 0x5200, IGB_RAL0 = 0x5400, IGB_RAH0 = 0x5404,
    IGB_RAL1 = 0x5408, IGB_RAH1 = 0x540c, IGB_MRQC = 0x5818, IGB_VTCTL = 0x581c,
    IGB_VMOLR0 = 0x5ad0, IGB_VMOLR1 = 0x5ad4, IGB_VFRE = 0xc8c,
    IGB_RAH_VALID = 1U << 31, IGB_RAH_POOL0 = 1U << 18, IGB_RAH_POOL1 = 1U << 19,
    IGB_VTCTL_NO_DEFAULT = 1U << 29, IGB_MRQC_VMDQ = 3,
    IGB_VMOLR_MULTICAST = 1U << 25, IGB_VMOLR_BROADCAST = 1U << 27,
    IGB_RX_BASE = 0xc000, IGB_RDBAL = 0, IGB_RDBAH = 4,
    IGB_RDLEN = 8, IGB_SRRCTL = 12, IGB_RDH = 16, IGB_RDT = 24, IGB_RXDCTL = 40,
    IGB_CTRL_RESET = 1U << 26, IGB_RCTL_SECRC = 1U << 26, IGB_RCTL_EN = 1U << 1,
    IGB_RCTL_BAM = 1U << 15, IGB_RCTL_MPE = 1U << 4,
    IGB_TCTL_EN = 1U << 1, IGB_TCTL_PSP = 1U << 3,
    IGB_RX_QUEUE_EN = 1U << 25, IGB_RX_DROP = 1U << 31,
    IGB_RX_HEADER_SPLIT = 2U << 25, IGB_RXD_DD = 1, IGB_RXD_EOP = 2,
    IGB_RXD_HBO = 1U << 23, IGB_RXD_FRAME_ERRORS = 0x97000000U,
    IGB_RXD_SPLIT = 1U << 15, IGB_RXD_HEADER_SHIFT = 5,
    IGB_RXD_HEADER_MASK = 1023, IGB_DEVICE = 0x10c98086U,
    IGB_MDIC_PHY1 = 1U << 21, IGB_MDIC_WRITE = 1U << 26,
    IGB_MDIC_READY = 1U << 28, IGB_MDIC_ERROR = 1U << 30,
    IGB_BMCR_AUTONEG_RESTART = (1U << 6) | (1U << 8) | (1U << 9) | (1U << 12),
    IGB_LINK_UP = 1U << 1,
    IGB_ICR_LINK_CHANGE = 1U << 2, IGB_ICR_RX_DONE = 1U << 7, IGB_ICR_TX_DONE = 1,
    IGB_INTERRUPT_MASK = IGB_ICR_LINK_CHANGE | IGB_ICR_RX_DONE | IGB_ICR_TX_DONE,
    IGB_TX_BASE = 0xe000, IGB_TDBAL = 0, IGB_TDBAH = 4, IGB_TDLEN = 8,
    IGB_TDH = 16, IGB_TDT = 24, IGB_TXDCTL = 40, IGB_TX_QUEUE_EN = 1U << 25,
    IGB_ADVTXD_DATA = 0x00300000, IGB_ADVTXD_EOP = 1U << 24,
    IGB_ADVTXD_FCS = 1U << 25, IGB_ADVTXD_RS = 1U << 27, IGB_ADVTXD_EXT = 1U << 29,
    IGB_ADVTXD_LENGTH_SHIFT = 14, IGB_TXD_DD = 1,
    IGB_MASK = KERNEL_IGB_RX_DEPTH - 1,
    IGB_RX_QUEUE_STRIDE = 0x40,
    IGB_TX_STORAGE = KERNEL_IGB_RX_DEPTH * KERNEL_IGB_RX_PAYLOAD,
};

struct igb_tx_descriptor {
    uint64_t physical;
    uint32_t command_length, status_length;
};

union igb_rx_descriptor {
    struct { uint64_t payload, header; } read;
    struct {
        uint16_t packet_info, header_info;
        uint32_t checksum_or_hash, status;
        uint16_t payload_bytes, vlan;
    } writeback;
};

_Static_assert(sizeof(union igb_rx_descriptor) == 16, "82576 receive descriptor");
_Static_assert(KERNEL_IGB_RX_DEPTH % 8 == 0, "82576 receive ring length alignment");
_Static_assert(sizeof(struct igb_tx_descriptor) == 16, "82576 transmit descriptor");

enum igb_state { IGB_READY, IGB_STOPPING, IGB_STOPPED, IGB_FAILED };
enum igb_queue_state { IGB_QUEUE_OFF, IGB_QUEUE_ON, IGB_QUEUE_STOPPING, IGB_QUEUE_FAILED };

struct igb_receive_queue {
    union igb_rx_descriptor *ring;
    void *owners[KERNEL_IGB_RX_DEPTH];
    unsigned head, tail;
    enum igb_queue_state state;
};

struct kernel_igb_rx {
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct pci_function function;
    struct kernel_raw_lock lock;
    struct kernel_wait_queue progress;
    volatile uint8_t *registers;
    struct igb_receive_queue queues[2];
    uint64_t dma_base;
    struct igb_tx_descriptor *tx_ring;
    uint64_t tx_dma_base;
    uint8_t *tx_buffers;
    uint32_t timebase, irq;
    unsigned dma_order, tx_dma_order;
    unsigned tx_head, tx_tail, tx_published;
    bool tx_reserving;
    uint8_t mac[6];
    enum igb_state state;
};

static uint32_t read_register(const struct kernel_igb_rx *rx, unsigned offset)
{ return *(volatile const uint32_t *)(rx->registers + offset); }

static void write_register(const struct kernel_igb_rx *rx, unsigned offset, uint32_t value)
{
    arch_io_barrier();
    *(volatile uint32_t *)(rx->registers + offset) = value;
}

static int wait_register(const struct kernel_igb_rx *rx, unsigned offset, uint32_t mask,
    uint32_t expected)
{
    uint64_t start = arch_time_read_ordered();
    do {
        uint32_t value = read_register(rx, offset);
        if (value == UINT32_MAX) return -KERNEL_EIO;
        if ((value & mask) == expected) return 0;
    } while (arch_time_read_ordered() - start < rx->timebase / 10);
    return -KERNEL_ETIMEDOUT;
}

static int reset(const struct kernel_igb_rx *rx)
{
    write_register(rx, IGB_IMC, UINT32_MAX);
    uint32_t control = read_register(rx, IGB_CTRL);
    if (control == UINT32_MAX) return -KERNEL_EIO;
    write_register(rx, IGB_CTRL, control | IGB_CTRL_RESET);
    return wait_register(rx, IGB_CTRL, IGB_CTRL_RESET, 0);
}

static void set_bus_master(const struct kernel_igb_rx *rx, int enabled)
{
    const struct pci_function *function = &rx->function;
    struct pci_host *host = function->host;
    host->write(host->context, function->bdf, 4, 2,
        (function->command & ~0x407U) | 2U |
        (enabled ? 4U : 0U) | (rx->irq && enabled ? 0U : 0x400U));
    arch_io_barrier();
}

static void interrupt(void *owner)
{
    struct kernel_igb_rx *rx = owner;
    if (read_register(rx, IGB_ICR) & IGB_INTERRUPT_MASK)
        (void)kernel_wait_queue_wake_all(&rx->progress);
}

static void release_storage(struct kernel_igb_rx *rx)
{
    struct pci_host *host = rx->function.host;
    if (rx->irq) host->unregister_irq(host->context, rx->irq, rx);
    if (kernel_wait_queue_close(&rx->progress) || kernel_wait_queue_destroy(&rx->progress))
        __builtin_trap();
    if (rx->tx_dma_base &&
        physical_page_release_order(rx->allocator, rx->tx_dma_base, rx->tx_dma_order))
        __builtin_trap();
    if (rx->dma_base && physical_page_release_order(rx->allocator, rx->dma_base, rx->dma_order))
        __builtin_trap();
    if (rx->function.assigned && pci_function_restore(&rx->function))
        __builtin_trap();
    kernel_heap_release(rx->heap, rx);
}

static unsigned rx_base(unsigned id)
{ return IGB_RX_BASE + IGB_RX_QUEUE_STRIDE * id; }

static void queue_configure(struct kernel_igb_rx *rx, unsigned id)
{
    struct igb_receive_queue *queue = &rx->queues[id];
    uint64_t ring = rx->dma_base + id * KERNEL_IGB_RX_DEPTH * sizeof(*queue->ring);
    unsigned base = rx_base(id);
    write_register(rx, base + IGB_RXDCTL, 0);
    write_register(rx, base + IGB_RDBAL, (uint32_t)ring);
    write_register(rx, base + IGB_RDBAH, (uint32_t)(ring >> 32));
    write_register(rx, base + IGB_RDLEN, KERNEL_IGB_RX_DEPTH * sizeof(*queue->ring));
    write_register(rx, base + IGB_RDH, 0);
    write_register(rx, base + IGB_RDT, 0);
    write_register(rx, base + IGB_SRRCTL, IGB_RX_DROP | IGB_RX_HEADER_SPLIT |
        (KERNEL_IGB_RX_HEADER << 2) | (KERNEL_IGB_RX_PAYLOAD >> 10));
    queue->head = queue->tail = 0;
}

int kernel_igb_rx_open(struct kernel_igb_rx **out, struct pci_host *host, uint16_t bdf,
    struct kernel_heap *heap, struct physical_page_allocator *allocator, uint32_t timebase)
{
    struct pci_function probe = {0};
    if (!out || !host || !host->map || !heap || !allocator || !timebase)
        return -KERNEL_EINVAL;
    if (pci_function_probe(host, bdf, &probe) || probe.identity != IGB_DEVICE)
        return -KERNEL_ENODEV;
    struct kernel_igb_rx *rx;
    if (kernel_heap_allocate_zeroed(heap, 1, sizeof(*rx), (void **)&rx))
        return -KERNEL_ENOMEM;
    rx->heap = heap;
    rx->allocator = allocator;
    rx->function = probe;
    rx->timebase = timebase;
    kernel_raw_lock_init(&rx->lock, KERNEL_RAW_RANK_OBJECT);
    kernel_wait_queue_init(&rx->progress);
    int error = -KERNEL_EIO;
    if (pci_function_assign(&rx->function)) goto fail;
    const struct pci_bar *bar = &rx->function.bars[0];
    if (bar->size < IGB_TX_BASE + IGB_TXDCTL + 4) goto fail;
    rx->registers = host->map(host->context, bar->address, bar->size);
    if (!rx->registers) goto fail;
    unsigned rx_ring_bytes = KERNEL_IGB_RX_DEPTH * sizeof(*rx->queues[0].ring);
    unsigned tx_ring_bytes = KERNEL_IGB_RX_DEPTH * sizeof(*rx->tx_ring);
    unsigned control_bytes = 2 * rx_ring_bytes + tx_ring_bytes;
    while (BOAROS_PAGE_SIZE << rx->dma_order < control_bytes)
        rx->dma_order++;
    while (BOAROS_PAGE_SIZE << rx->tx_dma_order < IGB_TX_STORAGE)
        rx->tx_dma_order++;
    error = -KERNEL_ENOMEM;
    if (physical_page_allocate_order(allocator, rx->dma_order, &rx->dma_base))
        goto fail;
    if (physical_page_allocate_order(allocator, rx->tx_dma_order, &rx->tx_dma_base))
        goto fail;
    void *mapping;
    error = -KERNEL_EIO;
    if (physical_page_map_owned(allocator, rx->dma_base, &mapping)) goto fail;
    rx->queues[0].ring = mapping;
    rx->queues[1].ring = (void *)((uint8_t *)mapping + rx_ring_bytes);
    rx->tx_ring = (void *)((uint8_t *)mapping + 2 * rx_ring_bytes);
    memset(mapping, 0, control_bytes);
    if (physical_page_map_owned(allocator, rx->tx_dma_base, &mapping)) goto fail;
    rx->tx_buffers = mapping;
    error = reset(rx);
    if (error) goto fail;
    write_register(rx, IGB_MDIC, IGB_MDIC_PHY1 | IGB_MDIC_WRITE | IGB_BMCR_AUTONEG_RESTART);
    error = wait_register(rx, IGB_MDIC, IGB_MDIC_READY, IGB_MDIC_READY);
    if (!error && (read_register(rx, IGB_MDIC) & IGB_MDIC_ERROR)) error = -KERNEL_EIO;
    if (error) goto fail;
    uint32_t mac_lower = read_register(rx, IGB_RAL0);
    uint16_t mac_upper = read_register(rx, IGB_RAH0);
    memcpy(rx->mac, &mac_lower, sizeof(mac_lower));
    memcpy(rx->mac + sizeof(mac_lower), &mac_upper, sizeof(mac_upper));
    write_register(rx, IGB_IMC, UINT32_MAX);
    (void)read_register(rx, IGB_ICR);
    write_register(rx, IGB_RCTL, 0);
    queue_configure(rx, 0);
    for (unsigned i = 0; i < 128; i++) write_register(rx, IGB_MTA + 4 * i, UINT32_MAX);
    write_register(rx, IGB_MRQC, 0);
    write_register(rx, IGB_RFCTL, 0xc1); /* Default extended protocol filters disabled. */
    uint64_t ring = rx->dma_base + 2 * rx_ring_bytes;
    write_register(rx, IGB_TCTL, 0);
    write_register(rx, IGB_TX_BASE + IGB_TXDCTL, 0);
    write_register(rx, IGB_TX_BASE + IGB_TDBAL, (uint32_t)ring);
    write_register(rx, IGB_TX_BASE + IGB_TDBAH, (uint32_t)(ring >> 32));
    write_register(rx, IGB_TX_BASE + IGB_TDLEN, tx_ring_bytes);
    write_register(rx, IGB_TX_BASE + IGB_TDH, 0);
    write_register(rx, IGB_TX_BASE + IGB_TDT, 0);
    unsigned source = host->interrupt_source ?
        host->interrupt_source(host->context, bdf, probe.interrupt_pin) : 0;
    if (source && host->register_irq && host->unregister_irq &&
        host->register_irq(host->context, source, interrupt, rx)) rx->irq = source;
    set_bus_master(rx, 1);
    write_register(rx, IGB_RX_BASE + IGB_RXDCTL, IGB_RX_QUEUE_EN);
    rx->queues[0].state = IGB_QUEUE_ON;
    write_register(rx, IGB_RCTL, IGB_RCTL_SECRC | IGB_RCTL_BAM | IGB_RCTL_MPE | IGB_RCTL_EN);
    write_register(rx, IGB_TX_BASE + IGB_TXDCTL, IGB_TX_QUEUE_EN);
    write_register(rx, IGB_TCTL, IGB_TCTL_EN | IGB_TCTL_PSP);
    if (rx->irq) write_register(rx, IGB_IMS, IGB_INTERRUPT_MASK);
    rx->state = IGB_READY;
    *out = rx;
    return 0;
fail:
    release_storage(rx);
    return error;
}

int kernel_igb_open_optional(struct kernel_igb_rx **out, struct pci_host *host,
    struct kernel_heap *heap, struct physical_page_allocator *allocator, uint32_t timebase)
{
    if (!host) return 0;
    for (unsigned device = 0; device < 32; device++) {
        uint16_t bdf = device << 3;
        if (host->read(host->context, bdf, 0, 4) == IGB_DEVICE)
            return kernel_igb_rx_open(out, host, bdf, heap, allocator, timebase);
    }
    return 0;
}

struct kernel_wait_queue *kernel_igb_progress(struct kernel_igb_rx *rx)
{ return &rx->progress; }

int kernel_igb_irq_available(const struct kernel_igb_rx *rx)
{ return rx->irq != 0; }

int kernel_igb_pending(struct kernel_igb_rx *rx, int may_refill)
{
    KERNEL_IRQ_LOCK_SCOPE(guard, &rx->lock);
    if (rx->state != IGB_READY) return 0;
    const struct igb_receive_queue *queue = &rx->queues[0];
    const volatile union igb_rx_descriptor *desc = &queue->ring[queue->head];
    const volatile struct igb_tx_descriptor *tx = &rx->tx_ring[rx->tx_head];
    return (may_refill && ((queue->tail + 1) & IGB_MASK) != queue->head) ||
        (queue->head != queue->tail && (desc->writeback.status & IGB_RXD_DD)) ||
        (rx->tx_head != rx->tx_published && (tx->status_length & IGB_TXD_DD));
}

int kernel_igb_rx_link_up(const struct kernel_igb_rx *rx)
{
    uint32_t status = read_register(rx, IGB_STATUS);
    return status != UINT32_MAX && (status & IGB_LINK_UP);
}

void kernel_igb_mac_address(const struct kernel_igb_rx *rx, uint8_t address[6])
{ memcpy(address, rx->mac, sizeof(rx->mac)); }

static unsigned queue_post(struct kernel_igb_rx *rx, unsigned id,
    const struct kernel_igb_rx_buffer *buffers, unsigned count)
{
    KERNEL_IRQ_LOCK_SCOPE(guard, &rx->lock);
    struct igb_receive_queue *queue = &rx->queues[id];
    if (rx->state != IGB_READY || queue->state != IGB_QUEUE_ON) return 0;
    unsigned accepted = 0;
    while (accepted < count && ((queue->tail + 1) & IGB_MASK) != queue->head) {
        struct kernel_igb_rx_buffer buffer = buffers[accepted];
        if (!buffer.owner || !buffer.physical || !buffer.header_physical ||
            (buffer.physical & (KERNEL_IGB_RX_PAYLOAD - 1)) ||
            (buffer.header_physical & (KERNEL_IGB_RX_HEADER - 1))) break;
        queue->owners[queue->tail] = buffer.owner;
        queue->ring[queue->tail].read.payload = buffer.physical;
        queue->ring[queue->tail].read.header = buffer.header_physical;
        queue->tail = (queue->tail + 1) & IGB_MASK;
        accepted++;
    }
    if (accepted) {
        arch_dma_barrier();
        write_register(rx, rx_base(id) + IGB_RDT, queue->tail);
    }
    return accepted;
}

static unsigned queue_reap(struct kernel_igb_rx *rx, unsigned id,
    struct kernel_igb_rx_packet *packets, unsigned max)
{
    KERNEL_IRQ_LOCK_SCOPE(guard, &rx->lock);
    if (rx->state != IGB_READY && rx->state != IGB_STOPPED) return 0;
    struct igb_receive_queue *queue = &rx->queues[id];
    if (queue->state == IGB_QUEUE_STOPPING || queue->state == IGB_QUEUE_FAILED) return 0;
    unsigned count = 0;
    while (count < max && queue->head != queue->tail) {
        unsigned index = queue->head;
        volatile union igb_rx_descriptor *desc = &queue->ring[index];
        uint32_t status = desc->writeback.status;
        if (!(status & IGB_RXD_DD) && queue->state == IGB_QUEUE_ON) break;
        struct kernel_igb_rx_packet *p = &packets[count++];
        p->owner = queue->owners[index];
        p->header_bytes = 0;
        p->payload_bytes = 0;
        p->vlan = 0;
        p->split = 0;
        p->end = 0;
        p->error = -KERNEL_ECANCELED;
        if (status & IGB_RXD_DD) {
            arch_dma_barrier();
            uint16_t info = desc->writeback.header_info;
            p->end = !!(status & IGB_RXD_EOP);
            p->split = (info & IGB_RXD_SPLIT) && !(status & IGB_RXD_HBO);
            if (p->split)
                p->header_bytes = (info >> IGB_RXD_HEADER_SHIFT) & IGB_RXD_HEADER_MASK;
            p->payload_bytes = desc->writeback.payload_bytes;
            p->vlan = desc->writeback.vlan;
            p->error = 0;
            if (!p->end || (status & IGB_RXD_FRAME_ERRORS) ||
                p->header_bytes > KERNEL_IGB_RX_HEADER || p->payload_bytes > KERNEL_IGB_RX_PAYLOAD) {
                p->header_bytes = 0;
                p->payload_bytes = 0;
                p->error = -KERNEL_EIO;
            }
        }
        queue->owners[index] = NULL;
        queue->head = (index + 1) & IGB_MASK;
    }
    return count;
}

unsigned kernel_igb_rx_post(struct kernel_igb_rx *rx,
    const struct kernel_igb_rx_buffer *buffers, unsigned count)
{ return queue_post(rx, 0, buffers, count); }

unsigned kernel_igb_rx_reap(struct kernel_igb_rx *rx,
    struct kernel_igb_rx_packet *packets, unsigned max)
{ return queue_reap(rx, 0, packets, max); }

unsigned kernel_igb_zc_post(struct kernel_igb_rx *rx,
    const struct kernel_igb_rx_buffer *buffers, unsigned count)
{ return queue_post(rx, 1, buffers, count); }

unsigned kernel_igb_zc_reap(struct kernel_igb_rx *rx,
    struct kernel_igb_rx_packet *packets, unsigned max)
{ return queue_reap(rx, 1, packets, max); }

unsigned kernel_igb_zc_space(struct kernel_igb_rx *rx)
{
    KERNEL_IRQ_LOCK_SCOPE(guard, &rx->lock);
    struct igb_receive_queue *queue = &rx->queues[1];
    if (rx->state != IGB_READY || queue->state != IGB_QUEUE_ON) return 0;
    return (queue->head - queue->tail - 1) & IGB_MASK;
}

int kernel_igb_zc_pending(struct kernel_igb_rx *rx)
{
    KERNEL_IRQ_LOCK_SCOPE(guard, &rx->lock);
    struct igb_receive_queue *queue = &rx->queues[1];
    if (rx->state != IGB_READY || queue->state != IGB_QUEUE_ON || queue->head == queue->tail)
        return 0;
    const volatile union igb_rx_descriptor *desc = &queue->ring[queue->head];
    return (desc->writeback.status & IGB_RXD_DD) != 0;
}

int kernel_igb_zc_bind(struct kernel_igb_rx *rx, const uint8_t address[6])
{
    if ((address[0] & 3) != 2 || !memcmp(rx->mac, address, 6)) return -KERNEL_EINVAL;
    KERNEL_IRQ_LOCK_SCOPE(guard, &rx->lock);
    struct igb_receive_queue *queue = &rx->queues[1];
    if (rx->state != IGB_READY) return -KERNEL_ENODEV;
    if (queue->state != IGB_QUEUE_OFF || queue->head != queue->tail) return -KERNEL_EBUSY;
    queue_configure(rx, 1);
    uint32_t mac_lower;
    uint16_t mac_upper;
    memcpy(&mac_lower, address, sizeof(mac_lower));
    memcpy(&mac_upper, address + sizeof(mac_lower), sizeof(mac_upper));
    write_register(rx, IGB_RAL1, mac_lower);
    write_register(rx, IGB_RAH1, mac_upper | IGB_RAH_VALID | IGB_RAH_POOL1);
    write_register(rx, IGB_RAH0, (read_register(rx, IGB_RAH0) & UINT16_MAX) |
        IGB_RAH_VALID | IGB_RAH_POOL0);
    write_register(rx, IGB_VMOLR0, IGB_VMOLR_MULTICAST | IGB_VMOLR_BROADCAST);
    write_register(rx, IGB_VMOLR1, 0);
    write_register(rx, IGB_VTCTL, IGB_VTCTL_NO_DEFAULT);
    write_register(rx, IGB_VFRE, 3);
    write_register(rx, rx_base(1) + IGB_RXDCTL, IGB_RX_QUEUE_EN);
    /* Enabling pool steering is the only point at which the secondary MAC
     * becomes reachable. The primary queue and its descriptors stay live. */
    write_register(rx, IGB_MRQC, IGB_MRQC_VMDQ);
    queue->state = IGB_QUEUE_ON;
    return 0;
}

int kernel_igb_zc_stop(struct kernel_igb_rx *rx)
{
    struct igb_receive_queue *queue = &rx->queues[1];
    unsigned offset = rx_base(1) + IGB_RXDCTL;
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &rx->lock);
        if (queue->state == IGB_QUEUE_OFF) return 0;
        if (queue->state == IGB_QUEUE_STOPPING || rx->state != IGB_READY)
            return -KERNEL_EBUSY;
        queue->state = IGB_QUEUE_STOPPING;
        write_register(rx, IGB_RAH1, 0);
        write_register(rx, IGB_VFRE, 1);
        write_register(rx, offset, 0);
        write_register(rx, IGB_MRQC, 0);
    }
    int error = wait_register(rx, offset, IGB_RX_QUEUE_EN, 0);
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &rx->lock);
        queue->state = error ? IGB_QUEUE_FAILED : IGB_QUEUE_OFF;
    }
    return error;
}

static unsigned tx_reap_locked(struct kernel_igb_rx *rx)
{
    unsigned count = 0;
    while (rx->tx_head != rx->tx_published) {
        const volatile struct igb_tx_descriptor *desc = &rx->tx_ring[rx->tx_head];
        if (!(desc->status_length & IGB_TXD_DD)) break;
        arch_dma_barrier();
        rx->tx_head = (rx->tx_head + 1) & IGB_MASK;
        count++;
    }
    return count;
}

static void tx_flush_locked(struct kernel_igb_rx *rx)
{
    if (rx->tx_tail == rx->tx_published) return;
    rx->tx_published = rx->tx_tail;
    arch_dma_barrier();
    write_register(rx, IGB_TX_BASE + IGB_TDT, rx->tx_tail);
}

int kernel_igb_tx_copy(struct kernel_igb_rx *rx, unsigned length,
    int (*copy)(const void *, void *, uint32_t), const void *context, int more)
{
    if (length < 14 || length > 1518) return -KERNEL_EMSGSIZE;
    unsigned index, next;
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &rx->lock);
        if (rx->state != IGB_READY) return -KERNEL_ENODEV;
        if (rx->tx_reserving) return -KERNEL_EAGAIN;
        index = rx->tx_tail;
        next = (index + 1) & IGB_MASK;
        if (next == rx->tx_head) {
            tx_reap_locked(rx);
            if (next == rx->tx_head) {
                tx_flush_locked(rx);
                return -KERNEL_EAGAIN;
            }
        }
        rx->tx_reserving = true;
    }
    unsigned offset = index * KERNEL_IGB_RX_PAYLOAD;
    int copied = copy(context, rx->tx_buffers + offset, length);
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &rx->lock);
        rx->tx_reserving = false;
        if (!copied) return -KERNEL_EFAULT;
        rx->tx_ring[index] = (struct igb_tx_descriptor){
            rx->tx_dma_base + offset,
            length | IGB_ADVTXD_DATA | IGB_ADVTXD_EOP | IGB_ADVTXD_FCS |
                IGB_ADVTXD_RS | IGB_ADVTXD_EXT,
            length << IGB_ADVTXD_LENGTH_SHIFT,
        };
        rx->tx_tail = next;
        if (!more) tx_flush_locked(rx);
    }
    return 0;
}

void kernel_igb_tx_flush(struct kernel_igb_rx *rx)
{
    KERNEL_IRQ_LOCK_SCOPE(guard, &rx->lock);
    if (rx->state == IGB_READY) tx_flush_locked(rx);
}

unsigned kernel_igb_tx_reap(struct kernel_igb_rx *rx)
{
    KERNEL_IRQ_LOCK_SCOPE(guard, &rx->lock);
    return rx->state == IGB_READY ? tx_reap_locked(rx) : 0;
}

int kernel_igb_rx_stop(struct kernel_igb_rx *rx)
{
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &rx->lock);
        if (rx->state == IGB_STOPPED) return 0;
        if (rx->state == IGB_STOPPING || rx->tx_reserving ||
            rx->queues[1].state == IGB_QUEUE_STOPPING) return -KERNEL_EBUSY;
        rx->state = IGB_STOPPING;
    }
    write_register(rx, IGB_IMC, UINT32_MAX);
    write_register(rx, IGB_RCTL, 0);
    write_register(rx, IGB_RX_BASE + IGB_RXDCTL, 0);
    write_register(rx, rx_base(1) + IGB_RXDCTL, 0);
    write_register(rx, IGB_TCTL, 0);
    write_register(rx, IGB_TX_BASE + IGB_TXDCTL, 0);
    int error = wait_register(rx, IGB_RX_BASE + IGB_RXDCTL, IGB_RX_QUEUE_EN, 0);
    if (!error) error = wait_register(rx, rx_base(1) + IGB_RXDCTL, IGB_RX_QUEUE_EN, 0);
    if (!error) error = wait_register(rx, IGB_TX_BASE + IGB_TXDCTL, IGB_TX_QUEUE_EN, 0);
    set_bus_master(rx, 0);
    if (!error) error = reset(rx);
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &rx->lock);
        rx->state = error ? IGB_FAILED : IGB_STOPPED;
        if (!error) rx->queues[0].state = rx->queues[1].state = IGB_QUEUE_OFF;
    }
    return error;
}

int kernel_igb_rx_destroy(struct kernel_igb_rx *rx)
{
    if (rx->state != IGB_STOPPED || rx->queues[0].head != rx->queues[0].tail ||
        rx->queues[1].head != rx->queues[1].tail) return -KERNEL_EBUSY;
    release_storage(rx);
    return 0;
}
