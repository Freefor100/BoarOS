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
 * @file  ext4_blockdev.c
 * @brief Block device module.
 */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_debug.h>

#include <ext4_blockdev.h>
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <kernel/cost.h>

#include <string.h>
#include <stdlib.h>

static void ext4_bdif_lock(struct ext4_blockdev *bdev)
{
	if (!bdev->bdif->lock)
		return;

	int r = bdev->bdif->lock(bdev);
	ext4_assert(r == EOK);
}

static void ext4_bdif_unlock(struct ext4_blockdev *bdev)
{
	if (!bdev->bdif->unlock)
		return;

	int r = bdev->bdif->unlock(bdev);
	ext4_assert(r == EOK);
}

static int ext4_bdif_bread(struct ext4_blockdev *bdev, void *buf,
			   uint64_t blk_id, uint32_t blk_cnt)
{
	ext4_bdif_lock(bdev);
	int r = bdev->bdif->bread(bdev, buf, blk_id, blk_cnt);
	__atomic_fetch_add(&bdev->bdif->bread_ctr,1,__ATOMIC_RELAXED);
	ext4_bdif_unlock(bdev);
	return r;
}

static int ext4_bdif_bwrite(struct ext4_blockdev *bdev, const void *buf,
			    uint64_t blk_id, uint32_t blk_cnt)
{
	ext4_bdif_lock(bdev);
	int r = bdev->bdif->bwrite(bdev, buf, blk_id, blk_cnt);
	__atomic_fetch_add(&bdev->bdif->bwrite_ctr,1,__ATOMIC_RELAXED);
	ext4_bdif_unlock(bdev);
	return r;
}

int ext4_blockdev_flush(struct ext4_blockdev *bdev)
{
	int r;
	if (!bdev->bdif->flush)
		return ENOTSUP;
	ext4_bdif_lock(bdev);
	r = bdev->bdif->flush(bdev);
	ext4_bdif_unlock(bdev);
	return r;
}

int ext4_block_init(struct ext4_blockdev *bdev)
{
	int rc;
	ext4_assert(bdev);
	ext4_assert(bdev->bdif);
	ext4_assert(bdev->bdif->open &&
		   bdev->bdif->close &&
		   bdev->bdif->bread &&
		   bdev->bdif->bwrite);

	if (bdev->bdif->ph_refctr) {
		bdev->bdif->ph_refctr++;
		return EOK;
	}

	/*Low level block init*/
	rc = bdev->bdif->open(bdev);
	if (rc != EOK)
		return rc;

	bdev->bdif->ph_refctr = 1;
	return EOK;
}

int ext4_block_bind_bcache(struct ext4_blockdev *bdev, struct ext4_bcache *bc)
{
	ext4_assert(bdev && bc);
	bdev->bc = bc;
	bc->bdev = bdev;
	return EOK;
}

void ext4_block_set_lb_size(struct ext4_blockdev *bdev, uint32_t lb_bsize)
{
	/*Logical block size has to be multiply of physical */
	ext4_assert(!(lb_bsize % bdev->bdif->ph_bsize));

	bdev->lg_bsize = lb_bsize;
	bdev->lg_bcnt = bdev->part_size / lb_bsize;
}

int ext4_block_fini(struct ext4_blockdev *bdev)
{
	ext4_assert(bdev);

	if (!bdev->bdif->ph_refctr)
		return EOK;

	bdev->bdif->ph_refctr--;
	if (bdev->bdif->ph_refctr)
		return EOK;

	/*Low level block fini*/
	return bdev->bdif->close(bdev);
}

static void ext4_block_end_write(struct ext4_bcache *bc, struct ext4_buf *buf, int error,
    void (*complete)(struct ext4_bcache *, struct ext4_buf *, int, void *), void *argument)
{
    if (!complete) return;
    bool previous;
    { KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);previous=bc->dont_shake;bc->dont_shake=true; }
    complete(bc,buf,error,argument);
    { KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);bc->dont_shake=previous; }
}

int ext4_block_flush_buf(struct ext4_blockdev *bdev, struct ext4_buf *buf)
{
    struct ext4_bcache *bc=bdev->bc;
    struct ext4_block_payload *payload;
    void (*complete)(struct ext4_bcache *, struct ext4_buf *, int, void *);
    void *argument;
    {
        KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);
        if (!buf->refctr || buf->bc!=bc) __builtin_trap();
        if (buf->journal_pending) return EBUSY;
        if (!ext4_bcache_test_flag(buf,BC_DIRTY) || !ext4_bcache_test_flag(buf,BC_UPTODATE)) return EOK;
        payload=buf->current_payload;ext4_block_payload_get(payload);
        complete=buf->end_write;argument=buf->end_write_arg;
    }
    int result=ext4_blocks_set_direct(bdev,payload->data,buf->lba,1);
    if (!result) {
        KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);
        /* A later publication still needs its own write, even when this older
         * payload completed successfully while its device loan stayed alive. */
        if (buf->current_payload==payload) {
            if (buf->on_dirty_list) {
                SLIST_REMOVE(&bc->dirty_list,buf,ext4_buf,dirty_node);
                buf->on_dirty_list=false;
            }
            ext4_bcache_clear_flag(buf,BC_DIRTY);
        }
    }
    ext4_block_end_write(bc,buf,result,complete,argument);
    ext4_block_payload_put(payload);
    return result;
}

int ext4_block_flush_lba(struct ext4_blockdev *bdev, uint64_t lba)
{
	int r = EOK;
	struct ext4_buf *buf;
	struct ext4_block b;
	buf = ext4_bcache_find_get(bdev->bc, &b, lba, NULL);
	if (buf) {
		r = ext4_block_flush_buf(bdev, buf);
		ext4_bcache_free(bdev->bc, &b);
	}
	return r;
}

int ext4_block_cache_shake(struct ext4_blockdev *bdev)
{ return ext4_bcache_shake(bdev->bc); }

static int block_get_reference(struct ext4_blockdev *bdev, struct ext4_block *b,
    uint64_t lba, uint32_t offset, uint32_t length, enum ext4_block_access access,
    enum ext4_bcache_read_state *read, bool *private)
{
	int r;

	ext4_assert(bdev && b);

	if (!bdev->bdif->ph_refctr)
		return EIO;

	if (!(lba < bdev->lg_bcnt))
		return ENXIO;

	b->lb_id = lba;
	if (private) *private = false;
#if CONFIG_JOURNALING_ENABLE
    if (bdev->fs) {
        struct jbd_trans *trans = ext4_current_trans(bdev->fs);
        if (trans) {
            bool found;
            r = jbd_trans_borrow_private(trans, b, lba, offset, length, access, &found);
            if (found) {
                COST_ADD(BLOCK_CACHE_HITS, 1);
                if (!r) {
                    if (read) *read = EXT4_BCACHE_READ_READY;
                    if (private) *private = true;
                }
                return r;
            }
        }
    }
#else
    (void)offset; (void)length; (void)access;
#endif

	/* 命中先取得引用：无需新分配的访问不能因回收而淘汰自身。 */
	if (ext4_bcache_find_get(bdev->bc, b, lba, read)) {
		COST_ADD(BLOCK_CACHE_HITS, 1);
		return EOK;
	}
	COST_ADD(BLOCK_CACHE_MISSES, 1);

	r = ext4_block_cache_shake(bdev);
	if (r != EOK)
		return r;

	/* 回收可能等待I/O；allocator重查期间被其他任务加载的同一块。 */
	r = ext4_bcache_alloc(bdev->bc, b, read);
	if (r != EOK)
		return r;

	if (!b->data)
		return ENOMEM;

	return EOK;
}

int ext4_block_get_noread(struct ext4_blockdev *bdev, struct ext4_block *b,
    uint64_t lba)
{
    return block_get_reference(bdev,b,lba,0,bdev->lg_bsize,EXT4_BLOCK_READ,NULL,NULL);
}
static void ext4_block_loaded(struct ext4_blockdev *bdev, struct ext4_buf *buf, int error)
{
    KERNEL_TASK_LOCK_SCOPE(guard,&buf->bc->metadata);
    if (!buf->loading || !buf->refctr) __builtin_trap();
    buf->load_error=error;
    if (!error) ext4_bcache_set_flag(buf,BC_UPTODATE);
    buf->loading=false;
    /* Notification borrows the buffer through the producer reference. */
    if (bdev->bdif->wake_read) bdev->bdif->wake_read(buf);
}
static int ext4_block_wait_loaded(struct ext4_blockdev *bdev, struct ext4_buf *buf)
{
    for (;;) {
        {
            KERNEL_TASK_LOCK_SCOPE(guard,&buf->bc->metadata);
            if (!buf->loading) return buf->load_error;
            if (!bdev->bdif->wait_read) return EBUSY;
        }
        COST_ADD(BLOCK_CACHE_LOAD_WAITS,1);
        /* The callback checks loading again and registers under metadata. */
        bdev->bdif->wait_read(buf);
    }
}
int ext4_block_get_range(struct ext4_blockdev *bdev, struct ext4_block *b,
    uint64_t lba, uint32_t offset, uint32_t length, enum ext4_block_access access)
{
    if (offset > bdev->lg_bsize || !length || length > bdev->lg_bsize - offset) return EINVAL;
    enum ext4_bcache_read_state read;
    bool private;
    int r=block_get_reference(bdev,b,lba,offset,length,access,&read,&private);
    if (r) return r;
    if (private) return EOK;
    if (read==EXT4_BCACHE_READ_LOAD) {
        r=ext4_blocks_get_direct(bdev,b->data,lba,1);
        ext4_block_loaded(bdev,b->buf,r);
    } else if (read==EXT4_BCACHE_READ_WAIT) r=ext4_block_wait_loaded(bdev,b->buf);
    if (r) ext4_bcache_free_read(bdev->bc,b);
#if CONFIG_JOURNALING_ENABLE
    else if (bdev->fs) {
        struct jbd_trans *trans=ext4_current_trans(bdev->fs);
        if (trans) {
            if (access == EXT4_BLOCK_WRITE)
                r=jbd_trans_get_write_access_range(trans,b,offset,length);
        }
    }
#else
    (void)access;
#endif
    return r;
}
int ext4_block_get(struct ext4_blockdev *bdev, struct ext4_block *b, uint64_t lba)
{ return ext4_block_get_range(bdev,b,lba,0,bdev->lg_bsize,EXT4_BLOCK_READ); }

static void ext4_bread_completed(void *context, unsigned index, const struct block_read_span *span)
{
    const struct block_read_completion *completion = context;
    struct block_read_span result = *span;
    if (!result.status && result.completed != result.size) result.status = EIO;
    completion->complete(completion->context, index, &result);
}

int ext4_blockdev_read_batch(struct ext4_blockdev *bdev,
	struct block_read_span *spans, unsigned count, const struct block_read_completion *completion)
{
	if (!bdev || !bdev->bdif || !bdev->bdif->ph_refctr || count > EXT4_BLOCK_BATCH_MAX ||
	    (count && !spans)) return EINVAL;
	uint32_t sector = bdev->bdif->ph_bsize;
	if (!sector) return EINVAL;
	for (unsigned i = 0; i < count; i++) {
		spans[i].completed = 0;
		spans[i].status = EIO;
	}
	for (unsigned i = 0; i < count; i++) {
		if (spans[i].offset % sector || spans[i].size % sector || spans[i].size / sector > UINT32_MAX) {
			for (unsigned j = 0; j < count; j++) spans[j].status = EINVAL;
			return EINVAL;
		}
	}
    const struct block_read_completion forward = {ext4_bread_completed, (void *)completion};
	if (count && bdev->bdif->bread_batch) {
		ext4_bdif_lock(bdev);
		bdev->bdif->bread_batch(bdev, spans, count, completion ? &forward : NULL);
		__atomic_fetch_add(&bdev->bdif->bread_ctr,count,__ATOMIC_RELAXED);
		ext4_bdif_unlock(bdev);
	} else for (unsigned i = 0; i < count; i++) {
		spans[i].status = ext4_bdif_bread(bdev, spans[i].buffer,
			spans[i].offset / sector, spans[i].size / sector);
		if (!spans[i].status) spans[i].completed = spans[i].size;
        if (completion && i + 1 < count) ext4_bread_completed((void *)completion, i, &spans[i]);
	}
	int first = EOK;
	for (unsigned i = 0; i < count; i++) {
		if (!spans[i].status && spans[i].completed != spans[i].size) spans[i].status = EIO;
		if (!first) first = spans[i].status;
	}
	return first;
}

int ext4_block_get_batch(struct ext4_blockdev *bdev, struct ext4_block *blocks,
	const uint64_t *lbas, int *errors, unsigned count)
{
	if (!bdev || !bdev->bdif || !bdev->bdif->ph_refctr ||
	    !bdev->lg_bsize || !bdev->bdif->ph_bsize ||
	    bdev->lg_bsize % bdev->bdif->ph_bsize || count > EXT4_BLOCK_BATCH_MAX ||
	    (count && (!blocks || !lbas || !errors))) return EINVAL;
	for (unsigned i = 0; i < count; i++) {
		blocks[i] = (struct ext4_block)EXT4_BLOCK_ZERO(); errors[i] = EOK;
	}
	for (unsigned i = 0; i < count; i++) {
		if (lbas[i] >= bdev->lg_bcnt ||
		    lbas[i] > (UINT64_MAX - bdev->part_offset) / bdev->lg_bsize) {
			for (unsigned j = 0; j < count; j++) errors[j] = EINVAL;
			return EINVAL;
		}
	}
	struct block_read_span reads[EXT4_BLOCK_BATCH_MAX];
	unsigned owners[EXT4_BLOCK_BATCH_MAX], pending = 0;
	uint32_t waiters=0;
	for (unsigned i = 0; i < count; i++) {
		enum ext4_bcache_read_state read;
		errors[i] = block_get_reference(bdev, &blocks[i], lbas[i], 0, bdev->lg_bsize,
            EXT4_BLOCK_READ, &read, NULL);
		if (errors[i]) continue;
		if (read==EXT4_BCACHE_READ_WAIT) { waiters|=UINT32_C(1)<<i;continue; }
		if (read==EXT4_BCACHE_READ_READY) continue;
		owners[pending] = i;
		reads[pending++] = (struct block_read_span){lbas[i] * bdev->lg_bsize + bdev->part_offset,
			blocks[i].data, bdev->lg_bsize, 0, EIO};
	}
	/* 先完成自己发布的加载，再等待别人的 loading，重叠批次不会互等未发布页。 */
	ext4_blockdev_read_batch(bdev, reads, pending, NULL);
    for (unsigned i=0;i<pending;i++) {
        unsigned owner=owners[i];
        errors[owner]=reads[i].status;
        ext4_block_loaded(bdev,blocks[owner].buf,errors[owner]);
        if (errors[owner]) ext4_bcache_free_read(bdev->bc,&blocks[owner]);
    }
	int first = EOK;
	for (unsigned i = 0; i < count; i++) {
		if (!errors[i] && (waiters & (UINT32_C(1)<<i))) {
            errors[i]=ext4_block_wait_loaded(bdev,blocks[i].buf);
            if (errors[i]) ext4_bcache_free_read(bdev->bc,&blocks[i]);
		}
		if (!first) first = errors[i];
	}
	return first;
}

int ext4_block_set(struct ext4_blockdev *bdev, struct ext4_block *b)
{
	ext4_assert(bdev && b);
	ext4_assert(b->buf);

	if (!bdev->bdif->ph_refctr)
		return EIO;

	return ext4_bcache_free(bdev->bc, b);
}

int ext4_blocks_get_direct(struct ext4_blockdev *bdev, void *buf, uint64_t lba,
			   uint32_t cnt)
{
	uint64_t pba;
	uint32_t pb_cnt;

	ext4_assert(bdev && buf);

	pba = (lba * bdev->lg_bsize + bdev->part_offset) / bdev->bdif->ph_bsize;
	pb_cnt = bdev->lg_bsize / bdev->bdif->ph_bsize;

	return ext4_bdif_bread(bdev, buf, pba, pb_cnt * cnt);
}

int ext4_blocks_set_direct(struct ext4_blockdev *bdev, const void *buf,
			   uint64_t lba, uint32_t cnt)
{
	uint64_t pba;
	uint32_t pb_cnt;

	ext4_assert(bdev && buf);

	pba = (lba * bdev->lg_bsize + bdev->part_offset) / bdev->bdif->ph_bsize;
	pb_cnt = bdev->lg_bsize / bdev->bdif->ph_bsize;

	return ext4_bdif_bwrite(bdev, buf, pba, pb_cnt * cnt);
}

int ext4_blocks_set_batch(struct ext4_blockdev *bdev,
	struct kernel_block_span *spans, unsigned count)
{
	if (!bdev || !bdev->bdif || count > EXT4_BLOCK_BATCH_MAX ||
	    (count && !spans) || !bdev->bdif->ph_bsize || !bdev->lg_bsize ||
	    bdev->lg_bsize % bdev->bdif->ph_bsize ||
	    bdev->part_offset % bdev->bdif->ph_bsize) return EINVAL;
	for (unsigned i = 0; i < count; i++) {
		if (!spans[i].buffer || !spans[i].size ||
		    spans[i].offset % bdev->lg_bsize || spans[i].size % bdev->lg_bsize ||
		    spans[i].offset / bdev->lg_bsize > bdev->lg_bcnt ||
		    spans[i].size / bdev->lg_bsize > bdev->lg_bcnt - spans[i].offset / bdev->lg_bsize ||
		    spans[i].offset > bdev->part_size || spans[i].size > bdev->part_size - spans[i].offset ||
		    spans[i].size / bdev->bdif->ph_bsize > UINT32_MAX ||
		    spans[i].offset > UINT64_MAX - bdev->part_offset ||
		    spans[i].size > UINT64_MAX - bdev->part_offset - spans[i].offset)
			return EINVAL;
		for (unsigned j = 0; j < i; j++)
			if (spans[i].offset < spans[j].offset + spans[j].size &&
			    spans[j].offset < spans[i].offset + spans[i].size) return EINVAL;
	}
	if (!count) return EOK;
	if (!bdev->bdif->bwrite_batch) {
		for (unsigned i = 0; i < count; i++) {
			int r = ext4_bdif_bwrite(bdev, spans[i].buffer,
				(spans[i].offset + bdev->part_offset) / bdev->bdif->ph_bsize,
				spans[i].size / bdev->bdif->ph_bsize);
			if (r != EOK) return r;
		}
		return EOK;
	}
	/* Caller planning storage stays borrowed through DMA drain. Restore its
	 * filesystem-relative offsets before cache updates and failed-map cleanup. */
	for (unsigned i = 0; i < count; i++) spans[i].offset += bdev->part_offset;
	ext4_bdif_lock(bdev);
	int r = bdev->bdif->bwrite_batch(bdev, spans, count);
	__atomic_fetch_add(&bdev->bdif->bwrite_ctr,count,__ATOMIC_RELAXED);
	ext4_bdif_unlock(bdev);
	for (unsigned i = 0; i < count; i++) spans[i].offset -= bdev->part_offset;
	return r;
}

int ext4_blocks_set_phase(struct ext4_blockdev *bdev,
	const struct ext4_block_write *first)
{
	if (!bdev || !bdev->bdif || !bdev->bdif->ph_bsize) return EINVAL;
	uint32_t sector = bdev->bdif->ph_bsize;
	for (const struct ext4_block_write *io = first; io; io = io->next) {
		if (!io->data || !io->size || io->offset % sector || io->size % sector ||
		    io->offset < bdev->part_offset || io->offset - bdev->part_offset > bdev->part_size ||
		    io->size > bdev->part_size - (io->offset - bdev->part_offset) ||
		    io->size - 1 > UINTPTR_MAX - (uintptr_t)io->data) return EINVAL;
	}
	if (!first) return EOK;
	ext4_bdif_lock(bdev);
	int r = EOK;
	if (bdev->bdif->bwrite_phase) r = bdev->bdif->bwrite_phase(bdev, first);
	else for (const struct ext4_block_write *io = first; io; io = io->next) {
		/* Legacy devices have no in-flight phase producer. */
		const uint8_t *data = io->data;
		uint64_t block = io->offset / sector;
		size_t left = io->size / sector;
		while (left) {
			uint32_t count = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
			r = bdev->bdif->bwrite(bdev, data, block, count);
			if (r != EOK) break;
			block += count; data += (size_t)count * sector; left -= count;
		}
		if (r != EOK) break;
	}
	for (const struct ext4_block_write *io = first; io; io = io->next) __atomic_fetch_add(&bdev->bdif->bwrite_ctr,1,__ATOMIC_RELAXED);
	ext4_bdif_unlock(bdev);
	return r;
}

int ext4_block_writebytes(struct ext4_blockdev *bdev, uint64_t off,
			  const void *buf, uint32_t len)
{
	uint64_t block_idx;
	uint32_t blen;
	uint32_t unalg;
	int r = EOK;

	const uint8_t *p = (void *)buf;

	ext4_assert(bdev && buf);

	if (!bdev->bdif->ph_refctr)
		return EIO;

	if (off + len > bdev->part_size)
		return EINVAL; /*Ups. Out of range operation*/

	block_idx = ((off + bdev->part_offset) / bdev->bdif->ph_bsize);

	/*OK lets deal with the first possible unaligned block*/
	unalg = (off & (bdev->bdif->ph_bsize - 1));
	if (unalg) {

		uint32_t wlen = (bdev->bdif->ph_bsize - unalg) > len
				    ? len
				    : (bdev->bdif->ph_bsize - unalg);

		r = ext4_bdif_bread(bdev, bdev->bdif->ph_bbuf, block_idx, 1);
		if (r != EOK)
			return r;

		memcpy(bdev->bdif->ph_bbuf + unalg, p, wlen);
		r = ext4_bdif_bwrite(bdev, bdev->bdif->ph_bbuf, block_idx, 1);
		if (r != EOK)
			return r;

		p += wlen;
		len -= wlen;
		block_idx++;
	}

	/*Aligned data*/
	blen = len / bdev->bdif->ph_bsize;
	if (blen != 0) {
		r = ext4_bdif_bwrite(bdev, p, block_idx, blen);
		if (r != EOK)
			return r;

		p += bdev->bdif->ph_bsize * blen;
		len -= bdev->bdif->ph_bsize * blen;

		block_idx += blen;
	}

	/*Rest of the data*/
	if (len) {
		r = ext4_bdif_bread(bdev, bdev->bdif->ph_bbuf, block_idx, 1);
		if (r != EOK)
			return r;

		memcpy(bdev->bdif->ph_bbuf, p, len);
		r = ext4_bdif_bwrite(bdev, bdev->bdif->ph_bbuf, block_idx, 1);
		if (r != EOK)
			return r;
	}

	return r;
}

int ext4_block_readbytes(struct ext4_blockdev *bdev, uint64_t off, void *buf,
			 uint32_t len)
{
	uint64_t block_idx;
	uint32_t blen;
	uint32_t unalg;
	int r = EOK;

	uint8_t *p = (void *)buf;

	ext4_assert(bdev && buf);

	if (!bdev->bdif->ph_refctr)
		return EIO;

	if (off + len > bdev->part_size)
		return EINVAL; /*Ups. Out of range operation*/

	block_idx = ((off + bdev->part_offset) / bdev->bdif->ph_bsize);

	/*OK lets deal with the first possible unaligned block*/
	unalg = (off & (bdev->bdif->ph_bsize - 1));
	if (unalg) {

		uint32_t rlen = (bdev->bdif->ph_bsize - unalg) > len
				    ? len
				    : (bdev->bdif->ph_bsize - unalg);

		r = ext4_bdif_bread(bdev, bdev->bdif->ph_bbuf, block_idx, 1);
		if (r != EOK)
			return r;

		memcpy(p, bdev->bdif->ph_bbuf + unalg, rlen);

		p += rlen;
		len -= rlen;
		block_idx++;
	}

	/*Aligned data*/
	blen = len / bdev->bdif->ph_bsize;

	if (blen != 0) {
		r = ext4_bdif_bread(bdev, p, block_idx, blen);
		if (r != EOK)
			return r;

		p += bdev->bdif->ph_bsize * blen;
		len -= bdev->bdif->ph_bsize * blen;

		block_idx += blen;
	}

	/*Rest of the data*/
	if (len) {
		r = ext4_bdif_bread(bdev, bdev->bdif->ph_bbuf, block_idx, 1);
		if (r != EOK)
			return r;

		memcpy(p, bdev->bdif->ph_bbuf, len);
	}

	return r;
}

int ext4_block_cache_flush(struct ext4_blockdev *bdev)
{
    for (;;) {
        struct ext4_block borrowed;
        struct ext4_buf *buf=ext4_bcache_dirty_get(bdev->bc,&borrowed);
        if (!buf) return EOK;
        int r=ext4_block_flush_buf(bdev,buf);
        ext4_bcache_free_read(bdev->bc,&borrowed);
        if (r) return r;
    }
}

int ext4_block_cache_write_back(struct ext4_blockdev *bdev, uint8_t on_off)
{
	if (on_off)
		bdev->cache_write_back++;

	if (!on_off && bdev->cache_write_back)
		bdev->cache_write_back--;

	if (bdev->cache_write_back)
		return EOK;

	/*Flush data in all delayed cache blocks*/
	return ext4_block_cache_flush(bdev);
}

/**
 * @}
 */
