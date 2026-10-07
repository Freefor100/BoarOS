/* Deterministic transport fixture; this does not measure entropy quality. */
#define _XOPEN_SOURCE 700
#include <arch/riscv/plic.h>
#include <arch/riscv/virtio_mmio_rng.h>
#include <kernel/virtio_rng.h>
#include <kernel/virtio_transport.h>
#include <kernel/page.h>
#include <kernel/errno.h>
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
static int pages, joined, create_fail, alloc_fail, irq_fail, credit, requests, generic, reset_stuck;
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
    dma = calloc(1, (size_t)BOAROS_PAGE_SIZE << o);
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
    reset_stuck=0;
}
static unsigned reg_offset(enum virtio_register reg)
{
    static const unsigned offsets[]={0x70,0x14,0x10,0x24,0x20,0x30,0x34,0x38,
        0x44,0x28,0x3c,0x40,0x50,0x80,0x84,0x90,0x94,0xa0,0xa4,0xfc};
    assert((unsigned)reg<sizeof(offsets)/sizeof(offsets[0]));return offsets[reg];
}
static uint32_t model_read(void *context,enum virtio_register reg)
{ assert(context==regs);return regs[reg_offset(reg)/4]; }
static void model_write(void *context,enum virtio_register reg,uint32_t value)
{
    assert(context==regs);
    if(reg==VIRTIO_REG_STATUS && !value && reset_stuck)return;
    if(reg==VIRTIO_REG_STATUS && !value) {
        regs[0x44/4]=0;regs[0x40/4]=0;
    }
    regs[reg_offset(reg)/4]=value;
}
static uint32_t model_ack(void *context)
{ assert(context==regs);uint32_t value=regs[0x60/4];regs[0x60/4]=0;return value; }
static int model_irq(void *context,uint32_t source,void (*handler)(void *),void *owner)
{ assert(context==regs);return riscv_plic_register(source,handler,owner); }
static void model_unregister(void *context,uint32_t source,void *owner)
{ assert(context==regs);riscv_plic_unregister(source,owner); }
static int start_model(struct riscv_virtio_mmio_rng *d,volatile void *mmio,uint64_t size,
    struct physical_page_allocator *allocator,uint64_t frequency,uint32_t source)
{
    if(!generic)return riscv_virtio_mmio_rng_start(d,mmio,size,allocator,frequency,source);
    struct virtio_transport t={.context=regs,.version=regs[1],.device_id=regs[2],
        .ops={.read=model_read,.write=model_write,.ack_interrupt=model_ack,
            .register_irq=model_irq,.unregister_irq=model_unregister}};
    return virtio_rng_start(d,&t,allocator,frequency,source);
}
static void complete(struct riscv_virtio_mmio_rng *d, unsigned len, unsigned id)
{
    unsigned off = d->version == 1 ? regs[0x3c/4] :
        (unsigned)(regs[0xa0/4]|((uint64_t)regs[0xa4/4]<<32))-0x81000000;
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
int main(int argc,char **argv)
{
    (void)argv;generic=argc>1;
    for (unsigned version = 1; version <= 2; version++) {
        struct riscv_virtio_mmio_rng d = {0};
        setup(version);
        assert(!start_model(&d, regs, sizeof(regs), &allocator,
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
        assert(!start_model(&d, regs, sizeof(regs), &allocator,
                                            100, 7));
        resume();
        complete(&d, 0, 0);
        assert(!credit && deadline == 6100 && d.zero_responses == 1);
        assert(!riscv_virtio_mmio_rng_stop(&d));
        assert(!pages);
        setup(version);
        memset(&d, 0, sizeof(d));
        assert(!start_model(&d, regs, sizeof(regs), &allocator,
                                            100, 7));
        resume();
        complete(&d, 65, 0);
        assert(!credit && deadline == 6100 && d.errors == 1);
        assert(!riscv_virtio_mmio_rng_stop(&d));
        setup(version);
        memset(&d, 0, sizeof(d));
        assert(!start_model(&d, regs, sizeof(regs), &allocator,
                                            100, 7));
        resume();
        complete(&d, 32, 1);
        assert(!credit && d.errors == 1 && deadline == 6100);
        assert(!riscv_virtio_mmio_rng_stop(&d));
        setup(version);
        memset(&d, 0, sizeof(d));
        assert(!start_model(&d, regs, sizeof(regs), &allocator,
                                            100, 7));
        resume();
        now = deadline;
        resume();
        assert(d.timeouts == 1 && !credit && deadline == now + 6000);
        assert(!riscv_virtio_mmio_rng_stop(&d));
        setup(version);
        memset(&d, 0, sizeof(d));
        assert(!start_model(&d, regs, sizeof(regs), &allocator,
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
            assert(start_model(&d, regs, sizeof(regs),
                                               &allocator, 100, 7) < 0);
            assert(!pages && !irq_fn && !d.transport.context);
        }
        setup(version);
        regs[2] = 2;
        memset(&d, 0, sizeof(d));
        assert(start_model(&d, regs, sizeof(regs), &allocator,
                                           100, 7) < 0);
        assert(!d.transport.context && !credit);
        if(generic) {
            setup(version);memset(&d,0,sizeof(d));
            assert(!start_model(&d,regs,sizeof(regs),&allocator,100,7));
            resume();reset_stuck=1;
            assert(riscv_virtio_mmio_rng_stop(&d)==-KERNEL_EIO);
            assert(joined && pages && dma && irq_fn && d.transport.context && d.queue.inflight==1 && !credit);
            reset_stuck=0;
            assert(!riscv_virtio_mmio_rng_stop(&d));
            assert(!pages && !irq_fn && !d.transport.context && !d.queue.inflight);
            /* Same owner storage can start again without stale ring pointers. */
            assert(!start_model(&d,regs,sizeof(regs),&allocator,100,7));
            resume();assert(!riscv_virtio_mmio_rng_stop(&d));
            setup(version);memset(&d,0,sizeof(d));regs[0x70/4]=3;reset_stuck=1;
            assert(start_model(&d,regs,sizeof(regs),&allocator,100,7)==-KERNEL_EIO);
            assert(d.transport.context && !pages && !irq_fn);
            assert(riscv_virtio_mmio_rng_stop(&d)==-KERNEL_EIO);
            reset_stuck=0;assert(!riscv_virtio_mmio_rng_stop(&d) && !d.transport.context);
        }
    }
    puts("virtio RNG transport lifecycle PASS");
    return 0;
}
