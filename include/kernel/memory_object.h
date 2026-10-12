#ifndef BOAROS_KERNEL_MEMORY_OBJECT_H
#define BOAROS_KERNEL_MEMORY_OBJECT_H

#include <stdint.h>

struct kernel_heap;
struct physical_page_allocator;
struct kernel_memory_object;
struct kernel_memory_budget { uint64_t limit, used; };
/* limit/budget绑定仅在私有构造期设置；used由对象原子收费，读取用atomic load。 */
void kernel_memory_object_set_budget(struct kernel_memory_object *, struct kernel_memory_budget *);

enum kernel_memory_object_status {
    KERNEL_MEMORY_OBJECT_OK = 0,
    KERNEL_MEMORY_OBJECT_NO_MEMORY,
    KERNEL_MEMORY_OBJECT_NOT_FOUND,
    KERNEL_MEMORY_OBJECT_NO_SPACE,
    KERNEL_MEMORY_OBJECT_STATE,
};

/* An object owns one reference per resident page. Each mapped PTE owns one
 * additional reference returned by get_page. References to the object are
 * held by MM backing registries, independently of VMA array movement.
 * Every operation needs an actual live object owner; acquire cannot stabilize
 * an unprotected raw pointer. Last release destroys only after all such owners
 * end. Page index/publication keeps the rank35 mutex; no MM lock or I/O callback
 * is entered from it. Returned get/find references belong to the caller. */
enum kernel_memory_object_status kernel_memory_object_check_domain(
    const struct kernel_memory_object *, const struct kernel_heap *,
    const struct physical_page_allocator *);
enum kernel_memory_object_status kernel_memory_object_create(
    struct kernel_heap *heap, struct physical_page_allocator *allocator,
    struct kernel_memory_object **object);
enum kernel_memory_object_status kernel_memory_object_acquire(
    struct kernel_memory_object *object);
void kernel_memory_object_release(struct kernel_memory_object **object);
enum kernel_memory_object_status kernel_memory_object_get_page(
    struct kernel_memory_object *object, uint64_t page_index,
    uint64_t *physical_address, int *created);
/* Concurrent MM preparation returns a page reference plus one pending claim.
 * The caller's actual object owner covers both until exactly one finish:
 * accepted transfers the page reference to the PTE; rejected consumes it.
 * Finish runs outside MM raw protection (the MM gate may remain held).
 * All rejected claims remove an unadopted page/charge; a prior acceptance is
 * permanent until truncate or object destruction, even after that PTE ends.
 * Direct get/find consumers accept existing prepared content immediately. */
enum kernel_memory_object_status kernel_memory_object_prepare_page(
    struct kernel_memory_object *, uint64_t index, uint64_t *address, int *created);
void kernel_memory_object_finish_page(struct kernel_memory_object *,
    uint64_t index, uint64_t address, int accepted);
/* Caller already released its failed publication reference. Discard only the
 * matching page with no PTE/other pin owners; an adopted page is preserved. */
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
