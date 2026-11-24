#ifndef BOAROS_KERNEL_MM_LEAF_H
#define BOAROS_KERNEL_MM_LEAF_H
#include <stdint.h>
#include <kernel/page.h>

enum kernel_mm_leaf_flags {
    KERNEL_MM_LEAF_COW = 1U,
    KERNEL_MM_LEAF_FILE = 2U, /* Private mapping still references file data; implies COW. */
};
/* Snapshot while the caller holds the MM metadata qualification. The PTE owns
 * exactly one reference to physical_address at order, including PROT_NONE.
 * Snapshot values are not references: retain the existing owner before dropping
 * qualification, and compare cookie before publishing a replacement. */
struct kernel_mm_leaf {
    uint64_t virtual_address;  /* aligned leaf start, not the queried address */
    uint64_t physical_address; /* aligned buddy head, never an interior page */
    uint64_t cookie;           /* architecture PTE identity under MM locking */
    uint32_t permissions;     /* effective ARCH_MMU_* flags; zero for PROT_NONE */
    uint32_t order;           /* number of base-page address bits in this leaf */
    uint32_t cow;
    uint32_t file_backed;
};
/* A leaf probe also states which parents are missing for a base-page install:
 * bit 0 is the middle table, bit 1 the leaf table. Present or PROT_NONE leaves
 * have no missing parents. The mask is a preparation hint, not a reference. */
static inline uint64_t kernel_mm_leaf_size(const struct kernel_mm_leaf *leaf)
{ return (uint64_t)BOAROS_PAGE_SIZE << leaf->order; }
#endif
