#ifndef BOAROS_ARCH_LOONGARCH_MMU_H
#define BOAROS_ARCH_LOONGARCH_MMU_H

#define ARCH_MMU_FORK_PRIVATE 0
#define ARCH_MMU_FORK_SHARED 1
#define ARCH_MMU_FORK_SKIP 2
#include <kernel/physical_page.h>
#include <kernel/mm_retire.h>
#include <kernel/mm_prepare.h>
#include <kernel/mm_leaf.h>
#include <kernel/page.h>
#include <stddef.h>
#include <stdint.h>
#define ARCH_MMU_PAGE_SIZE BOAROS_PAGE_SIZE
#define ARCH_MMU_USER_LIMIT (UINT64_C(1) << 47)
#define ARCH_MMU_READ 1U
#define ARCH_MMU_WRITE 2U
#define ARCH_MMU_EXECUTE 4U
#define ARCH_MMU_USER 8U
enum arch_mmu_status {
    ARCH_MMU_STATUS_OK = 0,
    ARCH_MMU_STATUS_INVALID,
    ARCH_MMU_STATUS_NO_MEMORY,
    ARCH_MMU_STATUS_CONFLICT,
    ARCH_MMU_STATUS_STATE,
    ARCH_MMU_STATUS_NOT_MAPPED,
    ARCH_MMU_STATUS_NEED_TABLES,
};

enum arch_mmu_page_table_state {
    ARCH_MMU_STATE_UNINITIALIZED = 0,
    ARCH_MMU_STATE_BUILDING,
    ARCH_MMU_STATE_FAILED,
    ARCH_MMU_STATE_ACTIVE,
};

struct arch_mmu_page_table {
    struct physical_page_allocator *allocator;
    uint64_t root_address;
    uint64_t table_pages;
    uint64_t leaf_pages;
    uint64_t large_leaves;
    enum arch_mmu_page_table_state state;
};

enum arch_mmu_user_space_state {
    ARCH_MMU_USER_SPACE_EMPTY = 0,
    ARCH_MMU_USER_SPACE_LIVE,
    ARCH_MMU_USER_SPACE_MOVED,
    ARCH_MMU_USER_SPACE_DESTROYED,
};

struct arch_mmu_user_space {
    struct physical_page_allocator *allocator;
    uint64_t root_address;
    uint64_t *root_entries; /* Borrowed from the address space's root owner. */
    uint32_t table_pages;
    uint32_t leaf_pages;
    uint32_t protected_pages;
    uint64_t peak_resident_pages;
    uint32_t cow_pages;
    uint64_t cow_copies;
    uint64_t cow_in_place;
    uint64_t protect_visits;
    uint64_t protect_address_flushes;
    uint64_t protect_global_flushes;
    enum arch_mmu_user_space_state state;
};

struct arch_mmu_mapping {
    uint64_t physical_address;
    uint32_t permissions;
};
/* Native PMD leaves are 32 MiB (two 16 MiB EntryLo halves). */
#define ARCH_MMU_HUGE_ORDER 11U
#define ARCH_MMU_HUGE_SIZE (UINT64_C(1) << 25)
/* Return a permitted base-page frame for a bounded copy under the MM
 * metadata lock. The leaf owns the reference throughout the copy. */
int arch_mmu_user_base_page(const struct arch_mmu_user_space *, uint64_t,
    uint32_t access, uint64_t *physical);
/* Snapshot includes PROT_NONE and accepts an interior byte VA. Caller holds
 * MM metadata protection; the result does not itself acquire ownership. */
enum arch_mmu_status arch_mmu_user_leaf_snapshot(
    const struct arch_mmu_user_space *, uint64_t, struct kernel_mm_leaf *);
enum arch_mmu_status arch_mmu_user_leaf_probe(
    const struct arch_mmu_user_space *, uint64_t, struct kernel_mm_leaf *, unsigned *missing_tables);
/* Only a mapped base page supplies a slot. Keep the MM metadata writer
 * continuously held from this lookup through any use of the slot. */
enum arch_mmu_status arch_mmu_user_leaf_probe_slot(
    const struct arch_mmu_user_space *, uint64_t, struct kernel_mm_leaf *, unsigned *missing_tables,
    uint64_t **slot);
enum arch_mmu_status arch_mmu_user_empty_window(const struct arch_mmu_user_space *,
    uint64_t, unsigned, unsigned *);
enum arch_mmu_status arch_mmu_user_leaf_missing_order(
    struct arch_mmu_user_space *, uint64_t, uint32_t, unsigned *);
/* Only orders 0/11. Entire-leaf aligned VA/PA; success consumes one data
 * owner and only the published prepared tables. Failure consumes neither.
 * No allocation/reclaim occurs here; caller publishes TLB changes. */
enum arch_mmu_status arch_mmu_user_map_prepared_order(
    struct arch_mmu_user_space *, uint64_t, uint64_t, uint32_t, int, uint32_t,
    struct kernel_mm_leaf_preparation *);
enum arch_mmu_status arch_mmu_user_cow_commit_order(
    struct arch_mmu_user_space *, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t,
    struct kernel_mm_retirement *);
enum arch_mmu_status arch_mmu_user_detach_range(
    struct arch_mmu_user_space *, uint64_t start, uint64_t end,
    struct kernel_mm_detach_cursor *, struct kernel_mm_retirement *,
    void (*before_detach)(void *, uint64_t, uint64_t), void *);
void arch_mmu_sync_user(uint32_t targets, uint64_t address, uint64_t size,
    int instructions);
void arch_mmu_flush_all(void);
enum arch_mmu_status arch_mmu_user_leaf_missing(
    struct arch_mmu_user_space *, uint64_t, unsigned *);
enum arch_mmu_status arch_mmu_user_prepare_move(
    struct arch_mmu_user_space *, uint64_t, uint32_t, struct kernel_mm_leaf_preparation *);
enum arch_mmu_status arch_mmu_user_move_leaf(
    struct arch_mmu_user_space *, uint64_t from, uint64_t to, uint32_t order);
uint64_t arch_mmu_user_thp_base_entry(uint64_t, const struct kernel_mm_leaf *);
void arch_mmu_user_thp_install_split(struct arch_mmu_user_space *,
    const struct kernel_mm_leaf *, uint64_t table);
enum arch_mmu_status arch_mmu_user_map_prepared_page(
    struct arch_mmu_user_space *, uint64_t, uint64_t, uint32_t, int cow,
    struct kernel_mm_leaf_preparation *);
enum arch_mmu_status arch_mmu_user_cow_snapshot(
    struct arch_mmu_user_space *, uint64_t, uint64_t *, uint64_t *);
/* replacement==KERNEL_MM_NO_PAGE requests last-reference in-place recovery. */
enum arch_mmu_status arch_mmu_user_cow_commit(
    struct arch_mmu_user_space *, uint64_t, uint64_t, uint64_t, uint32_t,
    struct kernel_mm_retirement *);
/* Requires the same metadata writer as the slot lookup and a sole PTE
 * reference observed by physical_page_cow_borrow under that writer. */
enum arch_mmu_status arch_mmu_user_cow_upgrade(
    struct arch_mmu_user_space *, uint64_t *slot, uint64_t cookie, uint32_t permissions);
/* The caller stabilizes the slot and owns the newly allocated base page.
 * Success transfers that reference to the table and retires the old one. */
enum arch_mmu_status arch_mmu_user_cow_replace_prepared(
    struct arch_mmu_user_space *, uint64_t *slot, uint64_t cookie, uint64_t replacement,
    uint32_t permissions, struct kernel_mm_retirement *);
enum arch_mmu_status arch_mmu_user_next_owned(
    struct arch_mmu_user_space *, uint64_t *, uint64_t end, uint64_t *);
enum arch_mmu_status arch_mmu_user_owned_snapshot(
    struct arch_mmu_user_space *, uint64_t, uint64_t *, int *);
/* Fork window starts on an inherited leaf of an unpublished child. Parent
 * metadata is held by the caller. Return the first address beyond the window. */
enum arch_mmu_status arch_mmu_user_fork_commit_window(
    struct arch_mmu_user_space *, const struct arch_mmu_user_space *, uint64_t, uint64_t *,
    int (*)(void *, uint64_t), void *, int *changed);
/* One base-leaf table. Transfer owned base-page references for returned
 * bits only, consuming the packed vector in increasing selected-bit order. */
unsigned arch_mmu_user_map_owned_window(struct arch_mmu_user_space *, uint64_t,
    unsigned, const uint64_t *, uint32_t, int flags);
unsigned arch_mmu_user_map_required_window(struct arch_mmu_user_space *, uint64_t,
    unsigned, const uint64_t *, uint32_t, int flags, unsigned required_slot);
enum arch_mmu_status arch_mmu_user_mark_cow_page(struct arch_mmu_user_space *, uint64_t);
enum arch_mmu_status arch_mmu_user_space_fork_prepare(
    struct arch_mmu_user_space *, struct arch_mmu_user_space *,
    int (*)(void *, uint64_t), void *);

/* The user-space object and kernel table must use the same allocator. */
enum arch_mmu_status arch_mmu_user_space_init(
    struct arch_mmu_user_space *space,
    struct physical_page_allocator *allocator,
    const struct arch_mmu_page_table *kernel_table);

/* Success transfers ownership of physical_address to space. */
enum arch_mmu_status arch_mmu_user_map_owned_page(
    struct arch_mmu_user_space *space,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint32_t permissions);

/* Success transfers one already-acquired shared page reference to space. */
enum arch_mmu_status arch_mmu_user_map_cow_page(
    struct arch_mmu_user_space *space,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint32_t permissions);

/* Releases a detached owned page. Invalid ownership is fail-stop. */
enum arch_mmu_status arch_mmu_user_discard_owned_page(
    struct arch_mmu_user_space *space,
    uint64_t physical_address);

/* Success allocates, zeroes, maps, and transfers one page to space. */
enum arch_mmu_status arch_mmu_user_map_zeroed_page(
    struct arch_mmu_user_space *space,
    uint64_t virtual_address,
    uint32_t permissions);

enum arch_mmu_status arch_mmu_user_lookup(
    const struct arch_mmu_user_space *space,
    uint64_t virtual_address,
    struct arch_mmu_mapping *mapping);

/*
 * Removes owned leaves in [start, end).  The caller must establish that this
 * is the only CPU accessing this address space before relying on local TLB
 * invalidation. Shared live MMs must use detach_range and deferred retirement.
 * Partial large leaves are rejected before mutation. A release is fail-stop;
 * all transferred owners are released before this call returns.
 */
enum arch_mmu_status arch_mmu_user_unmap_owned_range(
    struct arch_mmu_user_space *space,
    uint64_t start,
    uint64_t end);

/*
 * Changes access for owned leaves in [start, end).  permissions == 0 keeps
 * each physical page owned behind an invalid software PTE (PROT_NONE).
 * Missing leaves are unchanged; present leaves retain content.
 */
/* Same ownership/PROT_NONE contract, with a single page walk and local VA flush. */
/* Pure PTE update; caller publishes TLB/instruction changes outside raw. */
enum arch_mmu_status arch_mmu_user_protect_owned_page(
    struct arch_mmu_user_space *space, uint64_t address, uint32_t permissions);

enum arch_mmu_status arch_mmu_user_protect_owned_range(
    struct arch_mmu_user_space *space,
    uint64_t start,
    uint64_t end,
    uint32_t permissions);
enum arch_mmu_status arch_mmu_user_protect_resident_page(
    struct arch_mmu_user_space *, uint64_t, uint32_t);
enum arch_mmu_status arch_mmu_user_protect_batch(struct arch_mmu_user_space *,
    uint64_t *cursor, uint64_t begin, uint64_t end,
    uint32_t (*permissions)(void *, uint64_t), void *context);



/* Populate mapped owned pages before this address space becomes active. */
enum arch_mmu_status arch_mmu_user_space_populate(
    struct arch_mmu_user_space *space,
    uint64_t virtual_address,
    const void *bytes,
    size_t size);

enum arch_mmu_status arch_mmu_user_space_context(
    const struct arch_mmu_user_space *space,
    uint64_t *satp);

/*
 * Build child tables from owned user pages. fork_leaf returns PRIVATE for
 * copy-on-write, SHARED for unchanged sharing, or SKIP to omit the leaf.
 * Skipped and shared parent pages keep their original permissions.
 * The destination borrows the same kernel root entries as source. Parent
 * private leaves are committed to COW only after child construction;
 * after initialization, failure may leave destination as a LIVE owner for
 * the caller to destroy.
 */
enum arch_mmu_status arch_mmu_user_space_fork(
    struct arch_mmu_user_space *destination,
    struct arch_mmu_user_space *source,
    int (*fork_leaf)(void *context, uint64_t virtual_address),
    void *context);

/* Resolves only a present COW leaf; other mappings return NOT_MAPPED. */
enum arch_mmu_status arch_mmu_user_resolve_cow(
    struct arch_mmu_user_space *space,
    uint64_t virtual_address,
    uint32_t permissions);

/* Success consumes a LIVE source; failure changes neither object. */
enum arch_mmu_status arch_mmu_user_space_move(
    struct arch_mmu_user_space *destination,
    struct arch_mmu_user_space *source);

/* The active satp root cannot be destroyed. */
enum arch_mmu_status arch_mmu_user_space_destroy(
    struct arch_mmu_user_space *space);


uint64_t arch_mmu_current_context(void);
enum arch_mmu_status arch_mmu_switch_context(uint64_t);
void arch_mmu_sync_instructions(void);
void arch_mmu_flush_address(uint64_t);
void la_mmu_flush_range(uint64_t address, uint64_t size);
int arch_mmu_kernel_window_active(void);
int arch_mmu_secret_page_supported(void);
enum arch_mmu_status arch_mmu_secret_page_protect(struct physical_page_allocator *, uint64_t);
enum arch_mmu_status arch_mmu_secret_page_restore(struct physical_page_allocator *, uint64_t);
enum arch_mmu_status arch_mmu_kernel_window_map(struct physical_page_allocator *, uint64_t, uint64_t);
enum arch_mmu_status arch_mmu_kernel_window_unmap(struct physical_page_allocator *, uint64_t);
void la_mmu_initialize(void);
enum arch_mmu_status la_mmu_direct_map_initialize(
    struct physical_page_allocator *, const struct boot_memory_layout *);
enum arch_mmu_status la_mmu_kernel_window_initialize(struct physical_page_allocator *);
#endif
