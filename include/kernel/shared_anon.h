#ifndef BOAROS_KERNEL_SHARED_ANON_H
#define BOAROS_KERNEL_SHARED_ANON_H

#include <stdint.h>

struct kernel_heap;
struct physical_page_allocator;
struct kernel_shared_anon;

enum kernel_shared_anon_status {
    KERNEL_SHARED_ANON_OK = 0,
    KERNEL_SHARED_ANON_NO_MEMORY,
    KERNEL_SHARED_ANON_STATE,
};

/* An object owns one reference per resident page. Each mapped PTE owns one
 * additional reference returned by get_page. References to the object are
 * held by MM backing registries, independently of VMA array movement. */
enum kernel_shared_anon_status kernel_shared_anon_create(
    struct kernel_heap *heap, struct physical_page_allocator *allocator,
    struct kernel_shared_anon **object);
enum kernel_shared_anon_status kernel_shared_anon_acquire(
    struct kernel_shared_anon *object);
void kernel_shared_anon_release(struct kernel_shared_anon **object);
enum kernel_shared_anon_status kernel_shared_anon_get_page(
    struct kernel_shared_anon *object, uint64_t page_index,
    uint64_t *physical_address, int *created);
/* Caller already released the failed PTE's page reference. */
void kernel_shared_anon_discard_new_page(
    struct kernel_shared_anon *object, uint64_t page_index,
    uint64_t physical_address);

#endif
