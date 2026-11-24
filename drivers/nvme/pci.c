#include <kernel/nvme.h>
#include <arch/bus.h>
#include <arch/timer.h>
#include <kernel/block.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/pci.h>
#include <kernel/physical_page.h>
#include <kernel/raw_lock.h>
#include <kernel/scheduler.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include "../../fs/char_device_internal.h"
#include "../../fs/open_file_internal.h"
#include "../../fs/uaccess_iov_internal.h"
#include <stdbool.h>
#include <string.h>

enum {
    NVME_CAP = 0x00, NVME_INTMS = 0x0c, NVME_INTMC = 0x10,
    NVME_CC = 0x14, NVME_CSTS = 0x1c, NVME_AQA = 0x24,
    NVME_ASQ = 0x28, NVME_ACQ = 0x30, NVME_DOORBELL = 0x1000,
    ADMIN_CREATE_SQ = 1, ADMIN_GET_LOG = 2, ADMIN_CREATE_CQ = 5,
    ADMIN_IDENTIFY = 6, ADMIN_GET_FEATURE = 10,
    NVM_FLUSH = 0, NVM_WRITE = 1, NVM_READ = 2, NVM_COMPARE = 5,
    NVME_PAGE = 4096, NVME_DEPTH = 32, NVME_PRP_LIST = 256, NVME_BATCH = 8,
    NVME_MAX_BYTES = 128 * 1024, NVME_MAX_NAMESPACES = 64,
    NVME_BLOCK_MAJOR = 259, NVME_COMMAND_MAJOR = 240,
};

struct nvme_command {
    uint8_t opcode, flags;
    uint16_t cid;
    uint32_t nsid, dword2, dword3;
    uint64_t metadata, prp1, prp2;
    uint32_t dword[6];
};

struct nvme_completion {
    uint64_t result;
    uint16_t submission_head, submission_id, cid, status;
};

struct nvme_user_command {
    uint8_t opcode, flags;
    uint16_t reserved1;
    uint32_t nsid, dword2, dword3;
    uint64_t metadata, address;
    uint32_t metadata_len, length, dword[6], timeout_ms, reserved2;
};

_Static_assert(sizeof(struct nvme_command) == 64, "NVMe submission entry");
_Static_assert(sizeof(struct nvme_completion) == 16, "NVMe completion entry");
_Static_assert(sizeof(struct nvme_user_command) == 72, "Linux NVMe uring command");
_Static_assert(NVME_DEPTH * sizeof(struct nvme_command) <= NVME_PAGE, "submission allocation");
_Static_assert(NVME_MAX_BYTES / NVME_PAGE <= NVME_PRP_LIST / 8, "PRP capacity");

struct nvme_dma {
    void *data;
    uint64_t physical;
    unsigned order;
};

enum nvme_slot_state { SLOT_FREE, SLOT_RESERVED, SLOT_SUBMITTED, SLOT_COMPLETED };

struct nvme_slot {
    struct kernel_wait_queue done;
    struct nvme_dma buffer;
    uint64_t result;
    int status;
    enum nvme_slot_state state;
};

struct nvme_queue {
    struct nvme_dma submission, completion, prps;
    struct kernel_wait_queue available;
    struct nvme_slot slots[NVME_DEPTH];
    uint32_t free_slots;
    unsigned id, head, tail, depth, phase, in_use;
};

struct nvme_controller;
struct nvme_namespace;

struct nvme_target {
    struct kernel_char_device device;
    struct nvme_controller *controller;
    struct nvme_namespace *ns;
    unsigned opens;
};

struct nvme_namespace {
    struct kernel_block_device block;
    struct nvme_namespace *next;
    struct nvme_controller *controller;
    uint32_t nsid;
    uint32_t block_minor;
    struct nvme_target target;
};

enum nvme_state { NVME_NEW, NVME_READY, NVME_RESETTING, NVME_STOPPED, NVME_DEAD };

struct nvme_controller {
    struct nvme_controller *next;
    struct nvme_namespace *namespaces;
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct pci_function function;
    struct kernel_raw_lock lock;
    volatile uint8_t *registers;
    struct nvme_queue admin, io;
    struct nvme_target target;
    uint64_t reset_ticks;
    unsigned index;
    uint32_t irq, stride, max_transfer, timebase;
    enum nvme_state state;
};

static struct nvme_controller *controllers;
static unsigned next_disk_minor, next_command_minor, next_controller;
static void initialize_target(struct nvme_controller *, struct nvme_namespace *,
    struct nvme_target *);

static uint32_t reg_read(const struct nvme_controller *c, unsigned offset)
{ return *(volatile const uint32_t *)(c->registers + offset); }

static void reg_write(const struct nvme_controller *c, unsigned offset, uint32_t value)
{
    arch_io_barrier();
    *(volatile uint32_t *)(c->registers + offset) = value;
}

static void reg_write64(const struct nvme_controller *c, unsigned offset, uint64_t value)
{
    reg_write(c, offset, (uint32_t)value);
    reg_write(c, offset + 4, (uint32_t)(value >> 32));
}

static void doorbell(struct nvme_controller *c, struct nvme_queue *q, bool completion)
{
    reg_write(c, NVME_DOORBELL + (2 * q->id + completion) * c->stride,
        completion ? q->head : q->tail);
}

static void dma_release(struct nvme_controller *c, struct nvme_dma *dma)
{
    if (!dma->data) return;
    if (physical_page_release_order(c->allocator, dma->physical, dma->order))
        __builtin_trap();
    *dma = (struct nvme_dma){0};
}

/* A failed controller can retain its queue and transfer pages together. The
 * synchronous block/file adapters never loan transient buffers to a device
 * whose reset has not been confirmed. */
static int dma_resize(struct nvme_controller *c, struct nvme_dma *dma, unsigned bytes)
{
    if (dma->data && bytes <= BOAROS_PAGE_SIZE << dma->order) return 0;
    struct nvme_dma replacement = {0};
    while (bytes > BOAROS_PAGE_SIZE << replacement.order) replacement.order++;
    if (physical_page_allocate_order(c->allocator, replacement.order, &replacement.physical))
        return -KERNEL_ENOMEM;
    if (physical_page_map_owned(c->allocator, replacement.physical, &replacement.data)) {
        if (physical_page_release_order(c->allocator, replacement.physical, replacement.order))
            __builtin_trap();
        return -KERNEL_EIO;
    }
    dma_release(c, dma);
    *dma = replacement;
    return 0;
}

static int queue_init(struct nvme_controller *c, struct nvme_queue *q, unsigned id,
    unsigned depth)
{
    q->id = id;
    q->depth = depth;
    q->phase = 1;
    q->free_slots = UINT32_MAX >> (32 - depth);
    kernel_wait_queue_init(&q->available);
    for (unsigned i = 0; i < depth; i++) kernel_wait_queue_init(&q->slots[i].done);
    int result = dma_resize(c, &q->submission, NVME_PAGE);
    if (!result) result = dma_resize(c, &q->completion, NVME_PAGE);
    if (!result) result = dma_resize(c, &q->prps, depth * NVME_PRP_LIST);
    if (result) return result;
    memset(q->submission.data, 0, NVME_PAGE);
    memset(q->completion.data, 0, NVME_PAGE);
    return 0;
}

static void queue_destroy(struct nvme_controller *c, struct nvme_queue *q)
{
    if (!q->depth) return;
    if (q->in_use) __builtin_trap();
    for (unsigned i = 0; i < q->depth; i++) {
        struct nvme_slot *slot = &q->slots[i];
        if (kernel_wait_queue_close(&slot->done) || kernel_wait_queue_destroy(&slot->done))
            __builtin_trap();
        dma_release(c, &slot->buffer);
    }
    if (kernel_wait_queue_close(&q->available) || kernel_wait_queue_destroy(&q->available))
        __builtin_trap();
    dma_release(c, &q->prps);
    dma_release(c, &q->completion);
    dma_release(c, &q->submission);
    q->depth = 0;
}

static void wake(struct kernel_wait_queue *queue)
{ (void)kernel_wait_queue_wake_all(queue); }

static bool collect(struct nvme_controller *c, struct nvme_queue *q)
{
    if (!q->completion.data) return true;
    volatile const struct nvme_completion *entries = q->completion.data;
    unsigned old_head = q->head;
    do {
        volatile const struct nvme_completion *entry = &entries[q->head];
        uint16_t status = entry->status;
        if ((status & 1) != q->phase) break;
        arch_dma_barrier();
        uint16_t cid = entry->cid;
        if (cid >= q->depth || entry->submission_id != q->id ||
            entry->submission_head >= q->depth || q->slots[cid].state != SLOT_SUBMITTED)
            return false;
        struct nvme_slot *slot = &q->slots[cid];
        slot->result = entry->result;
        slot->status = status >> 1;
        slot->state = SLOT_COMPLETED;
        if (++q->head == q->depth) {
            q->head = 0;
            q->phase ^= 1;
        }
        wake(&slot->done);
    } while (q->head != old_head);
    if (q->head != old_head) doorbell(c, q, true);
    return true;
}

static void fail_queue(struct nvme_queue *q)
{
    for (unsigned i = 0; i < q->depth; i++) {
        struct nvme_slot *slot = &q->slots[i];
        if (slot->state == SLOT_SUBMITTED) {
            slot->state = SLOT_COMPLETED;
            slot->result = 0;
            slot->status = -KERNEL_EIO;
            wake(&slot->done);
        }
    }
    if (q->depth) wake(&q->available);
}

static void fail_locked(struct nvme_controller *c, enum nvme_state state)
{
    c->state = state;
    fail_queue(&c->admin);
    fail_queue(&c->io);
}

static void interrupt(void *owner)
{
    struct nvme_controller *c = owner;
    KERNEL_IRQ_LOCK_SCOPE(guard, &c->lock);
    if (c->state != NVME_READY) return;
    if (!collect(c, &c->admin) || !collect(c, &c->io)) {
        reg_write(c, NVME_INTMS, UINT32_MAX);
        fail_locked(c, NVME_DEAD);
    }
}

static int wait_ready(struct nvme_controller *c, bool ready)
{
    uint64_t limit = arch_time_read_ordered() + c->reset_ticks;
    do {
        uint32_t value = reg_read(c, NVME_CSTS);
        if (value == UINT32_MAX) return -KERNEL_ENODEV;
        if ((bool)(value & 1) == ready) return 0;
        if (ready && (value & 2)) return -KERNEL_EIO;
    } while ((int64_t)(arch_time_read_ordered() - limit) < 0);
    return -KERNEL_ETIMEDOUT;
}

static int stop_controller(struct nvme_controller *c)
{
    reg_write(c, NVME_INTMS, UINT32_MAX);
    uint32_t config = reg_read(c, NVME_CC);
    if (config == UINT32_MAX) return -KERNEL_ENODEV;
    if (config & 1) reg_write(c, NVME_CC, config & ~1U);
    return wait_ready(c, false);
}

static int wait_progress(struct nvme_controller *c, struct kernel_wait_queue *queue,
    uint64_t until, kernel_irq_lock_guard *guard)
{
    if ((int64_t)(arch_time_read_ordered() - until) >= 0) return -KERNEL_ETIMEDOUT;
    if (kernel_scheduler_can_sleep() && c->irq) {
        struct kernel_wait_token token;
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_prepare(queue, until, 0, &token)) __builtin_trap();
        kernel_irq_lock_release(guard);
        if (kernel_wait_park(&token, &reason) || kernel_wait_finish(&token)) __builtin_trap();
        kernel_irq_lock_acquire(&c->lock, guard);
    } else {
        kernel_irq_lock_release(guard);
        kernel_irq_lock_acquire(&c->lock, guard);
    }
    return (int64_t)(arch_time_read_ordered() - until) >= 0 ? -KERNEL_ETIMEDOUT : 0;
}

static int reserve_slots(struct nvme_controller *c, struct nvme_queue *q, uint64_t until,
    unsigned *cids, unsigned wanted)
{
    KERNEL_IRQ_LOCK_SCOPE(guard, &c->lock);
    while (c->state == NVME_READY) {
        unsigned count = 0;
        uint32_t available = q->free_slots;
        while (count < wanted && q->in_use < q->depth - 1) {
            unsigned index = 0;
            uint32_t mask = 1;
            while (!(available & mask)) { mask <<= 1; index++; }
            available ^= mask;
            q->free_slots &= ~(UINT32_C(1) << index);
            q->in_use++;
            q->slots[index].state = SLOT_RESERVED;
            cids[count++] = index;
        }
        if (count) return (int)count;
        int error = wait_progress(c, &q->available, until, &guard);
        if (error) return error;
    }
    return -KERNEL_EIO;
}

static int reserve_slot(struct nvme_controller *c, struct nvme_queue *q, uint64_t until,
    unsigned *cid)
{
    int result = reserve_slots(c, q, until, cid, 1);
    return result < 0 ? result : 0;
}

static void return_slots(struct nvme_controller *c, struct nvme_queue *q,
    const unsigned *cids, unsigned count)
{
    KERNEL_IRQ_LOCK_SCOPE(guard, &c->lock);
    for (unsigned i = 0; i < count; i++) {
        unsigned cid = cids[i];
        struct nvme_slot *slot = &q->slots[cid];
        if (slot->state != SLOT_RESERVED && slot->state != SLOT_COMPLETED) __builtin_trap();
        slot->state = SLOT_FREE;
        q->free_slots |= UINT32_C(1) << cid;
    }
    q->in_use -= count;
    wake(&q->available);
}

static void return_slot(struct nvme_controller *c, struct nvme_queue *q, unsigned cid)
{ return_slots(c, q, &cid, 1); }

static void populate_prps(struct nvme_queue *q, struct nvme_command *command,
    unsigned cid, uint64_t address, unsigned length)
{
    command->prp1 = address;
    if (length <= NVME_PAGE) return;
    if (length <= 2 * NVME_PAGE) {
        command->prp2 = address + NVME_PAGE;
        return;
    }
    uint64_t *list = (uint64_t *)((uint8_t *)q->prps.data + cid * NVME_PRP_LIST);
    unsigned pages = (length + NVME_PAGE - 1) / NVME_PAGE;
    for (unsigned i = 1; i < pages; i++) list[i - 1] = address + i * NVME_PAGE;
    command->prp2 = q->prps.physical + cid * NVME_PRP_LIST;
}

static void enqueue_locked(struct nvme_queue *q, struct nvme_command *command, unsigned cid)
{
    struct nvme_slot *slot = &q->slots[cid];
    command->cid = (uint16_t)cid;
    ((struct nvme_command *)q->submission.data)[q->tail] = *command;
    if (++q->tail == q->depth) q->tail = 0;
    slot->state = SLOT_SUBMITTED;
}

static int wait_slot_locked(struct nvme_controller *c, struct nvme_queue *q,
    unsigned cid, uint64_t until, uint64_t *result, kernel_irq_lock_guard *guard)
{
    struct nvme_slot *slot = &q->slots[cid];
    for (;;) {
        if (c->state == NVME_READY && !collect(c, q)) {
            reg_write(c, NVME_INTMS, UINT32_MAX);
            fail_locked(c, NVME_DEAD);
        }
        if (slot->state == SLOT_COMPLETED) break;
        if (!wait_progress(c, &slot->done, until, guard)) continue;
        if (c->state == NVME_READY) {
            if (collect(c, q) && slot->state == SLOT_COMPLETED) break;
            c->state = NVME_RESETTING;
            kernel_irq_lock_release(guard);
            int reset = stop_controller(c);
            kernel_irq_lock_acquire(&c->lock, guard);
            fail_locked(c, reset ? NVME_DEAD : NVME_STOPPED);
            return -KERNEL_ETIMEDOUT;
        }
        /* Another timeout owner is stopping this DMA queue. Its buffers
         * remain attached to the controller even if that reset fails. */
        until = arch_time_read_ordered() + c->reset_ticks;
    }
    if (result) *result = slot->result;
    return slot->status;
}

static int submit(struct nvme_controller *c, struct nvme_queue *q,
    struct nvme_command *command, unsigned cid, uint64_t until, uint64_t *result)
{
    KERNEL_IRQ_LOCK_SCOPE(guard, &c->lock);
    if (c->state != NVME_READY) return -KERNEL_EIO;
    enqueue_locked(q, command, cid);
    doorbell(c, q, false);
    return wait_slot_locked(c, q, cid, until, result, &guard);
}

static uint64_t command_deadline(const struct nvme_controller *c, uint32_t timeout_ms)
{
    if (!timeout_ms) timeout_ms = 30000;
    return arch_time_read_ordered() + ((uint64_t)timeout_ms * c->timebase + 999) / 1000;
}

static int execute(struct nvme_controller *c, struct nvme_queue *q,
    struct nvme_command *command, void *buffer, unsigned size, bool to_device,
    uint32_t timeout_ms, uint64_t *result)
{
    uint64_t limit = command_deadline(c, timeout_ms);
    unsigned cid;
    int error = reserve_slot(c, q, limit, &cid);
    if (error) return error;
    if (size) {
        struct nvme_dma *transfer = &q->slots[cid].buffer;
        error = dma_resize(c, transfer, size);
        if (error) goto out;
        if (to_device) memcpy(transfer->data, buffer, size);
        else if (q == &c->admin) memset(transfer->data, 0, size);
        populate_prps(q, command, cid, transfer->physical, size);
    }
    error = submit(c, q, command, cid, limit, result);
    if (!error && size && !to_device) memcpy(buffer, q->slots[cid].buffer.data, size);
out:
    return_slot(c, q, cid);
    return error;
}

static enum kernel_block_status block_status(int status)
{
    if (!status) return KERNEL_BLOCK_STATUS_OK;
    if (status == -KERNEL_ETIMEDOUT) return KERNEL_BLOCK_STATUS_TIMEOUT;
    if (status == -KERNEL_ENOMEM) return KERNEL_BLOCK_STATUS_NO_MEMORY;
    if (status == -KERNEL_EINVAL) return KERNEL_BLOCK_STATUS_INVALID;
    return KERNEL_BLOCK_STATUS_IO;
}

static enum kernel_block_status rw_namespace(struct nvme_namespace *ns,
    uint64_t offset, void *buffer, size_t bytes, uint8_t opcode)
{
    unsigned sector = ns->block.logical_block_size;
    if (offset % sector || bytes % sector) return KERNEL_BLOCK_STATUS_INVALID;
    struct nvme_controller *c = ns->controller;
    while (bytes) {
        unsigned chunk = bytes > c->max_transfer ? c->max_transfer : (unsigned)bytes;
        uint64_t lba = offset / sector;
        struct nvme_command op = { .opcode = opcode, .nsid = ns->nsid,
            .dword = {(uint32_t)lba, (uint32_t)(lba >> 32), chunk / sector - 1} };
        enum kernel_block_status status = block_status(execute(c, &c->io, &op, buffer, chunk,
            opcode != NVM_READ, 0, NULL));
        if (status) return status;
        offset += chunk;
        bytes -= chunk;
        buffer = (uint8_t *)buffer + chunk;
    }
    return KERNEL_BLOCK_STATUS_OK;
}

static enum kernel_block_status read_namespace(void *context, uint64_t offset, void *buffer,
    size_t bytes)
{ return rw_namespace(context, offset, buffer, bytes, NVM_READ); }

static enum kernel_block_status write_namespace(void *context, uint64_t offset,
    const void *buffer, size_t bytes)
{ return rw_namespace(context, offset, (void *)buffer, bytes, NVM_WRITE); }

static enum kernel_block_status flush_namespace(void *context)
{
    struct nvme_namespace *ns = context;
    struct nvme_controller *c = ns->controller;
    struct nvme_command op = { .opcode = NVM_FLUSH, .nsid = ns->nsid };
    return block_status(execute(c, &c->io, &op, NULL, 0, false, 0, NULL));
}

/* Device slots stay owned until the full published prefix settles. Do not
 * wait for new slots while holding an unpublished prefix. */
static unsigned block_batch(struct nvme_namespace *ns, const struct kernel_block_span *spans,
    unsigned wanted, bool writing, enum kernel_block_status *statuses)
{
    struct nvme_controller *c = ns->controller;
    struct nvme_queue *q = &c->io;
    uint64_t limit = command_deadline(c, 0);
    unsigned cids[NVME_BATCH];
    int reserved = reserve_slots(c, q, limit, cids, wanted);
    if (reserved < 0) { statuses[0] = block_status(reserved); return 1; }
    unsigned count = (unsigned)reserved;
    struct nvme_command commands[NVME_BATCH];
    unsigned valid = 0;
    for (; valid < count; valid++) {
        const struct kernel_block_span *span = &spans[valid];
        struct nvme_dma *dma = &q->slots[cids[valid]].buffer;
        int error = dma_resize(c, dma, (unsigned)span->size);
        if (error) { statuses[valid] = block_status(error); break; }
        unsigned sector = ns->block.logical_block_size;
        uint64_t lba = span->offset / sector;
        commands[valid] = (struct nvme_command){ .opcode = writing ? NVM_WRITE : NVM_READ,
            .nsid = ns->nsid,
            .dword = { (uint32_t)lba, (uint32_t)(lba >> 32), span->size / sector - 1 },
        };
        if (writing) memcpy(dma->data, span->buffer, span->size);
        populate_prps(q, &commands[valid], cids[valid], dma->physical, (unsigned)span->size);
    }
    bool published = false;
    if (valid) {
        KERNEL_IRQ_LOCK_SCOPE(guard, &c->lock);
        if (c->state == NVME_READY) {
            for (unsigned i = 0; i < valid; i++) enqueue_locked(q, &commands[i], cids[i]);
            doorbell(c, q, false);
            published = true;
        } else statuses[0] = KERNEL_BLOCK_STATUS_IO;
    }
    if (published) {
        for (unsigned i = 0; i < valid; i++) {
            int result;
            {
                KERNEL_IRQ_LOCK_SCOPE(guard, &c->lock);
                result = wait_slot_locked(c, q, cids[i], limit, NULL, &guard);
            }
            statuses[i] = block_status(result);
            if (!result && !writing)
                memcpy((void *)spans[i].buffer, q->slots[cids[i]].buffer.data, spans[i].size);
        }
    }
    return_slots(c, q, cids, count);
    return !published ? 1 : valid < count ? valid + 1 : valid;
}

static bool batchable(const struct nvme_namespace *ns, uint64_t offset, size_t bytes)
{
    unsigned sector = ns->block.logical_block_size;
    return bytes && bytes <= ns->controller->max_transfer && !(offset % sector) && !(bytes % sector);
}

static enum kernel_block_status write_batch(void *context, const struct kernel_block_span *spans,
    size_t count)
{
    struct nvme_namespace *ns = context;
    for (size_t i = 0; i < count;) {
        unsigned group = 0;
        while (group < NVME_BATCH && i + group < count &&
            batchable(ns, spans[i + group].offset, spans[i + group].size)) group++;
        if (group < 2) {
            enum kernel_block_status status = write_namespace(ns, spans[i].offset,
                spans[i].buffer, spans[i].size);
            if (status) return status;
            i++;
        } else {
            enum kernel_block_status statuses[NVME_BATCH];
            unsigned completed = block_batch(ns, spans + i, group, true, statuses);
            for (unsigned j = 0; j < completed; j++)
                if (statuses[j]) return statuses[j];
            i += completed;
        }
    }
    return KERNEL_BLOCK_STATUS_OK;
}

static enum kernel_block_status read_batch(void *context, struct block_read_span *spans,
    size_t count, const struct block_read_completion *completion)
{
    struct nvme_namespace *ns = context;
    enum kernel_block_status first = KERNEL_BLOCK_STATUS_OK;
    for (size_t i = 0; i < count;) {
        unsigned group = 0;
        struct kernel_block_span requests[NVME_BATCH];
        while (group < NVME_BATCH && i + group < count) {
            const struct block_read_span *span = &spans[i + group];
            if (!batchable(ns, span->offset, span->size)) break;
            requests[group++] = (struct kernel_block_span){span->offset, span->buffer, span->size};
        }
        enum kernel_block_status statuses[NVME_BATCH];
        unsigned completed;
        if (group < 2) {
            statuses[0] = read_namespace(ns, spans[i].offset, spans[i].buffer, spans[i].size);
            completed = 1;
        } else completed = block_batch(ns, requests, group, false, statuses);
        for (unsigned j = 0; j < completed; j++) {
            struct block_read_span *span = &spans[i + j];
            span->status = statuses[j];
            span->completed = statuses[j] ? 0 : span->size;
            if (!first && statuses[j]) first = statuses[j];
            if (completion && i + j + 1 < count) completion->complete(completion->context, i + j, span);
        }
        i += completed;
    }
    return first;
}

static int identify(struct nvme_controller *c, uint32_t nsid, unsigned selector, void *buffer)
{
    struct nvme_command op = {.opcode = ADMIN_IDENTIFY, .nsid = nsid, .dword = {selector}};
    int result = execute(c, &c->admin, &op, buffer, NVME_PAGE, false, 0, NULL);
    return result > 0 ? -KERNEL_EIO : result;
}

static int add_namespace(struct nvme_controller *c, uint32_t nsid, const uint8_t *identify_data)
{
    uint64_t blocks;
    memcpy(&blocks, identify_data, sizeof(blocks));
    unsigned flbas = identify_data[26];
    unsigned format = (flbas & 15) | ((flbas & 0x60) >> 1);
    const uint8_t *format_data = identify_data + 128 + 4 * format;
    unsigned shift = format_data[2];
    if (!blocks || shift < 9 || shift > 12 || blocks > UINT64_MAX >> shift ||
        format_data[0] || format_data[1] || (identify_data[29] & 7)) return -KERNEL_EOPNOTSUPP;
    struct nvme_namespace *ns;
    if (kernel_heap_allocate_zeroed(c->heap, 1, sizeof(*ns), (void **)&ns))
        return -KERNEL_ENOMEM;
    ns->controller = c;
    ns->nsid = nsid;
    ns->block_minor = next_disk_minor++;
    ns->block = (struct kernel_block_device){
        .context = ns, .read = read_namespace, .write = write_namespace,
        .read_batch = read_batch, .write_batch = write_batch,
        .flush = flush_namespace, .cache_mode = KERNEL_BLOCK_CACHE_WRITEBACK,
        .capacity_bytes = blocks << shift, .logical_block_size = 1U << shift,
    };
    uint64_t number = ((uint64_t)NVME_BLOCK_MAJOR << 8) |
        (ns->block_minor & 0xff) | ((uint64_t)(ns->block_minor & ~0xffU) << 12);
    initialize_target(c, ns, &ns->target);
    ns->next = c->namespaces;
    c->namespaces = ns;
    int error = kernel_block_register(&ns->block, number);
    if (error) {
        c->namespaces = ns->next;
        kernel_heap_release(c->heap, ns);
        return error;
    }
    return 0;
}

static int discover_namespaces(struct nvme_controller *c)
{
    uint8_t *info, *list;
    if (kernel_heap_allocate(c->heap, 2 * NVME_PAGE, (void **)&info)) return -KERNEL_ENOMEM;
    list = info + NVME_PAGE;
    int error = identify(c, 0, 1, info);
    if (error) goto out;
    unsigned mdts = info[77];
    c->max_transfer = mdts && mdts < 5 ? NVME_PAGE << mdts : NVME_MAX_BYTES;
    error = identify(c, 0, 2, list);
    if (error) goto out;
    for (unsigned i = 0; i <= NVME_MAX_NAMESPACES; i++) {
        uint32_t nsid;
        memcpy(&nsid, list + i * sizeof(nsid), sizeof(nsid));
        if (!nsid) break;
        if (i == NVME_MAX_NAMESPACES) { error = -KERNEL_E2BIG; break; }
        error = identify(c, nsid, 0, info);
        if (!error) error = add_namespace(c, nsid, info);
        if (error) break;
    }
out:
    kernel_heap_release(c->heap, info);
    return error;
}

static int create_io_queue(struct nvme_controller *c)
{
    unsigned depth = c->io.depth - 1;
    struct nvme_command create_cq = {
        .opcode = ADMIN_CREATE_CQ, .prp1 = c->io.completion.physical,
        .dword = { (depth << 16) | 1, c->irq ? 3 : 1 },
    };
    int error = execute(c, &c->admin, &create_cq, NULL, 0, false, 0, NULL);
    if (error) return error > 0 ? -KERNEL_EIO : error;
    struct nvme_command create_sq = {
        .opcode = ADMIN_CREATE_SQ, .prp1 = c->io.submission.physical,
        .dword = { (depth << 16) | 1, (1U << 16) | 1 },
    };
    error = execute(c, &c->admin, &create_sq, NULL, 0, false, 0, NULL);
    return error > 0 ? -KERNEL_EIO : error;
}

static int initialize(struct nvme_controller *c)
{
    struct pci_function *function = &c->function;
    struct pci_host *host = function->host;
    if (pci_function_assign(function)) return -KERNEL_EIO;
    if (!function->bars[0].size || function->bars[0].size < NVME_DOORBELL + 16)
        return -KERNEL_ENODEV;
    c->registers = host->map(host->context, function->bars[0].address, function->bars[0].size);
    if (!c->registers) return -KERNEL_ENODEV;
    uint32_t low = reg_read(c, NVME_CAP), high = reg_read(c, NVME_CAP + 4);
    unsigned depth = (low & 0xffff) + 1;
    if (depth > NVME_DEPTH) depth = NVME_DEPTH;
    c->stride = 4U << (high & 15);
    c->reset_ticks = ((uint64_t)(low >> 24) + 1) * c->timebase / 2;
    /* The NVM command set and 4 KiB controller pages are independent of
     * the guest CPU's allocator page size. */
    if (depth < 2 || !(high & (1U << 5)) || (high & (15U << 16)) ||
        NVME_DOORBELL + 3ULL * c->stride + 4 > function->bars[0].size)
        return -KERNEL_EOPNOTSUPP;
    int error = stop_controller(c);
    if (error) return error;
    error = queue_init(c, &c->admin, 0, depth);
    if (!error) error = queue_init(c, &c->io, 1, depth);
    if (error) return error;
    uint32_t source = host->interrupt_source ?
        host->interrupt_source(host->context, function->bdf, function->interrupt_pin) : 0;
    if (source && host->register_irq && host->unregister_irq &&
        host->register_irq(host->context, source, interrupt, c)) c->irq = source;
    host->write(host->context, function->bdf, 4, 2, (function->command | 6) & ~0x401U);
    reg_write(c, NVME_AQA, ((depth - 1) << 16) | (depth - 1));
    reg_write64(c, NVME_ASQ, c->admin.submission.physical);
    reg_write64(c, NVME_ACQ, c->admin.completion.physical);
    reg_write(c, NVME_CC, (4U << 20) | (6U << 16) | 1);
    error = wait_ready(c, true);
    if (error) return error;
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &c->lock);
        c->state = NVME_READY;
    }
    if (c->irq) reg_write(c, NVME_INTMC, 1);
    error = create_io_queue(c);
    return error ? error : discover_namespaces(c);
}

static int destroy(struct nvme_controller *c)
{
    if (__atomic_load_n(&c->target.opens, __ATOMIC_ACQUIRE)) return -KERNEL_EBUSY;
    while (c->namespaces) {
        struct nvme_namespace *ns = c->namespaces;
        if (__atomic_load_n(&ns->target.opens, __ATOMIC_ACQUIRE)) return -KERNEL_EBUSY;
        int error = kernel_block_unregister(&ns->block);
        if (error) return error;
        c->namespaces = ns->next;
        kernel_heap_release(c->heap, ns);
    }
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &c->lock);
        if (c->admin.in_use || c->io.in_use || c->state == NVME_RESETTING) return -KERNEL_EBUSY;
        c->state = NVME_STOPPED;
    }
    if (c->registers) {
        int error = stop_controller(c);
        if (error) return error;
    }
    struct pci_host *host = c->function.host;
    if (c->irq) {
        host->unregister_irq(host->context, c->irq, c);
        c->irq = 0;
    }
    queue_destroy(c, &c->io);
    queue_destroy(c, &c->admin);
    if (c->function.assigned && pci_function_restore(&c->function)) return -KERNEL_EIO;
    return 0;
}

int kernel_nvme_bus_start(struct pci_host *host, struct kernel_heap *heap,
    struct physical_page_allocator *allocator, uint32_t timebase)
{
    if (!host || !heap || !allocator || !timebase || controllers) return -KERNEL_EINVAL;
    for (unsigned bdf = 0; bdf < 256; bdf++) {
        struct pci_function probe = {0};
        if (pci_function_probe(host, bdf, &probe)) continue;
        if (host->read(host->context, bdf, 8, 4) >> 8 != 0x010802U) continue;
        struct nvme_controller *c;
        if (kernel_heap_allocate_zeroed(heap, 1, sizeof(*c), (void **)&c)) return -KERNEL_ENOMEM;
        c->heap = heap;
        c->allocator = allocator;
        c->timebase = timebase;
        c->function = probe;
        c->index = next_controller++;
        initialize_target(c, NULL, &c->target);
        kernel_raw_lock_init(&c->lock, KERNEL_RAW_RANK_OBJECT);
        c->next = controllers;
        controllers = c;
        int error = initialize(c);
        if (error) return error;
    }
    return 0;
}

int kernel_nvme_bus_stop(void)
{
    while (controllers) {
        struct nvme_controller *c = controllers;
        int error = destroy(c);
        if (error) return error;
        controllers = c->next;
        kernel_heap_release(c->heap, c);
    }
    next_disk_minor = 0;
    next_command_minor = 0;
    next_controller = 0;
    return 0;
}

static struct nvme_target *find_target(uint64_t rdev)
{
    if (((rdev >> 8) & 0xfff) != NVME_COMMAND_MAJOR) return NULL;
    for (struct nvme_controller *c = controllers; c; c = c->next) {
        if (c->target.device.rdev == rdev) return &c->target;
        for (struct nvme_namespace *ns = c->namespaces; ns; ns = ns->next)
            if (ns->target.device.rdev == rdev) return &ns->target;
    }
    return NULL;
}

const struct kernel_char_device *kernel_nvme_char_device_lookup(uint64_t rdev)
{
    struct nvme_target *target = find_target(rdev);
    return target ? &target->device : NULL;
}

static int target_open(struct kernel_heap *heap, struct kernel_task *task,
    uint32_t flags, const struct kernel_vfs_file *file, void **instance)
{
    (void)heap;
    (void)task;
    (void)flags;
    struct kernel_vfs_stat stat;
    int error = kernel_vfs_fstat(file, &stat);
    if (error) return error;
    struct nvme_target *target = find_target(stat.rdev);
    if (!target) return -KERNEL_ENODEV;
    if (__atomic_fetch_add(&target->opens, 1, __ATOMIC_ACQ_REL) == UINT32_MAX) __builtin_trap();
    *instance = target;
    return 0;
}

static void target_release(void *arg)
{
    struct nvme_target *target = arg;
    if (!__atomic_fetch_sub(&target->opens, 1, __ATOMIC_ACQ_REL)) __builtin_trap();
}

static void initialize_target(struct nvme_controller *c, struct nvme_namespace *ns,
    struct nvme_target *target)
{
    unsigned index = next_command_minor++;
    target->controller = c;
    target->ns = ns;
    target->device = (struct kernel_char_device){
        .rdev = ((uint64_t)NVME_COMMAND_MAJOR << 8) |
            (index & 0xff) | ((uint64_t)(index & ~0xffU) << 12),
        .kind = KERNEL_OPEN_FILE_KIND_REGULAR,
        .open = target_open, .release = target_release,
    };
}

static char *append_decimal(char *out, uint32_t number)
{
    char digits[10];
    unsigned length = 0;
    do {
        digits[length++] = '0' + number % 10;
        number /= 10;
    } while (number);
    while (length) *out++ = digits[--length];
    return out;
}

static void target_name(const char *prefix, size_t length, struct nvme_controller *c,
    struct nvme_namespace *ns, char name[32])
{
    memcpy(name, prefix, length);
    char *out = append_decimal(name + length, c->index);
    if (ns) {
        *out++ = 'n';
        out = append_decimal(out, ns->nsid);
    }
    *out = 0;
}

int kernel_nvme_block_name(uint64_t number, char name[32])
{
    if (((number >> 8) & 0xfff) != NVME_BLOCK_MAJOR) return 0;
    for (struct nvme_controller *c = controllers; c; c = c->next)
        for (struct nvme_namespace *ns = c->namespaces; ns; ns = ns->next)
            if (ns->block.device_number == number) {
                target_name("nvme", 4, c, ns, name);
                return 1;
            }
    return 0;
}

int kernel_nvme_visit_command_nodes(int (*publish)(void *, const char *, uint64_t), void *arg)
{
    for (struct nvme_controller *c = controllers; c; c = c->next) {
        char name[32];
        target_name("nvme", 4, c, NULL, name);
        int error = publish(arg, name, c->target.device.rdev);
        if (error) return error;
        for (struct nvme_namespace *ns = c->namespaces; ns; ns = ns->next) {
            target_name("ng", 2, c, ns, name);
            error = publish(arg, name, ns->target.device.rdev);
            if (error) return error;
        }
    }
    return 0;
}

static int command_length(struct nvme_target *target, struct nvme_user_command *op,
    bool admin, bool *to_device, unsigned *length)
{
    if (admin) {
        uint64_t bytes = 0;
        switch (op->opcode) {
        case ADMIN_IDENTIFY:
            bytes = NVME_PAGE;
            break;
        case ADMIN_GET_LOG: {
            uint64_t dwords = ((op->dword[0] >> 16) | ((uint64_t)(op->dword[1] & 0xffff) << 16)) + 1;
            bytes = dwords * 4;
            break;
        }
        case ADMIN_GET_FEATURE:
            /* Features which include a DMA payload need a separate size
             * check before forwarding their messages to the controller. */
            switch (op->dword[0] & 0xff) {
            case 1: case 2: case 4: case 5: case 6: case 7: case 8: case 9:
            case 10: case 11:
                break;
            default:
                return -KERNEL_EOPNOTSUPP;
            }
            break;
        default: return -KERNEL_EOPNOTSUPP;
        }
        if (bytes > target->controller->max_transfer) return -KERNEL_EINVAL;
        *length = (unsigned)bytes;
        return 0;
    }
    struct nvme_namespace *ns = target->ns;
    if (op->nsid != ns->nsid) return -KERNEL_EINVAL;
    if (op->opcode == NVM_FLUSH) return 0;
    if (op->opcode != NVM_READ && op->opcode != NVM_WRITE && op->opcode != NVM_COMPARE)
        return -KERNEL_EOPNOTSUPP;
    uint64_t bytes = ((op->dword[2] & 0xffff) + 1ULL) * ns->block.logical_block_size;
    if (bytes > target->controller->max_transfer) return -KERNEL_EINVAL;
    *length = (unsigned)bytes;
    *to_device = op->opcode != NVM_READ;
    return 0;
}

static int transfer_user(struct kernel_mm *mm, const struct kernel_uaccess_iovec *parts,
    unsigned count, void *buffer, bool to_device)
{
    for (unsigned i = 0; i < count; i++) {
        size_t copied;
        enum kernel_uaccess_status status = to_device
            ? kernel_copy_from_user(mm, buffer, parts[i].base, parts[i].length, &copied)
            : kernel_copy_to_user(mm, parts[i].base, buffer, parts[i].length, &copied);
        if (status || copied != parts[i].length) return -KERNEL_EFAULT;
        buffer = (uint8_t *)buffer + parts[i].length;
    }
    return 0;
}

int kernel_nvme_uring_command(struct kernel_open_file_description *file,
    struct kernel_mm *mm, const struct kernel_credentials *cred, uint32_t cmd_op,
    const void *payload, uint64_t *result)
{
    if (!file || !file->device || file->device->open != target_open ||
        !file->device_instance) return -KERNEL_EOPNOTSUPP;
    struct nvme_target *target = file->device_instance;
    bool admin, vector;
    switch (cmd_op) {
    case 0xc0484e80: admin = false; vector = false; break;
    case 0xc0484e81: admin = false; vector = true; break;
    case 0xc0484e82: admin = true; vector = false; break;
    case 0xc0484e83: admin = true; vector = true; break;
    default: return -KERNEL_ENOTTY;
    }
    if (admin == !!target->ns) return -KERNEL_ENOTTY;
    struct nvme_user_command op;
    memcpy(&op, payload, sizeof(op));
    if (op.flags || op.reserved1 || op.reserved2 || op.metadata_len || op.metadata)
        return -KERNEL_EINVAL;
    bool privileged = kernel_credentials_capable(cred, KERNEL_CAP_SYS_ADMIN);
    if (admin && !privileged) return -KERNEL_EACCES;
    unsigned length = 0;
    bool to_device = false;
    int error = command_length(target, &op, admin, &to_device, &length);
    if (error) return error;
    if (to_device && !kernel_open_file_writable(file) && !privileged) return -KERNEL_EACCES;
    if (!admin && op.opcode == NVM_WRITE && kernel_block_read_only(&target->ns->block))
        return -KERNEL_EROFS;

    struct nvme_controller *c = target->controller;
    struct nvme_queue *q = admin ? &c->admin : &c->io;
    struct kernel_uaccess_iovec scalar = {op.address, op.length};
    struct kernel_uaccess_iov_snapshot vectors = {0};
    const struct kernel_uaccess_iovec *parts = &scalar;
    unsigned count = 1;
    if (vector) {
        if (op.length > 1024 || (!op.length && length)) return -KERNEL_EINVAL;
        int64_t imported;
        if (kernel_uaccess_iov_import(&vectors, c->heap, mm, op.address, op.length, &imported))
            __builtin_trap();
        if (imported || vectors.total != length) {
            error = imported ? (int)imported : -KERNEL_EINVAL;
            goto out;
        }
        parts = vectors.view;
        count = (unsigned)vectors.count;
    } else if (op.length != length) return -KERNEL_EINVAL;
    if (length && !mm) {
        error = -KERNEL_EFAULT;
        goto out;
    }

    uint64_t limit = command_deadline(c, op.timeout_ms);
    unsigned cid;
    error = reserve_slot(c, q, limit, &cid);
    if (error) goto out;
    struct nvme_command command = { .opcode = op.opcode, .nsid = op.nsid,
        .dword2 = op.dword2, .dword3 = op.dword3 };
    memcpy(command.dword, op.dword, sizeof(command.dword));
    struct nvme_dma *buffer = &q->slots[cid].buffer;
    if (length) {
        error = dma_resize(c, buffer, length);
        if (error) goto return_owner;
        if (to_device) {
            error = transfer_user(mm, parts, count, buffer->data, true);
            if (error) goto return_owner;
        } else memset(buffer->data, 0, length);
        populate_prps(q, &command, cid, buffer->physical, length);
    }
    error = submit(c, q, &command, cid, limit, result);
    if (!error && length && !to_device) error = transfer_user(mm, parts, count, buffer->data, false);
return_owner:
    return_slot(c, q, cid);
out:
    kernel_uaccess_iov_cleanup(&vectors, c->heap);
    return error;
}
