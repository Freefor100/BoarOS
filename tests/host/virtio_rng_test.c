/* Deterministic transport fixture; this does not measure entropy quality. */
#define _XOPEN_SOURCE 700
#include <arch/riscv/plic.h>
#include <arch/riscv/virtio_mmio_rng.h>
#include <assert.h>
#include <kernel/random.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
extern void *calloc(size_t, size_t);
extern void free(void *);
static ucontext_t controller, worker_context;
static unsigned char stack[65536];
static void (*entry_fn)(void *), (*irq_fn)(void *);
static void *entry_arg, *irq_arg, *dma;
static uint64_t now, deadline;
static int pages, joined, create_fail, alloc_fail, irq_fail, credit, requests;
static uint32_t regs[128];
static void worker_entry(void)
{
    entry_fn(entry_arg);
    joined = 1;
}
static void resume(void)
{
    assert(!joined);
    swapcontext(&controller, &worker_context);
}
uint64_t riscv_time_read(void)
{
    return now;
}
void kernel_wait_queue_init(struct kernel_wait_queue *q)
{
    memset(q, 0, sizeof(*q));
}
enum kernel_scheduler_status
kernel_wait_queue_wake_all(struct kernel_wait_queue *q)
{
    (void)q;
    return 0;
}
enum kernel_scheduler_status
kernel_scheduler_block_current(struct kernel_wait_queue *q, uint64_t d, int i,
                               enum kernel_wait_wake_reason *r)
{
    (void)q;
    (void)i;
    assert(r);
    deadline = d;
    swapcontext(&worker_context, &controller);
    return 0;
}
enum kernel_scheduler_status
kernel_thread_create_joinable(void (*f)(void *), void *arg,
                              struct kernel_thread_join *j)
{
    if (create_fail)
        return KERNEL_SCHEDULER_STATUS_NO_MEMORY;
    entry_fn = f;
    entry_arg = arg;
    joined = 0;
    j->task = (void *)1;
    getcontext(&worker_context);
    worker_context.uc_stack.ss_sp = stack;
    worker_context.uc_stack.ss_size = sizeof(stack);
    worker_context.uc_link = &controller;
    makecontext(&worker_context, worker_entry, 0);
    return 0;
}
void kernel_thread_join(struct kernel_thread_join *j)
{
    while (!joined)
        resume();
    j->task = 0;
}
int riscv_plic_register(uint32_t s, void (*f)(void *), void *arg)
{
    (void)s;
    if (irq_fail)
        return 0;
    irq_fn = f;
    irq_arg = arg;
    return 1;
}
void riscv_plic_unregister(uint32_t s, void *arg)
{
    (void)s;
    assert(arg == irq_arg);
    irq_fn = 0;
}
void kernel_random_mix(const void *p, size_t n, int trusted)
{
    assert(p && n <= 64 && trusted);
    credit += (int)n;
}
int kernel_random_ready(void)
{
    return credit >= 32;
}
void kernel_random_erase(void *p, size_t n)
{
    memset(p, 0, n);
}
enum physical_page_status
physical_page_allocate_order(struct physical_page_allocator *a, uint32_t o,
                             uint64_t *p)
{
    (void)a;
    if (alloc_fail)
        return PHYSICAL_PAGE_STATUS_EMPTY;
    dma = calloc(1, 4096U << o);
    assert(dma);
    *p = 0x81000000;
    pages = 1 << o;
    return 0;
}
enum physical_page_status
physical_page_resolve(const struct physical_page_allocator *a, uint64_t p,
                      void **out)
{
    (void)a;
    (void)p;
    *out = dma;
    return 0;
}
enum physical_page_status
physical_page_release_order(struct physical_page_allocator *a, uint64_t p,
                            uint32_t o)
{
    (void)a;
    (void)o;
    assert(regs[0x70 / 4] == 0);
    assert(p == 0x81000000);
    free(dma);
    dma = 0;
    pages = 0;
    return 0;
}
static struct physical_page_allocator allocator;
static void setup(unsigned version)
{
    memset(regs, 0, sizeof(regs));
    regs[0] = 0x74726976;
    regs[1] = version;
    regs[2] = 4;
    regs[0x34 / 4] = 1;
    regs[0x10 / 4] = 1;
    now = 100;
    deadline = 0;
    credit = 0;
    requests = 0;
    create_fail = alloc_fail = irq_fail = 0;
}
static void complete(struct riscv_virtio_mmio_rng *d, unsigned len, unsigned id)
{
    unsigned off = d->version == 1 ? 4096 : 32;
    uint16_t *used = (void *)((char *)dma + off);
    uint32_t *elem = (void *)(used + 2);
    elem[0] = id;
    elem[1] = len;
    used[1]++;
    regs[0x60 / 4] = 1;
    irq_fn(irq_arg);
    requests++;
    resume();
}
int main(void)
{
    for (unsigned version = 1; version <= 2; version++) {
        struct riscv_virtio_mmio_rng d = {0};
        setup(version);
        assert(!riscv_virtio_mmio_rng_start(&d, regs, sizeof(regs), &allocator,
                                            100, 7));
        resume();
        assert(deadline == 600);
        complete(&d, 16, 0);
        assert(credit == 16 && deadline == 600);
        complete(&d, 16, 0);
        assert(credit == 32 && deadline == 6100);
        now = deadline;
        resume();
        assert(d.requests == 3 && deadline == 6600);
        complete(&d, 0, 0);
        assert(kernel_random_ready() && deadline == 12100);
        assert(!riscv_virtio_mmio_rng_stop(&d));
        assert(!pages && !irq_fn && joined);
        setup(version);
        memset(&d, 0, sizeof(d));
        assert(!riscv_virtio_mmio_rng_start(&d, regs, sizeof(regs), &allocator,
                                            100, 7));
        resume();
        complete(&d, 0, 0);
        assert(!credit && deadline == 6100 && d.zero_responses == 1);
        assert(!riscv_virtio_mmio_rng_stop(&d));
        assert(!pages);
        setup(version);
        memset(&d, 0, sizeof(d));
        assert(!riscv_virtio_mmio_rng_start(&d, regs, sizeof(regs), &allocator,
                                            100, 7));
        resume();
        complete(&d, 65, 0);
        assert(!credit && deadline == 6100 && d.errors == 1);
        assert(!riscv_virtio_mmio_rng_stop(&d));
        setup(version);
        memset(&d, 0, sizeof(d));
        assert(!riscv_virtio_mmio_rng_start(&d, regs, sizeof(regs), &allocator,
                                            100, 7));
        resume();
        complete(&d, 32, 1);
        assert(!credit && d.errors == 1 && deadline == 6100);
        assert(!riscv_virtio_mmio_rng_stop(&d));
        setup(version);
        memset(&d, 0, sizeof(d));
        assert(!riscv_virtio_mmio_rng_start(&d, regs, sizeof(regs), &allocator,
                                            100, 7));
        resume();
        now = deadline;
        resume();
        assert(d.timeouts == 1 && !credit && deadline == now + 6000);
        assert(!riscv_virtio_mmio_rng_stop(&d));
        setup(version);
        memset(&d, 0, sizeof(d));
        assert(!riscv_virtio_mmio_rng_start(&d, regs, sizeof(regs), &allocator,
                                            100, 7));
        resume();
        assert(!riscv_virtio_mmio_rng_stop(&d));
        assert(!credit && !pages);
        for (int failure = 0; failure < 3; failure++) {
            setup(version);
            memset(&d, 0, sizeof(d));
            alloc_fail = failure == 0;
            irq_fail = failure == 1;
            create_fail = failure == 2;
            assert(riscv_virtio_mmio_rng_start(&d, regs, sizeof(regs),
                                               &allocator, 100, 7) < 0);
            assert(!pages && !irq_fn && !d.mmio);
        }
        setup(version);
        regs[2] = 2;
        memset(&d, 0, sizeof(d));
        assert(riscv_virtio_mmio_rng_start(&d, regs, sizeof(regs), &allocator,
                                           100, 7) < 0);
        assert(!d.mmio && !credit);
    }
    puts("virtio RNG transport lifecycle PASS");
    return 0;
}
