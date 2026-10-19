/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
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
 * @file  ext4_bcache.h
 * @brief Block cache allocator.
 */

#ifndef EXT4_BCACHE_H_
#define EXT4_BCACHE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <ext4_config.h>

#include <stdint.h>
#include <stdbool.h>
#include <misc/tree.h>
#include <misc/queue.h>
#include <kernel/raw_lock.h>

#define EXT4_BLOCK_ZERO() 	\
	{.lb_id = 0, .data = 0}

/**@brief   Single block descriptor*/
struct ext4_block {
	/**@brief   Logical block ID*/
	uint64_t lb_id;

	/**@brief   Buffer */
	struct ext4_buf *buf;

	/**@brief   Data buffer.*/
	uint8_t *data;
};

struct ext4_bcache;
struct ext4_writeback_scope {
	struct ext4_buf *head;
	struct ext4_writeback_scope *previous;
};

/**@brief   Single block descriptor*/
struct ext4_buf {
	struct ext4_writeback_scope *writeback_scope;
	struct ext4_buf *writeback_next;
	/**@brief   Flags*/
	int flags;
	/* Uncommitted journal owners forbid implicit home writeback. */
	unsigned journal_pending;
	bool loading;
	int load_error;

	/**@brief   Logical block address*/
	uint64_t lba;

	/**@brief   Data buffer.*/
	uint8_t *data;

	/**@brief   Reference count table*/
	uint32_t refctr;

	/**@brief   The block cache this buffer belongs to. */
	struct ext4_bcache *bc;

	/**@brief   Whether or not buffer is on dirty list.*/
	bool on_dirty_list;

	/**@brief   LBA tree node*/
	RB_ENTRY(ext4_buf) lba_node;

	/* Unreferenced buffers, oldest at the cache head. */
	struct ext4_buf *lru_previous, *lru_next;

	/**@brief   Dirty list node*/
	SLIST_ENTRY(ext4_buf) dirty_node;

	/**@brief   Callback routine after a disk-write operation.
	 * @param   bc block cache descriptor
	 * @param   buf buffer descriptor
	 * @param   standard error code returned by bdev->bwrite()
	 * @param   arg argument passed to this routine*/
	void (*end_write)(struct ext4_bcache *bc,
			  struct ext4_buf *buf,
			  int res,
			  void *arg);

	/**@brief   argument passed to end_write() callback.*/
	void *end_write_arg;
};

/**@brief   Block cache descriptor*/
struct ext4_bcache {
	/* Index, LRU, references and load publication; no allocation/I/O inside. */
	struct kernel_raw_lock metadata;
	struct ext4_writeback_scope *writeback_scope;

	/**@brief   Item count in block cache*/
	uint32_t cnt;

	/**@brief   Item size in block cache*/
	uint32_t itemsize;

	/**@brief   Currently referenced datablocks*/
	uint32_t ref_blocks;

	/**@brief   Maximum referenced datablocks*/
	uint32_t max_ref_blocks;

	/**@brief   The blockdev binded to this block cache*/
	struct ext4_blockdev *bdev;

	/**@brief   The cache should not be shaked */
	bool dont_shake;

	/**@brief   A tree holding all bufs*/
	RB_HEAD(ext4_buf_lba, ext4_buf) lba_root;

	/**@brief   Unreferenced buffers in idle age order. */
	struct ext4_buf *lru_head, *lru_tail;

	/**@brief   A singly-linked list holding dirty buffers*/
	SLIST_HEAD(ext4_buf_dirty, ext4_buf) dirty_list;
};

/**@brief buffer state bits
 *
 *  - BC♡UPTODATE: Buffer contains valid data.
 *  - BC_DIRTY: Buffer is dirty.
 *  - BC_FLUSH: Buffer will be immediately flushed,
 *              when no one references it.
 *  - BC_TMP: Buffer will be dropped once its refctr
 *            reaches zero.
 */
enum bcache_state_bits {
	BC_UPTODATE,
	BC_DIRTY,
	BC_FLUSH,
	BC_TMP
};

/* Taken with the reference under metadata; WAIT also observes a saved error. */
enum ext4_bcache_read_state {
	EXT4_BCACHE_READ_READY,
	EXT4_BCACHE_READ_LOAD,
	EXT4_BCACHE_READ_WAIT,
};

#define ext4_bcache_set_flag(buf, b) \
    ((void)__atomic_fetch_or(&(buf)->flags, 1 << (b), __ATOMIC_RELAXED))
#define ext4_bcache_clear_flag(buf, b) \
    ((void)__atomic_fetch_and(&(buf)->flags, ~(1 << (b)), __ATOMIC_RELAXED))
#define ext4_bcache_test_flag(buf, b) \
    ((__atomic_load_n(&(buf)->flags, __ATOMIC_RELAXED) >> (b)) & 1)

void ext4_bcache_scope_track(struct ext4_buf *buf);
void ext4_bcache_scope_begin(struct ext4_bcache *bc,
                             struct ext4_writeback_scope *scope);
int ext4_bcache_scope_end(struct ext4_bcache *bc,
                          struct ext4_writeback_scope *scope);

static inline void ext4_bcache_set_dirty(struct ext4_buf *buf) {
	ext4_bcache_scope_track(buf);
	ext4_bcache_set_flag(buf, BC_UPTODATE);
	ext4_bcache_set_flag(buf, BC_DIRTY);
}

static inline void ext4_bcache_clear_dirty(struct ext4_buf *buf) {
	ext4_bcache_clear_flag(buf, BC_UPTODATE);
	ext4_bcache_clear_flag(buf, BC_DIRTY);
}

/* The caller already owns a reference; journal/scope pins share the index lock. */
void ext4_bcache_inc_ref(struct ext4_buf *buf);
void ext4_bcache_insert_dirty_node(struct ext4_bcache *, struct ext4_buf *);
void ext4_bcache_remove_dirty_node(struct ext4_bcache *, struct ext4_buf *);
/* Select and detach clean victims atomically, flushing only in a write context. */
int ext4_bcache_shake(struct ext4_bcache *);
int ext4_bcache_free_read(struct ext4_bcache *, struct ext4_block *);
/* Caller owns the returned dirty buffer across lock-free device I/O. */
struct ext4_buf *ext4_bcache_dirty_get(struct ext4_bcache *, struct ext4_block *);

/**@brief   Dynamic initialization of block cache.
 * @param   bc block cache descriptor
 * @param   cnt items count in block cache
 * @param   itemsize single item size (in bytes)
 * @return  standard error code*/
int ext4_bcache_init_dynamic(struct ext4_bcache *bc, uint32_t cnt,
			     uint32_t itemsize);

/**@brief   Do cleanup works on block cache.
 * @param   bc block cache descriptor.*/
void ext4_bcache_cleanup(struct ext4_bcache *bc);

/**@brief   Dynamic de-initialization of block cache.
 * @param   bc block cache descriptor
 * @return  standard error code*/
int ext4_bcache_fini_dynamic(struct ext4_bcache *bc);

/**@brief   Invalidate a buffer.
 * @param   bc block cache descriptor
 * @param   buf buffer*/
void ext4_bcache_invalidate_buf(struct ext4_bcache *bc,
				struct ext4_buf *buf);

/**@brief   Invalidate a range of buffers.
 * @param   bc block cache descriptor
 * @param   from starting lba
 * @param   cnt block counts*/
void ext4_bcache_invalidate_lba(struct ext4_bcache *bc,
				uint64_t from,
				uint32_t cnt);

/**@brief   Find existing buffer from block cache memory.
 *          Unreferenced block allocation is based on LRU
 *          (Last Recently Used) algorithm.
 * @param   bc block cache descriptor
 * @param   b block to alloc
 * @param   lba logical block address
 * @param   read optional read decision; atomically claims the unique loader
 * @return  block cache buffer */
struct ext4_buf *
ext4_bcache_find_get(struct ext4_bcache *bc, struct ext4_block *b,
		     uint64_t lba, enum ext4_bcache_read_state *read);

/**@brief   Allocate block from block cache memory.
 *          Unreferenced block allocation is based on LRU
 *          (Last Recently Used) algorithm.
 * @param   bc block cache descriptor
 * @param   b block to alloc
 * @param   read optional read decision, taken with publication/reference
 * @return  standard error code*/
int ext4_bcache_alloc(struct ext4_bcache *bc, struct ext4_block *b,
		      enum ext4_bcache_read_state *read);

/**@brief   Free block from cache memory (decrement reference counter).
 * @param   bc block cache descriptor
 * @param   b block to free
 * @return  standard error code*/
int ext4_bcache_free(struct ext4_bcache *bc, struct ext4_block *b);

/**@brief   Return a full status of block cache.
 * @param   bc block cache descriptor
 * @return  full status*/


#ifdef __cplusplus
}
#endif

#endif /* EXT4_BCACHE_H_ */

/**
 * @}
 */
