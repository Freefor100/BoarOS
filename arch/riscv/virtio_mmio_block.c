#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/page.h>

#include <stddef.h>
#include <stdint.h>

#define VIRTIO_MMIO_MAGIC_VALUE UINT32_C(0x74726976)
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

#define VIRTIO_BLOCK_REQUEST_IN 0U
#define VIRTIO_BLOCK_STATUS_OK 0U
#define VIRTIO_BLOCK_STATUS_IO_ERROR 1U
#define VIRTIO_BLOCK_STATUS_UNSUPPORTED 2U
#define VIRTIO_BLOCK_SECTOR_SIZE 512U

#define VIRTQ_DESC_NEXT UINT16_C(1)
#define VIRTQ_DESC_WRITE UINT16_C(2)
#define VIRTIO_QUEUE_SIZE 8U

#define VIRTIO_QUEUE_DESC_OFFSET 0U
#define VIRTIO_QUEUE_AVAIL_OFFSET 128U
#define VIRTIO_QUEUE_USED_OFFSET 160U
#define VIRTIO_REQUEST_HEADER_OFFSET 256U
#define VIRTIO_REQUEST_STATUS_OFFSET 272U
#define VIRTIO_BOUNCE_OFFSET 512U

#define RISCV_VIRTIO_BLOCK_STATE_EMPTY 0U
#define RISCV_VIRTIO_BLOCK_STATE_LIVE UINT32_C(0x56424c4b)
#define RISCV_VIRTIO_BLOCK_STATE_FAILED UINT32_C(0x56464149)
#define RISCV_VIRTIO_BLOCK_STATE_DESTROYED UINT32_C(0x56444541)

struct virtq_descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_available {
    uint16_t flags;
    uint16_t index;
    uint16_t ring[VIRTIO_QUEUE_SIZE];
    uint16_t used_event;
} __attribute__((packed));

struct virtq_used_element {
    uint32_t id;
    uint32_t length;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t index;
    struct virtq_used_element ring[VIRTIO_QUEUE_SIZE];
    uint16_t available_event;
} __attribute__((packed));

struct virtio_block_request_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

_Static_assert(sizeof(struct virtq_descriptor) == 16U,
               "VirtIO descriptor layout must match the specification");
_Static_assert(VIRTIO_BOUNCE_OFFSET + VIRTIO_BLOCK_SECTOR_SIZE <=
                   BOAROS_PAGE_SIZE,
               "VirtIO queue and bounce buffer must fit in one page");

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
                                        0U);

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
    uint32_t device_features_high;

    mmio_write32(device, VIRTIO_MMIO_STATUS_OFFSET, 0U);
    mmio_write32(device,
                 VIRTIO_MMIO_STATUS_OFFSET,
                 VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write32(device, VIRTIO_MMIO_STATUS_OFFSET, status);

    mmio_write32(device, VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET, 1U);
    device_features_high =
        mmio_read32(device, VIRTIO_MMIO_DEVICE_FEATURES_OFFSET);
    if ((device_features_high & VIRTIO_FEATURE_VERSION_1_HIGH_MASK) == 0U) {
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_UNSUPPORTED;
    }

    mmio_write32(device, VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET, 0U);
    mmio_write32(device, VIRTIO_MMIO_DRIVER_FEATURES_OFFSET, 0U);
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

static enum kernel_block_status fail_live_device(
    struct riscv_virtio_mmio_block *device,
    enum kernel_block_status status)
{
    device_reset(device);
    device->state = RISCV_VIRTIO_BLOCK_STATE_FAILED;
    return status;
}

static enum kernel_block_status submit_read(
    struct riscv_virtio_mmio_block *device,
    uint64_t sector,
    uint64_t data_address,
    uint32_t data_length,
    int bounce)
{
    struct virtq_descriptor *descriptors =
        (struct virtq_descriptor *)((unsigned char *)device->queue_memory +
                                    VIRTIO_QUEUE_DESC_OFFSET);
    volatile struct virtq_available *available =
        (volatile struct virtq_available *)(
            (unsigned char *)device->queue_memory +
            VIRTIO_QUEUE_AVAIL_OFFSET);
    volatile struct virtq_used *used =
        (volatile struct virtq_used *)(
            (unsigned char *)device->queue_memory +
            VIRTIO_QUEUE_USED_OFFSET);
    struct virtio_block_request_header *header =
        (struct virtio_block_request_header *)(
            (unsigned char *)device->queue_memory +
            VIRTIO_REQUEST_HEADER_OFFSET);
    volatile unsigned char *request_status =
        (volatile unsigned char *)device->queue_memory +
        VIRTIO_REQUEST_STATUS_OFFSET;
    uint16_t available_index;
    uint64_t start;
    struct virtq_used_element element;

    if (!device_live(device) || data_length == 0U ||
        data_length % VIRTIO_BLOCK_SECTOR_SIZE != 0U) {
        return KERNEL_BLOCK_STATUS_STATE;
    }

    header->type = VIRTIO_BLOCK_REQUEST_IN;
    header->reserved = 0U;
    header->sector = sector;
    *request_status = UINT8_MAX;

    descriptors[0].address = device->queue_physical_address +
                             VIRTIO_REQUEST_HEADER_OFFSET;
    descriptors[0].length = sizeof(*header);
    descriptors[0].flags = VIRTQ_DESC_NEXT;
    descriptors[0].next = 1U;
    descriptors[1].address = data_address;
    descriptors[1].length = data_length;
    descriptors[1].flags = VIRTQ_DESC_WRITE | VIRTQ_DESC_NEXT;
    descriptors[1].next = 2U;
    descriptors[2].address = device->queue_physical_address +
                             VIRTIO_REQUEST_STATUS_OFFSET;
    descriptors[2].length = 1U;
    descriptors[2].flags = VIRTQ_DESC_WRITE;
    descriptors[2].next = 0U;

    available_index = available->index;
    available->ring[available_index % device->queue_size] = 0U;
    memory_barrier();
    available->index = (uint16_t)(available_index + 1U);
    memory_barrier();
    mmio_write32(device, VIRTIO_MMIO_QUEUE_NOTIFY_OFFSET, 0U);

    start = time_now();
    while (used->index == device->last_used_index) {
        if (time_now() - start >= device->timeout_ticks) {
            device->statistics.timeouts++;
            return fail_live_device(device, KERNEL_BLOCK_STATUS_TIMEOUT);
        }
    }
    memory_barrier();

    element = used->ring[device->last_used_index % device->queue_size];
    device->last_used_index++;
    if (mmio_read32(device, VIRTIO_MMIO_INTERRUPT_STATUS_OFFSET) != 0U) {
        mmio_write32(device,
                     VIRTIO_MMIO_INTERRUPT_ACK_OFFSET,
                     mmio_read32(device,
                                 VIRTIO_MMIO_INTERRUPT_STATUS_OFFSET));
    }
    if (element.id != 0U || element.length < 1U) {
        device->statistics.io_errors++;
        return fail_live_device(device, KERNEL_BLOCK_STATUS_IO);
    }

    device->statistics.requests++;
    device->statistics.sectors_read +=
        data_length / VIRTIO_BLOCK_SECTOR_SIZE;
    if (bounce) {
        device->statistics.bounce_requests++;
    } else {
        device->statistics.direct_requests++;
    }

    if (*request_status == VIRTIO_BLOCK_STATUS_OK) {
        return KERNEL_BLOCK_STATUS_OK;
    }
    device->statistics.io_errors++;
    if (*request_status == VIRTIO_BLOCK_STATUS_UNSUPPORTED) {
        return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    }
    if (*request_status == VIRTIO_BLOCK_STATUS_IO_ERROR) {
        return KERNEL_BLOCK_STATUS_IO;
    }
    return fail_live_device(device, KERNEL_BLOCK_STATUS_IO);
}

static enum kernel_block_status virtio_block_read(void *context,
                                                  uint64_t offset,
                                                  void *buffer,
                                                  size_t size)
{
    struct riscv_virtio_mmio_block *device = context;
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
                enum kernel_block_status status = submit_read(
                    device,
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
                (unsigned char *)device->queue_memory + VIRTIO_BOUNCE_OFFSET;
            size_t copied = VIRTIO_BLOCK_SECTOR_SIZE - sector_offset;
            enum kernel_block_status status;

            if (copied > size) {
                copied = size;
            }
            status = submit_read(device,
                                 sector,
                                 device->queue_physical_address +
                                     VIRTIO_BOUNCE_OFFSET,
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
    result.timeout_ticks = timebase_frequency;

    if (mmio_read32(&result, VIRTIO_MMIO_MAGIC_VALUE_OFFSET) !=
        VIRTIO_MMIO_MAGIC_VALUE) {
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_INVALID;
    }
    if (mmio_read32(&result, VIRTIO_MMIO_DEVICE_ID_OFFSET) !=
        VIRTIO_DEVICE_ID_BLOCK) {
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK;
    }
    if (mmio_read32(&result, VIRTIO_MMIO_VERSION_OFFSET) !=
        VIRTIO_MMIO_VERSION_MODERN) {
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_UNSUPPORTED;
    }

    status = negotiate_features(&result);
    if (status != RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
        return init_failure(&result, device, status, 1, 0);
    }

    mmio_write32(&result, VIRTIO_MMIO_QUEUE_SEL_OFFSET, 0U);
    queue_max = mmio_read32(&result, VIRTIO_MMIO_QUEUE_NUM_MAX_OFFSET);
    if (queue_max < 4U ||
        mmio_read32(&result, VIRTIO_MMIO_QUEUE_READY_OFFSET) != 0U) {
        return init_failure(&result,
                            device,
                            RISCV_VIRTIO_MMIO_BLOCK_STATUS_UNSUPPORTED,
                            1,
                            0);
    }
    result.queue_size = queue_max >= VIRTIO_QUEUE_SIZE ?
                            VIRTIO_QUEUE_SIZE : 4U;

    page_status = physical_page_allocate_order(page_allocator,
                                                0U,
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
    bytes_zero(queue_memory, BOAROS_PAGE_SIZE);

    mmio_write32(&result,
                 VIRTIO_MMIO_QUEUE_NUM_OFFSET,
                 result.queue_size);
    write_queue_address(&result,
                        VIRTIO_MMIO_QUEUE_DESC_LOW_OFFSET,
                        VIRTIO_MMIO_QUEUE_DESC_HIGH_OFFSET,
                        result.queue_physical_address +
                            VIRTIO_QUEUE_DESC_OFFSET);
    write_queue_address(&result,
                        VIRTIO_MMIO_QUEUE_DRIVER_LOW_OFFSET,
                        VIRTIO_MMIO_QUEUE_DRIVER_HIGH_OFFSET,
                        result.queue_physical_address +
                            VIRTIO_QUEUE_AVAIL_OFFSET);
    write_queue_address(&result,
                        VIRTIO_MMIO_QUEUE_DEVICE_LOW_OFFSET,
                        VIRTIO_MMIO_QUEUE_DEVICE_HIGH_OFFSET,
                        result.queue_physical_address +
                            VIRTIO_QUEUE_USED_OFFSET);
    memory_barrier();
    mmio_write32(&result, VIRTIO_MMIO_QUEUE_READY_OFFSET, 1U);

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
                     VIRTIO_STATUS_FEATURES_OK |
                     VIRTIO_STATUS_DRIVER_OK);
    memory_barrier();

    result.block.context = device;
    result.block.read = virtio_block_read;
    result.block.capacity_bytes =
        capacity_sectors * VIRTIO_BLOCK_SECTOR_SIZE;
    result.block.logical_block_size = VIRTIO_BLOCK_SECTOR_SIZE;
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

    device_reset(device);
    page_status = physical_page_release_order(device->page_allocator,
                                              device->queue_physical_address,
                                              0U);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_STATE;
    }

    device->block.context = 0;
    device->block.read = 0;
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

    *statistics = device->statistics;
}
