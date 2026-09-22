/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef EXT4_TRUNCATE_H_
#define EXT4_TRUNCATE_H_

#include <ext4_fs.h>

/* Reclaim one bounded batch beyond the inode's already-persisted final size.
 * The caller holds a writable journal transaction and an inode reference
 * obtained inside it. This does not change i_size, commit, or release that ref.
 * Keep the orphan record until a successful committed call reports done.
 * On error abort the transaction, preserving the durable orphan for retry.
 *
 * Each call removes at most 32 data blocks (and empty mapping-tree nodes on
 * those paths). Sparse holes are skipped by tree traversal. No progress cursor
 * is needed: every committed batch removes its pointers and allocation bits
 * together, so the current tree is the restart position after a power cut. */
int ext4_orphan_truncate_step(struct ext4_inode_ref *inode, bool *done);

#endif
