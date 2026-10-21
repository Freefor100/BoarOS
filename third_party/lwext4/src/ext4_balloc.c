/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
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
 * @file  ext4_balloc.c
 * @brief Physical block allocator.
 */

/* BoarOS modification, 2026-08-27: use metadata checksum seeds. */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_debug.h>

#include <ext4_trans.h>
#include <ext4_balloc.h>
#include <ext4_super.h>
#include <ext4_crc32.h>
#include <ext4_block_group.h>
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <ext4_bitmap.h>
#include <ext4_inode.h>
#include <ext4_orphan.h>

/**@brief Compute number of block group from block address.
 * @param s superblock pointer.
 * @param baddr Absolute address of block.
 * @return Block group index
 */
uint32_t ext4_balloc_get_bgid_of_block(struct ext4_sblock *s,
				       uint64_t baddr)
{
	if (ext4_get32(s, first_data_block) && baddr)
		baddr--;

	return (uint32_t)(baddr / ext4_get32(s, blocks_per_group));
}

/**@brief Compute the starting block address of a block group
 * @param s   superblock pointer.
 * @param bgid block group index
 * @return Block address
 */
uint64_t ext4_balloc_get_block_of_bgid(struct ext4_sblock *s,
				       uint32_t bgid)
{
	uint64_t baddr = 0;
	if (ext4_get32(s, first_data_block))
		baddr++;

	baddr += bgid * ext4_get32(s, blocks_per_group);
	return baddr;
}

#if CONFIG_META_CSUM_ENABLE
static uint32_t ext4_balloc_bitmap_csum(struct ext4_sblock *sb,
					void *bitmap)
{
	uint32_t checksum = 0;
	if (ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM)) {
		uint32_t blocks_per_group = ext4_get32(sb, blocks_per_group);

		/* Start with the filesystem metadata checksum seed. */
		checksum = ext4_sb_get_csum_seed(sb);
		/* Then calculate crc32 checksum against block_group_desc */
		checksum = ext4_crc32c(checksum, bitmap, blocks_per_group / 8);
	}
	return checksum;
}
#else
#define ext4_balloc_bitmap_csum(...) 0
#endif

void ext4_balloc_set_bitmap_csum(struct ext4_sblock *sb,
				 struct ext4_bgroup *bg,
				 void *bitmap __unused)
{
	int desc_size = ext4_sb_get_desc_size(sb);
	uint32_t checksum = ext4_balloc_bitmap_csum(sb, bitmap);
	uint16_t lo_checksum = to_le16(checksum & 0xFFFF),
		 hi_checksum = to_le16(checksum >> 16);

	if (!ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM))
		return;

	/* See if we need to assign a 32bit checksum */
	bg->block_bitmap_csum_lo = lo_checksum;
	if (desc_size == EXT4_MAX_BLOCK_GROUP_DESCRIPTOR_SIZE)
		bg->block_bitmap_csum_hi = hi_checksum;

}

#if CONFIG_META_CSUM_ENABLE
static bool
ext4_balloc_verify_bitmap_csum(struct ext4_sblock *sb,
			       struct ext4_bgroup *bg,
			       void *bitmap __unused)
{
	int desc_size = ext4_sb_get_desc_size(sb);
	uint32_t checksum = ext4_balloc_bitmap_csum(sb, bitmap);
	uint16_t lo_checksum = to_le16(checksum & 0xFFFF),
		 hi_checksum = to_le16(checksum >> 16);

	if (!ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM))
		return true;

	if (bg->block_bitmap_csum_lo != lo_checksum)
		return false;

	if (desc_size == EXT4_MAX_BLOCK_GROUP_DESCRIPTOR_SIZE)
		if (bg->block_bitmap_csum_hi != hi_checksum)
			return false;

	return true;
}
#else
#define ext4_balloc_verify_bitmap_csum(...) true
#endif

static int ext4_balloc_prepare_free_groups(struct ext4_inode_ref *inode,
    uint32_t first, uint32_t last)
{
    struct ext4_fs *fs = inode->fs;
    struct ext4_operation *operation = ext4_current_operation(fs);
    if (!operation || !operation->orphan_metadata) return EOK;
    bool present;
    int result = ext4_orphan_contains(fs, inode->index, &present);
    if (result || !present) return result;
    uint32_t inode_group = (inode->index - 1) /
        ext4_get32(ext4_fs_super(fs), inodes_per_group);
    /* orphan 移除还要校验 inode 位图；此处已经知道实际释放的物理范围，
     * 将两者按组号合并取得，避免先拿任何一个高组后才请求已知的低组。
     * 后续范围若发现新的低组，仍由操作的完整撤销、排序重放处理。 */
    if (inode_group < first) {
        result = ext4_operation_require_resource(fs, EXT4_RESOURCE_BLOCK_GROUP, inode_group);
        if (result) return result;
    }
    for (uint32_t group = first; group <= last; group++) {
        result = ext4_operation_require_resource(fs, EXT4_RESOURCE_BLOCK_GROUP, group);
        if (result) return result;
    }
    return inode_group > last ?
        ext4_operation_require_resource(fs, EXT4_RESOURCE_BLOCK_GROUP, inode_group) : EOK;
}

int ext4_balloc_free_block(struct ext4_inode_ref *inode_ref, ext4_fsblk_t baddr)
{
	struct ext4_fs *fs = inode_ref->fs;
	struct ext4_sblock *sb = ext4_fs_super(fs);

	uint32_t bg_id = ext4_balloc_get_bgid_of_block(sb, baddr);
	uint32_t index_in_group = ext4_fs_addr_to_idx_bg(sb, baddr);
	int rc = ext4_balloc_prepare_free_groups(inode_ref, bg_id, bg_id);
	if (rc != EOK)
		return rc;

	/* Load block group reference */
	struct ext4_block_group_ref bg_ref;
	rc = ext4_fs_get_block_group_ref_write(fs, bg_id, &bg_ref);
	if (rc != EOK)
		return rc;
	rc = jbd_trans_quarantine(fs, baddr, 1, false);
	if (rc != EOK) { ext4_fs_put_block_group_ref(&bg_ref); return rc; }

	struct ext4_bgroup *bg = bg_ref.block_group;

	/* Load block with bitmap */
	ext4_fsblk_t bitmap_block_addr =
	    ext4_bg_get_block_bitmap(bg, sb);

	struct ext4_block bitmap_block;

	rc = ext4_trans_block_get(fs->bdev, &bitmap_block, bitmap_block_addr);
	if (rc != EOK) {
		ext4_fs_put_block_group_ref(&bg_ref);
		return rc;
	}

	if (!ext4_balloc_verify_bitmap_csum(sb, bg, bitmap_block.data)) {
		ext4_dbg(DEBUG_BALLOC,
			DBG_WARN "Bitmap checksum failed."
			"Group: %" PRIu32"\n",
			bg_ref.index);
		ext4_block_set(bg_ref.fs->bdev, &bitmap_block);
		ext4_fs_put_block_group_ref(&bg_ref);
		return EUCLEAN;
	}

	/* Modify bitmap */
	ext4_bmap_bit_clr(bitmap_block.data, index_in_group);
	ext4_balloc_set_bitmap_csum(sb, bg, bitmap_block.data);
	ext4_trans_set_block_dirty(bitmap_block.buf);

	/* Release block with bitmap */
	rc = ext4_block_set(fs->bdev, &bitmap_block);
	if (rc != EOK) {
		/* Error in saving bitmap */
		ext4_fs_put_block_group_ref(&bg_ref);
		return rc;
	}

	uint32_t block_size = ext4_sb_get_block_size(sb);

	/* Update superblock free blocks count */
    ext4_fs_account_blocks(fs, 1);

	/* Update inode blocks count */
	uint64_t ino_blocks = ext4_inode_get_blocks_count(sb, inode_ref->inode);
	ino_blocks -= block_size / EXT4_INODE_BLOCK_SIZE;
	ext4_inode_set_blocks_count(sb, inode_ref->inode, ino_blocks);
	inode_ref->dirty = true;

	/* Update block group free blocks count */
	uint32_t free_blocks = ext4_bg_get_free_blocks_count(bg, sb);
	free_blocks++;
	ext4_bg_set_free_blocks_count(bg, sb, free_blocks);

	bg_ref.dirty = true;

	rc = ext4_trans_try_revoke_block(fs->bdev, baddr);
	if (rc != EOK) {
		bg_ref.dirty = false;
		ext4_fs_put_block_group_ref(&bg_ref);
		return rc;
	}
	/* A journaled free stays private until acceptance and quarantined until
     * checkpoint. Abort must leave the accepted cache version untouched. */
    if (!fs->jbd_journal) ext4_bcache_invalidate_lba(fs->bdev->bc, baddr, 1);
	/* Release block group reference */
	rc = ext4_fs_put_block_group_ref(&bg_ref);

	return rc;
}

int ext4_balloc_free_blocks(struct ext4_inode_ref *inode_ref,
			    ext4_fsblk_t first, uint32_t count)
{
	int rc = EOK;
	uint32_t blk_cnt = count;
	ext4_fsblk_t start_block = first;
	struct ext4_fs *fs = inode_ref->fs;
	struct ext4_sblock *sb = ext4_fs_super(fs);

	uint32_t bg_first = ext4_balloc_get_bgid_of_block(sb, first);
	uint32_t bg_last = ext4_balloc_get_bgid_of_block(sb, first + count - 1);
	rc = ext4_balloc_prepare_free_groups(inode_ref, bg_first, bg_last);
	if (rc != EOK) return rc;
	int quarantine = jbd_trans_quarantine(fs, first, count, false);
    if (quarantine != EOK) return quarantine;

	if (!ext4_sb_feature_incom(sb, EXT4_FINCOM_FLEX_BG)) {
		/*It is not possible without flex_bg that blocks are continuous
		 * and and last block belongs to other bg.*/
		if (bg_last != bg_first) {
			ext4_dbg(DEBUG_BALLOC, DBG_WARN "FLEX_BG: disabled & "
				"bg_last: %"PRIu32" bg_first: %"PRIu32"\n",
				bg_last, bg_first);
		}
	}

	/* Load block group reference */
	struct ext4_block_group_ref bg_ref;
	while (bg_first <= bg_last) {
		rc = ext4_fs_get_block_group_ref_write(fs, bg_first, &bg_ref);
		if (rc != EOK)
			return rc;

		struct ext4_bgroup *bg = bg_ref.block_group;

		uint32_t idx_in_bg_first;
		idx_in_bg_first = ext4_fs_addr_to_idx_bg(sb, first);

		/* Load block with bitmap */
		ext4_fsblk_t bitmap_blk = ext4_bg_get_block_bitmap(bg, sb);

		struct ext4_block blk;
		rc = ext4_trans_block_get(fs->bdev, &blk, bitmap_blk);
		if (rc != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return rc;
		}

		if (!ext4_balloc_verify_bitmap_csum(sb, bg, blk.data)) {
			ext4_dbg(DEBUG_BALLOC,
				DBG_WARN "Bitmap checksum failed."
				"Group: %" PRIu32"\n",
				bg_ref.index);
			ext4_block_set(bg_ref.fs->bdev, &blk);
			ext4_fs_put_block_group_ref(&bg_ref);
			return EUCLEAN;
		}
		uint32_t free_cnt;
		free_cnt = ext4_sb_get_block_size(sb) * 8 - idx_in_bg_first;

		/*If last block, free only count blocks*/
		free_cnt = count > free_cnt ? free_cnt : count;

		/* Modify bitmap */
		ext4_bmap_bits_free(blk.data, idx_in_bg_first, free_cnt);
		ext4_balloc_set_bitmap_csum(sb, bg, blk.data);
		ext4_trans_set_block_dirty(blk.buf);

		count -= free_cnt;
		first += free_cnt;

		/* Release block with bitmap */
		rc = ext4_block_set(fs->bdev, &blk);
		if (rc != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return rc;
		}

		uint32_t block_size = ext4_sb_get_block_size(sb);

		/* Update superblock free blocks count */
        ext4_fs_account_blocks(fs, free_cnt);

		/* Update inode blocks count */
		uint64_t ino_blocks;
		ino_blocks = ext4_inode_get_blocks_count(sb, inode_ref->inode);
		ino_blocks -= free_cnt * (block_size / EXT4_INODE_BLOCK_SIZE);
		ext4_inode_set_blocks_count(sb, inode_ref->inode, ino_blocks);
		inode_ref->dirty = true;

		/* Update block group free blocks count */
		uint32_t free_blocks;
		free_blocks = ext4_bg_get_free_blocks_count(bg, sb);
		free_blocks += free_cnt;
		ext4_bg_set_free_blocks_count(bg, sb, free_blocks);
		bg_ref.dirty = true;

		/* Release block group reference */
		rc = ext4_fs_put_block_group_ref(&bg_ref);
		if (rc != EOK)
			break;

		bg_first++;
	}

	uint32_t i;
	for (i = 0;i < blk_cnt;i++) {
		rc = ext4_trans_try_revoke_block(fs->bdev, start_block + i);
		if (rc != EOK)
			return rc;

	}

	if (!fs->jbd_journal) ext4_bcache_invalidate_lba(fs->bdev->bc, start_block, blk_cnt);
	/*All blocks should be released*/
	ext4_assert(count == 0);

	return rc;
}

int ext4_balloc_alloc_blocks(struct ext4_inode_ref *inode_ref,
    ext4_fsblk_t goal, uint32_t max_blocks, ext4_fsblk_t *fblock, uint32_t *count)
{
    if (!max_blocks || !fblock || !count) return EINVAL;
    *fblock = 0; *count = 0;
    struct ext4_fs *fs = inode_ref->fs;
    struct ext4_sblock *sb = ext4_fs_super(fs);
    if (goal < ext4_get32(sb, first_data_block) || goal >= ext4_sb_get_blocks_cnt(sb))
        goal = ext4_fs_inode_to_goal_block(inode_ref);
    uint32_t first_group = ext4_balloc_get_bgid_of_block(sb, goal);
    uint32_t groups = ext4_block_group_cnt(sb);
    for (uint32_t step = 0; step < groups; step++) {
        uint32_t group = (first_group + step) % groups;
        struct ext4_block_group_ref bg;
        int r = ext4_fs_get_block_group_ref_alloc(fs, group, &bg, false);
        if (r) return r;
        if (!ext4_bg_get_free_blocks_count(bg.block_group, sb)) {
            r = ext4_fs_put_block_group_ref(&bg);
            if (r) return r;
            continue;
        }
        struct ext4_block bitmap;
        r = ext4_trans_block_get(fs->bdev, &bitmap,
            ext4_bg_get_block_bitmap(bg.block_group, sb));
        if (r) { ext4_fs_put_block_group_ref(&bg); return r; }
        if (!ext4_balloc_verify_bitmap_csum(sb, bg.block_group, bitmap.data)) {
            ext4_block_set(fs->bdev, &bitmap);
            ext4_fs_put_block_group_ref(&bg);
            return EUCLEAN;
        }
        uint32_t end = ext4_blocks_in_group_cnt(sb, group);
        uint32_t start = step ? 0 : ext4_fs_addr_to_idx_bg(sb, goal);
        uint32_t found = end, length=0;
        /* Search the goal suffix, then the same group's prefix, without
         * borrowing or checksumming its bitmap a second time. */
        for (unsigned pass = 0; pass < (start ? 2U : 1U); pass++) {
            uint32_t limit = pass ? start : end;
            int scan=jbd_journal_find_reusable(fs,bitmap.data,
                ext4_fs_bg_idx_to_addr(sb,0,group),pass ? 0 : start,limit,
                max_blocks,false,&found,&length);
            if (scan==EOK) break;
            if (scan!=ENOSPC) __builtin_trap();
        }
        if (length) {
            length = ext4_fs_reserve_block_range(fs, length);
            if (!length) {
                ext4_block_set(fs->bdev, &bitmap);
                ext4_fs_put_block_group_ref(&bg);
                return ENOSPC;
            }
            for (uint32_t i = 0; i < length; i++) ext4_bmap_bit_set(bitmap.data, found + i);
            ext4_balloc_set_bitmap_csum(sb, bg.block_group, bitmap.data);
            ext4_trans_set_block_dirty(bitmap.buf);
        }
        r = ext4_block_set(fs->bdev, &bitmap);
        if (r) { ext4_fs_put_block_group_ref(&bg); return r; }
        if (length) {
            ext4_fs_account_blocks(fs, -(int64_t)length);
            ext4_inode_set_blocks_count(sb, inode_ref->inode,
                ext4_inode_get_blocks_count(sb, inode_ref->inode) +
                (uint64_t)length * (ext4_sb_get_block_size(sb) / EXT4_INODE_BLOCK_SIZE));
            inode_ref->dirty = true;
            ext4_bg_set_free_blocks_count(bg.block_group, sb,
                ext4_bg_get_free_blocks_count(bg.block_group, sb) - length);
            bg.dirty = true;
        }
        r = ext4_fs_put_block_group_ref(&bg);
        if (r) return r;
        if (length) {
            *fblock = ext4_fs_bg_idx_to_addr(sb, found, group);
            *count = length;
            return EOK;
        }
    }
    return ENOSPC;
}

int ext4_balloc_alloc_block(struct ext4_inode_ref *inode_ref,
    ext4_fsblk_t goal, ext4_fsblk_t *fblock)
{
    uint32_t count;
    return ext4_balloc_alloc_blocks(inode_ref, goal, 1, fblock, &count);
}

int ext4_balloc_try_alloc_block(struct ext4_inode_ref *inode_ref,
				ext4_fsblk_t baddr, bool *free)
{
	int rc;

	struct ext4_fs *fs = inode_ref->fs;
	struct ext4_sblock *sb = ext4_fs_super(fs);

	/* Compute indexes */
	uint32_t block_group = ext4_balloc_get_bgid_of_block(sb, baddr);
	uint32_t index_in_group = ext4_fs_addr_to_idx_bg(sb, baddr);

	/* Load block group reference */
	struct ext4_block_group_ref bg_ref;
	rc = ext4_fs_get_block_group_ref_alloc(fs, block_group, &bg_ref, false);
	if (rc != EOK)
		return rc;

	/* Load block with bitmap */
	ext4_fsblk_t bmp_blk_addr;
	bmp_blk_addr = ext4_bg_get_block_bitmap(bg_ref.block_group, sb);

	struct ext4_block b;
	rc = ext4_trans_block_get(fs->bdev, &b, bmp_blk_addr);
	if (rc != EOK) {
		ext4_fs_put_block_group_ref(&bg_ref);
		return rc;
	}

	if (!ext4_balloc_verify_bitmap_csum(sb, bg_ref.block_group, b.data)) {
		ext4_dbg(DEBUG_BALLOC,
			DBG_WARN "Bitmap checksum failed."
			"Group: %" PRIu32"\n",
			bg_ref.index);
		ext4_block_set(bg_ref.fs->bdev, &b);
		ext4_fs_put_block_group_ref(&bg_ref);
		return EUCLEAN;
	}

	/* Check if block is free */
    uint32_t found,length;
    rc=jbd_journal_find_reusable(fs,b.data,baddr-index_in_group,
        index_in_group,index_in_group+1,1,false,&found,&length);
    if (rc!=EOK && rc!=ENOSPC) __builtin_trap();
    *free=rc==EOK;

	/* Allocate block if possible */
	if (*free) {
        rc = ext4_fs_reserve_blocks(fs, 1);
        if (rc) {
            ext4_block_set(fs->bdev, &b);
            ext4_fs_put_block_group_ref(&bg_ref);
            return rc;
        }
		ext4_bmap_bit_set(b.data, index_in_group);
		ext4_balloc_set_bitmap_csum(sb, bg_ref.block_group, b.data);
		ext4_trans_set_block_dirty(b.buf);
	}

	/* Release block with bitmap */
	rc = ext4_block_set(fs->bdev, &b);
	if (rc != EOK) {
		/* Error in saving bitmap */
		ext4_fs_put_block_group_ref(&bg_ref);
		return rc;
	}

	/* If block is not free, return */
	if (!(*free))
		goto terminate;

	uint32_t block_size = ext4_sb_get_block_size(sb);

	/* Update superblock free blocks count */
    ext4_fs_account_blocks(fs, -1);

	/* Update inode blocks count */
	uint64_t ino_blocks = ext4_inode_get_blocks_count(sb, inode_ref->inode);
	ino_blocks += block_size / EXT4_INODE_BLOCK_SIZE;
	ext4_inode_set_blocks_count(sb, inode_ref->inode, ino_blocks);
	inode_ref->dirty = true;

	/* Update block group free blocks count */
	uint32_t fb_cnt = ext4_bg_get_free_blocks_count(bg_ref.block_group, sb);
	fb_cnt--;
	ext4_bg_set_free_blocks_count(bg_ref.block_group, sb, fb_cnt);

	bg_ref.dirty = true;

terminate:
	return ext4_fs_put_block_group_ref(&bg_ref);
}

/**
 * @}
 */
