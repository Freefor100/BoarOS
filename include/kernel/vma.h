#ifndef BOAROS_KERNEL_VMA_H
#define BOAROS_KERNEL_VMA_H

#include <stdint.h>

struct kernel_heap;
struct kernel_vma_set;

enum kernel_vma_edit_kind {
    KERNEL_VMA_EDIT_REMOVE = 0,
    KERNEL_VMA_EDIT_REPLACE,
    KERNEL_VMA_EDIT_PROTECT,
};

enum kernel_vma_status {
    KERNEL_VMA_STATUS_OK = 0,
    KERNEL_VMA_STATUS_INVALID_ARGUMENT,
    KERNEL_VMA_STATUS_NO_MEMORY,
    KERNEL_VMA_STATUS_CONFLICT,
    KERNEL_VMA_STATUS_NOT_FOUND,
    KERNEL_VMA_STATUS_STATE,
};

enum kernel_vma_kind {
    KERNEL_VMA_KIND_ANONYMOUS = 0,
    KERNEL_VMA_KIND_FILE_PRIVATE,
    KERNEL_VMA_KIND_ELF_PRIVATE,
    KERNEL_VMA_KIND_ANON_SHARED,
};

enum kernel_vma_role {
    KERNEL_VMA_ROLE_NONE = 0,
    KERNEL_VMA_ROLE_ELF,
    KERNEL_VMA_ROLE_VDSO,
    KERNEL_VMA_ROLE_STACK,
    KERNEL_VMA_ROLE_HEAP,
    KERNEL_VMA_ROLE_MMAP,
};

enum kernel_vma_fault_policy {
    KERNEL_VMA_FAULT_RESIDENT_REQUIRED = 0,
    KERNEL_VMA_FAULT_DEMAND_ZERO,
    KERNEL_VMA_FAULT_FILE_PRIVATE,
    KERNEL_VMA_FAULT_ELF,
    KERNEL_VMA_FAULT_ANON_SHARED,
};

/* A value copy; callers must not retain backing as a standalone owner. */
struct kernel_vma {
    uint64_t start;
    uint64_t end;
    uint64_t backing_offset;
    uint32_t permissions;
    enum kernel_vma_kind kind;
    enum kernel_vma_role role;
    enum kernel_vma_fault_policy fault_policy;
    void *backing;
};

/*
 * A prepared edit reserves every descriptor it can need without changing the
 * logical VMA set.  Architecture MM code may then commit page-table state and
 * finish the edit without another allocation.
 */
struct kernel_vma_edit {
    const struct kernel_vma_set *set;
    uint64_t generation;
    uint64_t start;
    uint64_t end;
    uint32_t permissions;
    enum kernel_vma_edit_kind kind;
    struct kernel_vma replacement;
};

/*
 * These helpers are private to architecture MM backends.  A set borrows heap;
 * its owner must keep heap live through set destruction.
 */
enum kernel_vma_status kernel_vma_set_create(
    struct kernel_heap *heap,
    struct kernel_vma_set **set);

enum kernel_vma_status kernel_vma_set_clone(
    const struct kernel_vma_set *source,
    struct kernel_vma_set **destination);

/* Success clears *set. */
enum kernel_vma_status kernel_vma_set_destroy(struct kernel_vma_set **set);

enum kernel_vma_status kernel_vma_set_insert(
    struct kernel_vma_set *set,
    const struct kernel_vma *vma);

enum kernel_vma_status kernel_vma_set_lookup(
    const struct kernel_vma_set *set,
    uint64_t virtual_address,
    struct kernel_vma *vma);

enum kernel_vma_status kernel_vma_set_find_topdown_gap(
    const struct kernel_vma_set *set,
    uint64_t hint,
    uint64_t lower,
    uint64_t upper,
    uint64_t length,
    uint64_t *address);

enum kernel_vma_status kernel_vma_set_overlaps(
    const struct kernel_vma_set *set,
    uint64_t start,
    uint64_t end,
    int *overlaps);

enum kernel_vma_status kernel_vma_set_backing_in_use(
    const struct kernel_vma_set *set,
    const void *backing,
    int *in_use);

/* replacement == NULL prepares a Linux munmap-style range removal. */
enum kernel_vma_status kernel_vma_set_prepare_replace(
    struct kernel_vma_set *set,
    uint64_t start,
    uint64_t end,
    const struct kernel_vma *replacement,
    struct kernel_vma_edit *edit);

/* Every byte in the interval must already be covered by VMAs. */
enum kernel_vma_status kernel_vma_set_prepare_protect(
    struct kernel_vma_set *set,
    uint64_t start,
    uint64_t end,
    uint32_t permissions,
    struct kernel_vma_edit *edit);

/* No allocation is performed; a stale or foreign edit returns STATE. */
enum kernel_vma_status kernel_vma_set_commit_edit(
    struct kernel_vma_set *set,
    const struct kernel_vma_edit *edit);

/* Shrinks one exact VMA at its high end; new_end == start removes it. */
enum kernel_vma_status kernel_vma_set_trim_end(
    struct kernel_vma_set *set,
    uint64_t start,
    uint64_t old_end,
    uint64_t new_end);

#endif
