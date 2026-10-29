#include <kernel/block.h>
#include <kernel/accounting.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <arch/context.h>
#include <stddef.h>
#include <stdint.h>

static struct kernel_block_device *physical(struct kernel_block_device *device)
{ return device->parent ? device->parent : device; }

static uint64_t capacity(const struct kernel_block_device *device)
{ return __atomic_load_n(&device->capacity_bytes, __ATOMIC_ACQUIRE); }
/* Registry updates never sleep. IRQ masking also excludes same-hart preemption;
 * the atomic word protects dynamic device publication across harts. */
static unsigned char registry_busy;
static uintptr_t registry_lock(void)
{
    uintptr_t state = arch_interrupt_save();
    while (__atomic_test_and_set(&registry_busy, __ATOMIC_ACQUIRE)) {}
    return state;
}
static void registry_unlock(uintptr_t state)
{
    __atomic_clear(&registry_busy, __ATOMIC_RELEASE);
    arch_interrupt_restore(state);
}
static void prepare_locked(struct kernel_block_device *device)
{
    if (!device->io_initialized) {
        kernel_rwlock_init(&device->phase_barrier, 50, (uintptr_t)device);
        kernel_rwlock_init(&device->transfer_lock, 51, (uintptr_t)device);
        device->soft_block_size = device->logical_block_size;
        device->io_initialized = 1;
    }
}
void kernel_block_get_device(struct kernel_block_device *device)
{
    uintptr_t irq = registry_lock();
    if (!device || device->references == UINT32_MAX) __builtin_trap();
    prepare_locked(device);
    device->references++;
    registry_unlock(irq);
}
static int begin_notification_locked(struct kernel_block_device *device)
{
    if (!device->owners_changed) return 0;
    if (device->notifications == UINT32_MAX) __builtin_trap();
    device->notifications++;
    return 1;
}
static void finish_notification(struct kernel_block_device *device, int notify)
{
    if (!notify) return;
    device->owners_changed(device);
    uintptr_t irq = registry_lock();
    if (!device->notifications) __builtin_trap();
    device->notifications--;
    registry_unlock(irq);
}
void kernel_block_put(struct kernel_block_device *device)
{
    uintptr_t irq = registry_lock();
    if (!device || !device->references) __builtin_trap();
    device->references--;
    int notify = begin_notification_locked(device);
    registry_unlock(irq);
    finish_notification(device, notify);
}
void kernel_block_set_pressure_parent(struct kernel_block_device *device,struct kernel_block_device *parent)
{
    if (!device || !device->virtual_backing || device->parent) __builtin_trap();
    if (parent) parent=physical(parent);
    uintptr_t irq=registry_lock();
    struct kernel_block_device *old=device->pressure_parent;
    if (parent) {
        if (parent==device || parent->references==UINT32_MAX) __builtin_trap();
        prepare_locked(parent);parent->references++;
    }
    device->pressure_parent=parent;
    registry_unlock(irq);
    if (old) kernel_block_put(old);
}
struct kernel_block_device *kernel_block_pressure_device(struct kernel_block_device *device)
{
    if (!device) __builtin_trap();
    uintptr_t irq=registry_lock();
    struct kernel_block_device *target=physical(device);
    while (target && target->virtual_backing) target=target->pressure_parent;
    if (target) {
        if (target->references==UINT32_MAX) __builtin_trap();
        prepare_locked(target);target->references++;
    }
    registry_unlock(irq);return target;
}
static void block_request_put(struct kernel_block_device **device)
{ kernel_block_put(*device); }
#define BLOCK_REQUEST(device) \
    kernel_block_get_device(device); \
    struct kernel_block_device *request_owner __attribute__((cleanup(block_request_put))) = device

enum kernel_block_status kernel_block_flush(struct kernel_block_device *device)
{
    if (device == 0 || device->logical_block_size == 0U) {
        return KERNEL_BLOCK_STATUS_INVALID;
    }
    BLOCK_REQUEST(device);
    struct kernel_block_device *disk = physical(device);
    KERNEL_LOCK_SCOPE(io); kernel_rwlock_write(&disk->phase_barrier, &io);
    KERNEL_LOCK_SCOPE(transfer); kernel_rwlock_write(&disk->transfer_lock, &transfer);
    if (disk->flush != 0) {
        return disk->flush(disk->context);
    }
    return disk->cache_mode == KERNEL_BLOCK_CACHE_WRITETHROUGH
               ? KERNEL_BLOCK_STATUS_OK : KERNEL_BLOCK_STATUS_UNSUPPORTED;
}

enum kernel_block_status kernel_block_read_at(
    struct kernel_block_device *device,
    uint64_t offset,
    void *buffer,
    size_t size)
{
    if (device == 0 || physical(device)->read == 0 ||
        device->logical_block_size == 0U) {
        return KERNEL_BLOCK_STATUS_INVALID;
    }
    BLOCK_REQUEST(device);
    struct kernel_block_device *disk = physical(device);
    KERNEL_LOCK_SCOPE(transfer); kernel_rwlock_read(&disk->transfer_lock, &transfer);
    const uint64_t limit = capacity(device);
    if (offset > limit ||
        (uint64_t)size > limit - offset) {
        return KERNEL_BLOCK_STATUS_OUT_OF_RANGE;
    }
    if (size == 0U) {
        return KERNEL_BLOCK_STATUS_OK;
    }
    if (buffer == 0) {
        return KERNEL_BLOCK_STATUS_INVALID;
    }

    kernel_task_account_read(size);
    return disk->read(disk->context, device->start_bytes + offset, buffer, size);
}

enum kernel_block_status kernel_block_write_at(
    struct kernel_block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t size)
{
    if (device == 0 || physical(device)->write == 0 ||
        device->logical_block_size == 0U) {
        return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    }
    BLOCK_REQUEST(device);
    struct kernel_block_device *disk = physical(device);
    KERNEL_LOCK_SCOPE(transfer); kernel_rwlock_write(&disk->transfer_lock, &transfer);
    const uint64_t limit = capacity(device);
    if (offset > limit ||
        (uint64_t)size > limit - offset) {
        return KERNEL_BLOCK_STATUS_OUT_OF_RANGE;
    }
    if (size == 0U) {
        return KERNEL_BLOCK_STATUS_OK;
    }
    if (buffer == 0) {
        return KERNEL_BLOCK_STATUS_INVALID;
    }

    if (kernel_block_read_only(device)) return KERNEL_BLOCK_STATUS_READ_ONLY;
    return disk->write(disk->context, device->start_bytes + offset, buffer, size);
}

/* 分区转换单独使用本次请求大小的临时向量；整盘与单页缺页读取
 * 不再承担最大批量的栈空间。调用barrier及request owner覆盖驱动完成。 */
static __attribute__((noinline)) enum kernel_block_status partition_write_batch(
    struct kernel_block_device *device, const struct kernel_block_span *spans, size_t count)
{
    struct kernel_block_span translated[count];
    for (size_t i = 0; i < count; i++) {
        translated[i] = spans[i]; translated[i].offset += device->start_bytes;
    }
    struct kernel_block_device *disk = physical(device);
    return disk->write_batch(disk->context, translated, count);
}

enum kernel_block_status kernel_block_write_batch(
    struct kernel_block_device *device,
    const struct kernel_block_span *spans,
    size_t count)
{
    if (!device || !device->logical_block_size)
        return KERNEL_BLOCK_STATUS_INVALID;
    if (!physical(device)->write && !physical(device)->write_batch)
        return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    if (count > KERNEL_BLOCK_BATCH_MAX || (count && !spans))
        return KERNEL_BLOCK_STATUS_INVALID;
    BLOCK_REQUEST(device);
    struct kernel_block_device *disk = physical(device);
    KERNEL_LOCK_SCOPE(transfer); kernel_rwlock_write(&disk->transfer_lock, &transfer);
    const uint64_t limit = capacity(device);
    int nonempty = 0;
    for (size_t i = 0; i < count; i++) {
        const struct kernel_block_span *span = &spans[i];
        if (span->offset > limit ||
            (uint64_t)span->size > limit - span->offset)
            return KERNEL_BLOCK_STATUS_OUT_OF_RANGE;
        if (!span->size) continue;
        if (!span->buffer || span->size - 1 > UINTPTR_MAX - (uintptr_t)span->buffer)
            return KERNEL_BLOCK_STATUS_INVALID;
        nonempty = 1;
        for (size_t j = 0; j < i; j++) {
            if (spans[j].size && span->offset < spans[j].offset + spans[j].size &&
                spans[j].offset < span->offset + span->size)
                return KERNEL_BLOCK_STATUS_INVALID;
        }
    }
    if (!nonempty) return KERNEL_BLOCK_STATUS_OK;
    if (kernel_block_read_only(device)) return KERNEL_BLOCK_STATUS_READ_ONLY;
    if (disk->write_batch)
        return device->parent ? partition_write_batch(device, spans, count) :
            disk->write_batch(disk->context, spans, count);
    for (size_t i = 0; i < count; i++) {
        if (!spans[i].size) continue;
        enum kernel_block_status result = disk->write(disk->context,
            device->start_bytes + spans[i].offset, spans[i].buffer, spans[i].size);
        if (result != KERNEL_BLOCK_STATUS_OK) return result;
    }
    return KERNEL_BLOCK_STATUS_OK;
}

static int memory_overlaps(uintptr_t a, size_t an, uintptr_t b, size_t bn)
{ return an && bn && (a < b ? b - a < an : a - b < bn); }

#define BLOCK_TRANSFER_BYTES (128U * 1024U)
struct phase_window {
    struct kernel_block_span spans[KERNEL_BLOCK_BATCH_MAX];
    size_t count;
};
struct partition_phase_wrapper {
    kernel_block_phase_next_fn next;
    const void *next_context;
    uint64_t start_bytes;
};
static int partition_phase_next(const void *context, const void **cursor, struct kernel_block_span *span)
{
    const struct partition_phase_wrapper *wrap = context;
    int has_next = wrap->next(wrap->next_context, cursor, span);
    if (has_next && wrap->start_bytes) {
        span->offset += wrap->start_bytes;
    }
    return has_next;
}

/* The producer and full-call barrier remain borrowed across all windows. Only
 * the transfer qualifier is returned, after the driver's DMA has drained. */
static __attribute__((noinline)) enum kernel_block_status write_phase_windows(
    struct kernel_block_device *device,kernel_block_phase_next_fn next,const void *context,const void *cursor)
{
    struct kernel_block_device *disk=physical(device);
    if (disk->write_phase) {
        struct partition_phase_wrapper wrap = {next, context, device->start_bytes};
        KERNEL_LOCK_SCOPE(transfer); kernel_rwlock_write(&disk->transfer_lock, &transfer);
        return disk->write_phase(disk->context, partition_phase_next, &wrap, cursor);
    }
    struct phase_window window;
    struct kernel_block_span pending={0};
    int ended=0;
    do {
        size_t bytes=0;window.count=0;
        while (bytes<BLOCK_TRANSFER_BYTES && window.count<KERNEL_BLOCK_BATCH_MAX) {
            while (!pending.size && !ended) ended=!next(context,&cursor,&pending);
            if (!pending.size) break;
            size_t length=pending.size<BLOCK_TRANSFER_BYTES-bytes?pending.size:BLOCK_TRANSFER_BYTES-bytes;
            if (window.count &&
                window.spans[window.count-1].offset + window.spans[window.count-1].size == device->start_bytes + pending.offset &&
                (const unsigned char *)window.spans[window.count-1].buffer + window.spans[window.count-1].size == (const unsigned char *)pending.buffer) {
                window.spans[window.count-1].size += length;
            } else {
                window.spans[window.count++]=(struct kernel_block_span){
                    device->start_bytes+pending.offset,pending.buffer,length};
            }
            pending.offset+=length;pending.buffer=(const unsigned char *)pending.buffer+length;
            pending.size-=length;bytes+=length;
        }
        if (!window.count) break;
        KERNEL_LOCK_SCOPE(transfer);kernel_rwlock_write(&disk->transfer_lock,&transfer);
        for (size_t i=0;i<window.count;i++) {
            const struct kernel_block_span *span=&window.spans[i];
            enum kernel_block_status result=disk->write(disk->context,span->offset,span->buffer,span->size);
            if (result) return result;
        }
    } while (!ended || pending.size);
    return KERNEL_BLOCK_STATUS_OK;
}

enum kernel_block_status kernel_block_write_phase(struct kernel_block_device *device,
    kernel_block_phase_next_fn next, const void *next_context, const void *first)
{
    if (!device || !device->logical_block_size || !next) return KERNEL_BLOCK_STATUS_INVALID;
    if (!physical(device)->write && !physical(device)->write_phase) return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    BLOCK_REQUEST(device);
    struct kernel_block_device *disk = physical(device);
    KERNEL_LOCK_SCOPE(io); kernel_rwlock_read(&disk->phase_barrier, &io);
    const uint64_t limit = capacity(device);
    const void *cursor = first;
    struct kernel_block_span span;
    unsigned nonempty = 0;
    while (next(next_context, &cursor, &span)) {
        if (span.offset > limit || span.size > limit - span.offset)
            return KERNEL_BLOCK_STATUS_OUT_OF_RANGE;
        if (!span.size) continue;
        if (!span.buffer || span.size - 1 > UINTPTR_MAX - (uintptr_t)span.buffer ||
            span.offset % device->logical_block_size || span.size % device->logical_block_size)
            return KERNEL_BLOCK_STATUS_INVALID;
        nonempty = 1;
    }
    if (!nonempty) return KERNEL_BLOCK_STATUS_OK;
    if (kernel_block_read_only(device)) return KERNEL_BLOCK_STATUS_READ_ONLY;
    return write_phase_windows(device,next,next_context,first);
}

struct translated_completion {
    struct block_read_span *spans;
    const struct block_read_completion *completion;
};
static void read_completed(void *context, unsigned index, const struct block_read_span *span)
{
    struct translated_completion *state = context;
    state->spans[index].status = span->status;
    state->spans[index].completed = span->completed;
    state->completion->complete(state->completion->context, index, &state->spans[index]);
}

static __attribute__((noinline)) enum kernel_block_status partition_read_batch(
    struct kernel_block_device *device, struct block_read_span *spans, size_t count,
    const struct block_read_completion *completion)
{
    struct block_read_span translated[count];
    for (size_t i = 0; i < count; i++) {
        translated[i] = spans[i]; translated[i].offset += device->start_bytes;
    }
    struct translated_completion state = { spans, completion };
    struct block_read_completion notify = { read_completed, &state };
    struct kernel_block_device *disk = physical(device);
    enum kernel_block_status result = disk->read_batch(disk->context, translated, count,
        completion ? &notify : 0);
    for (size_t i = 0; i < count; i++) {
        spans[i].status = translated[i].status; spans[i].completed = translated[i].completed;
    }
    return result;
}

enum kernel_block_status kernel_block_read_batch(struct kernel_block_device *device,
    struct block_read_span *spans, size_t count, const struct block_read_completion *completion)
{
    if (!device || !device->logical_block_size || count > KERNEL_BLOCK_BATCH_MAX || (count && !spans))
        return KERNEL_BLOCK_STATUS_INVALID;
    if (!physical(device)->read && !physical(device)->read_batch) return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    BLOCK_REQUEST(device);
    struct kernel_block_device *disk = physical(device);
    KERNEL_LOCK_SCOPE(transfer); kernel_rwlock_read(&disk->transfer_lock, &transfer);
    const uint64_t limit = capacity(device);
    for (size_t i = 0; i < count; i++) {
        spans[i].completed = 0; spans[i].status = KERNEL_BLOCK_STATUS_NOT_SUBMITTED;
    }
    int nonempty = 0;
    uintptr_t first = UINTPTR_MAX, last = 0;
    for (size_t i = 0; i < count; i++) {
        struct block_read_span *span = &spans[i];
        if (span->offset > limit || span->size > limit - span->offset)
            return span->status = KERNEL_BLOCK_STATUS_OUT_OF_RANGE;
        if (!span->size) { span->status = KERNEL_BLOCK_STATUS_OK; continue; }
        uintptr_t buffer = (uintptr_t)span->buffer;
        if (!buffer || span->size - 1 > UINTPTR_MAX - buffer ||
            memory_overlaps(buffer, span->size, (uintptr_t)spans, count * sizeof(*spans)))
            return span->status = KERNEL_BLOCK_STATUS_INVALID;
        uintptr_t end = buffer + span->size - 1;
        /* 单调排列的 SG 目标不与已见范围相交；落入间隙时才逐项检查。 */
        if (nonempty && buffer <= last && end >= first)
            for (size_t j = 0; j < i; j++)
                if (memory_overlaps(buffer, span->size, (uintptr_t)spans[j].buffer, spans[j].size))
                    return span->status = KERNEL_BLOCK_STATUS_INVALID;
        if (buffer < first) first = buffer;
        if (end > last) last = end;
        nonempty = 1;
    }
    if (!nonempty) return KERNEL_BLOCK_STATUS_OK;
    for (size_t i = 0; i < count; i++) kernel_task_account_read(spans[i].size);
    if (disk->read_batch)
        return device->parent ? partition_read_batch(device, spans, count, completion) :
            disk->read_batch(disk->context, spans, count, completion);
    enum kernel_block_status result = KERNEL_BLOCK_STATUS_OK;
    for (size_t i = 0; i < count; i++) {
        if (spans[i].size) {
            spans[i].status = disk->read(disk->context, device->start_bytes + spans[i].offset, spans[i].buffer, spans[i].size);
            if (spans[i].status == KERNEL_BLOCK_STATUS_OK) spans[i].completed = spans[i].size;
            else if (result == KERNEL_BLOCK_STATUS_OK) result = spans[i].status;
        }
        if (completion && i + 1 < count) completion->complete(completion->context, i, &spans[i]);
    }
    return result;
}

static struct kernel_block_device *registered_devices;

int kernel_block_register(struct kernel_block_device *device, uint64_t number)
{
    if (!device || !physical(device)->read || !device->logical_block_size)
        return -KERNEL_EINVAL;
    uintptr_t irq = registry_lock();
    int error = 0;
    if (device->registered || device->references || device->notifications || device->claim_owner) error = -KERNEL_EBUSY;
    for (struct kernel_block_device *it = registered_devices; it; it = it->registry_next)
        if (it->device_number == number) error = -KERNEL_EBUSY;
    if (device->parent && (!device->parent->registered || device->parent->parent ||
        device->start_bytes > capacity(device->parent) ||
        capacity(device) > capacity(device->parent) - device->start_bytes)) error = -KERNEL_EINVAL;
    if (!error) {
        if (device->parent) {
            if (device->parent->references == UINT32_MAX) __builtin_trap();
            device->parent->references++;
        }
        prepare_locked(device);
        device->device_number = number;
        device->claim_owner = 0;
        device->registry_next = registered_devices;
        device->registered = 1U;
        registered_devices = device;
    }
    registry_unlock(irq);
    return error;
}

int kernel_block_unregister(struct kernel_block_device *device)
{
    if (!device) return -KERNEL_EINVAL;
    uintptr_t irq = registry_lock();
    int error = 0;
    if (!device->registered) error = -KERNEL_ENODEV;
    else if (device->references || device->notifications || device->claim_owner) error = -KERNEL_EBUSY;
    else {
        struct kernel_block_device **link = &registered_devices;
        while (*link && *link != device) link = &(*link)->registry_next;
        if (!*link) __builtin_trap();
        *link = device->registry_next;
        device->registered = 0U;
        device->registry_next = 0;
        if (device->parent) {
            if (!device->parent->references) __builtin_trap();
            device->parent->references--;
        }
    }
    registry_unlock(irq);
    return error;
}

struct kernel_block_device *kernel_block_lookup(uint64_t number)
{
    uintptr_t irq = registry_lock();
    struct kernel_block_device *device = registered_devices;
    while (device && device->device_number != number) device = device->registry_next;
    registry_unlock(irq);
    return device;
}

static int claim_conflicts_locked(struct kernel_block_device *device)
{
    if (device->claim_owner) return 1;
    if (device->parent) return device->parent->claim_owner != 0;
    for (struct kernel_block_device *it = registered_devices; it; it = it->registry_next)
        if (it->parent == device && it->claim_owner) return 1;
    return 0;
}

int kernel_block_claim(struct kernel_block_device *device, const void *owner)
{
    if (!device || !owner) return -KERNEL_EINVAL;
    uintptr_t irq = registry_lock();
    int error = claim_conflicts_locked(device) ? -KERNEL_EBUSY : 0;
    if (!error) {
        prepare_locked(device);
        if (device->references == UINT32_MAX) __builtin_trap();
        device->references++;
        device->claim_owner = owner;
    }
    registry_unlock(irq);
    return error;
}

void kernel_block_release_claim(struct kernel_block_device *device,
                                const void *owner)
{
    uintptr_t irq = registry_lock();
    if (!device || !owner || device->claim_owner != owner) __builtin_trap();
    device->claim_owner = 0;
    if (!device->references) __builtin_trap();
    device->references--;
    int notify = begin_notification_locked(device);
    registry_unlock(irq);
    finish_notification(device, notify);
}

struct kernel_block_device *kernel_block_get(uint64_t number)
{
    uintptr_t irq = registry_lock();
    struct kernel_block_device *device = registered_devices;
    while (device && device->device_number != number) device = device->registry_next;
    if (device) {
        if (device->references == UINT32_MAX) __builtin_trap();
        device->references++;
    }
    registry_unlock(irq);
    return device;
}
int kernel_block_claimed(struct kernel_block_device *device)
{
    uintptr_t irq = registry_lock();
    int claimed = claim_conflicts_locked(device);
    registry_unlock(irq);
    return claimed;
}
void kernel_block_usage(struct kernel_block_device *device, uint32_t *references, uint32_t *opens)
{
    uintptr_t irq = registry_lock();
    if (references) *references = device->references;
    if (opens) *opens = device->open_count;
    registry_unlock(irq);
}
int kernel_block_open(struct kernel_block_device *device, const void *owner, uint32_t flags)
{
    if (!device || !owner) return -KERNEL_EINVAL;
    uintptr_t irq = registry_lock();
    int error = (flags & 0200U) && claim_conflicts_locked(device) ? -KERNEL_EBUSY : 0;
    if (!error) {
        prepare_locked(device);
        if (device->references == UINT32_MAX || device->open_count == UINT32_MAX) __builtin_trap();
        device->references++; device->open_count++;
        if (flags & 0200U) device->claim_owner = owner;
    }
    registry_unlock(irq);
    if (!error && device->open) {
        error = device->open(device, flags);
        if (error) {
            irq = registry_lock();
            if ((flags & 0200U) && device->claim_owner == owner) device->claim_owner = 0;
            device->references--; device->open_count--;
            int notify = begin_notification_locked(device);
            registry_unlock(irq);
            finish_notification(device, notify);
        }
    }
    return error;
}
void kernel_block_close(struct kernel_block_device *device, const void *owner, uint32_t flags)
{
    if (device->close) device->close(device, flags);
    uintptr_t irq = registry_lock();
    if (!device->references || !device->open_count) __builtin_trap();
    if (flags & 0200U) {
        if (device->claim_owner != owner) __builtin_trap();
        device->claim_owner = 0;
    }
    device->references--; device->open_count--;
    int notify = begin_notification_locked(device);
    registry_unlock(irq);
    finish_notification(device, notify);
}
void kernel_block_set_capacity(struct kernel_block_device *device, uint64_t bytes)
{
    if (device->parent) __builtin_trap();
    kernel_block_get_device(device);
    {
        KERNEL_LOCK_SCOPE(io); kernel_rwlock_write(&device->phase_barrier, &io);
        KERNEL_LOCK_SCOPE(transfer); kernel_rwlock_write(&device->transfer_lock, &transfer);
        __atomic_store_n(&device->capacity_bytes, bytes, __ATOMIC_RELEASE);
    }
    kernel_block_put(device);
}
int kernel_block_read_only(const struct kernel_block_device *device)
{
    const struct kernel_block_device *disk = device->parent ? device->parent : device;
    return __atomic_load_n(&device->read_only, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&disk->read_only, __ATOMIC_ACQUIRE) ||
        (!disk->write && !disk->write_batch && !disk->write_phase);
}
int kernel_block_set_read_only(struct kernel_block_device *device, int read_only)
{
    if (!device) return -KERNEL_EINVAL;
    BLOCK_REQUEST(device);
    struct kernel_block_device *disk = physical(device);
    KERNEL_LOCK_SCOPE(io); kernel_rwlock_write(&disk->phase_barrier, &io);
    KERNEL_LOCK_SCOPE(transfer); kernel_rwlock_write(&disk->transfer_lock, &transfer);
    __atomic_store_n(&device->read_only, !!read_only, __ATOMIC_RELEASE);
    return 0;
}
static enum kernel_block_status byte_io(struct kernel_block_device *device,
    uint64_t offset, void *buffer, size_t size, void *scratch, size_t scratch_size, int writing, size_t *completed)
{
    if (!completed) return KERNEL_BLOCK_STATUS_INVALID;
    *completed = 0;
    if (!device || !physical(device)->read || !device->logical_block_size ||
        (writing && !physical(device)->write)) return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    BLOCK_REQUEST(device);
    struct kernel_block_device *disk = physical(device);
    KERNEL_LOCK_SCOPE(transfer); if (writing) kernel_rwlock_write(&disk->transfer_lock, &transfer);
    else kernel_rwlock_read(&disk->transfer_lock, &transfer);
    const uint64_t limit = capacity(device);
    if (offset > limit || size > limit - offset)
        return KERNEL_BLOCK_STATUS_OUT_OF_RANGE;
    if (!size) return KERNEL_BLOCK_STATUS_OK;
    size_t sector = device->logical_block_size;
    if (!buffer || !scratch || scratch_size < sector) return KERNEL_BLOCK_STATUS_INVALID;
    if (writing && kernel_block_read_only(device)) return KERNEL_BLOCK_STATUS_READ_ONLY;
    unsigned char *bytes = buffer;
    while (size) {
        size_t within = offset % sector;
        uint64_t first = offset - within;
        /* loop 的容量按 512 字节取整，末尾可能不足一个配置的逻辑扇区。
         * RMW 只借用设备暴露的字节，不能越过容量读取或改写后备文件。 */
        size_t span = limit - first < sector ? (size_t)(limit - first) : sector;
        size_t amount = size < span - within ? size : span - within;
        enum kernel_block_status result;
        size_t submitted;
        if (!within && amount == sector) {
            /* Whole sectors form one request, preserving the backend's batching. */
            amount = size - size % sector;
            submitted = amount;
            if (!writing) kernel_task_account_read(amount);
            result = writing ? disk->write(disk->context, device->start_bytes + offset, bytes, amount)
                             : disk->read(disk->context, device->start_bytes + offset, bytes, amount);
        } else {
            submitted = span;
            kernel_task_account_read(span);
            result = disk->read(disk->context, device->start_bytes + first, scratch, span);
            if (result != KERNEL_BLOCK_STATUS_OK) return result;
            if (writing) {
                __builtin_memcpy((unsigned char *)scratch + within, bytes, amount);
                result = disk->write(disk->context, device->start_bytes + first, scratch, span);
            } else __builtin_memcpy(bytes, (unsigned char *)scratch + within, amount);
        }
        if (result != KERNEL_BLOCK_STATUS_OK) return result;
        if (writing) kernel_task_account_write(submitted);
        *completed += amount;
        offset += amount; bytes += amount; size -= amount;
    }
    return KERNEL_BLOCK_STATUS_OK;
}
enum kernel_block_status kernel_block_read_bytes(struct kernel_block_device *device,
    uint64_t offset, void *buffer, size_t size, void *scratch, size_t scratch_size, size_t *completed)
{ return byte_io(device, offset, buffer, size, scratch, scratch_size, 0, completed); }
enum kernel_block_status kernel_block_write_bytes(struct kernel_block_device *device,
    uint64_t offset, const void *buffer, size_t size, void *scratch, size_t scratch_size, size_t *completed)
{ return byte_io(device, offset, (void *)buffer, size, scratch, scratch_size, 1, completed); }

static uint32_t mbr_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int kernel_block_scan_mbr(struct kernel_block_device *device,
    struct kernel_block_device children[4])
{
    if (!device || !children || !device->registered || device->parent ||
        device->logical_block_size != 512) return -KERNEL_EINVAL;
    unsigned char sector[512];
    enum kernel_block_status status = kernel_block_read_at(device, 0, sector, sizeof(sector));
    if (status != KERNEL_BLOCK_STATUS_OK) return -KERNEL_EIO;
    if (sector[510] != 0x55 || sector[511] != 0xaa) return 0;
    uint64_t starts[4] = {0}, sizes[4] = {0};
    for (unsigned i = 0; i < 4; i++) {
        const unsigned char *entry = sector + 446 + i * 16;
        uint32_t first = mbr_u32(entry + 8), count = mbr_u32(entry + 12);
        if (children[i].registered || children[i].references) return -KERNEL_EBUSY;
        if (entry[0] && entry[0] != 0x80) return -KERNEL_EINVAL;
        if (!entry[4] && !first && !count) continue;
        if (!entry[4] || !first || !count) return -KERNEL_EINVAL;
        /* Extended/GPT containers are not usable primary data partitions. */
        if (entry[4] == 0x05 || entry[4] == 0x0f || entry[4] == 0x85 || entry[4] == 0xee)
            return -KERNEL_ENOTSUP;
        starts[i] = (uint64_t)first * 512;
        sizes[i] = (uint64_t)count * 512;
        if (starts[i] > capacity(device) || sizes[i] > capacity(device) - starts[i]) return -KERNEL_EINVAL;
        for (unsigned j = 0; j < i; j++)
            if (sizes[j] && starts[i] < starts[j] + sizes[j] && starts[j] < starts[i] + sizes[i])
                return -KERNEL_EINVAL;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (!sizes[i]) continue;
        children[i] = (struct kernel_block_device){ .parent = device,
            .start_bytes = starts[i], .capacity_bytes = sizes[i], .logical_block_size = 512 };
        int error = kernel_block_register(&children[i], device->device_number + i + 1);
        if (error) {
            while (i) { i--; if (sizes[i] && kernel_block_unregister(&children[i])) __builtin_trap(); }
            return error;
        }
    }
    return 0;
}
