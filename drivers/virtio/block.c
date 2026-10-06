#include <kernel/cost.h>
#include <kernel/virtio_block.h>
#include <kernel/page.h>
#include <kernel/sync.h>
#include <kernel/proc_task.h>
#include <arch/context.h>
#include <arch/timer.h>
#include <arch/platform_io.h>
#include <arch/bus.h>
#include <kernel/console.h>

#include <stddef.h>
#include <stdint.h>

#define VIRTIO_STATUS_ACKNOWLEDGE 1U
#define VIRTIO_STATUS_DRIVER 2U
#define VIRTIO_STATUS_DRIVER_OK 4U
#define VIRTIO_STATUS_FEATURES_OK 8U
#define VIRTIO_STATUS_FAILED 128U

#define VIRTIO_FEATURE_VERSION_1_LOW_BIT 0U
#define VIRTIO_FEATURE_VERSION_1_HIGH_MASK \
    (UINT32_C(1) << VIRTIO_FEATURE_VERSION_1_LOW_BIT)

#define VIRTIO_BLOCK_FEATURE_RO (UINT32_C(1) << 5)
#define VIRTIO_BLOCK_FEATURE_FLUSH (UINT32_C(1) << 9)

#define VIRTIO_BLOCK_REQUEST_IN 0U
#define VIRTIO_BLOCK_REQUEST_OUT 1U
#define VIRTIO_BLOCK_REQUEST_FLUSH 4U
#define VIRTIO_BLOCK_STATUS_OK 0U
#define VIRTIO_BLOCK_STATUS_IO_ERROR 1U
#define VIRTIO_BLOCK_STATUS_UNSUPPORTED 2U
#define VIRTIO_BLOCK_SECTOR_SIZE 512U

#define VIRTQ_DESC_NEXT UINT16_C(1)
#define VIRTQ_DESC_WRITE UINT16_C(2)
#define VIRTIO_QUEUE_SIZE 32U
#define REQUEST_SLOTS 8U
#define REQUEST_STRIDE 768U

#define VIRTIO_QUEUE_DESC_OFFSET 0U
#define VIRTIO_REQUEST_HEADER_OFFSET 1024U
#define VIRTIO_LEGACY_QUEUE_USED_OFFSET BOAROS_PAGE_SIZE
#define VIRTIO_LEGACY_REQUEST_HEADER_OFFSET (2U * BOAROS_PAGE_SIZE)
#define VIRTIO_LEGACY_QUEUE_ALLOCATION_ORDER 2U
#define VIRTIO_LEGACY_QUEUE_ALIGNMENT BOAROS_PAGE_SIZE

#define VIRTIO_BLOCK_STATE_EMPTY 0U
#define VIRTIO_BLOCK_STATE_LIVE UINT32_C(0x56424c4b)
#define VIRTIO_BLOCK_STATE_FAILED UINT32_C(0x56464149)
#define VIRTIO_BLOCK_STATE_DESTROYED UINT32_C(0x56444541)

struct virtq_descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
};

struct virtq_available {
    uint16_t flags;
    uint16_t index;
    uint16_t ring[VIRTIO_QUEUE_SIZE];
    uint16_t used_event;
};

struct virtq_used_element {
    uint32_t id;
    uint32_t length;
};

struct virtq_used {
    uint16_t flags;
    uint16_t index;
    struct virtq_used_element ring[VIRTIO_QUEUE_SIZE];
    uint16_t available_event;
};

struct virtio_block_request_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

_Static_assert(sizeof(struct virtq_descriptor) == 16U,
               "VirtIO descriptor layout must match the specification");
/* ring 已按协议对齐；packed 会把 RV64 的共享 idx 拆成字节访问，进位时可被 DMA 撕裂。 */
_Static_assert(offsetof(struct virtq_available, index) == 2U &&
               offsetof(struct virtq_available, ring) == 4U &&
               _Alignof(struct virtq_available) >= 2U,
               "available index must be naturally aligned");
_Static_assert(offsetof(struct virtq_used, index) == 2U &&
               offsetof(struct virtq_used, ring) == 4U &&
               sizeof(struct virtq_used_element) == 8U &&
               _Alignof(struct virtq_used) >= 4U,
               "used index and elements must be naturally aligned");
struct block_request {
    struct virtio_block_request_header header;
    volatile unsigned char status;
    unsigned state; /* 0 free, 1 reserved, 2 submitted, 3 completed */
    enum kernel_block_status result;
    struct kernel_io_context *owner;
    struct kernel_wait_queue done;
    struct kernel_wait_queue *completion;
    uint64_t deadline;
    uint64_t publish_time;
    uint32_t type, data_length;
    int bounce_used;
    unsigned char bounce[VIRTIO_BLOCK_SECTOR_SIZE];
#if BOAROS_COST_DIAGNOSTICS
    struct kernel_cost_tag cost_tag;
    uint64_t cost_start;
    uint64_t cost_completed;
    unsigned cost_device;
    unsigned cost_woken;
#endif
};
_Static_assert(sizeof(struct block_request) <= REQUEST_STRIDE, "request slot overflow");
_Static_assert(VIRTIO_REQUEST_HEADER_OFFSET + REQUEST_SLOTS * REQUEST_STRIDE <= 8192, "modern queue overflow");
_Static_assert(VIRTIO_LEGACY_REQUEST_HEADER_OFFSET + REQUEST_SLOTS * REQUEST_STRIDE <= (4U * BOAROS_PAGE_SIZE), "legacy queue overflow");

static int legacy_transport(const struct virtio_block_device *device)
{
    return device->transport_version == 1U;
}

static uint32_t queue_avail_offset(const struct virtio_block_device *device)
{ return device->queue_size * sizeof(struct virtq_descriptor); }
static uint32_t queue_used_offset(const struct virtio_block_device *device)
{
    return legacy_transport(device) ? VIRTIO_LEGACY_QUEUE_USED_OFFSET :
        (queue_avail_offset(device) + 6 + 2 * device->queue_size + 15) & ~15U;
}
static uint32_t request_header_offset(const struct virtio_block_device *device)
{ return legacy_transport(device) ? VIRTIO_LEGACY_REQUEST_HEADER_OFFSET : VIRTIO_REQUEST_HEADER_OFFSET; }
static unsigned slot_count(const struct virtio_block_device *device)
{ unsigned n = device->queue_size / 3; return n < REQUEST_SLOTS ? n : REQUEST_SLOTS; }
static struct block_request *request_at(struct virtio_block_device *device, unsigned index)
{ return (void *)((unsigned char *)device->queue_memory + request_header_offset(device) + index * REQUEST_STRIDE); }
static uint64_t request_physical(struct virtio_block_device *device, struct block_request *request)
{ return device->queue_physical_address + ((unsigned char *)request - (unsigned char *)device->queue_memory); }
static uint32_t bounce_offset(struct virtio_block_device *device, struct block_request *request)
{ return (unsigned char *)request->bounce - (unsigned char *)device->queue_memory; }

static int device_live(const struct virtio_block_device *device)
{
    return device != 0 && device->state == VIRTIO_BLOCK_STATE_LIVE;
}

static void block_puts(const char *text)
{ while (*text) kernel_console_putc(*text++); }
static void block_hex(unsigned long value)
{
    char digits[16];unsigned count=0;
    block_puts("0x");
    do { digits[count++]="0123456789abcdef"[value&15];value>>=4; } while(value);
    while(count) kernel_console_putc(digits[--count]);
}
static uint32_t reg_read(const struct virtio_block_device *device,enum virtio_block_register reg)
{ return device->transport.ops->read(device->transport.context,reg); }
static void reg_write(struct virtio_block_device *device,enum virtio_block_register reg,uint32_t value)
{ device->transport.ops->write(device->transport.context,reg,value); }
static void memory_barrier(void) { arch_dma_barrier(); }
static uint64_t time_now(void) { return arch_time_read(); }

/* 在途深度变化前收口一段区间：累计 Σ深度·Δt 与忙时。 */
static void account_inflight(struct virtio_block_device *device)
{
    uint64_t now = time_now();
    uint64_t elapsed = now - device->statistics_last;

    device->statistics.inflight_ticks += (uint64_t)device->inflight * elapsed;
    if (device->inflight) device->statistics.busy_ticks += elapsed;
    device->statistics_last = now;
}

static void bytes_zero(void *pointer, size_t size)
{
    unsigned char *bytes = pointer;
    size_t index;

    for (index = 0U; index < size; index++) {
        bytes[index] = 0U;
    }
}

static void bytes_copy(void *destination,
                       const void *source,
                       size_t size)
{
    unsigned char *output = destination;
    const unsigned char *input = source;
    size_t index;

    COST_ADD(DEVICE_BOUNCE_COPY, size);
    for (index = 0U; index < size; index++) {
        output[index] = input[index];
    }
}

static void write_queue_address(struct virtio_block_device *device,
                                enum virtio_block_register low_offset,
                                enum virtio_block_register high_offset,
                                uint64_t address)
{
    reg_write(device, low_offset, (uint32_t)address);
    reg_write(device, high_offset, (uint32_t)(address >> 32U));
}

static void device_reset(struct virtio_block_device *device)
{
    reg_write(device, VIRTIO_BLOCK_REG_STATUS, 0U);
    memory_barrier();
    /* QEMU's MMIO reset is synchronous. Do not return a borrowed DMA buffer
     * or release a queue if the transport violates that reset contract. */
    if (reg_read(device, VIRTIO_BLOCK_REG_STATUS) != 0U) {
        __builtin_trap();
    }
}

static enum virtio_block_status init_failure(
    struct virtio_block_device *device,
    struct virtio_block_device *owner,
    enum virtio_block_status status,
    int reset,
    int release_queue)
{
    if (reset) {
        reg_write(device,
                     VIRTIO_BLOCK_REG_STATUS,
                     VIRTIO_STATUS_FAILED);
        device_reset(device);
    }
    if (release_queue) {
        enum physical_page_status page_status =
            physical_page_release_order(device->page_allocator,
                                        device->queue_physical_address,
                                        device->queue_allocation_order);

        if (page_status != PHYSICAL_PAGE_STATUS_OK) {
            device->state = VIRTIO_BLOCK_STATE_FAILED;
            if (owner != 0) {
                *owner = *device;
            }
            return VIRTIO_BLOCK_DRIVER_STATUS_STATE;
        }
    }
    return status;
}

static enum virtio_block_status negotiate_features(
    struct virtio_block_device *device)
{
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    uint32_t device_features_low;
    uint32_t device_features_high;
    uint32_t driver_features_low = 0U;

    reg_write(device, VIRTIO_BLOCK_REG_STATUS, 0U);
    reg_write(device,
                 VIRTIO_BLOCK_REG_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE);
    reg_write(device, VIRTIO_BLOCK_REG_STATUS, status);

    reg_write(device, VIRTIO_BLOCK_REG_DEVICE_FEATURES_SEL, 0U);
    device_features_low =
        reg_read(device, VIRTIO_BLOCK_REG_DEVICE_FEATURES);
    if ((device_features_low & VIRTIO_BLOCK_FEATURE_RO) != 0U) {
        driver_features_low |= VIRTIO_BLOCK_FEATURE_RO;
        device->read_only = 1U;
    }
    /* CONFIG_WCE is deliberately not negotiated: FLUSH then describes the
     * writeback mode, and absence of FLUSH means write-through (Linux virtblk
     * uses the same fallback). No configuration write can change that mode. */
    device->block.cache_mode = KERNEL_BLOCK_CACHE_WRITETHROUGH;
    if ((device_features_low & VIRTIO_BLOCK_FEATURE_FLUSH) != 0U) {
        driver_features_low |= VIRTIO_BLOCK_FEATURE_FLUSH;
        device->block.cache_mode = KERNEL_BLOCK_CACHE_WRITEBACK;
    }

    if (legacy_transport(device)) {
        reg_write(device, VIRTIO_BLOCK_REG_DRIVER_FEATURES_SEL, 0U);
        reg_write(device, VIRTIO_BLOCK_REG_DRIVER_FEATURES, driver_features_low);
        return VIRTIO_BLOCK_DRIVER_STATUS_OK;
    }

    reg_write(device, VIRTIO_BLOCK_REG_DEVICE_FEATURES_SEL, 1U);
    device_features_high =
        reg_read(device, VIRTIO_BLOCK_REG_DEVICE_FEATURES);
    if ((device_features_high & VIRTIO_FEATURE_VERSION_1_HIGH_MASK) == 0U) {
        return VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED;
    }

    reg_write(device, VIRTIO_BLOCK_REG_DRIVER_FEATURES_SEL, 0U);
    reg_write(device, VIRTIO_BLOCK_REG_DRIVER_FEATURES, driver_features_low);
    reg_write(device, VIRTIO_BLOCK_REG_DRIVER_FEATURES_SEL, 1U);
    reg_write(device,
                 VIRTIO_BLOCK_REG_DRIVER_FEATURES,
                 VIRTIO_FEATURE_VERSION_1_HIGH_MASK);

    status |= VIRTIO_STATUS_FEATURES_OK;
    reg_write(device, VIRTIO_BLOCK_REG_STATUS, status);
    if ((reg_read(device, VIRTIO_BLOCK_REG_STATUS) &
         VIRTIO_STATUS_FEATURES_OK) == 0U) {
        return VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED;
    }

    return VIRTIO_BLOCK_DRIVER_STATUS_OK;
}

static uint64_t read_capacity(struct virtio_block_device *device)
{
    uint32_t attempt;

    if (legacy_transport(device)) {
        uint32_t low = reg_read(device, VIRTIO_BLOCK_REG_CONFIG);
        uint32_t high = reg_read(device,
                                    VIRTIO_BLOCK_REG_CAPACITY_HIGH);

        return ((uint64_t)high << 32U) | low;
    }

    for (attempt = 0U; attempt < 8U; attempt++) {
        uint32_t generation_before =
            reg_read(device, VIRTIO_BLOCK_REG_CONFIG_GENERATION);
        uint32_t low = reg_read(device, VIRTIO_BLOCK_REG_CONFIG);
        uint32_t high = reg_read(device,
                                    VIRTIO_BLOCK_REG_CAPACITY_HIGH);
        uint32_t generation_after =
            reg_read(device, VIRTIO_BLOCK_REG_CONFIG_GENERATION);

        if (generation_before == generation_after) {
            return ((uint64_t)high << 32U) | low;
        }
    }

    return 0U;
}

static void wake(struct kernel_wait_queue *queue)
{
    if (queue->head && kernel_wait_queue_wake_all(queue) != KERNEL_SCHEDULER_STATUS_OK)
        __builtin_trap();
}
static void diagnose_queue_fault(struct virtio_block_device *device,
    const char *reason, uint16_t used_index, uint16_t observed_count,
    uint32_t pending, const struct virtq_used_element *item);
static void fail_device(struct virtio_block_device *device, enum kernel_block_status result)
{
    device->state = VIRTIO_BLOCK_STATE_FAILED;
    if (result == KERNEL_BLOCK_STATUS_TIMEOUT) {
        volatile struct virtq_used *used = (void *)((unsigned char *)device->queue_memory + queue_used_offset(device));
        uint16_t used_index = used->index;
        memory_barrier();
        /* 纯超时没有非法 used 项；reset 前仍需保留设备身份与在途 owner。 */
        diagnose_queue_fault(device, "timeout", used_index,
            (uint16_t)(used_index - device->last_used_index),
            device->transport.ops->ack_interrupt(device->transport.context), NULL);
        block_puts("BoarOS: block timeout; resetting device\n");
    }
    device_reset(device); /* No DMA owner is released before reset acknowledgement. */
    for (unsigned i = 0; i < slot_count(device); i++) {
        struct block_request *r = request_at(device, i);
        if (r->state == 2) {
            r->result = result; r->state = 3;
#if BOAROS_COST_DIAGNOSTICS
            enum kernel_cost_metric metric = (enum kernel_cost_metric)(COST_DEVICE0_REQUESTS + r->cost_device * 8);
            kernel_cost_add_tag(r->cost_tag, (enum kernel_cost_metric)(metric + 1), 1);
            kernel_cost_add_tag(r->cost_tag, (enum kernel_cost_metric)(metric + 2), 1);
            kernel_cost_sample_tag(r->cost_tag, (enum kernel_cost_metric)(metric + 7), kernel_cost_clock() - r->cost_start);
#endif
            if (!device->inflight) __builtin_trap();
            account_inflight(device);
            device->inflight--;
            device->statistics.service_ticks += time_now() - r->publish_time;
            if (r->completion->head) device->statistics.wakes++;
            wake(r->completion);
        }
    }
    wake(&device->available);
}

static void queue_fault_scalar(const char *name, uint64_t value)
{
    block_puts(name);
    block_hex((unsigned long)value);
}

static void diagnose_queue_fault(struct virtio_block_device *device,
    const char *reason, uint16_t used_index, uint16_t observed_count,
    uint32_t pending, const struct virtq_used_element *item)
{
    volatile struct virtq_available *available = (void *)((unsigned char *)device->queue_memory + queue_avail_offset(device));
    unsigned reserved = 0, published = 0, complete = 0;
    for (unsigned i = 0; i < slot_count(device); i++) {
        unsigned state = request_at(device, i)->state;
        reserved += state == 1;
        published += state == 2;
        complete += state == 3;
    }
    /* reset前记录driver拥有的标量；owner/completion只打印值，不追指针。 */
    block_puts("BoarOS: block queue fault reason=");
    block_puts(reason);
    queue_fault_scalar(" transport_context=", (uintptr_t)device->transport.context);
    queue_fault_scalar(" device_status=", reg_read(device, VIRTIO_BLOCK_REG_STATUS));
    queue_fault_scalar(" interrupt=", pending);
    queue_fault_scalar(" transport=", device->transport_version);
    queue_fault_scalar(" queue=", (uintptr_t)device->queue_memory);
    queue_fault_scalar(" physical=", device->queue_physical_address);
    queue_fault_scalar(" size=", device->queue_size);
    block_puts("\n");
    queue_fault_scalar(" used=", used_index);
    queue_fault_scalar(" consumed=", device->last_used_index);
    queue_fault_scalar(" avail=", available->index);
    queue_fault_scalar(" observed_count=", observed_count);
    queue_fault_scalar(" inflight=", device->inflight);
    queue_fault_scalar(" active=", device->active);
    queue_fault_scalar(" reserved=", reserved);
    queue_fault_scalar(" published=", published);
    queue_fault_scalar(" complete=", complete);
    block_puts("\n");
    if (item != NULL) {
        queue_fault_scalar(" item_index=", (uint16_t)(device->last_used_index - 1));
        queue_fault_scalar(" id=", item->id);
        queue_fault_scalar(" length=", item->length);
        block_puts("\n");
    }
    for (unsigned i = 0; i < slot_count(device); i++) {
        const struct block_request *r = request_at(device, i);
        queue_fault_scalar(" slot=", i);
        queue_fault_scalar(" state=", r->state);
        queue_fault_scalar(" owner=", (uintptr_t)r->owner);
        queue_fault_scalar(" completion=", (uintptr_t)r->completion);
        queue_fault_scalar(" status=", r->status);
        queue_fault_scalar(" type=", r->type);
        queue_fault_scalar(" bytes=", r->data_length);
        queue_fault_scalar(" sector=", r->header.sector);
        queue_fault_scalar(" deadline=", r->deadline);
        block_puts("\n");
    }
}

static void collect_used(struct virtio_block_device *device)
{
    uint32_t pending=device->transport.ops->ack_interrupt(device->transport.context);
    if (pending) arch_io_barrier();
    volatile struct virtq_used *used = (void *)((unsigned char *)device->queue_memory + queue_used_offset(device));
    uint16_t used_index = used->index;
    uint16_t count = (uint16_t)(used_index - device->last_used_index);
    uint16_t observed_count = count;
    int completed = count != 0;
    memory_barrier();
    if (count > slot_count(device)) {
        diagnose_queue_fault(device, "used-overflow", used_index, observed_count, pending, NULL);
        fail_device(device, KERNEL_BLOCK_STATUS_IO); return;
    }
    while (count--) {
        struct virtq_used_element item = used->ring[device->last_used_index++ % device->queue_size];
        if (item.id % 3 || item.id / 3 >= slot_count(device) || !item.length) {
            diagnose_queue_fault(device, "used-element", used_index, observed_count, pending, &item);
            fail_device(device, KERNEL_BLOCK_STATUS_IO); return;
        }
        struct block_request *r = request_at(device, item.id / 3);
        if (r->state != 2) {
            diagnose_queue_fault(device, "slot-state", used_index, observed_count, pending, &item);
            fail_device(device, KERNEL_BLOCK_STATUS_IO); return;
        }
        r->result = r->status == VIRTIO_BLOCK_STATUS_OK ? KERNEL_BLOCK_STATUS_OK :
            r->status == VIRTIO_BLOCK_STATUS_UNSUPPORTED ? KERNEL_BLOCK_STATUS_UNSUPPORTED : KERNEL_BLOCK_STATUS_IO;
        if (r->status > VIRTIO_BLOCK_STATUS_UNSUPPORTED) {
            diagnose_queue_fault(device, "device-status", used_index, observed_count, pending, &item);
            fail_device(device, KERNEL_BLOCK_STATUS_IO); return;
        }
        r->state = 3;
#if BOAROS_COST_DIAGNOSTICS
        r->cost_completed = kernel_cost_clock();
        r->cost_woken = r->completion->head != NULL;
        enum kernel_cost_metric metric = (enum kernel_cost_metric)(COST_DEVICE0_REQUESTS + r->cost_device * 8);
        kernel_cost_add_tag(r->cost_tag, (enum kernel_cost_metric)(metric + 1), 1);
        if (r->result != KERNEL_BLOCK_STATUS_OK) kernel_cost_add_tag(r->cost_tag, (enum kernel_cost_metric)(metric + 2), 1);
        kernel_cost_sample_tag(r->cost_tag, (enum kernel_cost_metric)(metric + 7), kernel_cost_clock() - r->cost_start);
#endif
        if (!device->inflight) __builtin_trap();
        account_inflight(device);
        device->inflight--;
        device->statistics.service_ticks += time_now() - r->publish_time;
        if (r->result != KERNEL_BLOCK_STATUS_OK) device->statistics.io_errors++;
        if (r->completion->head) device->statistics.wakes++;
        wake(r->completion);
#if BOAROS_COST_DIAGNOSTICS
        if (r->cost_woken) kernel_cost_add_tag(r->cost_tag, COST_IO_COMPLETE_TO_READY, kernel_cost_clock() - r->cost_completed);
#endif
    }
    if (completed) wake(&device->available);
}
static void block_irq(void *owner)
{
    struct virtio_block_device *device = owner;
    device->statistics.interrupts++;
    if (device_live(device)) collect_used(device);
    else (void)device->transport.ops->ack_interrupt(device->transport.context);
}
int virtio_block_enable_irq(struct virtio_block_device *device, uint32_t source)
{
    if (!device_live(device) || device->active || device->irq_source ||
        !device->transport.ops->register_irq || !device->transport.ops->unregister_irq ||
        !device->transport.ops->register_irq(device->transport.context,source,block_irq,device)) return 0;
    device->irq_source = source;
    return 1;
}
static int begin_call(struct virtio_block_device *device, int barrier)
{
    uintptr_t irq = arch_interrupt_save();
    if (device->irq_source && (!kernel_scheduler_can_sleep() || arch_external_interrupt_active())) __builtin_trap();
    if (barrier) device->barrier_waiters++;
    for (;;) {
        if (!device_live(device)) break;
        if (!device->barrier && (barrier ? !device->active : !device->barrier_waiters)) {
            if (device->active == UINT32_MAX) __builtin_trap();
            device->active++;
            if (barrier) { device->barrier_waiters--; device->barrier = 1; }
            arch_interrupt_restore(irq); return 1;
        }
        if (!device->irq_source) __builtin_trap();
        enum kernel_wait_wake_reason reason;
        device->statistics.queue_waits++;
        uint64_t wait_start = time_now();
        if (kernel_scheduler_block_current(&device->available, 0, 0, &reason) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
        device->statistics.queue_wait_ticks += time_now() - wait_start;
    }
    if (barrier) device->barrier_waiters--;
    arch_interrupt_restore(irq); return 0;
}
static void end_call(struct virtio_block_device *device, int barrier)
{
    uintptr_t irq = arch_interrupt_save();
    if (!device->active || (barrier && !device->barrier)) __builtin_trap();
    device->active--;
    if (barrier) device->barrier = 0;
    wake(&device->available);
    arch_interrupt_restore(irq);
}
/* Already admitted logical calls keep publishing even when FLUSH is waiting.
 * The call owns active until its unpublished spans and DMA are both drained. */
static struct block_request *try_reserve_request(struct virtio_block_device *device)
{
    if (!device_live(device)) return 0;
    for (unsigned i = 0; i < slot_count(device); i++) {
        struct block_request *r = request_at(device, i);
        if (!r->state) {
            r->state = 1;
            r->owner = kernel_io_context_current();
            r->completion = &r->done;
            return r;
        }
    }
    return 0;
}
static struct block_request *reserve_request(struct virtio_block_device *device)
{
    uintptr_t irq = arch_interrupt_save();
    struct block_request *r = 0;
    while (device_live(device) && !(r = try_reserve_request(device))) {
        if (!device->irq_source) __builtin_trap();
        enum kernel_wait_wake_reason reason;
        device->statistics.queue_waits++;
        uint64_t wait_start = time_now();
        if (kernel_scheduler_block_current(&device->available, 0, 0, &reason) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
        device->statistics.queue_wait_ticks += time_now() - wait_start;
    }
    if (!device_live(device)) r = 0;
    arch_interrupt_restore(irq); return r;
}
static void release_request(struct virtio_block_device *device, struct block_request *r)
{
    uintptr_t irq = arch_interrupt_save();
    if (r->state != 1 || r->owner != kernel_io_context_current() || !device->active || r->done.head)
        __builtin_trap();
    r->state = 0; r->owner = 0; r->completion = 0;
    wake(&device->available);
    arch_interrupt_restore(irq);
}
static enum kernel_block_status publish_request(struct virtio_block_device *device,
    struct block_request *r, uint32_t type, uint64_t sector,
    uint64_t data_address, uint32_t data_length, int bounce)
{
    uintptr_t irq = arch_interrupt_save();
    if (!device_live(device)) { arch_interrupt_restore(irq); return KERNEL_BLOCK_STATUS_IO; }
    if (r->state != 1 || r->owner != kernel_io_context_current()) __builtin_trap();
    unsigned head = ((unsigned char *)r - (unsigned char *)request_at(device, 0)) / REQUEST_STRIDE * 3;
    struct virtq_descriptor *d = device->queue_memory;
    volatile struct virtq_available *available = (void *)((unsigned char *)device->queue_memory + queue_avail_offset(device));
    int flushing = type == VIRTIO_BLOCK_REQUEST_FLUSH;
    r->header = (struct virtio_block_request_header){type, 0, sector};
    r->status = UINT8_MAX; r->state = 2;
    r->type = type; r->data_length = data_length; r->bounce_used = bounce;
    uint64_t publish_now = time_now();
    r->deadline = publish_now + device->timeout_ticks;
    r->publish_time = publish_now;
    account_inflight(device);
    device->statistics.requests++;
    device->inflight++;
#if BOAROS_COST_DIAGNOSTICS
    r->cost_tag = kernel_cost_capture();
    r->cost_start = kernel_cost_clock();
    r->cost_completed = 0;
    r->cost_woken = 0;
    r->cost_device = device->block.registered && device->block.device_number == KERNEL_BLOCK_DEVICE_NUMBER(0) ? 0 :
        device->block.registered && device->block.device_number == KERNEL_BLOCK_DEVICE_NUMBER(1) ? 1 : 2;
    enum kernel_cost_metric metric = (enum kernel_cost_metric)(COST_DEVICE0_REQUESTS + r->cost_device * 8);
    kernel_cost_add_tag(r->cost_tag, metric, 1);
    kernel_cost_add_tag(r->cost_tag, (enum kernel_cost_metric)(metric + 6), device->inflight);
    unsigned phase = kernel_cost_current()->operation >> 4;
    if (!flushing && type == VIRTIO_BLOCK_REQUEST_OUT && phase >= 1 && phase <= 4) {
        kernel_cost_add_tag(r->cost_tag, (enum kernel_cost_metric)(COST_DEVICE0_DATA_WRITE + r->cost_device * 4 + phase - 1), data_length);
    } else if (type == VIRTIO_BLOCK_REQUEST_IN && phase >= 5 && phase <= 7) {
        kernel_cost_add_tag(r->cost_tag, (enum kernel_cost_metric)(COST_DEVICE0_FILE_READ + r->cost_device * 3 + phase - 5), data_length);
    } else kernel_cost_add_tag(r->cost_tag, (enum kernel_cost_metric)(metric + (flushing ? 3 : type == VIRTIO_BLOCK_REQUEST_IN ? 4 : 5)), flushing ? 1 : data_length);
#endif
    if (device->inflight > device->statistics.max_inflight) device->statistics.max_inflight = device->inflight;
    uint64_t physical = request_physical(device, r);
    d[head] = (struct virtq_descriptor){physical, sizeof(r->header), VIRTQ_DESC_NEXT, head + (flushing ? 2 : 1)};
    d[head + 1] = (struct virtq_descriptor){data_address, data_length,
        VIRTQ_DESC_NEXT | (type == VIRTIO_BLOCK_REQUEST_IN ? VIRTQ_DESC_WRITE : 0), head + 2};
    d[head + 2] = (struct virtq_descriptor){physical + offsetof(struct block_request, status), 1, VIRTQ_DESC_WRITE, 0};
    available->ring[available->index % device->queue_size] = head;
    memory_barrier(); available->index++; memory_barrier();
    if (type == VIRTIO_BLOCK_REQUEST_IN) kernel_proc_task_note_block_read();
    reg_write(device, VIRTIO_BLOCK_REG_QUEUE_NOTIFY, 0);
    arch_interrupt_restore(irq);
    return KERNEL_BLOCK_STATUS_OK;
}
static enum kernel_block_status finish_request(struct virtio_block_device *device,
                                               struct block_request *r)
{
    if (r->state != 3 || r->owner != kernel_io_context_current()) __builtin_trap();
    enum kernel_block_status result = r->result;
#if BOAROS_COST_DIAGNOSTICS
    if (r->cost_woken) kernel_cost_add_tag(r->cost_tag, COST_IO_COMPLETE_TO_RESUME, kernel_cost_clock() - r->cost_completed);
#endif
    r->state = 1;
    if (r->type == VIRTIO_BLOCK_REQUEST_FLUSH) device->statistics.flush_requests++;
    else {
        if (r->type == VIRTIO_BLOCK_REQUEST_IN) device->statistics.sectors_read += r->data_length / 512;
        else device->statistics.sectors_written += r->data_length / 512;
        if (r->bounce_used) device->statistics.bounce_requests++; else device->statistics.direct_requests++;
    }
    return result;
}
static enum kernel_block_status wait_request(struct virtio_block_device *device,
                                             struct block_request *r)
{
    uintptr_t irq = arch_interrupt_save();
    while (r->state == 2) {
        /* During boot there is no IRQ consumer. Harvest a completion already
         * published by the device before deciding that its deadline elapsed. */
        if (!device->irq_source) collect_used(device);
        if (r->state != 2) break;
        if ((int64_t)(time_now() - r->deadline) >= 0) { device->statistics.timeouts++; fail_device(device, KERNEL_BLOCK_STATUS_TIMEOUT); break; }
        if (device->irq_source) {
            enum kernel_wait_wake_reason reason;
            device->statistics.sleeps++;
            if (kernel_scheduler_block_current(&r->done, r->deadline, 0, &reason) != KERNEL_SCHEDULER_STATUS_OK)
                __builtin_trap();
            /* A timer may win the trap race even though DMA completed. This
             * one final harvest is not a polling completion loop. */
            if (reason == KERNEL_WAIT_TIMEOUT && r->state == 2 && device_live(device))
                collect_used(device);
        }
    }
    enum kernel_block_status result = finish_request(device, r);
    arch_interrupt_restore(irq);
    return result;
}
static enum kernel_block_status submit_request(struct virtio_block_device *device,
    struct block_request *r, uint32_t type, uint64_t sector,
    uint64_t data_address, uint32_t data_length, int bounce)
{
    COST_SCOPE(device_cost, OPERATION_TICKS);
    enum kernel_block_status result = publish_request(device, r, type, sector,
        data_address, data_length, bounce);
    return result == KERNEL_BLOCK_STATUS_OK ? wait_request(device, r) : result;
}
static enum kernel_block_status virtio_block_flush(void *context)
{
    struct virtio_block_device *device = context;
    if (!device_live(device)) return KERNEL_BLOCK_STATUS_IO;
    if (!begin_call(device, 1)) return KERNEL_BLOCK_STATUS_IO;
    struct block_request *r = reserve_request(device);
    if (!r) { end_call(device, 1); return KERNEL_BLOCK_STATUS_IO; }
    enum kernel_block_status result = device->block.cache_mode == KERNEL_BLOCK_CACHE_WRITETHROUGH
        ? KERNEL_BLOCK_STATUS_OK : submit_request(device, r, VIRTIO_BLOCK_REQUEST_FLUSH, 0, 0, 0, 0);
    release_request(device, r); end_call(device, 1); return result;
}

static enum kernel_block_status transfer_read(struct virtio_block_device *device, struct block_request *r,
                                                  uint64_t offset,
                                                  void *buffer,
                                                  size_t size)
{
    unsigned char *output = buffer;

    if (!device_live(device)) {
        return KERNEL_BLOCK_STATUS_STATE;
    }

    while (size != 0U) {
        uint64_t sector = offset / VIRTIO_BLOCK_SECTOR_SIZE;
        uint32_t sector_offset =
            (uint32_t)(offset % VIRTIO_BLOCK_SECTOR_SIZE);
        uint64_t data_address;

        if (sector_offset == 0U && size >= VIRTIO_BLOCK_SECTOR_SIZE) {
            size_t direct_size = size;
            const size_t maximum_direct_size =
                (size_t)UINT32_MAX &
                ~(size_t)(VIRTIO_BLOCK_SECTOR_SIZE - 1U);

            if (direct_size > maximum_direct_size) {
                direct_size = maximum_direct_size;
            }
            direct_size &= ~(size_t)(VIRTIO_BLOCK_SECTOR_SIZE - 1U);
            if (!device->dma_address(output,
                                     direct_size,
                                     &data_address)) {
                direct_size = 0U;
            }
            if (direct_size != 0U) {
                enum kernel_block_status status = submit_request(
                    device, r,
                    VIRTIO_BLOCK_REQUEST_IN,
                    sector,
                    data_address,
                    (uint32_t)direct_size,
                    0);

                if (status != KERNEL_BLOCK_STATUS_OK) {
                    return status;
                }
                output += direct_size;
                offset += direct_size;
                size -= direct_size;
                continue;
            }
        }
        {
            unsigned char *bounce =
                (unsigned char *)device->queue_memory + bounce_offset(device, r);
            size_t copied = VIRTIO_BLOCK_SECTOR_SIZE - sector_offset;
            enum kernel_block_status status;

            if (copied > size) {
                copied = size;
            }
            status = submit_request(device, r,
                                    VIRTIO_BLOCK_REQUEST_IN,
                                    sector,
                                    device->queue_physical_address +
                                        bounce_offset(device, r),
                                    VIRTIO_BLOCK_SECTOR_SIZE,
                                    1);
            if (status != KERNEL_BLOCK_STATUS_OK) {
                return status;
            }
            bytes_copy(output, bounce + sector_offset, copied);
            output += copied;
            offset += copied;
            size -= copied;
        }
    }

    return KERNEL_BLOCK_STATUS_OK;
}

static enum kernel_block_status transfer_write(struct virtio_block_device *device, struct block_request *r,
                                                   uint64_t offset,
                                                   const void *buffer,
                                                   size_t size)
{
    const unsigned char *input = buffer;

    if (!device_live(device)) {
        return KERNEL_BLOCK_STATUS_STATE;
    }
    if (device->read_only != 0U) {
        return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    }

    while (size != 0U) {
        uint64_t sector = offset / VIRTIO_BLOCK_SECTOR_SIZE;
        uint32_t sector_offset =
            (uint32_t)(offset % VIRTIO_BLOCK_SECTOR_SIZE);
        uint64_t data_address;

        if (sector_offset == 0U && size >= VIRTIO_BLOCK_SECTOR_SIZE) {
            size_t direct_size = size;
            const size_t maximum_direct_size =
                (size_t)UINT32_MAX &
                ~(size_t)(VIRTIO_BLOCK_SECTOR_SIZE - 1U);

            if (direct_size > maximum_direct_size) {
                direct_size = maximum_direct_size;
            }
            direct_size &= ~(size_t)(VIRTIO_BLOCK_SECTOR_SIZE - 1U);
            if (!device->dma_address(input,
                                     direct_size,
                                     &data_address)) {
                direct_size = 0U;
            }
            if (direct_size != 0U) {
                enum kernel_block_status status = submit_request(
                    device, r,
                    VIRTIO_BLOCK_REQUEST_OUT,
                    sector,
                    data_address,
                    (uint32_t)direct_size,
                    0);

                if (status != KERNEL_BLOCK_STATUS_OK) {
                    return status;
                }
                input += direct_size;
                offset += direct_size;
                size -= direct_size;
                continue;
            }
        }
        {
            unsigned char *bounce =
                (unsigned char *)device->queue_memory + bounce_offset(device, r);
            size_t copied = VIRTIO_BLOCK_SECTOR_SIZE - sector_offset;
            enum kernel_block_status status;

            if (copied > size) {
                copied = size;
            }
            if (sector_offset != 0U || copied < VIRTIO_BLOCK_SECTOR_SIZE) {
                status = submit_request(device, r,
                                        VIRTIO_BLOCK_REQUEST_IN,
                                        sector,
                                        device->queue_physical_address +
                                            bounce_offset(device, r),
                                        VIRTIO_BLOCK_SECTOR_SIZE,
                                        1);
                if (status != KERNEL_BLOCK_STATUS_OK) {
                    return status;
                }
            }
            bytes_copy(bounce + sector_offset, input, copied);
            status = submit_request(device, r,
                                    VIRTIO_BLOCK_REQUEST_OUT,
                                    sector,
                                    device->queue_physical_address +
                                        bounce_offset(device, r),
                                    VIRTIO_BLOCK_SECTOR_SIZE,
                                    1);
            if (status != KERNEL_BLOCK_STATUS_OK) {
                return status;
            }
            input += copied;
            offset += copied;
            size -= copied;
        }
    }

    return KERNEL_BLOCK_STATUS_OK;
}

static enum kernel_block_status virtio_block_read(void *context, uint64_t offset, void *buffer, size_t size)
{
    struct virtio_block_device *device = context;
    if (!begin_call(device, 0)) return KERNEL_BLOCK_STATUS_IO;
    struct block_request *r = reserve_request(device);
    if (!r) { end_call(device, 0); return KERNEL_BLOCK_STATUS_IO; }
    enum kernel_block_status result = transfer_read(device, r, offset, buffer, size);
    release_request(device, r); end_call(device, 0); return result;
}
static enum kernel_block_status virtio_block_write(void *context, uint64_t offset, const void *buffer, size_t size)
{
    struct virtio_block_device *device = context;
    if (!begin_call(device, 0)) return KERNEL_BLOCK_STATUS_IO;
    struct block_request *r = reserve_request(device);
    if (!r) { end_call(device, 0); return KERNEL_BLOCK_STATUS_IO; }
    enum kernel_block_status result = transfer_write(device, r, offset, buffer, size);
    release_request(device, r); end_call(device, 0); return result;
}

struct batch_transfer {
    struct block_request *request;
    uint64_t offset;
    const unsigned char *input;
    size_t remaining, copied;
    int modify_ready;
};

static enum kernel_block_status publish_write_part(struct virtio_block_device *device,
                                                   struct batch_transfer *part)
{
    struct block_request *r = part->request;
    uint64_t sector = part->offset / VIRTIO_BLOCK_SECTOR_SIZE;
    unsigned sector_offset = part->offset % VIRTIO_BLOCK_SECTOR_SIZE;
    uint64_t address;
    if (!sector_offset && part->remaining >= VIRTIO_BLOCK_SECTOR_SIZE) {
        size_t direct = part->remaining;
        const size_t maximum = (size_t)UINT32_MAX & ~(size_t)(VIRTIO_BLOCK_SECTOR_SIZE - 1);
        if (direct > maximum) direct = maximum;
        direct &= ~(size_t)(VIRTIO_BLOCK_SECTOR_SIZE - 1);
        if (device->dma_address(part->input, direct, &address)) {
            part->copied = direct;
            return publish_request(device, r, VIRTIO_BLOCK_REQUEST_OUT,
                sector, address, (uint32_t)direct, 0);
        }
    }
    part->copied = VIRTIO_BLOCK_SECTOR_SIZE - sector_offset;
    if (part->copied > part->remaining) part->copied = part->remaining;
    address = device->queue_physical_address + bounce_offset(device, r);
    if (!part->modify_ready && (sector_offset || part->copied < VIRTIO_BLOCK_SECTOR_SIZE))
        return publish_request(device, r, VIRTIO_BLOCK_REQUEST_IN,
            sector, address, VIRTIO_BLOCK_SECTOR_SIZE, 1);
    bytes_copy(r->bounce + sector_offset, part->input, part->copied);
    return publish_request(device, r, VIRTIO_BLOCK_REQUEST_OUT,
        sector, address, VIRTIO_BLOCK_SECTOR_SIZE, 1);
}

static int earlier_sector_pending(const struct kernel_block_span *spans,
                                  const struct batch_transfer *parts, size_t index)
{
    uint64_t first = spans[index].offset / VIRTIO_BLOCK_SECTOR_SIZE;
    uint64_t last = (spans[index].offset + spans[index].size - 1) / VIRTIO_BLOCK_SECTOR_SIZE;
    for (size_t j = 0; j < index; j++) {
        if (!parts[j].remaining) continue;
        uint64_t other_first = spans[j].offset / VIRTIO_BLOCK_SECTOR_SIZE;
        uint64_t other_last = (spans[j].offset + spans[j].size - 1) / VIRTIO_BLOCK_SECTOR_SIZE;
        if (first <= other_last && other_first <= last) return 1;
    }
    return 0;
}

static enum kernel_block_status virtio_block_write_batch(void *context,
    const struct kernel_block_span *spans, size_t count)
{
    COST_SCOPE(device_cost, OPERATION_TICKS);
    struct virtio_block_device *device = context;
    if (!begin_call(device, 0)) return KERNEL_BLOCK_STATUS_IO;
    struct kernel_io_context *owner = kernel_io_context_current();
    struct batch_transfer parts[KERNEL_BLOCK_BATCH_MAX] = {0};
    for (size_t i = 0; i < count; i++) {
        parts[i].offset = spans[i].offset;
        parts[i].input = spans[i].buffer;
        parts[i].remaining = spans[i].size;
    }
    enum kernel_block_status result = KERNEL_BLOCK_STATUS_OK;
    for (;;) {
        uintptr_t irq = arch_interrupt_save();
        if (owner != kernel_io_context_current()) __builtin_trap();
        if (!device->irq_source && device_live(device)) collect_used(device);
        /* Harvest every completed slot before publishing more. One failed
         * completion stops all remaining spans, including ready RMW writes. */
        for (size_t i = 0; i < count; i++) {
            struct batch_transfer *part = &parts[i];
            struct block_request *r = part->request;
            if (!r || r->state != 3) continue;
            enum kernel_block_status status = finish_request(device, r);
            if (result == KERNEL_BLOCK_STATUS_OK && status != KERNEL_BLOCK_STATUS_OK) result = status;
            if (status == KERNEL_BLOCK_STATUS_OK && r->type == VIRTIO_BLOCK_REQUEST_IN) {
                part->modify_ready = 1;
            } else {
                if (status == KERNEL_BLOCK_STATUS_OK) {
                    part->offset += part->copied;
                    part->input += part->copied;
                    part->remaining -= part->copied;
                    part->modify_ready = 0;
                }
                release_request(device, r); part->request = 0;
            }
        }
        if (!device_live(device) && result == KERNEL_BLOCK_STATUS_OK) result = KERNEL_BLOCK_STATUS_IO;
        for (size_t i = 0; i < count; i++) {
            struct batch_transfer *part = &parts[i];
            if (result != KERNEL_BLOCK_STATUS_OK) continue;
            if (!part->remaining || earlier_sector_pending(spans, parts, i)) continue;
            if (!part->request) {
                part->request = try_reserve_request(device);
                if (!part->request) continue;
                part->request->completion = &device->available;
            }
            if (part->request->state == 1) {
                enum kernel_block_status status = publish_write_part(device, part);
                if (status != KERNEL_BLOCK_STATUS_OK) result = status;
            }
        }
        if (result != KERNEL_BLOCK_STATUS_OK) {
            for (size_t i = 0; i < count; i++) {
                if (parts[i].request && parts[i].request->state == 1) {
                    release_request(device, parts[i].request); parts[i].request = 0;
                }
            }
        }
        unsigned pending = 0, remaining = 0;
        uint64_t deadline = 0;
        for (size_t i = 0; i < count; i++) {
            struct block_request *r = parts[i].request;
            remaining += parts[i].remaining != 0;
            if (!r || r->state != 2) continue;
            pending++;
            if (!deadline || (int64_t)(r->deadline - deadline) < 0) deadline = r->deadline;
        }
        if (!pending && (result != KERNEL_BLOCK_STATUS_OK || !remaining)) {
            arch_interrupt_restore(irq); break;
        }
        if (deadline && (int64_t)(time_now() - deadline) >= 0) {
            /* A completion may have reached the ring before its IRQ. Harvest
             * once at the deadline before deciding to revoke DMA by reset. */
            if (device_live(device)) collect_used(device);
            for (size_t i = 0; i < count; i++) {
                struct block_request *r = parts[i].request;
                if (r && r->state == 2 && (int64_t)(time_now() - r->deadline) >= 0) {
                    device->statistics.timeouts++;
                    fail_device(device, KERNEL_BLOCK_STATUS_TIMEOUT); break;
                }
            }
            arch_interrupt_restore(irq); continue;
        }
        if (device->irq_source) {
            enum kernel_wait_wake_reason reason;
            if (pending) device->statistics.sleeps++;
            else device->statistics.queue_waits++;
            /* Other calls may free slots while every request in this batch is
             * still pending. Their completion must also wake this publisher. */
            uint64_t batch_wait_start = time_now();
            if (kernel_scheduler_block_current(&device->available, deadline, 0, &reason) != KERNEL_SCHEDULER_STATUS_OK)
                __builtin_trap();
            device->statistics.queue_wait_ticks += time_now() - batch_wait_start;
            if (reason == KERNEL_WAIT_TIMEOUT && device_live(device)) collect_used(device);
        }
        arch_interrupt_restore(irq);
    }
    for (size_t i = 0; i < count; i++) if (parts[i].request) __builtin_trap();
    end_call(device, 0);
    return result;
}

struct batch_read_transfer {
    struct block_request *request;
    uint64_t offset;
    unsigned char *output;
    size_t remaining, copied;
};

static enum kernel_block_status publish_read_part(struct virtio_block_device *device,
    struct batch_read_transfer *part)
{
    uint64_t sector = part->offset / VIRTIO_BLOCK_SECTOR_SIZE, address;
    unsigned skip = part->offset % VIRTIO_BLOCK_SECTOR_SIZE;
    if (!skip && part->remaining >= VIRTIO_BLOCK_SECTOR_SIZE) {
        size_t size = part->remaining;
        size_t maximum = (size_t)UINT32_MAX & ~(size_t)(VIRTIO_BLOCK_SECTOR_SIZE - 1);
        if (size > maximum) size = maximum;
        size &= ~(size_t)(VIRTIO_BLOCK_SECTOR_SIZE - 1);
        if (device->dma_address(part->output, size, &address)) {
            part->copied = size;
            return publish_request(device, part->request, VIRTIO_BLOCK_REQUEST_IN,
                sector, address, (uint32_t)size, 0);
        }
    }
    part->copied = VIRTIO_BLOCK_SECTOR_SIZE - skip;
    if (part->copied > part->remaining) part->copied = part->remaining;
    return publish_request(device, part->request, VIRTIO_BLOCK_REQUEST_IN, sector,
        device->queue_physical_address + bounce_offset(device, part->request), VIRTIO_BLOCK_SECTOR_SIZE, 1);
}

static enum kernel_block_status virtio_block_read_batch(void *context,
    struct kernel_block_read_span *spans, size_t count)
{
    COST_SCOPE(device_cost, OPERATION_TICKS);
    struct virtio_block_device *device = context;
    if (!begin_call(device, 0)) {
        for (size_t i = 0; i < count; i++) if (spans[i].size) spans[i].status = KERNEL_BLOCK_STATUS_IO;
        return KERNEL_BLOCK_STATUS_IO;
    }
    struct kernel_io_context *owner = kernel_io_context_current();
    struct batch_read_transfer parts[KERNEL_BLOCK_BATCH_MAX] = {0};
    for (size_t i = 0; i < count; i++) {
        parts[i].offset = spans[i].offset; parts[i].output = spans[i].buffer; parts[i].remaining = spans[i].size;
    }
    for (;;) {
        uintptr_t irq = arch_interrupt_save();
        if (owner != kernel_io_context_current()) __builtin_trap();
        if (!device->irq_source && device_live(device)) collect_used(device);
        for (size_t i = 0; i < count; i++) {
            struct batch_read_transfer *part = &parts[i];
            struct block_request *r = part->request;
            if (!r || r->state != 3) continue;
            enum kernel_block_status status = finish_request(device, r);
            if (status == KERNEL_BLOCK_STATUS_OK) {
                if (r->bounce_used)
                    bytes_copy(part->output, r->bounce + part->offset % VIRTIO_BLOCK_SECTOR_SIZE, part->copied);
                part->output += part->copied; part->offset += part->copied;
                part->remaining -= part->copied; spans[i].completed += part->copied;
                if (!part->remaining) spans[i].status = KERNEL_BLOCK_STATUS_OK;
            } else {
                spans[i].status = status; part->remaining = 0;
            }
            release_request(device, r); part->request = 0;
        }
        for (size_t i = 0; i < count; i++) {
            struct batch_read_transfer *part = &parts[i];
            if (!part->remaining || part->request) continue;
            if (!device_live(device)) { spans[i].status = KERNEL_BLOCK_STATUS_IO; part->remaining = 0; continue; }
            part->request = try_reserve_request(device);
            if (!part->request) continue;
            part->request->completion = &device->available;
            enum kernel_block_status status = publish_read_part(device, part);
            if (status != KERNEL_BLOCK_STATUS_OK) {
                spans[i].status = status; part->remaining = 0;
                release_request(device, part->request); part->request = 0;
            }
        }
        unsigned pending = 0, remaining = 0;
        uint64_t deadline = 0;
        for (size_t i = 0; i < count; i++) {
            struct block_request *r = parts[i].request;
            remaining += parts[i].remaining != 0;
            if (!r) continue;
            if (r->state != 2) __builtin_trap();
            pending++;
            if (!deadline || (int64_t)(r->deadline - deadline) < 0) deadline = r->deadline;
        }
        /* 单span错误不撤销其他读取；尤其不能越过尚未完成的DMA归还buffer。 */
        if (!pending && !remaining) { arch_interrupt_restore(irq); break; }
        if (deadline && (int64_t)(time_now() - deadline) >= 0) {
            if (device_live(device)) collect_used(device);
            for (size_t i = 0; i < count; i++) {
                struct block_request *r = parts[i].request;
                if (r && r->state == 2 && (int64_t)(time_now() - r->deadline) >= 0) {
                    device->statistics.timeouts++; fail_device(device, KERNEL_BLOCK_STATUS_TIMEOUT); break;
                }
            }
            arch_interrupt_restore(irq); continue;
        }
        if (device->irq_source) {
            enum kernel_wait_wake_reason reason;
            if (pending) device->statistics.sleeps++; else device->statistics.queue_waits++;
            uint64_t wait_start = time_now();
            if (kernel_scheduler_block_current(&device->available, deadline, 0, &reason) != KERNEL_SCHEDULER_STATUS_OK)
                __builtin_trap();
            device->statistics.queue_wait_ticks += time_now() - wait_start;
            if (reason == KERNEL_WAIT_TIMEOUT && device_live(device)) collect_used(device);
        }
        arch_interrupt_restore(irq);
    }
    enum kernel_block_status result = KERNEL_BLOCK_STATUS_OK;
    for (size_t i = 0; i < count; i++) {
        if (parts[i].request) __builtin_trap();
        if (result == KERNEL_BLOCK_STATUS_OK && spans[i].status != KERNEL_BLOCK_STATUS_OK) result = spans[i].status;
    }
    end_call(device, 0);
    return result;
}

enum virtio_block_status virtio_block_init(
    struct virtio_block_device *device,const struct virtio_block_transport *transport,
    struct physical_page_allocator *page_allocator,virtio_dma_address_fn dma_address,
    uint32_t timebase_frequency)
{
    struct virtio_block_device result={0};
    enum virtio_block_status status;
    enum physical_page_status page_status;
    uint64_t capacity_sectors;
    uint32_t queue_max;
    void *queue_memory;
    if (!device || device->state || !transport || !transport->context || !transport->ops ||
        !transport->ops->read || !transport->ops->write || !transport->ops->ack_interrupt ||
        !page_allocator || !dma_address || !timebase_frequency)
        return VIRTIO_BLOCK_DRIVER_STATUS_INVALID;
    if (transport->version!=1 && transport->version!=2) return VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED;
    result.page_allocator=page_allocator;result.dma_address=dma_address;result.transport=*transport;
    result.transport_version=transport->version;
    result.queue_allocation_order=legacy_transport(&result) ? VIRTIO_LEGACY_QUEUE_ALLOCATION_ORDER : 1U;
    result.timeout_ticks=(uint64_t)timebase_frequency*30;
    result.statistics_start=time_now();result.statistics_last=result.statistics_start;
    status = negotiate_features(&result);
    if (status != VIRTIO_BLOCK_DRIVER_STATUS_OK) {
        return init_failure(&result, device, status, 1, 0);
    }

    reg_write(&result, VIRTIO_BLOCK_REG_QUEUE_SEL, 0U);
    queue_max = reg_read(&result, VIRTIO_BLOCK_REG_QUEUE_NUM_MAX);
    if (queue_max < 4 ||
        (!legacy_transport(&result) &&
         reg_read(&result, VIRTIO_BLOCK_REG_QUEUE_READY) != 0U)) {
        return init_failure(&result,
                            device,
                            VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED,
                            1,
                            0);
    }
    result.queue_size = VIRTIO_QUEUE_SIZE;
    while (result.queue_size > queue_max) result.queue_size /= 2;

    page_status = physical_page_allocate_order(page_allocator,
                                                result.queue_allocation_order,
                                                &result.queue_physical_address);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return init_failure(&result,
                            device,
                            page_status == PHYSICAL_PAGE_STATUS_EMPTY ?
                                VIRTIO_BLOCK_DRIVER_STATUS_NO_MEMORY :
                                VIRTIO_BLOCK_DRIVER_STATUS_STATE,
                            1,
                            0);
    }
    page_status = physical_page_resolve(page_allocator,
                                        result.queue_physical_address,
                                        &queue_memory);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return init_failure(&result,
                            device,
                            VIRTIO_BLOCK_DRIVER_STATUS_STATE,
                            1,
                            1);
    }
    result.queue_memory = queue_memory;
    bytes_zero(queue_memory,
               (size_t)BOAROS_PAGE_SIZE << result.queue_allocation_order);

    reg_write(&result,
                 VIRTIO_BLOCK_REG_QUEUE_NUM,
                 result.queue_size);
    if (legacy_transport(&result)) {
        if (result.queue_physical_address >> BOAROS_PAGE_SHIFT >
            UINT32_MAX) {
            return init_failure(&result,
                                device,
                                VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED,
                                1,
                                1);
        }
        reg_write(&result,
                     VIRTIO_BLOCK_REG_GUEST_PAGE_SIZE,
                     BOAROS_PAGE_SIZE);
        reg_write(&result,
                     VIRTIO_BLOCK_REG_QUEUE_ALIGN,
                     VIRTIO_LEGACY_QUEUE_ALIGNMENT);
        memory_barrier();
        reg_write(&result,
                     VIRTIO_BLOCK_REG_QUEUE_PFN,
                     (uint32_t)(result.queue_physical_address >>
                                BOAROS_PAGE_SHIFT));
    } else {
        write_queue_address(&result,
                            VIRTIO_BLOCK_REG_QUEUE_DESC_LOW,
                            VIRTIO_BLOCK_REG_QUEUE_DESC_HIGH,
                            result.queue_physical_address +
                                VIRTIO_QUEUE_DESC_OFFSET);
        write_queue_address(&result,
                            VIRTIO_BLOCK_REG_QUEUE_DRIVER_LOW,
                            VIRTIO_BLOCK_REG_QUEUE_DRIVER_HIGH,
                            result.queue_physical_address +
                                queue_avail_offset(&result));
        write_queue_address(&result,
                            VIRTIO_BLOCK_REG_QUEUE_DEVICE_LOW,
                            VIRTIO_BLOCK_REG_QUEUE_DEVICE_HIGH,
                            result.queue_physical_address +
                                queue_used_offset(&result));
        memory_barrier();
        reg_write(&result, VIRTIO_BLOCK_REG_QUEUE_READY, 1U);
    }

    capacity_sectors = read_capacity(&result);
    if (capacity_sectors == 0U ||
        capacity_sectors > UINT64_MAX / VIRTIO_BLOCK_SECTOR_SIZE) {
        return init_failure(&result,
                            device,
                            VIRTIO_BLOCK_DRIVER_STATUS_DEVICE,
                            1,
                            1);
    }

    reg_write(&result,
                 VIRTIO_BLOCK_REG_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE |
                     VIRTIO_STATUS_DRIVER |
                     (legacy_transport(&result) ? 0U
                                                 : VIRTIO_STATUS_FEATURES_OK) |
                     VIRTIO_STATUS_DRIVER_OK);
    memory_barrier();

    result.block.context = device;
    result.block.read = virtio_block_read;
    result.block.read_batch = virtio_block_read_batch;
    result.block.write = result.read_only != 0U ? 0 : virtio_block_write;
    result.block.write_batch = result.read_only != 0U ? 0 : virtio_block_write_batch;
    result.block.flush = virtio_block_flush;
    result.block.capacity_bytes =
        capacity_sectors * VIRTIO_BLOCK_SECTOR_SIZE;
    result.block.logical_block_size = VIRTIO_BLOCK_SECTOR_SIZE;
    kernel_wait_queue_init(&result.available);
    for (unsigned i = 0; i < slot_count(&result); i++) kernel_wait_queue_init(&request_at(&result, i)->done);
    result.state = VIRTIO_BLOCK_STATE_LIVE;
    *device = result;
    device->block.context = device;
    return VIRTIO_BLOCK_DRIVER_STATUS_OK;
}

enum virtio_block_status virtio_block_destroy(
    struct virtio_block_device *device)
{
    enum physical_page_status page_status;

    if (device == 0 ||
        (device->state != VIRTIO_BLOCK_STATE_LIVE &&
         device->state != VIRTIO_BLOCK_STATE_FAILED)) {
        return VIRTIO_BLOCK_DRIVER_STATUS_STATE;
    }

    if (device->active || device->available.head) return VIRTIO_BLOCK_DRIVER_STATUS_STATE;
    /* 最终统计行：时间加权在途深度与等待/服务时间供真实窗口取证。 */
    struct virtio_block_statistics statistics;
    virtio_block_get_statistics(device, &statistics);
    block_puts("BoarOS: block final device=");
    block_hex((unsigned long)device->block.device_number);
    block_puts(" requests="); block_hex(statistics.requests);
    block_puts(" max-inflight="); block_hex(statistics.max_inflight);
    block_puts(" inflight-ticks="); block_hex(statistics.inflight_ticks);
    block_puts(" busy-ticks="); block_hex(statistics.busy_ticks);
    block_puts(" total-ticks="); block_hex(statistics.total_ticks);
    block_puts(" wait-ticks="); block_hex(statistics.queue_wait_ticks);
    block_puts(" service-ticks="); block_hex(statistics.service_ticks);
    block_puts(" timeouts="); block_hex(statistics.timeouts);
    block_puts(" errors="); block_hex(statistics.io_errors);
    block_puts(" flushes="); block_hex(statistics.flush_requests);
    kernel_console_putc('\n');
    device_reset(device);
    if (device->irq_source) { device->transport.ops->unregister_irq(device->transport.context,device->irq_source,device); device->irq_source = 0; }
    page_status = physical_page_release_order(device->page_allocator,
                                              device->queue_physical_address,
                                              device->queue_allocation_order);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return VIRTIO_BLOCK_DRIVER_STATUS_STATE;
    }

    device->block.context = 0;
    device->block.read = 0;
    device->block.read_batch = 0;
    device->block.write = 0;
    device->block.write_batch = 0;
    device->block.flush = 0;
    device->block.cache_mode = KERNEL_BLOCK_CACHE_UNKNOWN;
    device->block.capacity_bytes = 0U;
    device->block.logical_block_size = 0U;
    device->queue_memory = 0;
    device->state = VIRTIO_BLOCK_STATE_DESTROYED;
    return VIRTIO_BLOCK_DRIVER_STATUS_OK;
}

void virtio_block_get_statistics(
    const struct virtio_block_device *device,
    struct virtio_block_statistics *statistics)
{
    if (device == 0 || statistics == 0 ||
        (device->state != VIRTIO_BLOCK_STATE_LIVE &&
         device->state != VIRTIO_BLOCK_STATE_FAILED &&
         device->state != VIRTIO_BLOCK_STATE_DESTROYED)) {
        return;
    }

    uint64_t now = time_now();
    *statistics = device->statistics;
    /* 查询时收口打开的区间，使深度与忙时覆盖到调用时刻。 */
    uint64_t elapsed = now - device->statistics_last;
    statistics->inflight_ticks += (uint64_t)device->inflight * elapsed;
    if (device->inflight) statistics->busy_ticks += elapsed;
    statistics->total_ticks = now - device->statistics_start;
}
