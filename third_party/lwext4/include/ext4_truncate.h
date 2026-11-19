/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef EXT4_TRUNCATE_H_
#define EXT4_TRUNCATE_H_

#include <ext4_fs.h>

/* Reclaim one bounded batch beyond the inode's already-persisted final size.
 * The caller holds a writable backend operation and an inode reference
 * obtained inside it. This does not change i_size, commit, or release that ref.
 * Journal callers keep the orphan until a committed call reports done; abort
 * preserves it for retry. Nonjournal callers retain actual partial progress.
 *
 * Each call removes at most 32 data blocks (and empty mapping-tree nodes on
 * those paths). Sparse holes are skipped by tree traversal. No progress cursor
 * is needed: every committed batch removes its pointers and allocation bits
 * together, so the current tree is the restart position after a power cut. */
int ext4_inode_truncate_step(struct ext4_inode_ref *inode, bool *done);

/* The final unlinked owner may reclaim a longer extent run in one group.
 * A non-NULL expanded enables the larger run and reports whether it was used.
 * After a rolled-back resource error the caller can retry without expansion. */
int ext4_inode_truncate_orphan_step(struct ext4_inode_ref *inode, bool *done,
                                  bool *expanded);

#endif
