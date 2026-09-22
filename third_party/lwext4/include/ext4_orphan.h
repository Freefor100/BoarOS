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
 *
 * Reads validate the complete legacy list and orphan file; temporary memory is
 * O(number of live orphan records), with no retained handles or buffer pins.
 * Each operation scans those records and all preallocated orphan-file blocks.
 */
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
