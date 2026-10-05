#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtio_mmio_block_native.c"

static uint32_t registers[128];
static void *queue_allocation;
static size_t allocation_size;
static unsigned allocations;
static unsigned releases;
static uint64_t clock_value;
static uint64_t complete_at;
static struct kernel_io_context caller;
static char diagnostic[8192];
static size_t diagnostic_length;
static enum fault_kind {
    NORMAL, DEVICE_IO, DEVICE_UNSUPPORTED, USED_OVERFLOW, INVALID_ID,
    MISALIGNED_ID, ZERO_LENGTH, FREE_SLOT, DUPLICATE_SLOT, INVALID_STATUS,
    NO_COMPLETION, MANUAL_COMPLETION, BATCH_READ
} fault;
static int injected, final_statistics;
static unsigned batch_mode, batch_popped, batch_pushed;
static unsigned char batch_output[8][4096];

/* Independent wire records, from the VirtIO split-ring ABI. */
struct wire_descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
} __attribute__((packed));
struct wire_completion { uint32_t id, length; };

static void *physical_pointer(uint64_t address, size_t size)
{
    if (address >= UINT64_C(0x82000000)) {
        uint64_t offset = address - UINT64_C(0x82000000);
        assert(offset <= sizeof(batch_output) && size <= sizeof(batch_output) - offset);
        return (unsigned char *)batch_output + offset;
    }
    assert(address >= UINT64_C(0x81000000));
    uint64_t offset = address - UINT64_C(0x81000000);
    assert(offset <= allocation_size && size <= allocation_size - offset);
    return (unsigned char *)queue_allocation + offset;
}

static uint64_t register_address(unsigned low)
{
    return registers[low / 4] | ((uint64_t)registers[(low + 4) / 4] << 32);
}

static void read_batch_device(void)
{
    if (!queue_allocation) return;
    unsigned queue_size = registers[0x38 / 4];
    uint64_t descriptors_at = registers[4 / 4] == 1 ? (uint64_t)registers[0x40 / 4] << 12 : register_address(0x80);
    if (!descriptors_at) return;
    uint64_t available_at = registers[4 / 4] == 1 ? descriptors_at + queue_size * 16 : register_address(0x90);
    uint64_t used_at = registers[4 / 4] == 1 ? descriptors_at + 4096 : register_address(0xa0);
    uint16_t *available = physical_pointer(available_at, 4 + queue_size * 2);
    unsigned count = (uint16_t)(available[1] - batch_popped);
    if (!count || (!batch_popped && count < 8)) return;
    assert(count <= 8);
    struct wire_descriptor *descriptors = physical_pointer(descriptors_at, queue_size * 16);
    uint16_t *used = physical_pointer(used_at, 4 + queue_size * 8);
    struct wire_completion *completions = (void *)(used + 2);
    unsigned first = batch_popped; batch_popped += count;
    for (unsigned n = count; n > 0; n--) {
        unsigned head = available[2 + (first + n - 1) % queue_size];
        const unsigned char *header = physical_pointer(descriptors[head].address, 16);
        uint32_t type; uint64_t sector;
        memcpy(&type, header, 4); memcpy(&sector, header + 8, 8);
        assert(type == 0);
        unsigned data = descriptors[head].next, tail = descriptors[data].next;
        assert(descriptors[data].flags == 3 && descriptors[tail].flags == 2);
        unsigned index = (unsigned)(sector / 8);
        assert(index < 8);
        /* 一个错误不能使仍被设备持有的另一span提前回收。 */
        if ((batch_mode == 2 || batch_mode == 3) && index == 7) continue;
        int error = ((batch_mode == 1 || batch_mode == 3) && index == 3) ||
            (batch_mode == 6 && index == 3 && sector % 8 == 1);
        if (!error) {
            unsigned char *out = physical_pointer(descriptors[data].address, descriptors[data].length);
            for (unsigned i = 0; i < descriptors[data].length; i++) out[i] = (unsigned char)((sector * 512 + i) * 7 + 3);
        }
        *(unsigned char *)physical_pointer(descriptors[tail].address, 1) = error ? 1 : 0;
        completions[batch_pushed++ % queue_size] = (struct wire_completion){head, error ? 1 : descriptors[data].length + 1};
    }
    used[1] = (uint16_t)batch_pushed;
    registers[0x60 / 4] = 1;
}

uint64_t host_block_time(void)
{
    clock_value += 1;
    if (fault == BATCH_READ) { read_batch_device(); return clock_value; }
    if (!queue_allocation || injected || fault >= NO_COMPLETION || clock_value < complete_at) return clock_value;
    unsigned queue_size = registers[0x38 / 4];
    uint64_t descriptor_address, available_address, used_address;
    if (registers[4 / 4] == 1) {
        descriptor_address = (uint64_t)registers[0x40 / 4] << 12;
        available_address = descriptor_address + queue_size * 16;
        used_address = descriptor_address + 4096;
    } else {
        descriptor_address = register_address(0x80);
        available_address = register_address(0x90);
        used_address = register_address(0xa0);
    }
    if (descriptor_address == 0) return clock_value;
    uint16_t *available = physical_pointer(available_address, 6);
    if (available[1] == 0) return clock_value;
    assert(available[1] == 1);
    struct wire_descriptor *descriptors = physical_pointer(descriptor_address,
                                                           queue_size * 16);
    unsigned head = available[2];
    unsigned status_descriptor = head;
    while (descriptors[status_descriptor].flags & 1)
        status_descriptor = descriptors[status_descriptor].next;
    unsigned char *status = physical_pointer(descriptors[status_descriptor].address, 1);
    *status = fault == DEVICE_IO ? 1 : fault == DEVICE_UNSUPPORTED ? 2 :
        fault == INVALID_STATUS ? 7 : 0;
    uint16_t *used = physical_pointer(used_address, 4 + queue_size * 8);
    struct wire_completion *completion = (void *)(used + 2);
    completion[0] = (struct wire_completion){
        fault == INVALID_ID ? UINT32_MAX : fault == MISALIGNED_ID ? 1 :
        fault == FREE_SLOT ? 3 : head,
        fault == ZERO_LENGTH ? 0 : descriptors[head].next == status_descriptor ? 1 : 513
    };
    if (fault == DUPLICATE_SLOT) completion[1] = completion[0];
    used[1] = fault == USED_OVERFLOW ? 9 : fault == DUPLICATE_SLOT ? 2 : 1;
    registers[0x60 / 4] = 1;
    injected = 1;
    return clock_value;
}

void virt_uart_putc(char character)
{
    if (final_statistics) return; /* 销毁统计不属于reset前的故障快照。 */
    /* Every queue-fault scalar must be captured before reset changes MMIO. */
    assert(registers[0x70 / 4] != 0);
    assert(diagnostic_length + 1 < sizeof(diagnostic));
    diagnostic[diagnostic_length++] = character;
    diagnostic[diagnostic_length] = 0;
}
void virt_uart_puts(const char *text)
{
    while (*text) virt_uart_putc(*text++);
}
void virt_uart_put_hex(unsigned long value)
{
    char text[2 + sizeof(value) * 2 + 1];
    snprintf(text, sizeof(text), "0x%lx", value);
    virt_uart_puts(text);
}
struct kernel_io_context *kernel_io_context_current(void) { return &caller; }
int kernel_scheduler_can_sleep(void) { return 1; }
int riscv_plic_in_interrupt(void) { return 0; }
int riscv_plic_register(uint32_t source, void (*handler)(void *), void *owner)
{ (void)source; (void)handler; (void)owner; return 1; }
void riscv_plic_unregister(uint32_t source, void *owner)
{ (void)source; (void)owner; }
void kernel_proc_task_note_block_read(void) {}
void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{ memset(queue, 0, sizeof(*queue)); }
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{ assert(queue->head == NULL); return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_scheduler_status kernel_scheduler_block_current(struct kernel_wait_queue *queue,
    uint64_t deadline, int interruptible, enum kernel_wait_wake_reason *reason)
{
    (void)queue; (void)deadline; (void)interruptible; (void)reason;
    abort();
}
enum physical_page_status physical_page_allocate_order(struct physical_page_allocator *allocator,
                                                       uint32_t order, uint64_t *physical)
{
    (void)allocator;
    assert(queue_allocation == NULL);
    allocation_size = 4096U << order;
    queue_allocation = calloc(1, allocation_size);
    assert(queue_allocation != NULL);
    *physical = UINT64_C(0x81000000);
    ++allocations;
    return PHYSICAL_PAGE_STATUS_OK;
}
enum physical_page_status physical_page_resolve(const struct physical_page_allocator *allocator,
                                                uint64_t physical, void **pointer)
{
    (void)allocator;
    assert(physical == UINT64_C(0x81000000));
    *pointer = queue_allocation;
    return PHYSICAL_PAGE_STATUS_OK;
}
enum physical_page_status physical_page_release_order(struct physical_page_allocator *allocator,
                                                      uint64_t physical, uint32_t order)
{
    (void)allocator;
    assert(registers[0x70 / 4] == 0);
    assert(physical == UINT64_C(0x81000000));
    assert(allocation_size == (4096U << order));
    free(queue_allocation);
    queue_allocation = NULL;
    ++releases;
    return PHYSICAL_PAGE_STATUS_OK;
}
static int dma_address(const void *pointer, uint64_t size, uint64_t *physical)
{
    if (fault == BATCH_READ) {
        uintptr_t offset = (uintptr_t)pointer - (uintptr_t)batch_output;
        assert(offset < sizeof(batch_output) && size <= sizeof(batch_output) - offset);
        *physical = UINT64_C(0x82000000) + offset; return 1;
    }
    assert(pointer != NULL && size == 512);
    *physical = UINT64_C(0x82000000);
    return 1;
}
static uint64_t field(const char *name)
{
    const char *at = strstr(diagnostic, name);
    assert(at != NULL);
    return strtoull(at + strlen(name), NULL, 0);
}

static enum riscv_virtio_mmio_block_status destroy_model(struct riscv_virtio_mmio_block *device)
{
    final_statistics = 1;
    enum riscv_virtio_mmio_block_status result = riscv_virtio_mmio_block_destroy(device);
    final_statistics = 0;
    return result;
}

static void run_case(unsigned version, enum fault_kind kind)
{
    static const char *reasons[] = {
        NULL, NULL, NULL, "used-overflow", "used-element", "used-element",
        "used-element", "slot-state", "slot-state", "device-status", "timeout"
    };
    struct riscv_virtio_mmio_block device = {0};
    struct physical_page_allocator allocator = {0};
    unsigned char buffer[512];
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x74726976;
    registers[1] = version;
    registers[2] = 2;
    registers[0x10 / 4] = 1;
    registers[0x34 / 4] = 32;
    registers[0x100 / 4] = 16;
    clock_value = 0;
    injected = 0;
    fault = kind;
    diagnostic[0] = 0;
    diagnostic_length = 0;
    assert(riscv_virtio_mmio_block_init(&device, registers, sizeof(registers),
        &allocator, dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    /* A diagnostic may print opaque owner/completion values, never follow them.
     * The free slot cannot participate in reset or the caller's real I/O owner. */
    request_at(&device, 7)->owner = (struct kernel_io_context *)(uintptr_t)1;
    request_at(&device, 7)->completion = (struct kernel_wait_queue *)(uintptr_t)3;
    enum kernel_block_status result = device.block.read(device.block.context, 0,
                                                       buffer, sizeof(buffer));
    /* A second completion fails the device after the first valid completion;
     * it must not rewrite an already-complete request's actual result. */
    assert(result == (kind == NORMAL || kind == DUPLICATE_SLOT ? KERNEL_BLOCK_STATUS_OK :
        kind == DEVICE_UNSUPPORTED ? KERNEL_BLOCK_STATUS_UNSUPPORTED :
        kind == NO_COMPLETION ? KERNEL_BLOCK_STATUS_TIMEOUT : KERNEL_BLOCK_STATUS_IO));
    assert(device.active == 0 && device.inflight == 0);
    assert(request_at(&device, 0)->state == 0 && request_at(&device, 0)->owner == NULL);
    if (kind <= DEVICE_UNSUPPORTED) {
        assert(diagnostic[0] == 0);
        assert(device_live(&device));
    } else {
        assert(strstr(diagnostic, reasons[kind]) != NULL);
        assert(field("mmio=") == (uintptr_t)registers);
        assert(field("queue=") == (uintptr_t)queue_allocation);
        assert(field("transport=") == version);
        assert(field("avail=") == 1);
        assert(field("used=") == (kind == NO_COMPLETION ? 0 : kind == USED_OVERFLOW ? 9 : kind == DUPLICATE_SLOT ? 2 : 1));
        assert(field("observed_count=") == (kind == NO_COMPLETION ? 0 : kind == USED_OVERFLOW ? 9 : kind == DUPLICATE_SLOT ? 2 : 1));
        assert(field("consumed=") == (kind == NO_COMPLETION || kind == USED_OVERFLOW ? 0 : kind == DUPLICATE_SLOT ? 2 : 1));
        assert(field("published=") == (kind == DUPLICATE_SLOT ? 0 : 1));
        assert(field("complete=") == (kind == DUPLICATE_SLOT ? 1 : 0));
        assert(field("inflight=") == (kind == DUPLICATE_SLOT ? 0 : 1));
        assert(field("active=") == 1);
        assert(field("mmio_status=") == (version == 1 ? 7 : 15));
        assert(field("interrupt=") == (kind == NO_COMPLETION ? 0 : 1));
        if (kind != USED_OVERFLOW && kind != NO_COMPLETION) {
            assert(field("item_index=") == (kind == DUPLICATE_SLOT ? 1 : 0));
            assert(field("id=") == (kind == INVALID_ID ? UINT32_MAX :
                kind == MISALIGNED_ID ? 1 : kind == FREE_SLOT ? 3 : 0));
            assert(field("length=") == (kind == ZERO_LENGTH ? 0 : 513));
        }
        if (kind == NO_COMPLETION) {
            assert(device.statistics.timeouts == 1);
            assert(strstr(diagnostic, "item_index=") == NULL);
        }
        assert(field("owner=") == (uintptr_t)&caller);
        assert(strstr(diagnostic, "owner=0x1 completion=0x3") != NULL);
        assert(registers[0x70 / 4] == 0);
        assert(device.state == RISCV_VIRTIO_BLOCK_STATE_FAILED);
        size_t logged = diagnostic_length;
        assert(device.block.read(device.block.context, 0, buffer, sizeof(buffer)) == KERNEL_BLOCK_STATUS_IO);
        assert(diagnostic_length == logged && device.statistics.requests == 1);
    }
    assert(destroy_model(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    assert(allocations == releases);
    printf("PASS block-diagnostics transport=%u case=%u\n", version, kind);
}

/* Device-side pop/push accounting is independent of guest last_used_index.
 * Reuse eight owner slots across a complete 16-bit index wrap, with reversed
 * completions and repeated IRQs. No metadata corruption or special workload. */
static void queue_reuse_case(unsigned version)
{
    struct riscv_virtio_mmio_block device = {0};
    struct physical_page_allocator allocator = {0};
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x74726976;
    registers[1] = version;
    registers[2] = 2;
    registers[0x10 / 4] = 1;
    registers[0x34 / 4] = 32;
    registers[0x100 / 4] = 16;
    clock_value = 0;
    fault = MANUAL_COMPLETION;
    diagnostic_length = 0;
    diagnostic[0] = 0;
    assert(riscv_virtio_mmio_block_init(&device, registers, sizeof(registers),
        &allocator, dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    uint64_t descriptor_address = version == 1
        ? (uint64_t)registers[0x40 / 4] << 12 : register_address(0x80);
    uint64_t available_address = version == 1
        ? descriptor_address + registers[0x38 / 4] * 16 : register_address(0x90);
    uint64_t used_address = version == 1
        ? descriptor_address + 4096 : register_address(0xa0);
    struct wire_descriptor *descriptors = physical_pointer(descriptor_address, 32 * 16);
    uint16_t *available = physical_pointer(available_address, 4 + 32 * 2);
    uint16_t *used = physical_pointer(used_address, 4 + 32 * 8);
    struct wire_completion *completion = (void *)(used + 2);
    uint16_t popped = 0, pushed = 0;
    unsigned inuse = 0, live_heads = 0;
    for (unsigned round = 0; round < 8193; round++) {
        struct block_request *requests[8];
        unsigned heads[8];
        assert(begin_call(&device, 0));
        for (unsigned i = 0; i < 8; i++) {
            requests[i] = reserve_request(&device);
            assert(requests[i]);
            assert(publish_request(&device, requests[i], VIRTIO_BLOCK_REQUEST_IN,
                i, UINT64_C(0x82000000) + i * 512, 512, 0) == KERNEL_BLOCK_STATUS_OK);
        }
        assert((uint16_t)(available[1] - popped) == 8);
        for (unsigned i = 0; i < 8; i++) {
            unsigned head = available[2 + popped++ % 32];
            assert(head < 32 && !(live_heads & (1U << head)));
            heads[i] = head;
            live_heads |= 1U << head;
            assert(++inuse <= 8);
        }
        for (unsigned i = 8; i > 0; i--) {
            unsigned head = heads[i - 1];
            assert(live_heads & (1U << head));
            unsigned tail = descriptors[descriptors[head].next].next;
            *(unsigned char *)physical_pointer(descriptors[tail].address, 1) = 0;
            completion[pushed++ % 32] = (struct wire_completion){head, 513};
            live_heads &= ~(1U << head);
            inuse--;
        }
        used[1] = pushed;
        block_irq(&device);
        block_irq(&device);
        assert(device.inflight == 0 && !inuse && !live_heads);
        for (unsigned i = 0; i < 8; i++) {
            assert(wait_request(&device, requests[i]) == KERNEL_BLOCK_STATUS_OK);
            release_request(&device, requests[i]);
        }
        end_call(&device, 0);
    }
    assert(!diagnostic_length && device.statistics.requests == 65544);
    assert(device.statistics.max_inflight == 8 && !device.active);
    assert(destroy_model(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    assert(allocations == releases);
    printf("PASS block queue reuse transport=%u: 65544 requests, reversed completions, index wrap\n", version);
}

/* A valid backing-file sync can take several seconds. Independently advance
 * the device clock, then complete successfully; no wall-clock sleep or retry. */
static void slow_completion_case(unsigned version, unsigned type)
{
    struct riscv_virtio_mmio_block device = {0};
    struct physical_page_allocator allocator = {0};
    unsigned char buffer[512] = {0};
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x74726976;
    registers[1] = version;
    registers[2] = 2;
    registers[0x10 / 4] = 1 | (1 << 9);
    registers[0x34 / 4] = 32;
    registers[0x100 / 4] = 16;
    clock_value = 0;
    complete_at = 2500;
    injected = 0;
    fault = NORMAL;
    diagnostic_length = 0;
    diagnostic[0] = 0;
    assert(riscv_virtio_mmio_block_init(&device, registers, sizeof(registers),
        &allocator, dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    enum kernel_block_status status = type == 4 ? device.block.flush(device.block.context) :
        type == 1 ? device.block.write(device.block.context, 0, buffer, sizeof(buffer)) :
                    device.block.read(device.block.context, 0, buffer, sizeof(buffer));
    assert(status == KERNEL_BLOCK_STATUS_OK);
    assert(clock_value >= 2500 && !device.statistics.timeouts && !diagnostic_length);
    assert(device_live(&device) && !device.active && !device.inflight);
    assert(destroy_model(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    assert(allocations == releases);
    complete_at = 0;
    printf("PASS block slow completion transport=%u type=%u: 2.5 seconds, no false timeout\n", version, type);
}

static void read_batch_case(unsigned version, unsigned mode)
{
    struct riscv_virtio_mmio_block device = {0};
    struct physical_page_allocator allocator = {0};
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x74726976; registers[1] = version; registers[2] = 2;
    registers[0x10 / 4] = 1; registers[0x34 / 4] = 32; registers[0x100 / 4] = 1024;
    clock_value = 0; fault = BATCH_READ; batch_mode = mode; batch_popped = batch_pushed = 0;
    diagnostic_length = 0; diagnostic[0] = 0;
    assert(riscv_virtio_mmio_block_init(&device, registers, sizeof(registers), &allocator,
        dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    memset(batch_output, 0xa5, sizeof(batch_output));
    struct kernel_block_read_span spans[8];
    for (unsigned i = 0; i < 8; i++) spans[i] = (struct kernel_block_read_span){
        .offset = (uint64_t)i * 4096 + (mode >= 4 ? 7 : 0), .buffer = batch_output[i],
        .size = mode == 4 ? 496 : mode >= 5 ? 1200 : 512};
    enum kernel_block_status result = kernel_block_read_batch(&device.block, spans, 8);
    printf("block read wire transport=%u mode=%u popped=%u max-inflight=%llu result=%u\n",
        version, mode, batch_popped, (unsigned long long)device.statistics.max_inflight, result); fflush(stdout);
    assert(device.statistics.max_inflight == 8 && !device.inflight && !device.active);
    assert(result == (mode == 1 || mode == 3 || mode == 6 ? KERNEL_BLOCK_STATUS_IO : mode == 2 ? KERNEL_BLOCK_STATUS_TIMEOUT : KERNEL_BLOCK_STATUS_OK));
    for (unsigned i = 0; i < 8; i++) {
        enum kernel_block_status expected = (mode == 2 || mode == 3) && i == 7 ? KERNEL_BLOCK_STATUS_TIMEOUT :
            (mode == 1 || mode == 3 || mode == 6) && i == 3 ? KERNEL_BLOCK_STATUS_IO : KERNEL_BLOCK_STATUS_OK;
        size_t prefix = expected == KERNEL_BLOCK_STATUS_OK ? spans[i].size : mode == 6 && i == 3 ? 505 : 0;
        assert(spans[i].status == expected && spans[i].completed == prefix);
        for (unsigned j = 0; j < sizeof(batch_output[i]); j++)
            assert(batch_output[i][j] == (j < prefix ? (unsigned char)((spans[i].offset+j)*7+3) : 0xa5));
        assert(request_at(&device, i)->state == 0 && !request_at(&device, i)->owner);
    }
    if (mode == 2 || mode == 3) assert(device.statistics.timeouts == 1 && clock_value >= 30000);
    assert(destroy_model(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK && allocations == releases);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "read-batch")) {
        for (unsigned version = 1; version <= 2; version++)
            for (unsigned mode = 0; mode < 7; mode++) read_batch_case(version, mode);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "slow")) {
        for (unsigned version = 1; version <= 2; version++) {
            slow_completion_case(version, 0);
            slow_completion_case(version, 1);
            slow_completion_case(version, 4);
        }
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "reuse")) {
        queue_reuse_case(1);
        queue_reuse_case(2);
        return 0;
    }
    if (argc == 2) {
        run_case(2, (enum fault_kind)strtoul(argv[1], NULL, 0));
        return 0;
    }
    for (unsigned version = 1; version <= 2; ++version) {
        slow_completion_case(version, 0);
        slow_completion_case(version, 1);
        slow_completion_case(version, 4);
        queue_reuse_case(version);
        for (unsigned mode = 0; mode < 7; mode++) read_batch_case(version, mode);
        for (unsigned kind = NORMAL; kind <= NO_COMPLETION; ++kind) {
            run_case(version, (enum fault_kind)kind);
        }
    }
    return 0;
}
