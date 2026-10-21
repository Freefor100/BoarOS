/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef EXT4_ORPHAN_H_
#define EXT4_ORPHAN_H_

#include <ext4_fs.h>

#define EXT4_ORPHAN_FILE_COMPAT 0x1000U
#define EXT4_ORPHAN_PRESENT_RO_COMPAT 0x10000U
#define EXT4_ORPHAN_BLOCK_MAGIC 0x0b10ca04U

/* Mutations require the caller's writable journal operation and retain its
 * inode reference. orphan_file blocks have separate modification qualifications;
 * the legacy chain uses its own topology qualification through acceptance.
 * The caller journals the operation's superblock view in the same transaction.
 * Remove the orphan record before freeing the inode bitmap, in one transaction.
 * Add/remove are idempotent; malformed or duplicated records are errors.
 * ORPHAN_PRESENT is calculated from the accepted count plus this operation's
 * desired membership changes, independently of other private operations.
 *
 * Validation builds a filesystem-owned membership/location index after checking
 * every legacy and orphan_file record. Contains/peek borrow that index without
 * I/O or allocation; add/remove validate the affected metadata and retain private
 * desired records. Acceptance publishes those records under the state protection;
 * abort discards only this operation's records and its reserved index capacity.
 * Explicit validate rebuilds the index and is not allowed with pending changes.
 */
void ext4_orphan_index_fini(struct ext4_fs *fs);
/* The journal admission/abort owner already holds this filesystem's STATE. */
void ext4_orphan_trans_finish_state_held(struct jbd_trans *trans, bool abort);
bool ext4_orphan_trans_membership_changed(struct jbd_trans *trans);
int ext4_orphan_trans_count(struct jbd_trans *trans, size_t *count);
int ext4_orphan_validate(struct ext4_fs *fs);
int ext4_orphan_peek(struct ext4_fs *fs, uint32_t *inode);
int ext4_orphan_contains(struct ext4_fs *fs, uint32_t inode, bool *present);
/* Runtime intent distinguishes a live unlinked owner from pending EOF reclaim;
 * changes join the operation undo, and mount recovery defaults to inspection. */
int ext4_orphan_truncate_pending(struct ext4_fs *, uint32_t inode, bool *pending);
/* Retain an existing record before mapping changes acquire allocation groups.
 * A missing record needs no qualification and remains absent. */
int ext4_orphan_prepare_change(struct ext4_inode_ref *inode);
int ext4_orphan_mark_truncate(struct ext4_inode_ref *, bool pending);
int ext4_orphan_add(struct ext4_inode_ref *inode);
int ext4_orphan_remove(struct ext4_inode_ref *inode);

/* Change only the in-memory superblock marker. Clearing requires no remaining
 * orphan records; returns ENOTEMPTY otherwise. The caller journals the change.
 * A filesystem without orphan_file needs no ORPHAN_PRESENT marker. */
int ext4_orphan_set_present(struct ext4_fs *fs, bool present);

#endif
