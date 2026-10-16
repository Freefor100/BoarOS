/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef EXT4_ORPHAN_H_
#define EXT4_ORPHAN_H_

#include <ext4_fs.h>

#define EXT4_ORPHAN_FILE_COMPAT 0x1000U
#define EXT4_ORPHAN_PRESENT_RO_COMPAT 0x10000U
#define EXT4_ORPHAN_BLOCK_MAGIC 0x0b10ca04U

/* The caller serializes filesystem operations. Mutations require a writable
 * filesystem and its current journal transaction. They neither commit nor
 * release the caller's inode reference. The caller must journal fs->sb in the
 * same transaction and restore its in-memory snapshot on transaction abort.
 * Remove the orphan record before freeing the inode bitmap, in one transaction.
 * Add/remove are idempotent; malformed or duplicated records are errors.
 * Remove also clears ORPHAN_PRESENT when the indexed record count proves
 * the resulting set empty; the caller journals/rolls back that marker with sb.
 *
 * Validation builds a filesystem-owned membership/location index after checking
 * every legacy and orphan_file record. Contains/peek borrow that index without
 * I/O or allocation; add/remove validate the affected metadata and update it.
 * Undo records follow the current journal operation until abort or acceptance.
 * Explicit validate rebuilds the index and is not allowed with pending changes.
 */
void ext4_orphan_index_fini(struct ext4_fs *fs);
void ext4_orphan_trans_finish(struct jbd_trans *trans, bool abort);
int ext4_orphan_validate(struct ext4_fs *fs);
int ext4_orphan_peek(struct ext4_fs *fs, uint32_t *inode);
int ext4_orphan_contains(struct ext4_fs *fs, uint32_t inode, bool *present);
int ext4_orphan_add(struct ext4_inode_ref *inode);
int ext4_orphan_remove(struct ext4_inode_ref *inode);

/* Change only the in-memory superblock marker. Clearing requires no remaining
 * orphan records; returns ENOTEMPTY otherwise. The caller journals the change.
 * A filesystem without orphan_file needs no ORPHAN_PRESENT marker. */
int ext4_orphan_set_present(struct ext4_fs *fs, bool present);

#endif
