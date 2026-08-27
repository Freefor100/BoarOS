#ifndef BOAROS_KERNEL_HEAP_H
#define BOAROS_KERNEL_HEAP_H

#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_HEAP_ALIGNMENT 16U
#define KERNEL_HEAP_MIN_CLASS_SHIFT 4U
#define KERNEL_HEAP_SIZE_CLASS_COUNT \
    (BOAROS_PAGE_SHIFT - KERNEL_HEAP_MIN_CLASS_SHIFT)

enum kernel_heap_status {
    KERNEL_HEAP_STATUS_OK = 0,
    KERNEL_HEAP_STATUS_INVALID,
    KERNEL_HEAP_STATUS_EMPTY,
    KERNEL_HEAP_STATUS_OVERFLOW,
    KERNEL_HEAP_STATUS_DOUBLE_FREE,
    KERNEL_HEAP_STATUS_STATE,
};

/* Return nonzero only when pointer belongs to the heap's direct-map domain. */
typedef int (*kernel_heap_physical_address_fn)(
    const void *pointer,
    uint64_t *physical_address);

struct kernel_heap_statistics {
    uint64_t allocation_calls;
    uint64_t allocation_failures;
    uint64_t live_allocations;
    uint64_t current_pages;
    uint64_t peak_pages;
};

struct kernel_heap {
    struct physical_page_allocator *page_allocator;
    kernel_heap_physical_address_fn physical_address;
    void *partial_slabs[KERNEL_HEAP_SIZE_CLASS_COUNT];
    struct kernel_heap_statistics statistics;
    uint32_t initialized;
};

enum kernel_heap_status kernel_heap_init(
    struct kernel_heap *heap,
    struct physical_page_allocator *page_allocator,
    kernel_heap_physical_address_fn physical_address);

enum kernel_heap_status kernel_heap_allocate(
    struct kernel_heap *heap,
    size_t size,
    void **pointer);

enum kernel_heap_status kernel_heap_allocate_zeroed(
    struct kernel_heap *heap,
    size_t count,
    size_t size,
    void **pointer);

/* On failure, both the old allocation and output value remain unchanged. */
enum kernel_heap_status kernel_heap_resize(
    struct kernel_heap *heap,
    void *old_pointer,
    size_t new_size,
    void **new_pointer);

enum kernel_heap_status kernel_heap_release(
    struct kernel_heap *heap,
    void *pointer);

void kernel_heap_get_statistics(
    const struct kernel_heap *heap,
    struct kernel_heap_statistics *statistics);

#endif
