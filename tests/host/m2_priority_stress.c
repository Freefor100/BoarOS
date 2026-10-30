#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>

#include "virtio_mmio_block_native.c"

/* Architecture barrier and clock stubs for native host */
static uint64_t clock_value = 0;
uint64_t host_block_time(void)
{
    return ++clock_value;
}

static _Thread_local struct kernel_cpu block_cpu = {.initialized = KERNEL_CPU_INITIALIZED};
void *allocator_host_cpu_get(void) { return &block_cpu; }

void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *, struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock, struct kernel_raw_guard *guard)
{
    __real_kernel_raw_lock_acquire(lock, guard);
}

enum virtio_status __real_virtio_split_take(struct virtio_split_queue *, struct virtio_completion *);
enum virtio_status __wrap_virtio_split_take(struct virtio_split_queue *queue, struct virtio_completion *completion)
{
    return __real_virtio_split_take(queue, completion);
}

void virt_uart_putc(char c) { (void)c; }
void virt_uart_puts(const char *s) { (void)s; }
void virt_uart_put_hex(unsigned long v) { (void)v; }
void kernel_console_putc(char c) { (void)c; }
int kernel_scheduler_can_sleep(void) { return 1; }
int riscv_plic_in_interrupt(void) { return 0; }
int riscv_plic_register(uint32_t s, void (*h)(void *), void *o) { (void)s; (void)h; (void)o; return 1; }
void riscv_plic_unregister(uint32_t s, void *o) { (void)s; (void)o; }

/* Real multi-threaded wait queue support with pthread primitives */
struct thread_waiter {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    struct kernel_wait_node node;
    struct kernel_wait_queue *queue;
    int woken;
};

static _Thread_local struct thread_waiter tl_waiter;
static _Thread_local int tl_waiter_inited = 0;
static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;

static struct kernel_wait_queue *tracked_slots = NULL;
static struct kernel_wait_queue *tracked_slots_low = NULL;
static unsigned slots_wake_count = 0;
static unsigned slots_low_wake_count = 0;
static unsigned slots_wait_count = 0;
static unsigned slots_low_wait_count = 0;

static void init_tl_waiter(void)
{
    if (!tl_waiter_inited) {
        pthread_mutex_init(&tl_waiter.mutex, NULL);
        pthread_cond_init(&tl_waiter.cond, NULL);
        tl_waiter_inited = 1;
    }
}

enum kernel_scheduler_status kernel_wait_node_bind(struct kernel_wait_queue *queue,
    struct kernel_wait_node *node, const struct kernel_wait_token *token)
{
    (void)queue; (void)node; (void)token;
    return KERNEL_SCHEDULER_STATUS_OK;
}

void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{
    memset(queue, 0, sizeof(*queue));
    queue->initialized = KERNEL_WAIT_QUEUE_INITIALIZED;
}

enum kernel_scheduler_status kernel_wait_prepare(struct kernel_wait_queue *queue,
    uint64_t deadline, int interruptible, struct kernel_wait_token *token)
{
    (void)deadline; (void)interruptible;
    init_tl_waiter();
    pthread_mutex_lock(&tl_waiter.mutex);
    tl_waiter.queue = queue;
    tl_waiter.woken = 0;
    tl_waiter.node.queue = queue;
    tl_waiter.node.next = NULL;
    tl_waiter.node.previous = NULL;
    pthread_mutex_unlock(&tl_waiter.mutex);

    if (queue) {
        pthread_mutex_lock(&wait_lock);
        if (queue == tracked_slots) slots_wait_count++;
        if (queue == tracked_slots_low) slots_low_wait_count++;

        tl_waiter.node.previous = queue->tail;
        if (queue->tail) queue->tail->next = &tl_waiter.node;
        else queue->head = &tl_waiter.node;
        queue->tail = &tl_waiter.node;
        pthread_mutex_unlock(&wait_lock);
    }
    token->task = (void *)&tl_waiter;
    token->generation = 1;
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_wait_park(const struct kernel_wait_token *token,
    enum kernel_wait_wake_reason *reason)
{
    (void)token;
    init_tl_waiter();
    pthread_mutex_lock(&tl_waiter.mutex);
    while (!tl_waiter.woken) {
        pthread_cond_wait(&tl_waiter.cond, &tl_waiter.mutex);
    }
    pthread_mutex_unlock(&tl_waiter.mutex);
    if (reason) *reason = KERNEL_WAIT_WOKEN;
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_wait_finish(struct kernel_wait_token *token)
{
    (void)token;
    pthread_mutex_lock(&wait_lock);
    if (tl_waiter.queue) {
        struct kernel_wait_queue *q = tl_waiter.queue;
        if (tl_waiter.node.previous) tl_waiter.node.previous->next = tl_waiter.node.next;
        else q->head = tl_waiter.node.next;
        if (tl_waiter.node.next) tl_waiter.node.next->previous = tl_waiter.node.previous;
        else q->tail = tl_waiter.node.previous;
        tl_waiter.node.next = tl_waiter.node.previous = NULL;
        tl_waiter.queue = NULL;
    }
    pthread_mutex_unlock(&wait_lock);
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{
    if (!queue) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    pthread_mutex_lock(&wait_lock);
    if (queue == tracked_slots) slots_wake_count++;
    if (queue == tracked_slots_low) slots_low_wake_count++;

    struct kernel_wait_node *curr = queue->head;
    while (curr) {
        struct thread_waiter *w = (struct thread_waiter *)((char *)curr - offsetof(struct thread_waiter, node));
        pthread_mutex_lock(&w->mutex);
        w->woken = 1;
        pthread_cond_broadcast(&w->cond);
        pthread_mutex_unlock(&w->mutex);
        curr = curr->next;
    }
    pthread_mutex_unlock(&wait_lock);
    return KERNEL_SCHEDULER_STATUS_OK;
}

/* MMIO, split-ring and memory simulation */
static uint32_t registers[128];
static void *queue_allocation = NULL;
static size_t allocation_size = 0;
static void *workspace_allocation[VIRTIO_BLOCK_BATCH_POOL_PAGES];
static unsigned total_allocations = 0;
static unsigned total_releases = 0;
static unsigned char dma_buffers[16][4096] __attribute__((aligned(4096)));
static unsigned char disk_data[1024 * 1024];

struct wire_descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
} __attribute__((packed));
struct wire_completion { uint32_t id, length; };

static void *physical_pointer(uint64_t address, size_t size)
{
    if (address >= UINT64_C(0x82000000) && address < UINT64_C(0x83000000)) {
        uint64_t offset = address - UINT64_C(0x82000000);
        assert(offset + size <= sizeof(dma_buffers));
        return (unsigned char *)dma_buffers + offset;
    }
    if (address >= UINT64_C(0x81000000) && address < UINT64_C(0x82000000)) {
        uint64_t offset = address - UINT64_C(0x81000000);
        assert(queue_allocation && offset + size <= allocation_size);
        return (unsigned char *)queue_allocation + offset;
    }
    return (void *)(uintptr_t)address;
}

static uint64_t register_address(unsigned low)
{
    return registers[low / 4] | ((uint64_t)registers[(low + 4) / 4] << 32);
}

enum physical_page_status physical_page_allocate_order(struct physical_page_allocator *allocator,
                                                       uint32_t order, uint64_t *physical)
{
    (void)allocator;
    if (!order) {
        for (unsigned i = 0; i < VIRTIO_BLOCK_BATCH_POOL_PAGES; i++) {
            if (!workspace_allocation[i]) {
                workspace_allocation[i] = calloc(1, 4096);
                assert(workspace_allocation[i]);
                *physical = UINT64_C(0x83000000) + i * 4096;
                total_allocations++;
                return PHYSICAL_PAGE_STATUS_OK;
            }
        }
        return PHYSICAL_PAGE_STATUS_EMPTY;
    }
    assert(queue_allocation == NULL);
    allocation_size = 4096U << order;
    queue_allocation = calloc(1, allocation_size);
    assert(queue_allocation != NULL);
    *physical = UINT64_C(0x81000000);
    total_allocations++;
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
        total_releases++;
        return PHYSICAL_PAGE_STATUS_OK;
    }
    if (physical == UINT64_C(0x81000000)) {
        assert(queue_allocation != NULL);
        free(queue_allocation);
        queue_allocation = NULL;
        total_releases++;
        return PHYSICAL_PAGE_STATUS_OK;
    }
    return PHYSICAL_PAGE_STATUS_INVALID;
}

static int dma_address_fn(const void *pointer, uint64_t size, uint64_t *physical)
{
    (void)size;
    uintptr_t base = (uintptr_t)dma_buffers;
    uintptr_t ptr = (uintptr_t)pointer;
    if (ptr >= base && ptr < base + sizeof(dma_buffers)) {
        *physical = UINT64_C(0x82000000) + (ptr - base);
        return 1;
    }
    *physical = (uint64_t)ptr;
    return 1;
}

/* Simulated VirtIO Device Step */
static unsigned batch_popped = 0;
static unsigned batch_pushed = 0;
static int auto_complete_enabled = 1;
static struct riscv_virtio_mmio_block *active_test_device = NULL;

static void simulate_device_step(void)
{
    if (!queue_allocation) return;
    unsigned size = registers[0x38 / 4];
    uint64_t base = registers[1] == 1 ? (uint64_t)registers[0x40 / 4] << 12 : register_address(0x80);
    struct wire_descriptor *d = physical_pointer(base, size * 16);
    uint16_t *available = physical_pointer(registers[1] == 1 ? base + size * 16 : register_address(0x90), 4 + size * 2);
    uint16_t *used = physical_pointer(registers[1] == 1 ? base + 4096 : register_address(0xa0), 4 + size * 8);
    struct wire_completion *done = (void *)(used + 2);
    unsigned count = (uint16_t)(available[1] - batch_popped), first = batch_popped;
    if (!count) return;
    batch_popped += count;

    for (unsigned n = count; n; n--) {
        unsigned head = available[2 + (first + n - 1) % size], at = head;
        assert(head < size && d[head].flags == 1 && d[head].length == 16);
        unsigned char *header = physical_pointer(d[head].address, 16);
        uint32_t type; uint64_t sector;
        memcpy(&type, header, 4); memcpy(&sector, header + 8, 8);
        uint64_t offset = sector * 512;
        unsigned bytes = 0;

        while (d[at].flags & 1) {
            at = d[at].next;
            assert(at < size);
            if (!(d[at].flags & 1)) break;
            unsigned char *buf = physical_pointer(d[at].address, d[at].length);
            if (type == 0) {
                /* READ: load from disk_data */
                if (offset + d[at].length <= sizeof(disk_data))
                    memcpy(buf, disk_data + offset, d[at].length);
            } else if (type == 1) {
                /* WRITE: store to disk_data */
                if (offset + d[at].length <= sizeof(disk_data))
                    memcpy(disk_data + offset, buf, d[at].length);
            }
            offset += d[at].length;
            bytes += d[at].length;
        }
        assert(d[at].flags == 2 && d[at].length == 1);
        *(unsigned char *)physical_pointer(d[at].address, 1) = 0; /* STATUS_OK */
        done[batch_pushed++ % size] = (struct wire_completion){head, type == 0 ? bytes + 1 : 1};
    }
    used[1] = (uint16_t)batch_pushed;
    registers[0x60 / 4] = 1; /* Interrupt status */
}

static void (*orig_underlying_write)(void *, enum virtio_register, uint32_t);
static uint32_t (*orig_underlying_ack)(void *);

static void stress_observed_write(void *context, enum virtio_register reg, uint32_t value)
{
    orig_underlying_write(context, reg, value);
    if (reg == VIRTIO_REG_QUEUE_NOTIFY && auto_complete_enabled) {
        simulate_device_step();
    }
}

static uint32_t stress_observed_ack(void *context)
{
    return orig_underlying_ack(context);
}

static void setup_device(struct riscv_virtio_mmio_block *dev, unsigned version)
{
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x74726976;
    registers[1] = version;
    registers[2] = 2;
    registers[0x10 / 4] = 1 | (1 << 9); /* Split virtqueue + modern config */
    registers[0x34 / 4] = 32;          /* 32 descriptors in queue */
    registers[0x100 / 4] = sizeof(disk_data) / 512;

    batch_popped = batch_pushed = 0;
    auto_complete_enabled = 1;
    clock_value = 0;

    struct physical_page_allocator allocator = {0};
    assert(riscv_virtio_mmio_block_init(dev, registers, sizeof(registers),
        &allocator, dma_address_fn, 1000) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);

    orig_underlying_write = dev->transport.ops.write;
    dev->transport.ops.write = stress_observed_write;
    orig_underlying_ack = dev->transport.ops.ack_interrupt;
    dev->transport.ops.ack_interrupt = stress_observed_ack;

    assert(riscv_virtio_mmio_block_enable_irq(dev, 1));
    active_test_device = dev;
    tracked_slots = &dev->slots;
    tracked_slots_low = &dev->slots_low;
    slots_wake_count = slots_low_wake_count = 0;
    slots_wait_count = slots_low_wait_count = 0;
}

static void teardown_device(struct riscv_virtio_mmio_block *dev)
{
    assert(riscv_virtio_mmio_block_destroy(dev) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK);
    active_test_device = NULL;
    tracked_slots = tracked_slots_low = NULL;
    assert(total_allocations == total_releases);
}

/* Helper to count low-priority and total reserved slots */
static void get_slot_counts(struct virtio_block_device *dev, unsigned *low, unsigned *free, unsigned *total)
{
    *low = 0;
    *free = 0;
    *total = slot_count(dev);
    for (unsigned i = 0; i < *total; i++) {
        struct block_request *r = request_at(dev, i);
        if (r->state != 0) {
            if (r->priority == (uint8_t)KERNEL_BLOCK_PRIO_LOW) (*low)++;
        } else {
            (*free)++;
        }
    }
}

/* =========================================================================
 * TEST CASE 1: Low Priority Slot Quota Enforcement (Capped at 4)
 * ========================================================================= */
static void test_slot_quota_enforcement(void)
{
    printf("[TEST 1] Testing low-priority slot quota enforcement (max 4 of 8 slots)...\n");
    struct riscv_virtio_mmio_block dev = {0};
    setup_device(&dev, 2);
    assert(begin_call(&dev, 0));

    struct block_request *reqs[8] = {0};

    /* Reserve 4 low-priority requests */
    for (unsigned i = 0; i < 4; i++) {
        reqs[i] = try_reserve_request(&dev, KERNEL_BLOCK_PRIO_LOW);
        assert(reqs[i] != NULL);
        assert(reqs[i]->priority == (uint8_t)KERNEL_BLOCK_PRIO_LOW);
        assert(reqs[i]->state == 1);
    }

    unsigned low, free_slots, total;
    get_slot_counts(&dev, &low, &free_slots, &total);
    assert(total == 8);
    assert(low == 4);
    assert(free_slots == 4);

    /* Attempt 5th low-priority request: MUST be rejected by try_reserve_request */
    struct block_request *fifth_low = try_reserve_request(&dev, KERNEL_BLOCK_PRIO_LOW);
    assert(fifth_low == NULL && "5th low-priority writeback must NOT acquire a slot when low_count == 4");

    /* Verify slots 4..7 remain free and CAN be reserved by HIGH priority reads */
    for (unsigned i = 4; i < 8; i++) {
        reqs[i] = try_reserve_request(&dev, KERNEL_BLOCK_PRIO_HIGH);
        assert(reqs[i] != NULL && "High priority read must be able to acquire slots 4..7");
        assert(reqs[i]->priority == (uint8_t)KERNEL_BLOCK_PRIO_HIGH);
        assert(reqs[i]->state == 1);
    }

    get_slot_counts(&dev, &low, &free_slots, &total);
    assert(low == 4);
    assert(free_slots == 0);

    /* Release all 8 slots */
    for (unsigned i = 0; i < 8; i++) {
        release_request(&dev, reqs[i]);
    }
    end_call(&dev, 0);

    get_slot_counts(&dev, &low, &free_slots, &total);
    assert(low == 0 && free_slots == 8);
    teardown_device(&dev);
    printf("   -> PASSED: 4-slot low-priority cap strictly enforced while leaving 4 slots for reads.\n");
}

/* =========================================================================
 * TEST CASE 2 & 3: 5th Writeback Blocks on &slots_low, Read Preempts
 * ========================================================================= */
struct thread_arg {
    struct riscv_virtio_mmio_block *dev;
    volatile int started;
    volatile int acquired;
    volatile int release_signal;
    struct block_request *ret_request;
};

static void *wb_fifth_thread(void *arg)
{
    struct thread_arg *ctx = arg;
    __atomic_store_n(&ctx->started, 1, __ATOMIC_RELEASE);
    /* This call should block because low_count == 4 */
    struct block_request *r = reserve_request_prio(ctx->dev, KERNEL_BLOCK_PRIO_LOW);
    ctx->ret_request = r;
    __atomic_store_n(&ctx->acquired, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&ctx->release_signal, __ATOMIC_ACQUIRE)) {
        usleep(100);
    }
    release_request(ctx->dev, r);
    return NULL;
}

static void test_fifth_writeback_blocking_and_read_preemption(void)
{
    printf("[TEST 2 & 3] Testing 5th writeback blocking on &device->slots_low and concurrent read preemption...\n");
    struct riscv_virtio_mmio_block dev = {0};
    setup_device(&dev, 2);
    assert(begin_call(&dev, 0));

    struct block_request *low_reqs[4];
    for (unsigned i = 0; i < 4; i++) {
        low_reqs[i] = reserve_request_prio(&dev, KERNEL_BLOCK_PRIO_LOW);
        assert(low_reqs[i] != NULL);
        assert(low_reqs[i]->priority == (uint8_t)KERNEL_BLOCK_PRIO_LOW);
    }

    unsigned low, free_slots, total;
    get_slot_counts(&dev, &low, &free_slots, &total);
    assert(low == 4 && free_slots == 4);

    /* Launch 5th writeback thread */
    struct thread_arg arg = {.dev = &dev, .started = 0, .acquired = 0, .release_signal = 0, .ret_request = NULL};
    pthread_t th;
    assert(pthread_create(&th, NULL, wb_fifth_thread, &arg) == 0);

    while (!__atomic_load_n(&arg.started, __ATOMIC_ACQUIRE)) {
        usleep(100);
    }
    /* Give thread time to attempt reservation and enter wait_for_progress */
    usleep(20000);

    /* Verify the 5th writeback has NOT acquired a slot and is blocked on slots_low */
    assert(__atomic_load_n(&arg.acquired, __ATOMIC_ACQUIRE) == 0);
    assert(arg.ret_request == NULL);
    assert(dev.slots_low.head != NULL && "5th writeback must be queued on &device->slots_low");
    assert(dev.slots.head == NULL && "5th writeback must NOT be queued on &device->slots");
    assert(slots_low_wait_count >= 1);
    printf("   -> Sub-check: 5th writeback is confirmed blocked on &device->slots_low without acquiring a slot.\n");

    /* CONCURRENT READ PREEMPTION:
     * While the 5th writeback is blocked, a high-priority read attempts to acquire a slot. */
    unsigned read_wait_before = slots_wait_count;
    struct block_request *read_req = reserve_request_prio(&dev, KERNEL_BLOCK_PRIO_HIGH);
    assert(read_req != NULL && "Read request MUST immediately acquire a reserved slot");
    assert(read_req->priority == (uint8_t)KERNEL_BLOCK_PRIO_HIGH);
    assert(slots_wait_count == read_wait_before && "Read request MUST NOT wait on wait queue");

    /* Verify slot index is within the reserved upper range (slots 4..7) */
    unsigned slot_idx = (unsigned)((uintptr_t)read_req - (uintptr_t)request_at(&dev, 0)) / REQUEST_STRIDE;
    assert(slot_idx >= 4 && slot_idx < 8 && "Read request MUST be allocated from reserved slots 4..7");
    printf("   -> Sub-check: Concurrent read immediately acquired reserved slot %u without waiting.\n", slot_idx);

    /* Perform read transfer while writeback is still blocked */
    unsigned char read_buf[512];
    memset(read_buf, 0, sizeof(read_buf));
    memset(disk_data, 0x42, 512);

    /* Publish and wait for read completion */
    assert(publish_request_locked(&dev, read_req, 0, 0, (uint64_t)(uintptr_t)read_buf, 512, 0) == KERNEL_BLOCK_STATUS_OK);
    assert(wait_request(&dev, read_req) == KERNEL_BLOCK_STATUS_OK);
    assert(read_buf[0] == 0x42 && "Read payload must match disk data");
    release_request(&dev, read_req);

    /* Verify the 5th writeback is STILL blocked on slots_low after read release */
    assert(__atomic_load_n(&arg.acquired, __ATOMIC_ACQUIRE) == 0);
    assert(dev.slots_low.head != NULL);
    printf("   -> Sub-check: Read completed and released; 5th writeback remains correctly blocked.\n");

    /* CLEAN WAKEUP OF WRITEBACK:
     * Release 1 of the 4 held writeback slots. This should wake the blocked 5th writeback cleanly. */
    unsigned low_wakes_before = slots_low_wake_count;
    release_request(&dev, low_reqs[0]);
    low_reqs[0] = NULL;

    /* Wait for 5th writeback thread to wake and acquire slot */
    while (!__atomic_load_n(&arg.acquired, __ATOMIC_ACQUIRE)) {
        usleep(100);
    }
    assert(arg.ret_request != NULL);
    assert(arg.ret_request->priority == (uint8_t)KERNEL_BLOCK_PRIO_LOW);
    assert(slots_low_wake_count > low_wakes_before);
    printf("   -> Sub-check: Blocked writeback woke cleanly and acquired slot without deadlock.\n");

    /* Signal thread to release its request and join */
    __atomic_store_n(&arg.release_signal, 1, __ATOMIC_RELEASE);
    assert(pthread_join(th, NULL) == 0);

    /* Release remaining writeback requests */
    for (unsigned i = 1; i < 4; i++) {
        if (low_reqs[i]) release_request(&dev, low_reqs[i]);
    }
    end_call(&dev, 0);

    /* Assert zero descriptor leak, zero queue waiters */
    assert(dev.slots.head == NULL);
    assert(dev.slots_low.head == NULL);
    teardown_device(&dev);
    printf("   -> PASSED: 5th writeback blocking, reader preemption, and clean wakeups verified.\n");
}

/* =========================================================================
 * TEST CASE 4: Reader Anti-Starvation Invariant
 * ========================================================================= */
static void test_reader_anti_starvation(void)
{
    printf("[TEST 4] Testing reader anti-starvation invariant (low_count < cap yields to slots.head)...\n");
    struct riscv_virtio_mmio_block dev = {0};
    setup_device(&dev, 2);
    assert(begin_call(&dev, 0));

    /* Hold only 2 low-priority slots (low_count == 2 < low_cap of 4) */
    struct block_request *r0 = try_reserve_request(&dev, KERNEL_BLOCK_PRIO_LOW);
    struct block_request *r1 = try_reserve_request(&dev, KERNEL_BLOCK_PRIO_LOW);
    assert(r0 && r1);

    /* Under normal conditions, another low-priority request can be reserved */
    struct block_request *r2 = try_reserve_request(&dev, KERNEL_BLOCK_PRIO_LOW);
    assert(r2 != NULL);
    release_request(&dev, r2);

    /* Now simulate a reader queued on &device->slots */
    struct kernel_wait_node reader_sentinel = {0};
    dev.slots.head = &reader_sentinel;

    /* With a reader waiting on &device->slots, try_reserve_request(LOW) MUST yield (return NULL) */
    struct block_request *denied = try_reserve_request(&dev, KERNEL_BLOCK_PRIO_LOW);
    assert(denied == NULL && "Low priority request MUST yield when high priority reader is on &device->slots");

    /* High priority request MUST still succeed */
    struct block_request *hp = try_reserve_request(&dev, KERNEL_BLOCK_PRIO_HIGH);
    assert(hp != NULL && "High priority read MUST succeed even when reader is waiting");

    /* Remove sentinel */
    dev.slots.head = NULL;
    release_request(&dev, hp);
    release_request(&dev, r0);
    release_request(&dev, r1);
    end_call(&dev, 0);

    teardown_device(&dev);
    printf("   -> PASSED: Reader anti-starvation correctly prevents writeback stealing from waiting reader.\n");
}

/* =========================================================================
 * TEST CASE 5: Full Saturation & Priority Inversion Stress Test
 * ========================================================================= */
static volatile int full_sat_stage = 0;

static void *full_sat_wb_thread(void *arg)
{
    struct riscv_virtio_mmio_block *dev = arg;
    /* Blocks on slots_low */
    struct block_request *r = reserve_request_prio(dev, KERNEL_BLOCK_PRIO_LOW);
    assert(r != NULL);
    __atomic_store_n(&full_sat_stage, 2, __ATOMIC_RELEASE);
    release_request(dev, r);
    return NULL;
}

static void *full_sat_rd_thread(void *arg)
{
    struct riscv_virtio_mmio_block *dev = arg;
    /* Blocks on slots */
    struct block_request *r = reserve_request_prio(dev, KERNEL_BLOCK_PRIO_HIGH);
    assert(r != NULL);
    /* Reader MUST wake before writeback */
    assert(__atomic_load_n(&full_sat_stage, __ATOMIC_ACQUIRE) == 0 &&
           "High priority reader MUST wake up before low priority writeback!");
    __atomic_store_n(&full_sat_stage, 1, __ATOMIC_RELEASE);
    release_request(dev, r);
    return NULL;
}

static void test_full_saturation_priority_order(void)
{
    printf("[TEST 5] Testing full saturation and priority wake ordering (Reader vs Writeback)...\n");
    struct riscv_virtio_mmio_block dev = {0};
    setup_device(&dev, 2);
    assert(begin_call(&dev, 0));

    /* Hold 4 writeback slots and 4 read slots (all 8 slots occupied) */
    struct block_request *wb[4];
    struct block_request *rd[4];
    for (unsigned i = 0; i < 4; i++) wb[i] = reserve_request_prio(&dev, KERNEL_BLOCK_PRIO_LOW);
    for (unsigned i = 0; i < 4; i++) rd[i] = reserve_request_prio(&dev, KERNEL_BLOCK_PRIO_HIGH);

    unsigned low, free_slots, total;
    get_slot_counts(&dev, &low, &free_slots, &total);
    assert(low == 4 && free_slots == 0);

    full_sat_stage = 0;
    pthread_t th_wb, th_rd;
    assert(pthread_create(&th_wb, NULL, full_sat_wb_thread, &dev) == 0);
    assert(pthread_create(&th_rd, NULL, full_sat_rd_thread, &dev) == 0);

    /* Wait for both threads to block */
    usleep(30000);
    assert(dev.slots_low.head != NULL);
    assert(dev.slots.head != NULL);

    /* Release ONE writeback slot */
    release_request(&dev, wb[0]);

    /* Join reader first - it MUST finish stage 1 */
    assert(pthread_join(th_rd, NULL) == 0);
    assert(full_sat_stage >= 1);

    /* Release one more slot to let writeback thread finish */
    release_request(&dev, wb[1]);
    assert(pthread_join(th_wb, NULL) == 0);
    assert(full_sat_stage == 2);

    /* Cleanup remaining slots */
    release_request(&dev, wb[2]);
    release_request(&dev, wb[3]);
    for (unsigned i = 0; i < 4; i++) release_request(&dev, rd[i]);
    end_call(&dev, 0);

    teardown_device(&dev);
    printf("   -> PASSED: High-priority reader strictly took precedence over blocked writeback.\n");
}

/* =========================================================================
 * TEST CASE 6: Multi-Threaded Stress Harness (Race & Leak Detection)
 * ========================================================================= */
#define NUM_WORKERS 8
#define ITERATIONS_PER_WORKER 200

struct stress_worker_arg {
    struct riscv_virtio_mmio_block *dev;
    int worker_id;
    int is_writeback;
    unsigned completed_ops;
};

static void *stress_worker_thread(void *arg)
{
    struct stress_worker_arg *ctx = arg;
    enum kernel_block_priority prio = ctx->is_writeback ?
        KERNEL_BLOCK_PRIO_LOW : KERNEL_BLOCK_PRIO_HIGH;

    for (unsigned i = 0; i < ITERATIONS_PER_WORKER; i++) {
        assert(begin_call(ctx->dev, 0));
        struct block_request *r = reserve_request_prio(ctx->dev, prio);
        assert(r != NULL);
        assert(r->state == 1);

        /* Simulate brief I/O transfer */
        unsigned char buf[512];
        uint32_t type = ctx->is_writeback ? 1 : 0;
        uint64_t sector = (ctx->worker_id * 100 + i) % 1024;
        assert(submit_request(ctx->dev, r, type, sector, (uint64_t)(uintptr_t)buf, 512, 0) == KERNEL_BLOCK_STATUS_OK);

        release_request(ctx->dev, r);
        end_call(ctx->dev, 0);
        ctx->completed_ops++;
    }
    return NULL;
}

static void test_multithreaded_stress(void)
{
    printf("[TEST 6] Stress testing concurrent mixed read/writeback (8 threads, 1600 operations)...\n");
    struct riscv_virtio_mmio_block dev = {0};
    setup_device(&dev, 2);

    pthread_t threads[NUM_WORKERS];
    struct stress_worker_arg args[NUM_WORKERS];

    for (int i = 0; i < NUM_WORKERS; i++) {
        args[i].dev = &dev;
        args[i].worker_id = i;
        args[i].is_writeback = (i % 2 == 0); /* 4 writeback threads, 4 reader threads */
        args[i].completed_ops = 0;
        assert(pthread_create(&threads[i], NULL, stress_worker_thread, &args[i]) == 0);
    }

    for (int i = 0; i < NUM_WORKERS; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
        assert(args[i].completed_ops == ITERATIONS_PER_WORKER);
    }

    unsigned low, free_slots, total;
    get_slot_counts(&dev, &low, &free_slots, &total);
    assert(low == 0 && free_slots == 8);
    assert(dev.slots.head == NULL);
    assert(dev.slots_low.head == NULL);
    assert(!dev.queue.claimed);
    assert(!dev.queue.inflight);

    teardown_device(&dev);
    printf("   -> PASSED: 1600 concurrent operations completed with zero deadlocks and zero descriptor leaks.\n");
}

int main(void)
{
    printf("=================================================================\n");
    printf("Milestone 2 Empirical VirtIO Priority & Slot Quota Stress Harness\n");
    printf("=================================================================\n");

    test_slot_quota_enforcement();
    test_fifth_writeback_blocking_and_read_preemption();
    test_reader_anti_starvation();
    test_full_saturation_priority_order();
    test_multithreaded_stress();

    printf("=================================================================\n");
    printf("ALL EMPIRICAL TESTS PASSED SUCCESSFULLY! VERDICT: CONFIRM\n");
    printf("=================================================================\n");
    return 0;
}
