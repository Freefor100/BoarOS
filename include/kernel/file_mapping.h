#ifndef BOAROS_KERNEL_FILE_MAPPING_H
#define BOAROS_KERNEL_FILE_MAPPING_H

#include <stdint.h>

struct kernel_vfs_node;

/* MM owns this stable record and keeps its node alive through its OFDs.
 * Register/unregister/notify run in the single-hart non-scheduling region.
 * The callback must neither allocate nor change the registration list. */
struct kernel_file_mapping {
    struct kernel_file_mapping *next;
    struct kernel_file_mapping **previous;
    struct kernel_vfs_node *node;
    void *owner;
    void (*truncate)(void *owner, struct kernel_vfs_node *node, uint64_t size);
};

void kernel_file_mapping_register(struct kernel_file_mapping *mapping);
void kernel_file_mapping_unregister(struct kernel_file_mapping *mapping);

#endif
