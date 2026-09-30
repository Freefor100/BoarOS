#include <arch/riscv/context.h>
#include <arch/riscv/plic.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/virtio_mmio_rng.h>
#include <kernel/errno.h>
#include <kernel/random.h>
#include <stddef.h>

struct rng_descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
};
struct rng_available {
    uint16_t flags, index, ring;
};
struct rng_used {
    uint16_t flags, index;
    uint32_t id, length;
};
static uint32_t read_reg(struct riscv_virtio_mmio_rng *d, unsigned r)
{
    return *(volatile uint32_t *)(d->mmio + r);
}
static void write_reg(struct riscv_virtio_mmio_rng *d, unsigned r, uint32_t v)
{
    *(volatile uint32_t *)(d->mmio + r) = v;
}
static void barrier(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
static unsigned used_offset(struct riscv_virtio_mmio_rng *d)
{
    return d->version == 1 ? 4096 : 32;
}
static void *data(struct riscv_virtio_mmio_rng *d)
{
    return (uint8_t *)d->queue_memory + used_offset(d) + 64;
}
static int reset(struct riscv_virtio_mmio_rng *d)
{
    d->configured = 0;
    write_reg(d, 0x70, 0);
    barrier();
    /* 未确认 reset 时，设备仍可能写 DMA；保留真实 owner。 */
    if (read_reg(d, 0x70) != 0)
        return -KERNEL_EIO;
    d->active = d->configured = 0;
    return 0;
}
static int configure(struct riscv_virtio_mmio_rng *d)
{
    if (reset(d))
        return -KERNEL_EIO;
    kernel_random_erase(d->queue_memory, 4096U << d->order);
    d->available = d->consumed = 0;
    write_reg(d, 0x70, 1);
    write_reg(d, 0x70, 3);
    if (d->version == 2) {
        write_reg(d, 0x14, 1);
        if (!(read_reg(d, 0x10) & 1))
            return -KERNEL_ENOTSUP;
        write_reg(d, 0x24, 1);
        write_reg(d, 0x20, 1);
        write_reg(d, 0x24, 0);
        write_reg(d, 0x20, 0);
        write_reg(d, 0x70, 11);
        if (!(read_reg(d, 0x70) & 8))
            return -KERNEL_EIO;
    } else {
        write_reg(d, 0x20, 0);
        write_reg(d, 0x28, 4096);
    }
    write_reg(d, 0x30, 0);
    if (read_reg(d, 0x34) < 1)
        return -KERNEL_ENOTSUP;
    write_reg(d, 0x38, 1);
    if (d->version == 1) {
        if ((d->queue_phys >> 12) > UINT32_MAX)
            return -KERNEL_ENOTSUP;
        write_reg(d, 0x3c, 4096);
        write_reg(d, 0x40, (uint32_t)(d->queue_phys >> 12));
    } else {
        uint64_t p = d->queue_phys;
        write_reg(d, 0x80, (uint32_t)p);
        write_reg(d, 0x84, (uint32_t)(p >> 32));
        p += 16;
        write_reg(d, 0x90, (uint32_t)p);
        write_reg(d, 0x94, (uint32_t)(p >> 32));
        p = d->queue_phys + used_offset(d);
        write_reg(d, 0xa0, (uint32_t)p);
        write_reg(d, 0xa4, (uint32_t)(p >> 32));
        write_reg(d, 0x44, 1);
    }
    barrier();
    write_reg(d, 0x70, d->version == 2 ? 15 : 7);
    d->configured = 1;
    return 0;
}
static void interrupt(void *owner)
{
    struct riscv_virtio_mmio_rng *d = owner;
    uint32_t status = read_reg(d, 0x60);
    if (status)
        write_reg(d, 0x64, status);
    if (!d->active)
        return;
    volatile struct rng_used *used =
        (void *)((uint8_t *)d->queue_memory + used_offset(d));
    uint16_t index = used->index;
    barrier();
    if (index == d->consumed)
        return;
    uint32_t id = used->id, length = used->length;
    d->invalid = (uint16_t)(index - d->consumed) != 1 || id != 0 || length > 64;
    d->response_length = d->invalid ? 0 : length;
    d->consumed = index;
    d->completed = 1;
    d->active = 0;
    (void)kernel_wait_queue_wake_all(&d->progress);
}
static void submit(struct riscv_virtio_mmio_rng *d)
{
    struct rng_descriptor *descriptor = d->queue_memory;
    volatile struct rng_available *available =
        (void *)((uint8_t *)d->queue_memory + 16);
    descriptor->address = d->queue_phys + used_offset(d) + 64;
    descriptor->length = 64;
    descriptor->flags = 2;
    descriptor->next = 0;
    available->ring = 0;
    d->completed = d->invalid = 0;
    d->active = 1;
    barrier();
    available->index = ++d->available;
    barrier();
    write_reg(d, 0x50, 0);
    d->requests++;
}
static void wait_until(struct riscv_virtio_mmio_rng *d, uint64_t deadline)
{
    enum kernel_wait_wake_reason reason;
    while (!d->stopping && riscv_time_read() < deadline)
        if (kernel_scheduler_block_current(&d->progress, deadline, 0,
                                           &reason) !=
            KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
}
static void worker(void *owner)
{
    struct riscv_virtio_mmio_rng *d = owner;
    uintptr_t flags = riscv_interrupt_save();
    while (!d->stopping) {
        unsigned collected = 0;
        if (!d->configured && configure(d)) {
            d->errors++;
            (void)reset(d);
            wait_until(d, riscv_time_read() + 60 * d->frequency);
            continue;
        }
        while (!d->stopping && collected < 32) {
            submit(d);
            uint64_t deadline = riscv_time_read() + 5 * d->frequency;
            enum kernel_wait_wake_reason reason;
            while (!d->stopping && !d->completed &&
                   riscv_time_read() < deadline) {
                if (kernel_scheduler_block_current(&d->progress, deadline, 0,
                                                   &reason) !=
                    KERNEL_SCHEDULER_STATUS_OK)
                    __builtin_trap();
            }
            if (d->stopping)
                break;
            if (!d->completed) {
                d->timeouts++;
                (void)reset(d);
                break;
            }
            if (d->invalid) {
                d->errors++;
                (void)reset(d);
                break;
            }
            if (!d->response_length) {
                d->zero_responses++;
                (void)reset(d);
                break;
            }
            kernel_random_mix(data(d), d->response_length, 1);
            collected += d->response_length;
            d->bytes += d->response_length;
            kernel_random_erase(data(d), 64);
        }
        if (!d->stopping)
            wait_until(d, riscv_time_read() + 60 * d->frequency);
    }
    (void)reset(d);
    riscv_interrupt_restore(flags);
}
int riscv_virtio_mmio_rng_stop(struct riscv_virtio_mmio_rng *d)
{
    if (!d || !d->mmio)
        return 0;
    uintptr_t flags = riscv_interrupt_save();
    d->stopping = 1;
    if (d->started) {
        (void)kernel_wait_queue_wake_all(&d->progress);
        kernel_thread_join(&d->worker);
        d->started = 0;
    }
    if (reset(d)) {
        riscv_interrupt_restore(flags);
        return -KERNEL_EIO;
    }
    if (d->irq_registered) {
        riscv_plic_unregister(d->irq_source, d);
        d->irq_registered = 0;
    }
    if (d->queue_memory)
        kernel_random_erase(d->queue_memory, 4096U << d->order);
    if (d->queue_phys &&
        physical_page_release_order(d->allocator, d->queue_phys, d->order) !=
            PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    d->queue_memory = 0;
    d->queue_phys = 0;
    d->mmio = 0;
    riscv_interrupt_restore(flags);
    return 0;
}
int riscv_virtio_mmio_rng_start(struct riscv_virtio_mmio_rng *d,
                                volatile void *mmio, uint64_t size,
                                struct physical_page_allocator *allocator,
                                uint64_t frequency, uint32_t irq_source)
{
    if (!d || d->mmio || !mmio || size < 0x100 || !allocator || !frequency ||
        !irq_source)
        return -KERNEL_EINVAL;
    volatile uint32_t *r = mmio;
    if (r[0] != 0x74726976 || r[2] != 4 || (r[1] != 1 && r[1] != 2))
        return -KERNEL_ENODEV;
    d->mmio = mmio;
    d->allocator = allocator;
    d->frequency = frequency;
    d->irq_source = irq_source;
    d->version = r[1];
    d->order = d->version == 1 ? 1 : 0;
    d->stopping = 0;
    kernel_wait_queue_init(&d->progress);
    int error = -KERNEL_EIO;
    if (reset(d))
        return error;
    if (physical_page_allocate_order(allocator, d->order, &d->queue_phys) !=
        PHYSICAL_PAGE_STATUS_OK) {
        error = -KERNEL_ENOMEM;
        goto failed;
    }
    if (physical_page_resolve(allocator, d->queue_phys, &d->queue_memory) !=
        PHYSICAL_PAGE_STATUS_OK)
        goto failed;
    error = configure(d);
    if (error)
        goto failed;
    if (!riscv_plic_register(irq_source, interrupt, d)) {
        error = -KERNEL_EIO;
        goto failed;
    }
    d->irq_registered = 1;
    if (kernel_thread_create_joinable(worker, d, &d->worker) !=
        KERNEL_SCHEDULER_STATUS_OK) {
        error = -KERNEL_ENOMEM;
        goto failed;
    }
    d->started = 1;
    return 0;
failed:
    if (riscv_virtio_mmio_rng_stop(d))
        return -KERNEL_EIO;
    return error;
}
