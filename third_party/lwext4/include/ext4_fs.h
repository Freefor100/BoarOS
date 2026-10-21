/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 *
 *
 * HelenOS:
 * Copyright (c) 2012 Martin Sucha
 * Copyright (c) 2012 Frantisek Princ
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup lwext4
 * @{
 */
/**
 * @file  ext4_fs.c
 * @brief More complex filesystem functions.
 */

#ifndef EXT4_FS_H_
#define EXT4_FS_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_operation.h>
#include <ext4.h>
#include <kernel/io_context.h>

#include <stdint.h>
#include <stdbool.h>

struct ext4_orphan_index;
struct ext4_operation;
struct ext4_lock;

struct ext4_fs {
	bool read_only;
	bool super_replay_required;
	bool journaled_session;

	struct ext4_blockdev *bdev;
	struct ext4_sblock sb;

	uint64_t inode_block_limits[4];
	uint64_t inode_blocks_per_level[4];

	struct jbd_fs *jbd_fs;
	struct jbd_journal *jbd_journal;
	struct ext4_orphan_index *orphan_index;
	uint32_t active_operations;
	const struct ext4_lock *os_locks;
    uint64_t reserved_blocks;
    uint32_t reserved_inodes;
};

/* Keep the port dispatch at its actual caller; the port owns qualification. */
static inline void ext4_fs_state_lock(struct ext4_fs *fs)
{
    const struct ext4_lock *locks = fs->os_locks;
    if (locks && locks->state_lock) locks->state_lock(locks->context);
}
static inline void ext4_fs_state_read_lock(struct ext4_fs *fs)
{
    const struct ext4_lock *locks = fs->os_locks;
    if (locks && locks->state_read_lock) locks->state_read_lock(locks->context);
    else if (locks && locks->state_lock) locks->state_lock(locks->context);
}
static inline void ext4_fs_state_unlock(struct ext4_fs *fs)
{
    const struct ext4_lock *locks = fs->os_locks;
    if (locks && locks->state_unlock) locks->state_unlock(locks->context);
}
static inline bool ext4_fs_serial_writer(struct ext4_fs *fs)
{
    const struct ext4_lock *locks = fs->os_locks;
    return locks && locks->serial_writer && locks->serial_writer(locks->context);
}

static inline struct ext4_operation *ext4_current_operation(struct ext4_fs *fs)
{
    for (struct ext4_operation *operation = kernel_io_context_inline()->ext4_operations;
         operation; operation = operation->next)
        if (operation->fs == fs) return operation;
    return NULL;
}
static inline struct jbd_trans *ext4_current_trans(struct ext4_fs *fs)
{
    struct ext4_operation *operation = ext4_current_operation(fs);
    return operation ? operation->trans : NULL;
}
/* Geometry belongs to the mount. Mutable allocation state has explicit
 * accessors, so ordinary feature/size reads never look up a task context. */
static inline struct ext4_sblock *ext4_fs_super(struct ext4_fs *fs) { return &fs->sb; }
uint64_t ext4_fs_available_blocks(struct ext4_fs *fs);
uint32_t ext4_fs_available_inodes(struct ext4_fs *fs);
int ext4_fs_reserve_blocks(struct ext4_fs *fs, uint32_t count);
uint32_t ext4_fs_reserve_block_range(struct ext4_fs *fs, uint32_t count);
int ext4_fs_reserve_inodes(struct ext4_fs *fs, uint32_t count);
void ext4_fs_account_blocks(struct ext4_fs *fs, int64_t change);
void ext4_fs_account_inodes(struct ext4_fs *fs, int64_t change);
uint32_t ext4_fs_ro_flags(struct ext4_fs *fs);
void ext4_fs_add_ro_flags(struct ext4_fs *fs, uint32_t flags);
uint32_t ext4_fs_orphan_head(struct ext4_fs *fs);
void ext4_fs_set_orphan_head(struct ext4_fs *fs, uint32_t head);
bool ext4_operation_super_changed(struct ext4_operation *operation);
/* Admission/abort holds this filesystem's STATE while publishing counters. */
void ext4_operation_discard_super_state_held(struct ext4_operation *operation);
void ext4_operation_memory_denied(struct ext4_fs *fs);
/* Prepare/publish are memory-only phases owned by the caller's STATE. */
void ext4_operation_publish_super(struct ext4_operation *operation);
void ext4_operation_completed(struct ext4_operation *operation, uint64_t sequence, uint32_t tid);
int ext4_operation_prepare_super(struct ext4_operation *operation, struct ext4_sblock *merged);
int ext4_operation_require_resource(struct ext4_fs *fs, enum ext4_resource_kind kind, uint32_t key);
int ext4_operation_claim_inode(struct ext4_fs *fs, uint32_t inode);
void ext4_operation_release_resources(struct ext4_fs *fs);
bool ext4_operation_resource_retry(struct ext4_fs *fs);

struct ext4_block_group_ref {
	struct ext4_block block;
	struct ext4_bgroup *block_group;
	struct ext4_fs *fs;
	uint32_t index;
	bool dirty;
};

struct ext4_inode_ref {
	struct ext4_block block;
	struct ext4_inode *inode;
	struct ext4_fs *fs;
	uint32_t index;
	bool dirty;
};

/* Read-only geometry/GDT/journal validation; output changes only on success. */
int ext4_fs_calculate_overhead(struct ext4_fs *fs, uint64_t *blocks);


/**@brief Convert block address to relative index in block group.
 * @param s Superblock pointer
 * @param baddr Block number to convert
 * @return Relative number of block
 */
static inline uint32_t ext4_fs_addr_to_idx_bg(struct ext4_sblock *s,
						     ext4_fsblk_t baddr)
{
	if (ext4_get32(s, first_data_block) && baddr)
		baddr--;

	return baddr % ext4_get32(s, blocks_per_group);
}

/**@brief Convert relative block address in group to absolute address.
 * @param s Superblock pointer
 * @param index Relative block address
 * @param bgid Block group
 * @return Absolute block address
 */
static inline ext4_fsblk_t ext4_fs_bg_idx_to_addr(struct ext4_sblock *s,
						     uint32_t index,
						     uint32_t bgid)
{
	if (ext4_get32(s, first_data_block))
		index++;

	return ext4_get32(s, blocks_per_group) * bgid + index;
}

/**@brief TODO: */
static inline ext4_fsblk_t ext4_fs_first_bg_block_no(struct ext4_sblock *s,
						 uint32_t bgid)
{
	return (uint64_t)bgid * ext4_get32(s, blocks_per_group) +
	       ext4_get32(s, first_data_block);
}

/**@brief Initialize filesystem and read all needed data.
 * @param fs Filesystem instance to be initialized
 * @param bdev Identifier if device with the filesystem
 * @param read_only Mark the filesystem as read-only.
 * @return Error code
 */
int ext4_fs_init(struct ext4_fs *fs, struct ext4_blockdev *bdev,
		 bool read_only);

/**@brief Destroy filesystem instance (used by unmount operation).
 * @param fs Filesystem to be destroyed
 * @return Error code
 */
int ext4_fs_fini(struct ext4_fs *fs);

/**@brief Check filesystem's features, if supported by this driver
 * Function can return EOK and set read_only flag. It mean's that
 * there are some not-supported features, that can cause problems
 * during some write operations.
 * @param fs        Filesystem to be checked
 * @param read_only Flag if filesystem should be mounted only for reading
 * @return Error code
 */
int ext4_fs_check_features(struct ext4_fs *fs, bool *read_only);

/**@brief Get reference to block group specified by index.
 * @param fs   Filesystem to find block group on
 * @param bgid Index of block group to load
 * @param ref  Output pointer for reference
 * @return Error code
 */
int ext4_fs_get_block_group_ref(struct ext4_fs *fs, uint32_t bgid,
				struct ext4_block_group_ref *ref);
int ext4_fs_get_block_group_ref_write(struct ext4_fs *fs, uint32_t bgid,
                                    struct ext4_block_group_ref *ref);
/* Allocation prepares the block bitmap; inode allocation also prepares its
 * inode bitmap. New inode slots are initialized by ext4_fs_alloc_inode. */
int ext4_fs_get_block_group_ref_alloc(struct ext4_fs *fs, uint32_t bgid,
				      struct ext4_block_group_ref *ref,
                                      bool inode_allocation);

/**@brief Put reference to block group.
 * @param ref Pointer for reference to be put back
 * @return Error code
 */
int ext4_fs_put_block_group_ref(struct ext4_block_group_ref *ref);

/**@brief Get reference to i-node specified by index.
 * @param fs    Filesystem to find i-node on
 * @param index Index of i-node to load
 * @param ref   Output pointer for reference
 * @return Error code
 */
int ext4_fs_get_inode_ref(struct ext4_fs *fs, uint32_t index,
			  struct ext4_inode_ref *ref);
int ext4_fs_get_inode_ref_read(struct ext4_fs *fs, uint32_t index,
    struct ext4_inode_ref *ref);
/* Qualify an existing read reference before exposing writable inode bytes. */
int ext4_fs_inode_ref_write(struct ext4_inode_ref *ref);

/**@brief Reset blocks field of i-node.
 * @param fs        Filesystem to reset blocks field of i-inode on
 * @param inode_ref ref Pointer for inode to be operated on
 */
void ext4_fs_inode_blocks_init(struct ext4_fs *fs,
			       struct ext4_inode_ref *inode_ref);

/**@brief Put reference to i-node.
 * @param ref Pointer for reference to be put back
 * @return Error code
 */
int ext4_fs_put_inode_ref(struct ext4_inode_ref *ref);

/**@brief Convert filetype to inode mode.
 * @param filetype File type
 * @return inode mode
 */
uint32_t ext4_fs_correspond_inode_mode(int filetype);

/**@brief Allocate new i-node in the filesystem.
 * @param fs        Filesystem to allocated i-node on
 * @param inode_ref Output pointer to return reference to allocated i-node
 * @param filetype  File type of newly created i-node
 * @param parent_inode Parent i-node number, or zero when formatting the root
 * @param name Final component name, borrowed for this call
 * @param name_len Component length; zero when parent_inode is zero
 * @return Error code
 */
int ext4_fs_alloc_inode(struct ext4_fs *fs, struct ext4_inode_ref *inode_ref,
    int filetype, uint32_t parent_inode, const char *name, uint32_t name_len);

/**@brief Release i-node and mark it as free.
 * @param inode_ref I-node to be released
 * @return Error code
 */
int ext4_fs_free_inode(struct ext4_inode_ref *inode_ref);

/**@brief Truncate i-node data blocks.
 * @param inode_ref I-node to be truncated
 * @param new_size  New size of inode (must be < current size)
 * @return Error code
 */
int ext4_fs_truncate_inode(struct ext4_inode_ref *inode_ref, uint64_t new_size);

/**@brief Compute 'goal' for inode index
 * @param inode_ref Reference to inode, to allocate block for
 * @return goal
 */
ext4_fsblk_t ext4_fs_inode_to_goal_block(struct ext4_inode_ref *inode_ref);

/**@brief Compute 'goal' for allocation algorithm (For blockmap).
 * @param inode_ref Reference to inode, to allocate block for
 * @return error code
 */
int ext4_fs_indirect_find_goal(struct ext4_inode_ref *inode_ref,
				ext4_fsblk_t *goal);

/**@brief Get physical block address by logical index of the block.
 * @param inode_ref I-node to read block address from
 * @param iblock            Logical index of block
 * @param fblock            Output pointer for return physical
 *                          block address
 * @param support_unwritten Indicate whether unwritten block range
 *                          is supported under the current context
 * @return Error code
 */
int ext4_fs_get_inode_dblk_idx(struct ext4_inode_ref *inode_ref,
				 ext4_lblk_t iblock, ext4_fsblk_t *fblock,
				 bool support_unwritten);

/* Allocations end at a hole/extent boundary or an indirect pointer leaf;
 * a read-only lookup can cover an entire missing indirect subtree.
 * Unwritten ranges retain their physical address; readers return zeroes. */
struct ext4_inode_block_range {
    ext4_fsblk_t block;
    uint32_t count;
    bool unwritten;
    bool allocated;
};
int ext4_fs_get_inode_dblk_range(struct ext4_inode_ref *inode_ref,
    ext4_lblk_t iblock, uint32_t max_blocks, bool create,
    struct ext4_inode_block_range *range);

/**@brief Initialize a part of unwritten range of the inode.
 * @param inode_ref I-node to proceed on.
 * @param iblock    Logical index of block
 * @param fblock    Output pointer for return physical block address
 * @return Error code
 */
int ext4_fs_init_inode_dblk_idx(struct ext4_inode_ref *inode_ref,
				  ext4_lblk_t iblock, ext4_fsblk_t *fblock);

/**@brief Release one exact logical data block after failed initialization.
 * @param inode_ref I-node to proceed on.
 * @param iblock    Logical index of block.
 * @param fblock    Expected physical block address.
 * @return Error code
 */
int ext4_fs_release_inode_dblk_idx(struct ext4_inode_ref *inode_ref,
				   ext4_lblk_t iblock,
				   ext4_fsblk_t fblock);

/**@brief Append following logical block to the i-node.
 * @param inode_ref I-node to append block to
 * @param fblock    Output physical block address of newly allocated block
 * @param iblock    Output logical number of newly allocated block
 * @return Error code
 */
int ext4_fs_append_inode_dblk(struct ext4_inode_ref *inode_ref,
			      ext4_fsblk_t *fblock, ext4_lblk_t *iblock);

/**@brief   Increment inode link count.
 * @param   inode_ref none handle
 */
void ext4_fs_inode_links_count_inc(struct ext4_inode_ref *inode_ref);

/**@brief   Decrement inode link count.
 * @param   inode_ref none handle
 */
void ext4_fs_inode_links_count_dec(struct ext4_inode_ref *inode_ref);

#ifdef __cplusplus
}
#endif

#endif /* EXT4_FS_H_ */

/**
 * @}
 */
