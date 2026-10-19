#include <kernel/cost.h>
#include <kernel/virtio_block.h>
#include <kernel/page.h>
#include <kernel/sync.h>
#include <kernel/irq.h>
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

#define VIRTIO_BLOCK_FEATURE_SIZE_MAX (UINT32_C(1) << 1)
#define VIRTIO_BLOCK_FEATURE_SEG_MAX (UINT32_C(1) << 2)
#define VIRTIO_BLOCK_FEATURE_RO (UINT32_C(1) << 5)
#define VIRTIO_BLOCK_FEATURE_FLUSH (UINT32_C(1) << 9)

#define VIRTIO_BLOCK_REQUEST_IN 0U
#define VIRTIO_BLOCK_REQUEST_OUT 1U
#define VIRTIO_BLOCK_REQUEST_FLUSH 4U
#define VIRTIO_BLOCK_STATUS_OK 0U
#define VIRTIO_BLOCK_STATUS_IO_ERROR 1U
#define VIRTIO_BLOCK_TRANSFER_BYTES (128U * 1024U)
#define VIRTIO_BLOCK_STATUS_UNSUPPORTED 2U
#define VIRTIO_BLOCK_SECTOR_SIZE 512U

#define VIRTQ_DESC_NEXT UINT16_C(1)
#define VIRTQ_DESC_WRITE UINT16_C(2)
#define VIRTIO_QUEUE_SIZE 64U
#define REQUEST_SLOTS VIRTIO_BLOCK_REQUEST_SLOTS
#define REQUEST_STRIDE 768U

#define VIRTIO_QUEUE_DESC_OFFSET 0U
#define VIRTIO_REQUEST_HEADER_OFFSET 2048U
#define VIRTIO_LEGACY_QUEUE_USED_OFFSET BOAROS_PAGE_SIZE
#define VIRTIO_LEGACY_REQUEST_HEADER_OFFSET (2U * BOAROS_PAGE_SIZE)
#define VIRTIO_LEGACY_QUEUE_ALLOCATION_ORDER 2U
#define VIRTIO_LEGACY_QUEUE_ALIGNMENT BOAROS_PAGE_SIZE

#define VIRTIO_BLOCK_STATE_EMPTY 0U
#define VIRTIO_BLOCK_STATE_LIVE UINT32_C(0x56424c4b)
#define VIRTIO_BLOCK_STATE_FAILED UINT32_C(0x56464149)
#define VIRTIO_BLOCK_STATE_DESTROYED UINT32_C(0x56444541)

struct virtio_block_request_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

struct block_request {
    struct virtio_block_request_header header;
    volatile unsigned char status;
    unsigned state; /* 0 free, 1 reserved, 2 submitted, 3 completed */
    enum kernel_block_status result;
    struct kernel_io_context *owner;
    struct kernel_wait_queue done;
    struct kernel_wait_queue *completion;
    uint64_t deadline;
    uint32_t data_length;
    int bounce_used;
    unsigned char bounce[VIRTIO_BLOCK_SECTOR_SIZE];
#if BOAROS_COST_DIAGNOSTICS
    uint64_t publish_time;
    struct kernel_cost_tag cost_tag;
    uint64_t cost_start;
    uint64_t cost_completed;
    unsigned cost_device;
    unsigned cost_woken;
#endif
};
struct block_segment { uint64_t address; uint32_t length; };
struct batch_transfer {
    struct block_request *request;
    uint64_t offset;
    unsigned char *buffer;
    size_t remaining, copied;
    uint32_t members;
    uint8_t modify_ready, next_ready;
};

struct block_batch_workspace {
    struct batch_transfer parts[KERNEL_BLOCK_BATCH_MAX];
    struct block_segment segments[KERNEL_BLOCK_BATCH_MAX];
};
_Static_assert(sizeof(struct block_batch_workspace) <= VIRTIO_BLOCK_BATCH_WORKSPACE_BYTES, "batch workspace exceeds pool slot");

_Static_assert(BOAROS_PAGE_SIZE >= VIRTIO_BLOCK_BATCH_WORKSPACE_BYTES &&
    BOAROS_PAGE_SIZE % VIRTIO_BLOCK_BATCH_WORKSPACE_BYTES == 0, "batch slots must fit whole pages");
_Static_assert(sizeof(struct block_request) <= REQUEST_STRIDE, "request slot overflow");
_Static_assert((((16U * VIRTIO_QUEUE_SIZE + 6U + 2U * VIRTIO_QUEUE_SIZE + 15U) & ~15U) +
    6U + 8U * VIRTIO_QUEUE_SIZE) <= VIRTIO_REQUEST_HEADER_OFFSET, "modern ring overlaps requests");
_Static_assert(VIRTIO_REQUEST_HEADER_OFFSET + REQUEST_SLOTS * REQUEST_STRIDE <= 8192, "modern queue overflow");
_Static_assert(VIRTIO_LEGACY_REQUEST_HEADER_OFFSET + REQUEST_SLOTS * REQUEST_STRIDE <= (4U * BOAROS_PAGE_SIZE), "legacy queue overflow");

static int legacy_transport(const struct virtio_block_device *device)
{
    return device->transport_version == 1U;
}

static uint32_t queue_avail_offset(const struct virtio_block_device *device)
{ return device->queue_size * sizeof(struct virtio_descriptor); }
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
    return device != 0 && __atomic_load_n(&device->state,__ATOMIC_RELAXED) == VIRTIO_BLOCK_STATE_LIVE;
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
static uint32_t reg_read(const struct virtio_block_device *device,enum virtio_register reg)
{ return device->transport.ops.read(device->transport.context,reg); }
static void reg_write(struct virtio_block_device *device,enum virtio_register reg,uint32_t value)
{ device->transport.ops.write(device->transport.context,reg,value); }
static void memory_barrier(void) { arch_dma_barrier(); }
static uint64_t time_now(void) { return arch_time_read(); }

/* 在途深度变化前结算一段时间：累计 Σ深度·Δt 与忙时。 */
#if BOAROS_COST_DIAGNOSTICS
static void account_inflight(struct virtio_block_device *device)
{
    uint64_t now = time_now();
    uint64_t elapsed = now - device->statistics_last;

    device->statistics.inflight_ticks += (uint64_t)device->queue.inflight * elapsed;
    if (device->queue.inflight) device->statistics.busy_ticks += elapsed;
    device->statistics_last = now;
}
#else
#define account_inflight(device) ((void)0)
#endif

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

static void device_reset(struct virtio_block_device *device)
{
    /* QEMU synchronous reset must confirm before returning borrowed caller DMA. */
    if (virtio_transport_reset(&device->transport) != VIRTIO_OK) __builtin_trap();
    if (device->queue.size && virtio_split_reset(&device->queue, &device->transport) != VIRTIO_OK)
        __builtin_trap();
}

static void release_batch_pool(struct virtio_block_device *device)
{
    for (unsigned i = 0; i < VIRTIO_BLOCK_BATCH_POOL_PAGES; i++) {
        if (!device->batch_physical[i]) continue;
        if (physical_page_release_order(device->page_allocator, device->batch_physical[i], 0) != PHYSICAL_PAGE_STATUS_OK)
            __builtin_trap();
        device->batch_physical[i] = 0; device->batch_memory[i] = NULL;
    }
}

static enum virtio_block_status init_failure(
    struct virtio_block_device *device,
    enum virtio_block_status status,
    int reset,
    int release_queue)
{
    if (reset) {
        reg_write(device,
                     VIRTIO_REG_STATUS,
                     VIRTIO_STATUS_FAILED);
        device_reset(device);
    }
    if (release_queue) {
        release_batch_pool(device);
        enum physical_page_status page_status =
            physical_page_release_order(device->page_allocator,
                                        device->queue_physical_address,
                                        device->queue_allocation_order);

        if (page_status != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    }
    return status;
}

static enum virtio_block_status negotiate_features(
    struct virtio_block_device *device)
{
    uint64_t features;
    enum virtio_status status = virtio_transport_begin(&device->transport,
        VIRTIO_BLOCK_FEATURE_RO | VIRTIO_BLOCK_FEATURE_FLUSH |
        VIRTIO_BLOCK_FEATURE_SIZE_MAX | VIRTIO_BLOCK_FEATURE_SEG_MAX, 0, &features);
    if (status != VIRTIO_OK) return status == VIRTIO_UNSUPPORTED ?
        VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED : VIRTIO_BLOCK_DRIVER_STATUS_DEVICE;
    device->read_only = (features & VIRTIO_BLOCK_FEATURE_RO) != 0;
    /* 不协商CONFIG_WCE：原FLUSH/write-through业务契约保持。 */
    device->block.cache_mode = (features & VIRTIO_BLOCK_FEATURE_FLUSH) ?
        KERNEL_BLOCK_CACHE_WRITEBACK : KERNEL_BLOCK_CACHE_WRITETHROUGH;
    return VIRTIO_BLOCK_DRIVER_STATUS_OK;
}

static int read_segment_limits(struct virtio_block_device *device)
{
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        uint32_t generation = legacy_transport(device) ? 0 : reg_read(device, VIRTIO_REG_CONFIG_GENERATION);
        uint32_t size = UINT32_MAX, count = KERNEL_BLOCK_BATCH_MAX;
        if ((device->transport.features & VIRTIO_BLOCK_FEATURE_SIZE_MAX) &&
            virtio_transport_config_read(&device->transport, 8, 4, &size) != VIRTIO_OK) return 0;
        if ((device->transport.features & VIRTIO_BLOCK_FEATURE_SEG_MAX) &&
            virtio_transport_config_read(&device->transport, 12, 4, &count) != VIRTIO_OK) return 0;
        if (!legacy_transport(device) && generation != reg_read(device, VIRTIO_REG_CONFIG_GENERATION)) continue;
        /* direct 与 bounce 都传完整扇区；不足一扇区的上限无法承载 RMW。 */
        size &= ~(VIRTIO_BLOCK_SECTOR_SIZE - 1U);
        if (!size || !count) return 0;
        device->max_segment_size = size;
        device->max_segments = count < KERNEL_BLOCK_BATCH_MAX ? count : KERNEL_BLOCK_BATCH_MAX;
        if (device->max_segments > device->queue_size - 2U) device->max_segments = device->queue_size - 2U;
        return 1;
    }
    return 0;
}

static uint64_t read_capacity(struct virtio_block_device *device)
{
    uint32_t attempt;

    if (legacy_transport(device)) {
        uint32_t low, high;
        if (virtio_transport_config_read(&device->transport, 0, 4, &low) != VIRTIO_OK ||
            virtio_transport_config_read(&device->transport, 4, 4, &high) != VIRTIO_OK) return 0;

        return ((uint64_t)high << 32U) | low;
    }

    for (attempt = 0U; attempt < 8U; attempt++) {
        uint32_t generation_before =
            reg_read(device, VIRTIO_REG_CONFIG_GENERATION);
        uint32_t low, high;
        if (virtio_transport_config_read(&device->transport, 0, 4, &low) != VIRTIO_OK ||
            virtio_transport_config_read(&device->transport, 4, 4, &high) != VIRTIO_OK) return 0;
        uint32_t generation_after =
            reg_read(device, VIRTIO_REG_CONFIG_GENERATION);

        if (generation_before == generation_after) {
            return ((uint64_t)high << 32U) | low;
        }
    }

    return 0U;
}

static void wake(struct kernel_wait_queue *queue)
{
    /* 这些私有队列只在设备保护内登记；锁外finish只能移除节点。 */
    if (__atomic_load_n(&queue->head,__ATOMIC_RELAXED) &&
        kernel_wait_queue_wake_all(queue) != KERNEL_SCHEDULER_STATUS_OK)
        __builtin_trap();
}
static void diagnose_queue_fault(struct virtio_block_device *device,
    const char *reason, uint16_t used_index, uint16_t observed_count,
    uint32_t pending, const struct virtio_used_element *item);
static void fail_device(struct virtio_block_device *device, enum kernel_block_status result)
{
    __atomic_store_n(&device->state,VIRTIO_BLOCK_STATE_FAILED,__ATOMIC_RELAXED);
    if (result == KERNEL_BLOCK_STATUS_TIMEOUT) {
        volatile struct virtio_used_ring *used = (void *)((unsigned char *)device->queue_memory + queue_used_offset(device));
        uint16_t used_index = used->index;
        memory_barrier();
        /* 纯超时没有非法 used 项；reset 前仍需保留设备身份与在途 owner。 */
        diagnose_queue_fault(device, "timeout", used_index,
            (uint16_t)(used_index - device->queue.last_used),
            device->transport.ops.ack_interrupt(device->transport.context), NULL);
        block_puts("BoarOS: block timeout; resetting device\n");
    }
    account_inflight(device);
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
            device->statistics.service_ticks += time_now() - r->publish_time;
#endif
        }
        /* 本轮收割中先完成的owner也必须收到通知，不能被后面的坏used项截断。 */
        if (r->state == 3) {
            if (r->completion->head) device->statistics.wakes++;
            wake(r->completion);
        }
    }
    wake(&device->admission);
    wake(&device->idle);
    wake(&device->slots);
    wake(&device->descriptors);
    wake(&device->workspaces);
}

static void queue_fault_scalar(const char *name, uint64_t value)
{
    block_puts(name);
    block_hex((unsigned long)value);
}

static void diagnose_queue_fault(struct virtio_block_device *device,
    const char *reason, uint16_t used_index, uint16_t observed_count,
    uint32_t pending, const struct virtio_used_element *item)
{
    volatile struct virtio_available_ring *available = (void *)((unsigned char *)device->queue_memory + queue_avail_offset(device));
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
    queue_fault_scalar(" device_status=", reg_read(device, VIRTIO_REG_STATUS));
    queue_fault_scalar(" interrupt=", pending);
    queue_fault_scalar(" transport=", device->transport_version);
    queue_fault_scalar(" queue=", (uintptr_t)device->queue_memory);
    queue_fault_scalar(" physical=", device->queue_physical_address);
    queue_fault_scalar(" size=", device->queue_size);
    block_puts("\n");
    queue_fault_scalar(" used=", used_index);
    queue_fault_scalar(" consumed=", device->queue.last_used);
    queue_fault_scalar(" avail=", available->index);
    queue_fault_scalar(" observed_count=", observed_count);
    queue_fault_scalar(" inflight=", device->queue.inflight);
    queue_fault_scalar(" active=", device->active);
    queue_fault_scalar(" reserved=", reserved);
    queue_fault_scalar(" published=", published);
    queue_fault_scalar(" complete=", complete);
    block_puts("\n");
    if (item != NULL) {
        queue_fault_scalar(" item_index=", (uint16_t)(device->queue.last_used - 1));
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
        queue_fault_scalar(" type=", r->header.type);
        queue_fault_scalar(" bytes=", r->data_length);
        queue_fault_scalar(" sector=", r->header.sector);
        queue_fault_scalar(" deadline=", r->deadline);
        block_puts("\n");
    }
}

static unsigned first_descriptor(uint64_t bits);
static void harvest_used(struct virtio_block_device *device, uint32_t pending)
{
    uint32_t completed = 0;
    uint16_t used_index = device->queue.used->index;
    uint16_t observed_count = (uint16_t)(used_index - device->queue.last_used);
    if (observed_count > slot_count(device)) {
        diagnose_queue_fault(device, "used-overflow", used_index, observed_count, pending, NULL);
        fail_device(device, KERNEL_BLOCK_STATUS_IO); return;
    }
    for (;;) {
        struct virtio_completion completion;
        account_inflight(device);
        enum virtio_status status = virtio_split_take(&device->queue, &completion);
        if (status == VIRTIO_EMPTY) break;
        if (status != VIRTIO_OK) {
            const struct virtio_queue_fault *fault = &device->queue.fault;
            struct virtio_used_element item = {fault->head, fault->length};
            const char *reason = fault->reason == VIRTIO_QUEUE_OVERFLOW ? "used-overflow" :
                fault->head >= device->queue.size || !fault->length ||
                (fault->reason == VIRTIO_QUEUE_BAD_HEAD && (device->queue.claimed & (UINT64_C(1) << fault->head))) ?
                "used-element" : fault->reason == VIRTIO_QUEUE_BAD_LENGTH ? "used-length" : "slot-state";
            diagnose_queue_fault(device, reason, used_index, observed_count, pending,
                fault->reason == VIRTIO_QUEUE_OVERFLOW ? NULL : &item);
            fail_device(device, KERNEL_BLOCK_STATUS_IO); return;
        }
        struct virtio_used_element item = {completion.head, completion.length};
        struct block_request *r = completion.token;
        uintptr_t offset = (uintptr_t)r - (uintptr_t)request_at(device, 0);
        if (offset / REQUEST_STRIDE >= slot_count(device) || offset % REQUEST_STRIDE || r->state != 2) {
            diagnose_queue_fault(device, "slot-state", used_index, observed_count, pending, &item);
            fail_device(device, KERNEL_BLOCK_STATUS_IO); return;
        }
        /* modern IN 的 status 在全部数据之后；短完成并不覆盖它。合法
         * used 已结束此请求 DMA，按请求失败归还，不因此复位其他 owner。 */
        int short_read = !legacy_transport(device) && r->header.type == VIRTIO_BLOCK_REQUEST_IN &&
            completion.length != r->data_length + 1;
        r->result = short_read ? KERNEL_BLOCK_STATUS_IO :
            r->status == VIRTIO_BLOCK_STATUS_OK ? KERNEL_BLOCK_STATUS_OK :
            r->status == VIRTIO_BLOCK_STATUS_UNSUPPORTED ? KERNEL_BLOCK_STATUS_UNSUPPORTED : KERNEL_BLOCK_STATUS_IO;
        if (!short_read && r->status > VIRTIO_BLOCK_STATUS_UNSUPPORTED) {
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
        device->statistics.service_ticks += time_now() - r->publish_time;
#endif
        if (r->result != KERNEL_BLOCK_STATUS_OK) device->statistics.io_errors++;
        int first = 1;
        uint32_t previous = completed;
        while (previous) {
            unsigned j = first_descriptor(previous);
            previous &= previous - 1;
            if (request_at(device, j)->completion == r->completion) { first = 0; break; }
        }
        completed |= UINT32_C(1) << (offset / REQUEST_STRIDE);
        if (first) {
            if (r->completion->head) device->statistics.wakes++;
            wake(r->completion);
        }
#if BOAROS_COST_DIAGNOSTICS
        if (r->cost_woken)
            kernel_cost_add_tag(r->cost_tag, COST_IO_COMPLETE_TO_READY, kernel_cost_clock() - r->cost_completed);
#endif
    }
    if (completed) wake(&device->descriptors);
}
static uint32_t acknowledge_interrupt(struct virtio_block_device *device)
{
    uint32_t pending = device->transport.ops.ack_interrupt(device->transport.context);
    if (pending) arch_io_barrier();
    return pending;
}
static void collect_used_locked(struct virtio_block_device *device)
{
    if (device->queue.used->index == device->queue.last_used) return;
    harvest_used(device, acknowledge_interrupt(device));
}
static int completion_wait_needed(struct virtio_block_device *device, const struct block_request *request)
{
    if (!device_live(device)) return 0;
    if (device->queue.used->index != device->queue.last_used) {
        collect_used_locked(device);
        if (!request) return 0;
    }
    return device_live(device) && (!request || request->state == 2);
}
static void wait_for_progress(struct virtio_block_device *device, struct kernel_wait_queue *queue,
    uint64_t deadline, const struct block_request *request, int submitted,
    struct kernel_wait_queue *also,struct kernel_raw_guard *guard)
{
    if (!completion_wait_needed(device, request)) return;
    struct kernel_wait_token token;
    enum kernel_wait_wake_reason reason = KERNEL_WAIT_WOKEN;
    if (kernel_wait_prepare(queue, deadline, 0, &token) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    struct kernel_wait_node capacity = {.task = token.task};
    if (also && kernel_wait_node_bind(also, &capacity, &token) != KERNEL_SCHEDULER_STATUS_OK)
        __builtin_trap();
    /* 登记期间设备仍可完成DMA；先更新软件owner，再决定是否真正park。 */
    int needed=completion_wait_needed(device,request);
#if BOAROS_COST_DIAGNOSTICS
    uint64_t wait_start=needed ? time_now() : 0;
#endif
    if (needed) {
        if (submitted) device->statistics.sleeps++; else device->statistics.queue_waits++;
    }
    /* 条件和登记连续完成；DMA/另一CPU的通知在解锁后也能命中PREPARED。 */
    kernel_raw_lock_release(guard);
    if (needed) {
        if (kernel_wait_park(&token, &reason) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
    if (kernel_wait_finish(&token) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    kernel_raw_lock_acquire(&device->metadata,guard);
#if BOAROS_COST_DIAGNOSTICS
    if (needed) device->statistics.queue_wait_ticks += time_now()-wait_start;
#endif
    if (reason == KERNEL_WAIT_TIMEOUT && device_live(device)) collect_used_locked(device);
}
static void block_irq(void *owner)
{
    struct virtio_block_device *device = owner;
    KERNEL_RAW_SCOPE(guard,&device->metadata);
    device->statistics.interrupts++;
    uint32_t pending = acknowledge_interrupt(device);
    if (device_live(device)) harvest_used(device, pending);
}
int virtio_block_enable_irq(struct virtio_block_device *device, uint32_t source)
{
    kernel_assert_service_context();
    if (!device_live(device) || device->active || device->irq_source ||
        !device->transport.ops.register_irq || !device->transport.ops.unregister_irq ||
        !device->transport.ops.register_irq(device->transport.context,source,block_irq,device)) return 0;
    device->irq_source = source;
    return 1;
}
static int begin_call(struct virtio_block_device *device, int barrier)
{
    KERNEL_IRQ_SCOPE(irq);
    if (device->irq_source && (!kernel_scheduler_can_sleep() || arch_external_interrupt_active())) __builtin_trap();
    KERNEL_RAW_SCOPE(guard,&device->metadata);
    if (barrier) device->barrier_waiters++;
    for (;;) {
        if (!device_live(device)) break;
        if (!device->barrier && (barrier ? !device->active : !device->barrier_waiters)) {
            if (device->active == UINT32_MAX) __builtin_trap();
            device->active++;
            if (barrier) { device->barrier_waiters--; device->barrier = 1; }
            return 1;
        }
        if (!device->irq_source) __builtin_trap();
        wait_for_progress(device,barrier ? &device->idle : &device->admission,0,NULL,0,NULL,&guard);
    }
    if (barrier) device->barrier_waiters--;
    return 0;
}
static void end_call(struct virtio_block_device *device, int barrier)
{
    KERNEL_RAW_SCOPE(guard,&device->metadata);
    if (!device->active || (barrier && !device->barrier)) __builtin_trap();
    device->active--;
    if (barrier) device->barrier = 0;
    if (!device->active) wake(&device->idle);
    if (barrier && !device->barrier_waiters) wake(&device->admission);
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
    KERNEL_RAW_SCOPE(guard,&device->metadata);
    struct block_request *r = 0;
    while (device_live(device) && !(r = try_reserve_request(device))) {
        if (!device->irq_source) __builtin_trap();
        wait_for_progress(device, &device->slots, 0, NULL, 0, NULL,&guard);
    }
    if (!device_live(device)) r = 0;
    return r;
}
static void release_request_owner(struct virtio_block_device *device, struct block_request *r)
{
    if (r->state != 1 || r->owner != kernel_io_context_current() || !device->active || r->done.head)
        __builtin_trap();
    r->state = 0; r->owner = 0; r->completion = 0;
}
static void release_request(struct virtio_block_device *device, struct block_request *r)
{
    KERNEL_RAW_SCOPE(guard,&device->metadata);
    release_request_owner(device, r);
    wake(&device->slots);
}

static uint64_t free_descriptor_mask(const struct virtio_block_device *device)
{
    uint64_t mask = device->queue.size == 64 ? UINT64_MAX :
        (UINT64_C(1) << device->queue.size) - 1;
    return mask & ~device->queue.claimed;
}
static unsigned free_descriptors(const struct virtio_block_device *device)
{
    uint64_t bits = free_descriptor_mask(device);
    bits -= (bits >> 1) & UINT64_C(0x5555555555555555);
    bits = (bits & UINT64_C(0x3333333333333333)) + ((bits >> 2) & UINT64_C(0x3333333333333333));
    bits = (bits + (bits >> 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
    return (unsigned)((bits * UINT64_C(0x0101010101010101)) >> 56);
}
static unsigned first_descriptor(uint64_t bits)
{
    unsigned first = 0;
    if (!(bits & UINT32_MAX)) { first += 32; bits >>= 32; }
    if (!(bits & UINT16_MAX)) { first += 16; bits >>= 16; }
    if (!(bits & UINT8_MAX)) { first += 8; bits >>= 8; }
    if (!(bits & 15U)) { first += 4; bits >>= 4; }
    if (!(bits & 3U)) { first += 2; bits >>= 2; }
    return first + !(bits & 1U);
}

/* Device metadata protection covers descriptor selection through enqueue. Request slots own
 * header/status/bounce, while only the queue owns published descriptors. */
static enum kernel_block_status publish_segments(struct virtio_block_device *device,
    struct block_request *r, uint32_t type, uint64_t sector,
    const struct block_segment *segments, unsigned count, int bounce)
{
    if (!device_live(device)) return KERNEL_BLOCK_STATUS_IO;
    if (r->state != 1 || r->owner != kernel_io_context_current() || count + 2 > device->queue.size) __builtin_trap();
    uint16_t indices[VIRTIO_SPLIT_MAX_SIZE];
    unsigned found = 0;
    uint64_t free = free_descriptor_mask(device);
    while (free && found < count + 2) {
        indices[found++] = first_descriptor(free);
        free &= free - 1;
    }
    if (found != count + 2) return KERNEL_BLOCK_STATUS_NOT_SUBMITTED;
    uint32_t data_length = 0;
    for (unsigned i = 0; i < count; i++) {
        if (data_length > UINT32_MAX - segments[i].length) __builtin_trap();
        data_length += segments[i].length;
    }
    int flushing = type == VIRTIO_BLOCK_REQUEST_FLUSH;
    r->header = (struct virtio_block_request_header){type, 0, sector};
    r->status = UINT8_MAX; r->state = 2;
    r->data_length = data_length; r->bounce_used = bounce;
    uint64_t publish_now = time_now();
    r->deadline = publish_now + device->timeout_ticks;
    account_inflight(device);
    device->statistics.requests++;
#if BOAROS_COST_DIAGNOSTICS
    r->publish_time = publish_now;
    r->cost_tag = kernel_cost_capture();
    r->cost_start = kernel_cost_clock();
    r->cost_completed = 0;
    r->cost_woken = 0;
    r->cost_device = device->block.registered && device->block.device_number == KERNEL_BLOCK_DEVICE_NUMBER(0) ? 0 :
        device->block.registered && device->block.device_number == KERNEL_BLOCK_DEVICE_NUMBER(1) ? 1 : 2;
    enum kernel_cost_metric metric = (enum kernel_cost_metric)(COST_DEVICE0_REQUESTS + r->cost_device * 8);
    kernel_cost_add_tag(r->cost_tag, metric, 1);
    kernel_cost_add_tag(r->cost_tag, (enum kernel_cost_metric)(metric + 6), device->queue.inflight + 1);
    unsigned phase = kernel_cost_current()->operation >> 4;
    if (!flushing && type == VIRTIO_BLOCK_REQUEST_OUT && phase >= 1 && phase <= 4) {
        kernel_cost_add_tag(r->cost_tag, (enum kernel_cost_metric)(COST_DEVICE0_DATA_WRITE + r->cost_device * 4 + phase - 1), data_length);
    } else if (type == VIRTIO_BLOCK_REQUEST_IN && phase >= 5 && phase <= 7) {
        kernel_cost_add_tag(r->cost_tag, (enum kernel_cost_metric)(COST_DEVICE0_FILE_READ + r->cost_device * 3 + phase - 5), data_length);
    } else kernel_cost_add_tag(r->cost_tag, (enum kernel_cost_metric)(metric + (flushing ? 3 : type == VIRTIO_BLOCK_REQUEST_IN ? 4 : 5)), flushing ? 1 : data_length);
#else
    (void)flushing;
#endif
    struct virtio_descriptor *d = device->queue.descriptors;
    uint64_t physical = request_physical(device, r);
    d[indices[0]] = (struct virtio_descriptor){physical, sizeof(r->header), VIRTQ_DESC_NEXT, indices[1]};
    for (unsigned i = 0; i < count; i++)
        d[indices[i + 1]] = (struct virtio_descriptor){segments[i].address, segments[i].length,
            VIRTQ_DESC_NEXT | (type == VIRTIO_BLOCK_REQUEST_IN ? VIRTQ_DESC_WRITE : 0), indices[i + 2]};
    d[indices[count + 1]] = (struct virtio_descriptor){physical + offsetof(struct block_request, status), 1, VIRTQ_DESC_WRITE, 0};
    if (virtio_split_enqueue(&device->queue, &device->transport, indices[0], r,
        !legacy_transport(device) && type == VIRTIO_BLOCK_REQUEST_IN ? 0 : 1,
        1U + (type == VIRTIO_BLOCK_REQUEST_IN ? data_length : 0)) != VIRTIO_OK) __builtin_trap();
    if (device->queue.inflight > device->statistics.max_inflight) device->statistics.max_inflight = device->queue.inflight;
    return KERNEL_BLOCK_STATUS_OK;
}

static enum kernel_block_status publish_request_locked(struct virtio_block_device *device,
    struct block_request *r, uint32_t type, uint64_t sector,
    uint64_t data_address, uint32_t data_length, int bounce)
{
    struct block_segment segment = {data_address, data_length};
    enum kernel_block_status status = publish_segments(device, r, type, sector,
        &segment, type == VIRTIO_BLOCK_REQUEST_FLUSH ? 0 : 1, bounce);
    if (status == KERNEL_BLOCK_STATUS_OK &&
        virtio_split_notify(&device->queue, &device->transport, 0) != VIRTIO_OK) __builtin_trap();
    return status;
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
    if (r->header.type == VIRTIO_BLOCK_REQUEST_FLUSH) device->statistics.flush_requests++;
    else {
        if (r->header.type == VIRTIO_BLOCK_REQUEST_IN) device->statistics.sectors_read += r->data_length / 512;
        else device->statistics.sectors_written += r->data_length / 512;
        if (r->bounce_used) device->statistics.bounce_requests++; else device->statistics.direct_requests++;
    }
    return result;
}
static enum kernel_block_status wait_request(struct virtio_block_device *device,
                                             struct block_request *r)
{
    KERNEL_IRQ_SCOPE(irq);
    KERNEL_RAW_SCOPE(guard,&device->metadata);
    while (r->state == 2) {
        collect_used_locked(device);
        if (r->state != 2) break;
        if ((int64_t)(time_now() - r->deadline) >= 0) {
            /* Time reads and DMA progress are independent. Harvest again at
             * the deadline before revoking the caller's borrowed buffer. */
            collect_used_locked(device);
            if (r->state == 2) {
                device->statistics.timeouts++;
                fail_device(device, KERNEL_BLOCK_STATUS_TIMEOUT);
            }
            break;
        }
        if (device->irq_source) wait_for_progress(device, &r->done, r->deadline, r, 1, NULL,&guard);
    }
    enum kernel_block_status result = finish_request(device, r);
    return result;
}
static enum kernel_block_status submit_request(struct virtio_block_device *device,
    struct block_request *r, uint32_t type, uint64_t sector,
    uint64_t data_address, uint32_t data_length, int bounce)
{
    COST_SCOPE(device_cost, OPERATION_TICKS);
    enum kernel_block_status result;
    {
        KERNEL_IRQ_SCOPE(irq);
        KERNEL_RAW_SCOPE(guard,&device->metadata);
        while ((result = publish_request_locked(device, r, type, sector,
                data_address, data_length, bounce)) == KERNEL_BLOCK_STATUS_NOT_SUBMITTED) {
            if (device->irq_source) wait_for_progress(device, &device->descriptors, 0, NULL, 0, NULL,&guard);
            else collect_used_locked(device);
        }
    }
    if (result == KERNEL_BLOCK_STATUS_OK) result = wait_request(device, r);
    return result;
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
            const size_t maximum_direct_size = device->max_segment_size;

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
            const size_t maximum_direct_size = device->max_segment_size;

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

static enum kernel_block_status publish_batch_part(struct virtio_block_device *device,
    struct batch_transfer *parts, size_t count, size_t index,
    const struct kernel_block_span *writes, int writing, int phase, unsigned free,
    struct block_segment *segments)
{
    struct batch_transfer *part = &parts[index];
    struct block_request *r = part->request;
    if (free < 3) return KERNEL_BLOCK_STATUS_NOT_SUBMITTED;
    unsigned segments_count = 0;
    uint32_t members = 0, total = 0;
    const uint32_t maximum = VIRTIO_BLOCK_TRANSFER_BYTES;
    size_t next = index;
    uint64_t offset = part->offset;
    if (!part->modify_ready) {
        while (segments_count < device->max_segments && segments_count + 2 < free) {
            struct batch_transfer *candidate = &parts[next];
            if ((candidate->offset + candidate->copied) % VIRTIO_BLOCK_SECTOR_SIZE ||
                candidate->remaining - candidate->copied < VIRTIO_BLOCK_SECTOR_SIZE) break;
            size_t length = candidate->remaining - candidate->copied;
            if (length > device->max_segment_size) length = device->max_segment_size;
            if (length > maximum - total) length = maximum - total;
            length &= ~(size_t)(VIRTIO_BLOCK_SECTOR_SIZE - 1U);
            uint64_t address;
            if (!length || !device->dma_address(candidate->buffer + candidate->copied, length, &address)) break;
            segments[segments_count++] = (struct block_segment){address, (uint32_t)length};
            candidate->copied += length;
            members |= UINT32_C(1) << next;
            total += length; offset += length;
            if (candidate->copied < candidate->remaining) continue;
            if (phase) {
                next = candidate->next_ready;
                if (next == count || parts[next].offset != offset) break;
                continue;
            }
            for (next = 0; next < count; next++) {
                if (!parts[next].remaining || parts[next].copied || parts[next].request ||
                    parts[next].offset != offset) continue;
                if (writes && earlier_sector_pending(writes, parts, next)) continue;
                break;
            }
            if (next == count) break;
        }
    }
    uint32_t type = writing ? VIRTIO_BLOCK_REQUEST_OUT : VIRTIO_BLOCK_REQUEST_IN;
    int bounce = !segments_count;
    if (bounce) {
        unsigned skip = part->offset % VIRTIO_BLOCK_SECTOR_SIZE;
        part->copied = VIRTIO_BLOCK_SECTOR_SIZE - skip;
        if (part->copied > part->remaining) part->copied = part->remaining;
        members = UINT32_C(1) << index;
        if (writing) {
            if (!part->modify_ready && (skip || part->copied < VIRTIO_BLOCK_SECTOR_SIZE)) type = VIRTIO_BLOCK_REQUEST_IN;
            else bytes_copy(r->bounce + skip, part->buffer, part->copied);
        }
        segments[segments_count++] = (struct block_segment){device->queue_physical_address + bounce_offset(device, r), VIRTIO_BLOCK_SECTOR_SIZE};
    }
    part->members = members;
    return publish_segments(device, r, type, part->offset / VIRTIO_BLOCK_SECTOR_SIZE,
        segments, segments_count, bounce);
}

static void phase_ready_insert(struct batch_transfer *parts, uint8_t *head, unsigned index)
{
    while (*head != KERNEL_BLOCK_BATCH_MAX && parts[*head].offset < parts[index].offset)
        head = &parts[*head].next_ready;
    parts[index].next_ready = *head;
    *head = (uint8_t)index;
}

static enum kernel_block_status transfer_batch_parts(struct virtio_block_device *device,
    const struct kernel_block_span *writes, struct block_read_span *reads, size_t count,
    struct batch_transfer *parts, struct block_segment *segments,
    kernel_block_phase_next_fn next, const void *next_context, const void *cursor,
    const struct block_read_completion *completion)
{
    struct kernel_wait_queue done;
    kernel_wait_queue_init(&done);
    int writing = writes || next;
    COST_SCOPE(device_cost, OPERATION_TICKS);
    if (!begin_call(device, 0)) {
        if (reads) for (size_t i = 0; i < count; i++) if (reads[i].size) reads[i].status = KERNEL_BLOCK_STATUS_IO;
        return KERNEL_BLOCK_STATUS_IO;
    }
    struct kernel_io_context *owner = kernel_io_context_current();
    struct kernel_block_span phase_span = {0};
    uint8_t ready_head = KERNEL_BLOCK_BATCH_MAX;
    uint32_t ready_mask = 0;
    bytes_zero(parts, count * sizeof(*parts));
    for (size_t i = 0; !next && i < count; i++) {
        parts[i].offset = writes ? writes[i].offset : reads[i].offset;
        parts[i].buffer = writes ? (unsigned char *)writes[i].buffer : reads[i].buffer;
        parts[i].remaining = writes ? writes[i].size : reads[i].size;
    }
    enum kernel_block_status result = KERNEL_BLOCK_STATUS_OK;
    uint32_t completed_reads = 0;
    for (;;) {
        uintptr_t irq = arch_interrupt_save();
        KERNEL_RAW_SCOPE(guard,&device->metadata);
        if (owner != kernel_io_context_current()) __builtin_trap();
        if (device_live(device)) collect_used_locked(device);
        unsigned released = 0;
        /* 一个SG请求的成员共用设备状态；消费一次完成后再推进各span。
         * 写错误停止后续发布，已经借给设备的其他链仍须排空。 */
        for (size_t i = 0; i < count; i++) {
            struct batch_transfer *part = &parts[i];
            struct block_request *r = part->request;
            if (!r || r->state != 3) continue;
            enum kernel_block_status status = finish_request(device, r);
            if (writing && result == KERNEL_BLOCK_STATUS_OK && status != KERNEL_BLOCK_STATUS_OK) result = status;
            if (writing && status == KERNEL_BLOCK_STATUS_OK && r->header.type == VIRTIO_BLOCK_REQUEST_IN) {
                part->modify_ready = 1;
                continue;
            }
            uint32_t members = part->members;
            while (members) {
                unsigned j = first_descriptor(members);
                members &= members - 1;
                struct batch_transfer *member = &parts[j];
                if (status == KERNEL_BLOCK_STATUS_OK) {
                    if (reads && r->bounce_used)
                        bytes_copy(member->buffer, r->bounce + member->offset % VIRTIO_BLOCK_SECTOR_SIZE, member->copied);
                    member->offset += member->copied; member->buffer += member->copied;
                    member->remaining -= member->copied;
                    if (reads) {
                        reads[j].completed += member->copied;
                        if (!member->remaining) {
                            reads[j].status = KERNEL_BLOCK_STATUS_OK;
                            completed_reads |= UINT32_C(1) << j;
                        }
                    }
                } else if (reads) {
                    reads[j].status = status; member->remaining = 0;
                    completed_reads |= UINT32_C(1) << j;
                }
                member->copied = 0;
            }
            part->members = 0; part->modify_ready = 0;
            release_request_owner(device, r); part->request = NULL;
            released++;
        }
        if (released) wake(&device->slots);
        released = 0;
        if (writing && !device_live(device) && result == KERNEL_BLOCK_STATUS_OK) result = KERNEL_BLOCK_STATUS_IO;
        /* Refill each free owner slot immediately. A slow DMA in another
         * slot never prevents admission beyond the previous 32-span window. */
        if (next && result == KERNEL_BLOCK_STATUS_OK) {
            for (size_t i = 0; i < count; i++) {
                if (parts[i].remaining || parts[i].copied || parts[i].request) continue;
                while (!phase_span.size && next(next_context, &cursor, &phase_span)) {}
                if (!phase_span.size) break;
                size_t length = phase_span.size < VIRTIO_BLOCK_TRANSFER_BYTES ?
                    phase_span.size : VIRTIO_BLOCK_TRANSFER_BYTES;
                parts[i].offset = phase_span.offset;
                parts[i].buffer = (unsigned char *)phase_span.buffer;
                parts[i].remaining = length;
                phase_span.offset += length;
                phase_span.buffer = (const unsigned char *)phase_span.buffer + length;
                phase_span.size -= length;
            }
            for (unsigned i = 0; i < count; i++)
                if (parts[i].remaining && !parts[i].copied && !parts[i].request &&
                    !(ready_mask & (UINT32_C(1) << i))) {
                    phase_ready_insert(parts, &ready_head, i);
                    ready_mask |= UINT32_C(1) << i;
                }
        }
        unsigned published = 0;
        for (size_t i = next ? ready_head : 0; i < count; i = next ? ready_head : i + 1) {
            struct batch_transfer *part = &parts[i];
            if (next && result != KERNEL_BLOCK_STATUS_OK) break;
            if ((writing && result != KERNEL_BLOCK_STATUS_OK) || !part->remaining ||
                (part->copied && !part->modify_ready)) continue;
            if (writes && earlier_sector_pending(writes, parts, i)) continue;
            if (!device_live(device)) {
                if (reads) { reads[i].status = KERNEL_BLOCK_STATUS_IO; part->remaining = 0; }
                continue;
            }
            unsigned free = free_descriptors(device);
            if (free < 3) { if (next) break; else continue; }
            if (!part->request) {
                part->request = try_reserve_request(device);
                if (!part->request) { if (next) break; else continue; }
                part->request->completion = &done;
            }
            if (part->request->state == 1) {
                enum kernel_block_status status = publish_batch_part(device, parts, count, i, writes, writing,
                    next != NULL, free, segments);
                if (status == KERNEL_BLOCK_STATUS_OK) {
                    published++;
                    if (next) {
                        uint32_t members = part->members;
                        while (ready_head < count && (members & (UINT32_C(1) << ready_head)))
                            ready_head = parts[ready_head].next_ready;
                        ready_mask &= ~members;
                    }
                }
                else if (status != KERNEL_BLOCK_STATUS_NOT_SUBMITTED) __builtin_trap();
                else if (next) break;
            }
        }
        if (published && virtio_split_notify(&device->queue, &device->transport, 0) != VIRTIO_OK) __builtin_trap();
        if (writing && result != KERNEL_BLOCK_STATUS_OK) {
            for (size_t i = 0; i < count; i++) {
                if (parts[i].request && parts[i].request->state == 1) {
                    release_request_owner(device, parts[i].request); parts[i].request = NULL;
                    released++;
                }
            }
            if (released) wake(&device->slots);
        }
        /* 通知可以同步触发设备完成；先收割并消费它，不能为已完成DMA登记睡眠。 */
        if (device_live(device) && device->queue.used->index != device->queue.last_used) {
            collect_used_locked(device); kernel_raw_lock_release(&guard); arch_interrupt_restore(irq); continue;
        }
        unsigned pending = 0, remaining = 0, needs_capacity = 0;
        uint64_t deadline = 0;
        for (size_t i = 0; i < count; i++) {
            struct block_request *r = parts[i].request;
            remaining += parts[i].remaining != 0;
            if (parts[i].remaining && (!parts[i].copied || parts[i].modify_ready) &&
                (!r || r->state == 1)) needs_capacity = 1;
            if (!r || r->state != 2) continue;
            pending++;
            if (!deadline || (int64_t)(r->deadline - deadline) < 0) deadline = r->deadline;
        }
        if (!pending && (writing ? result != KERNEL_BLOCK_STATUS_OK || !remaining : !remaining)) {
            kernel_raw_lock_release(&guard); arch_interrupt_restore(irq); break;
        }
        if (deadline && (int64_t)(time_now() - deadline) >= 0) {
            if (device_live(device)) collect_used_locked(device);
            for (size_t i = 0; i < count; i++) {
                struct block_request *r = parts[i].request;
                if (r && r->state == 2 && (int64_t)(time_now() - r->deadline) >= 0) {
                    device->statistics.timeouts++; fail_device(device, KERNEL_BLOCK_STATUS_TIMEOUT); break;
                }
            }
            kernel_raw_lock_release(&guard); arch_interrupt_restore(irq); continue;
        }
        /* 整批已排空就通过返回值交付；只在还要等待时跨层发布已完成项。 */
        if (completion && completed_reads) {
            uint32_t ready = completed_reads;
            completed_reads = 0;
            kernel_raw_lock_release(&guard);
            arch_interrupt_restore(irq);
            while (ready) {
                unsigned i = first_descriptor(ready);
                ready &= ready - 1;
                completion->complete(completion->context, i, &reads[i]);
            }
            continue;
        }
        if (device->irq_source) {
            struct kernel_wait_queue *capacity = free_descriptors(device) < 3 ?
                &device->descriptors : &device->slots;
            wait_for_progress(device, pending ? &done : capacity, deadline, NULL,
                pending != 0, pending && needs_capacity ? capacity : NULL,&guard);
        }
        kernel_raw_lock_release(&guard);
        arch_interrupt_restore(irq);
    }
    for (size_t i = 0; i < count; i++) {
        if (parts[i].request) __builtin_trap();
        if (reads && result == KERNEL_BLOCK_STATUS_OK && reads[i].status != KERNEL_BLOCK_STATUS_OK) result = reads[i].status;
    }
    end_call(device, 0);
    return result;
}


static enum kernel_block_status transfer_batch(struct virtio_block_device *device,
    const struct kernel_block_span *writes, struct block_read_span *reads, size_t count,
    kernel_block_phase_next_fn next, const void *next_context, const void *cursor,
    const struct block_read_completion *completion)
{
    uintptr_t irq = arch_interrupt_save();
    KERNEL_RAW_SCOPE(guard,&device->metadata);
    unsigned slot = REQUEST_SLOTS;
    while (device_live(device)) {
        for (slot = 0; slot < slot_count(device); slot++)
            if (!(device->batch_claimed & (1U << slot))) break;
        if (slot < slot_count(device)) { device->batch_claimed |= 1U << slot; break; }
        if (!device->irq_source) __builtin_trap();
        wait_for_progress(device, &device->workspaces, 0, NULL, 0, NULL,&guard);
    }
    if (!device_live(device)) {
        kernel_raw_lock_release(&guard); arch_interrupt_restore(irq);
        if (reads) for (size_t i = 0; i < count; i++) if (reads[i].size) reads[i].status = KERNEL_BLOCK_STATUS_IO;
        return KERNEL_BLOCK_STATUS_IO;
    }
    kernel_raw_lock_release(&guard); arch_interrupt_restore(irq);
    /* 初始化时保留元数据页，最后一个可回收脏页的写回也不依赖再次分配。 */
    unsigned per_page = BOAROS_PAGE_SIZE / VIRTIO_BLOCK_BATCH_WORKSPACE_BYTES;
    struct block_batch_workspace *workspace = (void *)((unsigned char *)device->batch_memory[slot / per_page] +
        (slot % per_page) * VIRTIO_BLOCK_BATCH_WORKSPACE_BYTES);
    enum kernel_block_status status = transfer_batch_parts(device, writes, reads, count,
        workspace->parts, workspace->segments, next, next_context, cursor, completion);
    /* 全部本次 DMA 完成或 reset 确认后，才归还槽位供另一调用使用。 */
    irq = arch_interrupt_save();
    kernel_raw_lock_acquire(&device->metadata,&guard);
    if (!(device->batch_claimed & (1U << slot))) __builtin_trap();
    device->batch_claimed &= ~(1U << slot);
    wake(&device->workspaces);
    kernel_raw_lock_release(&guard); arch_interrupt_restore(irq);
    return status;
}

static enum kernel_block_status virtio_block_write_batch(void *context,
    const struct kernel_block_span *spans, size_t count)
{ return transfer_batch(context, spans, NULL, count, NULL, NULL, NULL, NULL); }

static enum kernel_block_status virtio_block_read_batch(void *context,
    struct block_read_span *spans, size_t count, const struct block_read_completion *completion)
{ return transfer_batch(context, NULL, spans, count, NULL, NULL, NULL, completion); }

static enum kernel_block_status virtio_block_write_phase(void *context,
    kernel_block_phase_next_fn next, const void *next_context, const void *first)
{ return transfer_batch(context, NULL, NULL, KERNEL_BLOCK_BATCH_MAX, next, next_context, first, NULL); }

enum virtio_block_status virtio_block_init(
    struct virtio_block_device *device,const struct virtio_transport *transport,
    struct physical_page_allocator *page_allocator,virtio_dma_address_fn dma_address,
    uint32_t timebase_frequency)
{
    struct virtio_block_device result={0};
    enum virtio_block_status status;
    enum physical_page_status page_status;
    uint64_t capacity_sectors;
    uint32_t queue_max;
    void *queue_memory;
    if (!device || device->state || !transport || !transport->context ||
        !transport->ops.read || !transport->ops.write || !transport->ops.ack_interrupt ||
        !page_allocator || !dma_address || !timebase_frequency)
        return VIRTIO_BLOCK_DRIVER_STATUS_INVALID;
    if (transport->version!=1 && transport->version!=2) return VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED;
    kernel_raw_lock_init(&result.metadata,KERNEL_RAW_RANK_OBJECT);
    result.page_allocator=page_allocator;result.dma_address=dma_address;result.transport=*transport;
    result.transport_version=transport->version;
    result.queue_allocation_order=legacy_transport(&result) ? VIRTIO_LEGACY_QUEUE_ALLOCATION_ORDER : 1U;
    result.timeout_ticks=(uint64_t)timebase_frequency*30;
#if BOAROS_COST_DIAGNOSTICS
    result.statistics_start=time_now();result.statistics_last=result.statistics_start;
#endif
    status = negotiate_features(&result);
    if (status != VIRTIO_BLOCK_DRIVER_STATUS_OK) {
        return init_failure(&result, status, 1, 0);
    }

    reg_write(&result, VIRTIO_REG_QUEUE_SEL, 0U);
    queue_max = reg_read(&result, VIRTIO_REG_QUEUE_NUM_MAX);
    if (queue_max < 4 ||
        (!legacy_transport(&result) &&
         reg_read(&result, VIRTIO_REG_QUEUE_READY) != 0U)) {
        return init_failure(&result,
                            VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED,
                            1,
                            0);
    }
    result.queue_size = VIRTIO_QUEUE_SIZE;
    while (result.queue_size > queue_max) result.queue_size /= 2;
    if (!read_segment_limits(&result))
        return init_failure(&result, VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED, 1, 0);

    page_status = physical_page_allocate_order(page_allocator,
                                                result.queue_allocation_order,
                                                &result.queue_physical_address);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return init_failure(&result,
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
                            VIRTIO_BLOCK_DRIVER_STATUS_STATE,
                            1,
                            1);
    }
    result.queue_memory = queue_memory;
    bytes_zero(queue_memory,
               (size_t)BOAROS_PAGE_SIZE << result.queue_allocation_order);

    if (virtio_split_initialize(&result.queue, queue_memory,
        (size_t)BOAROS_PAGE_SIZE << result.queue_allocation_order, result.queue_size,
        queue_avail_offset(&result), queue_used_offset(&result)) != VIRTIO_OK) __builtin_trap();
    if (virtio_transport_queue(&result.transport, 0, result.queue_size,
        result.queue_physical_address, queue_avail_offset(&result), queue_used_offset(&result),
        BOAROS_PAGE_SIZE) != VIRTIO_OK)
        return init_failure(&result, VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED, 1, 1);

    unsigned pool_pages = (slot_count(&result) * VIRTIO_BLOCK_BATCH_WORKSPACE_BYTES + BOAROS_PAGE_SIZE - 1U) / BOAROS_PAGE_SIZE;
    for (unsigned i = 0; i < pool_pages; i++) {
        page_status = physical_page_allocate_order(page_allocator, 0, &result.batch_physical[i]);
        if (page_status != PHYSICAL_PAGE_STATUS_OK)
            return init_failure(&result, page_status == PHYSICAL_PAGE_STATUS_EMPTY ?
                VIRTIO_BLOCK_DRIVER_STATUS_NO_MEMORY : VIRTIO_BLOCK_DRIVER_STATUS_STATE, 1, 1);
        if (physical_page_resolve(page_allocator, result.batch_physical[i], &result.batch_memory[i]) != PHYSICAL_PAGE_STATUS_OK)
            return init_failure(&result, VIRTIO_BLOCK_DRIVER_STATUS_STATE, 1, 1);
    }

    capacity_sectors = read_capacity(&result);
    if (capacity_sectors == 0U ||
        capacity_sectors > UINT64_MAX / VIRTIO_BLOCK_SECTOR_SIZE) {
        return init_failure(&result,
                            VIRTIO_BLOCK_DRIVER_STATUS_DEVICE,
                            1,
                            1);
    }

    if (virtio_transport_start(&result.transport) != VIRTIO_OK)
        return init_failure(&result, VIRTIO_BLOCK_DRIVER_STATUS_DEVICE, 1, 1);

    result.block.context = device;
    result.block.read = virtio_block_read;
    result.block.read_batch = virtio_block_read_batch;
    result.block.write = result.read_only != 0U ? 0 : virtio_block_write;
    result.block.write_batch = result.read_only != 0U ? 0 : virtio_block_write_batch;
    result.block.write_phase = result.read_only != 0U ? 0 : virtio_block_write_phase;
    result.block.flush = virtio_block_flush;
    result.block.capacity_bytes =
        capacity_sectors * VIRTIO_BLOCK_SECTOR_SIZE;
    result.block.logical_block_size = VIRTIO_BLOCK_SECTOR_SIZE;
    kernel_wait_queue_init(&result.admission);
    kernel_wait_queue_init(&result.idle);
    kernel_wait_queue_init(&result.slots);
    kernel_wait_queue_init(&result.descriptors);
    kernel_wait_queue_init(&result.workspaces);
    for (unsigned i = 0; i < slot_count(&result); i++) kernel_wait_queue_init(&request_at(&result, i)->done);
    result.state = VIRTIO_BLOCK_STATE_LIVE;
    *device = result;
    device->block.context = device;
    return VIRTIO_BLOCK_DRIVER_STATUS_OK;
}

enum virtio_block_status virtio_block_destroy(
    struct virtio_block_device *device)
{
    kernel_assert_service_context();
    KERNEL_IRQ_SCOPE(irq);
    enum physical_page_status page_status;

    if (!device) return VIRTIO_BLOCK_DRIVER_STATUS_STATE;
    uint32_t state=__atomic_load_n(&device->state,__ATOMIC_RELAXED);
    if (state!=VIRTIO_BLOCK_STATE_LIVE && state!=VIRTIO_BLOCK_STATE_FAILED)
        return VIRTIO_BLOCK_DRIVER_STATUS_STATE;
    {
        KERNEL_RAW_SCOPE(guard,&device->metadata);
        if (device->state != VIRTIO_BLOCK_STATE_LIVE && device->state != VIRTIO_BLOCK_STATE_FAILED)
            return VIRTIO_BLOCK_DRIVER_STATUS_STATE;
        if (device->active || device->admission.head || device->idle.head || device->slots.head ||
            device->descriptors.head || device->workspaces.head || device->batch_claimed)
            return VIRTIO_BLOCK_DRIVER_STATUS_STATE;
        /* 停止新调用后才复位和释放；配置/IRQ注销仍在CPU0完成。 */
        __atomic_store_n(&device->state,VIRTIO_BLOCK_STATE_DESTROYED,__ATOMIC_RELAXED);
    }
    /* 普通计数保持可见；详细时间只由成本诊断构建累计和打印。 */
    struct virtio_block_statistics statistics;
    virtio_block_get_statistics(device, &statistics);
    block_puts("BoarOS: block final device=");
    block_hex((unsigned long)device->block.device_number);
    block_puts(" requests="); block_hex(statistics.requests);
    block_puts(" max-inflight="); block_hex(statistics.max_inflight);
#if BOAROS_COST_DIAGNOSTICS
    block_puts(" inflight-ticks="); block_hex(statistics.inflight_ticks);
    block_puts(" busy-ticks="); block_hex(statistics.busy_ticks);
    block_puts(" total-ticks="); block_hex(statistics.total_ticks);
    block_puts(" wait-ticks="); block_hex(statistics.queue_wait_ticks);
    block_puts(" service-ticks="); block_hex(statistics.service_ticks);
#endif
    block_puts(" timeouts="); block_hex(statistics.timeouts);
    block_puts(" errors="); block_hex(statistics.io_errors);
    block_puts(" flushes="); block_hex(statistics.flush_requests);
    kernel_console_putc('\n');
    {
        KERNEL_RAW_SCOPE(guard,&device->metadata);
        device_reset(device);
    }
    if (device->irq_source) { device->transport.ops.unregister_irq(device->transport.context,device->irq_source,device); device->irq_source = 0; }
    release_batch_pool(device);
    page_status = physical_page_release_order(device->page_allocator,
                                              device->queue_physical_address,
                                              device->queue_allocation_order);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();

    KERNEL_RAW_SCOPE(guard,&device->metadata);
    device->block.context = 0;
    device->block.read = 0;
    device->block.read_batch = 0;
    device->block.write = 0;
    device->block.write_batch = 0;
    device->block.write_phase = 0;
    device->block.flush = 0;
    device->block.cache_mode = KERNEL_BLOCK_CACHE_UNKNOWN;
    device->block.capacity_bytes = 0U;
    device->block.logical_block_size = 0U;
    device->queue_memory = 0;
    device->queue_physical_address = 0;
    device->queue = (struct virtio_split_queue){0};
    return VIRTIO_BLOCK_DRIVER_STATUS_OK;
}

void virtio_block_get_statistics(
    const struct virtio_block_device *device,
    struct virtio_block_statistics *statistics)
{
    if (!device || !statistics) return;
    uint32_t state=__atomic_load_n(&device->state,__ATOMIC_RELAXED);
    if (state != VIRTIO_BLOCK_STATE_LIVE && state != VIRTIO_BLOCK_STATE_FAILED &&
        state != VIRTIO_BLOCK_STATE_DESTROYED) return;

    KERNEL_RAW_SCOPE(guard,&((struct virtio_block_device *)device)->metadata);
    *statistics = device->statistics;
#if BOAROS_COST_DIAGNOSTICS
    uint64_t now = time_now();
    uint64_t elapsed = now - device->statistics_last;
    statistics->inflight_ticks += (uint64_t)device->queue.inflight * elapsed;
    if (device->queue.inflight) statistics->busy_ticks += elapsed;
    statistics->total_ticks = now - device->statistics_start;
#endif
}
