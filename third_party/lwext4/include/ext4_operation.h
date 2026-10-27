#ifndef EXT4_OPERATION_H_
#define EXT4_OPERATION_H_

#include <ext4_types.h>
#include <ext4_bcache.h>
#include <kernel/cost.h>

struct ext4_fs;
struct jbd_trans;
struct ext4_inode_goal;
RB_HEAD(ext4_inode_goal_tree, ext4_inode_goal);
TAILQ_HEAD(ext4_inode_goal_queue, ext4_inode_goal);
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
    struct ext4_delalloc *delalloc;
    struct file_write_span *delalloc_spans;
    unsigned delalloc_span_count;
    uint64_t delalloc_credit, delalloc_used;

    uint32_t ro_flags_add;
    uint32_t orphan_base, orphan_head;
    struct {
        uint64_t blocks;
        uint32_t inodes, ro_flags, orphan_head, checksum;
    } super_publish;
    struct ext4_writeback_scope scope;
    void *staged_times;
    struct ext4_inode_goal_tree inode_goals;
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
