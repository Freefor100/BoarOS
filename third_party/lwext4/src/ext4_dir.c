/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 *
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
 * @file  ext4_dir.h
 * @brief Directory handle procedures.
 */

/* BoarOS modification, 2026-08-27: use metadata checksum seeds. */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_debug.h>

#include <ext4_trans.h>
#include <ext4_dir.h>
#include <ext4_dir_idx.h>
#include <ext4_crc32.h>
#include <ext4_inode.h>
#include <ext4_fs.h>
#include <ext4_journal.h>

#include <string.h>

/****************************************************************************/

/* Walk through a dirent block to find a checksum "dirent" at the tail */
static struct ext4_dir_entry_tail *
ext4_dir_get_tail(struct ext4_inode_ref *inode_ref,
		struct ext4_dir_en *de)
{
	struct ext4_dir_entry_tail *t;
	struct ext4_sblock *sb = &inode_ref->fs->sb;

	t = EXT4_DIRENT_TAIL(de, ext4_sb_get_block_size(sb));

	if (t->reserved_zero1 || t->reserved_zero2)
		return NULL;
	if (to_le16(t->rec_len) != sizeof(struct ext4_dir_entry_tail))
		return NULL;
	if (t->reserved_ft != EXT4_DIRENTRY_DIR_CSUM)
		return NULL;

	return t;
}

#if CONFIG_META_CSUM_ENABLE
static uint32_t ext4_dir_csum_seed(struct ext4_inode_ref *inode_ref)
{
	struct ext4_sblock *sb = &inode_ref->fs->sb;
	uint32_t identity[] = {to_le32(inode_ref->index),
		to_le32(ext4_inode_get_generation(inode_ref->inode))};
	return ext4_crc32c(ext4_sb_get_csum_seed(sb), identity, sizeof(identity));
}
static uint32_t ext4_dir_csum(struct ext4_inode_ref *inode_ref,
			      struct ext4_dir_en *dirent, int size)
{ return ext4_crc32c(ext4_dir_csum_seed(inode_ref), dirent, size); }
#else
#define ext4_dir_csum(...) 0
#endif

bool ext4_dir_csum_verify(struct ext4_inode_ref *inode_ref,
			      struct ext4_dir_en *dirent)
{
#ifdef CONFIG_META_CSUM_ENABLE
	struct ext4_dir_entry_tail *t;
	struct ext4_sblock *sb = &inode_ref->fs->sb;

	/* Compute the checksum only if the filesystem supports it */
	if (ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM)) {
		t = ext4_dir_get_tail(inode_ref, dirent);
		if (!t) {
			/* There is no space to hold the checksum */
			return false;
		}

		ptrdiff_t __unused diff = (char *)t - (char *)dirent;
		uint32_t csum = ext4_dir_csum(inode_ref, dirent, diff);
		if (t->checksum != to_le32(csum))
			return false;

	}
#endif
	return true;
}


static bool directory_leaf_verified(struct ext4_inode_ref *inode,
    const struct ext4_block *block, uint64_t key)
{
    uint64_t observed = __atomic_load_n(&block->payload->tree_verified, __ATOMIC_ACQUIRE);
    if (key && observed == key) return true;
    if (!ext4_dir_csum_verify(inode, (void *)block->data)) return false;
    if (key && observed != UINT64_MAX)
        (void)__atomic_compare_exchange_n(&block->payload->tree_verified,
            &observed, key, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    return true;
}

bool ext4_dir_block_csum_verify(struct ext4_inode_ref *inode, const struct ext4_block *block)
{
    return directory_leaf_verified(inode, block,
        ext4_fs_tree_check_key(inode, EXT4_TREE_DIRECTORY_LEAF));
}

static int directory_leaf_get(struct ext4_inode_ref *dir, struct ext4_block *block,
    ext4_fsblk_t lba, bool write, uint64_t key)
{
    struct ext4_blockdev *dev = dir->fs->bdev;
    int result = write ? ext4_trans_block_get(dev, block, lba) : ext4_block_get(dev, block, lba);
    if (result) return result;
#if CONFIG_JOURNALING_ENABLE
    if (write) {
        struct jbd_trans *trans = ext4_current_trans(dir->fs);
        if (trans && key && jbd_trans_first_tree_copy_checked(trans, block, key, dev->lg_bsize))
            return EOK;
    }
#endif
    if (directory_leaf_verified(dir, block, key)) return EOK;
    ext4_dbg(DEBUG_DIR, DBG_WARN "Leaf block checksum failed. Inode: %" PRIu32
        ", physical block: %" PRIu64 "\n", dir->index, (uint64_t)lba);
    ext4_block_set(dev, block);
    return EUCLEAN;
}

int ext4_dir_leaf_get(struct ext4_inode_ref *dir, struct ext4_block *block,
    ext4_fsblk_t lba, bool write)
{
    return directory_leaf_get(dir, block, lba, write,
        ext4_fs_tree_check_key(dir, EXT4_TREE_DIRECTORY_LEAF));
}

void ext4_dir_init_entry_tail(struct ext4_dir_entry_tail *t)
{
	memset(t, 0, sizeof(struct ext4_dir_entry_tail));
	t->rec_len = to_le16(sizeof(struct ext4_dir_entry_tail));
	t->reserved_ft = EXT4_DIRENTRY_DIR_CSUM;
}

int ext4_dir_mark_dirty(struct ext4_inode_ref *inode_ref, struct ext4_block *block)
{
	struct ext4_dir_en *dirent = (void *)block->data;
	struct ext4_sblock *sb = &inode_ref->fs->sb;
	uint64_t key = 0;

	if (ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM)) {
		struct ext4_dir_entry_tail *t = ext4_dir_get_tail(inode_ref, dirent);
		if (t) {
			ptrdiff_t __unused length = (char *)t - (char *)dirent;
			t->checksum = to_le32(ext4_dir_csum(inode_ref, dirent, length));
#if CONFIG_META_CSUM_ENABLE
			key = ext4_fs_tree_check_key(inode_ref, EXT4_TREE_DIRECTORY_LEAF);
#endif
		}
	}
	return ext4_trans_set_tree_dirty(block, key);
}

/**@brief Do some checks before returning iterator.
 * @param it Iterator to be checked
 * @return Error code
 */
static int ext4_dir_iterator_set(struct ext4_dir_iter *it)
{
	uint32_t block_size = it->block_size;
	uint32_t off_in_block = it->curr_off & (block_size - 1);
	struct ext4_sblock *sb = &it->inode_ref->fs->sb;

	it->curr = NULL;

	/* Ensure proper alignment */
	if ((off_in_block % 4) != 0)
		return EIO;

	/* Ensure that the core of the entry does not overflow the block */
	if (off_in_block > block_size - 8)
		return EIO;

	struct ext4_dir_en *en;
	en = (void *)(it->curr_blk.data + off_in_block);

	/* Ensure that the whole entry does not overflow the block */
	uint16_t length = ext4_dir_en_get_entry_len(en);
	if (length < 8 || length > block_size - off_in_block ||
	    length > it->size - it->curr_off)
		return EIO;

	/* Ensure the name length is not too large */
	if (ext4_dir_en_get_name_len(sb, en) > length - 8)
		return EIO;

	/* Everything OK - "publish" the entry */
	it->curr = en;
	it->next_off = it->curr_off + length;
	return EOK;
}

/**@brief Seek to next valid directory entry.
 *        Here can be jumped to the next data block.
 * @param it  Initialized iterator
 * @param pos Position of the next entry
 * @return Error code
 */
static int ext4_dir_iterator_seek(struct ext4_dir_iter *it, uint64_t pos)
{
	struct ext4_sblock *sb = &it->inode_ref->fs->sb;
	struct ext4_inode *inode = it->inode_ref->inode;
	struct ext4_blockdev *bdev = it->inode_ref->fs->bdev;
	it->size = ext4_inode_get_size(sb, inode);
	int r;

	/* The iterator is not valid until we seek to the desired position */
	it->curr = NULL;

	/* Are we at the end? */
	if (pos >= it->size) {
		if (it->curr_blk.lb_id) {

			r = ext4_block_set(bdev, &it->curr_blk);
			it->curr_blk.lb_id = 0;
			if (r != EOK)
				return r;
		}

		it->curr_off = pos;
		return EOK;
	}

	/* Compute next block address */
	uint32_t block_size = it->block_size;

	/*
	 * If we don't have a block or are moving across block boundary,
	 * we need to get another block
	 */
	if ((it->curr_blk.lb_id == 0) ||
	    ((it->curr_off ^ pos) >= block_size)) {
		if (it->curr_blk.lb_id) {
			r = ext4_block_set(bdev, &it->curr_blk);
			it->curr_blk.lb_id = 0;

			if (r != EOK)
				return r;
		}

		uint32_t next_blk_idx = (uint32_t)(pos >>
			(10U + ext4_get32(sb, log_block_size)));
		/* Bound speculation by EOF and by the one validated mapping
		 * run. The first cold block can start at either parity. */
		unsigned count=1;
		if (it->read_next) {
			uint64_t remaining=(it->size-(pos&~((uint64_t)block_size-1)))/block_size;
			if (remaining>EXT4_BLOCK_READ_AHEAD_MAX)
				remaining=EXT4_BLOCK_READ_AHEAD_MAX;
			if (remaining>1) count=(unsigned)remaining;
		}
		struct ext4_inode_block_range range;
		r=ext4_fs_get_inode_dblk_range(it->inode_ref,next_blk_idx,count,false,&range);
		if (r != EOK)
			return r;

		ext4_fsblk_t next_blk=range.unwritten ? 0 : range.block;
		r=next_blk && range.count>1 ?
		    ext4_block_get_read_ahead(bdev,&it->curr_blk,next_blk,range.count) :
		    ext4_block_get(bdev,&it->curr_blk,next_blk);
		if (r != EOK) {
			it->curr_blk.lb_id = 0;
			return r;
		}
	}

	it->curr_off = pos;
	return ext4_dir_iterator_set(it);
}

static int dir_iterator_init(struct ext4_dir_iter *it,
			   struct ext4_inode_ref *inode_ref,uint64_t pos,bool read_next)
{
	it->inode_ref = inode_ref;
	it->curr = 0;
	it->curr_off = 0;
	it->curr_blk.lb_id = 0;
	it->block_size = ext4_sb_get_block_size(&inode_ref->fs->sb);
	it->read_next=read_next;

	return ext4_dir_iterator_seek(it, pos);
}

int ext4_dir_iterator_init(struct ext4_dir_iter *it,
                           struct ext4_inode_ref *ref,uint64_t pos)
{ return dir_iterator_init(it,ref,pos,false); }

int ext4_dir_iterator_init_scan(struct ext4_dir_iter *it,
                               struct ext4_inode_ref *ref,uint64_t pos)
{ return dir_iterator_init(it,ref,pos,true); }

int ext4_dir_iterator_next(struct ext4_dir_iter *it)
{
	int r = EOK;
	uint16_t skip;

	while (r == EOK) {
		skip = ext4_dir_en_get_entry_len(it->curr);
		r = ext4_dir_iterator_seek(it, it->curr_off + skip);

		if (!it->curr)
			break;
		/*Skip NULL referenced entry*/
		if (ext4_dir_en_get_inode(it->curr) != 0)
			break;
	}

	return r;
}

int ext4_dir_iterator_next_raw(struct ext4_dir_iter *it)
{
	if (it == NULL || it->curr == NULL) {
		return EINVAL;
	}
	/* Read-only iteration owns this directory image and its current block.
	 * Crossing the block or EOF still uses seek to release or load a block. */
	uint64_t next = it->next_off;
	if (next < it->size && (next ^ it->curr_off) < it->block_size) {
		it->curr_off = next;
		return ext4_dir_iterator_set(it);
	}
	return ext4_dir_iterator_seek(it, next);
}

int ext4_dir_iterator_fini(struct ext4_dir_iter *it)
{
	it->curr = 0;

	if (it->curr_blk.lb_id)
		return ext4_block_set(it->inode_ref->fs->bdev, &it->curr_blk);

	return EOK;
}

void ext4_dir_write_entry(struct ext4_sblock *sb, struct ext4_dir_en *en,
			  uint16_t entry_len, struct ext4_inode_ref *child,
			  const char *name, size_t name_len)
{
	/* Check maximum entry length */
	ext4_assert(entry_len <= ext4_sb_get_block_size(sb));

	/* Set type of entry */
	switch (ext4_inode_type(sb, child->inode)) {
	case EXT4_INODE_MODE_DIRECTORY:
		ext4_dir_en_set_inode_type(sb, en, EXT4_DE_DIR);
		break;
	case EXT4_INODE_MODE_FILE:
		ext4_dir_en_set_inode_type(sb, en, EXT4_DE_REG_FILE);
		break;
	case EXT4_INODE_MODE_SOFTLINK:
		ext4_dir_en_set_inode_type(sb, en, EXT4_DE_SYMLINK);
		break;
	case EXT4_INODE_MODE_CHARDEV:
		ext4_dir_en_set_inode_type(sb, en, EXT4_DE_CHRDEV);
		break;
	case EXT4_INODE_MODE_BLOCKDEV:
		ext4_dir_en_set_inode_type(sb, en, EXT4_DE_BLKDEV);
		break;
	case EXT4_INODE_MODE_FIFO:
		ext4_dir_en_set_inode_type(sb, en, EXT4_DE_FIFO);
		break;
	case EXT4_INODE_MODE_SOCKET:
		ext4_dir_en_set_inode_type(sb, en, EXT4_DE_SOCK);
		break;
	default:
		/* FIXME: unsupported filetype */
		ext4_dir_en_set_inode_type(sb, en, EXT4_DE_UNKNOWN);
	}

	/* Set basic attributes */
	ext4_dir_en_set_inode(en, child->index);
	ext4_dir_en_set_entry_len(en, entry_len);
	ext4_dir_en_set_name_len(sb, en, (uint16_t)name_len);

	/* Write name */
	memcpy(en->name, name, name_len);
}

int ext4_dir_init_child(struct ext4_inode_ref *dir, struct ext4_inode_ref *parent)
{
	struct ext4_fs *fs = dir->fs;
	struct ext4_sblock *sb = &fs->sb;
	uint32_t size = ext4_sb_get_block_size(sb);
	ext4_fsblk_t address;
	bool fresh = false;
	int result;
	if (!ext4_inode_get_size(sb, dir->inode)) {
		struct ext4_inode_block_range range;
		result = ext4_fs_get_inode_dblk_range(dir, 0, 1, true, &range);
		if (result) return result;
		if (!range.block || range.unwritten) return EUCLEAN;
		address = range.block;
		fresh = range.allocated;
		ext4_inode_set_size(dir->inode, size);
		dir->dirty = true;
	} else {
		ext4_lblk_t logical;
		result = ext4_fs_append_inode_dblk(dir, &address, &logical);
		if (result) return result;
	}
	struct ext4_block block;
	result = fresh ? ext4_trans_block_get_new(fs->bdev, &block, address) :
	    ext4_trans_block_get_noread(fs->bdev, &block, address);
	if (result) return result;
	uint32_t records = size;
	memset(block.data, 0, size);
	if (ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM)) {
		records -= sizeof(struct ext4_dir_entry_tail);
		ext4_dir_init_entry_tail(EXT4_DIRENT_TAIL(block.data, size));
	}
	ext4_dir_write_entry(sb, (void *)block.data, 12, dir, ".", 1);
	ext4_dir_write_entry(sb, (void *)(block.data + 12), records - 12, parent, "..", 2);
	result = ext4_dir_mark_dirty(dir, &block);
	int release = ext4_block_set(fs->bdev, &block);
	return result ? result : release;
}

#if CONFIG_JOURNALING_ENABLE && CONFIG_META_CSUM_ENABLE
static int insert_checked_linear_leaf(struct ext4_inode_ref *parent, ext4_fsblk_t lba,
	struct ext4_inode_ref *child, const char *name, uint32_t name_len, struct jbd_trans *trans);
#endif

static int directory_parent_write(struct ext4_inode_ref *parent, bool *read_parent)
{
	if (!read_parent || !*read_parent) return EOK;
	int result = ext4_fs_inode_ref_write(parent);
	if (!result) *read_parent = false;
	return result;
}

int ext4_dir_add_entry_with_read_parent(struct ext4_inode_ref *parent, const char *name,
		       uint32_t name_len, struct ext4_inode_ref *child, bool *read_parent)
{
	int r;
	struct ext4_fs *fs = parent->fs;
	struct ext4_sblock *sb = &parent->fs->sb;
	r = ext4_operation_record_inode(fs, parent->index, true);
	if (!r) r = ext4_operation_record_inode(fs, child->index, false);
	if (r) return r;

#if CONFIG_DIR_INDEX_ENABLE
	/* Index adding (if allowed) */
	if ((ext4_sb_feature_com(sb, EXT4_FCOM_DIR_INDEX)) &&
	    (ext4_inode_has_flag(parent->inode, EXT4_INODE_FLAG_INDEX))) {
		r = directory_parent_write(parent, read_parent);
		if (r) return r;
		r = ext4_dir_dx_add_entry(parent, child, name, name_len);

		return r == EXT4_ERR_BAD_DX_DIR ? EUCLEAN : r;
	}
#endif

	/* Linear algorithm */
	uint32_t iblock = 0;
	ext4_fsblk_t fblock = 0;
	uint32_t block_size = ext4_sb_get_block_size(sb);
	uint64_t inode_size = ext4_inode_get_size(sb, parent->inode);
	uint32_t total_blocks = (uint32_t)(inode_size / block_size);
	struct jbd_trans *trans = ext4_current_trans(fs);
	bool can_promote __unused = CONFIG_DIR_INDEX_ENABLE && total_blocks == 1 && trans &&
		ext4_sb_feature_com(sb, EXT4_FCOM_DIR_INDEX);
#if CONFIG_JOURNALING_ENABLE && CONFIG_META_CSUM_ENABLE
	bool checked_write = trans && ext4_fs_serial_writer(fs) &&
		ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM);
#endif

	/* Find block, where is space for new entry and try to add */
	for (iblock = 0; iblock < total_blocks; ++iblock) {
		r = ext4_fs_get_inode_dblk_idx(parent, iblock, &fblock, false);
		if (r != EOK)
			return r;

#if CONFIG_JOURNALING_ENABLE && CONFIG_META_CSUM_ENABLE
		if (checked_write) {
			r = insert_checked_linear_leaf(parent, fblock, child, name, name_len, trans);
			if (r == ENOSPC && !can_promote) continue;
			if (r != ENOTSUP && r != ENOSPC) return r;
		}
#endif
		struct ext4_block block;
		r = ext4_dir_leaf_get(parent, &block, fblock, true);
		if (r != EOK)
			return r;

		/* If adding is successful, function can finish */
		r = ext4_dir_try_insert_entry(sb, parent, &block, child,
						name, name_len);
#if CONFIG_DIR_INDEX_ENABLE
		if (r == ENOSPC && can_promote) {
			r = directory_parent_write(parent, read_parent);
			if (!r) r = ext4_dir_dx_promote(parent, &block);
			int release = ext4_block_set(fs->bdev, &block);
			if (r) return r;
			if (release) return release;
			r = ext4_dir_dx_add_entry(parent, child, name, name_len);
			return r == EXT4_ERR_BAD_DX_DIR ? EUCLEAN : r;
		}
#endif
		int release = ext4_block_set(fs->bdev, &block);
		if (r != EOK && r != ENOSPC) return r;
		if (release != EOK) return release;
		if (r == EOK)
			return EOK;
	}

	/* No free block found - needed to allocate next data block */

	r = directory_parent_write(parent, read_parent);
	if (r) return r;
	iblock = 0;
	fblock = 0;
	r = ext4_fs_append_inode_dblk(parent, &fblock, &iblock);
	if (r != EOK)
		return r;

	/* Load new block */
	struct ext4_block b;

	r = ext4_trans_block_get_noread(fs->bdev, &b, fblock);
	if (r != EOK)
		return r;

	/* Fill block with zeroes */
	memset(b.data, 0, block_size);
	struct ext4_dir_en *blk_en = (void *)b.data;

	/* Save new block */
	if (ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM)) {
		uint16_t el = block_size - sizeof(struct ext4_dir_entry_tail);
		ext4_dir_write_entry(sb, blk_en, el, child, name, name_len);
		ext4_dir_init_entry_tail(EXT4_DIRENT_TAIL(b.data, block_size));
	} else {
		ext4_dir_write_entry(sb, blk_en, block_size, child, name,
				name_len);
	}

	r = ext4_dir_mark_dirty(parent, &b);
	int release = ext4_block_set(fs->bdev, &b);
	return r ? r : release;
}

int ext4_dir_add_entry(struct ext4_inode_ref *parent, const char *name,
		       uint32_t name_len, struct ext4_inode_ref *child)
{
	return ext4_dir_add_entry_with_read_parent(parent, name, name_len, child, NULL);
}

int ext4_dir_find_entry_from(struct ext4_dir_search_result *result,
			struct ext4_inode_ref *parent, const char *name,
			uint32_t name_len, bool write, uint32_t *lookup_block)
{
	int r;
	struct ext4_sblock *sb = &parent->fs->sb;

	/* Entry clear */
	result->block.lb_id = 0;
	result->dentry = NULL;
	result->previous = NULL;

#if CONFIG_DIR_INDEX_ENABLE
	/* Index search */
	if ((ext4_sb_feature_com(sb, EXT4_FCOM_DIR_INDEX)) &&
	    (ext4_inode_has_flag(parent->inode, EXT4_INODE_FLAG_INDEX))) {
		r = ext4_dir_dx_find_entry(result, parent, name_len, name, write);
		return r == EXT4_ERR_BAD_DX_DIR ? EUCLEAN : r;
	}
#endif

	ext4_fsblk_t fblock;
	uint32_t block_size = ext4_sb_get_block_size(sb);
	uint64_t inode_size = ext4_inode_get_size(sb, parent->inode);
	uint32_t total_blocks = (uint32_t)(inode_size / block_size);

	/* Start near the previous hit and wrap once over the directory. */
	uint32_t iblock = lookup_block && *lookup_block < total_blocks ? *lookup_block : 0;
	for (uint32_t left = total_blocks; left; --left,
	     iblock = iblock + 1 == total_blocks ? 0 : iblock + 1) {
		/* Load block address */
		r = ext4_fs_get_inode_dblk_idx(parent, iblock, &fblock, false);
		if (r != EOK)
			return r;

		/* Load data block */
		struct ext4_block b;
		r = ext4_dir_leaf_get(parent, &b, fblock, write);
		if (r != EOK)
			return r;

		/* Try to find entry in block */
		r = ext4_dir_find_in_block(&b, sb, name_len, name, result);
		if (r == EOK) {
			result->block = b;
			if (lookup_block)
				*lookup_block = iblock;
			return EOK;
		}

		/* Entry not found - put block and continue to the next block */
		int release = ext4_block_set(parent->fs->bdev, &b);
		if (r != ENOENT) return r;
		if (release != EOK) return release;
	}

	return ENOENT;
}

int ext4_dir_find_entry(struct ext4_dir_search_result *result,
			struct ext4_inode_ref *parent, const char *name,
			uint32_t name_len, bool write)
{
	return ext4_dir_find_entry_from(result, parent, name, name_len, write, NULL);
}

int ext4_dir_remove_entry(struct ext4_inode_ref *parent, const char *name,
			  uint32_t name_len)
{
	struct ext4_sblock *sb = &parent->fs->sb;
	/* Check if removing from directory */
	if (!ext4_inode_is_type(sb, parent->inode, EXT4_INODE_MODE_DIRECTORY))
		return ENOTDIR;

	/* Try to find entry */
	struct ext4_dir_search_result result = {0};
	int rc = ext4_dir_find_entry(&result, parent, name, name_len, true);
	if (rc == EOK) rc = ext4_dir_remove_found(parent, &result);
	int cleanup = ext4_dir_destroy_result(parent, &result);
	return rc == EOK ? cleanup : rc;
}

int ext4_dir_remove_found(struct ext4_inode_ref *parent,
                         struct ext4_dir_search_result *result)
{
	int recorded = ext4_operation_record_inode(parent->fs, parent->index, true);
	if (recorded) return recorded;
	/* Invalidate entry */
	ext4_dir_en_set_inode(result->dentry, 0);

	/* Lookup already validated the preceding record in this leaf version. */
	if (result->previous) {
		uint16_t length = ext4_dir_en_get_entry_len(result->previous);
		uint16_t removed = ext4_dir_en_get_entry_len(result->dentry);
		ext4_dir_en_set_entry_len(result->previous, length + removed);
	}

	return ext4_dir_mark_dirty(parent, &result->block);
}

typedef uint32_t directory_word __attribute__((__may_alias__));
_Static_assert(offsetof(struct ext4_dir_en,entry_len)==4 &&
	       offsetof(struct ext4_dir_en,name_len)==6 && offsetof(struct ext4_dir_en,name)==8,
	       "directory record header layout");

static inline uint32_t directory_name_mask(struct ext4_sblock *sb)
{
	return ext4_get32(sb, rev_level) == 0 &&
		ext4_get32(sb, minor_rev_level) < 5 ? UINT16_MAX : UINT8_MAX;
}

static inline uint32_t directory_record_header(struct ext4_dir_en *entry, bool aligned)
{
	if (aligned) {
		const directory_word *words = (const void *)entry;
		return to_le32(words[1]);
	}
	return ext4_dir_en_get_entry_len(entry) | (uint32_t)entry->name_len << 16 |
		(uint32_t)entry->in.name_length_high << 24;
}

static inline bool directory_record_has_inode(struct ext4_dir_en *entry, bool aligned)
{
	const directory_word *words = (const void *)entry;
	return aligned ? words[0] != 0 : ext4_dir_en_get_inode(entry) != 0;
}

static inline bool directory_record_fits(uint32_t length, uint32_t bytes, size_t remaining)
{
	return length >= 8 && !(length % 4) && length <= remaining && bytes <= length - 8;
}

enum {
	DIRECTORY_LAYOUT_MASK = 0x1fffU,
	DIRECTORY_LAYOUT_CHECKED = 0x2000U,
	DIRECTORY_SLOT_SHIFT = 14,
};

struct directory_insert_slot { uint32_t offset, length, split, first_possible; };
static inline __attribute__((always_inline)) int find_insert_slot(
	struct ext4_sblock *sb, uint8_t *data, uint32_t name_len, bool aligned,
	uint32_t start, struct directory_insert_slot *slot)
{
	uint32_t required = (8U + name_len + 3U) & ~3U;
	uint32_t name_mask = directory_name_mask(sb);
	uint32_t size = ext4_sb_get_block_size(sb);
	uint8_t *begin = data;
	uint32_t first_possible = size;
	data += start;
	size_t remaining = size - start;
	while (remaining) {
		if (remaining < 8) return EUCLEAN;
		struct ext4_dir_en *entry = (void *)data;
		uint32_t header = directory_record_header(entry, aligned);
		uint32_t length = (uint16_t)header;
		uint32_t bytes = (header >> 16) & name_mask;
		if (!directory_record_fits(length, bytes, remaining)) return EUCLEAN;
		uint32_t split = directory_record_has_inode(entry, aligned) ?
			(8U + bytes + 3U) & ~3U : 0;
		bool tail = !split && name_mask != UINT16_MAX &&
			header >> 24 == EXT4_DIRENTRY_DIR_CSUM;
		if (!tail && length - split >= 8 && first_possible == size)
			first_possible = data - begin;
		if (!tail && length - split >= required) {
			*slot = (struct directory_insert_slot){data - begin, length, split, first_possible};
			return EOK;
		}
		data += length;
		remaining -= length;
	}
	return ENOSPC;
}

static int directory_slot(struct ext4_sblock *sb, uint8_t *data, uint32_t name_len,
	uint32_t start, struct directory_insert_slot *slot)
{
	if (name_len > EXT4_DIRECTORY_FILENAME_LEN) return EINVAL;
	/* A valid record keeps every subsequent header four-byte aligned. */
	if ((uintptr_t)data & 3U) return find_insert_slot(sb, data, name_len, false, start, slot);
	return find_insert_slot(sb, data, name_len, true, start, slot);
}

static void write_directory_slot(struct ext4_sblock *sb, uint8_t *data,
	const struct directory_insert_slot *slot, struct ext4_inode_ref *child,
	const char *name, uint32_t name_len)
{
	uint8_t *record = data + slot->offset;
	if (slot->split) ext4_dir_en_set_entry_len((void *)record, slot->split);
	ext4_dir_write_entry(sb, (void *)(record + slot->split), slot->length - slot->split,
		child, name, name_len);
}

int ext4_dir_try_insert_entry(struct ext4_sblock *sb, struct ext4_inode_ref *parent,
			      struct ext4_block *block, struct ext4_inode_ref *child,
			      const char *name, uint32_t name_len)
{
	struct directory_insert_slot slot;
	int result = directory_slot(sb, block->data, name_len, 0, &slot);
	if (result) return result;
	write_directory_slot(sb, block->data, &slot, child, name, name_len);
	return ext4_dir_mark_dirty(parent, block);
}

#if CONFIG_JOURNALING_ENABLE && CONFIG_META_CSUM_ENABLE
/* Reflected CRC-32C factors for suffixes of 20 + 32 * index zero bytes.
 * The edit ends at a journal unit boundary and the final 12-byte directory
 * tail is outside the checksum. One table serves 1, 2 and 4 KiB leaves. */
static const uint32_t directory_crc_suffix[128] = {
	0x790606ffU, 0x0e148e82U, 0x041d3776U, 0x156c8e18U, 0x3f484024U, 0x78e87aafU,
	0x42316c00U, 0x7fec2963U, 0x79be2d77U, 0x8eae5107U, 0x478ac00aU, 0xc440ad59U,
	0x74d6fbcbU, 0xf282c2d0U, 0x94899a1fU, 0x271b7858U, 0xca12e5ddU, 0x260a2499U,
	0x51e379bdU, 0x31ef6835U, 0xc9c2df97U, 0xa7393cc0U, 0x2d9cbb98U, 0xb209bc78U,
	0x05fc06e9U, 0x3522e959U, 0x6f43c036U, 0xaf1f3bf7U, 0xec0241a4U, 0x6b9e3df5U,
	0xc5045adeU, 0xb4e7cf58U, 0x06c1b9d0U, 0x14f9345aU, 0x0b0a79cbU, 0x35f5eb9eU,
	0x7c64ed3dU, 0xf5b732a3U, 0xe4946ba4U, 0x0e56a229U, 0xe059842dU, 0x946db8dcU,
	0x3c889938U, 0xc683a57bU, 0x38cb8eaeU, 0x4fd9dde0U, 0x6716fbb4U, 0x5f30548dU,
	0x476291cbU, 0x23ac2352U, 0x6e0d0b06U, 0x03d63723U, 0x8f002916U, 0xf2a72c6cU,
	0x8189ff06U, 0xb22d497dU, 0xc68d315aU, 0x84daae92U, 0xbb93c515U, 0xe33d8574U,
	0x39eda602U, 0x9d9744b9U, 0xe553c60fU, 0x5708bae1U, 0x70431f2bU, 0x8221dbdfU,
	0x627593d9U, 0xfbba839fU, 0xb071401aU, 0x04e10536U, 0x96c0b335U, 0xa6c62f56U,
	0x85f33341U, 0x74ddfb24U, 0x7597dc4dU, 0x138e7da0U, 0xcd4f84b9U, 0x3ca6e879U,
	0x45cdf118U, 0x5cfc9de8U, 0x62e5219dU, 0x3dac49b9U, 0x07cbe348U, 0x0a0f4289U,
	0x5678918bU, 0x188b84dcU, 0xc2b84cf7U, 0x65172f69U, 0x0e25a5b3U, 0xd7021a10U,
	0xa8937d96U, 0xbf74bbf9U, 0x195565a0U, 0x906a362cU, 0x4819586dU, 0xa4e4b1f6U,
	0x2d7c36fcU, 0x534b0eeeU, 0x655187dbU, 0xd2f3ca34U, 0xf8dfdf9fU, 0xd502e359U,
	0x06897fdcU, 0x01ee033fU, 0xb6f9155dU, 0xcc3262f7U, 0x79c17941U, 0x11768634U,
	0x4ef66ed1U, 0x1d1cb968U, 0xff43ec2cU, 0xfcf5f0bfU, 0x14876727U, 0x1e92fec8U,
	0xf2eedd03U, 0xeaa29fb5U, 0x7e378b28U, 0x6cb0828dU, 0x37e58074U, 0x33a13823U,
	0x013caec7U, 0xc892c69dU, 0xc1fb983fU, 0x8b467c51U, 0x85f814f7U, 0xe12d244aU,
	0x4362e151U, 0xfe70ba71U,
};

static int insert_checked_linear_leaf(struct ext4_inode_ref *parent, ext4_fsblk_t lba,
	struct ext4_inode_ref *child, const char *name, uint32_t name_len, struct jbd_trans *trans)
{
	struct ext4_fs *fs = parent->fs;
	uint64_t key = ext4_fs_tree_check_key(parent, EXT4_TREE_DIRECTORY_LEAF);
	if (!key) return ENOTSUP;
	struct ext4_block block;
	int result = directory_leaf_get(parent, &block, lba, false, key);
	if (result) return result;
	uint8_t *baseline = block.data;
	const uint32_t size = ext4_sb_get_block_size(&fs->sb);
	const uint32_t layout = size | (directory_name_mask(&fs->sb) == UINT8_MAX);
	uint32_t hint = __atomic_load_n(&block.payload->directory_layout, __ATOMIC_ACQUIRE);
	bool matched_hint = size <= 4096U && (hint & DIRECTORY_LAYOUT_MASK) == layout;
	uint32_t first = matched_hint ? hint >> DIRECTORY_SLOT_SHIFT : 0;
	struct directory_insert_slot slot;
	result = directory_slot(&fs->sb, baseline, name_len, first, &slot);
	if (result) goto Finish;

	const uint32_t records_end = size - sizeof(struct ext4_dir_entry_tail);
	const uint32_t checksum_offset = size - 32U;
	const uint32_t start = slot.offset + (slot.split ? offsetof(struct ext4_dir_en, entry_len) : 0);
	const uint32_t end = slot.offset + slot.split + 8U + name_len;
	if (end > records_end) { result = EUCLEAN; goto Finish; }
	const uint32_t offset = start & ~31U;
	const uint32_t length = ((end + 31U) & ~31U) - offset;
	result = jbd_trans_prepare_checked_patch(trans, &block, offset, length, checksum_offset);
	if (result) goto Finish;

	/* The operation retains the verified baseline until publication. */
	write_directory_slot(&fs->sb, block.data, &slot, child, name, name_len);
	uint32_t checksum;
	if (size <= 4096U) {
		uint32_t limit = offset + length;
		if (limit > records_end) limit = records_end;
		uint32_t suffix = records_end - limit;
		uint32_t factor = suffix ? directory_crc_suffix[suffix / 32U] : 0x80000000U;
		uint32_t accepted = to_le32(EXT4_DIRENT_TAIL(baseline, size)->checksum);
		checksum = ext4_crc32c_replace(accepted, baseline + offset, block.data + offset,
			limit - offset, factor);
	} else {
		checksum = ext4_crc32c(ext4_dir_csum_seed(parent), baseline, start);
		checksum = ext4_crc32c(checksum, block.data + start, end - start);
		checksum = ext4_crc32c(checksum, baseline + end, records_end - end);
	}
	EXT4_DIRENT_TAIL(block.data, size)->checksum = to_le32(checksum);
	result = ext4_trans_set_tree_dirty_range(&block, offset, length, key);
	if (!result && checksum_offset >= offset + length)
		result = ext4_trans_set_tree_dirty_range(&block, checksum_offset, 32U, key);
	if (!result && size <= 4096U) {
		/* Move the first possible slot forward only when this edit used it.
		 * A skipped short hole can still fit a subsequent shorter name. */
		uint32_t next = slot.first_possible;
		if (next == slot.offset) {
			uint32_t used = (8U + name_len + 3U) & ~3U;
			next += slot.length - slot.split - used >= 8 ?
				slot.split : slot.length;
		}
		uint32_t checked = matched_hint ? hint & DIRECTORY_LAYOUT_CHECKED : 0;
		__atomic_store_n(&block.payload->directory_layout,
			layout | checked | (next << DIRECTORY_SLOT_SHIFT), __ATOMIC_RELAXED);
	}
Finish:;
	int release = ext4_block_set(fs->bdev, &block);
	return result ? result : release;
}
#endif

static inline __attribute__((always_inline)) int find_directory_record(uint8_t *data,
		size_t remaining, uint32_t name_mask, size_t name_len, const char *name,
		struct ext4_dir_search_result *result, bool aligned, bool checked,
		struct ext4_block_payload *learn, uint32_t layout, uint32_t observed,
		bool word_name)
{
	uint32_t name_word = 0;
	if (word_name)
		name_word = to_le32((uint32_t)(uint8_t)name[0] | (uint32_t)(uint8_t)name[1] << 8 |
			(uint32_t)(uint8_t)name[2] << 16 | (uint32_t)(uint8_t)name[3] << 24);
	struct ext4_dir_en *previous = NULL;
	bool found = false;
	const uint8_t *begin = data;
	uint32_t first_possible = remaining;
	while (remaining) {
		if (!checked && remaining < 8) return found ? EOK : EUCLEAN;
		struct ext4_dir_en *de = (void *)data;
		uint32_t header = directory_record_header(de, aligned);
		uint32_t length = (uint16_t)header;
		uint32_t bytes = (header >> 16) & name_mask;
		if (!checked && !directory_record_fits(length, bytes, remaining))
			return found ? EOK : EUCLEAN;
		bool used = directory_record_has_inode(de, aligned);
		if (learn && first_possible >= (layout & ~1U)) {
			uint32_t split = used ? (8U + bytes + 3U) & ~3U : 0;
			bool tail = !used && name_mask != UINT16_MAX &&
				header >> 24 == EXT4_DIRENTRY_DIR_CSUM;
			if (!tail && length - split >= 8) first_possible = data - begin;
		}
		if (!found && bytes == name_len && used &&
		    (word_name ? *(const directory_word *)(const void *)(data+8) == name_word :
		        !memcmp(name, de->name, name_len))) {
			result->dentry = de;
			result->previous = previous;
			if (!learn) return EOK;
			found = true;
		}
		previous = de;
		data += length;
		remaining -= length;
	}
	if (learn) {
		uint32_t learned = layout | DIRECTORY_LAYOUT_CHECKED |
			(first_possible << DIRECTORY_SLOT_SHIFT);
		(void)__atomic_compare_exchange_n(&learn->directory_layout, &observed,
			learned, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
	}
	return found ? EOK : ENOENT;
}

int ext4_dir_find_in_block(struct ext4_block *block, struct ext4_sblock *sb,
			   size_t name_len, const char *name,
			   struct ext4_dir_search_result *result)
{
	uint32_t name_mask = directory_name_mask(sb);
	size_t size = ext4_sb_get_block_size(sb);
	/* Every accepted record advances by a multiple of four bytes. */
	if ((uintptr_t)block->data & 3U)
		return find_directory_record(block->data,size,name_mask,name_len,name,result,false,false,NULL,0,0,false);
	if (block->payload && size<=4096U) {
		uint32_t layout=size | (name_mask==UINT8_MAX);
		uint32_t observed=__atomic_load_n(&block->payload->directory_layout,__ATOMIC_ACQUIRE);
		bool matched = (observed & DIRECTORY_LAYOUT_MASK) == layout;
		if (matched && (observed & DIRECTORY_LAYOUT_CHECKED)) {
			/* The layout proof covers this aligned four-byte name field. */
			if (name_len==sizeof(directory_word))
				return find_directory_record(block->data,size,name_mask,name_len,name,result,true,true,NULL,0,0,true);
			return find_directory_record(block->data,size,name_mask,name_len,name,result,true,true,NULL,0,0,false);
		}
		/* A lookup can succeed before a malformed later record. Keep that
		 * result, and publish a reusable layout only after checking every record. */
		if (!observed || matched)
			return find_directory_record(block->data,size,name_mask,name_len,name,result,
				true,false,block->payload,layout,observed,false);
	}
	return find_directory_record(block->data,size,name_mask,name_len,name,result,true,false,NULL,0,0,false);
}

int ext4_dir_destroy_result(struct ext4_inode_ref *parent,
			    struct ext4_dir_search_result *result)
{
	if (result->block.lb_id)
		return ext4_block_set(parent->fs->bdev, &result->block);

	return EOK;
}

/**
 * @}
 */

int ext4_dir_check_empty(struct ext4_inode_ref *dir, bool *empty)
{
    struct ext4_sblock *sb = &dir->fs->sb;
    uint32_t parent, size = ext4_sb_get_block_size(sb);
    int r = ext4_dir_parent_inode(dir, &parent);
    if (r != EOK) return r;
    uint64_t length = ext4_inode_get_size(sb, dir->inode);
    if (length % size || length / size > UINT32_MAX) return EUCLEAN;
    bool indexed = ext4_inode_has_flag(dir->inode, EXT4_INODE_FLAG_INDEX);
    for (uint32_t i = 0; i < length / size; i++) {
        ext4_fsblk_t home;
        r = ext4_fs_get_inode_dblk_idx(dir, i, &home, false);
        if (r != EOK) return r;
        if (!home || home >= dir->fs->bdev->lg_bcnt) return EUCLEAN;
        struct ext4_block block = EXT4_BLOCK_ZERO();
        r = ext4_block_get(dir->fs->bdev, &block, home);
        if (r != EOK) return r;
        bool index_block=false;
        if(indexed) {
            r=ext4_dir_dx_is_node(dir,i,&index_block);
            if(r!=EOK) { ext4_block_set(dir->fs->bdev,&block);return r; }
        }
        bool found = false;
        if (index_block) r = ext4_dir_dx_check(dir, &block);
        else {
            if (!ext4_dir_block_csum_verify(dir, &block)) r = EUCLEAN;
            for (uint32_t pos = 0; r == EOK && pos < size; ) {
                struct ext4_dir_en *entry = (void *)(block.data + pos);
                if (size - pos < 8) { r = EUCLEAN; break; }
                uint32_t n = ext4_dir_en_get_entry_len(entry);
                uint32_t namesz = ext4_dir_en_get_name_len(sb, entry);
                if (n < 8 || n % 4 || n > size - pos || namesz > n - 8) {
                    r = EUCLEAN; break;
                }
                if (ext4_dir_en_get_inode(entry)) {
                    bool dot = i == 0 && pos == 0 && namesz == 1 && entry->name[0] == '.';
                    bool dotdot = i == 0 && pos == 12 && namesz == 2 &&
                                  entry->name[0] == '.' && entry->name[1] == '.';
                    if (!dot && !dotdot) found = true;
                }
                pos += n;
            }
        }
        int release = ext4_block_set(dir->fs->bdev, &block);
        if (r == EOK) r = release;
        if (r != EOK) return r;
        if (found) { *empty = false; return EOK; }
    }
    *empty = true;
    return EOK;
}
