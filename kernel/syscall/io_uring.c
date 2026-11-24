#include <kernel/io_uring_zcrx.h>
#include <kernel/bitops.h>
#include <kernel/io_uring.h>
#include <kernel/aio.h>
#include <kernel/avl.h>
#include <kernel/credentials.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/heap.h>
#include <kernel/irq.h>
#include <kernel/mm.h>
#include <kernel/nvme.h>
#include <kernel/open_file.h>
#include <kernel/physical_page.h>
#include <kernel/pid.h>
#include <kernel/signal.h>
#include <kernel/signal_fd.h>
#include <kernel/sync.h>
#include <kernel/task.h>
#include <kernel/task_state.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/wait_internal.h>
#include "../../fs/char_device_internal.h"
#include "../../fs/files/private.h"
#include "../../fs/open_file_internal.h"
#include <string.h>

/* Linux UAPI, pinned at references/sources.tsv. Offsets for the shared
 * words are supplied in setup's result, so the layout can be cache aligned. */
enum {
    URING_SETUP = 425, URING_ENTER, URING_REGISTER,
    URING_CQSIZE = 1U << 3, URING_CLAMP = 1U << 4,
    URING_DISABLED = 1U << 6, URING_SUBMIT_ALL = 1U << 7,
    URING_SQE128 = 1U << 10, URING_CQE32 = 1U << 11,
    URING_SINGLE_ISSUER = 1U << 12, URING_DEFER_TASKRUN = 1U << 13, URING_NO_MMAP = 1U << 14, URING_NO_SQARRAY = 1U << 16,
    URING_GETEVENTS = 1U, URING_EXT_ARG = 1U << 3, URING_ABS_TIMER = 1U << 5,
    URING_FIXED_FILE = 1U, URING_DRAIN = 2U, URING_LINK = 4U,
    URING_HARDLINK = 8U, URING_ASYNC = 16U, URING_BUFFER_SELECT = 32U,
    URING_SKIP_SUCCESS = 64U,
    URING_CQE_BUFFER = 1U, URING_MAX_BIDS = 65536U,
    URING_CQE_MORE = 2U, URING_SQ_TASKRUN = 4U, URING_CQ_OVERFLOW = 2U, URING_CQ_EVENTFD_DISABLED = 1U,
    URING_TIMEOUT_ABS = 1U, URING_TIMEOUT_UPDATE = 2U, URING_TIMEOUT_BOOT = 4U,
    URING_TIMEOUT_REAL = 8U, URING_TIMEOUT_ETIME_SUCCESS = 32U,
    URING_TIMEOUT_IMMEDIATE_ARG = 128U,
    URING_MAX_SQ = 32768U, URING_MAX_CQ = 65536U,
    URING_ENFILE = 23, URING_EBADFD = 77, URING_ETIME = 62,
};
enum {
    URING_OP_NOP = 0, URING_OP_READV, URING_OP_WRITEV, URING_OP_FSYNC,
    URING_OP_READ_FIXED, URING_OP_WRITE_FIXED,
    URING_OP_POLL_ADD = 6, URING_OP_POLL_REMOVE = 7, URING_OP_TIMEOUT = 11,
    URING_OP_TIMEOUT_REMOVE = 12, URING_OP_ASYNC_CANCEL = 14, URING_OP_LINK_TIMEOUT,
    URING_OP_READ = 22, URING_OP_WRITE = 23,
    URING_OP_FADVISE = 24, URING_OP_MADVISE, URING_OP_PROVIDE_BUFFERS = 31,
    URING_OP_REMOVE_BUFFERS, URING_OP_URING_CMD = 46,
    URING_OP_OPENAT = 18, URING_OP_CLOSE = 19, URING_OP_OPENAT2 = 28,
    URING_OP_SPLICE = 30, URING_OP_TEE = 33,
    URING_OP_FIXED_FD_INSTALL = 54, URING_OP_RECV_ZC = 58,
    URING_OP_LIMIT = 65,
};
enum {
    URING_REGISTER_BUFFERS = 0, URING_UNREGISTER_BUFFERS,
    URING_REGISTER_FILES = 2, URING_UNREGISTER_FILES,
    URING_REGISTER_EVENTFD, URING_UNREGISTER_EVENTFD,
    URING_REGISTER_FILES_UPDATE, URING_REGISTER_EVENTFD_ASYNC,
    URING_REGISTER_PROBE, URING_REGISTER_PERSONALITY, URING_UNREGISTER_PERSONALITY,
    URING_REGISTER_RESTRICTIONS, URING_ENABLE, URING_REGISTER_FILES2,
    URING_REGISTER_FILES_UPDATE2,
    URING_REGISTER_FILE_RANGE = 25, URING_REGISTER_ZCRX = 32, URING_REGISTER_ZCRX_CTRL = 36, URING_REGISTER_LIMIT = 38,
};
enum {
    URING_FEAT_SINGLE_MMAP = 1U << 0, URING_FEAT_NODROP = 1U << 1,
    URING_FEAT_SUBMIT_STABLE = 1U << 2, URING_FEAT_EXT_ARG = 1U << 8,
    URING_FEAT_CQE_SKIP = 1U << 11,
};
#define URING_OFF_CQ_RING UINT64_C(0x8000000)
#define URING_OFF_SQES UINT64_C(0x10000000)

struct uring_sq_offsets {
    uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array, reserved;
    uint64_t user_address;
};
struct uring_cq_offsets {
    uint32_t head, tail, ring_mask, ring_entries, overflow, cqes, flags, reserved;
    uint64_t user_address;
};
struct uring_params {
    uint32_t sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle;
    uint32_t features, wq_fd, reserved[3];
    struct uring_sq_offsets sq;
    struct uring_cq_offsets cq;
};
struct uring_completion {
    uint64_t data;
    int32_t result;
    uint32_t flags;
    uint64_t extra[2];
};
struct uring_shared {
    _Alignas(64) uint32_t sq_head;
    uint32_t sq_tail, sq_mask, sq_entries, sq_flags, sq_dropped;
    _Alignas(64) uint32_t cq_head;
    uint32_t cq_tail, cq_mask, cq_entries, cq_flags, cq_overflow;
};
_Static_assert(sizeof(struct uring_params) == 120 && sizeof(struct uring_sqe) == 64 &&
    sizeof(struct uring_completion) == 32 && sizeof(struct uring_shared) == 128,
    "io_uring ABI and aligned shared layout");

struct uring_page { uint64_t physical; void *bytes; };
struct uring_overflow {
    struct uring_overflow *next;
    struct uring_completion event;
};
struct uring_file_table {
    unsigned count, start, end, next;
    struct kernel_open_file_description *files[];
};
struct uring_pending;
struct uring_personality {
    struct uring_personality *next;
    const struct kernel_credentials *credentials;
    uint16_t id;
};
struct uring_restrictions {
    uint64_t registers, operations[2];
    unsigned registers_restricted, operations_restricted, allowed, required;
};
struct uring_buffer_range {
    struct uring_buffer_range *next;
    uint64_t address;
    uint32_t length, first, count;
};
struct uring_buffer_group {
    struct uring_buffer_group *next;
    struct uring_buffer_range *head, **tail;
    uint32_t entries;
    uint16_t id;
};
struct uring_fixed_buffer {
    uint64_t address, length;
    size_t acquired;
    struct kernel_mm_long_pin pages[];
};
struct uring_fixed_buffers {
    struct kernel_heap *heap;
    struct kernel_uid_account *account;
    uint64_t charged;
    unsigned references, count;
    struct uring_fixed_buffer *slots[];
};
struct kernel_io_uring {
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct kernel_mutex submit_lock;
    struct kernel_raw_lock cq_lock;
    struct kernel_wait_queue ready, work;
    struct kernel_thread_join worker;
    struct uring_pending *pending, *work_head, **work_tail, *zcrx_parked;
    struct kernel_avl_node *pending_by_data, *timers[2];
    struct kernel_open_file_description *retired;
    uint64_t work_generation, zcrx_generation;
    unsigned worker_state, stopping;
    struct kernel_open_file_description *eventfd;
    struct uring_file_table *files;
    struct uring_buffer_group *groups;
    struct uring_fixed_buffers *fixed_buffers;
    struct kernel_uid_account *memory_account;
    struct kernel_mm_long_pin *external_pages;
    struct kernel_io_uring_zcrx *zcrx;
    unsigned charged_pages;
    struct kernel_task *issuer;
    struct uring_personality *personalities;
    struct uring_restrictions restrictions;
    uint16_t next_personality;
    struct uring_overflow *overflow, **overflow_tail;
    struct uring_shared *shared;
    uint32_t flags, sq_entries, cq_entries, sqe_size, cqe_size, sq_array;
    uint32_t sq_head, cq_head, cq_tail, cq_reserved, dropped;
    unsigned ring_pages, sqe_pages, allocated_pages;
    int eventfd_async;
    unsigned has_eventfd, has_overflow;
    struct uring_page pages[];
};

struct uring_submission;
struct kernel_io_uring_call {
    struct kernel_files *files;
    struct kernel_open_file_description *file, *preparing_file;
    struct uring_submission *batch;
    struct uring_fixed_buffers *preparing_buffers;
    struct kernel_io_uring *preparing_ring;
    struct kernel_io_uring_zcrx *preparing_zcrx;
    struct kernel_files_fd_reservation *preparing_fd;
    unsigned first, submitted;
    bool allocated;
    const struct kernel_credentials *saved_credentials;
    bool overridden;
};

struct uring_pending {
    struct kernel_io_uring *ring;
    struct kernel_open_file_description *file;
    struct kernel_pid *signal_recipient;
    struct uring_overflow *spare;
    struct uring_pending *next, **previous, *work_next, *peer;
    struct uring_pending *zcrx_next, **zcrx_previous;
    struct kernel_avl_node deadline_node, data_node;
    struct kernel_wait_node watch;
    uint64_t data, deadline;
    uint32_t events, zcrx_remaining;
    unsigned zcrx : 1, zcrx_bounded : 1;
    unsigned queued : 1, active : 1, canceled : 1, busy : 1, dirty : 1;
    unsigned realtime : 1, uses_realtime : 1;
};
static void stop_worker(struct kernel_io_uring *);
static void refill_zcrx(struct kernel_io_uring *);
static void destroy_waiters(struct kernel_wait_queue *);
static struct kernel_wait_queue wall_changed = KERNEL_WAIT_QUEUE_STATIC_INITIALIZER;
static uint32_t wall_generation;

static int copy_in(struct kernel_mm *mm, void *dst, uint64_t src, size_t size)
{
    size_t copied = 0;
    return kernel_copy_from_user(mm, dst, src, size, &copied) || copied != size ?
        -KERNEL_EFAULT : 0;
}
static int copy_out(struct kernel_mm *mm, uint64_t dst, const void *src, size_t size)
{
    size_t copied = 0;
    return kernel_copy_to_user(mm, dst, src, size, &copied) || copied != size ?
        -KERNEL_EFAULT : 0;
}
static int pin_file(struct kernel_files *files, unsigned fd,
    struct kernel_open_file_description **out)
{
    int64_t result = 0;
    if (kernel_files_pin(files, fd, out, &result)) return -KERNEL_EIO;
    return (int)result;
}
static int put_file(struct kernel_files *files, struct kernel_open_file_description **file)
{
    if (!*file) return 0;
    return kernel_files_release_io(files, file, KERNEL_FILES_STATUS_OK) ? -KERNEL_EIO : 0;
}
static struct kernel_io_uring *from_file(struct kernel_open_file_description *file)
{
    return file && file->kind == KERNEL_OPEN_FILE_KIND_IO_URING ? file->device_instance : NULL;
}
static void *page_address(const struct kernel_io_uring *ring, unsigned first, size_t offset)
{
    return (unsigned char *)ring->pages[first + (offset >> BOAROS_PAGE_SHIFT)].bytes +
        (offset & BOAROS_PAGE_MASK);
}
static uint32_t cq_ready(struct kernel_io_uring *ring)
{
    uint32_t head = __atomic_load_n(&ring->shared->cq_head, __ATOMIC_ACQUIRE);
    /* The mapped head is writable by userspace. Limit forward advances
     * to published entries and do not accept a backwards change. */
    if (head - ring->cq_head <= ring->cq_tail - ring->cq_head)
        ring->cq_head = head;
    return ring->cq_tail - ring->cq_head;
}
static void publish_locked(struct kernel_io_uring *ring, const struct uring_completion *cqe)
{
    size_t offset = sizeof(struct uring_shared) +
        (ring->cq_tail & (ring->cq_entries - 1U)) * ring->cqe_size;
    memcpy(page_address(ring, 0, offset), cqe, ring->cqe_size);
    ring->cq_tail++;
}
static void wake_queue(struct kernel_wait_queue *queue)
{
    if (kernel_wait_queue_registrations(queue)) {
        KERNEL_IRQ_SCOPE(irq);
        if (kernel_wait_queue_wake_all(queue)) __builtin_trap();
    }
}
void kernel_io_uring_clock_changed(void)
{
    __atomic_add_fetch(&wall_generation, 1, __ATOMIC_RELEASE);
    wake_queue(&wall_changed);
}
static void notify_batch(struct kernel_io_uring *ring, int events, bool async)
{
    if (!events) return;
    if (__atomic_load_n(&ring->has_eventfd, __ATOMIC_ACQUIRE)) {
        KERNEL_LOCK_SCOPE(submission);
        kernel_mutex_lock(&ring->submit_lock, &submission);
        if (ring->eventfd && (async || !ring->eventfd_async) &&
            !(__atomic_load_n(&ring->shared->cq_flags, __ATOMIC_RELAXED) & URING_CQ_EVENTFD_DISABLED))
            kernel_event_fd_signal(ring->eventfd);
    }
    wake_queue(&ring->ready);
}

static void flush_overflow(struct kernel_io_uring *ring)
{
    while (__atomic_load_n(&ring->has_overflow, __ATOMIC_ACQUIRE)) {
        struct uring_overflow *released = NULL;
        unsigned count = 0;
        {
            KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
            while (ring->overflow && count < 16 &&
                   cq_ready(ring) + ring->cq_reserved < ring->cq_entries) {
                struct uring_overflow *entry = ring->overflow;
                ring->overflow = entry->next;
                publish_locked(ring, &entry->event);
                entry->next = released;
                released = entry;
                count++;
            }
            if (!ring->overflow) {
                ring->overflow_tail = &ring->overflow;
                __atomic_store_n(&ring->has_overflow, 0, __ATOMIC_RELEASE);
                __atomic_fetch_and(&ring->shared->sq_flags, ~(URING_CQ_OVERFLOW | URING_SQ_TASKRUN), __ATOMIC_RELAXED);
            }
            if (count) __atomic_store_n(&ring->shared->cq_tail, ring->cq_tail, __ATOMIC_RELEASE);
        }
        while (released) {
            struct uring_overflow *next = released->next;
            kernel_heap_release(ring->heap, released);
            released = next;
        }
        if (count < 16) break;
    }
}

static void release_pages(struct kernel_io_uring *ring)
{
    if (ring->external_pages) {
        for (unsigned i = 0; i < ring->allocated_pages; i++)
            kernel_mm_long_pin_release(&ring->external_pages[i]);
        kernel_heap_release(ring->heap, ring->external_pages);
    } else {
        for (unsigned i = 0; i < ring->allocated_pages; i++)
            if (physical_page_release(ring->allocator, ring->pages[i].physical)) __builtin_trap();
    }
    if (ring->charged_pages)
        kernel_uid_account_pin_uncharge(ring->memory_account, ring->charged_pages);
    kernel_uid_account_put(ring->memory_account);
}
static void put_buffers(struct uring_fixed_buffers *table)
{
    if (!table || __atomic_fetch_sub(&table->references, 1, __ATOMIC_ACQ_REL) != 1) return;
    for (unsigned i = 0; i < table->count; i++) {
        struct uring_fixed_buffer *b = table->slots[i];
        if (!b) continue;
        for (size_t page = 0; page < b->acquired; page++)
            kernel_mm_long_pin_release(&b->pages[page]);
        kernel_heap_release(table->heap, b);
    }
    if (table->account) kernel_uid_account_pin_uncharge(table->account, table->charged);
    kernel_heap_release(table->heap, table);
}
int kernel_io_uring_release_file(struct kernel_open_file_description *file)
{
    struct kernel_io_uring *ring = from_file(file);
    if (!ring) return 0;
    stop_worker(ring);
    if (kernel_io_uring_zcrx_release(&ring->zcrx)) return -KERNEL_EAGAIN;
    while (ring->retired) {
        struct kernel_open_file_description *file = ring->retired;
        struct kernel_open_file_description *next = file->cleanup_next;
        if (kernel_open_file_release(&file)) return -KERNEL_EIO;
        ring->retired = next;
    }
    int retry = 0;
    if (ring->files) {
        for (unsigned i = 0; i < ring->files->count; i++) {
            struct kernel_open_file_description **slot = &ring->files->files[i];
            if (*slot && kernel_open_file_release(slot)) retry = 1;
        }
        if (retry) return -KERNEL_EIO;
        kernel_heap_release(ring->heap, ring->files);
        ring->files = NULL;
    }
    if (ring->eventfd && kernel_open_file_release(&ring->eventfd)) return -KERNEL_EIO;
    put_buffers(ring->fixed_buffers);
    if (ring->issuer) kernel_task_storage_put(ring->issuer);
    while (ring->groups) {
        struct uring_buffer_group *group = ring->groups;
        ring->groups = group->next;
        while (group->head) {
            struct uring_buffer_range *range = group->head;
            group->head = range->next;
            kernel_heap_release(ring->heap, range);
        }
        kernel_heap_release(ring->heap, group);
    }
    while (ring->personalities) {
        struct uring_personality *entry = ring->personalities;
        ring->personalities = entry->next;
        kernel_credentials_put(entry->credentials);
        kernel_heap_release(ring->heap,entry);
    }
    destroy_waiters(&ring->ready);
    destroy_waiters(&ring->work);
    while (ring->overflow) {
        struct uring_overflow *entry = ring->overflow;
        ring->overflow = entry->next;
        kernel_heap_release(ring->heap, entry);
    }
    release_pages(ring);
    kernel_heap_release(ring->heap, ring);
    file->device_instance = NULL;
    return 0;
}
static uint32_t poll_ring(void *instance, uint32_t flags, uint32_t requested,
    struct kernel_wait_queue **queue)
{
    struct kernel_io_uring *ring = instance;
    (void)flags;
    (void)requested;
    if (queue) *queue = &ring->ready;
    KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
    uint32_t mask = cq_ready(ring) || ring->overflow ? KERNEL_POLLIN|KERNEL_POLLRDNORM : 0;
    uint32_t sq_head = __atomic_load_n(&ring->shared->sq_head, __ATOMIC_RELAXED);
    uint32_t sq_tail = __atomic_load_n(&ring->shared->sq_tail, __ATOMIC_ACQUIRE);
    if (sq_tail - sq_head < ring->sq_entries) mask |= KERNEL_POLLOUT|KERNEL_POLLWRNORM;
    return mask;
}
static const struct kernel_char_device uring_operations = {
    .kind = KERNEL_OPEN_FILE_KIND_IO_URING, .stateless_io = 1, .poll = poll_ring,
};

int kernel_io_uring_mmap_begin(struct kernel_open_file_description *file, uint64_t offset,
    uint64_t length)
{
    struct kernel_io_uring *ring = from_file(file);
    if (!ring) return -KERNEL_ENODEV;
    if (offset == KERNEL_IO_URING_ZCRX_MMAP_OFFSET) return kernel_io_uring_zcrx_map(ring->zcrx, offset, length);
    if (ring->flags & URING_NO_MMAP) return -KERNEL_EINVAL;
    unsigned count;
    if (offset == URING_OFF_SQES) count = ring->sqe_pages;
    else if (!offset || offset == URING_OFF_CQ_RING) count = ring->ring_pages;
    else return -KERNEL_EINVAL;
    return !length || length > (uint64_t)count * BOAROS_PAGE_SIZE ? -KERNEL_EINVAL : 0;
}
int kernel_io_uring_mmap_page(struct kernel_open_file_description *file, uint64_t offset,
    int writable, uint64_t *physical)
{
    struct kernel_io_uring *ring = from_file(file);
    unsigned first = 0, count = ring->ring_pages;
    (void)writable;
    if (offset >= KERNEL_IO_URING_ZCRX_MMAP_OFFSET) return kernel_io_uring_zcrx_map_page(ring->zcrx, offset, physical);
    if (offset >= URING_OFF_SQES) {
        offset -= URING_OFF_SQES;
        first = ring->ring_pages;
        count = ring->sqe_pages;
    } else if (offset >= URING_OFF_CQ_RING) offset -= URING_OFF_CQ_RING;
    uint64_t page = offset >> BOAROS_PAGE_SHIFT;
    if (page >= count) return -KERNEL_EINVAL;
    *physical = ring->pages[first + page].physical;
    return physical_page_acquire(ring->allocator, *physical) ? -KERNEL_EIO : 0;
}
static uint32_t next_power(uint32_t value)
{
    if (value < 2) return 1U;
    value--;
    value |= value >> 1U;
    value |= value >> 2U;
    value |= value >> 4U;
    value |= value >> 8U;
    value |= value >> 16U;
    return value + 1U;
}
static __attribute__((noinline)) int setup_ring(struct kernel_io_uring_call *call, struct kernel_mm *mm, unsigned count,
    uint64_t user_params)
{
    struct kernel_files *files = call->files;
    struct uring_params params;
    int error = copy_in(mm, &params, user_params, sizeof(params));
    if (error) return error;
    unsigned flags = params.flags;
    const unsigned supported_flags = URING_CQSIZE|URING_CLAMP|URING_DISABLED|
        URING_SUBMIT_ALL|URING_SQE128|URING_CQE32|URING_SINGLE_ISSUER|URING_DEFER_TASKRUN|URING_NO_MMAP|URING_NO_SQARRAY;
    if (!count || params.reserved[0] || params.reserved[1] || params.reserved[2] ||
        (flags & ~supported_flags) ||
        ((flags & URING_DEFER_TASKRUN) && !(flags & URING_SINGLE_ISSUER))) return -KERNEL_EINVAL;
    if (count > URING_MAX_SQ) {
        if (!(flags & URING_CLAMP)) return -KERNEL_EINVAL;
        count = URING_MAX_SQ;
    }
    unsigned sq = next_power(count), cq = 2U * sq;
    if (flags & URING_CQSIZE) {
        cq = params.cq_entries;
        if (!cq) return -KERNEL_EINVAL;
        if (cq > URING_MAX_CQ) {
            if (!(flags & URING_CLAMP)) return -KERNEL_EINVAL;
            cq = URING_MAX_CQ;
        }
        cq = next_power(cq);
        if (cq < sq) return -KERNEL_EINVAL;
    }
    unsigned sqe_size = (flags & URING_SQE128) ? 128U : 64U;
    unsigned cqe_size = (flags & URING_CQE32) ? 32U : 16U;
    unsigned array = sizeof(struct uring_shared) + cq * cqe_size;
    size_t ring_size = array + ((flags & URING_NO_SQARRAY) ? 0 : sq * sizeof(uint32_t));
    unsigned ring_pages = (ring_size + BOAROS_PAGE_MASK) >> BOAROS_PAGE_SHIFT;
    unsigned sqe_pages = (sq * sqe_size + BOAROS_PAGE_MASK) >> BOAROS_PAGE_SHIFT;
    unsigned total_pages = ring_pages + sqe_pages;
    uint64_t user_ring = params.cq.user_address, user_sqes = params.sq.user_address;
    if (flags & URING_NO_MMAP) {
        if (!user_ring || !user_sqes) return -KERNEL_EFAULT;
        if ((user_ring | user_sqes) & BOAROS_PAGE_MASK) return -KERNEL_EINVAL;
        if (user_ring > UINT64_MAX - (uint64_t)ring_pages * BOAROS_PAGE_SIZE ||
            user_sqes > UINT64_MAX - (uint64_t)sqe_pages * BOAROS_PAGE_SIZE)
            return -KERNEL_EOVERFLOW;
    } else user_ring = user_sqes = 0;
    uint32_t fd;
    if (kernel_files_find_free_fd(files, &fd, &error)) return -KERNEL_EIO;
    if (error) return error;
    KERNEL_FILES_RESERVATION_SCOPE(reservation);
    reservation = (struct kernel_files_fd_reservation){files, fd};
    call->preparing_fd = &reservation;
    struct kernel_io_uring *ring = NULL;
    struct kernel_open_file_description *file = NULL;
    if (kernel_heap_allocate_zeroed(files->heap, 1,
            sizeof(*ring) + total_pages * sizeof(struct uring_page), (void **)&ring)) {
        call->preparing_fd = NULL;
        return -KERNEL_ENOMEM;
    }
    ring->heap = files->heap;
    ring->allocator = mm->allocator;
    call->preparing_ring = ring;
    ring->flags = flags;
    ring->sq_entries = sq;
    ring->cq_entries = cq;
    ring->sqe_size = sqe_size;
    ring->cqe_size = cqe_size;
    ring->ring_pages = ring_pages;
    ring->sqe_pages = sqe_pages;
    ring->sq_array = array;
    ring->overflow_tail = &ring->overflow;
    kernel_mutex_init(&ring->submit_lock, 4, (uintptr_t)ring);
    kernel_raw_lock_init(&ring->cq_lock, KERNEL_RAW_RANK_OBJECT);
    const struct kernel_credentials *credentials = kernel_credentials_current();
    if (!kernel_credentials_capable(credentials,KERNEL_CAP_IPC_LOCK)) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            ring->memory_account = kernel_uid_account_get_locked(credentials->account);
        }
        error = kernel_uid_account_pin_charge(ring->memory_account, total_pages,
            kernel_task_current_memlock_limit() >> BOAROS_PAGE_SHIFT);
        if (error) goto fail;
        ring->charged_pages = total_pages;
    }
    if (user_ring && kernel_heap_allocate_zeroed(ring->heap, total_pages,
            sizeof(ring->external_pages[0]), (void **)&ring->external_pages)) {
        error = -KERNEL_ENOMEM;
        goto fail;
    }
    for (unsigned i = 0; i < total_pages; i++) {
        struct uring_page *page = &ring->pages[i];
        if (user_ring) {
            uint64_t address = i < ring_pages ? user_ring + (uint64_t)i * BOAROS_PAGE_SIZE :
                user_sqes + (uint64_t)(i - ring_pages) * BOAROS_PAGE_SIZE;
            struct kernel_mm_long_pin *pin = &ring->external_pages[i];
            error = kernel_mm_long_pin_acquire(mm, address, pin);
            if (error) goto fail;
            page->physical = pin->physical;
            page->bytes = pin->page;
        } else if (physical_page_allocate(ring->allocator, &page->physical)) {
            error = -KERNEL_ENOMEM;
            goto fail;
        } else {
            if (physical_page_map_owned(ring->allocator, page->physical, &page->bytes)) __builtin_trap();
            memset(page->bytes, 0, BOAROS_PAGE_SIZE);
        }
        ring->allocated_pages++;
    }
    if (kernel_heap_allocate_zeroed(files->heap, 1, sizeof(*file), (void **)&file)) {
        error = -KERNEL_ENOMEM;
        goto fail;
    }
    call->preparing_file = file;
    ring->shared = ring->pages[0].bytes;
    if (user_ring) memset(ring->shared, 0, sizeof(*ring->shared));
    ring->shared->sq_mask = sq - 1U;
    ring->shared->cq_mask = cq - 1U;
    ring->shared->sq_entries = sq;
    ring->shared->cq_entries = cq;
    params.sq_entries = sq;
    params.cq_entries = cq;
    params.features = URING_FEAT_SINGLE_MMAP|URING_FEAT_NODROP|
        URING_FEAT_SUBMIT_STABLE|URING_FEAT_EXT_ARG|URING_FEAT_CQE_SKIP;
#define SHARED(field) offsetof(struct uring_shared, field)
    params.sq = (struct uring_sq_offsets){
        .head=SHARED(sq_head), .tail=SHARED(sq_tail), .ring_mask=SHARED(sq_mask),
        .ring_entries=SHARED(sq_entries), .flags=SHARED(sq_flags), .dropped=SHARED(sq_dropped),
        .array=(flags & URING_NO_SQARRAY) ? 0 : array,
        .user_address=user_sqes,
    };
    params.cq = (struct uring_cq_offsets){
        .head=SHARED(cq_head), .tail=SHARED(cq_tail), .ring_mask=SHARED(cq_mask),
        .ring_entries=SHARED(cq_entries), .overflow=SHARED(cq_overflow),
        .cqes=sizeof(struct uring_shared), .flags=SHARED(cq_flags),
        .user_address=user_ring,
    };
#undef SHARED
    error = copy_out(mm, user_params, &params, sizeof(params));
    if (error) goto fail;
    kernel_wait_queue_init(&ring->ready);
    kernel_wait_queue_init(&ring->work);
    ring->work_tail = &ring->work_head;
    if ((flags & (URING_SINGLE_ISSUER|URING_DISABLED)) == URING_SINGLE_ISSUER) {
        ring->issuer = kernel_task_current();
        kernel_task_storage_get(ring->issuer);
    }
    file->heap = files->heap;
    file->references = 1;
    file->kind = KERNEL_OPEN_FILE_KIND_IO_URING;
    file->open_flags = 2U;
    file->device = &uring_operations;
    file->device_instance = ring;
    file->credentials = kernel_credentials_get(kernel_credentials_current());
    kernel_mutex_init(&file->offset_lock, 10, (uintptr_t)file);
    call->preparing_ring = NULL;
    call->preparing_file = NULL;
    if (kernel_files_install_new_owned_at(files, fd, KERNEL_FILES_FD_CLOEXEC, &file)) {
        if (kernel_files_release_uninstalled_description(files, &file)) __builtin_trap();
        call->preparing_fd = NULL;
        return -KERNEL_EIO;
    }
    reservation.files = NULL;
    call->preparing_fd = NULL;
    return fd;

fail:
    call->preparing_fd = NULL;
    call->preparing_ring = NULL;
    call->preparing_file = NULL;
    if (file) kernel_heap_release(files->heap, file);
    release_pages(ring);
    kernel_heap_release(ring->heap, ring);
    return error;
}

static bool opcode_supported(unsigned opcode)
{
    switch (opcode) {
    case URING_OP_NOP:
    case URING_OP_READV:
    case URING_OP_WRITEV:
    case URING_OP_FSYNC:
    case URING_OP_READ_FIXED:
    case URING_OP_WRITE_FIXED:
    case URING_OP_READ:
    case URING_OP_WRITE:
    case URING_OP_POLL_ADD:
    case URING_OP_POLL_REMOVE:
    case URING_OP_TIMEOUT:
    case URING_OP_LINK_TIMEOUT:
    case URING_OP_TIMEOUT_REMOVE:
    case URING_OP_ASYNC_CANCEL:
    case URING_OP_FIXED_FD_INSTALL:
    case URING_OP_RECV_ZC:
    case URING_OP_SPLICE:
    case URING_OP_TEE:
    case URING_OP_FADVISE:
    case URING_OP_MADVISE:
    case URING_OP_PROVIDE_BUFFERS:
    case URING_OP_REMOVE_BUFFERS:
    case URING_OP_URING_CMD:
        return true;
    default:
        return kernel_io_uring_extra_supported(opcode);
    }
}
static bool needs_file(unsigned opcode)
{
    if (kernel_io_uring_extra_supported(opcode)) return kernel_io_uring_extra_needs_file(opcode);
    return opcode != URING_OP_NOP && opcode != URING_OP_MADVISE &&
        opcode != URING_OP_TIMEOUT && opcode != URING_OP_LINK_TIMEOUT &&
        opcode != URING_OP_TIMEOUT_REMOVE &&
        opcode != URING_OP_POLL_REMOVE && opcode != URING_OP_ASYNC_CANCEL &&
        opcode != URING_OP_PROVIDE_BUFFERS && opcode != URING_OP_REMOVE_BUFFERS;
}

static struct uring_buffer_group *buffer_group(struct kernel_io_uring *ring, uint16_t id)
{
    struct uring_buffer_group *group = ring->groups;
    while (group && group->id != id) group = group->next;
    return group;
}

static int provide_buffers(struct kernel_io_uring *ring, const struct uring_sqe *sqe)
{
    unsigned count = (unsigned)sqe->fd;
    uint64_t bytes = (uint64_t)sqe->length * count;
    if (!count || count > URING_MAX_BIDS || sqe->offset > UINT16_MAX) return -KERNEL_E2BIG;
    if (!sqe->length || sqe->offset + count > URING_MAX_BIDS) return -KERNEL_EINVAL;
    if (sqe->address > UINT64_MAX - bytes) return -KERNEL_EOVERFLOW;
    if (kernel_user_range_check(sqe->address, bytes)) return -KERNEL_EFAULT;
    struct uring_buffer_range *range = NULL;
    if (kernel_heap_allocate(ring->heap, sizeof(*range), (void **)&range)) return -KERNEL_ENOMEM;
    *range = (struct uring_buffer_range){
        .address=sqe->address, .length=sqe->length, .first=(uint32_t)sqe->offset, .count=count,
    };
    int result = 0;
    {
        KERNEL_LOCK_SCOPE(guard);
        kernel_mutex_lock(&ring->submit_lock, &guard);
        struct uring_buffer_group *group = buffer_group(ring, sqe->buffer);
        if (!group) {
            if (kernel_heap_allocate_zeroed(ring->heap, 1, sizeof(*group), (void **)&group))
                result = -KERNEL_ENOMEM;
            else {
                group->id = sqe->buffer;
                group->next = ring->groups;
                group->tail = &group->head;
                ring->groups = group;
            }
        }
        if (!result) {
            if (count > URING_MAX_BIDS - group->entries) result = -KERNEL_EOVERFLOW;
            else {
                *group->tail = range;
                group->tail = &range->next;
                group->entries += count;
                range = NULL;
            }
        }
    }
    if (range) kernel_heap_release(ring->heap, range);
    return result;
}

static void trim_buffer_range(struct uring_buffer_group *group, unsigned n)
{
    struct uring_buffer_range *range = group->head;
    range->address += (uint64_t)range->length * n;
    range->first += n;
    range->count -= n;
    group->entries -= n;
    if (!range->count) {
        group->head = range->next;
        if (!group->head) group->tail = &group->head;
    }
}

static int remove_buffers(struct kernel_io_uring *ring, const struct uring_sqe *sqe)
{
    unsigned count = (unsigned)sqe->fd, removed = 0;
    if (!count || count > URING_MAX_BIDS || sqe->address || sqe->length || sqe->offset)
        return -KERNEL_EINVAL;
    struct uring_buffer_range *retired = NULL;
    bool found;
    {
        KERNEL_LOCK_SCOPE(guard);
        kernel_mutex_lock(&ring->submit_lock, &guard);
        struct uring_buffer_group *group = buffer_group(ring, sqe->buffer);
        found = group != NULL;
        while (group && group->head && removed < count) {
            struct uring_buffer_range *range = group->head;
            unsigned n = range->count < count - removed ? range->count : count - removed;
            trim_buffer_range(group, n);
            if (!range->count) {
                range->next = retired;
                retired = range;
            }
            removed += n;
        }
    }
    while (retired) {
        struct uring_buffer_range *next = retired->next;
        kernel_heap_release(ring->heap, retired);
        retired = next;
    }
    return found ? (int)removed : -KERNEL_ENOENT;
}

static int select_buffer(struct kernel_io_uring *ring, uint16_t id, uint64_t *address,
    uint32_t *length, uint32_t *cqe_flags)
{
    struct uring_buffer_range *retired = NULL;
    {
        KERNEL_LOCK_SCOPE(guard);
        kernel_mutex_lock(&ring->submit_lock, &guard);
        struct uring_buffer_group *group = buffer_group(ring, id);
        if (!group || !group->head) return -KERNEL_ENOBUFS;
        struct uring_buffer_range *range = group->head;
        *address = range->address;
        if (!*length || *length > range->length) *length = range->length;
        *cqe_flags = URING_CQE_BUFFER | range->first << 16;
        trim_buffer_range(group, 1);
        if (!range->count) retired = range;
    }
    if (retired) kernel_heap_release(ring->heap, retired);
    return 0;
}
static int pin_operand_fd(struct kernel_io_uring *ring, struct kernel_files *files,
    unsigned slot, int fixed_file, struct kernel_open_file_description **file)
{
    if (!fixed_file) return pin_file(files, slot, file);
    struct kernel_open_file_description *fixed;
    if (!ring->files || slot >= ring->files->count || !(fixed = ring->files->files[slot]))
        return -KERNEL_EBADF;
    if (kernel_open_file_acquire(fixed)) return -KERNEL_EIO;
    *file = fixed;
    return 0;
}

static int pin_operands(struct kernel_io_uring *ring, struct kernel_files *files,
    const struct uring_sqe *sqe, struct kernel_open_file_description **file,
    struct kernel_open_file_description **file_in)
{
    int result = pin_operand_fd(ring, files, (unsigned)sqe->fd, sqe->flags & URING_FIXED_FILE, file);
    if (!result && (sqe->opcode == URING_OP_SPLICE || sqe->opcode == URING_OP_TEE))
        result = pin_operand_fd(ring, files, sqe->file_index, sqe->op_flags >> 31, file_in);
    return result;
}

static uint64_t *file_slots_used(struct uring_file_table *table)
{
    return (uint64_t *)(table->files + table->count);
}

static void set_file_slot(struct uring_file_table *table, unsigned slot,
    struct kernel_open_file_description *file)
{
    uint64_t mask = UINT64_C(1) << (slot % 64);
    uint64_t *bitmap = file_slots_used(table);
    if (file) bitmap[slot / 64] |= mask;
    else bitmap[slot / 64] &= ~mask;
    table->files[slot] = file;
}

static unsigned find_file_slot(struct uring_file_table *table, unsigned start, unsigned end)
{
    uint64_t *bitmap = file_slots_used(table);
    while (start < end) {
        uint64_t free = ~bitmap[start / 64] & (UINT64_MAX << (start % 64));
        if (free) {
            unsigned slot = (start & ~63U) + kernel_first_set_u64(free);
            return slot < end ? slot : end;
        }
        start = (start & ~63U) + 64;
    }
    return end;
}

static int direct_slot(struct kernel_io_uring *ring, struct kernel_files *files,
    unsigned index, struct kernel_open_file_description **owner)
{
    struct kernel_open_file_description *old = NULL;
    int result = 0;
    {
        KERNEL_LOCK_SCOPE(submission);
        kernel_mutex_lock(&ring->submit_lock, &submission);
        struct uring_file_table *table = ring->files;
        if (!table) return -KERNEL_ENXIO;
        unsigned slot = index - 1U;
        if (owner && index == UINT32_MAX) {
            slot = find_file_slot(table, table->next, table->end);
            if (slot == table->end) {
                slot = find_file_slot(table, table->start, table->next);
                if (slot == table->next) return -URING_ENFILE;
            }
            table->next = slot + 1 == table->end ? table->start : slot + 1;
            result = (int)slot;
        } else if (slot >= table->count) {
            return -KERNEL_EINVAL;
        }
        old = table->files[slot];
        if (!owner && !old) return -KERNEL_EBADF;
        set_file_slot(table, slot, owner ? *owner : NULL);
        if (owner) *owner = NULL;
    }
    (void)put_file(files, &old);
    return result;
}

static int install_fixed_fd(struct kernel_files *files,
    const struct uring_sqe *sqe, struct kernel_open_file_description **owner)
{
    struct kernel_files_fd_reservation reservation
        __attribute__((cleanup(kernel_files_cancel_reservation))) = {0};
    uint32_t fd;
    int result;
    if (kernel_files_find_free_fd(files, &fd, &result)) return -KERNEL_EIO;
    if (result) return result;
    reservation = (struct kernel_files_fd_reservation){files, fd};
    unsigned flags = sqe->op_flags & 1U ? 0 : KERNEL_FILES_FD_CLOEXEC;
    if (kernel_files_install_shared_acquired_at(files, fd, flags, owner)) return -KERNEL_EIO;
    reservation.files = NULL;
    return (int)fd;
}

static int64_t dispatch_sqe(struct kernel_io_uring *ring, struct kernel_files *files, struct kernel_mm *mm,
    const struct uring_sqe *sqe, struct kernel_open_file_description **file,
    struct kernel_open_file_description **file_in,
    const unsigned char *extension, struct uring_fixed_buffers *buffers, struct uring_completion *cqe)
{
    switch (sqe->opcode) {
    case URING_OP_NOP:
        return sqe->op_flags & 1U ? (int32_t)sqe->length : 0;
    case URING_OP_LINK_TIMEOUT:
        return -KERNEL_EINVAL;
    case URING_OP_FIXED_FD_INSTALL:
        return install_fixed_fd(files, sqe, file);
    case URING_OP_SPLICE: case URING_OP_TEE:
        return kernel_files_uring_splice(files, sqe->opcode == URING_OP_TEE, file_in,
            (int64_t)sqe->address, file, (int64_t)sqe->offset, sqe->length,
            sqe->op_flags & ~(UINT32_C(1) << 31));
    case URING_OP_CLOSE:
        if (sqe->file_index) return direct_slot(ring, files, sqe->file_index, NULL);
        break;
    case URING_OP_OPENAT: case URING_OP_OPENAT2:
        if (sqe->file_index) {
            int64_t result = kernel_io_uring_extra_execute(files, mm, sqe, file);
            if (result < 0) return result;
            if (from_file(*file)) return -KERNEL_EBADF;
            return direct_slot(ring, files, sqe->file_index, file);
        }
        break;
    case URING_OP_FSYNC: {
        uint64_t end = sqe->offset + sqe->length;
        if (!end || end > INT64_MAX) end = INT64_MAX;
        return kernel_files_fsync_range_owned(*file, sqe->offset, end + 1,
            sqe->op_flags & 1U);
    }
    case URING_OP_READ_FIXED:
    case URING_OP_WRITE_FIXED: {
        const struct uring_fixed_buffer *buffer = buffers->slots[sqe->buffer];
        size_t first = sqe->address - (buffer->address & ~BOAROS_PAGE_MASK);
        return kernel_files_uring_fixed_rw(files, *file, sqe->opcode == URING_OP_WRITE_FIXED,
            buffer->pages + (first >> BOAROS_PAGE_SHIFT), first & BOAROS_PAGE_MASK,
            sqe->length, (int64_t)sqe->offset, sqe->op_flags);
    }
    case URING_OP_FADVISE:
        return kernel_open_file_advise(*file, (int64_t)sqe->offset,
            sqe->extra[0] ? (int64_t)sqe->extra[0] : sqe->length, sqe->op_flags, 0);
    case URING_OP_MADVISE:
        return kernel_mm_madvise(mm, sqe->address,
            sqe->extra[0] ? sqe->extra[0] : sqe->length, sqe->op_flags);
    case URING_OP_PROVIDE_BUFFERS:
        return provide_buffers(ring, sqe);
    case URING_OP_REMOVE_BUFFERS:
        return remove_buffers(ring, sqe);
    case URING_OP_URING_CMD: {
        unsigned char payload[80];
        memcpy(payload, sqe->extra, sizeof(sqe->extra));
        memcpy(payload + sizeof(sqe->extra), extension, 64);
        return kernel_nvme_uring_command(*file, mm, kernel_credentials_current(),
            (uint32_t)sqe->offset, payload, &cqe->extra[0]);
    }
    default:
        break;
    }
    if (kernel_io_uring_extra_supported(sqe->opcode))
        return kernel_io_uring_extra_execute(files,mm,sqe,file);
    unsigned op = sqe->opcode == URING_OP_READV ? 7 : sqe->opcode == URING_OP_WRITEV ? 8 :
        sqe->opcode == URING_OP_READ ? 0 : 1;
    uint64_t address = sqe->address;
    uint32_t length = sqe->length;
    if (sqe->flags & URING_BUFFER_SELECT) {
        int result = select_buffer(ring, sqe->buffer, &address, &length, &cqe->flags);
        if (result) return result;
    }
    return kernel_files_uring_rw(files, mm, file, op, address, length,
        (int64_t)sqe->offset, sqe->op_flags);
}
static int validate_sqe(const struct kernel_io_uring *ring, const struct uring_sqe *sqe)
{
    const unsigned known = URING_FIXED_FILE|URING_LINK|URING_HARDLINK|URING_BUFFER_SELECT|
        URING_SKIP_SUCCESS;
    if (!opcode_supported(sqe->opcode) || (sqe->flags & ~known) ||
        (sqe->flags & (URING_LINK|URING_HARDLINK)) == (URING_LINK|URING_HARDLINK))
        return -KERNEL_EINVAL;
    if ((sqe->flags & URING_BUFFER_SELECT) && sqe->opcode != URING_OP_READ)
        return -KERNEL_EOPNOTSUPP;
    const struct uring_restrictions *res = &ring->restrictions;
    if (res->operations_restricted &&
        (!(res->operations[sqe->opcode / 64] & (UINT64_C(1) << (sqe->opcode % 64))) ||
         (sqe->flags & res->required) != res->required ||
         (sqe->flags & ~(res->allowed|res->required)))) return -KERNEL_EACCES;
    if (kernel_io_uring_extra_supported(sqe->opcode))
        return kernel_io_uring_extra_validate(sqe);
    if (sqe->opcode == URING_OP_SPLICE || sqe->opcode == URING_OP_TEE) {
        if (sqe->op_flags & ~UINT32_C(0x8000000f)) return -KERNEL_EINVAL;
        return sqe->opcode == URING_OP_TEE && (sqe->address || sqe->offset) ? -KERNEL_EINVAL : 0;
    }
    if (sqe->opcode == URING_OP_FSYNC &&
        (sqe->address || (int64_t)sqe->offset < 0 || (sqe->op_flags & ~1U)))
        return -KERNEL_EINVAL;
    if (sqe->opcode == URING_OP_RECV_ZC) {
        if (!ring->zcrx || !(sqe->ioprio & 2U) || (sqe->ioprio & ~3U) || sqe->offset ||
            sqe->address || sqe->extra[0] || sqe->op_flags || sqe->file_index || sqe->buffer)
            return -KERNEL_EINVAL;
        return 0;
    }
    if (sqe->opcode == URING_OP_FIXED_FD_INSTALL) {
        if (!(sqe->flags & URING_FIXED_FILE)) return -KERNEL_EBADF;
        if (sqe->personality) return -KERNEL_EPERM;
        if (sqe->offset || sqe->address || sqe->length || sqe->buffer ||
            sqe->file_index || sqe->extra[0] || (sqe->op_flags & ~1U)) return -KERNEL_EINVAL;
        return 0;
    }
    if (sqe->opcode == URING_OP_PROVIDE_BUFFERS || sqe->opcode == URING_OP_REMOVE_BUFFERS)
        return sqe->op_flags || sqe->file_index || sqe->ioprio ||
            (sqe->flags & URING_FIXED_FILE) ? -KERNEL_EINVAL : 0;
    if (!needs_file(sqe->opcode)) {
        if ((sqe->opcode == URING_OP_NOP && (sqe->op_flags & ~1U)) ||
            sqe->ioprio || sqe->buffer || sqe->file_index ||
            (sqe->flags & URING_FIXED_FILE)) return -KERNEL_EINVAL;
    } else if ((sqe->buffer && !(sqe->flags & URING_BUFFER_SELECT) &&
            sqe->opcode != URING_OP_READ_FIXED && sqe->opcode != URING_OP_WRITE_FIXED) ||
        sqe->ioprio || sqe->file_index) return -KERNEL_EINVAL;
    if (sqe->opcode == URING_OP_URING_CMD &&
        ((ring->flags & (URING_SQE128|URING_CQE32)) != (URING_SQE128|URING_CQE32) ||
        sqe->op_flags || sqe->offset >> 32)) return -KERNEL_EINVAL;
    return 0;
}

/* Reserve either a slot in the mapped CQ or an overflow record before
 * consuming a submission. The backing file can block with no ring lock held. */
static int completion_reserve(struct kernel_io_uring *ring, struct uring_overflow **spare, bool deferred)
{
    if (!deferred) {
        KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
        if (!ring->overflow && cq_ready(ring) + ring->cq_reserved < ring->cq_entries) {
            ring->cq_reserved++;
            return 0;
        }
    }
    return kernel_heap_allocate(ring->heap, sizeof(**spare), (void **)spare) ?
        -KERNEL_EAGAIN : 0;
}
static void complete_sqe(struct kernel_io_uring *ring, const struct uring_completion *cqe,
    struct uring_overflow *spare, bool from_worker)
{
    bool deferred = from_worker && (ring->flags & URING_DEFER_TASKRUN);
    {
        KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
        if (!spare) ring->cq_reserved--;
        if (cqe && (!spare || (!deferred && !ring->overflow &&
            cq_ready(ring) + ring->cq_reserved < ring->cq_entries))) {
            publish_locked(ring, cqe);
            __atomic_store_n(&ring->shared->cq_tail, ring->cq_tail, __ATOMIC_RELEASE);
        } else if (cqe) {
            spare->event = *cqe;
            spare->next = NULL;
            *ring->overflow_tail = spare;
            ring->overflow_tail = &spare->next;
            __atomic_store_n(&ring->has_overflow, 1, __ATOMIC_RELEASE);
            spare = NULL;
            __atomic_fetch_or(&ring->shared->sq_flags, deferred ? URING_SQ_TASKRUN : URING_CQ_OVERFLOW,
                __ATOMIC_RELAXED);
        }
    }
    if (spare) kernel_heap_release(ring->heap, spare);
}
static void destroy_waiters(struct kernel_wait_queue *queue)
{
    if (kernel_wait_queue_close(queue)) __builtin_trap();
    for (;;) {
        enum kernel_scheduler_status status = kernel_wait_queue_destroy(queue);
        if (!status) return;
        if (status != KERNEL_SCHEDULER_STATUS_BUSY) __builtin_trap();
        kernel_wait_backend_quiesce();
    }
}

static void unpark_zcrx_locked(struct uring_pending *request)
{
    if (!request->zcrx_previous) return;
    *request->zcrx_previous = request->zcrx_next;
    if (request->zcrx_next)
        request->zcrx_next->zcrx_previous = request->zcrx_previous;
    request->zcrx_previous = NULL;
    request->zcrx_next = NULL;
}

static void queue_work_locked(struct uring_pending *request)
{
    struct kernel_io_uring *ring = request->ring;
    request->dirty = 1;
    if (!request->active || request->queued || request->busy) return;
    unpark_zcrx_locked(request);
    request->queued = 1;
    *ring->work_tail = request;
    ring->work_tail = &request->work_next;
    ring->work_generation++;
}

static void poll_wake(struct kernel_wait_node *watch, uint32_t reason, uint64_t event)
{
    struct uring_pending *request = watch->context;
    struct kernel_io_uring *ring = request->ring;
    (void)event;
    if (reason == KERNEL_WAIT_POLL_FREE) kernel_wait_queue_remove(watch);
    {
        KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
        if (reason == KERNEL_WAIT_POLL_FREE) request->canceled = 1;
        queue_work_locked(request);
    }
    wake_queue(&ring->work);
}

static void attach_pending_locked(struct kernel_io_uring *ring, struct uring_pending *request)
{
    request->next = ring->pending;
    if (request->next) request->next->previous = &request->next;
    ring->pending = request;
    request->previous = &ring->pending;
    request->active = 1;
    request->data_node = (struct kernel_avl_node){.key=request->data, .height=1};
    ring->pending_by_data = kernel_avl_insert_node(ring->pending_by_data, &request->data_node);
    if (!request->file) {
        request->deadline_node = (struct kernel_avl_node){.key=request->deadline, .height=1};
        struct kernel_avl_node **timers = &ring->timers[request->realtime];
        *timers = kernel_avl_insert_node(*timers, &request->deadline_node);
        ring->work_generation++;
    } else queue_work_locked(request);
}

static void detach_pending_locked(struct uring_pending *request)
{
    unpark_zcrx_locked(request);
    struct kernel_io_uring *ring = request->ring;
    *request->previous = request->next;
    if (request->next) request->next->previous = request->previous;
    if (!request->file) {
        struct kernel_avl_node **timers = &ring->timers[request->realtime];
        *timers = kernel_avl_remove_node(*timers, &request->deadline_node);
    }
    ring->pending_by_data = kernel_avl_remove_node(ring->pending_by_data, &request->data_node);
    request->active = 0;
}

static struct uring_pending *timer_from_node(struct kernel_avl_node *node)
{
    return (void *)((char *)node - offsetof(struct uring_pending, deadline_node));
}

static void retire_operand(struct kernel_io_uring *ring, struct kernel_open_file_description **file)
{
    if (!*file || !kernel_open_file_release(file)) return;
    KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
    (*file)->cleanup_next = ring->retired;
    __atomic_store_n(&ring->retired, *file, __ATOMIC_RELEASE);
    *file = NULL;
}

static void drain_operands(struct kernel_io_uring *ring, struct kernel_files *files)
{
    if (!__atomic_load_n(&ring->retired, __ATOMIC_ACQUIRE)) return;
    struct kernel_open_file_description *pending;
    {
        KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
        pending = ring->retired;
        __atomic_store_n(&ring->retired, NULL, __ATOMIC_RELAXED);
    }
    while (pending) {
        struct kernel_open_file_description *file = pending;
        pending = file->cleanup_next;
        file->cleanup_next = NULL;
        (void)put_file(files, &file);
    }
}

static struct uring_pending *next_work_locked(struct kernel_io_uring *ring, uint64_t now,
    uint64_t wall, struct kernel_avl_node *current, struct kernel_avl_node *absolute)
{
    struct uring_pending *request = ring->work_head;
    if (request) {
        ring->work_head = request->work_next;
        if (!ring->work_head) ring->work_tail = &ring->work_head;
        request->work_next = NULL;
        request->queued = 0;
    } else if (ring->stopping) request = ring->pending;
    else if (current && current->key <= now) request = timer_from_node(current);
    else if (absolute && absolute->key <= wall) request = timer_from_node(absolute);
    if (request) {
        request->busy = 1;
        request->dirty = 0;
    }
    return request;
}

static void run_zcrx(struct uring_pending *request)
{
    struct kernel_io_uring *ring = request->ring;
    uint64_t generation;
    int result, canceled;
    {
        KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
        canceled = request->canceled || ring->stopping;
        generation = ring->zcrx_generation;
    }
    struct uring_overflow *spare = NULL, *completed = request->spare;
    struct uring_completion cqe = {.data = request->data};
    if (canceled) result = -KERNEL_ECANCELED;
    else if (!(kernel_open_file_poll(request->file, request->events, NULL) & request->events))
        result = -KERNEL_EAGAIN;
    else if (completion_reserve(ring, &spare, true)) result = -KERNEL_ENOMEM;
    else {
        unsigned count = request->zcrx_bounded ? request->zcrx_remaining : UINT32_MAX;
        result = count ? kernel_socket_zcrx_receive(&request->file, ring->zcrx, count,
            &cqe.extra[0]) : 0;
    }

    bool finished = false, publish = false;
    {
        KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
        request->busy = 0;
        canceled = request->canceled || ring->stopping;
        if (result > 0) {
            if (request->zcrx_bounded) request->zcrx_remaining -= result;
            request->spare = spare;
            spare = NULL;
            cqe.flags = URING_CQE_MORE;
            publish = true;
            queue_work_locked(request);
        } else {
            if (canceled) result = -KERNEL_ECANCELED;
            if (result == -KERNEL_EAGAIN || result == -KERNEL_ENOBUFS) {
                if (result == -KERNEL_ENOBUFS) {
                    if (generation != ring->zcrx_generation) queue_work_locked(request);
                    else {
                        request->zcrx_next = ring->zcrx_parked;
                        request->zcrx_previous = &ring->zcrx_parked;
                        if (request->zcrx_next)
                            request->zcrx_next->zcrx_previous = &request->zcrx_next;
                        ring->zcrx_parked = request;
                    }
                } else if (request->dirty) queue_work_locked(request);
            } else {
                detach_pending_locked(request);
                finished = publish = true;
            }
        }
    }
    if (spare) kernel_heap_release(ring->heap, spare);
    if (finished) {
        if (kernel_wait_node_remove_sync(&request->watch)) __builtin_trap();
        retire_operand(ring, &request->file);
        kernel_heap_release(ring->heap, request);
    }
    if (publish) {
        cqe.result = result;
        complete_sqe(ring, &cqe, completed, true);
        notify_batch(ring, 1, true);
    }
}

static void run_pending(struct uring_pending *request)
{
    if (request->zcrx) {
        run_zcrx(request);
        return;
    }
    struct kernel_io_uring *ring = request->ring;
    int result;
    if (request->signal_recipient) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_pid_member *member = request->signal_recipient->members[KERNEL_PID_TID];
        uint64_t mask = __atomic_load_n(kernel_signal_fd_mask(request->file), __ATOMIC_ACQUIRE);
        result = member && kernel_signal_fd_pending_locked(member->task, mask) ?
            (int)(KERNEL_POLLIN & request->events) : 0;
    } else if (request->file)
        result = (int)(kernel_open_file_poll(request->file, request->events, NULL) & request->events);
    else result = -URING_ETIME;
    {
        KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
        request->busy = 0;
        if (request->canceled || ring->stopping) result = -KERNEL_ECANCELED;
        if (!result) {
            if (request->dirty) queue_work_locked(request);
            return;
        }
        if (request->peer) {
            struct uring_pending *peer = request->peer;
            peer->peer = request->peer = NULL;
            peer->canceled = 1;
            queue_work_locked(peer);
        }
        detach_pending_locked(request);
    }
    if (request->file && kernel_wait_node_remove_sync(&request->watch)) __builtin_trap();
    if (request->signal_recipient) kernel_task_identity_put(request->signal_recipient);
    retire_operand(ring,&request->file);
    struct uring_completion cqe = {.data=request->data, .result=result};
    complete_sqe(ring,&cqe,request->spare,true);
    kernel_heap_release(ring->heap,request);
    notify_batch(ring,1,true);
}

static void ring_worker(void *context)
{
    struct kernel_io_uring *ring = context;
    for (;;) {
        uint64_t now = kernel_time_monotonic_ns();
        uint64_t deadline = 0, observed;
        bool is_wall = false, have_wall;
        unsigned observed_wall = __atomic_load_n(&wall_generation, __ATOMIC_ACQUIRE);
        struct uring_pending *request;
        {
            KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
            struct kernel_avl_node *current = kernel_avl_first(ring->timers[0]);
            struct kernel_avl_node *absolute = kernel_avl_first(ring->timers[1]);
            uint64_t wall = absolute ? kernel_time_realtime_ns() : 0;
            have_wall = absolute != NULL;
            request = next_work_locked(ring, now, wall, current, absolute);
            if (!request && ring->stopping) return;
            if (!request && absolute && (!current || absolute->key - wall < current->key - now)) {
                deadline = absolute->key;
                is_wall = true;
            } else if (current) deadline = current->key;
            observed = ring->work_generation;
        }
        if (request) {
            run_pending(request);
            continue;
        }
        struct kernel_wait_token token;
        enum kernel_wait_wake_reason reason;
        enum kernel_scheduler_status status = !deadline ? kernel_wait_prepare(&ring->work, 0, 0, &token) :
            is_wall ? kernel_wait_prepare_realtime(&ring->work, deadline, 0, &token) :
            kernel_wait_prepare_monotonic(&ring->work, deadline, 0, &token);
        if (status) __builtin_trap();
        struct kernel_wait_node clock_node;
        if (have_wall) {
            kernel_wait_node_init(&clock_node, kernel_task_current());
            if (kernel_wait_node_bind(&wall_changed, &clock_node, &token)) __builtin_trap();
        }
        int wait;
        {
            KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
            wait = !ring->stopping && observed == ring->work_generation &&
                (!have_wall || observed_wall == __atomic_load_n(&wall_generation, __ATOMIC_ACQUIRE));
        }
        if (wait && kernel_wait_park(&token,&reason)) __builtin_trap();
        if (kernel_wait_finish(&token)) __builtin_trap();
    }
}

static int ensure_worker(struct kernel_io_uring *ring)
{
    unsigned state = __atomic_load_n(&ring->worker_state,__ATOMIC_ACQUIRE);
    if (state == 2) return 0;
    if (state || !__atomic_compare_exchange_n(&ring->worker_state,&state,1,0,
            __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return -KERNEL_EAGAIN;
#if BOAROS_MAX_CPUS > 1
    unsigned cpu = kernel_cpu_current()->logical_id;
#endif
    KERNEL_SERVICE_SCOPE(service);
#if BOAROS_MAX_CPUS > 1
    int error = kernel_thread_create_joinable_worker(ring_worker,ring,&ring->worker,cpu);
#else
    int error = kernel_thread_create_joinable(ring_worker,ring,&ring->worker);
#endif
    __atomic_store_n(&ring->worker_state,error?0:2,__ATOMIC_RELEASE);
    return error ? -KERNEL_EAGAIN : 0;
}

static void stop_worker(struct kernel_io_uring *ring)
{
    if (!ring->worker_state) return;
    {
        KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
        ring->stopping = 1;
    }
    wake_queue(&ring->work);
    KERNEL_SERVICE_SCOPE(service);
    kernel_thread_join(&ring->worker);
    ring->worker_state = 0;
}

static bool is_deferred(unsigned opcode)
{
    return opcode == URING_OP_POLL_ADD || opcode == URING_OP_TIMEOUT || opcode == URING_OP_RECV_ZC;
}

static int import_timeout(struct kernel_mm *mm, uint64_t argument, unsigned flags, uint64_t *value)
{
    if (flags & URING_TIMEOUT_IMMEDIATE_ARG) {
        if (argument > INT64_MAX) return -KERNEL_EINVAL;
        *value = argument;
        return 0;
    }
    struct { int64_t sec, nsec; } ts;
    int error = copy_in(mm, &ts, argument, sizeof(ts));
    if (error) return error;
    if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000) return -KERNEL_EINVAL;
    *value = (uint64_t)ts.sec > (INT64_MAX - (uint64_t)ts.nsec) / 1000000000 ?
        INT64_MAX : (uint64_t)ts.sec * 1000000000 + ts.nsec;
    return 0;
}

static int prepare_timeout(struct kernel_mm *mm, const struct uring_sqe *sqe, uint64_t *deadline)
{
    unsigned flags = sqe->op_flags;
    if (sqe->length != 1 || sqe->offset || sqe->extra[0] || sqe->extra[1] ||
        (flags & ~(URING_TIMEOUT_ABS | URING_TIMEOUT_BOOT | URING_TIMEOUT_REAL |
                    URING_TIMEOUT_ETIME_SUCCESS | URING_TIMEOUT_IMMEDIATE_ARG)) ||
        (flags & (URING_TIMEOUT_BOOT | URING_TIMEOUT_REAL)) ==
            (URING_TIMEOUT_BOOT | URING_TIMEOUT_REAL)) return -KERNEL_EINVAL;
    int error = import_timeout(mm, sqe->address, flags, deadline);
    if (!error && !(flags & URING_TIMEOUT_ABS)) *deadline = kernel_time_monotonic_after(*deadline);
    return error;
}

static void initialize_timer(struct uring_pending *request, struct kernel_io_uring *ring,
    const struct uring_sqe *sqe, struct uring_overflow *spare, uint64_t deadline)
{
    request->ring = ring;
    request->data = sqe->data;
    request->deadline = deadline;
    request->spare = spare;
    request->uses_realtime = !!(sqe->op_flags & URING_TIMEOUT_REAL);
    request->realtime = request->uses_realtime && (sqe->op_flags & URING_TIMEOUT_ABS);
}

/* Return one only if the request and reserved completion were transferred. */
static int defer_sqe(struct kernel_io_uring *ring, struct kernel_mm *mm,
    const struct uring_sqe *sqe, struct kernel_open_file_description **file,
    struct uring_overflow *spare, int64_t *result, const struct uring_sqe *linked,
    struct uring_overflow *linked_spare)
{
    struct kernel_wait_queue *queue = NULL;
    uint32_t events = 0;
    uint64_t deadline = 0, linked_deadline = 0;
    if ((sqe->flags & URING_SKIP_SUCCESS) ||
        ((sqe->flags & (URING_LINK|URING_HARDLINK)) && !linked)) {
        *result = -KERNEL_EOPNOTSUPP;
        return 0;
    }
    if (linked) {
        *result = prepare_timeout(mm, linked, &linked_deadline);
        if (*result) return 0;
    }
    if (sqe->opcode == URING_OP_RECV_ZC) {
        events = KERNEL_POLLIN | KERNEL_POLLERR | KERNEL_POLLHUP;
        kernel_open_file_poll(*file, events, &queue);
        if (!queue) { *result = -KERNEL_ENOTSOCK; return 0; }
    } else if (sqe->opcode == URING_OP_POLL_ADD) {
        if (sqe->length || sqe->offset || from_file(*file)) {
            *result = -KERNEL_EINVAL;
            return 0;
        }
        events = sqe->op_flags|KERNEL_POLLERR|KERNEL_POLLHUP;
        *result = kernel_open_file_poll(*file,events,&queue) & events;
        if (*result) return 0;
        if (!queue) {
            *result = -KERNEL_EINVAL;
            return 0;
        }
    } else {
        *result = prepare_timeout(mm, sqe, &deadline);
        if (*result) return 0;
    }
    *result = ensure_worker(ring);
    if (*result) return 0;
    KERNEL_NO_RECLAIM_IO;
    struct uring_pending *pending = NULL;
    if (kernel_heap_allocate_zeroed(ring->heap,1,sizeof(*pending),(void **)&pending)) {
        *result = -KERNEL_ENOMEM;
        return 0;
    }
    initialize_timer(pending, ring, sqe, spare, deadline);
    if (linked) {
        struct uring_pending *timer = NULL;
        if (kernel_heap_allocate_zeroed(ring->heap, 1, sizeof(*timer), (void **)&timer)) {
            kernel_heap_release(ring->heap, pending);
            *result = -KERNEL_ENOMEM;
            return 0;
        }
        initialize_timer(timer, ring, linked, linked_spare, linked_deadline);
        timer->peer = pending;
        pending->peer = timer;
    }
    pending->file = *file;
    pending->events = events;
    pending->zcrx = sqe->opcode == URING_OP_RECV_ZC;
    pending->zcrx_remaining = sqe->length;
    pending->zcrx_bounded = sqe->length != 0;
    if (queue) {
        if (kernel_open_file_kind(*file) == KERNEL_OPEN_FILE_KIND_SIGNALFD) {
            kernel_pid_t tid;
            if (kernel_task_user_tid(kernel_task_current(), &tid) ||
                kernel_task_identity_get_user(tid, &pending->signal_recipient)) {
                if (pending->peer) kernel_heap_release(ring->heap, pending->peer);
                kernel_heap_release(ring->heap, pending);
                *result = -KERNEL_ESRCH;
                return 0;
            }
        }
        kernel_wait_node_init_callback(&pending->watch,poll_wake,pending);
        pending->watch.event_mask = kernel_signal_fd_mask(*file);
        kernel_wait_queue_add(queue,&pending->watch);
    }
    *file = NULL;
    {
        KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
        attach_pending_locked(ring,pending);
        if (pending->peer) attach_pending_locked(ring,pending->peer);
    }
    wake_queue(&ring->work);
    return 1;
}

static int cancel_pending(struct kernel_io_uring *ring, struct kernel_mm *mm, const struct uring_sqe *sqe)
{
    unsigned flags = sqe->op_flags;
    bool update = sqe->opcode == URING_OP_TIMEOUT_REMOVE && (flags & URING_TIMEOUT_UPDATE);
    if (sqe->length || (!update && (flags || sqe->offset)) ||
        (update && (flags & ~(URING_TIMEOUT_ABS | URING_TIMEOUT_UPDATE | URING_TIMEOUT_IMMEDIATE_ARG))))
        return -KERNEL_EINVAL;
    uint64_t replacement = 0;
    if (update) {
        int error = import_timeout(mm, sqe->offset, flags, &replacement);
        if (error) return error;
        if (!(flags & URING_TIMEOUT_ABS)) replacement = kernel_time_monotonic_after(replacement);
    }
    int result = -KERNEL_ENOENT;
    {
        KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
        struct kernel_avl_node *first = NULL;
        for (struct kernel_avl_node *node = ring->pending_by_data; node;) {
            if (node->key >= sqe->address) { first = node; node = node->left; }
            else node = node->right;
        }
        for (struct kernel_avl_node *node = first; node && node->key == sqe->address;
             node = kernel_avl_next(ring->pending_by_data, node)) {
            struct uring_pending *request = (void *)((char *)node - offsetof(struct uring_pending, data_node));
            if (request->canceled ||
                (sqe->opcode == URING_OP_POLL_REMOVE && !request->file) ||
                (sqe->opcode == URING_OP_TIMEOUT_REMOVE && request->file)) continue;
            if (update) {
                if (request->busy) return -KERNEL_EALREADY;
                struct kernel_avl_node **old = &ring->timers[request->realtime];
                *old = kernel_avl_remove_node(*old, &request->deadline_node);
                request->realtime = request->uses_realtime && (flags & URING_TIMEOUT_ABS);
                request->deadline = replacement;
                request->deadline_node = (struct kernel_avl_node){.key=replacement, .height=1};
                struct kernel_avl_node **timers = &ring->timers[request->realtime];
                *timers = kernel_avl_insert_node(*timers, &request->deadline_node);
                ring->work_generation++;
            } else {
                request->canceled = 1;
                queue_work_locked(request);
            }
            result = 0;
            break;
        }
    }
    if (!result) wake_queue(&ring->work);
    return result;
}

static struct uring_personality **personality_slot(struct kernel_io_uring *ring, uint16_t id)
{
    struct uring_personality **slot = &ring->personalities;
    while (*slot && (*slot)->id != id) slot = &(*slot)->next;
    return slot;
}
static int register_personality(struct kernel_io_uring *ring)
{
    uint16_t next = ring->next_personality;
    for (unsigned i = 0; i < UINT16_MAX; i++) {
        next++;
        if (!next) next = 1;
        if (*personality_slot(ring,next)) continue;
        struct uring_personality *entry = NULL;
        if (kernel_heap_allocate(ring->heap,sizeof(*entry),(void **)&entry)) return -KERNEL_ENOMEM;
        entry->id = next;
        entry->credentials = kernel_credentials_get(kernel_credentials_current());
        entry->next = ring->personalities;
        ring->personalities = entry;
        ring->next_personality = next;
        return next;
    }
    return -KERNEL_ENOSPC;
}
static int unregister_personality(struct kernel_io_uring *ring, unsigned id)
{
    if (!id || id > UINT16_MAX) return -KERNEL_EINVAL;
    struct uring_personality **slot = personality_slot(ring,(uint16_t)id);
    if (!*slot) return -KERNEL_EINVAL;
    struct uring_personality *entry = *slot;
    *slot = entry->next;
    kernel_credentials_put(entry->credentials);
    kernel_heap_release(ring->heap,entry);
    return 0;
}
struct uring_restriction { uint16_t type; uint8_t value, reserved; uint32_t extra[3]; };
static int register_restrictions(struct kernel_io_uring *ring, struct kernel_mm *mm,
    uint64_t address, unsigned count)
{
    if (!(ring->flags & URING_DISABLED)) return -URING_EBADFD;
    if (ring->restrictions.registers_restricted || ring->restrictions.operations_restricted)
        return -KERNEL_EBUSY;
    if (!address || count > (4 + URING_REGISTER_LIMIT + URING_OP_LIMIT)) return -KERNEL_EINVAL;
    struct uring_restrictions allowed = {0};
    if (!count) allowed.registers_restricted = allowed.operations_restricted = 1;
    for (unsigned i = 0; i < count; i++) {
        struct uring_restriction row;
        int error = copy_in(mm,&row,address+i*sizeof(row),sizeof(row));
        if (error) return error;
        switch (row.type) {
        case 0:
            if (row.value >= URING_REGISTER_LIMIT) return -KERNEL_EINVAL;
            allowed.registers |= UINT64_C(1) << row.value;
            allowed.registers_restricted = 1;
            break;
        case 1:
            if (row.value >= URING_OP_LIMIT) return -KERNEL_EINVAL;
            allowed.operations[row.value / 64] |= UINT64_C(1) << (row.value % 64);
            allowed.operations_restricted = 1;
            break;
        case 2:
            allowed.allowed = row.value;
            allowed.operations_restricted = 1;
            break;
        case 3:
            allowed.required = row.value;
            allowed.operations_restricted = 1;
            break;
        default:
            return -KERNEL_EINVAL;
        }
    }
    ring->restrictions = allowed;
    return 0;
}

struct uring_submission {
    struct uring_sqe sqe;
    struct kernel_open_file_description *file;
    struct kernel_open_file_description *file_in;
    struct uring_overflow *spare;
    const unsigned char *extension;
    const struct kernel_credentials *credentials;
    struct uring_fixed_buffers *buffers;
    int error;
    bool linked_file;
};

int kernel_io_uring_abort(struct kernel_task *task)
{
    struct kernel_io_uring_call *call = task->io_uring_call;
    if (!call) return 0;
    if (call->preparing_fd) kernel_files_cancel_reservation(call->preparing_fd);
    if (call->preparing_file) kernel_heap_release(call->files->heap, call->preparing_file);
    if (call->preparing_ring) {
        struct kernel_io_uring *prepared = call->preparing_ring;
        release_pages(prepared);
        kernel_heap_release(prepared->heap, prepared);
    }
    call->preparing_fd = NULL;
    call->preparing_file = NULL;
    call->preparing_ring = NULL;
    struct kernel_io_uring *ring = from_file(call->file);
    if (call->overridden) {
        task->credentials_override = call->saved_credentials;
        call->overridden = false;
    }
    while (call->first < call->submitted) {
        struct uring_submission *entry = &call->batch[call->first];
        if (put_file(call->files, &entry->file)) return -KERNEL_EIO;
        if (put_file(call->files, &entry->file_in)) return -KERNEL_EIO;
        if (entry->credentials) kernel_credentials_put(entry->credentials);
        put_buffers(entry->buffers);
        struct uring_completion canceled = {.data=entry->sqe.data, .result=-KERNEL_ECANCELED};
        complete_sqe(ring, &canceled, entry->spare, false);
        call->first++;
    }
    if (call->allocated) kernel_heap_release(ring->heap, call->batch);
    call->allocated = false;
    if (ring && call->submitted) wake_queue(&ring->ready);
    call->submitted = call->first = 0;
    if (kernel_io_uring_zcrx_release(&call->preparing_zcrx)) return -KERNEL_EIO;
    put_buffers(call->preparing_buffers);
    call->preparing_buffers = NULL;
    if (put_file(call->files, &call->file)) return -KERNEL_EIO;
    task->io_uring_call = NULL;
    return 0;
}

static const struct uring_sqe *sqe_at(const struct kernel_io_uring *ring)
{
    unsigned index = ring->sq_head & (ring->sq_entries - 1U);
    if (!(ring->flags & URING_NO_SQARRAY)) {
        uint32_t *slot = page_address(ring, 0, ring->sq_array + index * sizeof(*slot));
        index = __atomic_load_n(slot, __ATOMIC_RELAXED);
        if (index >= ring->sq_entries) return NULL;
    }
    return page_address(ring, ring->ring_pages, index * ring->sqe_size);
}

/* Plain NOPs have no file, observer, or deferred operation. Commit a short
 * batch with one acquisition of the completion lock and no heap requests. */
static unsigned submit_nops(struct kernel_io_uring *ring, unsigned count, unsigned *ready)
{
    unsigned completed = 0;
    if (ring->restrictions.operations_restricted) return 0;
    while (completed < count) {
        const struct uring_sqe *user = sqe_at(ring);
        if (!user || __atomic_load_n(&user->opcode,__ATOMIC_RELAXED) != URING_OP_NOP) break;
        unsigned old = completed;
        {
            KERNEL_RAW_SCOPE(guard,&ring->cq_lock);
            if (ring->overflow) return completed;
            unsigned space = ring->cq_entries - cq_ready(ring) - ring->cq_reserved;
            if (space > count - completed) space = count - completed;
            if (space > 16) space = 16;
            for (unsigned i = 0; i < space; i++) {
                if (i) user = sqe_at(ring);
                if (!user) break;
                if (__atomic_load_n(&user->opcode, __ATOMIC_RELAXED) != URING_OP_NOP ||
                    __atomic_load_n(&user->flags, __ATOMIC_RELAXED) ||
                    __atomic_load_n(&user->op_flags, __ATOMIC_RELAXED) ||
                    __atomic_load_n(&user->ioprio, __ATOMIC_RELAXED) ||
                    __atomic_load_n(&user->buffer, __ATOMIC_RELAXED) ||
                    __atomic_load_n(&user->personality, __ATOMIC_RELAXED) ||
                    __atomic_load_n(&user->file_index, __ATOMIC_RELAXED)) break;
                struct uring_completion cqe = {.data = __atomic_load_n(&user->data, __ATOMIC_RELAXED)};
                publish_locked(ring,&cqe);
                ring->sq_head++;
                completed++;
            }
            if (completed != old) {
                __atomic_store_n(&ring->shared->cq_tail, ring->cq_tail, __ATOMIC_RELEASE);
                *ready = cq_ready(ring);
            }
        }
        if (completed == old) break;
    }
    return completed;
}

static int submit(struct kernel_io_uring *ring, struct kernel_files *files,
    struct kernel_mm *mm, unsigned requested, unsigned *ready)
{
    struct uring_submission inline_batch[1];
    struct uring_submission *batch = inline_batch;
    struct kernel_io_uring_call *call = kernel_task_current()->io_uring_call;
    unsigned fast = 0, submitted = 0;
    int resource_error = 0;
    {
        KERNEL_LOCK_SCOPE(submission);
        kernel_mutex_lock(&ring->submit_lock, &submission);
        uint32_t pending = __atomic_load_n(&ring->shared->sq_tail, __ATOMIC_ACQUIRE) - ring->sq_head;
        if (pending > ring->sq_entries) pending = ring->sq_entries;
        if (pending > requested) pending = requested;
        fast = submit_nops(ring,pending,ready);
        pending -= fast;
        if (pending) *ready = 0;
        if (fast && !pending) {
            __atomic_store_n(&ring->shared->sq_head, ring->sq_head, __ATOMIC_RELEASE);
            kernel_lock_release(&submission);
            notify_batch(ring, 1, false);
            return (int)fast;
        }
        bool wide = ring->sqe_size == 128;
        if (pending && (wide || pending > sizeof(inline_batch) / sizeof(inline_batch[0])) &&
            kernel_heap_allocate(ring->heap, pending * (sizeof(*batch) + (wide ? 64 : 0)),
                (void **)&batch)) {
            resource_error = -KERNEL_EAGAIN;
            pending = 0;
            batch = inline_batch;
        }
        call->batch = batch;
        call->allocated = batch != inline_batch;
        unsigned char *wide_entries = (unsigned char *)(batch + pending);
        bool linked = false;
        for (; submitted < pending; submitted++) {
            const unsigned char *slot = (const unsigned char *)sqe_at(ring);
            if (!slot) {
                ring->sq_head++;
                __atomic_store_n(&ring->shared->sq_dropped, ++ring->dropped, __ATOMIC_RELAXED);
                break;
            }
            struct uring_submission *entry = &batch[submitted];
            entry->file = NULL;
            entry->file_in = NULL;
            entry->spare = NULL;
            entry->credentials = NULL;
            entry->buffers = NULL;
            memcpy(&entry->sqe, slot, sizeof(entry->sqe));
            resource_error = completion_reserve(ring, &entry->spare, is_deferred(entry->sqe.opcode) ||
                entry->sqe.opcode == URING_OP_LINK_TIMEOUT);
            if (resource_error) break;
            entry->extension = NULL;
            if (wide && entry->sqe.opcode == URING_OP_URING_CMD) {
                unsigned char *extension = wide_entries + submitted * 64;
                memcpy(extension, slot + sizeof(entry->sqe), 64);
                entry->extension = extension;
            }
            ring->sq_head++;
            call->submitted++;
            entry->error = validate_sqe(ring, &entry->sqe);
            if (!entry->error && entry->sqe.personality) {
                struct uring_personality *personality = *personality_slot(ring,entry->sqe.personality);
                if (!personality) entry->error = -KERNEL_EINVAL;
                else entry->credentials = kernel_credentials_get(personality->credentials);
            }
            entry->linked_file = linked && needs_file(entry->sqe.opcode);
            linked = (entry->sqe.flags & (URING_LINK | URING_HARDLINK)) != 0;
            if (!entry->error && !entry->linked_file && needs_file(entry->sqe.opcode))
                entry->error = pin_operands(ring, files, &entry->sqe, &entry->file, &entry->file_in);
            if (!entry->error && (entry->sqe.opcode == URING_OP_READ_FIXED ||
                    entry->sqe.opcode == URING_OP_WRITE_FIXED)) {
                struct uring_fixed_buffers *table = ring->fixed_buffers;
                const struct uring_fixed_buffer *buffer = table && entry->sqe.buffer < table->count ?
                    table->slots[entry->sqe.buffer] : NULL;
                uint64_t address = entry->sqe.address;
                if (!buffer || address < buffer->address ||
                    address - buffer->address > buffer->length ||
                    entry->sqe.length > buffer->length - (address - buffer->address))
                    entry->error = -KERNEL_EFAULT;
                else {
                    __atomic_fetch_add(&table->references, 1, __ATOMIC_RELAXED);
                    entry->buffers = table;
                }
            }
            if (entry->error && !(ring->flags & URING_SUBMIT_ALL) &&
                !(entry->sqe.flags & (URING_LINK|URING_HARDLINK))) {
                submitted++;
                break;
            }
        }
        __atomic_store_n(&ring->shared->sq_head, ring->sq_head, __ATOMIC_RELEASE);
    }
    int events = fast != 0, cancel_next = 0;
    for (unsigned i = 0; i < submitted; i++) {
        struct uring_submission *entry = &batch[i];
        struct uring_submission *linked = NULL;
        if (i + 1 < submitted && (entry->sqe.flags & (URING_LINK|URING_HARDLINK)) &&
            batch[i+1].sqe.opcode == URING_OP_LINK_TIMEOUT && !batch[i+1].error &&
            !(batch[i+1].sqe.flags & (URING_LINK|URING_HARDLINK|URING_SKIP_SUCCESS)))
            linked = &batch[i+1];
        struct uring_completion cqe = {.data=entry->sqe.data};
        int64_t result = cancel_next ? -KERNEL_ECANCELED : entry->error;
        bool deferred = false;
        if (!result && entry->linked_file) {
            KERNEL_LOCK_SCOPE(submission);
            kernel_mutex_lock(&ring->submit_lock, &submission);
            result = pin_operands(ring, files, &entry->sqe, &entry->file, &entry->file_in);
        }
        if (!result) {
            if (entry->credentials) {
                call->saved_credentials = kernel_credentials_override(entry->credentials);
                call->overridden = true;
            }
            if (linked && entry->sqe.opcode == URING_OP_NOP) {
                uint64_t unused;
                linked->error = prepare_timeout(mm, &linked->sqe, &unused);
                if (!linked->error) linked->error = -KERNEL_ECANCELED;
                result = dispatch_sqe(ring, files, mm, &entry->sqe, &entry->file,
                    &entry->file_in, entry->extension, entry->buffers, &cqe);
            } else if (linked && entry->sqe.opcode != URING_OP_POLL_ADD) {
                result = -KERNEL_EOPNOTSUPP;
            } else if (is_deferred(entry->sqe.opcode))
                deferred = defer_sqe(ring, mm, &entry->sqe, &entry->file, entry->spare, &result,
                    linked ? &linked->sqe : NULL, linked ? linked->spare : NULL);
            else if (entry->sqe.opcode == URING_OP_POLL_REMOVE ||
                     entry->sqe.opcode == URING_OP_TIMEOUT_REMOVE ||
                     entry->sqe.opcode == URING_OP_ASYNC_CANCEL)
                result = cancel_pending(ring, mm, &entry->sqe);
            else result = dispatch_sqe(ring, files, mm, &entry->sqe, &entry->file, &entry->file_in,
                entry->extension, entry->buffers, &cqe);
            if (call->overridden) {
                kernel_credentials_override(call->saved_credentials);
                call->overridden = false;
            }
        }
        if (entry->credentials) {
            kernel_credentials_put(entry->credentials);
            entry->credentials = NULL;
        }
        put_buffers(entry->buffers);
        entry->buffers = NULL;
        if (deferred) {
            call->first++;
            if (linked) {
                if (linked->credentials) kernel_credentials_put(linked->credentials);
                call->first++;
                i++;
            }
            continue;
        }
        if (linked && !linked->error) linked->error = -KERNEL_ECANCELED;
        if (put_file(files, &entry->file) && result >= 0) result = -KERNEL_EIO;
        if (put_file(files, &entry->file_in) && result >= 0) result = -KERNEL_EIO;
        if (entry->sqe.flags & URING_HARDLINK) {
            if (entry->error) cancel_next = 1;
        } else cancel_next = (entry->sqe.flags & URING_LINK) && (entry->error || result < 0);
        cqe.result = (int32_t)result;
        int publish = !(entry->sqe.flags & URING_SKIP_SUCCESS) || result < 0;
        complete_sqe(ring, publish ? &cqe : NULL, entry->spare, false);
        call->first++;
        events |= publish;
    }
    if (call->allocated) kernel_heap_release(ring->heap, batch);
    call->allocated = false;
    call->submitted = call->first = 0;
    notify_batch(ring, events, false);
    return fast || submitted ? (int)(fast + submitted) : resource_error;
}
struct uring_getevents_arg {
    uint64_t sigmask;
    uint32_t sigmask_size, min_wait_us;
    uint64_t timespec;
};
static int import_wait(struct kernel_mm *mm, const uint64_t *args, uint64_t *deadline,
    int *immediate, uint64_t *mask, int *have_mask)
{
    unsigned flags = (unsigned)args[3];
    size_t size = args[5];
    uint64_t sigmask = args[4];
    if (flags & URING_EXT_ARG) {
        struct uring_getevents_arg arg;
        if (size != sizeof(arg)) return -KERNEL_EINVAL;
        int error = copy_in(mm, &arg, args[4], sizeof(arg));
        if (error) return error;
        if (arg.min_wait_us) return -KERNEL_EINVAL;
        sigmask = arg.sigmask;
        size = arg.sigmask_size;
        if (arg.timespec) {
            struct { int64_t seconds, nanoseconds; } ts;
            error = copy_in(mm, &ts, arg.timespec, sizeof(ts));
            if (error) return error;
            if (ts.seconds < 0 || ts.nanoseconds < 0 || ts.nanoseconds >= 1000000000 ||
                (uint64_t)ts.seconds > (UINT64_MAX - ts.nanoseconds) / 1000000000)
                return -KERNEL_EINVAL;
            uint64_t target = (uint64_t)ts.seconds * 1000000000 + ts.nanoseconds;
            if (!(flags & URING_ABS_TIMER)) {
                uint64_t now = kernel_time_monotonic_ns();
                target = target > UINT64_MAX - now ? UINT64_MAX : now + target;
            }
            enum kernel_time_status status = kernel_time_deadline_from_monotonic(target, deadline);
            if (status == KERNEL_TIME_STATUS_DEADLINE_PASSED) *immediate = 1;
            else if (status) return -KERNEL_EINVAL;
        }
    }
    if (!sigmask) return 0;
    if (size != sizeof(*mask)) return -KERNEL_EINVAL;
    int error = copy_in(mm, mask, sigmask, size);
    if (!error) *have_mask = 1;
    return error;
}
static int wait_cq(struct kernel_io_uring *ring, struct kernel_task *task,
    struct kernel_mm *mm, const uint64_t *args)
{
    uint64_t deadline = 0, mask = 0, saved = 0;
    int immediate = 0, have_mask = 0;
    int error = import_wait(mm, args, &deadline, &immediate, &mask, &have_mask);
    if (error) return error;
    if (have_mask && kernel_signal_set_temporary_mask(task, mask, &saved)) __builtin_trap();
    unsigned min = (unsigned)args[2];
    if (min > ring->cq_entries) min = ring->cq_entries;
    for (;;) {
        flush_overflow(ring);
        struct kernel_wait_token token;
        enum kernel_wait_wake_reason reason = KERNEL_WAIT_WOKEN;
        {
            KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
            if (cq_ready(ring) >= min) break;
            if (immediate || kernel_signal_has_pending(task)) {
                error = cq_ready(ring) ? 0 : immediate ? -URING_ETIME : -KERNEL_EINTR;
                break;
            }
            if (kernel_wait_prepare(&ring->ready, deadline, KERNEL_WAIT_INTERRUPTIBLE, &token))
                __builtin_trap();
        }
        if (kernel_wait_park(&token, &reason) || kernel_wait_finish(&token)) __builtin_trap();
        if (reason == KERNEL_WAIT_TIMEOUT) immediate = 1;
    }
    if (have_mask) kernel_signal_restore_temporary_mask(task, saved, error == -KERNEL_EINTR);
    return error;
}

struct uring_probe { uint8_t last, length; uint16_t reserved; uint32_t reserved2[3]; };
struct uring_probe_op { uint8_t op, reserved; uint16_t flags; uint32_t reserved2; };
static __attribute__((noinline)) int register_probe(struct kernel_mm *mm, uint64_t address, unsigned max)
{
    if (max > 256) return -KERNEL_EINVAL;
    if (max > URING_OP_LIMIT) max = URING_OP_LIMIT;
    struct {
        struct uring_probe probe;
        struct uring_probe_op ops[URING_OP_LIMIT];
    } output;
    size_t size = sizeof(output.probe) + max * sizeof(output.ops[0]);
    int error = copy_in(mm, &output, address, size);
    if (error) return error;
    for (size_t i = 0; i < size; i++)
        if (((unsigned char *)&output)[i]) return -KERNEL_EINVAL;
    output.probe.last = URING_OP_LIMIT - 1U;
    output.probe.length = max;
    for (unsigned i = 0; i < max; i++) {
        output.ops[i].op = i;
        output.ops[i].flags = opcode_supported(i);
    }
    return copy_out(mm, address, &output, size);
}
static int import_slot(struct kernel_files *files, struct kernel_mm *mm, uint64_t address,
    struct kernel_open_file_description **out, bool updating)
{
    int32_t fd;
    int error = copy_in(mm, &fd, address, sizeof(fd));
    if (error || fd == -1) return error;
    if (fd == -2 && updating) return 1;
    error = pin_file(files, (unsigned)fd, out);
    if (!error && from_file(*out)) {
        (void)put_file(files, out);
        return -KERNEL_EBADF;
    }
    return error;
}
static void release_file_table(struct kernel_heap *heap, struct kernel_files *files,
    struct uring_file_table *table)
{
    for (unsigned i = 0; i < table->count; i++) (void)put_file(files, &table->files[i]);
    kernel_heap_release(heap, table);
}
static int register_buffers(struct kernel_io_uring *ring, struct kernel_mm *mm,
    uint64_t address, unsigned count)
{
    if (!address) return -KERNEL_EFAULT;
    if (!count || count > 16384) return -KERNEL_EINVAL;
    struct kernel_io_uring_call *call = kernel_task_current()->io_uring_call;
    struct uring_fixed_buffers *table = NULL;
    if (kernel_heap_allocate_zeroed(ring->heap, 1, sizeof(*table) +
            count * sizeof(table->slots[0]), (void **)&table)) return -KERNEL_ENOMEM;
    table->heap = ring->heap;
    table->references = 1;
    table->count = count;
    call->preparing_buffers = table;
    int error = 0;
    uint64_t pages = 0, limit = ring->memory_account ?
        kernel_task_current_memlock_limit() >> BOAROS_PAGE_SHIFT : UINT64_MAX;
    for (unsigned i = 0; i < count; i++) {
        struct { uint64_t address, length; } iov;
        error = copy_in(mm, &iov, address + i * sizeof(iov), sizeof(iov));
        if (error) break;
        if (!iov.address && !iov.length) continue;
        if (!iov.address || !iov.length || kernel_user_range_check(iov.address, iov.length)) {
            error = -KERNEL_EFAULT;
            break;
        }
        if (iov.length > (UINT64_C(1) << 40)) {
            error = -KERNEL_EINVAL;
            break;
        }
        size_t span = ((iov.address & BOAROS_PAGE_MASK) + iov.length + BOAROS_PAGE_MASK) >>
            BOAROS_PAGE_SHIFT;
        if (span > limit - pages || span > ring->allocator->total_pages ||
            span > (SIZE_MAX - sizeof(struct uring_fixed_buffer)) / sizeof(struct kernel_mm_long_pin)) {
            error = -KERNEL_ENOMEM;
            break;
        }
        struct uring_fixed_buffer *buffer = NULL;
        if (kernel_heap_allocate_zeroed(ring->heap, 1, sizeof(*buffer) +
                span * sizeof(buffer->pages[0]), (void **)&buffer)) {
            error = -KERNEL_ENOMEM;
            break;
        }
        buffer->address = iov.address;
        buffer->length = iov.length;
        table->slots[i] = buffer;
        pages += span;
    }
    if (!error && pages && ring->memory_account) {
        error = kernel_uid_account_pin_charge(ring->memory_account, pages, limit);
        if (!error) {
            table->charged = pages;
            table->account = ring->memory_account;
        }
    }
    for (unsigned i = 0; !error && i < count; i++) {
        struct uring_fixed_buffer *buffer = table->slots[i];
        if (!buffer) continue;
        uint64_t end = buffer->address + buffer->length;
        for (uint64_t page = buffer->address & ~BOAROS_PAGE_MASK; page < end; page += BOAROS_PAGE_SIZE) {
            error = kernel_mm_long_pin_acquire(mm, page, &buffer->pages[buffer->acquired]);
            if (error) break;
            buffer->acquired++;
        }
    }
    if (!error) {
        KERNEL_LOCK_SCOPE(guard);
        kernel_mutex_lock(&ring->submit_lock, &guard);
        if (ring->fixed_buffers) error = -KERNEL_EBUSY;
        else {
            ring->fixed_buffers = table;
            call->preparing_buffers = NULL;
            return 0;
        }
    }
    put_buffers(table);
    call->preparing_buffers = NULL;
    return error;
}

static int register_files(struct kernel_io_uring *ring, struct kernel_files *files,
    struct kernel_mm *mm, uint64_t address, unsigned count, bool sparse)
{
    if (ring->files) return -KERNEL_EBUSY;
    if (!count || count > URING_MAX_SQ) return -KERNEL_EINVAL;
    if (!sparse && (!address || address > UINT64_MAX - (uint64_t)count * sizeof(int32_t)))
        return -KERNEL_EFAULT;
    struct uring_file_table *table = NULL;
    size_t size = sizeof(*table) + count * sizeof(table->files[0]) +
        ((count + 63U) / 64) * sizeof(uint64_t);
    if (kernel_heap_allocate_zeroed(ring->heap, 1, size, (void **)&table))
        return -KERNEL_ENOMEM;
    table->count = table->end = count;
    for (unsigned i = 0; !sparse && i < count; i++) {
        struct kernel_open_file_description *file = NULL;
        int error = import_slot(files, mm, address + i * sizeof(int32_t), &file, false);
        if (error) {
            release_file_table(ring->heap, files, table);
            return error;
        }
        if (file) set_file_slot(table, i, file);
    }
    ring->files = table;
    return 0;
}
static int update_file_slots(struct kernel_io_uring *ring, struct kernel_files *files,
    struct kernel_mm *mm, uint64_t data, uint64_t tags, unsigned count, unsigned first)
{
    if (!ring->files) return -KERNEL_ENXIO;
    if (!count || first >= ring->files->count || count > ring->files->count - first)
        return -KERNEL_EINVAL;
    if (data > UINT64_MAX - (uint64_t)count * sizeof(int32_t) ||
        (tags && tags > UINT64_MAX - (uint64_t)count * sizeof(uint64_t)))
        return -KERNEL_EFAULT;
    int error = 0;
    unsigned i = 0;
    for (; i < count; i++) {
        if (tags) {
            uint64_t tag;
            error = copy_in(mm, &tag, tags + i * sizeof(tag), sizeof(tag));
            if (!error && tag) error = -KERNEL_EOPNOTSUPP;
            if (error) break;
        }
        struct kernel_open_file_description *file = NULL;
        error = import_slot(files, mm, data + i * sizeof(int32_t), &file, true);
        if (error < 0) break;
        if (!error) {
            struct kernel_open_file_description *old = ring->files->files[first + i];
            set_file_slot(ring->files, first + i, file);
            (void)put_file(files, &old);
        }
    }
    return i ? (int)i : error;
}
static int register_files2(struct kernel_io_uring *ring, struct kernel_files *files,
    struct kernel_mm *mm, uint64_t address, unsigned size)
{
    struct { uint32_t count, flags; uint64_t reserved, data, tags; } request;
    if (size != sizeof(request)) return -KERNEL_EINVAL;
    int error = copy_in(mm, &request, address, size);
    if (error) return error;
    if ((request.flags & ~1U) || request.reserved || (request.flags && request.data))
        return -KERNEL_EINVAL;
    if (!request.count || request.count > URING_MAX_SQ) return -KERNEL_EINVAL;
    if (request.tags) {
        if (request.tags > UINT64_MAX - (uint64_t)request.count * sizeof(uint64_t))
            return -KERNEL_EFAULT;
        for (unsigned i = 0; i < request.count; i++) {
            uint64_t tag;
            error = copy_in(mm, &tag, request.tags + i * sizeof(tag), sizeof(tag));
            if (error) return error;
            if (tag) return -KERNEL_EOPNOTSUPP;
        }
    }
    return register_files(ring, files, mm, request.data, request.count, request.flags != 0);
}
static int register_eventfd(struct kernel_io_uring *ring, struct kernel_files *files,
    struct kernel_mm *mm, uint64_t address, bool async)
{
    if (ring->eventfd) return -KERNEL_EBUSY;
    int32_t fd;
    int error = copy_in(mm, &fd, address, sizeof(fd));
    if (error) return error;
    struct kernel_open_file_description *file = NULL;
    error = pin_file(files, (unsigned)fd, &file);
    if (error) return error;
    if (file->kind != KERNEL_OPEN_FILE_KIND_EVENTFD) {
        (void)put_file(files, &file);
        return -KERNEL_EINVAL;
    }
    ring->eventfd_async = async;
    ring->eventfd = file;
    __atomic_store_n(&ring->has_eventfd, 1, __ATOMIC_RELEASE);
    return 0;
}
static __attribute__((noinline)) int ring_register(struct kernel_io_uring *ring, struct kernel_files *files,
    struct kernel_mm *mm, const uint64_t *args)
{
    KERNEL_LOCK_SCOPE(submission);
    kernel_mutex_lock(&ring->submit_lock, &submission);
    uint64_t address = args[2];
    unsigned count = (unsigned)args[3];
    unsigned opcode = (unsigned)args[1];
    if (ring->issuer && ring->issuer != kernel_task_current()) return -KERNEL_EEXIST;
    if (ring->restrictions.registers_restricted && !(ring->flags & URING_DISABLED) &&
        (opcode >= URING_REGISTER_LIMIT ||
            !(ring->restrictions.registers & (UINT64_C(1) << opcode)))) return -KERNEL_EACCES;
    switch (opcode) {
    case URING_REGISTER_BUFFERS:
        if (ring->fixed_buffers) return -KERNEL_EBUSY;
        kernel_lock_release(&submission);
        return register_buffers(ring, mm, address, count);
    case URING_UNREGISTER_BUFFERS: {
        if (address || count) return -KERNEL_EINVAL;
        struct uring_fixed_buffers *table = ring->fixed_buffers;
        if (!table) return -KERNEL_ENXIO;
        ring->fixed_buffers = NULL;
        kernel_lock_release(&submission);
        put_buffers(table);
        return 0;
    }
    case URING_REGISTER_ZCRX: {
        if (count != 1 || !(ring->flags & URING_DEFER_TASKRUN) ||
            !(ring->flags & URING_CQE32)) return -KERNEL_EINVAL;
        if (ring->zcrx) return -KERNEL_EBUSY;
        struct kernel_io_uring_call *call = kernel_task_current()->io_uring_call;
        int error = kernel_io_uring_zcrx_register(ring->heap, mm, ring->memory_account,
            address, &call->preparing_zcrx);
        if (!error) {
            __atomic_store_n(&ring->zcrx, call->preparing_zcrx, __ATOMIC_RELEASE);
            call->preparing_zcrx = NULL;
        }
        return error;
    }
    case URING_REGISTER_ZCRX_CTRL: {
        struct { uint32_t id, op; uint64_t reserved[2], data[6]; } control;
        if (count) return -KERNEL_EINVAL;
        int error = copy_in(mm, &control, address, sizeof(control));
        if (error) return error;
        if (control.reserved[0] || control.reserved[1]) return -KERNEL_EFAULT;
        if (control.id || !ring->zcrx) return -KERNEL_ENXIO;
        if (control.op) return -KERNEL_EOPNOTSUPP;
        for (unsigned i = 0; i < 6; i++) if (control.data[i]) return -KERNEL_EINVAL;
        kernel_lock_release(&submission);
        refill_zcrx(ring);
        return 0;
    }
    case URING_REGISTER_FILES:
        return register_files(ring, files, mm, address, count, false);
    case URING_REGISTER_FILES2:
        return register_files2(ring, files, mm, address, count);
    case URING_UNREGISTER_FILES:
        if (address || count) return -KERNEL_EINVAL;
        if (!ring->files) return -KERNEL_ENXIO;
        {
            struct uring_file_table *table = ring->files;
            ring->files = NULL;
            kernel_lock_release(&submission);
            release_file_table(ring->heap, files, table);
            return 0;
        }
    case URING_REGISTER_FILES_UPDATE: {
        struct { uint32_t offset, reserved; uint64_t data; } update;
        if (!count) return -KERNEL_EINVAL;
        int error = copy_in(mm, &update, address, sizeof(update));
        if (error) return error;
        if (update.reserved) return -KERNEL_EINVAL;
        return update_file_slots(ring, files, mm, update.data, 0, count, update.offset);
    }
    case URING_REGISTER_FILES_UPDATE2: {
        struct { uint32_t offset, reserved; uint64_t data, tags; uint32_t count, reserved2; } update;
        if (count != sizeof(update)) return -KERNEL_EINVAL;
        int error = copy_in(mm, &update, address, sizeof(update));
        if (error) return error;
        if (update.reserved || update.reserved2) return -KERNEL_EINVAL;
        return update_file_slots(ring, files, mm, update.data, update.tags,
            update.count, update.offset);
    }
    case URING_REGISTER_FILE_RANGE: {
        struct { uint32_t start, count; uint64_t reserved; } range;
        if (count != 0) return -KERNEL_EINVAL;
        int error = copy_in(mm, &range, address, sizeof(range));
        if (error) return error;
        struct uring_file_table *table = ring->files;
        if (!table || range.reserved || range.start > table->count ||
            range.count > table->count - range.start) return -KERNEL_EINVAL;
        table->next = table->start = range.start;
        table->end = range.start + range.count;
        return 0;
    }
    case URING_REGISTER_EVENTFD:
    case URING_REGISTER_EVENTFD_ASYNC:
        if (count != 1) return -KERNEL_EINVAL;
        return register_eventfd(ring, files, mm, address, args[1] == URING_REGISTER_EVENTFD_ASYNC);
    case URING_UNREGISTER_EVENTFD:
        if (address || count) return -KERNEL_EINVAL;
        if (!ring->eventfd) return -KERNEL_ENXIO;
        __atomic_store_n(&ring->has_eventfd, 0, __ATOMIC_RELEASE);
        return put_file(files, &ring->eventfd);
    case URING_REGISTER_PROBE:
        return register_probe(mm, address, count);
    case URING_REGISTER_PERSONALITY:
        if (address || count) return -KERNEL_EINVAL;
        return register_personality(ring);
    case URING_UNREGISTER_PERSONALITY:
        if (address) return -KERNEL_EINVAL;
        return unregister_personality(ring,count);
    case URING_REGISTER_RESTRICTIONS:
        return register_restrictions(ring, mm, address, count);
    case URING_ENABLE:
        if (address || count) return -KERNEL_EINVAL;
        if (!(ring->flags & URING_DISABLED)) return -URING_EBADFD;
        if (ring->flags & URING_SINGLE_ISSUER) {
            ring->issuer = kernel_task_current();
            kernel_task_storage_get(ring->issuer);
        }
        __atomic_fetch_and(&ring->flags, ~URING_DISABLED, __ATOMIC_RELEASE);
        return 0;
    default:
        return -KERNEL_EINVAL;
    }
}

static void refill_zcrx(struct kernel_io_uring *ring)
{
    struct kernel_io_uring_zcrx *area = __atomic_load_n(&ring->zcrx, __ATOMIC_ACQUIRE);
    if (!area || !kernel_io_uring_zcrx_refill(area)) return;
    bool wake = false;
    {
        KERNEL_RAW_SCOPE(guard, &ring->cq_lock);
        ring->zcrx_generation++;
        wake = ring->zcrx_parked != NULL;
        while (ring->zcrx_parked) queue_work_locked(ring->zcrx_parked);
    }
    if (wake) wake_queue(&ring->work);
}

static __attribute__((noinline)) int ring_enter(struct kernel_io_uring *ring, struct kernel_task *caller,
    struct kernel_files *files, struct kernel_mm *mm, const uint64_t *args)
{
    unsigned flags = (unsigned)args[3], requested = (unsigned)args[1];
    if (flags & ~(URING_GETEVENTS|URING_EXT_ARG|URING_ABS_TIMER)) return -KERNEL_EINVAL;
    if (__atomic_load_n(&ring->flags, __ATOMIC_ACQUIRE) & URING_DISABLED) return -URING_EBADFD;
    if ((requested || (ring->flags & URING_DEFER_TASKRUN)) &&
        ring->issuer && ring->issuer != caller) return -KERNEL_EEXIST;
    if (ring->flags & URING_DEFER_TASKRUN) refill_zcrx(ring);
    int count = 0;
    unsigned ready = 0;
    drain_operands(ring,files);
    if ((flags & URING_GETEVENTS) || !(ring->flags & URING_DEFER_TASKRUN)) flush_overflow(ring);
    if (requested) {
        count = submit(ring, files, mm, requested, &ready);
        if (count != (int)requested) return count;
    }
    if ((flags & URING_GETEVENTS) &&
        ((flags & URING_EXT_ARG) || args[4] || ready < (unsigned)args[2])) {
        int error = wait_cq(ring, caller, mm, args);
        if (!count && error) return error;
    }
    return count;
}

enum kernel_syscall_status syscall_handle_io_uring(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *decoded)
{
    struct kernel_files *files;
    struct kernel_mm *mm;
    if (kernel_task_mm_borrow_mutable(caller, &mm) || kernel_task_files_borrow(caller, &files))
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    const uint64_t *args = request->arguments;
    struct kernel_io_uring_call call = {.files=files};
    caller->io_uring_call = &call;
    int result;
    if (request->number == URING_SETUP) result = setup_ring(&call, mm, (unsigned)args[0], args[1]);
    else {
        result = pin_file(files, (unsigned)args[0], &call.file);
        if (!result) {
            struct kernel_io_uring *ring = from_file(call.file);
            result = !ring ? -KERNEL_EOPNOTSUPP : request->number == URING_ENTER ?
                ring_enter(ring, caller, files, mm, args) : ring_register(ring, files, mm, args);
        }
    }
    if (kernel_io_uring_abort(caller) && result >= 0) result = -KERNEL_EIO;
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = result;
    return KERNEL_SYSCALL_STATUS_OK;
}
