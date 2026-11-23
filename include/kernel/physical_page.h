#ifndef BOAROS_KERNEL_PHYSICAL_PAGE_H
#define BOAROS_KERNEL_PHYSICAL_PAGE_H

#include <kernel/boot_memory.h>
#include <kernel/raw_lock.h>

#include <stdint.h>

#define PHYSICAL_PAGE_MAX_ORDER 31U
#define PHYSICAL_PAGE_FAULT_BATCH 8U

enum physical_page_status {
    PHYSICAL_PAGE_STATUS_OK = 0,
    PHYSICAL_PAGE_STATUS_INVALID,
    PHYSICAL_PAGE_STATUS_EMPTY,
    PHYSICAL_PAGE_STATUS_DOUBLE_FREE,
    PHYSICAL_PAGE_STATUS_STATE,
};

typedef void *(*physical_page_access_fn)(uint64_t physical_address);
typedef uint64_t (*physical_page_reclaim_fn)(void *context,
                                             uint64_t target_pages);
struct kernel_memory_statistics;
struct physical_page_reclaim_hooks {
    physical_page_reclaim_fn reclaim;
    void (*notify)(void *, uint64_t available_pages);
    /* Optional filter under allocator->lock. Read only, no locks or waits.
     * Allocation failure still captures the hooks for actual reclaim. */
    int (*wants_notification)(void *, uint64_t available_pages);
    void (*wait)(void *);
    void (*snapshot)(void *, struct kernel_memory_statistics *);
    uint32_t *(*depth)(void);
    void (*retain)(void *);
    void (*release)(void *);
    void *context;
};

/* Optional native huge-pool custody transfer. retain_last runs only for a
 * validated high-order head's final reference, under allocator->lock. It must
 * not allocate, sleep, call allocator APIs or acquire another lock. Return one
 * to take that existing reference (count remains one, block stays allocated),
 * or zero to return an unmanaged block to buddy. No tail storage is touched.
 * The owner keeps context alive until every managed block/pending preparation
 * is removed and clear succeeds. Its ledger uses this same allocator lock. */
struct physical_page_compound_recycler {
    void *context;
    int (*retain_last)(void *, uint64_t head, uint32_t order);
};

/* Byte capacities; references and mappings never multiply owned storage. */
struct kernel_memory_statistics {
    uint64_t total, free, available, cached, shared, buffers;
    uint64_t dirty, writeback, reclaimable;
};
struct physical_page_allocator;
void kernel_memory_snapshot(const struct physical_page_allocator *allocator,
                            struct kernel_memory_statistics *out);

struct physical_page_metadata;
struct physical_page_root;

struct physical_page_range {
    uint64_t base;
    uint64_t next;
    uint64_t end;
    uint32_t first_page_index;
};

struct physical_page_allocator {
    struct kernel_raw_lock lock;
    struct physical_page_reclaim_hooks reclaim_hooks;
    struct physical_page_compound_recycler compound_recycler;
    /* Optional anonymous-page reclaim, installed when MADV_FREE first has work. */
    uint64_t (*lazy_reclaim)(struct physical_page_allocator *, uint64_t, uint64_t *);
    uint64_t shared_anon_pages;
    uint64_t (*buffer_bytes)(void *);
    void *buffer_context;
    uint64_t total_pages;
    uint64_t available_pages;
#if BOAROS_COST_DIAGNOSTICS
    uint64_t allocated_peak_pages; /* Allocator lifetime maximum, not a per-window peak. */
#endif
    uint64_t recycled_head;
    /* Unpublished order-zero backing. Each nonzero slot owns one reference. */
    uint64_t pipe_spares[2];
#if BOAROS_MAX_CPUS == 1
    uint64_t fault_spares[PHYSICAL_PAGE_FAULT_BATCH];
    unsigned fault_spare_count;
#endif
    uint64_t metadata_address;
    uint64_t metadata_pages;
    uint32_t range_count;
    uint32_t initialized;
    uint32_t finalized;
    uint32_t reclaiming; /* Active callbacks, including suspended tasks. */
    physical_page_access_fn access;
    struct physical_page_metadata *metadata;
    struct physical_page_root *roots;
    unsigned char *tree;
    uint64_t tree_nodes;
    uint32_t root_count;
    uint32_t free_heads[PHYSICAL_PAGE_MAX_ORDER + 1U];
    struct physical_page_range ranges[BOOT_MEMORY_MAX_USABLE_RANGES];
};

enum physical_page_status physical_page_allocator_init(
    struct physical_page_allocator *allocator,
    const struct boot_memory_layout *layout);

/* Binding is one-shot; before it, only never-released pages may be allocated. */
enum physical_page_status physical_page_allocator_bind_access(
    struct physical_page_allocator *allocator,
    physical_page_access_fn access);

enum physical_page_status physical_page_allocator_finalize(
    struct physical_page_allocator *allocator);

/* Caller owns an unpublished or exclusive instance; never audit a live pool.
 * No allocation, reclaim or I/O. Corrupt authority is fatal, not retryable. */
enum physical_page_status physical_page_allocator_audit(
    const struct physical_page_allocator *allocator);

int physical_page_allocator_is_finalized(
    const struct physical_page_allocator *allocator);

/* Page-cache transitions are serialized by the entry's cache protection.
 * Its order-zero page reference excludes free/reuse throughout publication. */
void physical_page_publish_cache_state(struct physical_page_allocator *,
    uint64_t address, int dirty, int writeback);
unsigned physical_page_flags_snapshot(struct physical_page_allocator *,
    uint64_t first_frame, uint64_t *output, unsigned count);

enum physical_page_status physical_page_allocator_set_reclaimer(
    struct physical_page_allocator *allocator,
    const struct physical_page_reclaim_hooks *hooks);

enum physical_page_status physical_page_allocator_clear_reclaimer(
    struct physical_page_allocator *allocator);

enum physical_page_status physical_page_allocator_set_compound_recycler(
    struct physical_page_allocator *, const struct physical_page_compound_recycler *);
/* Caller first removes all managed blocks under the shared allocator lock.
 * A mismatched owner cannot clear another pool's recycler. */
enum physical_page_status physical_page_allocator_clear_compound_recycler(
    struct physical_page_allocator *, void *expected_context);

enum physical_page_status physical_page_allocate(
    struct physical_page_allocator *allocator,
    uint64_t *address);

enum physical_page_status physical_page_allocate_order(
    struct physical_page_allocator *allocator,
    uint32_t order,
    uint64_t *address);

/* One buddy attempt; no reclaim hooks, I/O or wait. Valid in notification
 * context after releasing source locks. Failure leaves address unchanged. */
enum physical_page_status physical_page_allocate_order_nosleep(
    struct physical_page_allocator *, uint32_t order, uint64_t *address);

/* Allocate 2^order contiguous pages, each with its own order-zero reference.
 * mmap backing can share/release each page independently. No owners escape on
 * failure; success must be released page-by-page, not with release_order. */
enum physical_page_status physical_page_allocate_contiguous(
    struct physical_page_allocator *allocator, uint32_t order, uint64_t *address);

enum physical_page_status physical_page_release(
    struct physical_page_allocator *allocator,
    uint64_t address);

/* Only a fully consumed, exclusively owned pipe page may enter this pool.
 * A hit transfers that same owner. Buddy exhaustion and pipe close drain it. */
enum physical_page_status physical_page_pipe_allocate(
    struct physical_page_allocator *, uint64_t *address);
void physical_page_pipe_release(struct physical_page_allocator *, uint64_t address);
void physical_page_pipe_drain(struct physical_page_allocator *);

/* Each array item transfers one order-zero reference, including duplicate addresses. */
#define PHYSICAL_PAGE_REFERENCE_BATCH_MAX 16U
/* Try once for up to sixteen independent order-zero owners. Return the
 * completed prefix; publish pressure once outside the allocator protection.
 * A short prefix lets the caller enter its ordinary reclaim/wait path. */
#define PHYSICAL_PAGE_ALLOCATION_BATCH_MAX 16U
unsigned physical_page_allocate_batch(struct physical_page_allocator *,
    uint64_t *addresses, unsigned count);

/* Try to supply one base page without running reclaim or waiting. The
 * single-core allocator can refill a bounded reserve in one buddy operation. */
int physical_page_try_fault(struct physical_page_allocator *, uint64_t *address);
void physical_page_drain_faults(struct physical_page_allocator *);
void physical_page_release_batch(struct physical_page_allocator *allocator,
    const uint64_t *addresses, unsigned count);

/* Mixed-order finalized owners, one allocator critical section. Every address
 * is an owned head and each byte-sized order must exactly match its allocation.
 * Repeated heads consume separate owned references, as with release_batch. */
void physical_page_release_order_batch(struct physical_page_allocator *,
    const uint64_t *addresses, const uint8_t *orders, unsigned count);

void physical_page_acquire_batch(struct physical_page_allocator *allocator,
    const uint64_t *addresses, unsigned count);

/* Finalized order-zero references retain their page-only contract. */
/* Caller owns the page through a stable PTE. Return its pre-borrow count;
 * if shared, acquire the copy-source loan under that same protection. */
uint32_t physical_page_cow_borrow(struct physical_page_allocator *,uint64_t address);

enum physical_page_status physical_page_acquire(
    struct physical_page_allocator *allocator,
    uint64_t address);

/* Whole-allocation references for compound backing. address must be the live
 * buddy head and order must match its allocation. A tail is never a separate
 * owner. Each acquired or borrowed reference is returned with release_order
 * using the same head/order; release_allocation may discover that order.
 * Caller must already hold a stable reference, or hold the mapping lock
 * protecting a reference, throughout acquire/borrow. An address/order pair
 * alone is not an ownership token: allocator locking cannot prevent ABA when
 * a stale address has been freed and reused for another allocation.
 * No page-table mapping is installed, and no allocation is split here. */
enum physical_page_status physical_page_acquire_order(
    struct physical_page_allocator *, uint64_t address, uint32_t order);
/* Like cow_borrow, but the retained source is the entire compound allocation.
 * A return of one does not acquire a second reference. The caller must keep
 * its original owner and serialize changes to the associated mapping. */
uint32_t physical_page_cow_borrow_order(
    struct physical_page_allocator *, uint64_t address, uint32_t order);

enum physical_page_status physical_page_reference_count(
    const struct physical_page_allocator *allocator,
    uint64_t address,
    uint32_t *references);

enum physical_page_status physical_page_release_order(
    struct physical_page_allocator *allocator,
    uint64_t address,
    uint32_t order);

/* Return the actual order while releasing an owned buddy head. Corrupt or
 * non-head input is fatal, as with release_order; discovery and release share
 * one allocator critical section and the same complete buddy validation. */
uint32_t physical_page_release_allocation(struct physical_page_allocator *, uint64_t address);

/* Return the order stored on an allocated buddy head. */
enum physical_page_status physical_page_allocation_order(
    const struct physical_page_allocator *allocator,
    uint64_t address,
    uint32_t *order);

/* The caller owns a live head reference with stable allocation metadata. */
int physical_page_owned_order(const struct physical_page_allocator *,
    uint64_t address, uint32_t order);

/* The caller must own an allocated page; this validates only address history. */
/* 内部持有者访问：调用者须已持有覆盖此次使用的页、页表或 pin 引用。
 * 不查询 buddy 所有权，也不取得分配器锁。 */
enum physical_page_status physical_page_map_owned(
    const struct physical_page_allocator *, uint64_t physical_address,
    void **pointer);

enum physical_page_status physical_page_resolve(
    const struct physical_page_allocator *allocator,
    uint64_t physical_address,
    void **pointer);

uint64_t physical_page_total(
    const struct physical_page_allocator *allocator);

uint64_t physical_page_available(
    const struct physical_page_allocator *allocator);

uint64_t physical_page_metadata_pages(
    const struct physical_page_allocator *allocator);

#endif
