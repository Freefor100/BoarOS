#ifndef BOAROS_KERNEL_MEMORY_OBJECT_H
#define BOAROS_KERNEL_MEMORY_OBJECT_H

#include <stdint.h>

struct kernel_heap;
struct physical_page_allocator;
struct kernel_memory_object;

enum kernel_memory_object_status {
    KERNEL_MEMORY_OBJECT_OK = 0,
    KERNEL_MEMORY_OBJECT_NO_MEMORY,
    KERNEL_MEMORY_OBJECT_NOT_FOUND,
    KERNEL_MEMORY_OBJECT_STATE,
};

/* An object owns one reference per resident page. Each mapped PTE owns one
 * additional reference returned by get_page. References to the object are
 * held by MM backing registries, independently of VMA array movement. */
enum kernel_memory_object_status kernel_memory_object_create(
    struct kernel_heap *heap, struct physical_page_allocator *allocator,
    struct kernel_memory_object **object);
enum kernel_memory_object_status kernel_memory_object_acquire(
    struct kernel_memory_object *object);
void kernel_memory_object_release(struct kernel_memory_object **object);
enum kernel_memory_object_status kernel_memory_object_get_page(
    struct kernel_memory_object *object, uint64_t page_index,
    uint64_t *physical_address, int *created);
/* Caller already released the failed PTE's page reference. */
void kernel_memory_object_discard_new_page(
    struct kernel_memory_object *object, uint64_t page_index,
    uint64_t physical_address);

/* The caller serializes mutations and invalidates affected PTEs before truncate.
 * A successful lookup owns one page reference; a hole never allocates. */
enum kernel_memory_object_status kernel_memory_object_find_page(
    struct kernel_memory_object *object, uint64_t page_index, uint64_t *physical_address);
void kernel_memory_object_truncate(struct kernel_memory_object *object, uint64_t size);
uint64_t kernel_memory_object_resident_pages(const struct kernel_memory_object *object);

#endif
