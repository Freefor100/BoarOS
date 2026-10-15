#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtio_mmio_block_native.c"

static uint32_t registers[128];
static void *queue_allocation, *workspace_allocation[VIRTIO_BLOCK_BATCH_POOL_PAGES];
static unsigned release_batch_slot;
static unsigned fail_workspace;
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
    NO_COMPLETION, MANUAL_COMPLETION, BATCH_READ, SG_BATCH
} fault;
static int injected, final_statistics;
static unsigned batch_mode, batch_popped, batch_pushed;
static int batch_is_write;
static struct virtio_block_device *wait_irq_device;
static unsigned char batch_output[96][8192];
static unsigned char sg_disk[1048576];
static unsigned notifications, first_notify_heads, sg_max_segments, park_calls;
static unsigned sg_fail_sector;
static unsigned sg_max_bytes, ack_calls;
static uint32_t (*underlying_ack)(void *);
static void (*before_take)(void);
static struct kernel_wait_queue *late_wait_queue;
static struct kernel_wait_node late_waiter;
static unsigned late_wakes;
enum virtio_status __real_virtio_split_take(struct virtio_split_queue *, struct virtio_completion *);
enum virtio_status __wrap_virtio_split_take(struct virtio_split_queue *queue, struct virtio_completion *completion)
{
    if (before_take) {
        void (*hook)(void)=before_take;
        before_take=NULL;
        hook();
    }
    return __real_virtio_split_take(queue,completion);
}
static unsigned phase_hold, phase_held_head, phase_crossed_window;
static unsigned char *phase_held_status;
static void (*underlying_write)(void *, enum virtio_register, uint32_t);
static void sg_device_step(void);
static uint32_t observed_ack(void *context)
{ ack_calls++; return underlying_ack(context); }

static void observed_write(void *context, enum virtio_register reg, uint32_t value)
{
    underlying_write(context, reg, value);
    if (reg == VIRTIO_REG_QUEUE_NOTIFY) {
        notifications++;
        if (fault == SG_BATCH) sg_device_step();
    }
}

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

/* Decode each wire chain without using the driver's request layout. Complete
 * in reverse order, and publish used before delivering any simulated IRQ. */
static void sg_device_step(void)
{
    unsigned size = registers[0x38 / 4];
    uint64_t base = registers[1] == 1 ? (uint64_t)registers[0x40 / 4] << 12 : register_address(0x80);
    struct wire_descriptor *d = physical_pointer(base, size * 16);
    uint16_t *available = physical_pointer(registers[1] == 1 ? base + size * 16 : register_address(0x90), 4 + size * 2);
    uint16_t *used = physical_pointer(registers[1] == 1 ? base + 4096 : register_address(0xa0), 4 + size * 8);
    struct wire_completion *done = (void *)(used + 2);
    unsigned count = (uint16_t)(available[1] - batch_popped), first = batch_popped;
    assert(count && count <= 8);
    if (notifications == 1) first_notify_heads = count;
    batch_popped += count;
    for (unsigned n = count; n; n--) {
        unsigned head = available[2 + (first + n - 1) % size], at = head, segments = 0;
        assert(head < size && d[head].flags == 1 && d[head].length == 16);
        unsigned char *header = physical_pointer(d[head].address, 16);
        uint32_t type; uint64_t sector;
        memcpy(&type, header, 4); memcpy(&sector, header + 8, 8);
        assert(type == 0 || type == 1 || type == 4);
        if (phase_hold && sector>=64) phase_crossed_window=1;
        uint64_t offset = sector * 512;
        unsigned bytes = 0;
        int error = sector == sg_fail_sector;
        while (d[at].flags & 1) {
            at = d[at].next;
            assert(at < size);
            if (!(d[at].flags & 1)) break;
            assert(d[at].flags == (type == 0 ? 3 : 1));
            assert(d[at].length && !(d[at].length % 512));
            if (registers[0x108 / 4]) assert(d[at].length <= registers[0x108 / 4]);
            if (registers[0x10c / 4]) assert(segments < registers[0x10c / 4]);
            assert(offset + d[at].length <= sizeof(sg_disk));
            unsigned char *buffer = physical_pointer(d[at].address, d[at].length);
            if (!error) {
                if (type == 0) memcpy(buffer, sg_disk + offset, d[at].length);
                else memcpy(sg_disk + offset, buffer, d[at].length);
            }
            offset += d[at].length; bytes += d[at].length; segments++;
            assert(segments <= 32);
        }
        assert(d[at].flags == 2 && d[at].length == 1);
        if (segments > sg_max_segments) sg_max_segments = segments;
        if (bytes>sg_max_bytes) sg_max_bytes=bytes;
        if (phase_hold && !sector) {
            phase_held_head=head;
            phase_held_status=physical_pointer(d[at].address,1);
            continue;
        }
        *(unsigned char *)physical_pointer(d[at].address, 1) = error ? 1 : 0;
        done[batch_pushed++ % size] = (struct wire_completion){head, error || type != 0 ? 1 : bytes + 1};
    }
    if (phase_hold && phase_crossed_window && phase_held_status) {
        *phase_held_status=0;
        done[batch_pushed++ % size]=(struct wire_completion){phase_held_head,1};
        phase_held_status=NULL;
    }
    used[1] = (uint16_t)batch_pushed;
    registers[0x60 / 4] = 1;
}

static void read_batch_device(void)
{
    if (!queue_allocation || !(registers[0x70 / 4] & 4)) return;
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
        assert(type == (batch_is_write ? 1U : 0U));
        unsigned data = descriptors[head].next, tail = descriptors[data].next;
        assert(descriptors[data].flags == (batch_is_write ? 1 : 3) && descriptors[tail].flags == 2);
        unsigned index = (unsigned)(sector / 8);
        assert(index < 8);
        /* 一个错误不能使仍被设备持有的另一span提前回收。 */
        if ((batch_mode == 2 || batch_mode == 3) && index == 7) continue;
        int error = ((batch_mode == 1 || batch_mode == 3) && index == 3) ||
            (batch_mode == 6 && index == 3 && sector % 8 == 1);
        if (!error) {
            unsigned char *out = physical_pointer(descriptors[data].address, descriptors[data].length);
            for (unsigned i = 0; i < descriptors[data].length; i++) {
                unsigned char expected = (unsigned char)((sector * 512 + i) * 7 + 3);
                if (batch_is_write) assert(out[i] == expected);
                else out[i] = expected;
            }
        }
        *(unsigned char *)physical_pointer(descriptors[tail].address, 1) = error ? 1 : 0;
        completions[batch_pushed++ % queue_size] = (struct wire_completion){head, error || batch_is_write ? 1 : descriptors[data].length + 1};
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
    unsigned writable = 0, descriptor = head;
    for (;;) {
        if (descriptors[descriptor].flags & 2) writable += descriptors[descriptor].length;
        if (!(descriptors[descriptor].flags & 1)) break;
        descriptor = descriptors[descriptor].next;
    }
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
        fault == ZERO_LENGTH ? 0 : writable
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
void kernel_console_putc(char c) { virt_uart_putc(c); }
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
{
    if (queue==late_wait_queue) {
        assert(queue->head==&late_waiter && queue->tail==&late_waiter && late_waiter.queue==queue);
        queue->head=queue->tail=NULL; late_waiter.queue=NULL; late_wakes++;
    } else assert(queue->head == NULL);
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_scheduler_block_current(struct kernel_wait_queue *queue,
    uint64_t deadline, int interruptible, enum kernel_wait_wake_reason *reason)
{
    (void)queue; (void)deadline; (void)interruptible;
    park_calls++;
    if (wait_irq_device) {
        if (release_batch_slot) {
            wait_irq_device->batch_claimed &= ~(1U << (release_batch_slot - 1));
            release_batch_slot = 0;
        } else block_irq(wait_irq_device);
        *reason = KERNEL_WAIT_WOKEN;
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    abort();
}
enum physical_page_status physical_page_allocate_order(struct physical_page_allocator *allocator,
                                                       uint32_t order, uint64_t *physical)
{
    (void)allocator;
    if (!order) {
        unsigned i;
        for (i = 0; i < VIRTIO_BLOCK_BATCH_POOL_PAGES; i++) if (!workspace_allocation[i]) break;
        assert(i < VIRTIO_BLOCK_BATCH_POOL_PAGES);
        if (fail_workspace && i >= fail_workspace - 1) return PHYSICAL_PAGE_STATUS_EMPTY;
        workspace_allocation[i] = calloc(1, 4096);
        assert(workspace_allocation[i]);
        *physical = UINT64_C(0x83000000) + i * 4096; allocations++;
        return PHYSICAL_PAGE_STATUS_OK;
    }
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
    if (physical >= UINT64_C(0x83000000)) {
        unsigned i = (physical - UINT64_C(0x83000000)) / 4096;
        assert(i < VIRTIO_BLOCK_BATCH_POOL_PAGES && workspace_allocation[i]);
        *pointer = workspace_allocation[i]; return PHYSICAL_PAGE_STATUS_OK;
    }
    assert(physical == UINT64_C(0x81000000));
    *pointer = queue_allocation;
    return PHYSICAL_PAGE_STATUS_OK;
}
enum physical_page_status physical_page_release_order(struct physical_page_allocator *allocator,
                                                      uint64_t physical, uint32_t order)
{
    (void)allocator;
    if (physical >= UINT64_C(0x83000000)) {
        unsigned i = (physical - UINT64_C(0x83000000)) / 4096;
        assert(!order && i < VIRTIO_BLOCK_BATCH_POOL_PAGES && workspace_allocation[i]);
        free(workspace_allocation[i]); workspace_allocation[i] = NULL; releases++;
        return PHYSICAL_PAGE_STATUS_OK;
    }
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
    if (fault == BATCH_READ || fault == SG_BATCH) {
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

#ifdef HOST_ALTERNATE_TRANSPORT
static unsigned alternate_accesses;
static unsigned transport_offset(enum virtio_register reg)
{
    static const unsigned offsets[]={0x70,0x14,0x10,0x24,0x20,0x30,0x34,0x38,
        0x44,0x28,0x3c,0x40,0x50,0x80,0x84,0x90,0x94,0xa0,0xa4,0xfc,0x100,0x104};
    assert((unsigned)reg<sizeof(offsets)/sizeof(offsets[0]));
    return offsets[reg];
}
static uint32_t alternate_read(void *context,enum virtio_register reg)
{
    assert(context==&alternate_accesses);alternate_accesses++;
    return registers[transport_offset(reg)/4];
}
static void alternate_write(void *context,enum virtio_register reg,uint32_t value)
{
    assert(context==&alternate_accesses);alternate_accesses++;
    registers[transport_offset(reg)/4]=value;
}
static uint32_t alternate_ack(void *context)
{
    assert(context==&alternate_accesses);alternate_accesses++;
    uint32_t pending=registers[0x60/4];registers[0x60/4]=0;
    return pending;
}
static uint32_t alternate_config(void *context,uint32_t offset,unsigned width)
{ assert(context==&alternate_accesses && width==4 && offset<16);alternate_accesses++;return registers[(0x100+offset)/4]; }
static const struct virtio_transport_ops alternate_ops={
    .read=alternate_read,.write=alternate_write,.config_read=alternate_config,.ack_interrupt=alternate_ack};
#endif
static enum riscv_virtio_mmio_block_status initialize_model(
    struct riscv_virtio_mmio_block *device,volatile void *mmio,uint64_t size,
    struct physical_page_allocator *allocator,riscv_virtio_dma_address_fn dma,uint32_t frequency)
{
    enum riscv_virtio_mmio_block_status result;
#ifdef HOST_ALTERNATE_TRANSPORT
    (void)mmio;(void)size;
    struct virtio_transport transport={.context=&alternate_accesses,
        .ops=alternate_ops,.version=registers[1],.config_size=16};
    result = virtio_block_init(device,&transport,allocator,dma,frequency);
#else
    result = riscv_virtio_mmio_block_init(device,mmio,size,allocator,dma,frequency);
#endif
    if (result == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
        underlying_write = device->transport.ops.write;
        device->transport.ops.write = observed_write;
        underlying_ack = device->transport.ops.ack_interrupt;
        device->transport.ops.ack_interrupt = observed_ack;
    }
    return result;
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
    assert(initialize_model(&device, registers, sizeof(registers),
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
    assert(device.active == 0 && device.queue.inflight == 0);
    assert(request_at(&device, 0)->state == 0 && request_at(&device, 0)->owner == NULL);
    if (kind <= DEVICE_UNSUPPORTED) {
        assert(diagnostic[0] == 0);
        assert(device_live(&device));
    } else {
        assert(strstr(diagnostic, reasons[kind]) != NULL);
        #ifdef HOST_ALTERNATE_TRANSPORT
        assert(field("transport_context=") == (uintptr_t)&alternate_accesses);
#else
        assert(field("transport_context=") == (uintptr_t)registers);
#endif
        assert(field("queue=") == (uintptr_t)queue_allocation);
        assert(field("transport=") == version);
        assert(field("avail=") == 1);
        assert(field("used=") == (kind == NO_COMPLETION ? 0 : kind == USED_OVERFLOW ? 9 : kind == DUPLICATE_SLOT ? 2 : 1));
        assert(field("observed_count=") == (kind == NO_COMPLETION ? 0 : kind == USED_OVERFLOW ? 9 : kind == DUPLICATE_SLOT ? 2 : 1));
        assert(field("consumed=") == (kind == NO_COMPLETION || kind == USED_OVERFLOW ? 0 : kind == DUPLICATE_SLOT ? 2 : 1));
        assert(field("published=") == (kind == DUPLICATE_SLOT ? 0 : 1));
        assert(field("complete=") == (kind == DUPLICATE_SLOT ? 1 : 0));
        /* Queue ownership ends at a valid used item, even if block status is
         * then invalid; the pending business owner is still reset/completed. */
        assert(field("inflight=") == (kind == DUPLICATE_SLOT || kind == INVALID_STATUS ? 0 : 1));
        assert(field("active=") == 1);
        assert(field("device_status=") == (version == 1 ? 7 : 15));
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
        assert(device.state == VIRTIO_BLOCK_STATE_FAILED);
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
    assert(initialize_model(&device, registers, sizeof(registers),
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
        assert(device.queue.inflight == 0 && !inuse && !live_heads);
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
    assert(initialize_model(&device, registers, sizeof(registers),
        &allocator, dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    enum kernel_block_status status = type == 4 ? device.block.flush(device.block.context) :
        type == 1 ? device.block.write(device.block.context, 0, buffer, sizeof(buffer)) :
                    device.block.read(device.block.context, 0, buffer, sizeof(buffer));
    assert(status == KERNEL_BLOCK_STATUS_OK);
    assert(clock_value >= 2500 && !device.statistics.timeouts && !diagnostic_length);
    assert(device_live(&device) && !device.active && !device.queue.inflight);
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
    assert(initialize_model(&device, registers, sizeof(registers), &allocator,
        dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    memset(batch_output, 0xa5, sizeof(batch_output));
    struct kernel_block_read_span spans[8];
    for (unsigned i = 0; i < 8; i++) spans[i] = (struct kernel_block_read_span){
        .offset = (uint64_t)i * 4096 + (mode >= 4 ? 7 : 0), .buffer = batch_output[i],
        .size = mode == 4 ? 496 : mode >= 5 ? 1200 : 512};
    enum kernel_block_status result = kernel_block_read_batch(&device.block, spans, 8);
    printf("block read wire transport=%u mode=%u popped=%u max-inflight=%llu result=%u\n",
        version, mode, batch_popped, (unsigned long long)device.statistics.max_inflight, result); fflush(stdout);
    assert(device.statistics.max_inflight == 8 && !device.queue.inflight && !device.active);
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

/* DMA may publish all replies before the CPU enters its wait registration.
 * The ring and delayed IRQ are independent of guest request state. */
static void completed_before_park_case(unsigned version, int writing)
{
    struct riscv_virtio_mmio_block device = {0};
    struct physical_page_allocator allocator = {0};
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x74726976; registers[1] = version; registers[2] = 2;
    registers[0x10 / 4] = 1; registers[0x34 / 4] = 32; registers[0x100 / 4] = 1024;
    clock_value = 0; fault = BATCH_READ; batch_mode = 0;
    batch_popped = batch_pushed = 0; batch_is_write = writing;
    diagnostic_length = 0; diagnostic[0] = 0;
    assert(initialize_model(&device, registers, sizeof(registers), &allocator,
        dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    device.irq_source = 1; wait_irq_device = &device;
    struct kernel_block_read_span reads[8];
    struct kernel_block_span writes[8];
    for (unsigned i = 0; i < 8; i++) {
        for (unsigned j = 0; j < 512; j++)
            batch_output[i][j] = writing ? (unsigned char)((i * 4096 + j) * 7 + 3) : 0xa5;
        reads[i] = (struct kernel_block_read_span){i * 4096, batch_output[i], 512, 0, 0};
        writes[i] = (struct kernel_block_span){i * 4096, batch_output[i], 512};
    }
    enum kernel_block_status result = writing ? kernel_block_write_batch(&device.block, writes, 8) :
        kernel_block_read_batch(&device.block, reads, 8);
    assert(result == KERNEL_BLOCK_STATUS_OK && batch_pushed == 8);
    /* A completed device operation must not wait for its timeout to be harvested. */
    assert(clock_value < device.timeout_ticks && !device.statistics.timeouts);
    assert(!device.active && !device.queue.inflight);
    for (unsigned i = 0; i < 8; i++) {
        if (!writing) assert(reads[i].status == KERNEL_BLOCK_STATUS_OK && reads[i].completed == 512);
        for (unsigned j = 0; j < 512; j++)
            assert(batch_output[i][j] == (unsigned char)((i * 4096 + j) * 7 + 3));
    }
    device.irq_source = 0; wait_irq_device = NULL; batch_is_write = 0;
    assert(destroy_model(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK && allocations == releases);
    printf("PASS block DMA-before-park transport=%u write=%d: completion and owner drained before deadline\n", version, writing);
}

static void sg_batch_case(unsigned version, unsigned queue_size, int writing, int sparse, int failing)
{
    struct riscv_virtio_mmio_block device = {0};
    struct physical_page_allocator allocator = {0};
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x74726976; registers[1] = version; registers[2] = 2;
    registers[0x10 / 4] = 1 | (1 << 9); registers[0x34 / 4] = queue_size; registers[0x100 / 4] = 128;
    fault = SG_BATCH; batch_popped = batch_pushed = 0; clock_value = 0;
    notifications = first_notify_heads = sg_max_segments = park_calls = 0;
    sg_fail_sector = failing ? 0 : UINT32_MAX;
    diagnostic_length = 0; diagnostic[0] = 0;
    assert(initialize_model(&device, registers, sizeof(registers), &allocator,
        dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    device.irq_source = 1; wait_irq_device = &device;
    struct kernel_block_read_span reads[8]; struct kernel_block_span writes[8];
    for (unsigned j = 0; j < sizeof(sg_disk); j++) sg_disk[j] = (unsigned char)(j * 7 + j / 512 + 3);
    for (unsigned i = 0; i < 8; i++) {
        unsigned offset = i * (sparse ? 4096 : 512);
        memset(batch_output[i], 0xa5, 4096);
        if (writing) for (unsigned j = 0; j < 512; j++) batch_output[i][j] = (unsigned char)(i + j * 13);
        reads[i] = (struct kernel_block_read_span){offset, batch_output[i], 512, 0, 0};
        writes[i] = (struct kernel_block_span){offset, batch_output[i], 512};
    }
    enum kernel_block_status result = writing ? kernel_block_write_batch(&device.block, writes, 8) :
        kernel_block_read_batch(&device.block, reads, 8);
    assert(result == (failing ? KERNEL_BLOCK_STATUS_IO : KERNEL_BLOCK_STATUS_OK));
    if (queue_size == 32) {
        assert(notifications == 1);
        assert(first_notify_heads == (sparse ? 8U : 1U));
        assert(device.statistics.requests == (sparse ? 8U : 1U));
        assert(sg_max_segments == (sparse ? 1U : 8U));
    } else {
        assert(sg_max_segments == queue_size - 2);
        assert(device.statistics.requests == (queue_size == 4 ? 4U : 2U));
    }
    assert(!park_calls && !device.statistics.sleeps && !device.statistics.timeouts);
    for (unsigned i = 0; i < 8; i++) {
        if (!writing) assert(reads[i].status == (failing ? KERNEL_BLOCK_STATUS_IO : KERNEL_BLOCK_STATUS_OK) &&
            reads[i].completed == (failing ? 0U : 512U));
        for (unsigned j = 0; j < 512; j++) {
            unsigned offset = i * (sparse ? 4096 : 512) + j;
            if (!failing) assert(writing ? sg_disk[offset] == (unsigned char)(i + j * 13) :
                batch_output[i][j] == (unsigned char)(offset * 7 + offset / 512 + 3));
            assert(batch_output[i][512 + j] == 0xa5);
        }
    }
    assert(!device.active && !device.queue.inflight && !device.queue.claimed && !diagnostic_length);
    device.irq_source = 0; wait_irq_device = NULL;
    assert(destroy_model(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK && allocations == releases);
    printf("PASS block SG transport=%u queue=%u write=%d sparse=%d error=%d: payload, descriptors, notifications and no park\n",
        version, queue_size, writing, sparse, failing);
}

/* Gaps between the mapped pages make every 4 KiB buffer a distinct DMA
 * extent; the other 4 KiB in each slot is a canary, never a bounce area. */
static void sg_128k_case(unsigned version, unsigned queue_size, int writing,
                         unsigned size_max, unsigned seg_max)
{
    struct riscv_virtio_mmio_block device = {0};
    struct physical_page_allocator allocator = {0};
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x74726976; registers[1] = version; registers[2] = 2;
    registers[0x10 / 4] = 1 | (1 << 9) | (size_max ? 2 : 0) | (seg_max ? 4 : 0);
    registers[0x34 / 4] = queue_size; registers[0x100 / 4] = sizeof(sg_disk) / 512;
    registers[0x108 / 4] = size_max; registers[0x10c / 4] = seg_max;
    fault = SG_BATCH; batch_popped = batch_pushed = 0; clock_value = 0;
    notifications = first_notify_heads = sg_max_segments = park_calls = 0;
    sg_fail_sector = UINT32_MAX; diagnostic_length = 0;
    assert(initialize_model(&device, registers, sizeof(registers), &allocator,
        dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    device.irq_source = 1; wait_irq_device = &device;
    struct kernel_block_read_span reads[32]; struct kernel_block_span writes[32];
    for (unsigned j = 0; j < sizeof(sg_disk); j++) sg_disk[j] = (unsigned char)(j * 7 + j / 512 + 3);
    for (unsigned i = 0; i < 32; i++) {
        memset(batch_output[i], 0xa5, sizeof(batch_output[i]));
        if (writing) for (unsigned j = 0; j < 4096; j++) batch_output[i][j] = (unsigned char)(i + j * 13);
        reads[i] = (struct kernel_block_read_span){i * 4096, batch_output[i], 4096, 0, 0};
        writes[i] = (struct kernel_block_span){i * 4096, batch_output[i], 4096};
    }
    enum kernel_block_status result = writing ? kernel_block_write_batch(&device.block, writes, 32) :
        kernel_block_read_batch(&device.block, reads, 32);
    if (result != KERNEL_BLOCK_STATUS_OK) {
        fprintf(stderr, "128 KiB SG rejected transport=%u queue=%u status=%u submitted=%llu\n",
            version, queue_size, result, (unsigned long long)device.statistics.requests);
    }
    assert(result == KERNEL_BLOCK_STATUS_OK);
    if (queue_size == 64 && !size_max && !seg_max)
        assert(notifications == 1 && first_notify_heads == 1 && device.statistics.requests == 1 && sg_max_segments == 32);
    if (seg_max) assert(sg_max_segments <= seg_max);
    if (queue_size < 64) assert(sg_max_segments <= queue_size - 2);
    for (unsigned i = 0; i < 32; i++) {
        if (!writing) assert(reads[i].completed == 4096 && reads[i].status == KERNEL_BLOCK_STATUS_OK);
        for (unsigned j = 0; j < 4096; j++) {
            unsigned offset = i * 4096 + j;
            assert(writing ? sg_disk[offset] == (unsigned char)(i + j * 13) :
                batch_output[i][j] == (unsigned char)(offset * 7 + offset / 512 + 3));
            assert(batch_output[i][4096 + j] == 0xa5);
        }
    }
    if (size_max) {
        uint64_t before = device.statistics.requests;
        enum kernel_block_status scalar = writing ? kernel_block_write_at(&device.block, 0, batch_output[0], 4096) :
            kernel_block_read_at(&device.block, 0, batch_output[0], 4096);
        assert(scalar == KERNEL_BLOCK_STATUS_OK && device.statistics.requests == before + 4096 / size_max);
    }
    assert(!device.active && !device.queue.inflight && !device.queue.claimed && !diagnostic_length);
    assert(!park_calls && !device.statistics.bounce_requests);
    device.irq_source = 0; wait_irq_device = NULL;
    printf("PASS 128 KiB discrete SG transport=%u queue=%u write=%d size_max=%u seg_max=%u: requests=%llu notifications=%u max-segments=%u\n",
        version, queue_size, writing, size_max, seg_max, (unsigned long long)device.statistics.requests, notifications, sg_max_segments);
    assert(destroy_model(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK && allocations == releases);
}

struct phase_entry { struct kernel_block_span span; struct phase_entry *next; };
static int phase_next(const void **cursor, struct kernel_block_span *span)
{
    const struct phase_entry *entry=*cursor;
    if (!entry) return 0;
    *span=entry->span; *cursor=entry->next; return 1;
}
static void write_phase_case(unsigned version, int failing)
{
    struct riscv_virtio_mmio_block device={0}; struct physical_page_allocator allocator={0};
    memset(registers,0,sizeof(registers));
    registers[0]=0x74726976; registers[1]=version; registers[2]=2;
    registers[0x10/4]=1; registers[0x34/4]=32; registers[0x100/4]=sizeof(sg_disk)/512;
    fault=SG_BATCH; batch_popped=batch_pushed=clock_value=0;
    notifications=first_notify_heads=sg_max_segments=park_calls=0;
    sg_fail_sector=failing?64:UINT32_MAX; diagnostic_length=0;
    phase_hold=1; phase_crossed_window=0; phase_held_status=NULL;
    assert(initialize_model(&device,registers,sizeof(registers),&allocator,dma_address,1000)==RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    device.irq_source=1; wait_irq_device=&device;
    struct phase_entry entries[80];
    memset(sg_disk,0,sizeof(sg_disk));
    for (unsigned i=0;i<80;i++) {
        memset(batch_output[i],(unsigned char)(i+3),512);
        entries[i]=(struct phase_entry){{i*1024,batch_output[i],512},i==79?NULL:&entries[i+1]};
    }
    /* A malformed final range must prevent publication of the valid prefix. */
    entries[79].span.offset=sizeof(sg_disk);
    assert(kernel_block_write_phase(&device.block,phase_next,entries)==KERNEL_BLOCK_STATUS_OUT_OF_RANGE && !device.statistics.requests);
    entries[79].span.offset=79*1024;
    enum kernel_block_status result=kernel_block_write_phase(&device.block,phase_next,entries);
    assert(result==(failing?KERNEL_BLOCK_STATUS_IO:KERNEL_BLOCK_STATUS_OK));
    assert(phase_crossed_window && !phase_held_status);
    assert(first_notify_heads==8 && device.statistics.max_inflight==8);
    assert(!device.active && !device.queue.inflight && !device.queue.claimed && !device.batch_claimed);
    if (!failing) for (unsigned i=0;i<80;i++) for (unsigned j=0;j<512;j++)
        assert(sg_disk[i*1024+j]==(unsigned char)(i+3));
    else assert(device.statistics.requests<80);
    phase_hold=0; device.irq_source=0; wait_irq_device=NULL;
    printf("PASS journal phase transport=%u error=%d: 80 spans, later window enters while first DMA held, error drains owners\n",version,failing);
    assert(destroy_model(&device)==RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK && allocations==releases);
}
static void large_phase_case(unsigned version)
{
    struct riscv_virtio_mmio_block device={0}; struct physical_page_allocator allocator={0};
    memset(registers,0,sizeof(registers));
    registers[0]=0x74726976; registers[1]=version; registers[2]=2;
    registers[0x10/4]=1; registers[0x34/4]=32; registers[0x100/4]=sizeof(sg_disk)/512;
    fault=SG_BATCH; batch_popped=batch_pushed=clock_value=0;
    notifications=first_notify_heads=sg_max_segments=sg_max_bytes=park_calls=0;
    sg_fail_sector=UINT32_MAX; diagnostic_length=0;
    assert(initialize_model(&device,registers,sizeof(registers),&allocator,dma_address,1000)==RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    device.irq_source=1; wait_irq_device=&device;
    unsigned char *bytes=(void *)batch_output;
    for (unsigned i=0;i<512*1024;i++) bytes[i]=(unsigned char)(i*13+i/512);
    struct phase_entry entry={{0,bytes,512*1024},NULL};
    assert(kernel_block_write_phase(&device.block,phase_next,&entry)==KERNEL_BLOCK_STATUS_OK);
    assert(sg_max_bytes==128*1024 && device.statistics.requests==4 && first_notify_heads==4);
    assert(device.statistics.max_inflight==4 && !memcmp(bytes,sg_disk,512*1024));
    struct kernel_block_span public_span={0,bytes,512*1024};
    assert(kernel_block_write_batch(&device.block,&public_span,1)==KERNEL_BLOCK_STATUS_OK);
    assert(device.statistics.requests==8 && sg_max_bytes==128*1024);
    unsigned before=ack_calls;
    for (unsigned i=0;i<32;i++) collect_used(&device);
    assert(ack_calls==before);
    block_irq(&device);
    assert(ack_calls==before+1); /* A real IRQ is acknowledged even with no used entry. */
    assert(!device.active && !device.queue.inflight && !device.queue.claimed && !device.batch_claimed);
    device.irq_source=0; wait_irq_device=NULL;
    printf("PASS large journal phase transport=%u: one 512KiB owner, four concurrent 128KiB requests, quiet poll and IRQ ack\n",version);
    assert(destroy_model(&device)==RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK && allocations==releases);
}
static void late_harvest_case(unsigned version)
{
    struct riscv_virtio_mmio_block device={0}; struct physical_page_allocator allocator={0};
    memset(registers,0,sizeof(registers));
    registers[0]=0x74726976; registers[1]=version; registers[2]=2;
    registers[0x10/4]=1; registers[0x34/4]=32; registers[0x100/4]=sizeof(sg_disk)/512;
    fault=MANUAL_COMPLETION; batch_popped=batch_pushed=clock_value=0;
    notifications=first_notify_heads=sg_max_segments=0; sg_fail_sector=UINT32_MAX;
    diagnostic_length=0; late_wakes=0;
    assert(initialize_model(&device,registers,sizeof(registers),&allocator,dma_address,1000)==RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    assert(begin_call(&device,0));
    struct block_request *requests[8];
    for (unsigned i=0;i<8;i++) {
        requests[i]=reserve_request(&device); assert(requests[i]);
        requests[i]->completion=&device.available;
        assert(publish_request(&device,requests[i],VIRTIO_BLOCK_REQUEST_OUT,i,
            UINT64_C(0x82000000)+i*512,512,0)==KERNEL_BLOCK_STATUS_OK);
    }
    late_waiter=(struct kernel_wait_node){.queue=&device.available};
    late_wait_queue=&device.available;
    device.available.head=device.available.tail=&late_waiter;
    assert(device.queue.used->index==device.queue.last_used);
    /* This hook runs only on entry to the real queue take, after harvest's
     * initial used-index snapshot. Eight completions become visible then. */
    before_take=sg_device_step;
    block_irq(&device);
    assert(!before_take && late_wakes==1 && !late_waiter.queue && !device.available.head);
    assert(!device.queue.inflight && device.statistics.wakes==1);
    late_wait_queue=NULL;
    for (unsigned i=0;i<8;i++) {
        assert(wait_request(&device,requests[i])==KERNEL_BLOCK_STATUS_OK);
        release_request(&device,requests[i]);
    }
    end_call(&device,0);
    assert(!device.active && !device.queue.claimed);
    printf("PASS late used harvest transport=%u: eight completions after empty snapshot wake the registered batch waiter once\n",version);
    assert(destroy_model(&device)==RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK && allocations==releases);
}
static void bad_segment_limits_case(unsigned version)
{
    for (unsigned kind = 0; kind < 2; kind++) {
        struct riscv_virtio_mmio_block device = {0};
        struct physical_page_allocator allocator = {0};
        memset(registers, 0, sizeof(registers));
        registers[0] = 0x74726976; registers[1] = version; registers[2] = 2;
        registers[0x10 / 4] = 1 | 2 | 4; registers[0x34 / 4] = 64;
        registers[0x108 / 4] = kind ? 4096 : 511;
        registers[0x10c / 4] = kind ? 0 : 32;
        unsigned before = allocations;
        assert(initialize_model(&device, registers, sizeof(registers), &allocator,
            dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_UNSUPPORTED);
        assert(allocations == before && allocations == releases && !registers[0x70 / 4] && !device.state);
    }
    printf("PASS invalid segment limits transport=%u: reject before allocating DMA\n", version);
}

static void workspace_pool_case(unsigned version)
{
    struct riscv_virtio_mmio_block device = {0};
    struct physical_page_allocator allocator = {0};
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x74726976; registers[1] = version; registers[2] = 2;
    registers[0x10 / 4] = 1; registers[0x34 / 4] = 64; registers[0x100 / 4] = 128;
    fault = SG_BATCH; batch_popped = batch_pushed = 0; clock_value = 0;
    notifications = first_notify_heads = sg_max_segments = park_calls = 0; sg_fail_sector = UINT32_MAX;
    assert(initialize_model(&device, registers, sizeof(registers), &allocator,
        dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    struct kernel_block_read_span reads[2]; struct kernel_block_span writes[2];
    for (unsigned i = 0; i < 2; i++) {
        reads[i] = (struct kernel_block_read_span){i * 4096, batch_output[i], 4096, 0, 0};
        writes[i] = (struct kernel_block_span){i * 4096, batch_output[i], 4096};
    }
    unsigned before = allocations;
    fail_workspace = 1;
    assert(kernel_block_read_batch(&device.block, reads, 2) == KERNEL_BLOCK_STATUS_OK);
    for (unsigned i = 0; i < 2; i++) assert(reads[i].completed == 4096 && reads[i].status == KERNEL_BLOCK_STATUS_OK);
    assert(kernel_block_write_batch(&device.block, writes, 2) == KERNEL_BLOCK_STATUS_OK);
    assert(!device.active && !device.queue.inflight && !device.batch_claimed && allocations == before);
    for (unsigned i = 0; i < VIRTIO_BLOCK_BATCH_POOL_PAGES; i++) memset(device.batch_memory[i], 0xa7, 4096);
    device.batch_claimed = 255;
    device.irq_source = 1; wait_irq_device = &device; release_batch_slot = 8;
    assert(kernel_block_read_batch(&device.block, reads, 2) == KERNEL_BLOCK_STATUS_OK);
    assert(park_calls == 1 && !release_batch_slot && device.batch_claimed == 127);
    for (unsigned i = 0; i < 7 * VIRTIO_BLOCK_BATCH_WORKSPACE_BYTES; i++)
        assert(((unsigned char *)device.batch_memory[i / 4096])[i % 4096] == 0xa7);
    device.batch_claimed = 0; device.irq_source = 0; wait_irq_device = NULL;
    assert(kernel_block_read_batch(&device.block, reads, 1) == KERNEL_BLOCK_STATUS_OK && reads[0].completed == 4096);
    assert(allocations == before && !caller.allocation_depth);
    fail_workspace = 0;
    assert(destroy_model(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK && allocations == releases);
    printf("PASS reserved batch workspace transport=%u: allocation denied, full pool waits, other owners intact, one span allocates nothing\n", version);
    for (unsigned failure = 1; failure <= 3; failure += 2) {
        device = (struct riscv_virtio_mmio_block){0};
        memset(registers, 0, sizeof(registers));
        registers[0] = 0x74726976; registers[1] = version; registers[2] = 2;
        registers[0x10 / 4] = 1; registers[0x34 / 4] = 64; registers[0x100 / 4] = 128;
        fail_workspace = failure;
        assert(initialize_model(&device, registers, sizeof(registers), &allocator,
            dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_NO_MEMORY);
        assert(!device.state && allocations == releases && !queue_allocation);
    }
    fail_workspace = 0;
}

static void scalar_dma_before_irq_case(unsigned version)
{
    struct riscv_virtio_mmio_block device = {0};
    struct physical_page_allocator allocator = {0};
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x74726976; registers[1] = version; registers[2] = 2;
    registers[0x10 / 4] = 1; registers[0x34 / 4] = 32; registers[0x100 / 4] = 128;
    fault = SG_BATCH; batch_popped = batch_pushed = 0; clock_value = 0;
    notifications = first_notify_heads = sg_max_segments = park_calls = 0; sg_fail_sector = UINT32_MAX;
    diagnostic_length = 0; diagnostic[0] = 0;
    assert(initialize_model(&device, registers, sizeof(registers), &allocator,
        dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    device.irq_source = 1; wait_irq_device = &device;
    assert(kernel_block_read_at(&device.block, 0, batch_output[0], 512) == KERNEL_BLOCK_STATUS_OK);
    assert(!park_calls && !device.statistics.sleeps && !device.queue.inflight);
    struct virtio_block_statistics statistics;
    uint64_t before = clock_value;
    virtio_block_get_statistics(&device, &statistics);
    assert(clock_value == before && !statistics.inflight_ticks && !statistics.busy_ticks &&
        !statistics.total_ticks && !statistics.queue_wait_ticks && !statistics.service_ticks);
    device.irq_source = 0; wait_irq_device = NULL;
    assert(destroy_model(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK && allocations == releases);
    printf("PASS block scalar DMA-before-IRQ transport=%u: zero sleep\n", version);
}

static void fragmented_descriptors_case(unsigned version)
{
    struct riscv_virtio_mmio_block device = {0};
    struct physical_page_allocator allocator = {0};
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x74726976; registers[1] = version; registers[2] = 2;
    registers[0x10 / 4] = 1; registers[0x34 / 4] = 32; registers[0x100 / 4] = 128;
    fault = MANUAL_COMPLETION; clock_value = 0; diagnostic_length = 0; diagnostic[0] = 0;
    assert(initialize_model(&device, registers, sizeof(registers), &allocator,
        dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    assert(begin_call(&device, 0));
    struct block_request *first = reserve_request(&device), *held = reserve_request(&device);
    assert(publish_request(&device, first, 0, 0, UINT64_C(0x82000200), 512, 0) == KERNEL_BLOCK_STATUS_OK);
    assert(publish_request(&device, held, 0, 0, UINT64_C(0x82000400), 512, 0) == KERNEL_BLOCK_STATUS_OK);
    unsigned first_head = device.queue.available->ring[0], held_head = device.queue.available->ring[1];
    struct virtio_descriptor saved[3]; unsigned at = held_head;
    for (unsigned i = 0; i < 3; i++) { saved[i] = device.queue.descriptors[at]; at = saved[i].next; }
    first->status = 0;
    device.queue.used->ring[0] = (struct virtio_used_element){first_head, 513}; device.queue.used->index = 1;
    assert(wait_request(&device, first) == KERNEL_BLOCK_STATUS_OK); release_request(&device, first);
    fault = SG_BATCH; batch_popped = 2; batch_pushed = 1;
    notifications = first_notify_heads = sg_max_segments = park_calls = 0; sg_fail_sector = UINT32_MAX;
    device.irq_source = 1; wait_irq_device = &device;
    struct kernel_block_read_span reads[8];
    for (unsigned i = 0; i < 8; i++) reads[i] = (struct kernel_block_read_span){i * 512, batch_output[i], 512, 0, 0};
    assert(kernel_block_read_batch(&device.block, reads, 8) == KERNEL_BLOCK_STATUS_OK);
    assert(notifications == 1 && sg_max_segments == 8 && device.queue.inflight == 1);
    at = held_head;
    for (unsigned i = 0; i < 3; i++) { assert(!memcmp(&saved[i], &device.queue.descriptors[at], sizeof(saved[i]))); at = saved[i].next; }
    held->status = 0;
    device.queue.used->ring[2] = (struct virtio_used_element){held_head, 513}; device.queue.used->index = 3;
    assert(wait_request(&device, held) == KERNEL_BLOCK_STATUS_OK); release_request(&device, held); end_call(&device, 0);
    assert(!park_calls && !device.queue.claimed && !device.queue.inflight && !device.active);
    device.irq_source = 0; wait_irq_device = NULL;
    assert(destroy_model(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK && allocations == releases);
    printf("PASS block fragmented descriptor ownership transport=%u: SG skips a live chain\n", version);
}

int main(int argc, char **argv)
{
    if (argc==2 && !strcmp(argv[1],"late-harvest")) {
        late_harvest_case(1); late_harvest_case(2); return 0;
    }
    if (argc==2 && !strcmp(argv[1],"large-phase")) {
        large_phase_case(1); large_phase_case(2); return 0;
    }
    if (argc==2 && !strcmp(argv[1],"write-phase")) {
        for (unsigned version=1;version<=2;version++) for (int failing=0;failing<=1;failing++) write_phase_case(version,failing);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "sg-128k")) {
        for (unsigned version = 1; version <= 2; version++) for (int writing = 0; writing <= 1; writing++) {
            bad_segment_limits_case(version);
            workspace_pool_case(version);
            sg_128k_case(version, 64, writing, 0, 0);
            sg_128k_case(version, 32, writing, 0, 0);
            sg_128k_case(version, 64, writing, 1024, 3);
        }
        return 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "sg") || !strcmp(argv[1], "sg-batch"))) {
        for (unsigned version = 1; version <= 2; version++) {
            if (!strcmp(argv[1], "sg")) scalar_dma_before_irq_case(version);
            fragmented_descriptors_case(version);
            for (int writing = 0; writing <= 1; writing++) {
                sg_batch_case(version, 32, writing, 0, 0);
                sg_batch_case(version, 32, writing, 1, 0);
                sg_batch_case(version, 32, writing, 0, 1);
                sg_batch_case(version, 4, writing, 0, 0);
                sg_batch_case(version, 8, writing, 0, 0);
            }
        }
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "pre-park")) {
        for (unsigned version = 1; version <= 2; version++)
            for (int writing = 0; writing <= 1; writing++) completed_before_park_case(version, writing);
        return 0;
    }
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
        late_harvest_case(version);
        large_phase_case(version);
        write_phase_case(version,0);
        write_phase_case(version,1);
        bad_segment_limits_case(version);
        workspace_pool_case(version);
        for (int writing = 0; writing <= 1; writing++) {
            sg_128k_case(version, 64, writing, 0, 0);
            sg_128k_case(version, 32, writing, 0, 0);
            sg_128k_case(version, 64, writing, 1024, 3);
        }
        scalar_dma_before_irq_case(version);
        fragmented_descriptors_case(version);
        for (int writing = 0; writing <= 1; writing++) {
            sg_batch_case(version, 32, writing, 0, 0);
            sg_batch_case(version, 32, writing, 1, 0);
            sg_batch_case(version, 32, writing, 0, 1);
            sg_batch_case(version, 4, writing, 0, 0);
            sg_batch_case(version, 8, writing, 0, 0);
        }
        slow_completion_case(version, 0);
        slow_completion_case(version, 1);
        slow_completion_case(version, 4);
        queue_reuse_case(version);
        completed_before_park_case(version, 0);
        completed_before_park_case(version, 1);
        for (unsigned mode = 0; mode < 7; mode++) read_batch_case(version, mode);
        for (unsigned kind = NORMAL; kind <= NO_COMPLETION; ++kind) {
            run_case(version, (enum fault_kind)kind);
        }
    }
    return 0;
}

#include "wait_boundary.h"
HOST_WAIT_BOUNDARY(, kernel_scheduler_block_current)
