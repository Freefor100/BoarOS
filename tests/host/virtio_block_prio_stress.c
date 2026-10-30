#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtio_mmio_block_native.c"

static uint32_t registers[128];
static void *queue_allocation, *workspace_allocation[VIRTIO_BLOCK_BATCH_POOL_PAGES];
static size_t allocation_size;
static unsigned allocations, releases;
static uint64_t clock_value;
static unsigned char batch_output[96][8192];

static _Thread_local struct kernel_cpu block_cpu = {.initialized = KERNEL_CPU_INITIALIZED};
void *allocator_host_cpu_get(void) { return &block_cpu; }

uint64_t host_block_time(void) { return ++clock_value; }
void virt_uart_putc(char c) { (void)c; }
void virt_uart_puts(const char *s) { (void)s; }
void virt_uart_put_hex(unsigned long v) { (void)v; }

void kernel_console_putc(char c) { (void)c; }
int kernel_scheduler_can_sleep(void) { return 1; }
int riscv_plic_in_interrupt(void) { return 0; }
int riscv_plic_register(uint32_t source, void (*handler)(void *), void *owner)
{ (void)source; (void)handler; (void)owner; return 1; }
void riscv_plic_unregister(uint32_t source, void *owner)
{ (void)source; (void)owner; }

void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{ memset(queue, 0, sizeof(*queue)); }

enum kernel_scheduler_status kernel_wait_node_bind(struct kernel_wait_queue *queue,
    struct kernel_wait_node *node, const struct kernel_wait_token *token)
{
    assert(queue && node->task == token->task && token->generation && !node->queue);
    node->queue = queue;
    return KERNEL_SCHEDULER_STATUS_OK;
}

static unsigned mock_slots_wakes = 0;
static unsigned mock_slots_low_wakes = 0;

enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{
    extern struct riscv_virtio_mmio_block *g_test_device;
    if (g_test_device) {
        if (queue == &g_test_device->slots) mock_slots_wakes++;
        if (queue == &g_test_device->slots_low) mock_slots_low_wakes++;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

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
    if (!order && registers[0x70 / 4] != 0) {
        for (unsigned i = 0; i < VIRTIO_BLOCK_BATCH_POOL_PAGES; i++) {
            if (!workspace_allocation[i]) {
                workspace_allocation[i] = calloc(1, 4096);
                assert(workspace_allocation[i]);
                *physical = UINT64_C(0x83000000) + (uint64_t)i * 4096;
                allocations++;
                return PHYSICAL_PAGE_STATUS_OK;
            }
        }
    }
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
        *pointer = workspace_allocation[i];
        return PHYSICAL_PAGE_STATUS_OK;
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
        free(workspace_allocation[i]);
        workspace_allocation[i] = NULL;
        releases++;
        return PHYSICAL_PAGE_STATUS_OK;
    }
    assert(physical == UINT64_C(0x81000000));
    assert(allocation_size == (4096U << order));
    free(queue_allocation);
    queue_allocation = NULL;
    ++releases;
    return PHYSICAL_PAGE_STATUS_OK;
}

static int dma_address(const void *pointer, uint64_t size, uint64_t *physical)
{
    (void)pointer; (void)size;
    *physical = UINT64_C(0x82000000);
    return 1;
}

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
#ifdef HOST_ALTERNATE_TRANSPORT
    (void)mmio;(void)size;
    struct virtio_transport transport={.context=&alternate_accesses,
        .ops=alternate_ops,.version=registers[1],.config_size=16};
    return virtio_block_init(device,&transport,allocator,dma,frequency);
#else
    return riscv_virtio_mmio_block_init(device,mmio,size,allocator,dma,frequency);
#endif
}

static enum riscv_virtio_mmio_block_status destroy_model(struct riscv_virtio_mmio_block *device)
{
    return riscv_virtio_mmio_block_destroy(device);
}

static enum kernel_block_status publish_for_test(struct virtio_block_device *device,
    struct block_request *request,uint32_t type,uint64_t sector,uint64_t address,uint32_t size,int bounce)
{
    KERNEL_RAW_SCOPE(guard,&device->metadata);
    return publish_request_locked(device,request,type,sector,address,size,bounce);
}

static unsigned popcount64(uint64_t x)
{
    unsigned count = 0;
    while (x) {
        count += (x & 1U);
        x >>= 1;
    }
    return count;
}

/* Adversarial Test 1: Quota Partitioning and Anti-Starvation */
static void test_quota_and_anti_starvation(unsigned version)
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

    assert(initialize_model(&device, registers, sizeof(registers),
           &allocator, dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    assert(begin_call(&device, 0));

    struct block_request *low_reqs[8] = {0};
    struct block_request *high_reqs[8] = {0};

    /* 1. Allocate 4 low-priority requests (capped at 4) */
    for (unsigned i = 0; i < 4; i++) {
        low_reqs[i] = try_reserve_request(&device, KERNEL_BLOCK_PRIO_LOW);
        assert(low_reqs[i] != NULL);
        assert(low_reqs[i]->priority == (uint8_t)KERNEL_BLOCK_PRIO_LOW);
    }
    assert(!low_slots_under_cap(&device));

    /* 2. 5th low-priority request must be rejected */
    assert(try_reserve_request(&device, KERNEL_BLOCK_PRIO_LOW) == NULL);

    /* 3. High-priority request can still allocate remaining 4 slots */
    for (unsigned i = 0; i < 4; i++) {
        high_reqs[i] = try_reserve_request(&device, KERNEL_BLOCK_PRIO_HIGH);
        assert(high_reqs[i] != NULL);
        assert(high_reqs[i]->priority == (uint8_t)KERNEL_BLOCK_PRIO_HIGH);
    }

    /* 4. Total slots exhausted (8/8) */
    assert(try_reserve_request(&device, KERNEL_BLOCK_PRIO_HIGH) == NULL);
    assert(try_reserve_request(&device, KERNEL_BLOCK_PRIO_LOW) == NULL);

    /* 5. Reader anti-starvation: release 1 low-priority request (low_count drops to 3) */
    release_request_owner(&device, low_reqs[3]);
    low_reqs[3] = NULL;
    assert(low_slots_under_cap(&device));

    /* With slots.head != NULL (high priority reader waiting), low priority must yield */
    struct kernel_wait_node reader_waiter = {0};
    device.slots.head = &reader_waiter;
    assert(try_reserve_request(&device, KERNEL_BLOCK_PRIO_LOW) == NULL);

    /* High priority request CAN allocate */
    struct block_request *high_extra = try_reserve_request(&device, KERNEL_BLOCK_PRIO_HIGH);
    assert(high_extra != NULL);
    device.slots.head = NULL;

    /* Release high_extra */
    release_request_owner(&device, high_extra);

    /* Now with slots.head == NULL, low priority request CAN allocate */
    low_reqs[3] = try_reserve_request(&device, KERNEL_BLOCK_PRIO_LOW);
    assert(low_reqs[3] != NULL);

    /* Clean up all allocated requests */
    for (unsigned i = 0; i < 4; i++) {
        if (low_reqs[i]) release_request_owner(&device, low_reqs[i]);
        if (high_reqs[i]) release_request_owner(&device, high_reqs[i]);
    }
    assert(low_slots_under_cap(&device));

    /* 6. Verify high priority can allocate all 8 slots when uncontended */
    for (unsigned i = 0; i < 8; i++) {
        high_reqs[i] = try_reserve_request(&device, KERNEL_BLOCK_PRIO_HIGH);
        assert(high_reqs[i] != NULL);
    }
    assert(try_reserve_request(&device, KERNEL_BLOCK_PRIO_HIGH) == NULL);
    for (unsigned i = 0; i < 8; i++) {
        release_request_owner(&device, high_reqs[i]);
    }

    end_call(&device, 0);
    assert(destroy_model(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    assert(allocations == releases);
    printf("PASS Quota partitioning and reader anti-starvation (transport=%u)\n", version);
}

struct riscv_virtio_mmio_block *g_test_device = NULL;

/* Adversarial Test 2: Wait Queue Wake Isolation */
static void test_wait_queue_isolation(unsigned version)
{
    struct riscv_virtio_mmio_block device = {0};
    struct physical_page_allocator allocator = {0};
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x74726976; registers[1] = version; registers[2] = 2;
    registers[0x10 / 4] = 1; registers[0x34 / 4] = 32; registers[0x100 / 4] = 16;
    clock_value = 0;

    assert(initialize_model(&device, registers, sizeof(registers),
           &allocator, dma_address, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    assert(begin_call(&device, 0));
    g_test_device = &device;

    struct block_request *r1 = try_reserve_request(&device, KERNEL_BLOCK_PRIO_LOW);
    assert(r1 != NULL);

    /* Attach sentinel waiters to slots and slots_low */
    struct kernel_wait_node sentinel_high = {0};
    struct kernel_wait_node sentinel_low = {0};

    /* Scenario A: slots.head != NULL -> release_request must wake slots but NOT slots_low */
    device.slots.head = &sentinel_high;
    device.slots_low.head = &sentinel_low;
    mock_slots_wakes = 0;
    mock_slots_low_wakes = 0;

    release_request(&device, r1);
    assert(mock_slots_wakes == 1);
    assert(mock_slots_low_wakes == 0); /* slots_low NOT woken because readers are queued */

    /* Scenario B: slots.head == NULL -> release_request wakes BOTH slots and slots_low */
    device.slots.head = NULL;
    struct block_request *r2 = try_reserve_request(&device, KERNEL_BLOCK_PRIO_LOW);
    assert(r2 != NULL);

    mock_slots_wakes = 0;
    mock_slots_low_wakes = 0;
    release_request(&device, r2);
    assert(mock_slots_wakes == 0); /* slots.head was NULL, so wake() skipped it */
    assert(mock_slots_low_wakes == 1); /* slots_low WAS woken */

    /* Reset heads */
    device.slots.head = NULL;
    device.slots_low.head = NULL;
    g_test_device = NULL;

    end_call(&device, 0);
    assert(destroy_model(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    assert(allocations == releases);
    printf("PASS Wait queue wake isolation (transport=%u)\n", version);
}

/* Adversarial Test 3: Free Descriptors Accounting & Queue Claimed Invariants */
static void test_descriptor_accounting_drift(unsigned version)
{
    struct riscv_virtio_mmio_block device = {0};
    struct physical_page_allocator allocator = {0};
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x74726976; registers[1] = version; registers[2] = 2;
    registers[0x10 / 4] = 1; registers[0x34 / 4] = 32; registers[0x100 / 4] = 16;
    clock_value = 0;

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

    /* Initial state checks */
    assert(device.queue.claimed == 0);
    assert(device.queue.inflight == 0);
    assert(free_descriptors(&device) == 32);

    /* Run 10,000 rounds of publish and harvest with varying depths */
    for (unsigned round = 0; round < 10000; round++) {
        assert(begin_call(&device, 0));
        unsigned batch_count = 1 + (round % 8);
        struct block_request *reqs[8];
        unsigned heads[8];

        for (unsigned i = 0; i < batch_count; i++) {
            enum kernel_block_priority p = (i % 3 == 0) ? KERNEL_BLOCK_PRIO_LOW :
                                           (i % 3 == 1) ? KERNEL_BLOCK_PRIO_NORMAL :
                                                          KERNEL_BLOCK_PRIO_HIGH;
            reqs[i] = reserve_request_prio(&device, p);
            assert(reqs[i] != NULL);
            assert(publish_for_test(&device, reqs[i], VIRTIO_BLOCK_REQUEST_IN,
                   i, UINT64_C(0x82000000) + i * 512, 512, 0) == KERNEL_BLOCK_STATUS_OK);

            /* Invariant check: free + claimed popcount == total queue size */
            unsigned free_desc = free_descriptors(&device);
            unsigned claimed_desc = popcount64(device.queue.claimed);
            assert(free_desc + claimed_desc == device.queue.size);
        }

        assert((uint16_t)(available[1] - popped) == batch_count);
        for (unsigned i = 0; i < batch_count; i++) {
            heads[i] = available[2 + popped++ % 32];
        }

        /* Complete requests and check accounting */
        for (unsigned i = 0; i < batch_count; i++) {
            unsigned head = heads[i];
            unsigned tail = descriptors[descriptors[head].next].next;
            *(unsigned char *)physical_pointer(descriptors[tail].address, 1) = 0;
            completion[pushed++ % 32] = (struct wire_completion){head, 513};
        }
        used[1] = pushed;

        block_irq(&device);
        assert(device.queue.inflight == 0);

        /* After harvest, all claimed bits for completed requests must be released */
        assert(device.queue.claimed == 0);
        assert(free_descriptors(&device) == device.queue.size);

        for (unsigned i = 0; i < batch_count; i++) {
            assert(wait_request(&device, reqs[i]) == KERNEL_BLOCK_STATUS_OK);
            release_request(&device, reqs[i]);
        }
        end_call(&device, 0);
    }

    assert(device.queue.claimed == 0);
    assert(device.queue.inflight == 0);
    assert(free_descriptors(&device) == 32);
    assert(destroy_model(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    assert(allocations == releases);
    printf("PASS 10,000-cycle descriptor accounting and zero-drift verification (transport=%u)\n", version);
}

#include "wait_boundary.h"
HOST_WAIT_BOUNDARY(, kernel_scheduler_block_current)

int main(void)
{
    printf("--- Running Milestone 2 VirtIO-blk Priority & Descriptor Stress Harness ---\n");
    for (unsigned version = 1; version <= 2; version++) {
        test_quota_and_anti_starvation(version);
        test_wait_queue_isolation(version);
        test_descriptor_accounting_drift(version);
    }
    printf("--- All Milestone 2 Stress Tests PASSED Successfully ---\n");
    return 0;
}
