#include <kernel/block.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <arch/context.h>
#include <stddef.h>
#include <stdint.h>

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
        kernel_mutex_init(&device->io_lock, 50, (uintptr_t)device);
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
static void block_request_put(struct kernel_block_device **device)
{ kernel_block_put(*device); }
#define BLOCK_REQUEST(device) \
    kernel_block_get_device(device); \
    struct kernel_block_device *request_owner __attribute__((cleanup(block_request_put))) = device

enum kernel_block_status kernel_block_flush(struct kernel_block_device *device)
{
    kernel_assert_service_context();
    if (device == 0 || device->logical_block_size == 0U) {
        return KERNEL_BLOCK_STATUS_INVALID;
    }
    BLOCK_REQUEST(device);
    KERNEL_LOCK_SCOPE(io); kernel_mutex_lock(&device->io_lock, &io);
    if (device->flush != 0) {
        return device->flush(device->context);
    }
    return device->cache_mode == KERNEL_BLOCK_CACHE_WRITETHROUGH
               ? KERNEL_BLOCK_STATUS_OK : KERNEL_BLOCK_STATUS_UNSUPPORTED;
}

enum kernel_block_status kernel_block_read_at(
    struct kernel_block_device *device,
    uint64_t offset,
    void *buffer,
    size_t size)
{
    kernel_assert_service_context();
    if (device == 0 || device->read == 0 ||
        device->logical_block_size == 0U) {
        return KERNEL_BLOCK_STATUS_INVALID;
    }
    BLOCK_REQUEST(device);
    KERNEL_LOCK_SCOPE(io); kernel_mutex_lock(&device->io_lock, &io);
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

    return device->read(device->context, offset, buffer, size);
}

enum kernel_block_status kernel_block_write_at(
    struct kernel_block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t size)
{
    kernel_assert_service_context();
    if (device == 0 || device->write == 0 ||
        device->logical_block_size == 0U) {
        return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    }
    BLOCK_REQUEST(device);
    KERNEL_LOCK_SCOPE(io); kernel_mutex_lock(&device->io_lock, &io);
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
    return device->write(device->context, offset, buffer, size);
}

enum kernel_block_status kernel_block_write_batch(
    struct kernel_block_device *device,
    const struct kernel_block_span *spans,
    size_t count)
{
    kernel_assert_service_context();
    if (!device || !device->logical_block_size)
        return KERNEL_BLOCK_STATUS_INVALID;
    if (!device->write && !device->write_batch)
        return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    if (count > KERNEL_BLOCK_BATCH_MAX || (count && !spans))
        return KERNEL_BLOCK_STATUS_INVALID;
    BLOCK_REQUEST(device);
    KERNEL_LOCK_SCOPE(io); kernel_mutex_lock(&device->io_lock, &io);
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
    if (device->write_batch)
        return device->write_batch(device->context, spans, count);
    for (size_t i = 0; i < count; i++) {
        if (!spans[i].size) continue;
        enum kernel_block_status result = device->write(device->context,
            spans[i].offset, spans[i].buffer, spans[i].size);
        if (result != KERNEL_BLOCK_STATUS_OK) return result;
    }
    return KERNEL_BLOCK_STATUS_OK;
}

static int memory_overlaps(uintptr_t a, size_t an, uintptr_t b, size_t bn)
{ return an && bn && (a < b ? b - a < an : a - b < bn); }

enum kernel_block_status kernel_block_write_phase(struct kernel_block_device *device,
    kernel_block_phase_next_fn next, const void *first)
{
    kernel_assert_service_context();
    if (!device || !device->logical_block_size || !next) return KERNEL_BLOCK_STATUS_INVALID;
    if (!device->write && !device->write_phase) return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    BLOCK_REQUEST(device);
    KERNEL_LOCK_SCOPE(io); kernel_mutex_lock(&device->io_lock, &io);
    const uint64_t limit = capacity(device);
    const void *cursor = first;
    struct kernel_block_span span;
    unsigned nonempty = 0;
    while (next(&cursor, &span)) {
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
    if (device->write_phase) return device->write_phase(device->context, next, first);
    cursor = first;
    while (next(&cursor, &span)) {
        if (!span.size) continue;
        enum kernel_block_status result = device->write(device->context, span.offset, span.buffer, span.size);
        if (result != KERNEL_BLOCK_STATUS_OK) return result;
    }
    return KERNEL_BLOCK_STATUS_OK;
}

enum kernel_block_status kernel_block_read_batch(struct kernel_block_device *device,
    struct block_read_span *spans, size_t count, const struct block_read_completion *completion)
{
    kernel_assert_service_context();
    if (!device || !device->logical_block_size || count > KERNEL_BLOCK_BATCH_MAX || (count && !spans))
        return KERNEL_BLOCK_STATUS_INVALID;
    if (!device->read && !device->read_batch) return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    BLOCK_REQUEST(device);
    KERNEL_LOCK_SCOPE(io); kernel_mutex_lock(&device->io_lock, &io);
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
    if (device->read_batch) return device->read_batch(device->context, spans, count, completion);
    enum kernel_block_status result = KERNEL_BLOCK_STATUS_OK;
    for (size_t i = 0; i < count; i++) {
        if (spans[i].size) {
            spans[i].status = device->read(device->context, spans[i].offset, spans[i].buffer, spans[i].size);
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
    if (!device || !device->read || !device->logical_block_size)
        return -KERNEL_EINVAL;
    uintptr_t irq = registry_lock();
    int error = 0;
    if (device->registered || device->references || device->notifications || device->claim_owner) error = -KERNEL_EBUSY;
    for (struct kernel_block_device *it = registered_devices; it; it = it->registry_next)
        if (it->device_number == number) error = -KERNEL_EBUSY;
    if (!error) {
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

int kernel_block_claim(struct kernel_block_device *device, const void *owner)
{
    if (!device || !owner) return -KERNEL_EINVAL;
    uintptr_t irq = registry_lock();
    int error = device->claim_owner ? -KERNEL_EBUSY : 0;
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
    int claimed = device->claim_owner != 0;
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
    if ((flags & 3U) && kernel_block_read_only(device)) return -KERNEL_EACCES;
    uintptr_t irq = registry_lock();
    int error = (flags & 0200U) && device->claim_owner ? -KERNEL_EBUSY : 0;
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
    kernel_block_get_device(device);
    {
        KERNEL_LOCK_SCOPE(io); kernel_mutex_lock(&device->io_lock, &io);
        __atomic_store_n(&device->capacity_bytes, bytes, __ATOMIC_RELEASE);
    }
    kernel_block_put(device);
}
int kernel_block_read_only(const struct kernel_block_device *device)
{ return __atomic_load_n(&device->read_only, __ATOMIC_ACQUIRE) != 0 ||
    (!device->write && !device->write_batch && !device->write_phase); }
int kernel_block_set_read_only(struct kernel_block_device *device, int read_only)
{
    if (!device) return -KERNEL_EINVAL;
    BLOCK_REQUEST(device);
    KERNEL_LOCK_SCOPE(io); kernel_mutex_lock(&device->io_lock, &io);
    __atomic_store_n(&device->read_only, !!read_only, __ATOMIC_RELEASE);
    return 0;
}
static enum kernel_block_status byte_io(struct kernel_block_device *device,
    uint64_t offset, void *buffer, size_t size, void *scratch, size_t scratch_size, int writing)
{
    if (!device || !device->read || !device->logical_block_size ||
        (writing && !device->write)) return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    BLOCK_REQUEST(device);
    KERNEL_LOCK_SCOPE(io); kernel_mutex_lock(&device->io_lock, &io);
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
        size_t amount = size < sector - within ? size : sector - within;
        enum kernel_block_status result;
        if (!within && amount == sector) {
            /* Whole sectors form one request, preserving the backend's batching. */
            amount = size - size % sector;
            result = writing ? device->write(device->context, offset, bytes, amount)
                             : device->read(device->context, offset, bytes, amount);
        } else {
            result = device->read(device->context, offset - within, scratch, sector);
            if (result != KERNEL_BLOCK_STATUS_OK) return result;
            if (writing) {
                __builtin_memcpy((unsigned char *)scratch + within, bytes, amount);
                result = device->write(device->context, offset - within, scratch, sector);
            } else __builtin_memcpy(bytes, (unsigned char *)scratch + within, amount);
        }
        if (result != KERNEL_BLOCK_STATUS_OK) return result;
        offset += amount; bytes += amount; size -= amount;
    }
    return KERNEL_BLOCK_STATUS_OK;
}
enum kernel_block_status kernel_block_read_bytes(struct kernel_block_device *device,
    uint64_t offset, void *buffer, size_t size, void *scratch, size_t scratch_size)
{ return byte_io(device, offset, buffer, size, scratch, scratch_size, 0); }
enum kernel_block_status kernel_block_write_bytes(struct kernel_block_device *device,
    uint64_t offset, const void *buffer, size_t size, void *scratch, size_t scratch_size)
{ return byte_io(device, offset, (void *)buffer, size, scratch, scratch_size, 1); }
