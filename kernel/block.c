#include <kernel/block.h>
#include <kernel/accounting.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <kernel/time.h>
#include <arch/context.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct kernel_block_device *physical(struct kernel_block_device *device)
{ return device->parent ? device->parent : device; }

enum block_operation { BLOCK_IO_READ, BLOCK_IO_WRITE, BLOCK_IO_FLUSH };

static void block_activity(struct kernel_block_usage *usage, uint64_t now, int completing)
{
    uint64_t ms = now / UINT64_C(1000000);
    uint64_t previous = __atomic_load_n(&usage->event_milliseconds, __ATOMIC_RELAXED);
    if (ms > previous && __atomic_compare_exchange_n(&usage->event_milliseconds,
            &previous, ms, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) &&
        (completing || __atomic_load_n(&usage->in_flight, __ATOMIC_RELAXED)))
        __atomic_fetch_add(&usage->busy_milliseconds, ms - previous, __ATOMIC_RELAXED);
}

static uint64_t block_io_begin(struct kernel_block_device *device)
{
    uint64_t started = kernel_time_monotonic_ns();
    for (; device; device = device->parent) {
        block_activity(&device->usage, started, 0);
        __atomic_fetch_add(&device->usage.in_flight, 1, __ATOMIC_RELAXED);
    }
    return started;
}

static void block_io_end(struct kernel_block_device *device, enum block_operation operation,
    uint64_t started, uint64_t bytes)
{
    uint64_t now = kernel_time_monotonic_ns();
    uint64_t elapsed = now - started;
    uint64_t sectors = bytes / 512U + !!(bytes % 512U);
    for (; device; device = device->parent) {
        struct kernel_block_usage *usage = &device->usage;
        struct kernel_block_stat_operation *stats = operation == BLOCK_IO_READ ? &usage->read :
            operation == BLOCK_IO_WRITE ? &usage->write : &usage->flush;
        block_activity(usage, now, 1);
        __atomic_fetch_add(&stats->elapsed_ns, elapsed, __ATOMIC_RELAXED);
        if (sectors) __atomic_fetch_add(&stats->sectors, sectors, __ATOMIC_RELAXED);
        __atomic_fetch_add(&stats->completed, 1, __ATOMIC_RELAXED);
        __atomic_fetch_sub(&usage->in_flight, 1, __ATOMIC_RELAXED);
    }
}

static enum kernel_block_status read_backend(struct kernel_block_device *device,
    uint64_t offset, void *buffer, size_t size)
{
    struct kernel_block_device *disk = physical(device);
    uint64_t started = block_io_begin(device);
    enum kernel_block_status result = disk->read(disk->context, device->start_bytes + offset, buffer, size);
    block_io_end(device, BLOCK_IO_READ, started, result ? 0 : size);
    return result;
}

static enum kernel_block_status write_backend(struct kernel_block_device *device,
    uint64_t offset, const void *buffer, size_t size)
{
    struct kernel_block_device *disk = physical(device);
    uint64_t started = block_io_begin(device);
    enum kernel_block_status result = disk->write(disk->context, device->start_bytes + offset, buffer, size);
    block_io_end(device, BLOCK_IO_WRITE, started, result ? 0 : size);
    return result;
}

static char *append_number(char *out, unsigned value)
{
    if (value >= 10) out = append_number(out, value / 10);
    *out++ = '0' + value % 10;
    return out;
}

int kernel_block_device_name(uint64_t number, char name[32])
{
    unsigned major = (number >> 8) & 0xfff;
    unsigned minor = (number & 0xff) | ((number >> 12) & 0xfff00);
    char *out = name;
    if (major == 7) {
        memcpy(out, "loop", 4);
        out = append_number(out + 4, minor);
    } else if (major == 252) {
        unsigned disk = minor / 16;
        char reversed[8];
        size_t length = 0;
        do {
            reversed[length++] = 'a' + disk % 26;
            disk = disk / 26;
            if (!disk) break;
            disk--;
        } while (1);
        *out++ = 'v'; *out++ = 'd';
        while (length) *out++ = reversed[--length];
        if (minor % 16) out = append_number(out, minor % 16);
    } else return 0;
    *out = '\0';
    return 1;
}

static int write_needs_rmw(struct kernel_block_device *device,uint64_t offset,size_t size)
{
    uint32_t sector=physical(device)->logical_block_size;
    /* 调用者持有 transfer 或 phase 资格，保护 loop 可配置的物理扇区大小。
     * 驱动按物理盘扇区做 RMW；分区自身的对齐不能代替平移后的对齐。
     * 分开取余也让尚未通过范围检查的 offset 不会在相加时溢出。 */
    return size && ((device->start_bytes%sector+offset%sector)%sector || size%sector);
}

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

void kernel_block_note_writeback(struct kernel_block_device *device)
{
    struct kernel_block_device *disk = physical(device);
    if (!__atomic_add_fetch(&disk->writeback_epoch, 1, __ATOMIC_RELEASE))
        __builtin_trap();
}

uint64_t kernel_block_writeback_epoch(struct kernel_block_device *device)
{ return __atomic_load_n(&physical(device)->writeback_epoch, __ATOMIC_ACQUIRE); }

enum kernel_block_status kernel_block_flush_through(struct kernel_block_device *device,
    uint64_t target)
{
    if (device == 0 || device->logical_block_size == 0U) {
        return KERNEL_BLOCK_STATUS_INVALID;
    }
    BLOCK_REQUEST(device);
    struct kernel_block_device *disk = physical(device);
    KERNEL_LOCK_SCOPE(io); kernel_rwlock_write(&disk->phase_barrier, &io);
    KERNEL_LOCK_SCOPE(transfer); kernel_rwlock_write(&disk->transfer_lock, &transfer);
    if (target && __atomic_load_n(&disk->flushed_writeback_epoch, __ATOMIC_RELAXED) >= target)
        return KERNEL_BLOCK_STATUS_OK;
    uint64_t completed = kernel_block_writeback_epoch(disk);
    enum kernel_block_status result;
    if (disk->flush != 0) {
        /* FLUSH covers the whole underlying queue, regardless of its caller. */
        uint64_t started = block_io_begin(disk);
        result = disk->flush(disk->context);
        block_io_end(disk, BLOCK_IO_FLUSH, started, 0);
    } else {
        result = disk->cache_mode == KERNEL_BLOCK_CACHE_WRITETHROUGH
            ? KERNEL_BLOCK_STATUS_OK : KERNEL_BLOCK_STATUS_UNSUPPORTED;
    }
    if (result == KERNEL_BLOCK_STATUS_OK)
        __atomic_store_n(&disk->flushed_writeback_epoch, completed, __ATOMIC_RELEASE);
    return result;
}

enum kernel_block_status kernel_block_flush(struct kernel_block_device *device)
{ return kernel_block_flush_through(device, 0); }

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
    return read_backend(device, offset, buffer, size);
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
    KERNEL_LOCK_SCOPE(transfer); kernel_rwlock_read(&disk->transfer_lock,&transfer);
    if (write_needs_rmw(device,offset,size)) {
        kernel_lock_release(&transfer);
        kernel_rwlock_write(&disk->transfer_lock,&transfer);
    }
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
    return write_backend(device, offset, buffer, size);
}

#define PARTITION_SMALL_BATCH 4U

static enum kernel_block_status partition_write_translated(
    struct kernel_block_device *device, const struct kernel_block_span *spans,
    struct kernel_block_span *translated, size_t count, enum kernel_block_priority priority)
{
    for (size_t i = 0; i < count; i++) {
        translated[i] = spans[i]; translated[i].offset += device->start_bytes;
    }
    struct kernel_block_device *disk = physical(device);
    if (disk->write_batch_prio)
        return disk->write_batch_prio(disk->context, translated, count, priority);
    return disk->write_batch(disk->context, translated, count);
}

static __attribute__((noinline)) enum kernel_block_status partition_write_batch_large(
    struct kernel_block_device *device, const struct kernel_block_span *spans, size_t count,
    enum kernel_block_priority priority)
{
    struct kernel_block_span translated[KERNEL_BLOCK_BATCH_MAX];
    return partition_write_translated(device,spans,translated,count,priority);
}

/* A 4 KiB fault can split into four 1 KiB filesystem blocks. Keep that path
 * separate from the maximum batch frame without splitting a driver's batch.
 * The caller's transfer qualification and request owner cover completion. */
static __attribute__((noinline)) enum kernel_block_status partition_write_batch_prio(
    struct kernel_block_device *device, const struct kernel_block_span *spans, size_t count,
    enum kernel_block_priority priority)
{
    if(count>PARTITION_SMALL_BATCH)return partition_write_batch_large(device,spans,count,priority);
    struct kernel_block_span translated[PARTITION_SMALL_BATCH];
    return partition_write_translated(device,spans,translated,count,priority);
}

enum kernel_block_status kernel_block_write_batch_prio(
    struct kernel_block_device *device,
    const struct kernel_block_span *spans,
    size_t count,
    enum kernel_block_priority priority)
{
    if (!device || !device->logical_block_size)
        return KERNEL_BLOCK_STATUS_INVALID;
    if (!physical(device)->write && !physical(device)->write_batch &&
        !physical(device)->write_batch_prio)
        return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    if (count > KERNEL_BLOCK_BATCH_MAX || (count && !spans))
        return KERNEL_BLOCK_STATUS_INVALID;
    BLOCK_REQUEST(device);
    struct kernel_block_device *disk = physical(device);
    KERNEL_LOCK_SCOPE(transfer); kernel_rwlock_read(&disk->transfer_lock,&transfer);
    int rmw=0;
    for (size_t i=0;i<count;i++)
        if (write_needs_rmw(device,spans[i].offset,spans[i].size)) { rmw=1;break; }
    if (rmw) {
        kernel_lock_release(&transfer);
        kernel_rwlock_write(&disk->transfer_lock,&transfer);
    }
    const uint64_t limit = capacity(device);
    int nonempty = 0;
    uint64_t first=UINT64_MAX,last=0,submitted=0;
    for (size_t i = 0; i < count; i++) {
        const struct kernel_block_span *span = &spans[i];
        if (span->offset > limit ||
            (uint64_t)span->size > limit - span->offset)
            return KERNEL_BLOCK_STATUS_OUT_OF_RANGE;
        if (!span->size) continue;
        if (!span->buffer || span->size - 1 > UINTPTR_MAX - (uintptr_t)span->buffer)
            return KERNEL_BLOCK_STATUS_INVALID;
        uint64_t end=span->offset+span->size;
        /* 顺序窗口在已见区间之外；落入间隙的乱序窗口才核对各段。 */
        if (nonempty && span->offset<last && end>first)
            for (size_t j = 0; j < i; j++)
                if (spans[j].size && span->offset < spans[j].offset + spans[j].size &&
                    spans[j].offset < end)
                    return KERNEL_BLOCK_STATUS_INVALID;
        if (span->offset<first) first=span->offset;
        if (end>last) last=end;
        submitted+=span->size;
        nonempty = 1;
    }
    if (!nonempty) return KERNEL_BLOCK_STATUS_OK;
    if (kernel_block_read_only(device)) return KERNEL_BLOCK_STATUS_READ_ONLY;
    if (disk->write_batch_prio || disk->write_batch) {
        uint64_t started = block_io_begin(device);
        enum kernel_block_status result;
        if (device->parent)
            result = partition_write_batch_prio(device, spans, count, priority);
        else if (disk->write_batch_prio)
            result = disk->write_batch_prio(disk->context, spans, count, priority);
        else result = disk->write_batch(disk->context, spans, count);
        block_io_end(device, BLOCK_IO_WRITE, started, result ? 0 : submitted);
        return result;
    }
    for (size_t i = 0; i < count; i++) {
        if (!spans[i].size) continue;
        enum kernel_block_status result = write_backend(device,
            spans[i].offset, spans[i].buffer, spans[i].size);
        if (result != KERNEL_BLOCK_STATUS_OK) return result;
    }
    return KERNEL_BLOCK_STATUS_OK;
}

enum kernel_block_status kernel_block_write_batch(
    struct kernel_block_device *device,
    const struct kernel_block_span *spans,
    size_t count)
{
    struct kernel_io_context *io_ctx = kernel_io_context_current();
    enum kernel_block_priority prio = (io_ctx && io_ctx->background_reclaim) ?
        KERNEL_BLOCK_PRIO_LOW : KERNEL_BLOCK_PRIO_NORMAL;
    return kernel_block_write_batch_prio(device, spans, count, prio);
}

static int memory_overlaps(uintptr_t a, size_t an, uintptr_t b, size_t bn)
{ return an && bn && (a < b ? b - a < an : a - b < bn); }

#define BLOCK_TRANSFER_BYTES (128U * 1024U)
struct phase_window {
    struct kernel_block_span spans[KERNEL_BLOCK_BATCH_MAX];
    size_t count;
};
struct translated_phase {
    kernel_block_phase_next_fn next;
    const void *context;
    uint64_t offset;
};
static int translated_phase_next(const void *context, const void **cursor,
    struct kernel_block_span *span)
{
    const struct translated_phase *phase = context;
    if (!phase->next(phase->context, cursor, span)) return 0;
    span->offset += phase->offset;
    return 1;
}

/* Synchronous backends have no pending pipeline. Return their transfer
 * qualification between bounded windows after all writes have completed. */
static __attribute__((noinline)) enum kernel_block_status write_phase_windows(
    struct kernel_block_device *device,kernel_block_phase_next_fn next,const void *context,const void *cursor)
{
    struct kernel_block_device *disk=physical(device);
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
        /* Preflight proved sector alignment. Only RMW needs exclusivity;
         * the phase barrier independently keeps FLUSH behind all windows. */
        KERNEL_LOCK_SCOPE(transfer);kernel_rwlock_read(&disk->transfer_lock,&transfer);
        if (disk->write_batch) {
            uint64_t started = block_io_begin(device);
            enum kernel_block_status result=disk->write_batch(disk->context,window.spans,window.count);
            block_io_end(device, BLOCK_IO_WRITE, started, result ? 0 : bytes);
            if (result) return result;
        } else for (size_t i=0;i<window.count;i++) {
            const struct kernel_block_span *span=&window.spans[i];
            enum kernel_block_status result=write_backend(device,span->offset-device->start_bytes,span->buffer,span->size);
            if (result) return result;
        }
    } while (!ended || pending.size);
    return KERNEL_BLOCK_STATUS_OK;
}

enum kernel_block_status kernel_block_write_phase(struct kernel_block_device *device,
    kernel_block_phase_next_fn next, const void *next_context, const void *first)
{
    if (!device || !device->logical_block_size || !next) return KERNEL_BLOCK_STATUS_INVALID;
    if (!physical(device)->write && !physical(device)->write_batch && !physical(device)->write_phase) return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    BLOCK_REQUEST(device);
    struct kernel_block_device *disk = physical(device);
    KERNEL_LOCK_SCOPE(io); kernel_rwlock_read(&disk->phase_barrier, &io);
    const uint64_t limit = capacity(device);
    const void *cursor = first;
    struct kernel_block_span span;
    uint64_t submitted = 0;
    int nonempty = 0;
    while (next(next_context, &cursor, &span)) {
        if (span.offset > limit || span.size > limit - span.offset)
            return KERNEL_BLOCK_STATUS_OUT_OF_RANGE;
        if (!span.size) return KERNEL_BLOCK_STATUS_INVALID;
        if (!span.buffer || span.size - 1 > UINTPTR_MAX - (uintptr_t)span.buffer ||
            span.offset % device->logical_block_size || span.size % device->logical_block_size ||
            write_needs_rmw(device,span.offset,span.size))
            return KERNEL_BLOCK_STATUS_INVALID;
        submitted += span.size;
        nonempty = 1;
    }
    if (!nonempty) return KERNEL_BLOCK_STATUS_OK;
    if (kernel_block_read_only(device)) return KERNEL_BLOCK_STATUS_READ_ONLY;
    if (disk->write_phase) {
        /* Keep the immutable producer live until every DMA completes. The
         * driver refills free request slots without draining artificial windows.
         * Aligned peers remain concurrent; RMW waits for this finite phase. */
        KERNEL_LOCK_SCOPE(transfer); kernel_rwlock_read(&disk->transfer_lock, &transfer);
        uint64_t started = block_io_begin(device);
        const struct translated_phase phase = { next, next_context, device->start_bytes };
        enum kernel_block_status result = device->start_bytes ?
            disk->write_phase(disk->context, translated_phase_next, &phase, first) :
            disk->write_phase(disk->context, next, next_context, first);
        block_io_end(device, BLOCK_IO_WRITE, started, result ? 0 : submitted);
        return result;
    }
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

static enum kernel_block_status partition_read_translated(
    struct kernel_block_device *device, struct block_read_span *spans,
    struct block_read_span *translated, size_t count,
    const struct block_read_completion *completion)
{
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

static __attribute__((noinline)) enum kernel_block_status partition_read_batch_large(
    struct kernel_block_device *device, struct block_read_span *spans, size_t count,
    const struct block_read_completion *completion)
{
    struct block_read_span translated[KERNEL_BLOCK_BATCH_MAX];
    return partition_read_translated(device,spans,translated,count,completion);
}

static __attribute__((noinline)) enum kernel_block_status partition_read_batch(
    struct kernel_block_device *device, struct block_read_span *spans, size_t count,
    const struct block_read_completion *completion)
{
    if(count>PARTITION_SMALL_BATCH)return partition_read_batch_large(device,spans,count,completion);
    struct block_read_span translated[PARTITION_SMALL_BATCH];
    return partition_read_translated(device,spans,translated,count,completion);
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
    uint64_t submitted=0;
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
        if (submitted && buffer <= last && end >= first)
            for (size_t j = 0; j < i; j++)
                if (memory_overlaps(buffer, span->size, (uintptr_t)spans[j].buffer, spans[j].size))
                    return span->status = KERNEL_BLOCK_STATUS_INVALID;
        if (buffer < first) first = buffer;
        if (end > last) last = end;
        submitted+=span->size;
    }
    if (!submitted) return KERNEL_BLOCK_STATUS_OK;
    /* The validated nonoverlapping destinations fit one address space.
     * Account the complete submission with one task lookup and update. */
    kernel_task_account_read(submitted);
    if (disk->read_batch) {
        uint64_t started = block_io_begin(device);
        enum kernel_block_status result = device->parent ?
            partition_read_batch(device, spans, count, completion) :
            disk->read_batch(disk->context, spans, count, completion);
        uint64_t completed = 0;
        for (size_t i = 0; i < count; i++) completed += spans[i].completed;
        block_io_end(device, BLOCK_IO_READ, started, completed);
        return result;
    }
    enum kernel_block_status result = KERNEL_BLOCK_STATUS_OK;
    for (size_t i = 0; i < count; i++) {
        if (spans[i].size) {
            spans[i].status = read_backend(device, spans[i].offset, spans[i].buffer, spans[i].size);
            if (spans[i].status == KERNEL_BLOCK_STATUS_OK) spans[i].completed = spans[i].size;
            else if (result == KERNEL_BLOCK_STATUS_OK) result = spans[i].status;
        }
        if (completion && i + 1 < count) completion->complete(completion->context, i, &spans[i]);
    }
    return result;
}

static struct kernel_block_device *registered_devices;
static kernel_block_devnode_fn devnode_publisher;
static uint64_t registration_generation;

void kernel_block_set_devnode_publisher(kernel_block_devnode_fn fn)
{
    uintptr_t irq = registry_lock();
    devnode_publisher = fn;
    registry_unlock(irq);
}

int kernel_block_snapshot(struct kernel_block_device **devices, size_t capacity, size_t *count)
{
    uintptr_t irq = registry_lock();
    size_t n = 0;
    for (struct kernel_block_device *it = registered_devices; it; it = it->registry_next)
        if (it->registered) n++;
    *count = n;
    if (n > capacity) { registry_unlock(irq); return -KERNEL_ENOSPC; }
    n = 0;
    for (struct kernel_block_device *it = registered_devices; it; it = it->registry_next)
        if (it->registered) {
            if (it->references == UINT32_MAX) __builtin_trap();
            it->references++;
            devices[n++] = it;
        }
    registry_unlock(irq);
    return 0;
}

static struct kernel_block_stat_operation block_operation_snapshot(
    const struct kernel_block_stat_operation *operation)
{
    return (struct kernel_block_stat_operation){
        .completed = __atomic_load_n(&operation->completed, __ATOMIC_RELAXED),
        .sectors = __atomic_load_n(&operation->sectors, __ATOMIC_RELAXED),
        .elapsed_ns = __atomic_load_n(&operation->elapsed_ns, __ATOMIC_RELAXED),
    };
}

int kernel_block_stat(uint64_t number, uint64_t generation, struct kernel_block_usage *out)
{
    uintptr_t irq = registry_lock();
    struct kernel_block_device *device = registered_devices;
    while (device && (!device->registered || device->device_number != number ||
            device->registration_generation != generation))
        device = device->registry_next;
    if (device) {
        struct kernel_block_usage *usage = &device->usage;
        *out = (struct kernel_block_usage){
            .read = block_operation_snapshot(&usage->read),
            .write = block_operation_snapshot(&usage->write),
            .flush = block_operation_snapshot(&usage->flush),
            .in_flight = __atomic_load_n(&usage->in_flight, __ATOMIC_RELAXED),
            .busy_milliseconds = __atomic_load_n(&usage->busy_milliseconds, __ATOMIC_RELAXED),
        };
    }
    registry_unlock(irq);
    return device ? 0 : -KERNEL_ENODEV;
}

static void remove_device_locked(struct kernel_block_device *device)
{
    struct kernel_block_device **link = &registered_devices;
    while (*link && *link != device) link = &(*link)->registry_next;
    if (!*link) __builtin_trap();
    *link = device->registry_next;
    device->registry_next = NULL;
    if (device->parent) {
        if (!device->parent->references) __builtin_trap();
        device->parent->references--;
    }
}

static void activate_devices(struct kernel_block_device *const *devices, size_t count)
{
    /* devtmpfs owns the directory lock here. No registry owner calls into
     * the filesystem with the registry lock held. */
    uintptr_t irq = registry_lock();
    for (size_t i = 0; i < count; i++) {
        if (!devices[i]->registering || devices[i]->registered) __builtin_trap();
        devices[i]->registered = 1;
    }
    registry_unlock(irq);
}

/* Numbers and parent references have already been reserved under *irq.
 * Retain registration ownership until the publisher finishes notifying users. */
static int publish_devices_locked(struct kernel_block_device *const *devices,
    size_t count, uintptr_t *irq)
{
    for (size_t i = 0; i < count; i++) {
        if (++registration_generation == 0) __builtin_trap();
        devices[i]->registration_generation = registration_generation;
        devices[i]->usage = (struct kernel_block_usage){0};
    }
    kernel_block_devnode_fn publish = devnode_publisher;
    if (!publish) {
        for (size_t i = 0; i < count; i++) devices[i]->registered = 1;
        return 0;
    }
    for (size_t i = 0; i < count; i++) devices[i]->registering = 1;
    registry_unlock(*irq);
    int error = publish(devices, count, 1, activate_devices);
    *irq = registry_lock();
    for (size_t i = 0; i < count; i++) {
        struct kernel_block_device *device = devices[i];
        if (device->registered != (unsigned)!error) __builtin_trap();
        if (error) remove_device_locked(device);
        device->registering = 0;
    }
    return error;
}

int kernel_block_register(struct kernel_block_device *device, uint64_t number)
{
    if (!device || !physical(device)->read || !device->logical_block_size)
        return -KERNEL_EINVAL;
    uintptr_t irq = registry_lock();
    int error = 0;
    if (device->registered || device->registering || device->references || device->notifications || device->claim_owner) error = -KERNEL_EBUSY;
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
        registered_devices = device;
        error = publish_devices_locked(&device, 1, &irq);
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
    else if (device->registering || device->references || device->notifications || device->claim_owner) error = -KERNEL_EBUSY;
    else {
        /* Stop new opens while retaining the name reservation. An existing
         * open, partition or mount already prevented us from entering here. */
        device->registered = 0;
        kernel_block_devnode_fn publish = devnode_publisher;
        if (publish) {
            device->registering = 1;
            registry_unlock(irq);
            error = publish(&device, 1, 0, NULL);
            irq = registry_lock();
            device->registering = 0;
        }
        if (error) device->registered = 1;
        else remove_device_locked(device);
    }
    registry_unlock(irq);
    return error;
}

struct kernel_block_device *kernel_block_lookup(uint64_t number)
{
    uintptr_t irq = registry_lock();
    struct kernel_block_device *device = registered_devices;
    while (device && (!device->registered || device->device_number != number)) device = device->registry_next;
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
    while (device && (!device->registered || device->device_number != number)) device = device->registry_next;
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
        (!disk->write && !disk->write_batch && !disk->write_batch_prio &&
         !disk->write_phase);
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
            result = writing ? write_backend(device, offset, bytes, amount)
                             : read_backend(device, offset, bytes, amount);
        } else {
            submitted = span;
            kernel_task_account_read(span);
            result = read_backend(device, first, scratch, span);
            if (result != KERNEL_BLOCK_STATUS_OK) return result;
            if (writing) {
                __builtin_memcpy((unsigned char *)scratch + within, bytes, amount);
                result = write_backend(device, first, scratch, span);
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

static int scan_mbr_sector(struct kernel_block_device *device,
    struct kernel_block_device children[4], const unsigned char sector[512])
{
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

int kernel_block_scan_mbr(struct kernel_block_device *device,
    struct kernel_block_device children[4])
{
    if (!device || !children || !device->registered || device->parent ||
        device->logical_block_size != 512) return -KERNEL_EINVAL;
    unsigned char sector[512];
    enum kernel_block_status status = kernel_block_read_at(device, 0, sector, sizeof(sector));
    if (status != KERNEL_BLOCK_STATUS_OK) return -KERNEL_EIO;
    return scan_mbr_sector(device, children, sector);
}

int kernel_block_scan_partitions(struct kernel_block_device *device,
    struct kernel_block_device children[KERNEL_BLOCK_GPT_PARTITIONS])
{
    if (!device || !children || !device->registered || device->parent ||
        device->logical_block_size != 512) return -KERNEL_EINVAL;
    unsigned char sector[512];
    if (kernel_block_read_at(device, 0, sector, sizeof(sector)) != KERNEL_BLOCK_STATUS_OK)
        return -KERNEL_EIO;
    if (sector[510] == 0x55 && sector[511] == 0xaa)
        for (unsigned i = 0; i < 4; i++)
            if (sector[446 + i * 16 + 4] == 0xee)
                /* Recheck the PMBR under GPT's exclusive transfer lock. */
                return kernel_block_scan_gpt(device, children, KERNEL_BLOCK_GPT_PARTITIONS);
    return scan_mbr_sector(device, children, sector);
}

/* GPT discovery is explicit, like MBR discovery: the caller owns unpublished
 * child storage and the parent lifetime. Never expose half a partition table. */
#define GPT_ENTRY_LIMIT 128U
struct gpt_partition { uint64_t first, count; unsigned char guid[16]; };
struct gpt_header {
    uint64_t first, last, entries;
    uint32_t count, size, crc;
    unsigned char guid[16];
};
static uint64_t gpt_u64(const unsigned char *p)
{ return (uint64_t)mbr_u32(p) | (uint64_t)mbr_u32(p + 4) << 32; }
static uint32_t gpt_crc(uint32_t crc, const unsigned char *p, size_t size)
{
    while (size--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
    }
    return crc;
}
static int gpt_nonzero(const unsigned char *p, unsigned size)
{
    unsigned char bits = 0;
    while (size--) bits |= *p++;
    return bits != 0;
}
/* The transfer write lock excludes both aligned writers and capacity changes.
 * Call the backend directly: recursive public read_at would reacquire it. */
static int gpt_read(struct kernel_block_device *disk, uint64_t lba, unsigned char sector[512])
{
    kernel_task_account_read(512);
    return read_backend(disk, lba * 512, sector, 512) == KERNEL_BLOCK_STATUS_OK ? 0 : -KERNEL_EIO;
}
static int gpt_copy(struct kernel_block_device *disk, uint64_t where, uint64_t end,
    struct gpt_header *header, struct gpt_partition parts[KERNEL_BLOCK_GPT_PARTITIONS], size_t slots)
{
    unsigned char sector[512];
    if (gpt_read(disk, where, sector)) return -KERNEL_EIO;
    if (__builtin_memcmp(sector, "EFI PART", 8) || mbr_u32(sector + 8) != 0x10000 ||
        mbr_u32(sector + 20) || gpt_u64(sector + 24) != where ||
        gpt_u64(sector + 32) != (where == 1 ? end : 1)) return -KERNEL_EINVAL;
    uint32_t size = mbr_u32(sector + 12), crc = mbr_u32(sector + 16);
    if (size < 92 || size > 512) return -KERNEL_EINVAL;
    __builtin_memset(sector + 16, 0, 4);
    if (~gpt_crc(UINT32_MAX, sector, size) != crc) return -KERNEL_EINVAL;
    header->first = gpt_u64(sector + 40); header->last = gpt_u64(sector + 48);
    __builtin_memcpy(header->guid, sector + 56, 16);
    header->entries = gpt_u64(sector + 72); header->count = mbr_u32(sector + 80);
    header->size = mbr_u32(sector + 84); header->crc = mbr_u32(sector + 88);
    if (!header->count || header->first > header->last || header->last >= end ||
        !gpt_nonzero(header->guid, 16)) return -KERNEL_EINVAL;
    /* Bound boot work independently of untrusted disk fields. Larger entry
     * layouts can be added without changing the device-number contract. */
    if (header->count > GPT_ENTRY_LIMIT ||
        (header->size != 128 && header->size != 256 && header->size != 512)) return -KERNEL_ENOTSUP;
    uint64_t bytes = (uint64_t)header->count * header->size;
    uint64_t sectors = (bytes + 511) / 512;
    uint64_t reserved = sectors < 32 ? 32 : sectors; /* GPT reserves at least 16 KiB. */
    if (reserved >= end || header->first < 2 + reserved || header->last >= end - reserved ||
        header->entries < 2 || header->entries >= end || sectors > end - header->entries)
        return -KERNEL_EINVAL;
    if (where == 1 ? header->entries + sectors > header->first :
        header->entries <= header->last) return -KERNEL_EINVAL;
    __builtin_memset(parts, 0, sizeof(*parts) * KERNEL_BLOCK_GPT_PARTITIONS);
    int unsupported_slot = 0;
    crc = UINT32_MAX;
    for (uint64_t block = 0; block < sectors; block++) {
        if (gpt_read(disk, header->entries + block, sector)) return -KERNEL_EIO;
        size_t n = bytes < 512 ? (size_t)bytes : 512;
        crc = gpt_crc(crc, sector, n); bytes -= n;
        for (unsigned offset = 0; offset < n; offset += header->size) {
            const unsigned char *entry = sector + offset;
            unsigned index = (unsigned)((block * 512 + offset) / header->size);
            if (!gpt_nonzero(entry, 16)) continue;
            if (index >= slots) {
                unsupported_slot = 1;
                continue;
            }
            uint64_t first = gpt_u64(entry + 32), last = gpt_u64(entry + 40);
            if (first < header->first || last > header->last || first > last ||
                !gpt_nonzero(entry + 16, 16)) return -KERNEL_EINVAL;
            for (unsigned j = 0; j < index; j++) {
                if (!parts[j].count) continue;
                if ((first < parts[j].first + parts[j].count && parts[j].first <= last) ||
                    !__builtin_memcmp(parts[j].guid, entry + 16, 16)) return -KERNEL_EINVAL;
            }
            parts[index].first = first; parts[index].count = last - first + 1;
            __builtin_memcpy(parts[index].guid, entry + 16, 16);
        }
    }
    /* A corrupt type GUID in an unused entry must not mask a good copy. */
    if (~crc != header->crc) return -KERNEL_EINVAL;
    return unsupported_slot ? -KERNEL_ENOSPC : 0;
}
int kernel_block_scan_gpt(struct kernel_block_device *disk,
    struct kernel_block_device *children, size_t slots)
{
    if (!disk || !children || !slots || slots > KERNEL_BLOCK_GPT_PARTITIONS ||
        !disk->registered || disk->parent || !disk->read || disk->logical_block_size != 512 ||
        (disk->device_number & 15U)) return -KERNEL_EINVAL;
    BLOCK_REQUEST(disk);
    KERNEL_LOCK_SCOPE(transfer); kernel_rwlock_write(&disk->transfer_lock, &transfer);
    uint64_t bytes = capacity(disk);
    if (bytes < 68 * 512 || bytes % 512) return -KERNEL_EINVAL;
    uint64_t end = bytes / 512 - 1;
    unsigned char sector[512];
    if (gpt_read(disk, 0, sector)) return -KERNEL_EIO;
    if (sector[510] != 0x55 || sector[511] != 0xaa) return -KERNEL_EINVAL;
    unsigned protective = 0;
    for (unsigned i = 0; i < 4; i++) {
        const unsigned char *p = sector + 446 + 16 * i;
        if (!gpt_nonzero(p, 16)) continue;
        if (p[4] != 0xee) return -KERNEL_ENOTSUP; /* No hybrid MBR. */
        if (p[0] || mbr_u32(p + 8) != 1 ||
            mbr_u32(p + 12) != (end > UINT32_MAX ? UINT32_MAX : (uint32_t)end))
            return -KERNEL_EINVAL;
        protective++;
    }
    if (protective != 1) return -KERNEL_EINVAL;
    struct gpt_header primary, backup;
    struct gpt_partition first[KERNEL_BLOCK_GPT_PARTITIONS], second[KERNEL_BLOCK_GPT_PARTITIONS];
    int a = gpt_copy(disk, 1, end, &primary, first, slots);
    if (a == -KERNEL_ENOTSUP || a == -KERNEL_ENOSPC) return a;
    int b = gpt_copy(disk, end, end, &backup, second, slots);
    if (b == -KERNEL_ENOTSUP || b == -KERNEL_ENOSPC) return b;
    if (a && b) return a == -KERNEL_EIO || b == -KERNEL_EIO ? -KERNEL_EIO : -KERNEL_EINVAL;
    if (!a && !b && (primary.first != backup.first || primary.last != backup.last ||
        primary.count != backup.count || primary.size != backup.size || primary.crc != backup.crc ||
        __builtin_memcmp(primary.guid, backup.guid, 16) || __builtin_memcmp(first, second, sizeof(first))))
        return -KERNEL_EINVAL;
    const struct gpt_partition *parts = a ? second : first;
    /* Preflight the whole publication under the registry lock. No unregister
     * rollback can race a consumer acquiring a partially published child. */
    uintptr_t irq = registry_lock();
    int error = 0;
    size_t used = 0;
    struct kernel_block_device *batch[KERNEL_BLOCK_GPT_PARTITIONS];
    for (size_t i = 0; i < slots; i++) {
        if (children[i].registered || children[i].registering || children[i].references || children[i].notifications || children[i].claim_owner)
            error = -KERNEL_EBUSY;
        if (!parts[i].count) continue;
        batch[used++] = &children[i];
        for (struct kernel_block_device *it = registered_devices; it; it = it->registry_next)
            if (it->device_number == disk->device_number + i + 1) error = -KERNEL_EBUSY;
    }
    if (!disk->registered || used > UINT32_MAX - disk->references) error = -KERNEL_EBUSY;
    if (!error) for (size_t i = 0; i < slots; i++) {
        if (!parts[i].count) continue;
        children[i] = (struct kernel_block_device){ .parent = disk,
            .start_bytes = parts[i].first * 512, .capacity_bytes = parts[i].count * 512,
            .logical_block_size = 512, .device_number = disk->device_number + i + 1 };
        prepare_locked(&children[i]); disk->references++;
        children[i].registry_next = registered_devices;
        registered_devices = &children[i];
    }
    if (!error && used) error = publish_devices_locked(batch, used, &irq);
    registry_unlock(irq);
    return error;
}
