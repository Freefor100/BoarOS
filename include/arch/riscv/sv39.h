#ifndef BOAROS_ARCH_RISCV_SV39_H
#define BOAROS_ARCH_RISCV_SV39_H

#include <kernel/physical_page.h>

#include <stddef.h>
#include <stdint.h>

#define RISCV_SV39_PAGE_SIZE_4K UINT64_C(0x1000)
#define RISCV_SV39_PAGE_SIZE_2M UINT64_C(0x200000)
#define RISCV_SV39_USER_LIMIT (UINT64_C(1) << 38U)

#define RISCV_SV39_READ (UINT32_C(1) << 0U)
#define RISCV_SV39_WRITE (UINT32_C(1) << 1U)
#define RISCV_SV39_EXECUTE (UINT32_C(1) << 2U)
#define RISCV_SV39_USER (UINT32_C(1) << 3U)

enum riscv_sv39_status {
    RISCV_SV39_STATUS_OK = 0,
    RISCV_SV39_STATUS_INVALID,
    RISCV_SV39_STATUS_NO_MEMORY,
    RISCV_SV39_STATUS_CONFLICT,
    RISCV_SV39_STATUS_STATE,
    RISCV_SV39_STATUS_NOT_MAPPED,
    RISCV_SV39_STATUS_CLEANUP_REQUIRED,
};

enum riscv_sv39_page_table_state {
    RISCV_SV39_STATE_UNINITIALIZED = 0,
    RISCV_SV39_STATE_BUILDING,
    RISCV_SV39_STATE_FAILED,
    RISCV_SV39_STATE_ACTIVE,
};

struct riscv_sv39_page_table {
    struct physical_page_allocator *allocator;
    uint64_t root_address;
    uint64_t table_pages;
    uint64_t leaf_4k;
    uint64_t leaf_2m;
    enum riscv_sv39_page_table_state state;
};

enum riscv_sv39_user_space_state {
    RISCV_SV39_USER_SPACE_EMPTY = 0,
    RISCV_SV39_USER_SPACE_LIVE,
    RISCV_SV39_USER_SPACE_MOVED,
    RISCV_SV39_USER_SPACE_DESTROYED,
    RISCV_SV39_USER_SPACE_CLEANUP,
};

struct riscv_sv39_user_space {
    struct physical_page_allocator *allocator;
    uint64_t root_address;
    uint32_t table_pages;
    uint32_t leaf_pages;
    uint32_t protected_pages;
    uint32_t cow_pages;
    uint32_t retired_pages;
    uint64_t cleanup_page_address;
    uint32_t cleanup_page_owned;
    uint64_t cow_copies;
    uint64_t cow_in_place;
    enum riscv_sv39_user_space_state state;
};

struct riscv_sv39_mapping {
    uint64_t physical_address;
    uint32_t permissions;
};

/* The table object must be zero-initialized before its first initialization. */
enum riscv_sv39_status riscv_sv39_page_table_init(
    struct riscv_sv39_page_table *table,
    struct physical_page_allocator *allocator);

/*
 * Build mappings before activation, while every page-table physical address is
 * directly accessible.  Invalid inputs leave a BUILDING table reusable.  An
 * allocation or mapping conflict changes it to FAILED and may leave earlier
 * leaves and intermediate tables installed.
 */
enum riscv_sv39_status riscv_sv39_map_range(
    struct riscv_sv39_page_table *table,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t size,
    uint32_t permissions);

/* Both address spaces must keep the current execution context and table writable. */
enum riscv_sv39_status riscv_sv39_activate(
    struct riscv_sv39_page_table *table);

uint64_t riscv_sv39_current_satp(void);

/* Only Bare or Sv39 with ASID 0 is accepted. */
enum riscv_sv39_status riscv_sv39_switch_satp(uint64_t satp);

/* The user-space object and kernel table must use the same allocator. */
enum riscv_sv39_status riscv_sv39_user_space_init(
    struct riscv_sv39_user_space *space,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table);

/* Success transfers ownership of physical_address to space. */
enum riscv_sv39_status riscv_sv39_user_map_owned_page(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint32_t permissions);

/* Success transfers one already-acquired shared page reference to space. */
enum riscv_sv39_status riscv_sv39_user_map_cow_page(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint32_t permissions);

/*
 * Success allocates, zeroes, maps, and transfers one page to space.
 * CLEANUP_REQUIRED leaves a retryable CLEANUP owner; only move/destroy it.
 */
enum riscv_sv39_status riscv_sv39_user_map_zeroed_page(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    uint32_t permissions);

enum riscv_sv39_status riscv_sv39_user_lookup(
    const struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    struct riscv_sv39_mapping *mapping);

/*
 * Removes owned leaves in [start, end).  The caller must establish that this
 * is the active address space before relying on the local TLB flush.  A
 * release failure leaves an invalid, software-owned retired PTE and is
 * reported through deferred_pages while the address remains inaccessible.
 */
enum riscv_sv39_status riscv_sv39_user_unmap_owned_range(
    struct riscv_sv39_user_space *space,
    uint64_t start,
    uint64_t end,
    uint32_t *deferred_pages);

/*
 * Changes access for owned leaves in [start, end).  permissions == 0 keeps
 * each physical page owned behind an invalid software PTE (PROT_NONE).
 * Missing and retired leaves are unchanged; present leaves retain content.
 */
enum riscv_sv39_status riscv_sv39_user_protect_owned_range(
    struct riscv_sv39_user_space *space,
    uint64_t start,
    uint64_t end,
    uint32_t permissions);

/* Retries retired-page release without changing any active mapping. */
enum riscv_sv39_status riscv_sv39_user_reclaim_retired_range(
    struct riscv_sv39_user_space *space,
    uint64_t start,
    uint64_t end,
    uint32_t *deferred_pages);

/* Populate mapped owned pages before this address space becomes active. */
enum riscv_sv39_status riscv_sv39_user_space_populate(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    const void *bytes,
    size_t size);

enum riscv_sv39_status riscv_sv39_user_space_satp(
    const struct riscv_sv39_user_space *space,
    uint64_t *satp);

/*
 * Build child tables that share every owned user page with source through
 * copy-on-write.  The destination borrows the same kernel root entries as
 * source.  Parent leaves are committed to COW only after child construction;
 * after initialization, failure may leave destination as a LIVE or CLEANUP
 * owner for the caller to destroy.
 */
enum riscv_sv39_status riscv_sv39_user_space_fork(
    struct riscv_sv39_user_space *destination,
    struct riscv_sv39_user_space *source);

/* Resolves only a present COW leaf; other mappings return NOT_MAPPED. */
enum riscv_sv39_status riscv_sv39_user_resolve_cow(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    uint32_t permissions);

/* Success consumes a LIVE or CLEANUP source; failure changes neither object. */
enum riscv_sv39_status riscv_sv39_user_space_move(
    struct riscv_sv39_user_space *destination,
    struct riscv_sv39_user_space *source);

/* The active satp root cannot be destroyed; CLEANUP is retryable. */
enum riscv_sv39_status riscv_sv39_user_space_destroy(
    struct riscv_sv39_user_space *space);

#endif
