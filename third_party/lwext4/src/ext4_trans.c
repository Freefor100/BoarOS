/*
 * Copyright (c) 2015 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 * Copyright (c) 2015 Kaho Ng (ngkaho1234@gmail.com)
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
 * @file  ext4_trans.c
 * @brief Ext4 transaction buffer operations.
 */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_debug.h>

#include <ext4_fs.h>
#include <ext4_journal.h>
#include <ext4_trans.h>

static int trans_error(struct ext4_blockdev *bdev, int r)
{
#if CONFIG_JOURNALING_ENABLE
	if (r && bdev->fs && bdev->fs->curr_trans &&
	    !bdev->fs->curr_trans->error)
		bdev->fs->curr_trans->error = r;
#else
	(void)bdev;
#endif
	return r;
}

int ext4_trans_set_block_dirty(struct ext4_buf *buf)
{
	int r = EOK;
#if CONFIG_JOURNALING_ENABLE
	struct ext4_fs *fs = buf->bc->bdev->fs;
	struct ext4_block block = {
		.lb_id = buf->lba,
		.data = buf->data,
		.buf = buf
	};

	if (fs && fs->jbd_journal && fs->jbd_journal->error)
		return trans_error(buf->bc->bdev, fs->jbd_journal->error);
	if (fs && fs->jbd_journal && fs->curr_trans) {
		struct jbd_trans *trans = fs->curr_trans;
		return trans_error(buf->bc->bdev,
			jbd_trans_set_block_dirty(trans, &block));
	}
#endif
	ext4_bcache_set_dirty(buf);
	return r;
}

int ext4_trans_block_get_noread(struct ext4_blockdev *bdev,
			  struct ext4_block *b,
			  uint64_t lba)
{
#if CONFIG_JOURNALING_ENABLE
	/* Rollback must also restore any existing held reference. Read the old
	 * block before handing writable storage to a transaction. */
	if (bdev->fs && bdev->fs->curr_trans)
		return ext4_trans_block_get(bdev, b, lba);
#endif
	int r = ext4_block_get_noread(bdev, b, lba);
	if (r != EOK)
		return r;

	return r;
}

int ext4_trans_block_get(struct ext4_blockdev *bdev,
		   struct ext4_block *b,
		   uint64_t lba)
{
	int r = ext4_block_get(bdev, b, lba);
	if (r != EOK)
		return trans_error(bdev, r);

#if CONFIG_JOURNALING_ENABLE
	if (bdev->fs && bdev->fs->jbd_journal && bdev->fs->curr_trans) {
		r = jbd_trans_get_write_access(bdev->fs->curr_trans, b);
		if (r != EOK) {
			bdev->cache_write_back++;
			ext4_block_set(bdev, b);
			bdev->cache_write_back--;
			return trans_error(bdev, r);
		}
	}
#endif

	return trans_error(bdev, r);
}

int ext4_trans_data_get(struct ext4_blockdev *bdev, struct ext4_block *b,
			uint64_t lba)
{
	int r = ext4_block_get(bdev, b, lba);
	if (r != EOK)
		return trans_error(bdev, r);
#if CONFIG_JOURNALING_ENABLE
	if (bdev->fs && bdev->fs->jbd_journal && bdev->fs->curr_trans) {
		r = jbd_trans_get_data_access(bdev->fs->curr_trans, b);
		if (r != EOK) {
			bdev->cache_write_back++;
			ext4_block_set(bdev, b);
			bdev->cache_write_back--;
		}
	}
#endif
	return trans_error(bdev, r);
}

int ext4_trans_data_get_noread(struct ext4_blockdev *bdev, struct ext4_block *b,
			       uint64_t lba)
{
#if CONFIG_JOURNALING_ENABLE
	if (bdev->fs && bdev->fs->jbd_journal && bdev->fs->curr_trans)
		return ext4_trans_data_get(bdev, b, lba);
#endif
	return trans_error(bdev, ext4_block_get_noread(bdev, b, lba));
}

int ext4_trans_set_data_dirty(struct ext4_buf *buf)
{
#if CONFIG_JOURNALING_ENABLE
	struct ext4_fs *fs = buf->bc->bdev->fs;
	if (fs && fs->jbd_journal && fs->jbd_journal->error)
		return trans_error(buf->bc->bdev, fs->jbd_journal->error);
	if (fs && fs->jbd_journal && fs->curr_trans) {
		struct ext4_block block = {
			.lb_id = buf->lba, .data = buf->data, .buf = buf
		};
		return trans_error(buf->bc->bdev,
			jbd_trans_set_data_dirty(fs->curr_trans, &block));
	}
#endif
	ext4_bcache_set_dirty(buf);
	return EOK;
}

int ext4_trans_try_revoke_block(struct ext4_blockdev *bdev __unused,
			        uint64_t lba __unused)
{
	int r = EOK;
#if CONFIG_JOURNALING_ENABLE
	struct ext4_fs *fs = bdev->fs;
	if (fs->jbd_journal && fs->curr_trans) {
		struct jbd_trans *trans = fs->curr_trans;
		r = jbd_trans_try_revoke_block(trans, lba);
	} else if (fs->jbd_journal) {
		r = ext4_block_flush_lba(fs->bdev, lba);
	}
#endif
	return trans_error(bdev, r);
}

/**
 * @}
 */
