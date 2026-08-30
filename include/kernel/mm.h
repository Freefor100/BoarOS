#ifndef BOAROS_KERNEL_MM_H
#define BOAROS_KERNEL_MM_H

#include <kernel/physical_page.h>
#include <kernel/vma.h>

#include <stdint.h>

#define KERNEL_MM_READ (UINT32_C(1) << 0U)
#define KERNEL_MM_WRITE (UINT32_C(1) << 1U)
#define KERNEL_MM_EXECUTE (UINT32_C(1) << 2U)
#define KERNEL_MM_USER (UINT32_C(1) << 3U)

enum kernel_mm_status {
    KERNEL_MM_STATUS_OK = 0,
    KERNEL_MM_STATUS_INVALID_ARGUMENT,
    KERNEL_MM_STATUS_NO_MEMORY,
    KERNEL_MM_STATUS_NOT_MAPPED,
    KERNEL_MM_STATUS_PAGE_ACCESS,
    KERNEL_MM_STATUS_PAGE_RELEASE,
    KERNEL_MM_STATUS_ADDRESS_SPACE,
    KERNEL_MM_STATUS_CLEANUP_REQUIRED,
    KERNEL_MM_STATUS_STATE,
    KERNEL_MM_STATUS_CONFLICT,
};

enum kernel_mm_state {
    KERNEL_MM_EMPTY = 0,
    KERNEL_MM_LIVE,
    KERNEL_MM_MOVED,
    KERNEL_MM_RELEASED,
    KERNEL_MM_CLEANUP,
};

enum kernel_mm_cleanup_stage {
    KERNEL_MM_CLEANUP_NONE = 0,
    KERNEL_MM_CLEANUP_VMAS,
    KERNEL_MM_CLEANUP_SPACE,
    KERNEL_MM_CLEANUP_RECORD,
};

struct kernel_mm {
    struct physical_page_allocator *allocator;
    uint64_t record_page_address;
    enum kernel_mm_state state;
    enum kernel_mm_cleanup_stage cleanup_stage;
};

struct kernel_mm_mapping {
    uint64_t physical_address;
    uint32_t permissions;
};

/* Success creates another independently owned reference to the same MM. */
enum kernel_mm_status kernel_mm_acquire(
    struct kernel_mm *destination,
    const struct kernel_mm *source);

/* Success creates an independent address space with copied user pages. */
enum kernel_mm_status kernel_mm_fork(
    struct kernel_mm *destination,
    const struct kernel_mm *source);

/* Success consumes a LIVE or CLEANUP source without changing its refcount. */
enum kernel_mm_status kernel_mm_move(
    struct kernel_mm *destination,
    struct kernel_mm *source);

enum kernel_mm_status kernel_mm_lookup(
    const struct kernel_mm *mm,
    uint64_t virtual_address,
    struct kernel_mm_mapping *mapping);

/*
 * VMA metadata is enabled exactly once while this MM has one owner.  heap is
 * borrowed and must outlive the MM, including every cleanup retry.
 */
enum kernel_mm_status kernel_mm_vma_enable(
    struct kernel_mm *mm,
    struct kernel_heap *heap);

enum kernel_mm_status kernel_mm_vma_insert_anon(
    struct kernel_mm *mm,
    uint64_t start,
    uint64_t end,
    uint32_t permissions,
    enum kernel_vma_role role,
    enum kernel_vma_fault_policy fault_policy);

/* Returns NOT_MAPPED for a valid address outside all VMAs. */
enum kernel_mm_status kernel_mm_vma_lookup(
    const struct kernel_mm *mm,
    uint64_t virtual_address,
    struct kernel_vma *vma);

/*
 * Resolve one hardware user fault in the MM active on this hart.  access must
 * be exactly one of READ, WRITE, or EXECUTE.  NOT_MAPPED means the access is
 * not resolvable from VMA policy; success makes the new PTE locally visible.
 */
enum kernel_mm_status kernel_mm_resolve_user_fault(
    struct kernel_mm *mm,
    uint64_t virtual_address,
    uint32_t access);

/* Success consumes one reference; last-reference cleanup is retryable. */
enum kernel_mm_status kernel_mm_release(struct kernel_mm *mm);

#endif
