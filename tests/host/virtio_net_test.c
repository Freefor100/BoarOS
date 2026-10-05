#include <arch/riscv/virtio_mmio_net.h>
#include <kernel/console.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void *calloc(size_t, size_t);
extern void free(void *);

/* Decode the device protocol without importing the driver's queue layout. */
enum {
    MMIO_MAGIC = 0x00, MMIO_VERSION = 0x04, MMIO_DEVICE_ID = 0x08,
    MMIO_DEVICE_FEATURES = 0x10, MMIO_DEVICE_FEATURES_SEL = 0x14,
    MMIO_DRIVER_FEATURES = 0x20, MMIO_DRIVER_FEATURES_SEL = 0x24,
    MMIO_GUEST_PAGE_SIZE = 0x28, MMIO_QUEUE_SEL = 0x30,
    MMIO_QUEUE_NUM_MAX = 0x34, MMIO_QUEUE_NUM = 0x38,
    MMIO_QUEUE_ALIGN = 0x3c, MMIO_QUEUE_PFN = 0x40,
    MMIO_QUEUE_READY = 0x44, MMIO_QUEUE_NOTIFY = 0x50,
    MMIO_INTERRUPT_STATUS = 0x60, MMIO_INTERRUPT_ACK = 0x64,
    MMIO_STATUS = 0x70, MMIO_QUEUE_DESC_LOW = 0x80,
    MMIO_QUEUE_DESC_HIGH = 0x84, MMIO_QUEUE_AVAIL_LOW = 0x90,
    MMIO_QUEUE_AVAIL_HIGH = 0x94, MMIO_QUEUE_USED_LOW = 0xa0,
    MMIO_QUEUE_USED_HIGH = 0xa4, MMIO_CONFIG = 0x100,
};
enum { FEATURE_MAC = 1U << 5, FEATURE_STATUS = 1U << 16, FEATURE_INDIRECT = 1U << 28 };
enum { DESC_NEXT = 1U, DESC_WRITE = 2U, DESC_INDIRECT = 4U };
enum { STATUS_FEATURES_OK = 8, STATUS_DRIVER_OK = 4, STATUS_DEVICE_NEEDS_RESET = 64 };

struct allocation { uint64_t phys; void *data; unsigned order; };
struct model_queue {
    uint32_t maximum, number, alignment, pfn, ready;
    uint64_t descriptor, available, used;
    uint16_t observed_available;
    uint16_t pending[256];
    unsigned pending_count, notifications;
};
struct wire_descriptor { uint64_t address; uint32_t length; uint16_t flags, next; };

static const unsigned char device_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static struct physical_page_allocator allocator;
static struct allocation memory[8];
static struct model_queue queues[2];
static uint32_t regs[128], selected, low_features, high_features;
static uint32_t driver_features[2], guest_page_size;
static unsigned allocation_count, allocation_calls, live, allocation_fail;
static unsigned reset_calls, reject_reset_from, reject_features, reject_plic;
static uint64_t now, dma_base;
static void (*interrupt_fn)(void *);
static void *interrupt_owner;

static uint16_t read16(const void *data)
{
    const unsigned char *p = data;
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t read32(const void *data)
{
    const unsigned char *p = data;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t read64(const void *data)
{
    const unsigned char *p = data;
    return read32(p) | ((uint64_t)read32(p + 4) << 32);
}
static void write16(void *data, uint16_t value)
{
    unsigned char *p = data;
    p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8);
}
static void write32(void *data, uint32_t value)
{
    unsigned char *p = data;
    for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (8 * i));
}
static void *resolve(uint64_t p)
{
    for (unsigned i = 0; i < allocation_count; i++) {
        if (memory[i].data && p >= memory[i].phys &&
            p - memory[i].phys < (UINT64_C(4096) << memory[i].order))
            return (char *)memory[i].data + p - memory[i].phys;
    }
    assert(0); return 0;
}
/* 测试自有的 DMA 可见缓冲：不参与驱动分配计数（live/allocation_calls）。 */
static void *model_extra_memory(uint64_t *phys)
{
    unsigned i = allocation_count++;
    assert(i < 8);
    memory[i] = (struct allocation){dma_base + i * UINT64_C(0x100000), calloc(1, 4096), 0};
    assert(memory[i].data); *phys = memory[i].phys; return memory[i].data;
}
static void model_extra_free(uint64_t phys)
{
    for (unsigned i = 0; i < allocation_count; i++)
        if (memory[i].data && memory[i].phys == phys) { free(memory[i].data); memory[i].data = 0; return; }
    assert(0);
}
static void *released_owner[8]; static unsigned released_count;
static void collect_owner(void *owner)
{
    assert(released_count < 8); released_owner[released_count++] = owner;
}
static struct wire_descriptor raw_descriptor(uint64_t table, unsigned id)
{
    const unsigned char *p = resolve(table + 16U * id);
    return (struct wire_descriptor){read64(p), read32(p + 8), read16(p + 12), read16(p + 14)};
}
static struct wire_descriptor descriptor(unsigned q, unsigned id)
{
    assert(q < 2 && id < queues[q].number);
    struct wire_descriptor result = raw_descriptor(queues[q].descriptor, id);
    if (result.flags & DESC_INDIRECT) {
        assert(q == 1 && !(result.flags & ~DESC_INDIRECT) && result.next == 0);
        assert(result.length && result.length % 16U == 0);
        (void)resolve(result.address + result.length - 1U);
        return result;
    }
    assert(!(result.flags & ~DESC_WRITE) && result.next == 0);
    assert((result.flags & DESC_WRITE) == (q == 0 ? DESC_WRITE : 0U));
    (void)resolve(result.address + result.length - 1U);
    return result;
}
static struct wire_descriptor table_entry(uint64_t table, unsigned index)
{
    struct wire_descriptor entry = raw_descriptor(table, index);
    (void)resolve(entry.address + entry.length - 1U);
    return entry;
}
static uint16_t available_index(unsigned q)
{ return read16((char *)resolve(queues[q].available) + 2); }
static uint16_t used_index(unsigned q)
{ return read16((char *)resolve(queues[q].used) + 2); }
static void observe_notification(unsigned q)
{
    assert(q < 2 && (regs[MMIO_STATUS / 4] & STATUS_DRIVER_OK));
    struct model_queue *queue = &queues[q];
    assert(queue->number && (queue->pfn || queue->ready));
    uint16_t index = available_index(q);
    unsigned count = (uint16_t)(index - queue->observed_available);
    assert(count <= queue->number - queue->pending_count);
    const unsigned char *a = resolve(queue->available);
    while (queue->observed_available != index) {
        unsigned id = read16(a + 4 + 2U * (queue->observed_available % queue->number));
        assert(id < queue->number);
        for (unsigned i = 0; i < queue->pending_count; i++) assert(queue->pending[i] != id);
        (void)descriptor(q, id);
        queue->pending[queue->pending_count++] = (uint16_t)id;
        queue->observed_available++;
    }
    queue->notifications++;
}
uint32_t riscv_virtio_net_read(volatile uint8_t *base, unsigned offset)
{
    assert(base == (volatile uint8_t *)regs && offset < sizeof(regs) && !(offset & 3U));
    struct model_queue *queue = &queues[selected];
    switch (offset) {
    case MMIO_DEVICE_FEATURES:
        if (!regs[MMIO_DEVICE_FEATURES_SEL / 4]) return low_features;
        return regs[MMIO_VERSION / 4] == 1 ? 0U : high_features;
    case MMIO_QUEUE_NUM_MAX: return queue->maximum;
    case MMIO_QUEUE_NUM: return queue->number;
    case MMIO_QUEUE_ALIGN: return queue->alignment;
    case MMIO_QUEUE_PFN: return queue->pfn;
    case MMIO_QUEUE_READY: return queue->ready;
    case MMIO_QUEUE_DESC_LOW: return (uint32_t)queue->descriptor;
    case MMIO_QUEUE_DESC_HIGH: return (uint32_t)(queue->descriptor >> 32);
    case MMIO_QUEUE_AVAIL_LOW: return (uint32_t)queue->available;
    case MMIO_QUEUE_AVAIL_HIGH: return (uint32_t)(queue->available >> 32);
    case MMIO_QUEUE_USED_LOW: return (uint32_t)queue->used;
    case MMIO_QUEUE_USED_HIGH: return (uint32_t)(queue->used >> 32);
    default: return regs[offset / 4];
    }
}
static void address_part(uint64_t *address, unsigned high, uint32_t value)
{
    if (high) *address = (*address & UINT32_MAX) | ((uint64_t)value << 32);
    else *address = (*address & UINT64_C(0xffffffff00000000)) | value;
}
void riscv_virtio_net_write(volatile uint8_t *base, unsigned offset, uint32_t value)
{
    assert(base == (volatile uint8_t *)regs && offset < MMIO_CONFIG && !(offset & 3U));
    if (offset == MMIO_STATUS && value == 0) {
        reset_calls++;
        if (reject_reset_from && reset_calls >= reject_reset_from) return;
        for (unsigned q = 0; q < 2; q++) {
            uint32_t maximum = queues[q].maximum;
            memset(&queues[q], 0, sizeof(queues[q])); queues[q].maximum = maximum;
        }
        regs[MMIO_INTERRUPT_STATUS / 4] = 0;
    }
    if (offset == MMIO_STATUS && reject_features && (value & STATUS_FEATURES_OK))
        value &= ~STATUS_FEATURES_OK;
    if (offset == MMIO_INTERRUPT_ACK) {
        regs[MMIO_INTERRUPT_STATUS / 4] &= ~value; return;
    }
    regs[offset / 4] = value;
    if (offset == MMIO_QUEUE_SEL) { assert(value < 2); selected = value; }
    struct model_queue *queue = &queues[selected];
    switch (offset) {
    case MMIO_DEVICE_FEATURES_SEL: case MMIO_DRIVER_FEATURES_SEL: assert(value < 2); break;
    case MMIO_DRIVER_FEATURES: driver_features[regs[MMIO_DRIVER_FEATURES_SEL / 4]] = value; break;
    case MMIO_GUEST_PAGE_SIZE: guest_page_size = value; break;
    case MMIO_QUEUE_NUM:
        assert(value && value <= queue->maximum && !(value & (value - 1U)));
        queue->number = value; break;
    case MMIO_QUEUE_ALIGN: assert(value && !(value & (value - 1U))); queue->alignment = value; break;
    case MMIO_QUEUE_PFN:
        assert(regs[MMIO_VERSION / 4] == 1 && guest_page_size && queue->alignment);
        queue->pfn = value; queue->descriptor = (uint64_t)value * guest_page_size;
        queue->available = queue->descriptor + 16U * queue->number;
        queue->used = (queue->available + 4 + 2U * queue->number + queue->alignment - 1U) &
                      ~(uint64_t)(queue->alignment - 1U);
        break;
    case MMIO_QUEUE_READY: assert(regs[MMIO_VERSION / 4] == 2); queue->ready = value; break;
    case MMIO_QUEUE_DESC_LOW: address_part(&queue->descriptor, 0, value); break;
    case MMIO_QUEUE_DESC_HIGH: address_part(&queue->descriptor, 1, value); break;
    case MMIO_QUEUE_AVAIL_LOW: address_part(&queue->available, 0, value); break;
    case MMIO_QUEUE_AVAIL_HIGH: address_part(&queue->available, 1, value); break;
    case MMIO_QUEUE_USED_LOW: address_part(&queue->used, 0, value); break;
    case MMIO_QUEUE_USED_HIGH: address_part(&queue->used, 1, value); break;
    case MMIO_QUEUE_NOTIFY: observe_notification(value); break;
    default: break;
    }
}
uint64_t riscv_time_read(void) { return now; }
void kernel_wait_queue_init(struct kernel_wait_queue *q) { memset(q, 0, sizeof(*q)); }
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *q)
{ (void)q; return KERNEL_SCHEDULER_STATUS_OK; }
int riscv_plic_register(uint32_t source, void (*fn)(void *), void *owner)
{
    assert(source == 7 && !interrupt_fn);
    if (reject_plic) return 0;
    interrupt_fn = fn; interrupt_owner = owner; return 1;
}
void riscv_plic_unregister(uint32_t source, void *owner)
{
    assert(source == 7 && interrupt_fn && owner == interrupt_owner);
    interrupt_fn = 0; interrupt_owner = 0;
}
static char console_text[16384]; static size_t console_length;
static int console_contains(const char *needle)
{
    size_t n = 0; while (needle[n]) n++;
    for (size_t i = 0; i + n <= console_length; i++) {
        size_t j = 0; while (j < n && console_text[i + j] == needle[j]) j++;
        if (j == n) return 1;
    }
    return 0;
}
void kernel_console_putc(char character)
{
    if (console_length + 1 < sizeof(console_text)) console_text[console_length++] = character;
    console_text[console_length] = 0;
}
enum physical_page_status physical_page_allocate_order(struct physical_page_allocator *a,
    uint32_t order, uint64_t *p)
{
    assert(a == &allocator);
    if (++allocation_calls == allocation_fail) return PHYSICAL_PAGE_STATUS_EMPTY;
    unsigned i = allocation_count++; assert(i < 8 && order <= 5);
    memory[i] = (struct allocation){dma_base + i * UINT64_C(0x100000),
                                    calloc(1, 4096U << order), order};
    assert(memory[i].data); *p = memory[i].phys; live++; return PHYSICAL_PAGE_STATUS_OK;
}
enum physical_page_status physical_page_resolve(const struct physical_page_allocator *a,
    uint64_t p, void **out)
{ assert(a == &allocator); *out = resolve(p); return PHYSICAL_PAGE_STATUS_OK; }
enum physical_page_status physical_page_release_order(struct physical_page_allocator *a,
    uint64_t p, uint32_t order)
{
    assert(a == &allocator && regs[MMIO_STATUS / 4] == 0 && !interrupt_fn);
    assert(!queues[0].pfn && !queues[0].ready && !queues[1].pfn && !queues[1].ready);
    for (unsigned i = 0; i < allocation_count; i++) if (memory[i].phys == p) {
        assert(memory[i].data && memory[i].order == order);
        free(memory[i].data); memory[i].data = 0; live--; return PHYSICAL_PAGE_STATUS_OK;
    }
    assert(0); return PHYSICAL_PAGE_STATUS_INVALID;
}
static void setup(unsigned version)
{
    assert(!live && !interrupt_fn);
    memset(regs, 0, sizeof(regs)); memset(memory, 0, sizeof(memory));
    memset(queues, 0, sizeof(queues)); memset(driver_features, 0, sizeof(driver_features));
    selected = guest_page_size = allocation_count = allocation_calls = allocation_fail = 0;
    reset_calls = reject_reset_from = reject_features = reject_plic = 0;
    regs[MMIO_MAGIC / 4] = UINT32_C(0x74726976); regs[MMIO_VERSION / 4] = version;
    regs[MMIO_DEVICE_ID / 4] = 1; low_features = FEATURE_MAC | FEATURE_STATUS | FEATURE_INDIRECT; high_features = 1;
    memcpy((char *)regs + MMIO_CONFIG, device_mac, sizeof(device_mac));
    ((unsigned char *)regs)[MMIO_CONFIG + 6] = 1;
    for (unsigned q = 0; q < 2; q++) queues[q].maximum = 256;
    /* Modern queue addresses must exercise the high register banks as well. */
    dma_base = version == 1 ? UINT64_C(0x81000000) : UINT64_C(0x181000000);
    now = 100;
}
static void start(unsigned version, struct riscv_virtio_mmio_net *d)
{
    setup(version); memset(d, 0, sizeof(*d));
    assert(riscv_virtio_mmio_net_init(d, regs, sizeof(regs), &allocator, 100, 7) == 0);
    assert(live == 3 && d->link_up && !memcmp(d->mac, device_mac, sizeof(device_mac)));
    assert(driver_features[0] == (FEATURE_MAC | FEATURE_STATUS | (low_features & FEATURE_INDIRECT)));
    assert(driver_features[1] == (version == 2 ? 1U : 0U));
    assert(queues[0].pending_count == queues[0].number && queues[1].pending_count == 0);
}
static void stop(struct riscv_virtio_mmio_net *d)
{ assert(riscv_virtio_mmio_net_stop(d) == 0 && !live && !d->mmio); }
static void trigger_interrupt(unsigned bits)
{
    assert(interrupt_fn); regs[MMIO_INTERRUPT_STATUS / 4] |= bits;
    interrupt_fn(interrupt_owner);
    assert(!regs[MMIO_INTERRUPT_STATUS / 4]);
}
static unsigned next_head(unsigned q)
{ assert(queues[q].pending_count); return queues[q].pending[0]; }
static void complete(unsigned q, unsigned id, unsigned length, int interrupt)
{
    struct model_queue *queue = &queues[q];
    for (unsigned i = 0; i < queue->pending_count; i++) if (queue->pending[i] == id) {
        for (unsigned j = i + 1; j < queue->pending_count; j++) queue->pending[j - 1] = queue->pending[j];
        queue->pending_count--; break;
    }
    unsigned char *p = resolve(queue->used); uint16_t index = read16(p + 2);
    unsigned char *entry = p + 4 + 8U * (index % queue->number);
    write32(entry, id); write32(entry + 4, length); write16(p + 2, (uint16_t)(index + 1U));
    if (interrupt) trigger_interrupt(1);
}
static unsigned prepare_rx(const void *payload, unsigned length, unsigned flags, unsigned gso)
{
    unsigned id = next_head(0); struct wire_descriptor desc = descriptor(0, id);
    unsigned h = regs[MMIO_VERSION / 4] == 1 ? 10 : 12;
    assert(length + h <= desc.length);
    unsigned char *p = resolve(desc.address); memset(p, 0, h);
    p[0] = (unsigned char)flags; p[1] = (unsigned char)gso;
    memcpy(p + h, payload, length); return id;
}
static void receive(const void *payload, unsigned length)
{
    unsigned id = prepare_rx(payload, length, 0, 0);
    complete(0, id, length + (regs[MMIO_VERSION / 4] == 1 ? 10 : 12), 1);
}
static void failed_owner(struct riscv_virtio_mmio_net *d)
{
    unsigned notifications = queues[0].notifications + queues[1].notifications;
    assert(riscv_virtio_mmio_net_service(d) == -KERNEL_EIO && live == 3);
    assert(d->statistics.errors == 1);
    assert(riscv_virtio_mmio_net_send(d, "x", 1) == -KERNEL_EIO);
    assert(queues[0].notifications + queues[1].notifications == notifications);
}
static void test_loan_budget(unsigned version)
{
    struct riscv_virtio_mmio_net d; start(version, &d); struct riscv_net_frame frames[32];
    for (unsigned i = 0; i < 32; i++) {
        receive("owned", 5); assert(riscv_virtio_mmio_net_receive(&d, &frames[i]) == 1);
        assert(frames[i].size == 5 && !memcmp(frames[i].data, "owned", 5));
        assert(!riscv_virtio_mmio_net_lend(&d, frames[i].buffer));
        assert(!riscv_virtio_mmio_net_service(&d));
    }
    assert(d.loaned == 32); receive("later", 5); struct riscv_net_frame frame;
    assert(riscv_virtio_mmio_net_receive(&d, &frame) == 1);
    assert(riscv_virtio_mmio_net_lend(&d, frame.buffer) == -KERNEL_EAGAIN);
    riscv_virtio_mmio_net_release(&d, frame.buffer);
    for (unsigned i = 0; i < 2; i++) {
        assert(riscv_virtio_mmio_net_stop(&d) == -KERNEL_EBUSY && live == 3 && d.mmio);
        for (unsigned j = 0; j < 32; j++) assert(!memcmp(frames[j].data, "owned", 5));
    }
    for (unsigned i = 0; i < 32; i++) riscv_virtio_mmio_net_release(&d, frames[i].buffer);
    stop(&d); printf("PASS: VirtIO-net v%u DMA loans survive reset, 32-loan budget and final owner release\n", version);
}
static void test_tx_copy_budget(unsigned version)
{
    struct riscv_virtio_mmio_net d; start(version, &d); char input[64]; memset(input, 'x', sizeof(input));
    for (unsigned i = 0; i < 64; i++) assert(riscv_virtio_mmio_net_send(&d, input, sizeof(input)) == 64);
    assert(riscv_virtio_mmio_net_send(&d, input, sizeof(input)) == -KERNEL_EAGAIN);
    memset(input, 'z', sizeof(input)); unsigned id = next_head(1);
    struct wire_descriptor desc = descriptor(1, id); unsigned h = version == 1 ? 10 : 12;
    assert(desc.length == h + sizeof(input));
    assert(((char *)resolve(desc.address))[h] == 'x');
    complete(1, id, 0, 1); assert(!riscv_virtio_mmio_net_service(&d));
    assert(riscv_virtio_mmio_net_send(&d, input, sizeof(input)) == 64);
    complete(1, 99, 0, 1); failed_owner(&d); stop(&d);
    printf("PASS: VirtIO-net v%u TX copies, 64-buffer budget and out-of-range used ID\n", version);
}
static void seed_ring(struct riscv_virtio_mmio_net *d, unsigned q, uint16_t consumed, uint16_t advertised)
{
    /* Public split-ring indices model long prior history; no private FIFO state is patched. */
    write16((char *)resolve(queues[q].used) + 2, consumed);
    write16((char *)resolve(queues[q].available) + 2, advertised);
    d->consumed[q] = consumed; d->available[q] = advertised;
    queues[q].observed_available = advertised;
    unsigned char *a = resolve(queues[q].available);
    for (unsigned i = 0; i < queues[q].pending_count; i++)
        write16(a + 4 + 2U * ((consumed + i) % queues[q].number), queues[q].pending[i]);
}
static void test_index_wrap(unsigned version)
{
    struct riscv_virtio_mmio_net d; start(version, &d);
    seed_ring(&d, 0, 65498U, 65530U); seed_ring(&d, 1, 65530U, 65530U);
    for (unsigned i = 0; i < 40; i++) {
        unsigned char payload[4] = {'w', (unsigned char)i, 'r', 'p'};
        receive(payload, sizeof(payload)); struct riscv_net_frame frame;
        assert(riscv_virtio_mmio_net_receive(&d, &frame) == 1 && frame.size == sizeof(payload));
        assert(!memcmp(frame.data, payload, sizeof(payload)));
        riscv_virtio_mmio_net_release(&d, frame.buffer); assert(!riscv_virtio_mmio_net_service(&d));
        assert(riscv_virtio_mmio_net_send(&d, payload, sizeof(payload)) == 4);
        unsigned id = next_head(1); struct wire_descriptor desc = descriptor(1, id);
        assert(!memcmp((char *)resolve(desc.address) + (version == 1 ? 10 : 12), payload, sizeof(payload)));
        complete(1, id, 0, 1); assert(!riscv_virtio_mmio_net_service(&d));
    }
    assert(used_index(0) == 2 && d.consumed[0] == 2 && available_index(0) == 34 && d.available[0] == 34);
    assert(used_index(1) == 34 && d.consumed[1] == 34 && available_index(1) == 34 && d.available[1] == 34);
    assert(d.statistics.rx_packets == 40 && d.statistics.tx_packets == 40);
    assert(queues[0].pending_count == queues[0].number && queues[1].pending_count == 0);
    stop(&d); printf("PASS: VirtIO-net v%u RX/TX available and used indices cross 65535 to 0\n", version);
}
static void test_used_index(unsigned version, unsigned q, int backwards)
{
    struct riscv_virtio_mmio_net d; start(version, &d);
    if (q) assert(riscv_virtio_mmio_net_send(&d, "x", 1) == 1);
    uint16_t index = backwards ? UINT16_MAX : (uint16_t)(queues[q].pending_count + 1U);
    write16((char *)resolve(queues[q].used) + 2, index); trigger_interrupt(1);
    failed_owner(&d);
    /* A later duplicate-ID failure must not hide a missing index-range guard. */
    assert(d.consumed[q] == 0 && d.statistics.rx_packets == 0);
    stop(&d);
    printf("PASS: VirtIO-net v%u queue%u %s used-index rejected with live DMA owner\n", version, q, backwards ? "backwards" : "excess");
}
static void test_duplicate_id(unsigned version, unsigned q)
{
    struct riscv_virtio_mmio_net d; start(version, &d);
    unsigned length = 0;
    if (q) {
        assert(riscv_virtio_mmio_net_send(&d, "x", 1) == 1);
        assert(riscv_virtio_mmio_net_send(&d, "y", 1) == 1);
    } else length = version == 1 ? 10 : 12;
    unsigned id = next_head(q);
    if (!q) (void)prepare_rx("", 0, 0, 0);
    complete(q, id, length, 0); complete(q, id, length, 1);
    failed_owner(&d); assert(d.consumed[q] == 1); stop(&d);
    printf("PASS: VirtIO-net v%u queue%u duplicate used ID rejected before second owner release\n", version, q);
}
static void test_rx_length(unsigned version, int oversized)
{
    struct riscv_virtio_mmio_net d; start(version, &d);
    unsigned id = prepare_rx("x", 1, 0, 0); struct wire_descriptor desc = descriptor(0, id);
    unsigned length = oversized ? desc.length + 1U : (version == 1 ? 9U : 11U);
    complete(0, id, length, 1); failed_owner(&d); struct riscv_net_frame frame;
    assert(riscv_virtio_mmio_net_receive(&d, &frame) == 0 && d.statistics.rx_packets == 0);
    stop(&d); printf("PASS: VirtIO-net v%u RX %s used length rejected before exposing data\n", version, oversized ? "oversized" : "short-header");
}
static unsigned test_rx_offload(unsigned version, unsigned flags, unsigned gso)
{
    struct riscv_virtio_mmio_net d; start(version, &d);
    unsigned id = prepare_rx("bad", 3, flags, gso);
    complete(0, id, (version == 1 ? 10 : 12) + 3, 1); struct riscv_net_frame frame;
    int accepted = riscv_virtio_mmio_net_receive(&d, &frame);
    if (accepted) {
        fprintf(stderr, "FAIL: VirtIO-net v%u unnegotiated RX flags=%u gso=%u delivered size=%u\n", version, flags, gso, frame.size);
        riscv_virtio_mmio_net_release(&d, frame.buffer);
    } else {
        failed_owner(&d); assert(d.statistics.rx_packets == 0);
        printf("PASS: VirtIO-net v%u unnegotiated RX flags=%u gso=%u rejected\n", version, flags, gso);
    }
    stop(&d); return accepted != 0;
}
static void test_tx_timeout(unsigned version)
{
    struct riscv_virtio_mmio_net d; start(version, &d);
    assert(riscv_virtio_mmio_net_send(&d, "x", 1) == 1);
    now = 599; assert(!riscv_virtio_mmio_net_service(&d) && live == 3);
    now = 600; failed_owner(&d); stop(&d);
    printf("PASS: VirtIO-net v%u TX timeout at five seconds retains DMA until reset\n", version);
}
static void test_configuration_irq(unsigned version)
{
    struct riscv_virtio_mmio_net d; start(version, &d);
    ((unsigned char *)regs)[MMIO_CONFIG + 6] = 0; trigger_interrupt(2);
    assert(!d.link_up && riscv_virtio_mmio_net_send(&d, "x", 1) == -KERNEL_ENETDOWN);
    ((unsigned char *)regs)[MMIO_CONFIG + 6] = 1; trigger_interrupt(2);
    assert(d.link_up && riscv_virtio_mmio_net_send(&d, "x", 1) == 1);
    complete(1, next_head(1), 0, 1); stop(&d);
    printf("PASS: VirtIO-net v%u MAC/config registers and link interrupt decoded independently\n", version);
}
static void test_negotiation_failure(unsigned version, unsigned which)
{
    struct riscv_virtio_mmio_net d = {0}; setup(version);
    int expected = -KERNEL_ENOTSUP; const char *name;
    if (which == 0) { low_features &= ~FEATURE_MAC; name = "missing MAC"; }
    else if (which == 1) { assert(version == 2); high_features = 0; name = "missing VERSION_1"; }
    else { assert(version == 2); reject_features = 1; expected = -KERNEL_EIO; name = "FEATURES_OK refusal"; }
    assert(riscv_virtio_mmio_net_init(&d, regs, sizeof(regs), &allocator, 100, 7) == expected);
    assert(!live && !d.mmio && !interrupt_fn && allocation_calls == 0);
    printf("PASS: VirtIO-net v%u %s resets and leaves no published owner\n", version, name);
}
static void test_allocation_failure(unsigned version, unsigned nth, int reset_rejected)
{
    struct riscv_virtio_mmio_net d = {0}; setup(version); allocation_fail = nth;
    if (reset_rejected) reject_reset_from = 2;
    int expected = reset_rejected ? -KERNEL_EIO : -KERNEL_ENOMEM;
    assert(riscv_virtio_mmio_net_init(&d, regs, sizeof(regs), &allocator, 100, 7) == expected);
    assert(allocation_calls == nth);
    if (reset_rejected) {
        assert(live == nth - 1U && d.mmio);
        uint64_t physical[3] = {d.queue_phys, d.rx_phys, d.tx_phys};
        void *data[3] = {d.queues, d.rx_memory, d.tx_memory};
        for (unsigned i = 0; i < 2; i++) {
            assert(riscv_virtio_mmio_net_stop(&d) == -KERNEL_EIO && live == nth - 1U && d.mmio);
            assert(d.queue_phys == physical[0] && d.rx_phys == physical[1] && d.tx_phys == physical[2]);
            assert(d.queues == data[0] && d.rx_memory == data[1] && d.tx_memory == data[2]);
            for (unsigned j = 0; j < allocation_count; j++) assert(resolve(memory[j].phys) == memory[j].data);
        }
        reject_reset_from = 0; stop(&d);
    } else assert(!live && !d.mmio && !interrupt_fn);
    printf("PASS: VirtIO-net v%u allocation%u failure %s\n", version, nth,
           reset_rejected ? "retains owner through two refused resets, releases after acknowledgement" : "rolls back after reset acknowledgement");
}
static void test_plic_failure(unsigned version, int reset_rejected)
{
    struct riscv_virtio_mmio_net d = {0}; setup(version); reject_plic = 1;
    if (reset_rejected) reject_reset_from = 2;
    assert(riscv_virtio_mmio_net_init(&d, regs, sizeof(regs), &allocator, 100, 7) == -KERNEL_EIO);
    assert(!interrupt_fn);
    if (reset_rejected) {
        assert(live == 3 && d.mmio);
        assert(riscv_virtio_mmio_net_stop(&d) == -KERNEL_EIO && live == 3 && d.mmio);
        reject_reset_from = 0; stop(&d);
    } else assert(!live && !d.mmio);
    printf("PASS: VirtIO-net v%u PLIC registration failure %s\n", version,
           reset_rejected ? "retains DMA owner while reset refused" : "rolls back all DMA allocations");
}
static void test_reset_owner(unsigned version)
{
    struct riscv_virtio_mmio_net d; start(version, &d); receive("borrow", 6); struct riscv_net_frame frame;
    assert(riscv_virtio_mmio_net_receive(&d, &frame) == 1 && !riscv_virtio_mmio_net_lend(&d, frame.buffer));
    reject_reset_from = reset_calls + 1;
    for (unsigned i = 0; i < 2; i++) {
        assert(riscv_virtio_mmio_net_stop(&d) == -KERNEL_EIO && live == 3 && d.mmio && interrupt_fn);
        assert(!memcmp(frame.data, "borrow", 6));
    }
    reject_reset_from = 0;
    assert(riscv_virtio_mmio_net_stop(&d) == -KERNEL_EBUSY && live == 3 && d.mmio && !interrupt_fn);
    assert(!memcmp(frame.data, "borrow", 6)); riscv_virtio_mmio_net_release(&d, frame.buffer); stop(&d);
    printf("PASS: VirtIO-net v%u repeated reset refusal and borrowed frame retain the same DMA owner\n", version);
}

static unsigned test_device_needs_reset(unsigned entry)
{
    static const char *const names[] = {"config-IRQ", "service-without-IRQ", "send-without-IRQ"};
    struct riscv_virtio_mmio_net d; start(2, &d);
    uint16_t advertised[2] = {available_index(0), available_index(1)};
    unsigned notifications = queues[0].notifications + queues[1].notifications;
    /* QEMU virtio_error publishes this modern status bit, then config IRQ. */
    regs[MMIO_STATUS / 4] |= STATUS_DEVICE_NEEDS_RESET;
    if (entry == 0) trigger_interrupt(2);
    int irq_reported = entry != 0 || (d.failed == 1 && d.statistics.errors == 1);
    int result = entry == 2 ? riscv_virtio_mmio_net_send(&d, "x", 1) :
                              riscv_virtio_mmio_net_service(&d);
    int correct = result == -KERNEL_EIO && irq_reported &&
                  d.failed == 1 && d.statistics.errors == 1 && live == 3 &&
                  d.statistics.tx_packets == 0 &&
                  available_index(0) == advertised[0] && available_index(1) == advertised[1] &&
                  queues[0].notifications + queues[1].notifications == notifications;
    if (!correct) {
        fprintf(stderr, "FAIL: VirtIO-net v2 DEVICE_NEEDS_RESET entry=%s result=%d IRQ_reported=%d failed=%u errors=%llu tx_packets=%llu notifications_before=%u notifications_after=%u\n",
                names[entry], result, irq_reported, d.failed, (unsigned long long)d.statistics.errors,
                (unsigned long long)d.statistics.tx_packets, notifications,
                queues[0].notifications + queues[1].notifications);
    } else {
        failed_owner(&d);
        printf("PASS: VirtIO-net v2 DEVICE_NEEDS_RESET via %s fails once and publishes no descriptors\n", names[entry]);
    }
    stop(&d); return !correct;
}

static unsigned test_device_needs_reset_loan(void)
{
    struct riscv_virtio_mmio_net d; start(2, &d); receive("status-loan", 11);
    struct riscv_net_frame frame;
    assert(riscv_virtio_mmio_net_receive(&d, &frame) == 1 && !riscv_virtio_mmio_net_lend(&d, frame.buffer));
    assert(!riscv_virtio_mmio_net_service(&d));
    uint64_t physical[3] = {d.queue_phys, d.rx_phys, d.tx_phys};
    void *data[3] = {d.queues, d.rx_memory, d.tx_memory};
    unsigned notifications = queues[0].notifications + queues[1].notifications;
    regs[MMIO_STATUS / 4] |= STATUS_DEVICE_NEEDS_RESET; trigger_interrupt(2);
    int result = riscv_virtio_mmio_net_service(&d);
    int correct = result == -KERNEL_EIO && d.failed == 1 && d.statistics.errors == 1 &&
                  queues[0].notifications + queues[1].notifications == notifications;
    if (!correct)
        fprintf(stderr, "FAIL: VirtIO-net v2 DEVICE_NEEDS_RESET with loan result=%d failed=%u errors=%llu\n",
                result, d.failed, (unsigned long long)d.statistics.errors);
    reject_reset_from = reset_calls + 1;
    for (unsigned i = 0; i < 2; i++) {
        assert(riscv_virtio_mmio_net_stop(&d) == -KERNEL_EIO && live == 3 && d.mmio);
        assert(d.queue_phys == physical[0] && d.rx_phys == physical[1] && d.tx_phys == physical[2]);
        assert(d.queues == data[0] && d.rx_memory == data[1] && d.tx_memory == data[2]);
        assert(d.loaned == 1 && !memcmp(frame.data, "status-loan", 11));
    }
    reject_reset_from = 0;
    assert(riscv_virtio_mmio_net_stop(&d) == -KERNEL_EBUSY && live == 3 && d.mmio);
    assert(d.loaned == 1 && !memcmp(frame.data, "status-loan", 11));
    riscv_virtio_mmio_net_release(&d, frame.buffer); assert(!d.loaned); stop(&d);
    if (correct)
        puts("PASS: VirtIO-net v2 DEVICE_NEEDS_RESET and refused resets retain DMA owner until final loan return");
    return !correct;
}

static void test_legacy_reserved_status(void)
{
    struct riscv_virtio_mmio_net d; start(1, &d);
    regs[MMIO_STATUS / 4] |= STATUS_DEVICE_NEEDS_RESET; trigger_interrupt(2);
    assert(!riscv_virtio_mmio_net_service(&d) && !d.failed && !d.statistics.errors);
    assert(riscv_virtio_mmio_net_send(&d, "x", 1) == 1);
    complete(1, next_head(1), 0, 1); stop(&d);
    puts("PASS: VirtIO-net v1 reserved status bit does not imply unnegotiated modern reset semantics");
}
static void test_failure_slot_dump(unsigned version)
{
    struct riscv_virtio_mmio_net d; start(version, &d);
    console_length = 0; console_text[0] = 0;
    /* 两个在借 RX、一个在途 TX，再以越界 used id 触发失败快照。 */
    struct riscv_net_frame frames[2];
    receive("ab", 2); assert(riscv_virtio_mmio_net_receive(&d, &frames[0]) == 1);
    receive("cd", 2); assert(riscv_virtio_mmio_net_receive(&d, &frames[1]) == 1);
    assert(!riscv_virtio_mmio_net_lend(&d, frames[0].buffer));
    assert(!riscv_virtio_mmio_net_lend(&d, frames[1].buffer));
    assert(riscv_virtio_mmio_net_send(&d, "xyz", 3) == 3);
    complete(1, 99, 0, 1);
    assert(riscv_virtio_mmio_net_service(&d) == -KERNEL_EIO);
    assert(console_contains("VirtIO-net failed reason=head-owner") != 0);
    assert(console_contains("VirtIO-net slots version=") != 0);
    /* diagnostic_hex 固定输出 16 位十六进制，槽快照断言按同一格式。 */
    assert(console_contains("loaned=0x0000000000000002") != 0 &&
           console_contains("pending=0x0000000000000000") != 0 &&
           console_contains("done=0x0000000000000000") != 0);
    assert(console_contains("VirtIO-net rx buffer=0x0000000000000000"
                            " state=0x0000000000000004") != 0);
    assert(console_contains("VirtIO-net tx buffer=0x") != 0 &&
           console_contains("owner=0x0000000000000000") != 0);
    riscv_virtio_mmio_net_release(&d, frames[0].buffer);
    riscv_virtio_mmio_net_release(&d, frames[1].buffer);
    stop(&d);
    printf("PASS: VirtIO-net v%u failure keeps a device/queue/slot snapshot\n", version);
}

static void test_tx_segments(unsigned version)
{
    struct riscv_virtio_mmio_net d; start(version, &d);
    uint64_t phys[2]; unsigned char *data[2] = {model_extra_memory(&phys[0]), model_extra_memory(&phys[1])};
    memset(data[0], 'a', 64); memset(data[1], 'b', 37);
    struct riscv_net_tx_segment segments[2] = {{phys[0], 64}, {phys[1], 37}};
    int owner = 42; released_count = 0;
    assert(riscv_virtio_mmio_net_send_segments(&d, segments, 2, &owner) == 0);
    assert(d.statistics.tx_sg_packets == 1 && d.statistics.tx_copy_packets == 0);
    unsigned id = next_head(1); struct wire_descriptor desc = descriptor(1, id);
    assert(desc.flags == DESC_INDIRECT && desc.length == 3U * 16U);
    unsigned h = version == 1 ? 10 : 12;
    struct wire_descriptor header = table_entry(desc.address, 0);
    struct wire_descriptor first = table_entry(desc.address, 1);
    struct wire_descriptor second = table_entry(desc.address, 2);
    assert(header.flags == DESC_NEXT && header.next == 1 && header.length == h);
    assert(first.flags == DESC_NEXT && first.next == 2 && first.length == 64 &&
           !memcmp(resolve(first.address), data[0], 64));
    assert(second.flags == 0 && second.length == 37 && !memcmp(resolve(second.address), data[1], 37));
    complete(1, id, 0, 1); assert(!riscv_virtio_mmio_net_service(&d));
    riscv_virtio_mmio_net_tx_release(&d, collect_owner, 0);
    assert(released_count == 1 && released_owner[0] == &owner);
    riscv_virtio_mmio_net_tx_release(&d, collect_owner, 0);
    assert(released_count == 1);
    /* abandon 释放仍持有的在途 owner 且不重复释放。 */
    assert(riscv_virtio_mmio_net_send_segments(&d, segments, 1, &owner) == 0);
    riscv_virtio_mmio_net_tx_release(&d, collect_owner, 1);
    assert(released_count == 2);
    riscv_virtio_mmio_net_tx_release(&d, collect_owner, 1);
    assert(released_count == 2);
    stop(&d); model_extra_free(phys[0]); model_extra_free(phys[1]);
    printf("PASS: VirtIO-net v%u TX segments post an indirect table and release owners once\n", version);
}

static void test_tx_segments_unsupported(unsigned version)
{
    setup(version); low_features = FEATURE_MAC | FEATURE_STATUS;
    struct riscv_virtio_mmio_net d; memset(&d, 0, sizeof(d));
    assert(riscv_virtio_mmio_net_init(&d, regs, sizeof(regs), &allocator, 100, 7) == 0);
    struct riscv_net_tx_segment segment = {UINT64_C(0x81000000), 16};
    assert(riscv_virtio_mmio_net_send_segments(&d, &segment, 1, 0) == -KERNEL_ENOTSUP);
    stop(&d);
    printf("PASS: VirtIO-net v%u TX segments require the negotiated indirect feature\n", version);
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0); unsigned errors = 0;
    for (unsigned version = 1; version <= 2; version++) {
        test_loan_budget(version); test_tx_copy_budget(version); test_failure_slot_dump(version);
        test_tx_segments(version);
        test_tx_segments_unsupported(version); test_index_wrap(version);
        for (unsigned q = 0; q < 2; q++) { test_used_index(version, q, 0); test_used_index(version, q, 1); test_duplicate_id(version, q); }
        test_rx_length(version, 0); test_rx_length(version, 1);
        errors += test_rx_offload(version, 1, 0); errors += test_rx_offload(version, 0, 1);
        test_tx_timeout(version); test_configuration_irq(version); test_negotiation_failure(version, 0);
        if (version == 2) { test_negotiation_failure(version, 1); test_negotiation_failure(version, 2); }
        for (unsigned nth = 1; nth <= 3; nth++) { test_allocation_failure(version, nth, 0); test_allocation_failure(version, nth, 1); }
        test_plic_failure(version, 0); test_plic_failure(version, 1); test_reset_owner(version);
    }
    test_legacy_reserved_status();
    for (unsigned entry = 0; entry < 3; entry++) errors += test_device_needs_reset(entry);
    errors += test_device_needs_reset_loan();
    return errors ? 1 : 0;
}
