#ifndef BOAROS_ARCH_RISCV_SV39_H
#define BOAROS_ARCH_RISCV_SV39_H

#include <kernel/physical_page.h>
#include <kernel/mm_retire.h>
#include <kernel/mm_prepare.h>

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
};

struct riscv_sv39_user_space {
    struct physical_page_allocator *allocator;
    uint64_t root_address;
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

/*
 * Kernel stack window: reserve the empty subtree while the table is
 * BUILDING so every later user root copy inherits it, then insert and
 * remove 4 KiB leaves at runtime.  The window must stay inside one root
 * index; unmapped pages inside it fault instead of following a fallback.
 * The runtime allocator is passed per call: reserve runs before relocation,
 * so it must not capture pointers.
 */
enum riscv_sv39_status riscv_sv39_kernel_window_reserve(
    struct riscv_sv39_page_table *table,
    uint64_t virtual_address,
    uint64_t size);
int riscv_sv39_kernel_window_active(void);
/* 仅CPU0修改；候选表私有准备，全部叶子及远程失效完成后才返回。 */
enum riscv_sv39_status riscv_sv39_kernel_window_map_pages(
    struct physical_page_allocator *, uint64_t va, uint64_t pa, unsigned pages);
enum riscv_sv39_status riscv_sv39_kernel_window_unmap_pages(
    struct physical_page_allocator *, uint64_t va, unsigned pages);
enum riscv_sv39_status riscv_sv39_kernel_window_map(
    struct physical_page_allocator *allocator,
    uint64_t virtual_address,
    uint64_t physical_address);
enum riscv_sv39_status riscv_sv39_kernel_window_unmap(
    struct physical_page_allocator *allocator,
    uint64_t virtual_address);

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

/* Releases a detached owned page. Invalid ownership is fail-stop. */
enum riscv_sv39_status riscv_sv39_user_discard_owned_page(
    struct riscv_sv39_user_space *space,
    uint64_t physical_address);

/* Success allocates, zeroes, maps, and transfers one page to space. */
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
 * release is fail-stop. All owners are released before this call returns.
 */
enum riscv_sv39_status riscv_sv39_user_unmap_owned_range(
    struct riscv_sv39_user_space *space,
    uint64_t start,
    uint64_t end);
/* MM短保护内只摘PTE/交接owner，无分配、flush、物理释放。 */
enum riscv_sv39_status riscv_sv39_user_detach_range(
    struct riscv_sv39_user_space *, uint64_t start, uint64_t end,
    struct kernel_mm_detach_cursor *, struct kernel_mm_retirement *,
    void (*before_detach)(void *, uint64_t, uint64_t), void *);
enum riscv_sv39_status riscv_sv39_user_leaf_missing(
    struct riscv_sv39_user_space *, uint64_t, unsigned *);
/* Install only empty destination tables; move transfers the exact leaf owner.
 * Both run under MM metadata protection; caller synchronizes before reclaim. */
enum riscv_sv39_status riscv_sv39_user_prepare_move(
    struct riscv_sv39_user_space *, uint64_t, struct kernel_mm_leaf_preparation *);
enum riscv_sv39_status riscv_sv39_user_move_leaf(
    struct riscv_sv39_user_space *, uint64_t from, uint64_t to);
enum riscv_sv39_status riscv_sv39_user_map_prepared_page(
    struct riscv_sv39_user_space *, uint64_t, uint64_t, uint32_t,
    struct kernel_mm_leaf_preparation *);
enum riscv_sv39_status riscv_sv39_user_cow_snapshot(
    struct riscv_sv39_user_space *, uint64_t, uint64_t *, uint64_t *);
/* replacement==KERNEL_MM_NO_PAGE只请求末引用原地恢复；0是合法新页。 */
enum riscv_sv39_status riscv_sv39_user_cow_commit(
    struct riscv_sv39_user_space *, uint64_t, uint64_t, uint64_t, uint32_t,
    struct kernel_mm_retirement *);
enum riscv_sv39_status riscv_sv39_user_next_owned(
    struct riscv_sv39_user_space *, uint64_t *, uint64_t end, uint64_t *);
enum riscv_sv39_status riscv_sv39_user_mark_cow_page(
    struct riscv_sv39_user_space *, uint64_t);
enum riscv_sv39_status riscv_sv39_user_space_fork_prepare(
    struct riscv_sv39_user_space *, struct riscv_sv39_user_space *,
    int (*)(void *, uint64_t), void *);

/*
 * Changes access for owned leaves in [start, end).  permissions == 0 keeps
 * each physical page owned behind an invalid software PTE (PROT_NONE).
 * Missing leaves are unchanged; present leaves retain content.
 */
/* Pure PTE update: caller publishes TLB/instruction changes after leaving raw. */
enum riscv_sv39_status riscv_sv39_user_protect_owned_page(
    struct riscv_sv39_user_space *space, uint64_t address, uint32_t permissions);

enum riscv_sv39_status riscv_sv39_user_protect_owned_range(
    struct riscv_sv39_user_space *space,
    uint64_t start,
    uint64_t end,
    uint32_t permissions);



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
 * Build child tables that share owned user pages. shared_leaf identifies
 * pages whose writes must remain shared; all other pages use copy-on-write.
 * The destination borrows the same kernel root entries as source. Parent
 * private leaves are committed to COW only after child construction;
 * after initialization, failure may leave destination as a LIVE owner for
 * the caller to destroy.
 */
enum riscv_sv39_status riscv_sv39_user_space_fork(
    struct riscv_sv39_user_space *destination,
    struct riscv_sv39_user_space *source,
    int (*shared_leaf)(void *context, uint64_t virtual_address),
    void *context);

/* Resolves only a present COW leaf; other mappings return NOT_MAPPED. */
enum riscv_sv39_status riscv_sv39_user_resolve_cow(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    uint32_t permissions);

/* Success consumes a LIVE source; failure changes neither object. */
enum riscv_sv39_status riscv_sv39_user_space_move(
    struct riscv_sv39_user_space *destination,
    struct riscv_sv39_user_space *source);

/* The active satp root cannot be destroyed. */
enum riscv_sv39_status riscv_sv39_user_space_destroy(
    struct riscv_sv39_user_space *space);

#endif
