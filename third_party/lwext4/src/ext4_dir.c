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
static uint32_t ext4_dir_csum(struct ext4_inode_ref *inode_ref,
			      struct ext4_dir_en *dirent, int size)
{
	uint32_t csum;
	struct ext4_sblock *sb = &inode_ref->fs->sb;
	uint32_t ino_index = to_le32(inode_ref->index);
	uint32_t ino_gen = to_le32(ext4_inode_get_generation(inode_ref->inode));

	/* Start with the filesystem metadata checksum seed. */
	csum = ext4_sb_get_csum_seed(sb);
	/* Then calculate crc32 checksum against inode number
	 * and inode generation */
	csum = ext4_crc32c(csum, &ino_index, sizeof(ino_index));
	csum = ext4_crc32c(csum, &ino_gen, sizeof(ino_gen));
	/* Finally calculate crc32 checksum against directory entries */
	csum = ext4_crc32c(csum, dirent, size);
	return csum;
}
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


bool ext4_dir_block_csum_verify(struct ext4_inode_ref *inode, const struct ext4_block *block)
{
    uint64_t key = ext4_fs_tree_check_key(inode, EXT4_TREE_DIRECTORY_LEAF);
    uint64_t observed = __atomic_load_n(&block->payload->tree_verified, __ATOMIC_ACQUIRE);
    if (key && observed == key) return true;
    if (!ext4_dir_csum_verify(inode, (void *)block->data)) return false;
    if (key && observed != UINT64_MAX)
        (void)__atomic_compare_exchange_n(&block->payload->tree_verified,
            &observed, key, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    return true;
}

int ext4_dir_leaf_get(struct ext4_inode_ref *dir, struct ext4_block *block,
    ext4_fsblk_t lba, bool write)
{
    struct ext4_blockdev *dev = dir->fs->bdev;
    int result = write ? ext4_trans_block_get(dev, block, lba) : ext4_block_get(dev, block, lba);
    if (result) return result;
#if CONFIG_JOURNALING_ENABLE
    if (write) {
        struct jbd_trans *trans = ext4_current_trans(dir->fs);
        uint64_t key = ext4_fs_tree_check_key(dir, EXT4_TREE_DIRECTORY_LEAF);
        if (trans && key && jbd_trans_first_tree_copy_checked(trans, block, key, dev->lg_bsize))
            return EOK;
    }
#endif
    if (ext4_dir_block_csum_verify(dir, block)) return EOK;
    ext4_dbg(DEBUG_DIR, DBG_WARN "Leaf block checksum failed. Inode: %" PRIu32
        ", physical block: %" PRIu64 "\n", dir->index, (uint64_t)lba);
    ext4_block_set(dev, block);
    return EUCLEAN;
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

int ext4_dir_add_entry(struct ext4_inode_ref *parent, const char *name,
		       uint32_t name_len, struct ext4_inode_ref *child)
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

	/* Find block, where is space for new entry and try to add */
	for (iblock = 0; iblock < total_blocks; ++iblock) {
		r = ext4_fs_get_inode_dblk_idx(parent, iblock, &fblock, false);
		if (r != EOK)
			return r;

		struct ext4_block block;
		r = ext4_dir_leaf_get(parent, &block, fblock, true);
		if (r != EOK)
			return r;

		/* If adding is successful, function can finish */
		r = ext4_dir_try_insert_entry(sb, parent, &block, child,
						name, name_len);
#if CONFIG_DIR_INDEX_ENABLE
		if (r == ENOSPC && total_blocks == 1 && ext4_current_trans(fs) &&
		    ext4_sb_feature_com(sb, EXT4_FCOM_DIR_INDEX)) {
			r = ext4_dir_dx_promote(parent, &block);
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

static inline __attribute__((always_inline)) int try_insert_directory_record(
	struct ext4_sblock *sb, struct ext4_inode_ref *parent, struct ext4_block *block,
	struct ext4_inode_ref *child, const char *name, uint32_t name_len, bool aligned)
{
	uint32_t required = (8U + name_len + 3U) & ~3U;
	uint32_t name_mask = directory_name_mask(sb);
	size_t remaining = ext4_sb_get_block_size(sb);
	uint8_t *data = block->data;
	while (remaining) {
		if (remaining < 8) return EUCLEAN;
		struct ext4_dir_en *entry = (void *)data;
		uint32_t header = directory_record_header(entry, aligned);
		uint32_t length = (uint16_t)header;
		uint32_t bytes = (header >> 16) & name_mask;
		if (!directory_record_fits(length, bytes, remaining)) return EUCLEAN;
		if (directory_record_has_inode(entry, aligned)) {
			uint32_t used = (8U + bytes + 3U) & ~3U;
			if (length - used >= required) {
				struct ext4_dir_en *slot = (void *)(data + used);
				ext4_dir_en_set_entry_len(entry, used);
				ext4_dir_write_entry(sb, slot, length - used, child, name, name_len);
				return ext4_dir_mark_dirty(parent, block);
			}
		} else if (length >= required &&
			   (name_mask == UINT16_MAX || header >> 24 != EXT4_DIRENTRY_DIR_CSUM)) {
			ext4_dir_write_entry(sb, entry, length, child, name, name_len);
			return ext4_dir_mark_dirty(parent, block);
		}
		data += length;
		remaining -= length;
	}
	return ENOSPC;
}

int ext4_dir_try_insert_entry(struct ext4_sblock *sb, struct ext4_inode_ref *parent,
			      struct ext4_block *block, struct ext4_inode_ref *child,
			      const char *name, uint32_t name_len)
{
	if (name_len > EXT4_DIRECTORY_FILENAME_LEN) return EINVAL;
	/* A valid record keeps every subsequent header four-byte aligned. */
	if ((uintptr_t)block->data & 3U)
		return try_insert_directory_record(sb, parent, block, child, name, name_len, false);
	return try_insert_directory_record(sb, parent, block, child, name, name_len, true);
}

static inline __attribute__((always_inline)) int find_directory_record(uint8_t *data,
		size_t remaining, uint32_t name_mask, size_t name_len, const char *name,
		struct ext4_dir_search_result *result, bool aligned, bool checked,
		struct ext4_block_payload *learn, uint32_t layout)
{
	struct ext4_dir_en *previous = NULL;
	bool found = false;
	while (remaining) {
		if (!checked && remaining < 8) return found ? EOK : EUCLEAN;
		struct ext4_dir_en *de = (void *)data;
		uint32_t header = directory_record_header(de, aligned);
		uint32_t length = (uint16_t)header;
		uint32_t bytes = (header >> 16) & name_mask;
		if (!checked && !directory_record_fits(length, bytes, remaining))
			return found ? EOK : EUCLEAN;
		if (!found && bytes == name_len && directory_record_has_inode(de, aligned) &&
			    !memcmp(name, de->name, name_len)) {
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
		uint32_t old = 0;
		(void)__atomic_compare_exchange_n(&learn->directory_layout, &old,
			layout, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
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
		return find_directory_record(block->data,size,name_mask,name_len,name,result,false,false,NULL,0);
	if (block->payload && size<=4096U) {
		uint32_t layout=size | (name_mask==UINT8_MAX);
		uint32_t observed=__atomic_load_n(&block->payload->directory_layout,__ATOMIC_ACQUIRE);
		if (observed==layout)
			return find_directory_record(block->data,size,name_mask,name_len,name,result,true,true,NULL,0);
		/* A lookup can succeed before a malformed later record. Keep that
		 * result, and publish a reusable layout only after checking every record. */
		if (!observed)
			return find_directory_record(block->data,size,name_mask,name_len,name,result,
				true,false,block->payload,layout);
	}
	return find_directory_record(block->data,size,name_mask,name_len,name,result,true,false,NULL,0);
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
