#ifndef EXT4_OPERATION_H_
#define EXT4_OPERATION_H_

#include <ext4_types.h>
#include <ext4_bcache.h>
#include <kernel/cost.h>

struct ext4_fs;
struct jbd_trans;
enum ext4_resource_kind {
    EXT4_RESOURCE_INODE,
    EXT4_RESOURCE_ORPHAN,
    EXT4_RESOURCE_BLOCK_GROUP,
};
/* Private preparation belongs to the actual task, independently of journal
 * groups and the mount's accepted metadata. */
struct ext4_operation {
    struct ext4_fs *fs;
    struct jbd_trans *trans;
    struct ext4_operation *next;
    int64_t block_delta, inode_delta;
    uint64_t reserved_blocks;
    uint32_t reserved_inodes;
    uint32_t ro_flags_add;
    uint32_t orphan_base, orphan_head;
    struct {
        uint64_t blocks;
        uint32_t inodes, ro_flags, orphan_head, checksum;
    } super_publish;
    struct ext4_writeback_scope scope;
    void *staged_times;
#if BOAROS_COST_DIAGNOSTICS
    struct kernel_cost_scope prepare_cost;
#endif
    uint32_t depth;
    int error;
    bool aborted;
    bool orphan_metadata;
    bool orphan_chain;
    bool published;
    bool completed;
    bool super_prepared;
    bool orphan_presence_touched;
    bool log_reservation_failed;
    bool memory_reservation_failed;
};

#endif
