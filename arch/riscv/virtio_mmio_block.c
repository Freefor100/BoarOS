#include <kernel/cost.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/page.h>
#include <kernel/sync.h>
#include <kernel/proc_task.h>
#include <arch/riscv/context.h>
#include <arch/riscv/plic.h>
#include <arch/riscv/virt_uart.h>

#include <stddef.h>
#include <stdint.h>

#define VIRTIO_MMIO_MAGIC_VALUE UINT32_C(0x74726976)
#define VIRTIO_MMIO_VERSION_LEGACY 1U
#define VIRTIO_MMIO_VERSION_MODERN 2U
#define VIRTIO_DEVICE_ID_BLOCK 2U

#define VIRTIO_MMIO_MAGIC_VALUE_OFFSET 0x000U
#define VIRTIO_MMIO_VERSION_OFFSET 0x004U
#define VIRTIO_MMIO_DEVICE_ID_OFFSET 0x008U
#define VIRTIO_MMIO_DEVICE_FEATURES_OFFSET 0x010U
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET 0x014U
#define VIRTIO_MMIO_DRIVER_FEATURES_OFFSET 0x020U
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET 0x024U
#define VIRTIO_MMIO_QUEUE_SEL_OFFSET 0x030U
#define VIRTIO_MMIO_QUEUE_NUM_MAX_OFFSET 0x034U
#define VIRTIO_MMIO_QUEUE_NUM_OFFSET 0x038U
#define VIRTIO_MMIO_GUEST_PAGE_SIZE_OFFSET 0x028U
#define VIRTIO_MMIO_QUEUE_ALIGN_OFFSET 0x03cU
#define VIRTIO_MMIO_QUEUE_PFN_OFFSET 0x040U
#define VIRTIO_MMIO_QUEUE_READY_OFFSET 0x044U
#define VIRTIO_MMIO_QUEUE_NOTIFY_OFFSET 0x050U
#define VIRTIO_MMIO_INTERRUPT_STATUS_OFFSET 0x060U
#define VIRTIO_MMIO_INTERRUPT_ACK_OFFSET 0x064U
#define VIRTIO_MMIO_STATUS_OFFSET 0x070U
#define VIRTIO_MMIO_QUEUE_DESC_LOW_OFFSET 0x080U
#define VIRTIO_MMIO_QUEUE_DESC_HIGH_OFFSET 0x084U
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW_OFFSET 0x090U
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH_OFFSET 0x094U
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW_OFFSET 0x0a0U
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH_OFFSET 0x0a4U
#define VIRTIO_MMIO_CONFIG_GENERATION_OFFSET 0x0fcU
#define VIRTIO_MMIO_CONFIG_OFFSET 0x100U
#define VIRTIO_MMIO_REQUIRED_SIZE 0x108U

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

#define RISCV_VIRTIO_BLOCK_STATE_EMPTY 0U
#define RISCV_VIRTIO_BLOCK_STATE_LIVE UINT32_C(0x56424c4b)
#define RISCV_VIRTIO_BLOCK_STATE_FAILED UINT32_C(0x56464149)
#define RISCV_VIRTIO_BLOCK_STATE_DESTROYED UINT32_C(0x56444541)

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
_Static_assert(VIRTIO_LEGACY_REQUEST_HEADER_OFFSET + REQUEST_SLOTS * REQUEST_STRIDE <= 16384, "legacy queue overflow");

static int legacy_transport(const struct riscv_virtio_mmio_block *device)
{
    return device->transport_version == VIRTIO_MMIO_VERSION_LEGACY;
}

static uint32_t queue_avail_offset(const struct riscv_virtio_mmio_block *device)
{ return device->queue_size * sizeof(struct virtq_descriptor); }
static uint32_t queue_used_offset(const struct riscv_virtio_mmio_block *device)
{
    return legacy_transport(device) ? VIRTIO_LEGACY_QUEUE_USED_OFFSET :
        (queue_avail_offset(device) + 6 + 2 * device->queue_size + 15) & ~15U;
}
static uint32_t request_header_offset(const struct riscv_virtio_mmio_block *device)
{ return legacy_transport(device) ? VIRTIO_LEGACY_REQUEST_HEADER_OFFSET : VIRTIO_REQUEST_HEADER_OFFSET; }
static unsigned slot_count(const struct riscv_virtio_mmio_block *device)
{ unsigned n = device->queue_size / 3; return n < REQUEST_SLOTS ? n : REQUEST_SLOTS; }
static struct block_request *request_at(struct riscv_virtio_mmio_block *device, unsigned index)
{ return (void *)((unsigned char *)device->queue_memory + request_header_offset(device) + index * REQUEST_STRIDE); }
static uint64_t request_physical(struct riscv_virtio_mmio_block *device, struct block_request *request)
{ return device->queue_physical_address + ((unsigned char *)request - (unsigned char *)device->queue_memory); }
static uint32_t bounce_offset(struct riscv_virtio_mmio_block *device, struct block_request *request)
{ return (unsigned char *)request->bounce - (unsigned char *)device->queue_memory; }

static int device_live(const struct riscv_virtio_mmio_block *device)
{
    return device != 0 && device->state == RISCV_VIRTIO_BLOCK_STATE_LIVE;
}

static uint32_t mmio_read32(
    const struct riscv_virtio_mmio_block *device,
    uint32_t offset)
{
    volatile const uint32_t *value =
        (volatile const uint32_t *)(const volatile void *)(device->mmio +
                                                           offset);

    return *value;
}

static void mmio_write32(struct riscv_virtio_mmio_block *device,
                         uint32_t offset,
                         uint32_t value)
{
    volatile uint32_t *target =
        (volatile uint32_t *)(volatile void *)(device->mmio + offset);

    *target = value;
}

static void memory_barrier(void)
{
    __asm__ volatile("fence rw, rw" ::: "memory");
}

static uint64_t time_now(void)
{
    uint64_t value;

    __asm__ volatile("csrr %0, time" : "=r"(value));
    return value;
}

/* 在途深度变化前收口一段区间：累计 Σ深度·Δt 与忙时。 */
static void account_inflight(struct riscv_virtio_mmio_block *device)
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

static void write_queue_address(struct riscv_virtio_mmio_block *device,
                                uint32_t low_offset,
                                uint32_t high_offset,
                                uint64_t address)
{
    mmio_write32(device, low_offset, (uint32_t)address);
    mmio_write32(device, high_offset, (uint32_t)(address >> 32U));
}

static void device_reset(struct riscv_virtio_mmio_block *device)
{
    mmio_write32(device, VIRTIO_MMIO_STATUS_OFFSET, 0U);
    memory_barrier();
    /* QEMU's MMIO reset is synchronous. Do not return a borrowed DMA buffer
     * or release a queue if the transport violates that reset contract. */
    if (mmio_read32(device, VIRTIO_MMIO_STATUS_OFFSET) != 0U) {
        __builtin_trap();
    }
}

static enum riscv_virtio_mmio_block_status init_failure(
    struct riscv_virtio_mmio_block *device,
    struct riscv_virtio_mmio_block *owner,
    enum riscv_virtio_mmio_block_status status,
    int reset,
    int release_queue)
{
    if (reset) {
        mmio_write32(device,
                     VIRTIO_MMIO_STATUS_OFFSET,
                     VIRTIO_STATUS_FAILED);
        device_reset(device);
    }
    if (release_queue) {
        enum physical_page_status page_status =
            physical_page_release_order(device->page_allocator,
                                        device->queue_physical_address,
                                        device->queue_allocation_order);

        if (page_status != PHYSICAL_PAGE_STATUS_OK) {
            device->state = RISCV_VIRTIO_BLOCK_STATE_FAILED;
            if (owner != 0) {
                *owner = *device;
            }
            return RISCV_VIRTIO_MMIO_BLOCK_STATUS_STATE;
        }
    }
    return status;
}

static enum riscv_virtio_mmio_block_status negotiate_features(
    struct riscv_virtio_mmio_block *device)
{
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    uint32_t device_features_low;
    uint32_t device_features_high;
    uint32_t driver_features_low = 0U;

    mmio_write32(device, VIRTIO_MMIO_STATUS_OFFSET, 0U);
    mmio_write32(device,
                 VIRTIO_MMIO_STATUS_OFFSET,
                 VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write32(device, VIRTIO_MMIO_STATUS_OFFSET, status);

    mmio_write32(device, VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET, 0U);
    device_features_low =
        mmio_read32(device, VIRTIO_MMIO_DEVICE_FEATURES_OFFSET);
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
        mmio_write32(device, VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET, 0U);
        mmio_write32(device, VIRTIO_MMIO_DRIVER_FEATURES_OFFSET, driver_features_low);
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK;
    }

    mmio_write32(device, VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET, 1U);
    device_features_high =
        mmio_read32(device, VIRTIO_MMIO_DEVICE_FEATURES_OFFSET);
    if ((device_features_high & VIRTIO_FEATURE_VERSION_1_HIGH_MASK) == 0U) {
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_UNSUPPORTED;
    }

    mmio_write32(device, VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET, 0U);
    mmio_write32(device, VIRTIO_MMIO_DRIVER_FEATURES_OFFSET, driver_features_low);
    mmio_write32(device, VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET, 1U);
    mmio_write32(device,
                 VIRTIO_MMIO_DRIVER_FEATURES_OFFSET,
                 VIRTIO_FEATURE_VERSION_1_HIGH_MASK);

    status |= VIRTIO_STATUS_FEATURES_OK;
    mmio_write32(device, VIRTIO_MMIO_STATUS_OFFSET, status);
    if ((mmio_read32(device, VIRTIO_MMIO_STATUS_OFFSET) &
         VIRTIO_STATUS_FEATURES_OK) == 0U) {
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_UNSUPPORTED;
    }

    return RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK;
}

static uint64_t read_capacity(struct riscv_virtio_mmio_block *device)
{
    uint32_t attempt;

    if (legacy_transport(device)) {
        uint32_t low = mmio_read32(device, VIRTIO_MMIO_CONFIG_OFFSET);
        uint32_t high = mmio_read32(device,
                                    VIRTIO_MMIO_CONFIG_OFFSET + 4U);

        return ((uint64_t)high << 32U) | low;
    }

    for (attempt = 0U; attempt < 8U; attempt++) {
        uint32_t generation_before =
            mmio_read32(device, VIRTIO_MMIO_CONFIG_GENERATION_OFFSET);
        uint32_t low = mmio_read32(device, VIRTIO_MMIO_CONFIG_OFFSET);
        uint32_t high = mmio_read32(device,
                                    VIRTIO_MMIO_CONFIG_OFFSET + 4U);
        uint32_t generation_after =
            mmio_read32(device, VIRTIO_MMIO_CONFIG_GENERATION_OFFSET);

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
static void diagnose_queue_fault(struct riscv_virtio_mmio_block *device,
    const char *reason, uint16_t used_index, uint16_t observed_count,
    uint32_t pending, const struct virtq_used_element *item);
static void fail_device(struct riscv_virtio_mmio_block *device, enum kernel_block_status result)
{
    device->state = RISCV_VIRTIO_BLOCK_STATE_FAILED;
    if (result == KERNEL_BLOCK_STATUS_TIMEOUT) {
        volatile struct virtq_used *used = (void *)((unsigned char *)device->queue_memory + queue_used_offset(device));
        uint16_t used_index = used->index;
        memory_barrier();
        /* 纯超时没有非法 used 项；reset 前仍需保留设备身份与在途 owner。 */
        diagnose_queue_fault(device, "timeout", used_index,
            (uint16_t)(used_index - device->last_used_index),
            mmio_read32(device, VIRTIO_MMIO_INTERRUPT_STATUS_OFFSET), NULL);
        virt_uart_puts("BoarOS: block timeout; resetting device\n");
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
    virt_uart_puts(name);
    virt_uart_put_hex((unsigned long)value);
}

static void diagnose_queue_fault(struct riscv_virtio_mmio_block *device,
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
    virt_uart_puts("BoarOS: block queue fault reason=");
    virt_uart_puts(reason);
    queue_fault_scalar(" mmio=", (uintptr_t)device->mmio);
    queue_fault_scalar(" mmio_status=", mmio_read32(device, VIRTIO_MMIO_STATUS_OFFSET));
    queue_fault_scalar(" interrupt=", pending);
    queue_fault_scalar(" transport=", device->transport_version);
    queue_fault_scalar(" queue=", (uintptr_t)device->queue_memory);
    queue_fault_scalar(" physical=", device->queue_physical_address);
    queue_fault_scalar(" size=", device->queue_size);
    virt_uart_puts("\n");
    queue_fault_scalar(" used=", used_index);
    queue_fault_scalar(" consumed=", device->last_used_index);
    queue_fault_scalar(" avail=", available->index);
    queue_fault_scalar(" observed_count=", observed_count);
    queue_fault_scalar(" inflight=", device->inflight);
    queue_fault_scalar(" active=", device->active);
    queue_fault_scalar(" reserved=", reserved);
    queue_fault_scalar(" published=", published);
    queue_fault_scalar(" complete=", complete);
    virt_uart_puts("\n");
    if (item != NULL) {
        queue_fault_scalar(" item_index=", (uint16_t)(device->last_used_index - 1));
        queue_fault_scalar(" id=", item->id);
        queue_fault_scalar(" length=", item->length);
        virt_uart_puts("\n");
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
        virt_uart_puts("\n");
    }
}

static void collect_used(struct riscv_virtio_mmio_block *device)
{
    uint32_t pending = mmio_read32(device, VIRTIO_MMIO_INTERRUPT_STATUS_OFFSET);
    if (pending) {
        /* Ack before the used-index snapshot: a later completion must leave
         * its interrupt pending instead of being erased after the scan. */
        mmio_write32(device, VIRTIO_MMIO_INTERRUPT_ACK_OFFSET, pending);
        __asm__ volatile("fence iorw, iorw" ::: "memory");
    }
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
    struct riscv_virtio_mmio_block *device = owner;
    device->statistics.interrupts++;
    if (device_live(device)) collect_used(device);
    else mmio_write32(device, VIRTIO_MMIO_INTERRUPT_ACK_OFFSET,
                      mmio_read32(device, VIRTIO_MMIO_INTERRUPT_STATUS_OFFSET));
}
int riscv_virtio_mmio_block_enable_irq(struct riscv_virtio_mmio_block *device, uint32_t source)
{
    if (!device_live(device) || device->active || device->irq_source ||
        !riscv_plic_register(source, block_irq, device)) return 0;
    device->irq_source = source;
    return 1;
}
static int begin_call(struct riscv_virtio_mmio_block *device, int barrier)
{
    uintptr_t irq = riscv_interrupt_save();
    if (device->irq_source && (!kernel_scheduler_can_sleep() || riscv_plic_in_interrupt())) __builtin_trap();
    if (barrier) device->barrier_waiters++;
    for (;;) {
        if (!device_live(device)) break;
        if (!device->barrier && (barrier ? !device->active : !device->barrier_waiters)) {
            if (device->active == UINT32_MAX) __builtin_trap();
            device->active++;
            if (barrier) { device->barrier_waiters--; device->barrier = 1; }
            riscv_interrupt_restore(irq); return 1;
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
    riscv_interrupt_restore(irq); return 0;
}
static void end_call(struct riscv_virtio_mmio_block *device, int barrier)
{
    uintptr_t irq = riscv_interrupt_save();
    if (!device->active || (barrier && !device->barrier)) __builtin_trap();
    device->active--;
    if (barrier) device->barrier = 0;
    wake(&device->available);
    riscv_interrupt_restore(irq);
}
/* Already admitted logical calls keep publishing even when FLUSH is waiting.
 * The call owns active until its unpublished spans and DMA are both drained. */
static struct block_request *try_reserve_request(struct riscv_virtio_mmio_block *device)
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
static struct block_request *reserve_request(struct riscv_virtio_mmio_block *device)
{
    uintptr_t irq = riscv_interrupt_save();
    struct block_request *r;
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
    riscv_interrupt_restore(irq); return r;
}
static void release_request(struct riscv_virtio_mmio_block *device, struct block_request *r)
{
    uintptr_t irq = riscv_interrupt_save();
    if (r->state != 1 || r->owner != kernel_io_context_current() || !device->active || r->done.head)
        __builtin_trap();
    r->state = 0; r->owner = 0; r->completion = 0;
    wake(&device->available);
    riscv_interrupt_restore(irq);
}
static enum kernel_block_status publish_request(struct riscv_virtio_mmio_block *device,
    struct block_request *r, uint32_t type, uint64_t sector,
    uint64_t data_address, uint32_t data_length, int bounce)
{
    uintptr_t irq = riscv_interrupt_save();
    if (!device_live(device)) { riscv_interrupt_restore(irq); return KERNEL_BLOCK_STATUS_IO; }
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
    mmio_write32(device, VIRTIO_MMIO_QUEUE_NOTIFY_OFFSET, 0);
    riscv_interrupt_restore(irq);
    return KERNEL_BLOCK_STATUS_OK;
}
static enum kernel_block_status finish_request(struct riscv_virtio_mmio_block *device,
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
static enum kernel_block_status wait_request(struct riscv_virtio_mmio_block *device,
                                             struct block_request *r)
{
    uintptr_t irq = riscv_interrupt_save();
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
    riscv_interrupt_restore(irq);
    return result;
}
static enum kernel_block_status submit_request(struct riscv_virtio_mmio_block *device,
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
    struct riscv_virtio_mmio_block *device = context;
    if (!device_live(device)) return KERNEL_BLOCK_STATUS_IO;
    if (!begin_call(device, 1)) return KERNEL_BLOCK_STATUS_IO;
    struct block_request *r = reserve_request(device);
    if (!r) { end_call(device, 1); return KERNEL_BLOCK_STATUS_IO; }
    enum kernel_block_status result = device->block.cache_mode == KERNEL_BLOCK_CACHE_WRITETHROUGH
        ? KERNEL_BLOCK_STATUS_OK : submit_request(device, r, VIRTIO_BLOCK_REQUEST_FLUSH, 0, 0, 0, 0);
    release_request(device, r); end_call(device, 1); return result;
}

static enum kernel_block_status transfer_read(struct riscv_virtio_mmio_block *device, struct block_request *r,
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

static enum kernel_block_status transfer_write(struct riscv_virtio_mmio_block *device, struct block_request *r,
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
    struct riscv_virtio_mmio_block *device = context;
    if (!begin_call(device, 0)) return KERNEL_BLOCK_STATUS_IO;
    struct block_request *r = reserve_request(device);
    if (!r) { end_call(device, 0); return KERNEL_BLOCK_STATUS_IO; }
    enum kernel_block_status result = transfer_read(device, r, offset, buffer, size);
    release_request(device, r); end_call(device, 0); return result;
}
static enum kernel_block_status virtio_block_write(void *context, uint64_t offset, const void *buffer, size_t size)
{
    struct riscv_virtio_mmio_block *device = context;
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

static enum kernel_block_status publish_write_part(struct riscv_virtio_mmio_block *device,
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
    struct riscv_virtio_mmio_block *device = context;
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
        uintptr_t irq = riscv_interrupt_save();
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
            riscv_interrupt_restore(irq); break;
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
            riscv_interrupt_restore(irq); continue;
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
        riscv_interrupt_restore(irq);
    }
    for (size_t i = 0; i < count; i++) if (parts[i].request) __builtin_trap();
    end_call(device, 0);
    return result;
}

enum riscv_virtio_mmio_block_status riscv_virtio_mmio_block_init(
    struct riscv_virtio_mmio_block *device,
    volatile void *mmio,
    uint64_t mmio_size,
    struct physical_page_allocator *page_allocator,
    riscv_virtio_dma_address_fn dma_address,
    uint32_t timebase_frequency)
{
    struct riscv_virtio_mmio_block result = {0};
    enum riscv_virtio_mmio_block_status status;
    enum physical_page_status page_status;
    uint64_t capacity_sectors;
    uint32_t queue_max;
    uint32_t version;
    void *queue_memory;

    if (device == 0 || device->state != RISCV_VIRTIO_BLOCK_STATE_EMPTY ||
        mmio == 0 || mmio_size < VIRTIO_MMIO_REQUIRED_SIZE ||
        page_allocator == 0 || dma_address == 0 ||
        timebase_frequency == 0U) {
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_INVALID;
    }

    result.page_allocator = page_allocator;
    result.dma_address = dma_address;
    result.mmio = mmio;
    result.mmio_size = mmio_size;
    /* 合法 FLUSH 可能等待宿主整份后备文件落盘；一秒不能判定设备故障。
     * 保留有限期限及原 reset/DMA owner 契约，乘法先提升避免频率溢出。 */
    result.timeout_ticks = (uint64_t)timebase_frequency * 30;
    result.statistics_start = time_now();
    result.statistics_last = result.statistics_start;

    if (mmio_read32(&result, VIRTIO_MMIO_MAGIC_VALUE_OFFSET) !=
        VIRTIO_MMIO_MAGIC_VALUE) {
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_INVALID;
    }
    if (mmio_read32(&result, VIRTIO_MMIO_DEVICE_ID_OFFSET) !=
        VIRTIO_DEVICE_ID_BLOCK) {
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK;
    }
    version = mmio_read32(&result, VIRTIO_MMIO_VERSION_OFFSET);
    if (version != VIRTIO_MMIO_VERSION_LEGACY &&
        version != VIRTIO_MMIO_VERSION_MODERN) {
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_UNSUPPORTED;
    }
    result.transport_version = version;
    result.queue_allocation_order =
        legacy_transport(&result) ? VIRTIO_LEGACY_QUEUE_ALLOCATION_ORDER
                                  : 1U;

    status = negotiate_features(&result);
    if (status != RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
        return init_failure(&result, device, status, 1, 0);
    }

    mmio_write32(&result, VIRTIO_MMIO_QUEUE_SEL_OFFSET, 0U);
    queue_max = mmio_read32(&result, VIRTIO_MMIO_QUEUE_NUM_MAX_OFFSET);
    if (queue_max < 4 ||
        (!legacy_transport(&result) &&
         mmio_read32(&result, VIRTIO_MMIO_QUEUE_READY_OFFSET) != 0U)) {
        return init_failure(&result,
                            device,
                            RISCV_VIRTIO_MMIO_BLOCK_STATUS_UNSUPPORTED,
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
                                RISCV_VIRTIO_MMIO_BLOCK_STATUS_NO_MEMORY :
                                RISCV_VIRTIO_MMIO_BLOCK_STATUS_STATE,
                            1,
                            0);
    }
    page_status = physical_page_resolve(page_allocator,
                                        result.queue_physical_address,
                                        &queue_memory);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return init_failure(&result,
                            device,
                            RISCV_VIRTIO_MMIO_BLOCK_STATUS_STATE,
                            1,
                            1);
    }
    result.queue_memory = queue_memory;
    bytes_zero(queue_memory,
               (size_t)BOAROS_PAGE_SIZE << result.queue_allocation_order);

    mmio_write32(&result,
                 VIRTIO_MMIO_QUEUE_NUM_OFFSET,
                 result.queue_size);
    if (legacy_transport(&result)) {
        if (result.queue_physical_address >> BOAROS_PAGE_SHIFT >
            UINT32_MAX) {
            return init_failure(&result,
                                device,
                                RISCV_VIRTIO_MMIO_BLOCK_STATUS_UNSUPPORTED,
                                1,
                                1);
        }
        mmio_write32(&result,
                     VIRTIO_MMIO_GUEST_PAGE_SIZE_OFFSET,
                     BOAROS_PAGE_SIZE);
        mmio_write32(&result,
                     VIRTIO_MMIO_QUEUE_ALIGN_OFFSET,
                     VIRTIO_LEGACY_QUEUE_ALIGNMENT);
        memory_barrier();
        mmio_write32(&result,
                     VIRTIO_MMIO_QUEUE_PFN_OFFSET,
                     (uint32_t)(result.queue_physical_address >>
                                BOAROS_PAGE_SHIFT));
    } else {
        write_queue_address(&result,
                            VIRTIO_MMIO_QUEUE_DESC_LOW_OFFSET,
                            VIRTIO_MMIO_QUEUE_DESC_HIGH_OFFSET,
                            result.queue_physical_address +
                                VIRTIO_QUEUE_DESC_OFFSET);
        write_queue_address(&result,
                            VIRTIO_MMIO_QUEUE_DRIVER_LOW_OFFSET,
                            VIRTIO_MMIO_QUEUE_DRIVER_HIGH_OFFSET,
                            result.queue_physical_address +
                                queue_avail_offset(&result));
        write_queue_address(&result,
                            VIRTIO_MMIO_QUEUE_DEVICE_LOW_OFFSET,
                            VIRTIO_MMIO_QUEUE_DEVICE_HIGH_OFFSET,
                            result.queue_physical_address +
                                queue_used_offset(&result));
        memory_barrier();
        mmio_write32(&result, VIRTIO_MMIO_QUEUE_READY_OFFSET, 1U);
    }

    capacity_sectors = read_capacity(&result);
    if (capacity_sectors == 0U ||
        capacity_sectors > UINT64_MAX / VIRTIO_BLOCK_SECTOR_SIZE) {
        return init_failure(&result,
                            device,
                            RISCV_VIRTIO_MMIO_BLOCK_STATUS_DEVICE,
                            1,
                            1);
    }

    mmio_write32(&result,
                 VIRTIO_MMIO_STATUS_OFFSET,
                 VIRTIO_STATUS_ACKNOWLEDGE |
                     VIRTIO_STATUS_DRIVER |
                     (legacy_transport(&result) ? 0U
                                                 : VIRTIO_STATUS_FEATURES_OK) |
                     VIRTIO_STATUS_DRIVER_OK);
    memory_barrier();

    result.block.context = device;
    result.block.read = virtio_block_read;
    result.block.write = result.read_only != 0U ? 0 : virtio_block_write;
    result.block.write_batch = result.read_only != 0U ? 0 : virtio_block_write_batch;
    result.block.flush = virtio_block_flush;
    result.block.capacity_bytes =
        capacity_sectors * VIRTIO_BLOCK_SECTOR_SIZE;
    result.block.logical_block_size = VIRTIO_BLOCK_SECTOR_SIZE;
    kernel_wait_queue_init(&result.available);
    for (unsigned i = 0; i < slot_count(&result); i++) kernel_wait_queue_init(&request_at(&result, i)->done);
    result.state = RISCV_VIRTIO_BLOCK_STATE_LIVE;
    *device = result;
    device->block.context = device;
    return RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK;
}

enum riscv_virtio_mmio_block_status riscv_virtio_mmio_block_destroy(
    struct riscv_virtio_mmio_block *device)
{
    enum physical_page_status page_status;

    if (device == 0 ||
        (device->state != RISCV_VIRTIO_BLOCK_STATE_LIVE &&
         device->state != RISCV_VIRTIO_BLOCK_STATE_FAILED)) {
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_STATE;
    }

    if (device->active || device->available.head) return RISCV_VIRTIO_MMIO_BLOCK_STATUS_STATE;
    /* 最终统计行：时间加权在途深度与等待/服务时间供真实窗口取证。 */
    struct riscv_virtio_mmio_block_statistics statistics;
    riscv_virtio_mmio_block_get_statistics(device, &statistics);
    virt_uart_puts("BoarOS: block final device=");
    virt_uart_put_hex((unsigned long)device->block.device_number);
    virt_uart_puts(" requests="); virt_uart_put_hex(statistics.requests);
    virt_uart_puts(" max-inflight="); virt_uart_put_hex(statistics.max_inflight);
    virt_uart_puts(" inflight-ticks="); virt_uart_put_hex(statistics.inflight_ticks);
    virt_uart_puts(" busy-ticks="); virt_uart_put_hex(statistics.busy_ticks);
    virt_uart_puts(" total-ticks="); virt_uart_put_hex(statistics.total_ticks);
    virt_uart_puts(" wait-ticks="); virt_uart_put_hex(statistics.queue_wait_ticks);
    virt_uart_puts(" service-ticks="); virt_uart_put_hex(statistics.service_ticks);
    virt_uart_puts(" timeouts="); virt_uart_put_hex(statistics.timeouts);
    virt_uart_puts(" errors="); virt_uart_put_hex(statistics.io_errors);
    virt_uart_puts(" flushes="); virt_uart_put_hex(statistics.flush_requests);
    virt_uart_putc('\n');
    device_reset(device);
    if (device->irq_source) { riscv_plic_unregister(device->irq_source, device); device->irq_source = 0; }
    page_status = physical_page_release_order(device->page_allocator,
                                              device->queue_physical_address,
                                              device->queue_allocation_order);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_STATE;
    }

    device->block.context = 0;
    device->block.read = 0;
    device->block.write = 0;
    device->block.write_batch = 0;
    device->block.flush = 0;
    device->block.cache_mode = KERNEL_BLOCK_CACHE_UNKNOWN;
    device->block.capacity_bytes = 0U;
    device->block.logical_block_size = 0U;
    device->queue_memory = 0;
    device->state = RISCV_VIRTIO_BLOCK_STATE_DESTROYED;
    return RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK;
}

void riscv_virtio_mmio_block_get_statistics(
    const struct riscv_virtio_mmio_block *device,
    struct riscv_virtio_mmio_block_statistics *statistics)
{
    if (device == 0 || statistics == 0 ||
        (device->state != RISCV_VIRTIO_BLOCK_STATE_LIVE &&
         device->state != RISCV_VIRTIO_BLOCK_STATE_FAILED &&
         device->state != RISCV_VIRTIO_BLOCK_STATE_DESTROYED)) {
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
