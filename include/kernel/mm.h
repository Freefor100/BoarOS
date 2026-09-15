#ifndef BOAROS_KERNEL_MM_H
#define BOAROS_KERNEL_MM_H

#include <kernel/physical_page.h>
#include <kernel/vma.h>

#include <stdint.h>

struct kernel_open_file_description;
struct kernel_elf64_source;

#define KERNEL_MM_READ (UINT32_C(1) << 0U)
#define KERNEL_MM_WRITE (UINT32_C(1) << 1U)
#define KERNEL_MM_EXECUTE (UINT32_C(1) << 2U)
#define KERNEL_MM_USER (UINT32_C(1) << 3U)

/* Architecture-neutral placement policy used by anonymous mmap. */
#define KERNEL_MM_MAP_FIXED (UINT32_C(1) << 0U)
#define KERNEL_MM_MAP_FIXED_NOREPLACE (UINT32_C(1) << 1U)

enum kernel_mm_status {
    KERNEL_MM_STATUS_OK = 0,
    KERNEL_MM_STATUS_INVALID_ARGUMENT,
    KERNEL_MM_STATUS_NO_MEMORY,
    KERNEL_MM_STATUS_NOT_MAPPED,
    KERNEL_MM_STATUS_PAGE_ACCESS,
    KERNEL_MM_STATUS_ADDRESS_SPACE,
    KERNEL_MM_STATUS_CLEANUP_REQUIRED,
    KERNEL_MM_STATUS_STATE,
    KERNEL_MM_STATUS_CONFLICT,
    KERNEL_MM_STATUS_BUS_FAULT,
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
    KERNEL_MM_CLEANUP_FILE_SOURCES,
    KERNEL_MM_CLEANUP_ELF_SOURCES,
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

/* Success creates an independent address space sharing private pages by COW. */
enum kernel_mm_status kernel_mm_fork(
    struct kernel_mm *destination,
    struct kernel_mm *source);

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

/* Maps normalized PT_LOAD runs and keeps one source reference in this MM. */
enum kernel_mm_status kernel_mm_map_elf_source(
    struct kernel_mm *mm,
    struct kernel_elf64_source *source,
    uint64_t load_bias);

/* Returns NOT_MAPPED for a valid address outside all VMAs. */
enum kernel_mm_status kernel_mm_vma_lookup(
    const struct kernel_mm *mm,
    uint64_t virtual_address,
    struct kernel_vma *vma);

/* Initializes the exact Linux program break for a newly loaded image. */
enum kernel_mm_status kernel_mm_brk_initialize(
    struct kernel_mm *mm,
    uint64_t start,
    uint64_t limit);

/* Non-fixed mmap uses this per-MM top-down ceiling. */
enum kernel_mm_status kernel_mm_mmap_base_initialize(
    struct kernel_mm *mm,
    uint64_t base);

enum kernel_mm_status kernel_mm_vdso_set_address(
    struct kernel_mm *mm,
    uint64_t address);

enum kernel_mm_status kernel_mm_vdso_address(
    const struct kernel_mm *mm,
    uint64_t *address);

/* Implements the raw Linux brk syscall result: success or rejection address. */
enum kernel_mm_status kernel_mm_brk(
    struct kernel_mm *mm,
    uint64_t requested,
    uint64_t *result);

/* Anonymous private demand-zero mappings; length is rounded up to pages. */
enum kernel_mm_status kernel_mm_mmap_anonymous(
    struct kernel_mm *mm,
    uint64_t hint,
    uint64_t length,
    uint32_t permissions,
    uint32_t flags,
    uint64_t *address);

/* Side-effect-free validation; success does not reserve the chosen range. */
enum kernel_mm_status kernel_mm_validate_file_private_mapping(
    struct kernel_mm *mm,
    uint64_t hint,
    uint64_t length,
    uint64_t file_offset,
    uint32_t permissions,
    uint32_t flags);

/* Success consumes *file; failure leaves the caller's OFD reference intact. */
enum kernel_mm_status kernel_mm_mmap_file_private(
    struct kernel_mm *mm,
    struct kernel_open_file_description **file,
    uint64_t hint,
    uint64_t length,
    uint64_t file_offset,
    uint32_t permissions,
    uint32_t flags,
    uint64_t *address);

/* Linux range semantics: munmap tolerates holes; mprotect requires coverage. */
enum kernel_mm_status kernel_mm_munmap(
    struct kernel_mm *mm,
    uint64_t address,
    uint64_t length);

enum kernel_mm_status kernel_mm_mprotect(
    struct kernel_mm *mm,
    uint64_t address,
    uint64_t length,
    uint32_t permissions);

/*
 * Resolve one hardware user fault in the MM active on this hart.  access must
 * be exactly one of READ, WRITE, or EXECUTE.  NOT_MAPPED means the access is
 * not resolvable from VMA policy; success makes the new PTE locally visible.
 */
enum kernel_mm_status kernel_mm_resolve_user_fault(
    struct kernel_mm *mm,
    uint64_t virtual_address,
    uint32_t access);

/* Success consumes one reference; last-reference cleanup can retain VFS owners. */
enum kernel_mm_status kernel_mm_release(struct kernel_mm *mm);

#endif
