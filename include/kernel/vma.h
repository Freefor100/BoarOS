#ifndef BOAROS_KERNEL_VMA_H
#define BOAROS_KERNEL_VMA_H

#include <stdint.h>

struct kernel_heap;
struct kernel_vma_set;

enum kernel_vma_status {
    KERNEL_VMA_STATUS_OK = 0,
    KERNEL_VMA_STATUS_INVALID_ARGUMENT,
    KERNEL_VMA_STATUS_NO_MEMORY,
    KERNEL_VMA_STATUS_CONFLICT,
    KERNEL_VMA_STATUS_NOT_FOUND,
    KERNEL_VMA_STATUS_CLEANUP_REQUIRED,
    KERNEL_VMA_STATUS_STATE,
};

enum kernel_vma_kind {
    KERNEL_VMA_KIND_ANONYMOUS = 0,
    KERNEL_VMA_KIND_FILE_PRIVATE,
};

enum kernel_vma_role {
    KERNEL_VMA_ROLE_NONE = 0,
    KERNEL_VMA_ROLE_ELF,
    KERNEL_VMA_ROLE_STACK,
    KERNEL_VMA_ROLE_HEAP,
    KERNEL_VMA_ROLE_MMAP,
};

enum kernel_vma_fault_policy {
    KERNEL_VMA_FAULT_RESIDENT_REQUIRED = 0,
    KERNEL_VMA_FAULT_DEMAND_ZERO,
};

/* A value copy; callers must not retain backing as a standalone owner. */
struct kernel_vma {
    uint64_t start;
    uint64_t end;
    uint64_t file_offset;
    uint32_t permissions;
    enum kernel_vma_kind kind;
    enum kernel_vma_role role;
    enum kernel_vma_fault_policy fault_policy;
    void *backing;
};

/*
 * These helpers are private to architecture MM backends.  A set borrows heap;
 * its owner must keep heap live through set destruction and every retry.
 */
enum kernel_vma_status kernel_vma_set_create(
    struct kernel_heap *heap,
    struct kernel_vma_set **set);

/* On cleanup-required failure, destination remains an owned retryable set. */
enum kernel_vma_status kernel_vma_set_clone(
    const struct kernel_vma_set *source,
    struct kernel_vma_set **destination);

/* Success clears *set; cleanup-required leaves it retryable. */
enum kernel_vma_status kernel_vma_set_destroy(struct kernel_vma_set **set);

enum kernel_vma_status kernel_vma_set_insert(
    struct kernel_vma_set *set,
    const struct kernel_vma *vma);

enum kernel_vma_status kernel_vma_set_lookup(
    const struct kernel_vma_set *set,
    uint64_t virtual_address,
    struct kernel_vma *vma);

#endif
