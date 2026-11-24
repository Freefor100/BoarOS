#ifndef BOAROS_KERNEL_HUGETLB_H
#define BOAROS_KERNEL_HUGETLB_H
#include <stdint.h>
struct kernel_heap;
struct physical_page_allocator;
struct kernel_hstate;
struct kernel_hugetlb_map;
enum kernel_hugetlb_status {
    KERNEL_HUGETLB_OK, KERNEL_HUGETLB_INVALID, KERNEL_HUGETLB_NO_MEMORY,
    KERNEL_HUGETLB_BUS, KERNEL_HUGETLB_BUSY, KERNEL_HUGETLB_REVOKE_CHILDREN, KERNEL_HUGETLB_NOT_FOUND
};
enum kernel_hugetlb_mode { KERNEL_HUGETLB_PRIVATE, KERNEL_HUGETLB_SHARED, KERNEL_HUGETLB_CHILD };
struct kernel_hstate_snapshot { uint64_t total, free, reserved, maps, persistent, surplus; uint32_t order; };
/* One native pool per allocator. Actual pool owners never reenter buddy until
 * shrink/destroy removes their management record. The allocator recycler runs
 * under allocator->lock; all pool/ledger metadata shares that qualification.
 * Heap and physical allocation, zeroing and normal reference operations happen
 * outside it. Caller serializes control operations and holds hstate lifetime;
 * destruction requires no map/claim owners and no simultaneous pool users. */
enum kernel_hugetlb_status kernel_hstate_create(struct kernel_heap *,
    struct physical_page_allocator *, uint32_t order, struct kernel_hstate **);
/* Registry holds its original owner until explicit destroy. These accessors
 * return additional stable references; put returns exactly that reference.
 * get never creates, allowing /proc readers to report an absent pool as zero. */
enum kernel_hugetlb_status kernel_hstate_get(struct physical_page_allocator *, struct kernel_hstate **);
enum kernel_hugetlb_status kernel_hstate_get_or_create(struct kernel_heap *,
    struct physical_page_allocator *, uint32_t order, struct kernel_hstate **);
void kernel_hstate_put(struct kernel_hstate *);
enum kernel_hugetlb_status kernel_hstate_resize(struct kernel_hstate *, uint64_t target);
enum kernel_hugetlb_status kernel_hstate_grow(struct kernel_hstate *, uint64_t count);
/* Releases only unreserved free blocks; actual number removed is returned. */
uint64_t kernel_hstate_shrink(struct kernel_hstate *, uint64_t count);
enum kernel_hugetlb_status kernel_hstate_destroy(struct kernel_hstate **);
/* Final allocator teardown only: all users, map callbacks and proc readers must
 * be quiesced. Rejects live map/pin owners rather than hiding a cleanup leak. */
enum kernel_hugetlb_status kernel_hstate_shutdown(struct physical_page_allocator *);
void kernel_hstate_snapshot(struct kernel_hstate *, struct kernel_hstate_snapshot *);
/* Construction reserves all slots against already held contiguous capacity,
 * unless noreserve. A private fork uses CHILD and gets no parent reservation.
 * Shared fork reuses the same object with acquire. VMA splits share a map and
 * retain their original indices; only final removal cancels those indices. */
enum kernel_hugetlb_status kernel_hugetlb_map_create(struct kernel_hstate *,
    uint64_t pages, enum kernel_hugetlb_mode, int noreserve, struct kernel_hugetlb_map **);
/* Files share a sparse backing. Reservations and cache references belong to
 * the inode until truncate or final release, independently of its VMAs. */
struct kernel_hugetlb_file_slot;
struct kernel_hugetlb_reservation {
    struct kernel_hugetlb_map *map;
    struct kernel_hugetlb_file_slot *candidates;
};
enum kernel_hugetlb_status kernel_hugetlb_file_create(struct kernel_hstate *,
    struct kernel_hugetlb_map **);
/* Charge capacity before mapping and publish the reservation only after it
 * succeeds. The caller excludes removal in [first, end) until finish. */
enum kernel_hugetlb_status kernel_hugetlb_file_reserve_begin(struct kernel_hugetlb_map *,
    uint64_t first, uint64_t end, struct kernel_hugetlb_reservation *);
void kernel_hugetlb_file_reserve_finish(struct kernel_hugetlb_reservation *, int accepted);
/* Return one physical reference to a resident file page without faulting.
 * The caller excludes removal until the physical reference is acquired. */
enum kernel_hugetlb_status kernel_hugetlb_file_find_page(struct kernel_hugetlb_map *,
    uint64_t index, uint64_t *physical);
/* The inode's mapping publication is closed and affected faults are drained.
 * Already acquired physical references survive removal of the inode owner. */
void kernel_hugetlb_file_remove(struct kernel_hugetlb_map *, uint64_t first, uint64_t end);
void kernel_hugetlb_file_punch(struct kernel_hugetlb_map *, uint64_t first, uint64_t end);
void kernel_hugetlb_map_acquire(struct kernel_hugetlb_map *);
void kernel_hugetlb_map_release(struct kernel_hugetlb_map **);
enum kernel_hugetlb_status kernel_hugetlb_map_fork(struct kernel_hugetlb_map *,
    struct kernel_hugetlb_map **);
/* Private file maps use local slots and preserve their base file index across
 * fork. Set it before publishing the first VMA. */
void kernel_hugetlb_map_set_file_index(struct kernel_hugetlb_map *, uint64_t first);
uint64_t kernel_hugetlb_map_file_index(const struct kernel_hugetlb_map *);
/* The MM writer has already detached the affected leaves. Slots may fault
 * again after file growth; unused reservations retain their original owner. */
void kernel_hugetlb_private_reset(struct kernel_hugetlb_map *, uint64_t first, uint64_t end);
/* Private unmap: MM must prevent later publication for the removed indices.
 * Cancels unused reservations; PTE/pin owners still retire through the allocator.
 * Shared munmap does not delete backing slots; release its actual map owner. */
enum kernel_hugetlb_status kernel_hugetlb_map_unmap(struct kernel_hugetlb_map *,
    uint64_t first, uint64_t end);
/* Read-only shared-cache residency snapshot; caller holds a stable map owner.
 * Does not fault, acquire a data owner, or report private PTE residency. */
/* Shared backing residency in base-page units; O(1), stable map owner required. */
uint64_t kernel_hugetlb_map_resident_pages(struct kernel_hugetlb_map *);
int kernel_hugetlb_map_resident(struct kernel_hugetlb_map *, uint64_t index);
/* Non-owner descendants explicitly revoked by a reserve owner's failed COW
 * must stay fault-ineligible. Caller first withdraws PTEs and confirms TLB/pins. */
enum kernel_hugetlb_status kernel_hugetlb_map_revoke(struct kernel_hugetlb_map *, uint64_t index);
int kernel_hugetlb_map_is_child_of(const struct kernel_hugetlb_map *child,
    const struct kernel_hugetlb_map *owner);
/* MM owns this node and a live map reference through stop+drain. A callback
 * borrow pins the registration and its owner; stop only unlinks, drain waits
 * outside MM qualification. Callback runs with no allocator or MM raw lock. */
struct kernel_hugetlb_mapping {
    struct kernel_hugetlb_mapping *next, **previous, *retired_next;
    struct kernel_hugetlb_map *map;
    void *owner;
    enum kernel_hugetlb_status (*revoke)(void *, struct kernel_hugetlb_map *, uint64_t index, uint64_t old_head);
    uint32_t borrows;
};
enum kernel_hugetlb_status kernel_hugetlb_map_epoch(struct kernel_hugetlb_map *, uint64_t *);
enum kernel_hugetlb_status kernel_hugetlb_mapping_register(struct kernel_hugetlb_mapping *, uint64_t epoch);
void kernel_hugetlb_mapping_stop(struct kernel_hugetlb_mapping *);
void kernel_hugetlb_mapping_drain(struct kernel_hugetlb_mapping *);
/* Caller retains owner map but drops all MM gates. Child callbacks must finish
 * withdrawal/TLB/pin draining and mark revoked before returning. Recheck parent
 * PTE cookie after reacquiring its gate; this routine is not a COW commit. */
enum kernel_hugetlb_status kernel_hugetlb_revoke_children(struct kernel_hugetlb_map *owner,uint64_t index,uint64_t old_head);
/* Token cannot be copied. begin retains the map and returns one real data owner.
 * cow=1 never spends a mapping's unused reservation. Reserved owner COW OOM
 * requests MM child revocation; unreserved/child OOM means SIGBUS. BUSY means
 * another claim is preparing this slot, not successful allocation. */
struct kernel_hugetlb_slot;
struct kernel_hugetlb_claim {
    struct kernel_hugetlb_map *map;
    struct kernel_hugetlb_slot *slot;
    uint64_t index, physical_address;
    uint32_t order, cow, reserved, shared;
};
enum kernel_hugetlb_status kernel_hugetlb_fault_begin(struct kernel_hugetlb_map *,
    uint64_t index, int cow, struct kernel_hugetlb_claim *);
/* Consume the first private reservation for a file-backed copy. The caller
 * initializes every byte from the file page before exposing the new leaf. */
enum kernel_hugetlb_status kernel_hugetlb_private_copy_begin(struct kernel_hugetlb_map *,
    uint64_t index, struct kernel_hugetlb_claim *);
/* accepted transfers the token's data reference to the PTE. On rejection this
 * routine returns it and restores a consumed reservation atomically with pool
 * custody. Call only after MM publication/revalidation, outside MM raw locks. */
void kernel_hugetlb_fault_finish(struct kernel_hugetlb_claim *, int accepted);
/* Two-phase DONTNEED: begin precedes PTE withdrawal and holds the slot/map.
 * Caller holds MM writer qualification throughout; exclusive additionally means
 * pin admission is closed/drained and no new owner can be acquired through the
 * old PTE until finish. begin verifies ref1, otherwise reserves extra capacity.
 * BUSY changes nothing. finish accepted consumes exactly the detached PTE owner
 * after TLB invalidation; rejected retains it and rolls back the preflight.
 * Successful begin makes finish infallible under this ownership contract. */
struct kernel_hugetlb_discard_claim {
    struct kernel_hugetlb_map *map;
    uint64_t index, physical_address;
    uint32_t exclusive, reserved;
};
enum kernel_hugetlb_status kernel_hugetlb_discard_begin(struct kernel_hugetlb_map *,
    uint64_t index, uint64_t physical_address, int exclusive,
    struct kernel_hugetlb_discard_claim *);
void kernel_hugetlb_discard_finish(struct kernel_hugetlb_discard_claim *, int accepted);
/* Restore a private DONTNEED slot using a real detached reference after TLB
 * invalidation. A sole reference is transferred directly to free-pool custody.
 * With remaining pins/aliases restoration needs additional unreserved capacity;
 * BUSY leaves both ledger and supplied owner unchanged for MM to handle. On OK
 * consumes the detached reference. Shared DONTNEED leaves backing untouched. */
enum kernel_hugetlb_status kernel_hugetlb_discard(struct kernel_hugetlb_map *,
    uint64_t index, uint64_t physical_address);
#endif
