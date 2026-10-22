/*
 * Copyright (c) 2015 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 * Copyright (c) 2015 Kaho Ng (ngkaho1234@gmail.com)
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
 * @file  ext4_journal.h
 * @brief Journal handle functions
 */

#ifndef EXT4_JOURNAL_H_
#define EXT4_JOURNAL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_blockdev.h>
#include <ext4_operation.h>
#include <ext4_journal_memory.h>
#include <misc/queue.h>
#include <misc/tree.h>

struct jbd_fs {
	struct ext4_blockdev *bdev;
	struct ext4_inode_ref inode_ref;
	struct jbd_sb sb;

	bool dirty;
};

/* 块载荷独立分配，控制记录不会把一个整块申请推到下一阶。 */
struct jbd_image {
	struct ext4_block_payload payload;
	void *data;
	struct jbd_memory *memory;
	struct jbd_image *next;
	bool active;
};

struct jbd_buf {
	bool private_access;
	struct jbd_block_rec *reserved_rec;
	bool escaped;
	bool modified;
	/* Materialized only when a record's baseline differs from block.payload. */
	struct jbd_image *before;
	struct jbd_image *after;
	struct ext4_block_write home_io;
	uint32_t jbd_lba;
	struct ext4_block block;
	struct jbd_trans *trans;
	/* Prepare borrows the index identity; accepted access is queued in it. */
	struct jbd_block_rec *block_rec;
	TAILQ_ENTRY(jbd_buf) buf_node;
	TAILQ_ENTRY(jbd_buf) dirty_buf_node;
	TAILQ_ENTRY(jbd_buf) checkpoint_node;
	/* Dirty words followed by initialized words, one bit per 32-byte fragment.
	 * Each map includes a whole final word, including for 1K blocks. */
	uint64_t dirty_ranges[];
};

struct jbd_source;
/* File payload belongs to the transaction but is never a log record. */
struct jbd_data {
	struct ext4_block block;
	struct jbd_image *image;
	struct jbd_source *source;
	struct ext4_block_write io;
	RB_ENTRY(jbd_data) index_node;
	TAILQ_ENTRY(jbd_data) node;
};

struct jbd_revoke_rec {
	ext4_fsblk_t lba;
	RB_ENTRY(jbd_revoke_rec) revoke_node;
};

struct jbd_block_rec {
	ext4_fsblk_t lba;
	struct jbd_trans *trans;
	uint32_t private_refs;
	struct jbd_buf *checkpoint;
	RB_ENTRY(jbd_block_rec) block_rec_node;
	LIST_ENTRY(jbd_block_rec) tbrec_node;
	TAILQ_HEAD(jbd_buf_dirty, jbd_buf) dirty_buf_queue;
};

struct jbd_quarantine {
	ext4_fsblk_t first;
	uint32_t count;
	bool inode;
	RB_ENTRY(jbd_quarantine) index_node;
	LIST_ENTRY(jbd_quarantine) node;
	struct jbd_trans *owner;
};

struct ext4_orphan_undo;

struct jbd_trans {
	struct ext4_operation operation;
	uint32_t trans_id;

	uint32_t start_iblock;
	int alloc_blocks;
	int data_cnt;
	uint32_t data_csum;
	int written_cnt;
	int error;
	uint64_t sequence;
	uint64_t first_dirty_ns;
	unsigned reserved_logs;
	/* Nonzero sequence binds a delta reservation to the actual running group. */
	uint64_t reservation_group_sequence;
	unsigned ordered_count, revoke_count;
	bool frozen;

	struct jbd_journal *journal;
	struct ext4_orphan_undo *orphan_undo;

	TAILQ_HEAD(jbd_trans_buf, jbd_buf) buf_queue;
	TAILQ_HEAD(jbd_trans_data, jbd_data) data_queue;
	RB_HEAD(jbd_data_tree, jbd_data) data_root;
	LIST_HEAD(jbd_quarantine_list, jbd_quarantine) quarantine;
	RB_HEAD(jbd_revoke_tree, jbd_revoke_rec) revoke_root;
	LIST_HEAD(jbd_trans_block_rec, jbd_block_rec) tbrec_list;
	TAILQ_ENTRY(jbd_trans) trans_node;
};

struct jbd_journal {
	/* Last durable commit, and sticky session error owned by this journal. */
	uint32_t committed_id;
	int error;
	struct jbd_trans *failed_trans;
	struct jbd_trans *running, *committing;
	struct jbd_trans *checkpointing, *checkpoint_last;
	TAILQ_HEAD(jbd_checkpoint_bufs, jbd_buf) checkpoint_bufs;
	struct ext4_block_write *checkpoint_io;
	TAILQ_HEAD(jbd_sealed_queue, jbd_trans) sealed_queue;
	unsigned sealed_count, checkpoint_count, log_reserved;
	uint64_t checkpoint_target;
	bool grouped;
	/* Transaction lists own each range through abort or checkpoint. These
	 * block/inode indexes borrow the same nodes across accept and sealing. */
	RB_HEAD(jbd_quarantine_tree, jbd_quarantine) quarantine_index[2];
	uint64_t quarantined_blocks, quarantined_inodes;
	struct jbd_memory *memory;
	RB_HEAD(jbd_source_tree, jbd_source) sources;
	uint64_t accepted_sequence, sealed_sequence, durable_sequence, checkpoint_sequence;
	uint64_t seal_target;
	ext4_fsblk_t *log_map;
	/* Each actual consumer owns one block until its synchronous I/O ends. */
	struct jbd_image *commit_image, *checkpoint_image;

	uint32_t first;
	uint32_t start;
	uint32_t last;
	uint32_t trans_id;
	uint32_t alloc_trans_id;

	uint32_t block_size;

	TAILQ_HEAD(jbd_cp_queue, jbd_trans) cp_queue;
	RB_HEAD(jbd_block, jbd_block_rec) block_rec_root;

	struct jbd_fs *jbd_fs;
};

static inline int jbd_journal_error(const struct jbd_journal *journal)
{ return __atomic_load_n(&journal->error, __ATOMIC_ACQUIRE); }

int jbd_get_fs(struct ext4_fs *fs,
	       struct jbd_fs *jbd_fs);
int jbd_put_fs(struct jbd_fs *jbd_fs);
int jbd_inode_bmap(struct jbd_fs *jbd_fs,
		   ext4_lblk_t iblock,
		   ext4_fsblk_t *fblock);
int jbd_recover(struct jbd_fs *jbd_fs);
/* Start/stop journal the primary superblock before checkpointing it. A start
 * failure with journal.error != 0 retains the initialized journal/fs owner;
 * the caller must not release jbd_fs or its mount storage on that path. */
int jbd_journal_start(struct jbd_fs *jbd_fs,
		      struct jbd_journal *journal);
int jbd_journal_stop(struct jbd_journal *journal);
int jbd_journal_new_trans(struct jbd_journal *journal, struct jbd_trans **out);
int jbd_trans_set_block_dirty(struct jbd_trans *trans,
			      struct ext4_block *block);
/* Obtain a private replacement before changing a buffer. */
int jbd_trans_get_write_access(struct jbd_trans *trans,
			       struct ext4_block *block);
int jbd_trans_get_write_access_range(struct jbd_trans *trans,
    struct ext4_block *block, uint32_t offset, uint32_t length);
int jbd_trans_write_data(struct jbd_trans *trans, ext4_fsblk_t block,
	uint32_t offset, const void *data, uint32_t length, bool initialize,
	const struct file_data_source *source);
/* State protection selects a version and retains its payload/source. The
 * caller copies outside that protection and releases the actual borrow. */
struct jbd_data_borrow {
    const void *data;
    struct ext4_block_payload *payload;
    struct jbd_source *source;
};
bool jbd_journal_borrow_data(struct ext4_fs *, ext4_fsblk_t, struct jbd_data_borrow *);
void jbd_journal_release_borrow(struct jbd_data_borrow *);
int jbd_trans_revoke_block(struct jbd_trans *trans,
			   ext4_fsblk_t lba);
int jbd_trans_try_revoke_block(struct jbd_trans *trans,
			       ext4_fsblk_t lba);
void jbd_journal_free_trans(struct jbd_journal *journal,
			    struct jbd_trans *trans,
			    bool abort);
/* The caller owns this journal's STATE through admission or complete abort.
 * Independent callers use the public entrypoints which acquire it once. */
void jbd_journal_free_trans_state_held(struct jbd_journal *journal,
    struct jbd_trans *trans, bool abort);
/* Requires one successful prepare_operation after the final private mutation.
 * Consumes trans on success or a retryable precommit failure (journal.error
 * stays zero). On critical journal I/O failure, failed_trans retains ownership.
 * A failed operation discards its own superblock deltas and reservations. */
int jbd_journal_commit_trans(struct jbd_journal *journal,
			     struct jbd_trans *trans);
int
jbd_journal_purge_cp_trans(struct jbd_journal *journal,
			   bool flush,
			   bool once);

/** Verify that a transaction dependency has reached stable journal storage.
 * IDs use modulo-32-bit ordering within one mounted journal session.
 * This does not write unrelated home buffers. Returns EAGAIN for future IDs.
 */
int jbd_journal_sync(struct jbd_journal *journal, uint32_t trans_id);
/* Operation-owned auxiliary records use the same mount budget as versions. */
void *jbd_journal_alloc(struct jbd_journal *journal, size_t bytes);
void jbd_journal_free(void *pointer);

/* An existing private access owns its loaded home buffer and replacement.
 * On a hit lend those owners directly; errors return no descriptor loan. */
int jbd_trans_borrow_private(struct jbd_trans *trans, struct ext4_block *block,
    ext4_fsblk_t lba, uint32_t offset, uint32_t length,
    enum ext4_block_access access, bool *found);
int jbd_trans_set_block_dirty_range(struct jbd_trans *trans,
    struct ext4_block *block, uint32_t offset, uint32_t length);

/* Bind final private mutations and reserve ring space. Parallel grouping
 * reserves standalone credits; a serial writer reserves its bound group's
 * delta. Failure or a lost group requires abort before preparing again. */
int jbd_trans_prepare_operation(struct jbd_trans *trans);
int jbd_journal_accept(struct jbd_journal *journal, struct jbd_trans *operation,
	uint64_t now_ns);
int jbd_journal_accept_state_held(struct jbd_journal *journal,
    struct jbd_trans *operation, uint64_t now_ns);
int jbd_journal_group_init(struct jbd_journal *journal, size_t limit);
void jbd_journal_group_fini(struct jbd_journal *journal);
/* Freeze transfers accepted versions and a complete ring interval to the
 * sealed FIFO. The selected commit owner encodes and submits it outside state. */
int jbd_journal_freeze(struct jbd_journal *journal);
int jbd_journal_submit(struct jbd_journal *journal);
enum jbd_journal_stage {
	JBD_JOURNAL_IDLE,
	JBD_JOURNAL_COMMIT,
	JBD_JOURNAL_CHECKPOINT,
};
int jbd_journal_retire_stage(struct jbd_journal *journal,
	enum jbd_journal_stage stage, int error);
/* The two consumers select and retire their own immutable owners under the
 * modification lock. Their device I/O may overlap outside that lock. IDLE
 * selects either stage for a caller that drives both synchronously. */
enum jbd_journal_stage jbd_journal_select_stage(struct jbd_journal *journal,
	enum jbd_journal_stage stage);
bool jbd_journal_checkpoint_ready(const struct jbd_journal *journal);
bool jbd_journal_select(struct jbd_journal *journal);
int jbd_journal_retire(struct jbd_journal *journal, int error);
int jbd_journal_checkpoint(struct jbd_journal *journal);
/* Caller holds STATE. An allocated transaction already charges this journal's
 * budget; NULL includes the control storage still needed before attachment. */
bool jbd_journal_operation_room(struct jbd_journal *journal, const struct jbd_trans *allocated,
    size_t payloads, size_t data_images,
	const struct file_data_source *const *sources, unsigned count);
uint64_t jbd_journal_reclaim_target(struct jbd_journal *journal);
int jbd_trans_quarantine(struct ext4_fs *fs, ext4_fsblk_t first,
	uint32_t count, bool inode);
/* The caller pins the bitmap and owns its block-group modification right.
 * Find one clear run while walking the protected quarantine index once. */
int jbd_journal_find_reusable(struct ext4_fs *fs, const void *bitmap,
    ext4_fsblk_t base, uint32_t start, uint32_t end, uint32_t maximum,
    bool inode, uint32_t *found, uint32_t *length);

#ifdef __cplusplus
}
#endif

#endif /* EXT4_JOURNAL_H_ */

/**
 * @}
 */
