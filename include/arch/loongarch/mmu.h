#ifndef BOAROS_ARCH_LOONGARCH_MMU_H
#define BOAROS_ARCH_LOONGARCH_MMU_H
#include <kernel/physical_page.h>
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
    uint32_t table_pages;
    uint32_t leaf_pages;
    uint32_t protected_pages;
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
 * is the active address space before relying on the local TLB flush.  A
 * release is fail-stop. All owners are released before this call returns.
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
enum arch_mmu_status arch_mmu_user_protect_owned_page(
    struct arch_mmu_user_space *space, uint64_t address, uint32_t permissions);

enum arch_mmu_status arch_mmu_user_protect_owned_range(
    struct arch_mmu_user_space *space,
    uint64_t start,
    uint64_t end,
    uint32_t permissions);



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
 * Build child tables that share owned user pages. shared_leaf identifies
 * pages whose writes must remain shared; all other pages use copy-on-write.
 * The destination borrows the same kernel root entries as source. Parent
 * private leaves are committed to COW only after child construction;
 * after initialization, failure may leave destination as a LIVE owner for
 * the caller to destroy.
 */
enum arch_mmu_status arch_mmu_user_space_fork(
    struct arch_mmu_user_space *destination,
    struct arch_mmu_user_space *source,
    int (*shared_leaf)(void *context, uint64_t virtual_address),
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
int arch_mmu_kernel_window_active(void);
enum arch_mmu_status arch_mmu_kernel_window_map(struct physical_page_allocator *, uint64_t, uint64_t);
enum arch_mmu_status arch_mmu_kernel_window_unmap(struct physical_page_allocator *, uint64_t);
void la_mmu_initialize(void);
#endif
