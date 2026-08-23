#ifndef BOAROS_KERNEL_PHYSICAL_PAGE_H
#define BOAROS_KERNEL_PHYSICAL_PAGE_H

#include <kernel/boot_memory.h>

#include <stdint.h>

enum physical_page_status {
    PHYSICAL_PAGE_STATUS_OK = 0,
    PHYSICAL_PAGE_STATUS_INVALID,
    PHYSICAL_PAGE_STATUS_EMPTY,
    PHYSICAL_PAGE_STATUS_DOUBLE_FREE,
    PHYSICAL_PAGE_STATUS_STATE,
};

typedef void *(*physical_page_access_fn)(uint64_t physical_address);

struct physical_page_range {
    uint64_t base;
    uint64_t next;
    uint64_t end;
};

struct physical_page_allocator {
    uint64_t total_pages;
    uint64_t available_pages;
    uint64_t recycled_head;
    uint32_t range_count;
    uint32_t initialized;
    physical_page_access_fn access;
    struct physical_page_range ranges[BOOT_MEMORY_MAX_USABLE_RANGES];
};

enum physical_page_status physical_page_allocator_init(
    struct physical_page_allocator *allocator,
    const struct boot_memory_layout *layout);

/* Binding is one-shot; before it, only never-released pages may be allocated. */
enum physical_page_status physical_page_allocator_bind_access(
    struct physical_page_allocator *allocator,
    physical_page_access_fn access);

enum physical_page_status physical_page_allocate(
    struct physical_page_allocator *allocator,
    uint64_t *address);

enum physical_page_status physical_page_release(
    struct physical_page_allocator *allocator,
    uint64_t address);

uint64_t physical_page_total(
    const struct physical_page_allocator *allocator);

uint64_t physical_page_available(
    const struct physical_page_allocator *allocator);

#endif
