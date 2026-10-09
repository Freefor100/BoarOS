#include <arch/context.h>
#include <arch/bus.h>
#include <arch/timer.h>
#include <kernel/virtio_rng.h>
#include <kernel/page.h>
#include <kernel/errno.h>
#include <kernel/random.h>
#include <stddef.h>

static unsigned used_offset(struct virtio_rng_device *d)
{
    return d->version == 1 ? BOAROS_PAGE_SIZE : 32;
}
static void *data(struct virtio_rng_device *d)
{
    return (uint8_t *)d->queue_memory + used_offset(d) + 64;
}
static int reset(struct virtio_rng_device *d)
{
    d->configured = 0;
    /* 未确认reset时，descriptor与业务DMA页仍属于本实例。 */
    if (virtio_transport_reset(&d->transport) != VIRTIO_OK) return -KERNEL_EIO;
    if (d->queue.size && virtio_split_reset(&d->queue, &d->transport) != VIRTIO_OK) __builtin_trap();
    d->active = 0;
    return 0;
}
static int configure(struct virtio_rng_device *d)
{
    if (reset(d)) return -KERNEL_EIO;
    kernel_random_erase(d->queue_memory, (size_t)BOAROS_PAGE_SIZE << d->order);
    enum virtio_status status=virtio_transport_begin(&d->transport,0,0,0);
    if (status != VIRTIO_OK) return status==VIRTIO_UNSUPPORTED ? -KERNEL_ENOTSUP : -KERNEL_EIO;
    if (virtio_split_initialize(&d->queue,d->queue_memory,(size_t)BOAROS_PAGE_SIZE<<d->order,
        1,16,used_offset(d)) != VIRTIO_OK) __builtin_trap();
    status=virtio_transport_queue(&d->transport,0,1,d->queue_phys,16,used_offset(d),BOAROS_PAGE_SIZE);
    if (status != VIRTIO_OK) return status==VIRTIO_UNSUPPORTED ? -KERNEL_ENOTSUP : -KERNEL_EIO;
    if (virtio_transport_start(&d->transport) != VIRTIO_OK) return -KERNEL_EIO;
    d->configured=1;
    return 0;
}
static void interrupt(void *owner)
{
    struct virtio_rng_device *d=owner;
    uint32_t pending=d->transport.ops.ack_interrupt(d->transport.context);
    if (pending) arch_io_barrier();
    if (!d->active) return;
    struct virtio_completion done;
    enum virtio_status status=virtio_split_take(&d->queue,&done);
    if (status==VIRTIO_EMPTY) return;
    if (status==VIRTIO_OK && (done.token!=d || done.head)) __builtin_trap();
    d->invalid=status!=VIRTIO_OK;
    d->response_length=d->invalid ? 0 : done.length;
    d->completed=1;d->active=0;
    (void)kernel_wait_queue_wake_all(&d->progress);
}
static void submit(struct virtio_rng_device *d)
{
    d->queue.descriptors[0]=(struct virtio_descriptor){
        d->queue_phys+used_offset(d)+64,64,VIRTIO_DESC_WRITE,0};
    d->completed=d->invalid=0;d->active=1;
    if (virtio_split_publish(&d->queue,&d->transport,0,0,d,0,64)!=VIRTIO_OK) __builtin_trap();
    d->requests++;
}

static void wait_until(struct virtio_rng_device *d, uint64_t deadline)
{
    enum kernel_wait_wake_reason reason;
    while (!d->stopping && arch_time_read() < deadline)
        if (KERNEL_WAIT_RECHECK(&d->progress, deadline, 0,
                                           &reason,
                (!d->stopping && arch_time_read() < deadline)) !=
            KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
}
static void worker(void *owner)
{
    struct virtio_rng_device *d = owner;
    uintptr_t flags = arch_interrupt_save();
    while (!d->stopping) {
        unsigned collected = 0;
        if (!d->configured && configure(d)) {
            d->errors++;
            (void)reset(d);
            wait_until(d, arch_time_read() + 60 * d->frequency);
            continue;
        }
        while (!d->stopping && collected < 32) {
            submit(d);
            uint64_t deadline = arch_time_read() + 5 * d->frequency;
            enum kernel_wait_wake_reason reason;
            while (!d->stopping && !d->completed &&
                   arch_time_read() < deadline) {
                if (KERNEL_WAIT_RECHECK(&d->progress, deadline, 0,
                                                   &reason,
                (!d->stopping && !d->completed && arch_time_read() < deadline)) !=
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
            wait_until(d, arch_time_read() + 60 * d->frequency);
    }
    (void)reset(d);
    arch_interrupt_restore(flags);
}
int virtio_rng_stop(struct virtio_rng_device *d)
{
    if (!d || !d->transport.context)
        return 0;
    uintptr_t flags = arch_interrupt_save();
    d->stopping = 1;
    if (d->started) {
        (void)kernel_wait_queue_wake_all(&d->progress);
        kernel_thread_join(&d->worker);
        d->started = 0;
    }
    if (reset(d)) {
        arch_interrupt_restore(flags);
        return -KERNEL_EIO;
    }
    if (d->irq_registered) {
        d->transport.ops.unregister_irq(d->transport.context,d->irq_source,d);
        d->irq_registered = 0;
    }
    if (d->queue_memory)
        kernel_random_erase(d->queue_memory, (size_t)BOAROS_PAGE_SIZE << d->order);
    if (d->queue_phys &&
        physical_page_release_order(d->allocator, d->queue_phys, d->order) !=
            PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    d->queue_memory = 0;
    d->queue_phys = 0;
    d->transport.context = 0;
    d->queue=(struct virtio_split_queue){0};
    arch_interrupt_restore(flags);
    return 0;
}
int virtio_rng_start(struct virtio_rng_device *d,const struct virtio_transport *transport,
    struct physical_page_allocator *allocator,uint64_t frequency,uint32_t irq_source)
{
    if (!d || d->transport.context || !transport || !transport->context || !allocator ||
        !frequency || frequency>UINT64_MAX/60 || !irq_source || !transport->ops.read ||
        !transport->ops.write || !transport->ops.ack_interrupt || !transport->ops.register_irq ||
        !transport->ops.unregister_irq) return -KERNEL_EINVAL;
    if (transport->device_id!=4 || (transport->version!=1 && transport->version!=2)) return -KERNEL_ENODEV;
    d->transport=*transport;
    d->allocator = allocator;
    d->frequency = frequency;
    d->irq_source = irq_source;
    d->version = transport->version;
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
    if (!d->transport.ops.register_irq(d->transport.context,irq_source,interrupt,d)) {
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
    if (virtio_rng_stop(d))
        return -KERNEL_EIO;
    return error;
}
