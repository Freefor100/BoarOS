#ifndef BOAROS_KERNEL_PHYSICAL_PAGE_H
#define BOAROS_KERNEL_PHYSICAL_PAGE_H

#include <kernel/boot_memory.h>

#include <stdint.h>

#define PHYSICAL_PAGE_MAX_ORDER 31U

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

struct physical_page_metadata;

struct physical_page_range {
    uint64_t base;
    uint64_t next;
    uint64_t end;
    uint32_t first_page_index;
};

struct physical_page_allocator {
    uint64_t total_pages;
    uint64_t available_pages;
    uint64_t recycled_head;
    uint64_t metadata_address;
    uint64_t metadata_pages;
    uint32_t range_count;
    uint32_t initialized;
    uint32_t finalized;
    uint32_t reclaiming;
    physical_page_access_fn access;
    physical_page_reclaim_fn reclaimer;
    void *reclaimer_context;
    struct physical_page_metadata *metadata;
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

int physical_page_allocator_is_finalized(
    const struct physical_page_allocator *allocator);

enum physical_page_status physical_page_allocator_set_reclaimer(
    struct physical_page_allocator *allocator,
    physical_page_reclaim_fn reclaimer,
    void *context);

enum physical_page_status physical_page_allocator_clear_reclaimer(
    struct physical_page_allocator *allocator);

enum physical_page_status physical_page_allocate(
    struct physical_page_allocator *allocator,
    uint64_t *address);

enum physical_page_status physical_page_allocate_order(
    struct physical_page_allocator *allocator,
    uint32_t order,
    uint64_t *address);

enum physical_page_status physical_page_release(
    struct physical_page_allocator *allocator,
    uint64_t address);

/* Only finalized order-zero allocations may gain shared owners. */
enum physical_page_status physical_page_acquire(
    struct physical_page_allocator *allocator,
    uint64_t address);

enum physical_page_status physical_page_reference_count(
    const struct physical_page_allocator *allocator,
    uint64_t address,
    uint32_t *references);

enum physical_page_status physical_page_release_order(
    struct physical_page_allocator *allocator,
    uint64_t address,
    uint32_t order);

/* Return the order stored on an allocated buddy head. */
enum physical_page_status physical_page_allocation_order(
    const struct physical_page_allocator *allocator,
    uint64_t address,
    uint32_t *order);

/* The caller must own an allocated page; this validates only address history. */
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
