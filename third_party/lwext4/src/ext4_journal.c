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
 * @file  ext4_journal.c
 * @brief Journal handle functions
 */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_debug.h>

#include <ext4_fs.h>
#include <ext4_orphan.h>
#include <ext4_super.h>
#include <ext4_journal.h>
#include <ext4_blockdev.h>
#include <ext4_bitmap.h>
#include <ext4_crc32.h>
#include <ext4_journal.h>

#include <string.h>
#include <stdlib.h>
#include <kernel/cost.h>

struct jbd_state_guard { struct ext4_fs *fs; };
static struct jbd_state_guard jbd_state_enter(const struct jbd_journal *journal)
{
    struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
    ext4_fs_state_lock(fs);
    return (struct jbd_state_guard){fs};
}
static struct jbd_state_guard jbd_state_read_enter(const struct jbd_journal *journal)
{
    struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
    ext4_fs_state_read_lock(fs);
    return (struct jbd_state_guard){fs};
}
static void jbd_state_leave(struct jbd_state_guard *guard)
{ ext4_fs_state_unlock(guard->fs); }
#define JBD_STATE_SCOPE(journal) \
    struct jbd_state_guard state_guard __attribute__((cleanup(jbd_state_leave))) = jbd_state_enter(journal)
#define JBD_STATE_READ_SCOPE(journal) \
    struct jbd_state_guard state_guard __attribute__((cleanup(jbd_state_leave))) = jbd_state_read_enter(journal)

/* A version owns both its image and the shared home-buffer budget pin. */
static struct jbd_image *jbd_version(struct jbd_journal *journal, struct ext4_buf *buf)
{
    struct jbd_image *image=jbd_image_alloc(journal);
    if (!image) return NULL;
    if (!jbd_memory_pin_home(journal,buf)) {jbd_image_free(image);return NULL;}
    return image;
}
static void jbd_unpin_version(struct jbd_journal *journal, struct ext4_buf *buf)
{ jbd_memory_unpin_home(journal,buf); }

/**@brief  Revoke entry during journal replay.*/
struct revoke_entry {
	/**@brief  Block number not to be replayed.*/
	ext4_fsblk_t block;

	/**@brief  For any transaction id smaller
	 *         than trans_id, records of @block
	 *         in those transactions should not
	 *         be replayed.*/
	uint32_t trans_id;

	/**@brief  Revoke tree node.*/
	RB_ENTRY(revoke_entry) revoke_node;
};

/**@brief  Valid journal replay information.*/
struct recover_info {
	int error;
	/**@brief  Starting transaction id.*/
	uint32_t start_trans_id;

	/**@brief  Ending transaction id.*/
	uint32_t last_trans_id;

	/**@brief  Used as internal argument.*/
	uint32_t this_trans_id;

	/**@brief  No of transactions went through.*/
	uint32_t trans_cnt;
	bool super_replayed;

	/**@brief  RB-Tree storing revoke entries.*/
	RB_HEAD(jbd_revoke, revoke_entry) revoke_root;
};

/**@brief  Journal replay internal arguments.*/
struct replay_arg {
	/**@brief  Journal replay information.*/
	struct recover_info *info;

	/**@brief  Current block we are on.*/
	uint32_t *this_block;

	/**@brief  Current trans_id we are on.*/
	uint32_t this_trans_id;
};

/* Make sure we wrap around the log correctly! */
#define wrap(sb, var)						\
do {									\
	if (var >= jbd_get32((sb), maxlen))					\
		var -= (jbd_get32((sb), maxlen) - jbd_get32((sb), first));	\
} while (0)

static inline int32_t
trans_id_diff(uint32_t x, uint32_t y)
{
	int32_t diff = x - y;
	return diff;
}

static int
jbd_revoke_entry_cmp(struct revoke_entry *a, struct revoke_entry *b)
{
	if (a->block > b->block)
		return 1;
	else if (a->block < b->block)
		return -1;
	return 0;
}

static int
jbd_block_rec_cmp(struct jbd_block_rec *a, struct jbd_block_rec *b)
{
	if (a->lba > b->lba)
		return 1;
	else if (a->lba < b->lba)
		return -1;
	return 0;
}

static int
jbd_revoke_rec_cmp(struct jbd_revoke_rec *a, struct jbd_revoke_rec *b)
{
	if (a->lba > b->lba)
		return 1;
	else if (a->lba < b->lba)
		return -1;
	return 0;
}

RB_GENERATE_INTERNAL(jbd_revoke, revoke_entry, revoke_node,
		     jbd_revoke_entry_cmp, static inline)
RB_GENERATE_INTERNAL(jbd_block, jbd_block_rec, block_rec_node,
		     jbd_block_rec_cmp, static inline)
RB_GENERATE_INTERNAL(jbd_revoke_tree, jbd_revoke_rec, revoke_node,
		     jbd_revoke_rec_cmp, static inline)

static int jbd_quarantine_cmp(struct jbd_quarantine *a, struct jbd_quarantine *b)
{ return a->first < b->first ? -1 : a->first > b->first; }
RB_GENERATE_INTERNAL(jbd_quarantine_tree, jbd_quarantine, index_node,
		     jbd_quarantine_cmp, static inline)

#define jbd_alloc_revoke_entry() ext4_calloc(1, sizeof(struct revoke_entry))
#define jbd_free_revoke_entry(addr) ext4_free(addr)

static int jbd_has_csum(struct jbd_sb *jbd_sb)
{
	if (JBD_HAS_INCOMPAT_FEATURE(jbd_sb, JBD_FEATURE_INCOMPAT_CSUM_V2))
		return 2;

	if (JBD_HAS_INCOMPAT_FEATURE(jbd_sb, JBD_FEATURE_INCOMPAT_CSUM_V3))
		return 3;

	return 0;
}

/* This implementation uses ordered synchronous commits. Async commit and
 * the legacy whole-transaction CRC32 format need their own recovery protocol. */
static int jbd_check_features(struct jbd_fs *jbd_fs)
{
	struct jbd_sb *sb = &jbd_fs->sb;
	uint32_t supported = JBD_FEATURE_INCOMPAT_REVOKE |
		JBD_FEATURE_INCOMPAT_64BIT | JBD_FEATURE_INCOMPAT_CSUM_V2 |
		JBD_FEATURE_INCOMPAT_CSUM_V3;
	uint32_t features = jbd_get32(sb, feature_incompat);
	if ((features & ~supported) || jbd_get32(sb, feature_compat) ||
	    jbd_get32(sb, feature_ro_compat))
		return ENOTSUP;
	if ((features & JBD_FEATURE_INCOMPAT_CSUM_V2) &&
	    (features & JBD_FEATURE_INCOMPAT_CSUM_V3))
		return EINVAL;
	if (jbd_has_csum(sb) && (!CONFIG_META_CSUM_ENABLE ||
	    sb->checksum_type != JBD_CRC32C_CHKSUM))
		return ENOTSUP;
	if (jbd_get32(sb, blocksize) != jbd_fs->inode_ref.fs->bdev->lg_bsize ||
	    !jbd_get32(sb, first) ||
	    jbd_get32(sb, first) >= jbd_get32(sb, maxlen) ||
	    (jbd_get32(sb, start) &&
	     (jbd_get32(sb, start) < jbd_get32(sb, first) ||
	      jbd_get32(sb, start) >= jbd_get32(sb, maxlen))))
		return EINVAL;
	return EOK;
}

#if CONFIG_META_CSUM_ENABLE
static uint32_t jbd_sb_csum(struct jbd_sb *jbd_sb)
{
	uint32_t checksum = 0;

	if (jbd_has_csum(jbd_sb)) {
		uint32_t orig_checksum = jbd_sb->checksum;
		jbd_set32(jbd_sb, checksum, 0);
		/* Calculate crc32c checksum against tho whole superblock */
		checksum = ext4_crc32c(EXT4_CRC32_INIT, jbd_sb,
				JBD_SUPERBLOCK_SIZE);
		jbd_sb->checksum = orig_checksum;
	}
	return checksum;
}
#else
#define jbd_sb_csum(...) 0
#endif

static void jbd_sb_csum_set(struct jbd_sb *jbd_sb)
{
	if (!jbd_has_csum(jbd_sb))
		return;

	jbd_set32(jbd_sb, checksum, jbd_sb_csum(jbd_sb));
}

#if CONFIG_META_CSUM_ENABLE
static bool
jbd_verify_sb_csum(struct jbd_sb *jbd_sb)
{
	if (!jbd_has_csum(jbd_sb))
		return true;

	return jbd_sb_csum(jbd_sb) == jbd_get32(jbd_sb, checksum);
}
#else
#define jbd_verify_sb_csum(...) true
#endif

#if CONFIG_META_CSUM_ENABLE
static uint32_t jbd_meta_csum(struct jbd_fs *jbd_fs,
			      struct jbd_bhdr *bhdr)
{
	uint32_t checksum = 0;

	if (jbd_has_csum(&jbd_fs->sb)) {
		uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
		struct jbd_block_tail *tail =
			(struct jbd_block_tail *)((char *)bhdr + block_size -
				sizeof(struct jbd_block_tail));
		uint32_t orig_checksum = tail->checksum;
		tail->checksum = 0;

		/* First calculate crc32c checksum against fs uuid */
		checksum = ext4_crc32c(EXT4_CRC32_INIT, jbd_fs->sb.uuid,
				       sizeof(jbd_fs->sb.uuid));
		/* Calculate crc32c checksum against tho whole block */
		checksum = ext4_crc32c(checksum, bhdr,
				block_size);
		tail->checksum = orig_checksum;
	}
	return checksum;
}
#else
#define jbd_meta_csum(...) 0
#endif

static void jbd_meta_csum_set(struct jbd_fs *jbd_fs,
			      struct jbd_bhdr *bhdr)
{
	uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
	struct jbd_block_tail *tail = (struct jbd_block_tail *)
				((char *)bhdr + block_size -
				sizeof(struct jbd_block_tail));
	if (!jbd_has_csum(&jbd_fs->sb))
		return;

	tail->checksum = to_be32(jbd_meta_csum(jbd_fs, bhdr));
}

#if CONFIG_META_CSUM_ENABLE
static bool
jbd_verify_meta_csum(struct jbd_fs *jbd_fs,
		     struct jbd_bhdr *bhdr)
{
	uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
	struct jbd_block_tail *tail = (struct jbd_block_tail *)
				((char *)bhdr + block_size -
				sizeof(struct jbd_block_tail));
	if (!jbd_has_csum(&jbd_fs->sb))
		return true;

	return jbd_meta_csum(jbd_fs, bhdr) == to_be32(tail->checksum);
}
#else
#define jbd_verify_meta_csum(...) true
#endif

#if CONFIG_META_CSUM_ENABLE
static uint32_t jbd_commit_csum(struct jbd_fs *jbd_fs,
			      struct jbd_commit_header *header)
{
	uint32_t checksum = 0;

	if (jbd_has_csum(&jbd_fs->sb)) {
		uint8_t orig_checksum_type = header->chksum_type,
			 orig_checksum_size = header->chksum_size;
		uint32_t orig_checksum = header->chksum[0];
		uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
		header->chksum_type = 0;
		header->chksum_size = 0;
		header->chksum[0] = 0;

		/* First calculate crc32c checksum against fs uuid */
		checksum = ext4_crc32c(EXT4_CRC32_INIT, jbd_fs->sb.uuid,
				       sizeof(jbd_fs->sb.uuid));
		/* Calculate crc32c checksum against tho whole block */
		checksum = ext4_crc32c(checksum, header,
				block_size);

		header->chksum_type = orig_checksum_type;
		header->chksum_size = orig_checksum_size;
		header->chksum[0] = orig_checksum;
	}
	return checksum;
}
#else
#define jbd_commit_csum(...) 0
#endif

static void jbd_commit_csum_set(struct jbd_fs *jbd_fs,
			      struct jbd_commit_header *header)
{
	if (!jbd_has_csum(&jbd_fs->sb))
		return;

	header->chksum_type = 0;
	header->chksum_size = 0;
	header->chksum[0] = to_be32(jbd_commit_csum(jbd_fs, header));
}

#if CONFIG_META_CSUM_ENABLE
static bool jbd_verify_commit_csum(struct jbd_fs *jbd_fs,
				   struct jbd_commit_header *header)
{
	if (!jbd_has_csum(&jbd_fs->sb))
		return true;
	uint32_t saved = header->chksum[0];
	header->chksum[0] = 0;
	uint32_t sum = ext4_crc32c(EXT4_CRC32_INIT, jbd_fs->sb.uuid, UUID_SIZE);
	sum = ext4_crc32c(sum, header, jbd_get32(&jbd_fs->sb, blocksize));
	header->chksum[0] = saved;
	return saved == to_be32(sum);
}
#else
#define jbd_verify_commit_csum(...) true
#endif

#if CONFIG_META_CSUM_ENABLE
/*
 * NOTE: We only make use of @csum parameter when
 *       JBD_FEATURE_COMPAT_CHECKSUM is enabled.
 */
static uint32_t jbd_block_csum(struct jbd_fs *jbd_fs, const void *buf,
			       uint32_t csum,
			       uint32_t sequence, bool escaped)
{
	uint32_t checksum = 0;

	if (jbd_has_csum(&jbd_fs->sb)) {
		uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
		/* First calculate crc32c checksum against fs uuid */
		checksum = ext4_crc32c(EXT4_CRC32_INIT, jbd_fs->sb.uuid,
				       sizeof(jbd_fs->sb.uuid));
		/* Then calculate crc32c checksum against sequence no. */
		sequence = to_be32(sequence);
		checksum = ext4_crc32c(checksum, &sequence,
				sizeof(uint32_t));
		/* Escape changes wire bytes, never the shared frozen version. */
		if (escaped) {
			uint32_t zero = 0;
			checksum = ext4_crc32c(checksum, &zero, sizeof(zero));
			buf = (const uint8_t *)buf + sizeof(zero);
			block_size -= sizeof(zero);
		}
		checksum = ext4_crc32c(checksum, buf,
				block_size);
	} else if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_COMPAT_CHECKSUM)) {
		uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
		if (escaped) {
			uint32_t zero = 0;
			csum = ext4_crc32(csum, &zero, sizeof(zero));
			buf = (const uint8_t *)buf + sizeof(zero);
			block_size -= sizeof(zero);
		}
		checksum = ext4_crc32(csum, buf,
				block_size);
	}
	return checksum;
}
#else
#define jbd_block_csum(...) 0
#endif

static void jbd_block_tag_csum_set(struct jbd_fs *jbd_fs, void *__tag,
				   uint32_t checksum)
{
	int ver = jbd_has_csum(&jbd_fs->sb);
	if (!ver)
		return;

	if (ver == 2) {
		struct jbd_block_tag *tag = __tag;
		tag->checksum = to_be16((uint16_t)checksum);
	} else {
		struct jbd_block_tag3 *tag = __tag;
		tag->checksum = to_be32(checksum);
	}
}

/**@brief  Write jbd superblock to disk.
 * @param  jbd_fs jbd filesystem
 * @param  s jbd superblock
 * @return standard error code*/
static int jbd_sb_write(struct jbd_fs *jbd_fs, struct jbd_sb *s)
{
	int rc;
	struct ext4_fs *fs = jbd_fs->inode_ref.fs;
	uint64_t offset;
	ext4_fsblk_t fblock;
	rc = jbd_inode_bmap(jbd_fs, 0, &fblock);
	if (rc != EOK)
		return rc;

	jbd_sb_csum_set(s);
	offset = fblock * ext4_sb_get_block_size(&fs->sb);
	return ext4_block_writebytes(fs->bdev, offset, s,
				     EXT4_SUPERBLOCK_SIZE);
}

/**@brief  Read jbd superblock from disk.
 * @param  jbd_fs jbd filesystem
 * @param  s jbd superblock
 * @return standard error code*/
static int jbd_sb_read(struct jbd_fs *jbd_fs, struct jbd_sb *s)
{
	int rc;
	struct ext4_fs *fs = jbd_fs->inode_ref.fs;
	uint64_t offset;
	ext4_fsblk_t fblock;
	rc = jbd_inode_bmap(jbd_fs, 0, &fblock);
	if (rc != EOK)
		return rc;

	offset = fblock * ext4_sb_get_block_size(&fs->sb);
	return ext4_block_readbytes(fs->bdev, offset, s,
				    EXT4_SUPERBLOCK_SIZE);
}

/**@brief  Verify jbd superblock.
 * @param  sb jbd superblock
 * @return true if jbd superblock is valid */
static bool jbd_verify_sb(struct jbd_sb *sb)
{
	struct jbd_bhdr *header = &sb->header;
	if (jbd_get32(header, magic) != JBD_MAGIC_NUMBER)
		return false;

	if (jbd_get32(header, blocktype) != JBD_SUPERBLOCK &&
	    jbd_get32(header, blocktype) != JBD_SUPERBLOCK_V2)
		return false;

	return jbd_verify_sb_csum(sb);
}

/**@brief  Write back dirty jbd superblock to disk.
 * @param  jbd_fs jbd filesystem
 * @return standard error code*/
static int jbd_write_sb(struct jbd_fs *jbd_fs)
{
	int rc = EOK;
	if (jbd_fs->dirty) {
		rc = jbd_sb_write(jbd_fs, &jbd_fs->sb);
		if (rc != EOK)
			return rc;

		jbd_fs->dirty = false;
	}
	return rc;
}

/**@brief  Get reference to jbd filesystem.
 * @param  fs Filesystem to load journal of
 * @param  jbd_fs jbd filesystem
 * @return standard error code*/
int jbd_get_fs(struct ext4_fs *fs,
	       struct jbd_fs *jbd_fs)
{
	int rc;
	uint32_t journal_ino;

	memset(jbd_fs, 0, sizeof(struct jbd_fs));
	/* See if there is journal inode on this filesystem.*/
	/* FIXME: detection on existance ofbkejournal bdev is
	 *        missing.*/
	journal_ino = ext4_get32(&fs->sb, journal_inode_number);
	if (!journal_ino)
		return ENOTSUP;

	rc = ext4_fs_get_inode_ref_read(fs,
				   journal_ino,
				   &jbd_fs->inode_ref);
	if (rc != EOK)
		return rc;

	rc = jbd_sb_read(jbd_fs, &jbd_fs->sb);
	if (rc != EOK)
		goto Error;

	if (!jbd_verify_sb(&jbd_fs->sb)) {
		rc = EIO;
		goto Error;
	}

	rc = jbd_check_features(jbd_fs);
	if (rc != EOK)
		goto Error;
	jbd_fs->bdev = fs->bdev;

	return rc;
Error:
	ext4_fs_put_inode_ref(&jbd_fs->inode_ref);
	memset(jbd_fs, 0, sizeof(struct jbd_fs));

	return rc;
}

/**@brief  Put reference of jbd filesystem.
 * @param  jbd_fs jbd filesystem
 * @return standard error code*/
int jbd_put_fs(struct jbd_fs *jbd_fs)
{
	int rc = EOK;
	rc = jbd_write_sb(jbd_fs);
	if (rc != EOK)
		return rc;
	return ext4_fs_put_inode_ref(&jbd_fs->inode_ref);
}

/**@brief  Data block lookup helper.
 * @param  jbd_fs jbd filesystem
 * @param  iblock block index
 * @param  fblock logical block address
 * @return standard error code*/
int jbd_inode_bmap(struct jbd_fs *jbd_fs,
		   ext4_lblk_t iblock,
		   ext4_fsblk_t *fblock)
{
	int rc = ext4_fs_get_inode_dblk_idx(
			&jbd_fs->inode_ref,
			iblock,
			fblock,
			false);
	return rc;
}

/**@brief   jbd block get function (through cache).
 * @param   jbd_fs jbd filesystem
 * @param   block block descriptor
 * @param   fblock jbd logical block address
 * @return  standard error code*/
static int jbd_block_get(struct jbd_fs *jbd_fs,
		  struct ext4_block *block,
		  ext4_fsblk_t fblock)
{
	COST_PHASE_SCOPE(read_source, 7);
	/* TODO: journal device. */
	int rc;
	struct ext4_blockdev *bdev = jbd_fs->bdev;
	ext4_lblk_t iblock = (ext4_lblk_t)fblock;

	/* Lookup the logical block address of
	 * fblock.*/
	struct jbd_journal *journal = bdev->journal;
	if (journal && journal->jbd_fs == jbd_fs && journal->log_map) {
		if (iblock >= jbd_get32(&jbd_fs->sb, maxlen)) return EUCLEAN;
		fblock = journal->log_map[iblock];
		rc = EOK;
	} else rc = jbd_inode_bmap(jbd_fs, iblock, &fblock);
	if (rc != EOK)
		return rc;

	rc = ext4_block_get(bdev, block, fblock);

	/* If succeeded, mark buffer as BC_FLUSH to indicate
	 * that data should be written to disk immediately.*/
	if (rc == EOK) {
		ext4_bcache_set_flag(block->buf, BC_FLUSH);
		/* As we don't want to occupy too much space
		 * in block cache, we set this buffer BC_TMP.*/
		ext4_bcache_set_flag(block->buf, BC_TMP);
	}

	return rc;
}

/**@brief   jbd block set procedure (through cache).
 * @param   jbd_fs jbd filesystem
 * @param   block block descriptor
 * @return  standard error code*/
static int jbd_block_set(struct jbd_fs *jbd_fs,
		  struct ext4_block *block)
{
	struct ext4_blockdev *bdev = jbd_fs->bdev;
	int r = ext4_block_flush_buf(bdev, block->buf);
	/* A failed temporary log write is not a pending home write. The
	 * transaction owner retains the source and will report the failure. */
	if (r != EOK)
		ext4_bcache_clear_dirty(block->buf);
	int release = ext4_block_set(bdev, block);
	return r != EOK ? r : release;
}

static unsigned jbd_log_free_blocks(const struct jbd_journal *journal)
{
	uint32_t capacity = jbd_get32(&journal->jbd_fs->sb, maxlen) - journal->first;
	uint32_t used = journal->last >= journal->start ? journal->last - journal->start :
		capacity - (journal->start - journal->last);
	return capacity - used - 1;
}

static int jbd_tag_bytes(struct jbd_fs *jbd_fs);
static unsigned jbd_log_images_needed(const struct jbd_journal *journal,
	unsigned payloads, unsigned revokes, bool commit)
{
	struct jbd_fs *fs = journal->jbd_fs;
	unsigned tail = jbd_has_csum(&fs->sb) ? sizeof(struct jbd_block_tail) : 0;
	unsigned tags = (journal->block_size - sizeof(struct jbd_bhdr) - tail - UUID_SIZE) /
		jbd_tag_bytes(journal->jbd_fs);
	unsigned record = JBD_HAS_INCOMPAT_FEATURE(&fs->sb, JBD_FEATURE_INCOMPAT_64BIT) ? 8 : 4;
	unsigned entries = (journal->block_size - sizeof(struct jbd_revoke_header) - tail) / record;
	if (!tags || !entries) __builtin_trap();
	return payloads / tags + (payloads % tags != 0) +
		revokes / entries + (revokes % entries != 0) + commit;
}


static int jbd_group_requirements(struct jbd_journal *journal,
    struct jbd_trans *running, struct jbd_trans *operation,
    unsigned *credits, unsigned *images);

/* STATE protects credits, not wire buffers. A paused preparer therefore
 * cannot lend storage or an unclaimed ring address to the commit consumer. */
static int jbd_reserve_logs(struct jbd_trans *trans, bool serial_writer)
{
    struct jbd_journal *journal = trans->journal;
    unsigned payloads = trans->data_cnt, revokes = trans->revoke_count;
    if (!journal->grouped && !payloads && !revokes) return EOK;
    unsigned images = jbd_log_images_needed(journal, payloads, revokes, true);
    unsigned capacity = jbd_get32(&journal->jbd_fs->sb, maxlen) - journal->first;
    if (payloads >= capacity || images >= capacity - payloads) return ENOSPC;
    unsigned count = payloads + images;
    if (journal->grouped && serial_writer && journal->running) {
        unsigned combined, combined_images;
        int result = jbd_group_requirements(journal, journal->running, trans,
            &combined, &combined_images);
        if (result) {
            /* This operation fits alone; draining the current group can make
             * room. Preserve that distinction through abort to its caller. */
            trans->operation.log_reservation_failed = true;
            return result;
        }
        if (combined < journal->running->reserved_logs) __builtin_trap();
        count = combined - journal->running->reserved_logs;
        trans->reservation_group_sequence = journal->running->sequence;
    }
    unsigned available = jbd_log_free_blocks(journal);
    if (journal->log_reserved > available) __builtin_trap();
    if (count > available - journal->log_reserved) {
        trans->operation.log_reservation_failed = true;
        return ENOSPC;
    }
    journal->log_reserved += count;
    trans->reserved_logs = count;
    return EOK;
}

static void jbd_release_prepared(struct jbd_trans *trans)
{
    if (trans->journal->log_reserved < trans->reserved_logs) __builtin_trap();
    trans->journal->log_reserved -= trans->reserved_logs;
    trans->reserved_logs = 0;
}

static void jbd_trim_reserved_logs(struct jbd_trans *trans, unsigned credits)
{
    if (trans->reserved_logs < credits) __builtin_trap();
    unsigned unused = trans->reserved_logs - credits;
    if (trans->journal->log_reserved < unused) __builtin_trap();
    trans->journal->log_reserved -= unused;
    trans->reserved_logs = credits;
}

static void jbd_claim_logs(struct jbd_trans *trans)
{
    struct jbd_journal *journal = trans->journal;
    unsigned credits = trans->reserved_logs;
    if (!credits || trans->alloc_blocks || trans->frozen ||
        journal->log_reserved < credits ||
        journal->log_reserved > jbd_log_free_blocks(journal) ||
        credits != (unsigned)trans->data_cnt +
            jbd_log_images_needed(journal, trans->data_cnt, trans->revoke_count, true))
        __builtin_trap();
    trans->start_iblock = journal->last;
    trans->alloc_blocks = credits;
    trans->frozen = true;
    uint64_t end = (uint64_t)journal->last + credits;
    wrap(&journal->jbd_fs->sb, end);
    journal->last = (uint32_t)end;
    journal->log_reserved -= credits;
    trans->reserved_logs = 0;
}

/* Nodes belong to the already charged stable version/control owners. The
 * phase adds neither an array allocation nor another payload version. */
struct jbd_io_phase { struct ext4_block_write *first, *last; };
static void jbd_phase_add(struct jbd_journal *journal, struct jbd_io_phase *phase,
    struct ext4_block_write *io, const void *data, ext4_fsblk_t block,
    uint32_t within, size_t size)
{
    struct ext4_blockdev *bdev = journal->jbd_fs->bdev;
    *io = (struct ext4_block_write){block * journal->block_size + within + bdev->part_offset,
        data, size, NULL};
    struct ext4_block_write *last = phase->last;
    if (last && last->offset <= UINT64_MAX - last->size &&
        last->offset + last->size == io->offset &&
        last->size <= UINTPTR_MAX - (uintptr_t)last->data &&
        (uintptr_t)last->data + last->size == (uintptr_t)io->data &&
        io->size <= SIZE_MAX - last->size) {
        last->size += io->size;
        return;
    }
    if (last) last->next = io;
    else phase->first = io;
    phase->last = io;
}
static void jbd_ordered_phase(struct jbd_trans *trans, struct jbd_io_phase *phase)
{
    struct jbd_data *data;
    TAILQ_FOREACH(data, &trans->data_queue, node)
        jbd_phase_add(trans->journal, phase, &data->io, data->block.data,
            data->block.lb_id, 0, trans->journal->block_size);
}
/**@brief  helper functions to calculate
 *         block tag size, not including UUID part.
 * @param  jbd_fs jbd filesystem
 * @return tag size in bytes*/
static int jbd_tag_bytes(struct jbd_fs *jbd_fs)
{
	int size;

	/* It is very easy to deal with the case which
	 * JBD_FEATURE_INCOMPAT_CSUM_V3 is enabled.*/
	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V3))
		return sizeof(struct jbd_block_tag3);

	size = sizeof(struct jbd_block_tag);

	/* If JBD_FEATURE_INCOMPAT_CSUM_V2 is enabled,
	 * add 2 bytes to size.*/
	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V2))
		size += sizeof(uint16_t);

	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_64BIT))
		return size;

	/* If block number is 4 bytes in size,
	 * minus 4 bytes from size */
	return size - sizeof(uint32_t);
}

/**@brief  Tag information. */
struct tag_info {
	/**@brief  Tag size in bytes, including UUID part.*/
	int tag_bytes;

	/**@brief  block number stored in this tag.*/
	ext4_fsblk_t block;

	/**@brief  Is the first 4 bytes of block equals to
	 *	   JBD_MAGIC_NUMBER? */
	bool is_escape;

	/**@brief  whether UUID part exists or not.*/
	bool uuid_exist;

	/**@brief  UUID content if UUID part exists.*/
	uint8_t uuid[UUID_SIZE];

	/**@brief  Is this the last tag? */
	bool last_tag;

	/**@brief  crc32c checksum. */
	uint32_t checksum;
};

/**@brief  Extract information from a block tag.
 * @param  __tag pointer to the block tag
 * @param  tag_bytes block tag size of this jbd filesystem
 * @param  remain_buf_size size in buffer containing the block tag
 * @param  tag_info information of this tag.
 * @return  EOK when succeed, otherwise return EINVAL.*/
static int
jbd_extract_block_tag(struct jbd_fs *jbd_fs,
		      void *__tag,
		      int tag_bytes,
		      int32_t remain_buf_size,
		      struct tag_info *tag_info)
{
	char *uuid_start;
	tag_info->tag_bytes = tag_bytes;
	tag_info->uuid_exist = false;
	tag_info->last_tag = false;
	tag_info->is_escape = false;

	/* See whether it is possible to hold a valid block tag.*/
	if (remain_buf_size - tag_bytes < 0)
		return EINVAL;

	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V3)) {
		struct jbd_block_tag3 *tag = __tag;
		tag_info->block = jbd_get32(tag, blocknr);
		tag_info->checksum = jbd_get32(tag, checksum);
		if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
					     JBD_FEATURE_INCOMPAT_64BIT))
			 tag_info->block |=
				 (uint64_t)jbd_get32(tag, blocknr_high) << 32;

		if (jbd_get32(tag, flags) & JBD_FLAG_ESCAPE)
			tag_info->is_escape = true;

		if (!(jbd_get32(tag, flags) & JBD_FLAG_SAME_UUID)) {
			/* See whether it is possible to hold UUID part.*/
			if (remain_buf_size - tag_bytes < UUID_SIZE)
				return EINVAL;

			uuid_start = (char *)tag + tag_bytes;
			tag_info->uuid_exist = true;
			tag_info->tag_bytes += UUID_SIZE;
			memcpy(tag_info->uuid, uuid_start, UUID_SIZE);
		}

		if (jbd_get32(tag, flags) & JBD_FLAG_LAST_TAG)
			tag_info->last_tag = true;

	} else {
		struct jbd_block_tag *tag = __tag;
		tag_info->block = jbd_get32(tag, blocknr);
		tag_info->checksum = jbd_get16(tag, checksum);
		if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
					     JBD_FEATURE_INCOMPAT_64BIT))
			 tag_info->block |=
				 (uint64_t)jbd_get32(tag, blocknr_high) << 32;

		if (jbd_get16(tag, flags) & JBD_FLAG_ESCAPE)
			tag_info->is_escape = true;

		if (!(jbd_get16(tag, flags) & JBD_FLAG_SAME_UUID)) {
			/* See whether it is possible to hold UUID part.*/
			if (remain_buf_size - tag_bytes < UUID_SIZE)
				return EINVAL;

			uuid_start = (char *)tag + tag_bytes;
			tag_info->uuid_exist = true;
			tag_info->tag_bytes += UUID_SIZE;
			memcpy(tag_info->uuid, uuid_start, UUID_SIZE);
		}

		if (jbd_get16(tag, flags) & JBD_FLAG_LAST_TAG)
			tag_info->last_tag = true;

	}
	uint32_t flags = jbd_has_csum(&jbd_fs->sb) == 3 ?
		jbd_get32((struct jbd_block_tag3 *)__tag, flags) :
		jbd_get16((struct jbd_block_tag *)__tag, flags);
	if (flags & ~(JBD_FLAG_ESCAPE | JBD_FLAG_SAME_UUID |
		      JBD_FLAG_DELETED | JBD_FLAG_LAST_TAG))
		return EUCLEAN;
	if (tag_info->block >= jbd_fs->bdev->lg_bcnt ||
	    (tag_info->uuid_exist && memcmp(tag_info->uuid, jbd_fs->sb.uuid, UUID_SIZE)))
		return EUCLEAN;
	return EOK;
}

/**@brief  Write information to a block tag.
 * @param  __tag pointer to the block tag
 * @param  remain_buf_size size in buffer containing the block tag
 * @param  tag_info information of this tag.
 * @return  EOK when succeed, otherwise return EINVAL.*/
static int
jbd_write_block_tag(struct jbd_fs *jbd_fs,
		    void *__tag,
		    int32_t remain_buf_size,
		    struct tag_info *tag_info)
{
	char *uuid_start;
	int tag_bytes = jbd_tag_bytes(jbd_fs);

	tag_info->tag_bytes = tag_bytes;

	/* See whether it is possible to hold a valid block tag.*/
	if (remain_buf_size - tag_bytes < 0)
		return EINVAL;

	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V3)) {
		struct jbd_block_tag3 *tag = __tag;
		memset(tag, 0, sizeof(struct jbd_block_tag3));
		jbd_set32(tag, blocknr, (uint32_t)tag_info->block);
		if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
					     JBD_FEATURE_INCOMPAT_64BIT))
			jbd_set32(tag, blocknr_high, tag_info->block >> 32);

		if (tag_info->uuid_exist) {
			/* See whether it is possible to hold UUID part.*/
			if (remain_buf_size - tag_bytes < UUID_SIZE)
				return EINVAL;

			uuid_start = (char *)tag + tag_bytes;
			tag_info->tag_bytes += UUID_SIZE;
			memcpy(uuid_start, tag_info->uuid, UUID_SIZE);
		} else
			jbd_set32(tag, flags,
				  jbd_get32(tag, flags) | JBD_FLAG_SAME_UUID);

		jbd_block_tag_csum_set(jbd_fs, __tag, tag_info->checksum);

		if (tag_info->last_tag)
			jbd_set32(tag, flags,
				  jbd_get32(tag, flags) | JBD_FLAG_LAST_TAG);

		if (tag_info->is_escape)
			jbd_set32(tag, flags,
				  jbd_get32(tag, flags) | JBD_FLAG_ESCAPE);

	} else {
		struct jbd_block_tag *tag = __tag;
		memset(tag, 0, tag_bytes);
		jbd_set32(tag, blocknr, (uint32_t)tag_info->block);
		if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
					     JBD_FEATURE_INCOMPAT_64BIT))
			jbd_set32(tag, blocknr_high, tag_info->block >> 32);

		if (tag_info->uuid_exist) {
			/* See whether it is possible to hold UUID part.*/
			if (remain_buf_size - tag_bytes < UUID_SIZE)
				return EINVAL;

			uuid_start = (char *)tag + tag_bytes;
			tag_info->tag_bytes += UUID_SIZE;
			memcpy(uuid_start, tag_info->uuid, UUID_SIZE);
		} else
			jbd_set16(tag, flags,
				  jbd_get16(tag, flags) | JBD_FLAG_SAME_UUID);

		jbd_block_tag_csum_set(jbd_fs, __tag, tag_info->checksum);

		if (tag_info->last_tag)
			jbd_set16(tag, flags,
				  jbd_get16(tag, flags) | JBD_FLAG_LAST_TAG);


		if (tag_info->is_escape)
			jbd_set16(tag, flags,
				  jbd_get16(tag, flags) | JBD_FLAG_ESCAPE);

	}
	return EOK;
}

/**@brief  Iterate all block tags in a block.
 * @param  jbd_fs jbd filesystem
 * @param  __tag_start pointer to the block
 * @param  tag_tbl_size size of the block
 * @param  func callback routine to indicate that
 *         a block tag is found
 * @param  arg additional argument to be passed to func */
static int
jbd_iterate_block_table(struct jbd_fs *jbd_fs,
			void *__tag_start,
			int32_t tag_tbl_size,
			void (*func)(struct jbd_fs * jbd_fs,
				     struct tag_info *tag_info,
				     void *arg),
			void *arg)
{
	char *tag_start, *tag_ptr;
	uint32_t tags = 0;
	int tag_bytes = jbd_tag_bytes(jbd_fs);
	tag_start = __tag_start;
	tag_ptr = tag_start;

	/* Cut off the size of block tail storing checksum. */
	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V2) ||
	    JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V3))
		tag_tbl_size -= sizeof(struct jbd_block_tail);

	while (tag_tbl_size > 0) {
		struct tag_info tag_info;
		int rc = jbd_extract_block_tag(jbd_fs,
				      tag_ptr,
				      tag_bytes,
				      tag_tbl_size,
				      &tag_info);
		if (rc != EOK)
			return EUCLEAN;

		if (++tags >= jbd_get32(&jbd_fs->sb, maxlen) - jbd_get32(&jbd_fs->sb, first))
			return EUCLEAN;

		if (func)
			func(jbd_fs, &tag_info, arg);

		/* Stop the iteration when we reach the last tag. */
		if (tag_info.last_tag)
			return EOK;

		tag_ptr += tag_info.tag_bytes;
		tag_tbl_size -= tag_info.tag_bytes;
	}
	return EUCLEAN; /* No complete LAST_TAG within the descriptor payload. */
}

static void jbd_display_block_tags(struct jbd_fs *jbd_fs,
				   struct tag_info *tag_info __unused,
				   void *arg)
{
	uint32_t *iblock = arg;
	ext4_dbg(DEBUG_JBD, "Block in block_tag: %" PRIu64 "\n", tag_info->block);
	(*iblock)++;
	wrap(&jbd_fs->sb, *iblock);
	(void)jbd_fs;
	return;
}

static struct revoke_entry *
jbd_revoke_entry_lookup(struct recover_info *info, ext4_fsblk_t block)
{
	struct revoke_entry tmp = {
		.block = block
	};

	return RB_FIND(jbd_revoke, &info->revoke_root, &tmp);
}

/**@brief  Replay a block in a transaction.
 * @param  jbd_fs jbd filesystem
 * @param  tag_info tag_info of the logged block.*/
static void jbd_replay_block_tags(struct jbd_fs *jbd_fs,
				  struct tag_info *tag_info, void *__arg)
{
	struct replay_arg *arg = __arg;
	struct recover_info *info = arg->info;
	struct ext4_fs *fs = jbd_fs->inode_ref.fs;
	struct ext4_block log = EXT4_BLOCK_ZERO();
	int r;
	(*arg->this_block)++;
	wrap(&jbd_fs->sb, *arg->this_block);
	if (info->error)
		return;
	struct revoke_entry *revoke = jbd_revoke_entry_lookup(info, tag_info->block);
	if (revoke && trans_id_diff(arg->this_trans_id, revoke->trans_id) <= 0)
		return;
	r = jbd_block_get(jbd_fs, &log, *arg->this_block);
	if (r != EOK) {
		info->error = r;
		return;
	}
	if (jbd_has_csum(&jbd_fs->sb)) {
		uint32_t csum = jbd_block_csum(jbd_fs, log.data, 0, arg->this_trans_id, false);
		if (jbd_has_csum(&jbd_fs->sb) == 2)
			csum = (uint16_t)csum;
		if (csum != tag_info->checksum) {
			jbd_block_set(jbd_fs, &log);
			info->error = EIO;
			return;
		}
	}
	if (tag_info->is_escape)
		((struct jbd_bhdr *)log.data)->magic = to_be32(JBD_MAGIC_NUMBER);
	uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
	bool is_super = tag_info->block == EXT4_SUPERBLOCK_OFFSET / block_size;
	if (is_super) {
		struct ext4_sblock *super = (void *)(log.data +
			EXT4_SUPERBLOCK_OFFSET % block_size);
		/* Never manufacture a good checksum for a corrupt logged image.
		 * Bootstrap geometry and journal identity cannot change in replay. */
		if (!ext4_sb_check(super) || ext4_sb_get_block_size(super) != block_size ||
		    memcmp(super->uuid, fs->sb.uuid, UUID_SIZE)) {
			jbd_block_set(jbd_fs, &log);
			info->error = EUCLEAN;
			return;
		}
	}
	r = ext4_blocks_set_direct(fs->bdev, log.data, tag_info->block, 1);
	if (r == EOK && is_super) {
		memcpy(&fs->sb, log.data + EXT4_SUPERBLOCK_OFFSET % block_size,
		       EXT4_SUPERBLOCK_SIZE);
		info->super_replayed = true;
	}
	if (r == EOK) {
		/* Recovery precedes normal operations, but inode 8 and mount
		 * metadata can already be cached. Refresh those live buffers too. */
		struct ext4_block cached = EXT4_BLOCK_ZERO();
		struct ext4_buf *buf = ext4_bcache_find_get(fs->bdev->bc, &cached,
							  tag_info->block, NULL);
		if (buf) {
			memcpy(buf->data, log.data, block_size);
			ext4_bcache_clear_dirty(buf);
			ext4_bcache_set_flag(buf, BC_UPTODATE);
			r = ext4_block_set(fs->bdev, &cached);
		}
	}
	int release = jbd_block_set(jbd_fs, &log);
	info->error = r != EOK ? r : release;
}

/**@brief  Add block address to revoke tree, along with
 *         its transaction id.
 * @param  info  journal replay info
 * @param  block  block address to be replayed.*/
static void jbd_add_revoke_block_tags(struct recover_info *info,
				      ext4_fsblk_t block)
{
	struct revoke_entry *revoke_entry;

	ext4_dbg(DEBUG_JBD, "Add block %" PRIu64 " to revoke tree\n", block);
	/* If the revoke entry with respect to the block address
	 * exists already, update its transaction id.*/
	revoke_entry = jbd_revoke_entry_lookup(info, block);
	if (revoke_entry) {
		revoke_entry->trans_id = info->this_trans_id;
		return;
	}

	revoke_entry = jbd_alloc_revoke_entry();
	if (!revoke_entry) {
		info->error = ENOMEM;
		return;
	}
	revoke_entry->block = block;
	revoke_entry->trans_id = info->this_trans_id;
	RB_INSERT(jbd_revoke, &info->revoke_root, revoke_entry);

	return;
}

static void jbd_destroy_revoke_tree(struct recover_info *info)
{
	while (!RB_EMPTY(&info->revoke_root)) {
		struct revoke_entry *revoke_entry =
			RB_MIN(jbd_revoke, &info->revoke_root);
		ext4_assert(revoke_entry);
		RB_REMOVE(jbd_revoke, &info->revoke_root, revoke_entry);
		jbd_free_revoke_entry(revoke_entry);
	}
}


#define ACTION_SCAN 0
#define ACTION_REVOKE 1
#define ACTION_RECOVER 2

/**@brief  Add entries in a revoke block to revoke tree.
 * @param  jbd_fs jbd filesystem
 * @param  header revoke block header
 * @param  info  journal replay info*/
static int jbd_build_revoke_tree(struct jbd_fs *jbd_fs,
				  struct jbd_bhdr *header,
				  struct recover_info *info)
{
	char *blocks_entry;
	struct jbd_revoke_header *revoke_hdr =
		(struct jbd_revoke_header *)header;
	uint32_t i, nr_entries, record_len = 4;

	/* If we are working on a 64bit jbd filesystem, */
	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_64BIT))
		record_len = 8;

	uint32_t count = jbd_get32(revoke_hdr, count);
	uint32_t payload = jbd_get32(&jbd_fs->sb, blocksize);
	if (jbd_has_csum(&jbd_fs->sb))
		payload -= sizeof(struct jbd_revoke_tail);
	if (count < sizeof(*revoke_hdr) || count > payload ||
	    (count - sizeof(*revoke_hdr)) % record_len)
		return EUCLEAN;
	nr_entries = (count - sizeof(*revoke_hdr)) / record_len;
	if (!info)
		return EOK; /* SCAN validates geometry without building the tree. */

	blocks_entry = (char *)(revoke_hdr + 1);

	for (i = 0;i < nr_entries;i++) {
		if (record_len == 8) {
			uint64_t *blocks =
				(uint64_t *)blocks_entry;
			jbd_add_revoke_block_tags(info, to_be64(*blocks));
		} else {
			uint32_t *blocks =
				(uint32_t *)blocks_entry;
			jbd_add_revoke_block_tags(info, to_be32(*blocks));
		}
		blocks_entry += record_len;
	}
	return info->error;
}

static int jbd_debug_descriptor_block(struct jbd_fs *jbd_fs,
				       struct jbd_bhdr *header,
				       uint32_t *iblock)
{
	return jbd_iterate_block_table(jbd_fs,
				header + 1,
				jbd_get32(&jbd_fs->sb, blocksize) -
					sizeof(struct jbd_bhdr),
				jbd_display_block_tags,
				iblock);
}

static int jbd_replay_descriptor_block(struct jbd_fs *jbd_fs,
					struct jbd_bhdr *header,
					struct replay_arg *arg)
{
	return jbd_iterate_block_table(jbd_fs,
				header + 1,
				jbd_get32(&jbd_fs->sb, blocksize) -
					sizeof(struct jbd_bhdr),
				jbd_replay_block_tags,
				arg);
}

/**@brief  The core routine of journal replay.
 * @param  jbd_fs jbd filesystem
 * @param  info  journal replay info
 * @param  action action needed to be taken
 * @return standard error code*/
/* Linux fs/jbd2/recovery.c at f4cdf7ca9a1f defers a metadata
 * checksum mismatch during SCAN until a matching commit distinguishes a
 * committed corruption from interrupted or stale log contents. */
static int jbd_iterate_log(struct jbd_fs *jbd_fs,
			   struct recover_info *info, int action)
{
	struct jbd_sb *sb = &jbd_fs->sb;
	uint32_t id = jbd_get32(sb, sequence);
	uint32_t first_id = id;
	uint32_t position = jbd_get32(sb, start);
	uint32_t capacity = jbd_get32(sb, maxlen) - jbd_get32(sb, first);
	uint32_t remaining = capacity;
	uint64_t last_commit_time = 0;
	bool suspect_metadata = false;
	bool ended = false;
	if (action == ACTION_SCAN)
		info->trans_cnt = 0;
	else if (!info->trans_cnt)
		return EOK;
	while (remaining && !ended) {
		if (action != ACTION_SCAN && trans_id_diff(id, info->last_trans_id) > 0)
			break;
		struct ext4_block block = EXT4_BLOCK_ZERO();
		uint32_t at = position;
		int r = jbd_block_get(jbd_fs, &block, position);
		if (r != EOK)
			return r;
		struct jbd_bhdr *header = (void *)block.data;
		if (jbd_get32(header, magic) != JBD_MAGIC_NUMBER ||
		    jbd_get32(header, sequence) != id) {
			ended = true;
		} else switch (jbd_get32(header, blocktype)) {
		case JBD_DESCRIPTOR_BLOCK:
			if (!jbd_verify_meta_csum(jbd_fs, header)) {
				if (action != ACTION_SCAN) { r = EIO; break; }
				suspect_metadata = true;
			}
			if (action == ACTION_RECOVER) {
				struct replay_arg arg = { info, &position, id };
				r = jbd_replay_descriptor_block(jbd_fs, header, &arg);
			} else
				r = jbd_debug_descriptor_block(jbd_fs, header, &position);
			break;
		case JBD_REVOKE_BLOCK:
			/* Check lengths before accessing entries, including in SCAN. */
			r = jbd_build_revoke_tree(jbd_fs, header, NULL);
			if (r != EOK)
				break;
			if (!jbd_verify_meta_csum(jbd_fs, header)) {
				if (action != ACTION_SCAN) { r = EIO; break; }
				suspect_metadata = true;
			}
			if (action == ACTION_REVOKE) {
				info->this_trans_id = id;
				r = jbd_build_revoke_tree(jbd_fs, header, info);
			}
			break;
		case JBD_COMMIT_BLOCK: {
			struct jbd_commit_header *commit = (void *)header;
			uint64_t time = to_be64(commit->commit_sec);
			if (suspect_metadata) {
				if (time >= last_commit_time)
					r = EIO;
				else
					ended = true;
				break;
			}
			bool valid = jbd_verify_commit_csum(jbd_fs, commit);
			if (!valid) {
				/* Linux accepts an intact commit header when the unused
				 * padding sectors did not reach disk. */
				void *partial = ext4_calloc(1, jbd_get32(sb, blocksize));
				if (!partial) { r = ENOMEM; break; }
				memcpy(partial, commit, sizeof(*commit));
				valid = jbd_verify_commit_csum(jbd_fs, partial);
				ext4_free(partial);
			}
			if (!valid) {
				if (time < last_commit_time)
					ended = true;
				else
					r = EIO;
				break;
			}
			last_commit_time = time;
			id++;
			if (action == ACTION_SCAN)
				info->trans_cnt++;
			break;
		}
		default:
			ended = true;
			break;
		}
		int release = jbd_block_set(jbd_fs, &block);
		if (r != EOK || info->error || release != EOK)
			return r != EOK ? r : info->error ? info->error : release;
		uint32_t skipped = position >= at ? position - at : capacity - (at - position);
		if (skipped >= remaining) {
			ended = true;
			break;
		}
		remaining -= skipped + 1;
		position++;
		wrap(sb, position);
	}
	if (action == ACTION_SCAN) {
		info->start_trans_id = first_id;
		info->last_trans_id = info->trans_cnt ? id - 1 : first_id;
	} else if (id != info->last_trans_id + 1) {
		/* A later pass must reach exactly the end proven by SCAN. */
		return EIO;
	}
	return EOK;
}

/**@brief  Replay journal.
 * @param  jbd_fs jbd filesystem
 * @return standard error code*/
int jbd_recover(struct jbd_fs *jbd_fs)
{
	int r;
	struct recover_info info = {0};
	struct ext4_fs *fs = jbd_fs->inode_ref.fs;
	r = jbd_check_features(jbd_fs);
	if (r != EOK)
		return r;
	if (!jbd_fs->sb.start)
		return fs->super_replay_required ? EUCLEAN : EOK;
	if (fs->read_only)
		return EROFS;
	if (!jbd_fs->bdev->bdif->flush)
		return ENOTSUP;
	RB_INIT(&info.revoke_root);
	r = jbd_iterate_log(jbd_fs, &info, ACTION_SCAN);
	if (r == EOK)
		r = jbd_iterate_log(jbd_fs, &info, ACTION_REVOKE);
	if (r == EOK)
		r = jbd_iterate_log(jbd_fs, &info, ACTION_RECOVER);
	if (r == EOK && fs->super_replay_required && !info.super_replayed)
		r = EUCLEAN;
	/* Never discard recovery information before all home writes persist. */
	if (r == EOK)
		r = ext4_blockdev_flush(jbd_fs->bdev);
	if (r == EOK) {
		fs->super_replay_required = false;
		jbd_set32(&jbd_fs->sb, start, 0);
		jbd_set32(&jbd_fs->sb, sequence, info.last_trans_id + 1);
		jbd_fs->dirty = true;
		r = jbd_write_sb(jbd_fs);
		if (r == EOK)
			r = ext4_blockdev_flush(jbd_fs->bdev);
	}
	/* RECOVER is changed only by the journalled start/stop superblock
	 * transactions. A direct write here, after discarding the log, could
	 * tear the 1024-byte primary across physical 512-byte sectors. */
	jbd_destroy_revoke_tree(&info);
	return r;
}

static void jbd_journal_write_sb(struct jbd_journal *journal)
{
	struct jbd_fs *jbd_fs = journal->jbd_fs;
	jbd_set32(&jbd_fs->sb, start, journal->start);
	jbd_set32(&jbd_fs->sb, sequence, journal->trans_id);
	jbd_fs->dirty = true;
}

/* Publish mount state through the same durable log/checkpoint protocol as
 * ordinary metadata. A torn primary always retains a complete replay image. */
static int jbd_journal_update_super(struct jbd_journal *journal, bool active)
{
	COST_PHASE_SCOPE(read_source, 6);
	struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
	struct ext4_sblock before = fs->sb;
	struct ext4_block block = EXT4_BLOCK_ZERO();
	struct jbd_trans *trans;
	int r = jbd_journal_new_trans(journal, &trans);
	if (r) return r;
	r = ext4_block_get(fs->bdev, &block,
		EXT4_SUPERBLOCK_OFFSET / journal->block_size);
	if (r == EOK)
		r = jbd_trans_get_write_access(trans, &block);
	if (r == EOK) {
		uint32_t features = ext4_get32(&fs->sb, features_incompatible);
		ext4_set32(&fs->sb, features_incompatible, active ?
			features | EXT4_FINCOM_RECOVER : features & ~EXT4_FINCOM_RECOVER);
		if (active)
			ext4_set16(&fs->sb, mount_count,
				ext4_get16(&fs->sb, mount_count) + 1);
		ext4_sb_set_csum(&fs->sb);
		memcpy(block.data + EXT4_SUPERBLOCK_OFFSET % journal->block_size,
		       &fs->sb, sizeof(fs->sb));
		r = jbd_trans_set_block_dirty(trans, &block);
	}
	if (block.data) {
		fs->bdev->cache_write_back++;
		int release = ext4_block_set(fs->bdev, &block);
		fs->bdev->cache_write_back--;
		if (r == EOK)
			r = release;
	}
	if (r == EOK) r = jbd_trans_prepare_operation(trans);
	if (r != EOK) {
		jbd_journal_free_trans(journal, trans, true);
		fs->sb = before;
		return r;
	}
	r = jbd_journal_commit_trans(journal, trans);
	if (r != EOK && !jbd_journal_error(journal))
		fs->sb = before;
	return r;
}

/**@brief  Start accessing the journal.
 * @param  jbd_fs jbd filesystem
 * @param  journal current journal session
 * @return standard error code*/
static void jbd_release_session_storage(struct jbd_journal *journal)
{
    jbd_image_free(journal->commit_image);
    jbd_image_free(journal->checkpoint_image);
    journal->commit_image = journal->checkpoint_image = NULL;
    jbd_free(journal->log_map);
    journal->log_map = NULL;
}

static int jbd_prepare_session_storage(struct jbd_journal *journal)
{
    uint32_t blocks = jbd_get32(&journal->jbd_fs->sb, maxlen);
    size_t bytes = (size_t)blocks * sizeof(*journal->log_map);
    if (bytes / sizeof(*journal->log_map) != blocks) return ENOMEM;
    journal->log_map = jbd_alloc(journal, bytes);
    if (!journal->log_map) return ENOMEM;
    for (uint32_t i = 0; i < blocks; i++) {
        int result = jbd_inode_bmap(journal->jbd_fs, i, &journal->log_map[i]);
        if (result) return result;
    }
    journal->commit_image = jbd_image_alloc(journal);
    journal->checkpoint_image = jbd_image_alloc(journal);
    if (!journal->commit_image || !journal->checkpoint_image) return ENOMEM;
    /* Only start/sequence/checksum change during this session. Keep the
     * immutable parameters here so checkpoint need not copy a live sb. */
    memcpy(journal->checkpoint_image->data, &journal->jbd_fs->sb, sizeof(struct jbd_sb));
    memset((uint8_t *)journal->checkpoint_image->data + sizeof(struct jbd_sb), 0,
        journal->block_size - sizeof(struct jbd_sb));
    return EOK;
}

int jbd_journal_start(struct jbd_fs *jbd_fs,
		      struct jbd_journal *journal)
{
	int r;
	struct ext4_fs *fs = jbd_fs->inode_ref.fs;
	memset(journal, 0, sizeof(*journal));
	if (fs->read_only)
		return EROFS;
	r = jbd_check_features(jbd_fs);
	if (r != EOK)
		return r;
	if (jbd_fs->sb.start || fs->super_replay_required)
		return EUCLEAN;
	if (!jbd_fs->bdev->bdif->flush)
		return ENOTSUP;
	journal->first = jbd_get32(&jbd_fs->sb, first);
	journal->start = journal->first;
	journal->last = journal->first;
	/*
	 * To invalidate any stale records we need to start from
	 * the checkpoint transaction ID of the previous journalling session
	 * plus 1.
	 */
	journal->trans_id = jbd_get32(&jbd_fs->sb, sequence) + 1;
	journal->alloc_trans_id = journal->trans_id;
	journal->committed_id = journal->trans_id - 1;

	journal->block_size = jbd_get32(&jbd_fs->sb, blocksize);
    r = jbd_memory_start(journal, journal->block_size);
    if (r) return r;

	TAILQ_INIT(&journal->cp_queue);
	RB_INIT(&journal->block_rec_root);
	RB_INIT(&journal->sources);
	journal->jbd_fs = jbd_fs;
    TAILQ_INIT(&journal->sealed_queue);
    r = jbd_prepare_session_storage(journal);
    if (r) {
        jbd_release_session_storage(journal);
        jbd_memory_stop(journal);
        return r;
    }
	/* From this point the mount owns a live journal, including on failure. */
	jbd_fs->bdev->journal = journal;
	fs->journaled_session = true;
	jbd_journal_write_sb(journal);
	r = jbd_write_sb(jbd_fs);
	if (r == EOK)
		r = ext4_blockdev_flush(jbd_fs->bdev);
	if (r != EOK) {
		__atomic_store_n(&journal->error,r,__ATOMIC_RELEASE);
		return r;
	}
	r = jbd_journal_update_super(journal, true);
	if (r == EOK)
		r = jbd_journal_purge_cp_trans(journal, true, false);
	else if (!jbd_journal_error(journal)) {
		/* A resource failure submitted no commit. Close the empty log so
		 * a later start can retry without an artificial recovery owner. */
		journal->start = 0;
		jbd_journal_write_sb(journal);
		int cleanup = jbd_write_sb(jbd_fs);
		if (cleanup == EOK)
			cleanup = ext4_blockdev_flush(jbd_fs->bdev);
		if (cleanup != EOK) {
			r = cleanup;
			__atomic_store_n(&journal->error,cleanup,__ATOMIC_RELEASE);
		}
        else {
            jbd_fs->bdev->journal = NULL;
            jbd_release_session_storage(journal);
            jbd_memory_stop(journal);
        }
	}
	return r;
}

static void jbd_trans_end_write(struct ext4_bcache *bc __unused,
			  struct ext4_buf *buf __unused,
			  int res,
			  void *arg);

/*
 * This routine is only suitable to committed transactions. */
static int jbd_journal_flush_trans(struct jbd_trans *trans)
{
	struct jbd_journal *journal = trans->journal;
	struct ext4_blockdev *bdev = journal->jbd_fs->bdev;
	while (!TAILQ_EMPTY(&trans->buf_queue)) {
		struct jbd_buf *jb = TAILQ_FIRST(&trans->buf_queue);
		struct ext4_block block = EXT4_BLOCK_ZERO();
		struct ext4_buf *buf = ext4_bcache_find_get(bdev->bc, &block,
							jb->block_rec->lba, NULL);
		int r;
		if (buf && ext4_bcache_test_flag(buf, BC_UPTODATE) &&
		    jb->block_rec->trans == trans) {
			r = ext4_block_flush_buf(bdev, buf);
		} else {
			struct ext4_block log = EXT4_BLOCK_ZERO();
			r = jbd_block_get(journal->jbd_fs, &log, jb->jbd_lba);
			if (r == EOK) {
				/* Restore the escaped magic before writing the home block. */
				if (jb->escaped)
					((struct jbd_bhdr *)log.data)->magic = to_be32(JBD_MAGIC_NUMBER);
				r = ext4_blocks_set_direct(bdev, log.data, jb->block_rec->lba, 1);
				int release = jbd_block_set(journal->jbd_fs, &log);
				if (r == EOK)
					r = release;
			}
			jbd_trans_end_write(bdev->bc, buf, r, jb);
		}
		if (buf) {
			/* Avoid a second, implicit home write on the error path. */
			bdev->cache_write_back++;
			int release = ext4_block_set(bdev, &block);
			bdev->cache_write_back--;
			if (r == EOK)
				r = release;
		}
		if (r != EOK)
			return r;
	}
	return EOK;
}

int jbd_journal_purge_cp_trans(struct jbd_journal *journal,
			      bool flush, bool once)
{
	struct jbd_trans *trans;
	int r;
	if (jbd_journal_error(journal))
		return jbd_journal_error(journal);
	while ((trans = TAILQ_FIRST(&journal->cp_queue))) {
		if (trans->written_cnt != trans->data_cnt) {
			if (!flush)
				break;
			r = jbd_journal_flush_trans(trans);
			if (r != EOK)
				goto Error;
		}
		/* Completed writes are still volatile until this barrier. */
		r = ext4_blockdev_flush(journal->jbd_fs->bdev);
		if (r != EOK)
			goto Error;
		uint32_t old_start = journal->start, old_id = journal->trans_id;
		journal->start = trans->start_iblock + trans->alloc_blocks;
		wrap(&journal->jbd_fs->sb, journal->start);
		journal->trans_id = trans->trans_id + 1;
		jbd_journal_write_sb(journal);
		r = jbd_write_sb(journal->jbd_fs);
		if (r == EOK)
			r = ext4_blockdev_flush(journal->jbd_fs->bdev);
		if (r != EOK) {
			journal->start = old_start;
			journal->trans_id = old_id;
			jbd_journal_write_sb(journal);
			goto Error;
		}
		TAILQ_REMOVE(&journal->cp_queue, trans, trans_node);
		jbd_journal_free_trans(journal, trans, false);
		if (once)
			break;
	}
	return EOK;
Error:
	__atomic_store_n(&journal->error,r,__ATOMIC_RELEASE);
	return r;
}

int jbd_journal_sync(struct jbd_journal *journal, uint32_t trans_id)
{
	if (jbd_journal_error(journal))
		return jbd_journal_error(journal);
	return trans_id_diff(trans_id, journal->committed_id) > 0 ? EAGAIN : EOK;
}

/**@brief  Stop accessing the journal.
 * @param  journal current journal session
 * @return standard error code*/
int jbd_journal_stop(struct jbd_journal *journal)
{
	int r;
	struct jbd_fs *jbd_fs = journal->jbd_fs;
	if (jbd_fs->inode_ref.fs->read_only)
		return EROFS;
	if (jbd_journal_error(journal))
		return jbd_journal_error(journal);
	r = jbd_journal_update_super(journal, false);
	if (r != EOK)
		return r;
	/* The clean primary is stable before its recovery image is reclaimed. */
	r = jbd_journal_purge_cp_trans(journal, true, false);
	if (r != EOK)
		return r;

	journal->start = 0;
	jbd_journal_write_sb(journal);
	r = jbd_write_sb(journal->jbd_fs);
	if (r == EOK)
		r = ext4_blockdev_flush(jbd_fs->bdev);
	if (r != EOK)
		__atomic_store_n(&journal->error,r,__ATOMIC_RELEASE);
    else {
        jbd_release_session_storage(journal);
        jbd_memory_stop(journal);
    }
	return r;
}

static struct jbd_block_rec *
jbd_trans_block_rec_lookup(struct jbd_journal *journal,
			   ext4_fsblk_t lba)
{
	struct jbd_block_rec tmp = {
		.lba = lba
	};

	return RB_FIND(jbd_block,
		       &journal->block_rec_root,
		       &tmp);
}

/* Caller holds journal state. The running group already owns a wire record
 * for each of its LBAs and revokes; duplicate private changes add no record. */
static int jbd_group_requirements(struct jbd_journal *journal,
    struct jbd_trans *running, struct jbd_trans *operation,
    unsigned *credits, unsigned *images)
{
    unsigned payloads = running->data_cnt, revokes = running->revoke_count;
    uint32_t capacity = jbd_get32(&journal->jbd_fs->sb, maxlen) - journal->first;
    struct jbd_buf *jb;
    TAILQ_FOREACH(jb, &operation->buf_queue, buf_node) {
        struct jbd_block_rec *record = jbd_trans_block_rec_lookup(journal, jb->block.lb_id);
        if (record && record->trans == running) continue;
        if (payloads >= capacity) return ENOSPC;
        payloads++;
    }
    struct jbd_revoke_rec *revoke;
    RB_FOREACH(revoke, jbd_revoke_tree, &operation->revoke_root) {
        if (RB_FIND(jbd_revoke_tree, &running->revoke_root, revoke)) continue;
        if (revokes == UINT32_MAX) return ENOSPC;
        revokes++;
    }
    *images = jbd_log_images_needed(journal, payloads, revokes, true);
    if (payloads >= capacity || *images >= capacity - payloads) return ENOSPC;
    *credits = payloads + *images;
    return EOK;
}

static void
jbd_trans_change_ownership(struct jbd_block_rec *block_rec,
			   struct jbd_trans *new_trans)
{
	if (block_rec->trans) LIST_REMOVE(block_rec, tbrec_node);
	if (new_trans) {
		/* Now this block record belongs to this transaction. */
		LIST_INSERT_HEAD(&new_trans->tbrec_list, block_rec, tbrec_node);
	}
	block_rec->trans = new_trans;
}

/*
 * This routine will do the dirty works.
 */
static void
jbd_trans_finish_callback(struct jbd_journal *journal,
			  const struct jbd_trans *trans,
			  struct jbd_block_rec *block_rec)
{
    if (block_rec->trans != trans) return;
    struct jbd_buf *jb, *next;
    TAILQ_FOREACH_SAFE(jb, &block_rec->dirty_buf_queue, dirty_buf_node, next)
        jbd_trans_end_write(journal->jbd_fs->bdev->bc, NULL, EOK, jb);
}

/* Private preparations retain only the index identity, not the retired
 * transaction or its payload. Their last release can retire an empty entry. */
static void jbd_retire_block_rec(struct jbd_journal *journal,
    struct jbd_block_rec *record)
{
    if (record->trans || record->private_refs) return;
    if (record->checkpoint || !TAILQ_EMPTY(&record->dirty_buf_queue)) __builtin_trap();
    RB_REMOVE(jbd_block, &journal->block_rec_root, record);
    jbd_free(record);
}

static struct jbd_block_rec *jbd_borrow_block_rec(struct jbd_journal *journal,
    ext4_fsblk_t lba)
{
    struct jbd_block_rec *record = jbd_trans_block_rec_lookup(journal, lba);
    if (record) {
        if (record->private_refs == UINT32_MAX) __builtin_trap();
        record->private_refs++;
    }
    return record;
}

static void jbd_put_private_record(struct jbd_journal *journal,
    struct jbd_block_rec *record)
{
    if (!record) return;
    JBD_STATE_SCOPE(journal);
    if (!record->private_refs) __builtin_trap();
    record->private_refs--;
    jbd_retire_block_rec(journal, record);
}

static inline void
jbd_trans_remove_block_rec(struct jbd_journal *journal,
			   struct jbd_block_rec *block_rec,
			   struct jbd_trans *trans)
{
	/* If this block record doesn't belong to this transaction,
	 * give up.*/
	if (block_rec->trans == trans) {
		LIST_REMOVE(block_rec, tbrec_node);
		block_rec->trans = NULL;
		jbd_retire_block_rec(journal, block_rec);
	}
}

static int jbd_data_cmp(struct jbd_data *a, struct jbd_data *b)
{
	return a->block.lb_id < b->block.lb_id ? -1 : a->block.lb_id > b->block.lb_id;
}
RB_GENERATE_INTERNAL(jbd_data_tree, jbd_data, index_node, jbd_data_cmp, static inline)

static struct jbd_data *jbd_trans_find_data(struct jbd_trans *trans, ext4_fsblk_t lba)
{
	if (!trans) return NULL;
	struct jbd_data key = { .block.lb_id = lba };
	return RB_FIND(jbd_data_tree, &trans->data_root, &key);
}

struct jbd_source {
    struct file_data_source owner;
    struct jbd_memory *memory;
    size_t blocks;
    uint32_t references;
    RB_ENTRY(jbd_source) node;
};

bool jbd_journal_borrow_data(struct ext4_fs *fs, ext4_fsblk_t lba,
    struct jbd_data_borrow *borrow)
{
    *borrow = (struct jbd_data_borrow){0};
    struct jbd_journal *journal = fs->jbd_journal;
    if (!journal) return false;
    JBD_STATE_READ_SCOPE(journal);
    struct jbd_data *data = jbd_trans_find_data(ext4_current_trans(fs), lba);
    if (!data) data = jbd_trans_find_data(journal->running, lba);
    if (!data) {
        struct jbd_trans *trans;
        TAILQ_FOREACH(trans, &journal->sealed_queue, trans_node) {
            struct jbd_data *candidate = jbd_trans_find_data(trans, lba);
            if (candidate) data = candidate;
        }
    }
    if (!data) data = jbd_trans_find_data(journal->committing, lba);
    if (!data) data = jbd_trans_find_data(journal->failed_trans, lba);
    if (!data) return false;
    borrow->data = data->block.data;
    if (data->image) {
        borrow->payload = &data->image->payload;
        ext4_block_payload_get(borrow->payload);
    } else {
        borrow->source = data->source;
        if (!borrow->source || __atomic_fetch_add(&borrow->source->references, 1, __ATOMIC_ACQ_REL) == UINT32_MAX)
            __builtin_trap();
    }
    return true;
}
static void jbd_source_put(struct jbd_source *source)
{
    if (__atomic_sub_fetch(&source->references, 1, __ATOMIC_ACQ_REL)) return;
    source->owner.release(source->owner.context);
    jbd_memory_release_borrow(source->memory, source->owner.bytes);
    jbd_free(source);
}
void jbd_journal_release_borrow(struct jbd_data_borrow *borrow)
{
    if (borrow->payload) ext4_block_payload_put(borrow->payload);
    if (borrow->source) jbd_source_put(borrow->source);
    *borrow = (struct jbd_data_borrow){0};
}

static int jbd_source_cmp(struct jbd_source *a, struct jbd_source *b)
{
	uintptr_t first = (uintptr_t)a->owner.context, second = (uintptr_t)b->owner.context;
	return first < second ? -1 : first > second;
}
RB_GENERATE_INTERNAL(jbd_source_tree, jbd_source, node, jbd_source_cmp, static inline)

static int jbd_source_borrow(struct jbd_journal *journal,
	const struct file_data_source *owner, struct jbd_source **out)
{
    JBD_STATE_SCOPE(journal);
	struct jbd_source key = { .owner.context = owner->context };
	struct jbd_source *source = RB_FIND(jbd_source_tree, &journal->sources, &key);
	if (source) {
		if (source->owner.retain != owner->retain || source->owner.release != owner->release ||
		    source->owner.bytes != owner->bytes) return EINVAL;
		if (!source->blocks || source->blocks == SIZE_MAX) __builtin_trap();
		source->blocks++;
        if (__atomic_fetch_add(&source->references, 1, __ATOMIC_ACQ_REL) == UINT32_MAX) __builtin_trap();
	} else {
		size_t control = jbd_allocation_charge(sizeof(*source));
        if (owner->bytes > SIZE_MAX - control || !jbd_memory_borrow(journal, owner->bytes)) return ENOMEM;
        source = jbd_alloc(journal, sizeof(*source));
        if (!source) { jbd_memory_release_borrow(journal->memory, owner->bytes); return ENOMEM; }
		source->owner = *owner;
        source->blocks = 1;
        source->references = 1;
        source->memory = journal->memory;
        owner->retain(owner->context);
		if (RB_INSERT(jbd_source_tree, &journal->sources, source)) __builtin_trap();
	}
	*out = source;
	return EOK;
}

static void jbd_data_release_source(struct jbd_journal *journal, struct jbd_data *data)
{
    struct jbd_source *source;
    {
        JBD_STATE_SCOPE(journal);
        source = data->source;
        if (!source) return;
        data->source = NULL;
        if (!source->blocks) __builtin_trap();
        if (!--source->blocks) RB_REMOVE(jbd_source_tree, &journal->sources, source);
    }
    jbd_source_put(source);
}

static void jbd_data_free(struct jbd_journal *journal, struct jbd_data *data)
{
	jbd_data_release_source(journal, data);
	jbd_image_free(data->image);
	jbd_free(data);
}

int jbd_trans_write_data(struct jbd_trans *trans, ext4_fsblk_t lba,
	uint32_t offset, const void *input, uint32_t length, bool initialize,
	const struct file_data_source *source)
{
	struct jbd_journal *journal = trans->journal;
	struct ext4_blockdev *bdev = journal->jbd_fs->bdev;
	if (jbd_journal_error(journal) || trans->error)
		return jbd_journal_error(journal) ? jbd_journal_error(journal) : trans->error;
	if (source && (!!source->retain != !!source->release || !!source->retain != !!source->bytes))
		return EINVAL;
	if (offset > journal->block_size || length > journal->block_size - offset)
		return EINVAL;
	if (!length) return EOK;
	struct jbd_data *data = jbd_trans_find_data(trans, lba);
	bool fresh = !data;
	if (fresh) {
		data = jbd_alloc(journal, sizeof(*data));
		if (!data) return ENOMEM;
		data->block.lb_id = lba;
		/* Reusing a metadata block must first finish its older checkpoint;
		 * replay of that incarnation is excluded by this operation's revoke. */
		int r = EOK;
        bool prior_metadata;
        {
            JBD_STATE_SCOPE(journal);
            struct jbd_block_rec *rec = jbd_trans_block_rec_lookup(journal, lba);
            prior_metadata = rec && rec->trans;
            if (prior_metadata) r = rec->trans == trans ? EINVAL : jbd_trans_revoke_block(trans, lba);
        }
        if (!r && prior_metadata) r = ext4_block_flush_lba(bdev, lba);
		if (r) { jbd_free(data); return r; }
	}
	int r = EOK;
	bool whole = !offset && length == journal->block_size;
	if (whole && input && source && source->retain && source->release) {
		if (source->bytes < length) { r = EINVAL; goto Failed; }
		struct jbd_source *borrow;
		r = jbd_source_borrow(journal, source, &borrow);
		if (r != EOK) goto Failed;
		jbd_data_release_source(journal, data);
		jbd_image_free(data->image); data->image = NULL;
		data->source = borrow;
		data->block.data = (void *)input;
	} else {
		struct jbd_image *image = data->image;
		if (!image) image = jbd_image_alloc(journal);
		if (!image) { r = ENOMEM; goto Failed; }
		if (!whole && !data->image) {
            struct jbd_data_borrow borrow = {0};
            bool pending = !initialize && jbd_journal_borrow_data(bdev->fs, lba, &borrow);
            if (initialize) memset(image->data, 0, journal->block_size);
            else if (pending) memcpy(image->data, borrow.data, journal->block_size);
            else r = ext4_blocks_get_direct(bdev, image->data, lba, 1);
            jbd_journal_release_borrow(&borrow);
			if (r) { jbd_image_free(image); goto Failed; }
		}
		if (input) memcpy((uint8_t *)image->data + offset, input, length);
		else memset((uint8_t *)image->data + offset, 0, length);
		jbd_data_release_source(journal, data);
		data->image = image;
		data->block.data = image->data;
	}
	if (fresh) {
		RB_INSERT(jbd_data_tree, &trans->data_root, data);
		TAILQ_INSERT_TAIL(&trans->data_queue, data, node);
		trans->ordered_count++;
	}
	return EOK;
Failed:
	if (fresh) jbd_free(data);
	return r;
}

static void jbd_trans_release_data(struct jbd_trans *trans, bool abort __unused)
{
	struct jbd_data *data;
	while ((data = TAILQ_FIRST(&trans->data_queue))) {
		TAILQ_REMOVE(&trans->data_queue, data, node);
		RB_REMOVE(jbd_data_tree, &trans->data_root, data);
		if (!trans->ordered_count) __builtin_trap();
		trans->ordered_count--;
		jbd_data_free(trans->journal, data);
	}
}

static int jbd_trans_write_ordered(struct jbd_trans *trans)
{
    COST_PHASE_SCOPE(data_only, 1);
    struct jbd_io_phase phase = {0};
    jbd_ordered_phase(trans, &phase);
    return ext4_blocks_set_phase(trans->journal->jbd_fs->bdev, phase.first);
}

/**@brief  Add block to a transaction and mark it dirty.
 * @param  trans transaction
 * @param  block block descriptor
 * @return standard error code*/
static uint32_t jbd_range_map_words(const struct jbd_journal *journal)
{ return (journal->block_size + 2047U) / 2048U; }
static uint64_t *jbd_initialized_ranges(struct jbd_buf *jb)
{ return jb->dirty_ranges + jbd_range_map_words(jb->trans->journal); }
static bool jbd_range_bit(const uint64_t *ranges, uint32_t unit)
{ return ranges[unit / 64] & (UINT64_C(1) << (unit % 64)); }
static void jbd_range_set(uint64_t *ranges, uint32_t unit)
{ ranges[unit / 64] |= UINT64_C(1) << (unit % 64); }

enum { JBD_RANGE_ANY = 1, JBD_RANGE_ALL = 2 };
static unsigned jbd_range_test(const uint64_t *ranges, uint32_t first, uint32_t end)
{
    unsigned result = JBD_RANGE_ALL;
    while (first < end) {
        unsigned count = 64 - first % 64;
        if (count > end - first) count = end - first;
        uint64_t mask = (UINT64_MAX >> (64 - count)) << (first % 64);
        uint64_t bits = ranges[first / 64] & mask;
        if (bits) result |= JBD_RANGE_ANY;
        if (bits != mask) result &= ~JBD_RANGE_ALL;
        if (result == JBD_RANGE_ANY) break;
        first += count;
    }
    return result;
}

/* A 4K block has only two map words. Skip a full clean/dirty word at once;
 * the end bound excludes the padding in a 1K block's final word. */
static uint32_t jbd_range_next(const uint64_t *ranges, uint32_t first,
    uint32_t end, bool value)
{
    while (first < end) {
        uint64_t word = value ? ranges[first / 64] : ~ranges[first / 64];
        word >>= first % 64;
        if (word) {
            /* RV's baseline ISA has no ctz instruction or libgcc dependency. */
            unsigned bit = 0;
            if (!(word & UINT32_MAX)) { bit += 32; word >>= 32; }
            if (!(word & 65535U)) { bit += 16; word >>= 16; }
            if (!(word & 255U)) { bit += 8; word >>= 8; }
            if (!(word & 15U)) { bit += 4; word >>= 4; }
            if (!(word & 3U)) { bit += 2; word >>= 2; }
            if (!(word & 1U)) bit++;
            uint32_t next = first + bit;
            return next < end ? next : end;
        }
        first = (first & ~63U) + 64;
    }
    return end;
}
static void jbd_range_fill(uint64_t *ranges, uint32_t first, uint32_t end)
{
    while (first < end) {
        unsigned count = 64 - first % 64;
        if (count > end - first) count = end - first;
        ranges[first / 64] |= (UINT64_MAX >> (64 - count)) << (first % 64);
        first += count;
    }
}

int jbd_trans_set_block_dirty_range(struct jbd_trans *trans,
    struct ext4_block *block, uint32_t offset, uint32_t length)
{
    uint32_t size = trans->journal->block_size;
    if (offset > size || length > size - offset || (offset & 31) || (length & 31)) return EINVAL;
    struct jbd_buf *private;
    TAILQ_FOREACH(private, &trans->buf_queue, buf_node) {
        if (!private->private_access || private->block.lb_id != block->lb_id) continue;
        private->modified = true;
        uint64_t *initialized = jbd_initialized_ranges(private);
        uint32_t first = offset / 32, end = (offset + length) / 32;
        if (!(jbd_range_test(initialized, first, end) & JBD_RANGE_ALL)) __builtin_trap();
        jbd_range_fill(private->dirty_ranges, first, end);
        block->data = private->after->data;
        return EOK;
    }
    return EINVAL; /* Writable metadata must first belong to this operation. */
}


int jbd_trans_set_block_dirty(struct jbd_trans *trans, struct ext4_block *block)
{ return jbd_trans_set_block_dirty_range(trans, block, 0, trans->journal->block_size); }

static struct jbd_buf *jbd_trans_find_buf(struct jbd_trans *trans, ext4_fsblk_t lba)
{
    struct jbd_buf *buf;
    TAILQ_FOREACH(buf, &trans->buf_queue, buf_node)
        if (buf->block.lb_id == lba) return buf;
    return NULL;
}

static const uint8_t *jbd_before_data(const struct jbd_buf *jb)
{ return jb->before ? jb->before->data : jb->block.payload->data; }

static void jbd_copy_initialized(struct jbd_buf *jb, uint8_t *destination,
    const uint8_t *source)
{
    const uint64_t *initialized = jbd_initialized_ranges(jb);
    uint32_t unit = 0, end = jb->trans->journal->block_size / 32;
    while ((unit = jbd_range_next(initialized, unit, end, true)) < end) {
        uint32_t first = unit;
        unit = jbd_range_next(initialized, first, end, false);
        memcpy(destination + first * 32U, source + first * 32U, (unit - first) * 32U);
    }
}

static int jbd_materialize_before(struct jbd_buf *jb)
{
    if (jb->before) return EOK;
    struct jbd_image *image = jbd_image_alloc(jb->trans->journal);
    if (!image) return ENOMEM;
    jbd_copy_initialized(jb, image->data, jb->block.payload->data);
    jb->before = image;
    return EOK;
}

static int jbd_initialize_range(struct jbd_buf *jb,
    const struct ext4_block_payload *accepted, uint32_t offset, uint32_t length,
    bool unchanged)
{
    uint64_t *initialized = jbd_initialized_ranges(jb);
    uint32_t unit = offset / 32, end = (offset + length + 31U) / 32;
    /* The first accepted payload already owns the baseline. Only a clean
     * record which actually starts from newer bytes needs writable storage. */
    if (!jb->before && !unchanged) {
        for (uint32_t next = unit;
             (next = jbd_range_next(jb->dirty_ranges, next, end, false)) < end;) {
            uint32_t first = next;
            next = jbd_range_next(jb->dirty_ranges, first, end, true);
            if (!memcmp(jb->block.payload->data + first * 32U,
                        accepted->data + first * 32U, (next - first) * 32U)) continue;
            int result = jbd_materialize_before(jb);
            if (result) return result;
            break;
        }
    }
    const uint8_t *base = jbd_before_data(jb);
    unsigned present = jbd_range_test(initialized, unit, end);
    if ((present & JBD_RANGE_ALL) &&
        (unchanged || !memcmp(base + unit * 32U,
            accepted->data + unit * 32U, (end - unit) * 32U))) return EOK;
    if (!(present & JBD_RANGE_ANY)) {
        size_t bytes = (end - unit) * 32U;
        if (jb->before)
            memcpy((uint8_t *)jb->before->data + unit * 32U, accepted->data + unit * 32U, bytes);
        memcpy((uint8_t *)jb->after->data + unit * 32U, accepted->data + unit * 32U, bytes);
        jbd_range_fill(initialized, unit, end);
        return EOK;
    }
    while (unit < end) {
        if (jbd_range_bit(initialized, unit) && (jbd_range_bit(jb->dirty_ranges, unit) ||
            !memcmp(base + unit * 32U, accepted->data + unit * 32U, 32))) {
            unit++; continue;
        }
        uint32_t first = unit;
        do { jbd_range_set(initialized, unit++); }
        while (unit < end && (!jbd_range_bit(initialized, unit) ||
            (!jbd_range_bit(jb->dirty_ranges, unit) &&
             memcmp(base + unit * 32U, accepted->data + unit * 32U, 32))));
        size_t bytes = (unit - first) * 32U;
        if (jb->before)
            memcpy((uint8_t *)jb->before->data + first * 32U,
                accepted->data + first * 32U, bytes);
        memcpy((uint8_t *)jb->after->data + first * 32U,
            accepted->data + first * 32U, bytes);
    }
    return EOK;
}

static int jbd_rebind_private_range(struct jbd_buf *jb,
    struct ext4_block *block, uint32_t offset, uint32_t length)
{
    if (!jb->private_access) return EOK;
    uint32_t first = offset / 32, end = (offset + length + 31U) / 32;
    int result = EOK;
    /* Dirty units are already initialized and owned by this private view.
     * New accepted bytes matter only for a clean or uninitialized unit;
     * publication still checks every dirty unit against its baseline. */
    if (!(jbd_range_test(jb->dirty_ranges, first, end) & JBD_RANGE_ALL)) {
        struct ext4_block_payload *accepted;
        {
            KERNEL_TASK_LOCK_SCOPE(guard, &block->buf->bc->metadata);
            accepted = block->buf->current_payload;
            ext4_block_payload_get(accepted);
        }
        /* Compare only the requested record's baseline. Another record can
         * be accepted while this task holds an unmarked modified ref. Keep
         * the first accepted baseline identity across clean-range refresh. */
        bool unchanged = jb->block.payload == accepted;
        result = jbd_initialize_range(jb, accepted, offset, length, unchanged);
        ext4_block_payload_put(accepted);
    }
    if (!result) ext4_block_rebind(block, &jb->after->payload);
    return result;
}

int jbd_trans_borrow_private(struct jbd_trans *trans, struct ext4_block *block,
    ext4_fsblk_t lba, uint32_t offset, uint32_t length,
    enum ext4_block_access access, bool *found)
{
    struct jbd_buf *jb = jbd_trans_find_buf(trans, lba);
    *found = jb && jb->private_access;
    if (!*found) return EOK;
    if (access == EXT4_BLOCK_WRITE && (jbd_journal_error(trans->journal) || trans->error))
        return jbd_journal_error(trans->journal) ? jbd_journal_error(trans->journal) : trans->error;
    uint32_t first = offset / 32, end = (offset + length + 31U) / 32;
    bool clean = !(jbd_range_test(jb->dirty_ranges, first, end) & JBD_RANGE_ALL);
    struct ext4_block_payload *newer = ext4_bcache_borrow_owned(&jb->block,
        &jb->after->payload, clean, block);
    int result = clean ? jbd_initialize_range(jb, newer ? newer : jb->block.payload,
        offset, length, !newer) : EOK;
    if (newer) ext4_block_payload_put(newer);
    if (result) ext4_bcache_free_read(block->buf->bc, block);
    return result;
}

int jbd_trans_get_write_access_range(struct jbd_trans *trans,
    struct ext4_block *block, uint32_t offset, uint32_t length)
{
    COST_SCOPE(prepare_cost, JOURNAL_RECORD_PREPARE_TICKS);
    struct jbd_journal *journal = trans->journal;
    if (offset > journal->block_size || !length || length > journal->block_size - offset)
        return EINVAL;
    if (jbd_journal_error(journal) || trans->error) return jbd_journal_error(journal) ? jbd_journal_error(journal) : trans->error;
    struct jbd_buf *jb = jbd_trans_find_buf(trans, block->lb_id);
    if (jb) return jbd_rebind_private_range(jb, block, offset, length);
    /* The home cache stays at its accepted version. All writable references
     * within this operation share one private replacement. */
    jb = jbd_alloc(journal, sizeof(*jb) +
        2 * jbd_range_map_words(journal) * sizeof(*jb->dirty_ranges));
    if (!jb) return ENOMEM;
    jb->after = journal->grouped ? jbd_version(journal, block->buf) : jbd_image_alloc(journal);
    if (!jb->after) { jbd_free(jb); return ENOMEM; }
    /* A reference can survive earlier transactions. Its read view remains
     * stable, but a new write must start from the latest accepted version. */
    jb->trans = trans;
    ext4_bcache_prepare_private(block, &jb->block, &jb->after->payload);
    jb->private_access = true;
    if (jbd_initialize_range(jb, jb->block.payload, offset, length, true)) __builtin_trap();
    TAILQ_INSERT_HEAD(&trans->buf_queue, jb, buf_node);
    trans->data_cnt++;
    return EOK;
}

int jbd_trans_get_write_access(struct jbd_trans *trans, struct ext4_block *block)
{ return jbd_trans_get_write_access_range(trans, block, 0, trans->journal->block_size); }

/* All records and credits were reserved before publication. Validate the full
 * operation before publishing any home block; unrelated accepted bytes are
 * retained when separate records share one physical block. */
static int jbd_publish_private(struct jbd_trans *trans)
{
    COST_SCOPE(publish_cost, JOURNAL_PUBLISH_TICKS);
    struct jbd_journal *journal = trans->journal;
    struct jbd_buf *jb, *super = NULL;
    if (ext4_operation_super_changed(&trans->operation)) {
        struct ext4_operation *op = &trans->operation;
        struct ext4_sblock merged;
        int result = ext4_operation_prepare_super(op, &merged);
        if (result) return result;
        uint64_t lba = EXT4_SUPERBLOCK_OFFSET / journal->block_size;
        super = jbd_trans_find_buf(trans, lba);
        if (!super || !super->private_access || !super->modified) __builtin_trap();
        size_t offset = EXT4_SUPERBLOCK_OFFSET % journal->block_size;
        memcpy((uint8_t *)super->after->data + offset, &merged, sizeof(merged));
    }
    TAILQ_FOREACH(jb, &trans->buf_queue, buf_node) {
        if (!jb->private_access || !jb->modified) continue;
        const uint8_t *base = jbd_before_data(jb), *next = jb->after->data;
        const uint8_t *current = jb->block.buf->data;
        /* Accepted versions are immutable. The original version still being
         * current proves that no competing publication changed these bytes. */
        if (jb->block.buf->current_payload == jb->block.payload) continue;
        uint32_t end = journal->block_size / 32;
        for (uint32_t unit = 0;
             (unit = jbd_range_next(jb->dirty_ranges, unit, end, true)) < end;) {
            uint32_t first = unit;
            unit = jbd_range_next(jb->dirty_ranges, first, end, false);
            if (jb == super) {
                uint32_t start = (EXT4_SUPERBLOCK_OFFSET % journal->block_size) / 32;
                uint32_t finish = start + sizeof(struct ext4_sblock) / 32;
                /* Typed superblock changes were checked and merged against
                 * the latest accepted counters above. Other bytes sharing
                 * this block still require ordinary conflict validation. */
                if (first < start && unit > start) unit = start;
                else if (first >= start && first < finish) {
                    if (unit > finish) unit = finish;
                    continue;
                }
            }
            size_t offset = first * 32U, bytes = (unit - first) * 32U;
            if (!memcmp(current + offset, base + offset, bytes) ||
                !memcmp(current + offset, next + offset, bytes)) continue;
            for (uint32_t i = offset; i < offset + bytes; i++)
                if (current[i] != base[i] && current[i] != next[i]) return EAGAIN;
        }
    }
    TAILQ_FOREACH(jb, &trans->buf_queue, buf_node) {
        if (!jb->private_access || !jb->modified) continue;
        struct ext4_block_payload *current = jb->block.buf->current_payload;
        /* STATE keeps the latest accepted version stable through this merge. */
        uint32_t end = journal->block_size / 32;
        for (uint32_t unit = 0;
             (unit = jbd_range_next(jb->dirty_ranges, unit, end, false)) < end;) {
            uint32_t first = unit;
            unit = jbd_range_next(jb->dirty_ranges, first, end, true);
            memcpy((uint8_t *)jb->after->data + first * 32U,
                current->data + first * 32U, (unit - first) * 32U);
        }
        jbd_image_free(jb->before); jb->before = NULL;
        struct jbd_block_rec *rec = jbd_trans_block_rec_lookup(journal, jb->block.lb_id);
        if (jb->block_rec && jb->block_rec != rec) __builtin_trap();
        if (rec) { jbd_free(jb->reserved_rec); jbd_trans_change_ownership(rec, trans); }
        else {
            rec = jb->reserved_rec;
            if (!rec) __builtin_trap();
            rec->lba = jb->block.lb_id; rec->trans = trans;
            TAILQ_INIT(&rec->dirty_buf_queue);
            LIST_INSERT_HEAD(&trans->tbrec_list, rec, tbrec_node);
            if (RB_INSERT(jbd_block, &journal->block_rec_root, rec)) __builtin_trap();
        }
        /* The accepted transaction now owns the entry before its preparer
         * drops the borrow; no index gap or replacement allocation is needed. */
        jbd_put_private_record(journal, jb->block_rec);
        jb->reserved_rec = NULL;
        jb->block_rec = rec;
        TAILQ_INSERT_TAIL(&rec->dirty_buf_queue, jb, dirty_buf_node);
        jb->block.buf->end_write = jbd_trans_end_write;
        jb->block.buf->end_write_arg = jb;
        ext4_bcache_publish(jb->block.buf, &jb->after->payload);
        ext4_block_rebind(&jb->block, &jb->after->payload);
        jb->private_access = false;
        ext4_bcache_set_dirty(jb->block.buf);
    }
    return EOK;
}

/**@brief  Add block to be revoked to a transaction
 * @param  trans transaction
 * @param  lba logical block address
 * @return standard error code*/
int jbd_trans_revoke_block(struct jbd_trans *trans,
			   ext4_fsblk_t lba)
{
	struct jbd_revoke_rec tmp_rec = {
		.lba = lba
	}, *rec;
	rec = RB_FIND(jbd_revoke_tree,
		      &trans->revoke_root,
		      &tmp_rec);
	if (rec)
		return EOK;

	rec = jbd_alloc(trans->journal, sizeof(struct jbd_revoke_rec));
	if (!rec)
		return ENOMEM;

	rec->lba = lba;
	RB_INSERT(jbd_revoke_tree, &trans->revoke_root, rec);
	trans->revoke_count++;
	return EOK;
}

/**@brief  Try to add block to be revoked to a transaction.
 *         If @lba still remains in an transaction on checkpoint
 *         queue, add @lba as a revoked block to the transaction.
 * @param  trans transaction
 * @param  lba logical block address
 * @return standard error code*/
int jbd_trans_try_revoke_block(struct jbd_trans *trans,
			       ext4_fsblk_t lba)
{
    JBD_STATE_READ_SCOPE(trans->journal);
	struct jbd_journal *journal = trans->journal;
	struct jbd_block_rec *block_rec =
		jbd_trans_block_rec_lookup(journal, lba);

	if (block_rec && block_rec->trans) {
		if (block_rec->trans == trans) {
			struct jbd_buf *jbd_buf =
				TAILQ_LAST(&block_rec->dirty_buf_queue,
					jbd_buf_dirty);
			/* If there are still unwritten buffers. */
			if (TAILQ_FIRST(&block_rec->dirty_buf_queue) !=
			    jbd_buf)
				return jbd_trans_revoke_block(trans, lba);

		} else
			return jbd_trans_revoke_block(trans, lba);
	}

	return EOK;
}

/**@brief  Free a transaction
 * @param  journal current journal session
 * @param  trans transaction
 * @param  abort discard all the modifications on the block?*/
void jbd_journal_free_trans_state_held(struct jbd_journal *journal,
			    struct jbd_trans *trans,
			    bool abort)
{
	struct jbd_buf *jbd_buf, *tmp;
	struct jbd_revoke_rec *rec, *tmp2;
	struct jbd_block_rec *block_rec, *tmp3;
	struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
	jbd_release_prepared(trans);
	jbd_trans_release_data(trans, abort);
	TAILQ_FOREACH_SAFE(jbd_buf, &trans->buf_queue, buf_node,
			  tmp) {
        if (jbd_buf->private_access) {
            if (abort) jbd_copy_initialized(jbd_buf, jbd_buf->after->data, jbd_before_data(jbd_buf));
            TAILQ_REMOVE(&trans->buf_queue, jbd_buf, buf_node);
            if (journal->grouped) jbd_unpin_version(journal, jbd_buf->block.buf);
            ext4_block_set(fs->bdev, &jbd_buf->block);
            jbd_image_free(jbd_buf->after); jbd_image_free(jbd_buf->before);
            jbd_put_private_record(journal, jbd_buf->block_rec);
            jbd_free(jbd_buf->reserved_rec); jbd_free(jbd_buf);
            continue;
        }
		block_rec = jbd_buf->block_rec;
		if (jbd_buf->after && journal->grouped) {
			if (!jbd_buf->block.buf->journal_pending) __builtin_trap();
			jbd_unpin_version(journal, jbd_buf->block.buf);
		}
        if (abort) __builtin_trap();
        TAILQ_REMOVE(&jbd_buf->block_rec->dirty_buf_queue, jbd_buf, dirty_buf_node);
        jbd_trans_finish_callback(journal, trans, block_rec);
		TAILQ_REMOVE(&trans->buf_queue, jbd_buf, buf_node);
		if (jbd_buf->after) {
			jbd_image_free(jbd_buf->after);
		}
		jbd_free(jbd_buf);
	}
	RB_FOREACH_SAFE(rec, jbd_revoke_tree, &trans->revoke_root,
			  tmp2) {
		RB_REMOVE(jbd_revoke_tree, &trans->revoke_root, rec);
		jbd_free(rec);
	}
	LIST_FOREACH_SAFE(block_rec, &trans->tbrec_list, tbrec_node,
			  tmp3) {
		jbd_trans_remove_block_rec(journal, block_rec, trans);
	}

	struct jbd_quarantine *q;
	while ((q = LIST_FIRST(&trans->quarantine))) {
		if (q->owner != trans) __builtin_trap();
        /* Quarantine also owns the cache-retirement intent. Only successful
         * checkpoint retirement has ended every old home/DMA owner; abort
         * leaves accepted bytes and callbacks usable by the restored inode. */
        if (!abort && !q->inode)
            ext4_bcache_invalidate_lba(fs->bdev->bc, q->first, q->count);
		LIST_REMOVE(q, node);
		RB_REMOVE(jbd_quarantine_tree, &journal->quarantine_index[q->inode], q);
		uint64_t *count = q->inode ? &journal->quarantined_inodes : &journal->quarantined_blocks;
		if (*count < q->count) __builtin_trap();
		*count -= q->count;
		jbd_free(q);
	}
	ext4_orphan_trans_finish_state_held(trans, abort);
	ext4_operation_discard_super_state_held(&trans->operation);
	ext4_operation_discard_inode_goals(&trans->operation);
	jbd_free(trans);
}

void jbd_journal_free_trans(struct jbd_journal *journal,
    struct jbd_trans *trans, bool abort)
{
    JBD_STATE_SCOPE(journal);
    jbd_journal_free_trans_state_held(journal, trans, abort);
}

static void jbd_trans_prune(struct jbd_journal *journal, struct jbd_trans *trans)
{
    struct jbd_buf *jb, *next;
    TAILQ_FOREACH_SAFE(jb, &trans->buf_queue, buf_node, next) {
        if (!jb->private_access || jb->block_rec || jb->reserved_rec) __builtin_trap();
        if (jb->modified) continue;
        TAILQ_REMOVE(&trans->buf_queue, jb, buf_node);
        trans->data_cnt--;
        if (journal->grouped) jbd_unpin_version(journal, jb->block.buf);
        ext4_block_set(journal->jbd_fs->bdev, &jb->block);
        jbd_image_free(jb->after);
        jbd_image_free(jb->before);
        jbd_free(jb);
    }
}


/* A frozen group owns its complete ring interval. The cursor is local to its
 * real consumer; all addresses and storage were prepared before publication. */
static uint32_t jbd_wire_next(struct jbd_trans *trans, unsigned *used)
{
    if (*used >= (unsigned)trans->alloc_blocks) __builtin_trap();
    uint64_t block = (uint64_t)trans->start_iblock + (*used)++;
    wrap(&trans->journal->jbd_fs->sb, block);
    return (uint32_t)block;
}

static int jbd_submit_phase(struct jbd_trans *trans, struct jbd_io_phase *phase)
{
    int result = ext4_blocks_set_phase(trans->journal->jbd_fs->bdev, phase->first);
    *phase = (struct jbd_io_phase){0};
    return result;
}

static int jbd_encode_submit(struct jbd_trans *trans)
{
    struct jbd_journal *journal = trans->journal;
    struct jbd_fs *fs = journal->jbd_fs;
    struct ext4_blockdev *bdev = fs->bdev;
    if (!trans->frozen || !journal->commit_image || !journal->log_map) __builtin_trap();
    uint8_t *wire = journal->commit_image->data;
    unsigned used = 0;
    struct ext4_block_write control_io;
    struct jbd_io_phase phase = {0};
    jbd_ordered_phase(trans, &phase);
    struct jbd_buf *first = TAILQ_FIRST(&trans->buf_queue);
    uint32_t checksum = EXT4_CRC32_INIT;
    unsigned tail = jbd_has_csum(&fs->sb) ? sizeof(struct jbd_block_tail) : 0;
    int result;
    while (first) {
        uint32_t descriptor = jbd_wire_next(trans, &used);
        memset(wire, 0, journal->block_size);
        struct jbd_bhdr *header = (void *)wire;
        jbd_set32(header, magic, JBD_MAGIC_NUMBER);
        jbd_set32(header, blocktype, JBD_DESCRIPTOR_BLOCK);
        jbd_set32(header, sequence, trans->trans_id);
        char *tag = (char *)(header + 1);
        int remaining = journal->block_size - sizeof(*header) - tail;
        struct jbd_buf *next = first;
        bool uuid = true;
        do {
            struct jbd_buf *jb = next;
            next = TAILQ_NEXT(jb, buf_node);
            bool escape = ((const struct jbd_bhdr *)jb->block.data)->magic ==
                to_be32(JBD_MAGIC_NUMBER);
            int bytes = jbd_tag_bytes(fs) + (uuid ? UUID_SIZE : 0);
            struct tag_info info = {
                .block = jb->block.lb_id, .uuid_exist = uuid, .is_escape = escape,
                .last_tag = !next || remaining - bytes < jbd_tag_bytes(fs),
            };
            checksum = jbd_block_csum(fs, jb->block.data, checksum, trans->trans_id, escape);
            info.checksum = checksum;
            if (uuid) memcpy(info.uuid, fs->sb.uuid, UUID_SIZE);
            if (jbd_write_block_tag(fs, tag, remaining, &info)) __builtin_trap();
            jb->jbd_lba = jbd_wire_next(trans, &used);
            jb->escaped = escape;
            tag += info.tag_bytes;
            remaining -= info.tag_bytes;
            uuid = false;
            if (info.last_tag) break;
        } while (next);
        jbd_meta_csum_set(fs, header);
        jbd_phase_add(journal, &phase, &control_io, wire,
            journal->log_map[descriptor], 0, journal->block_size);
        for (struct jbd_buf *jb = first; jb != next; jb = TAILQ_NEXT(jb, buf_node)) {
            const void *payload = jb->block.data;
            if (jb->escaped) {
                /* End the descriptor's actual DMA borrow before overwriting
                 * its image. The accepted payload remains immutable. */
                result = jbd_submit_phase(trans, &phase);
                if (result) return result;
                memcpy(wire, payload, journal->block_size);
                ((struct jbd_bhdr *)wire)->magic = 0;
                payload = wire;
            }
            jbd_phase_add(journal, &phase, &jb->home_io, payload,
                journal->log_map[jb->jbd_lba], 0, journal->block_size);
            if (jb->escaped) {
                result = jbd_submit_phase(trans, &phase);
                if (result) return result;
            }
        }
        result = jbd_submit_phase(trans, &phase);
        if (result) return result;
        first = next;
    }
    /* Ordered-only groups also need to finish borrowing the work sources. */
    if (phase.first) {
        result = jbd_submit_phase(trans, &phase);
        if (result) return result;
    }
    struct jbd_revoke_rec *revoke = RB_MIN(jbd_revoke_tree, &trans->revoke_root);
    unsigned entry = JBD_HAS_INCOMPAT_FEATURE(&fs->sb, JBD_FEATURE_INCOMPAT_64BIT) ? 8 : 4;
    while (revoke) {
        uint32_t address = jbd_wire_next(trans, &used);
        memset(wire, 0, journal->block_size);
        struct jbd_revoke_header *header = (void *)wire;
        jbd_set32(&header->header, magic, JBD_MAGIC_NUMBER);
        jbd_set32(&header->header, blocktype, JBD_REVOKE_BLOCK);
        jbd_set32(&header->header, sequence, trans->trans_id);
        unsigned offset = sizeof(*header);
        do {
            if (entry == 8) { uint64_t value = to_be64(revoke->lba); memcpy(wire + offset, &value, 8); }
            else { uint32_t value = to_be32((uint32_t)revoke->lba); memcpy(wire + offset, &value, 4); }
            offset += entry;
            revoke = RB_NEXT(jbd_revoke_tree, &trans->revoke_root, revoke);
        } while (revoke && journal->block_size - tail - offset >= entry);
        jbd_set32(header, count, offset);
        jbd_meta_csum_set(fs, (struct jbd_bhdr *)wire);
        jbd_phase_add(journal, &phase, &control_io, wire,
            journal->log_map[address], 0, journal->block_size);
        result = jbd_submit_phase(trans, &phase);
        if (result) return result;
    }
    {
        COST_PHASE_SCOPE(precommit_body, 2);
        result = ext4_blockdev_flush(bdev);
    }
    if (result) return result;
    uint32_t commit = jbd_wire_next(trans, &used);
    if (used != (unsigned)trans->alloc_blocks) __builtin_trap();
    trans->data_csum = checksum;
    memset(wire, 0, journal->block_size);
    struct jbd_commit_header *header = (void *)wire;
    jbd_set32(&header->header, magic, JBD_MAGIC_NUMBER);
    jbd_set32(&header->header, blocktype, JBD_COMMIT_BLOCK);
    jbd_set32(&header->header, sequence, trans->trans_id);
    if (JBD_HAS_INCOMPAT_FEATURE(&fs->sb, JBD_FEATURE_COMPAT_CHECKSUM)) {
        header->chksum_type = JBD_CRC32_CHKSUM;
        header->chksum_size = JBD_CRC32_CHKSUM_SIZE;
        jbd_set32(header, chksum[0], checksum);
    }
    jbd_commit_csum_set(fs, header);
    result = ext4_blocks_set_direct(bdev, wire, journal->log_map[commit], 1);
    return result ? result : ext4_blockdev_flush(bdev);
}

/**@brief  Put references of block descriptors in a transaction.
 * @param  journal current journal session
 * @param  trans transaction*/
static void jbd_journal_cp_trans(struct jbd_journal *journal, struct jbd_trans *trans)
{
	struct jbd_buf *jbd_buf, *tmp;
	struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
	fs->bdev->cache_write_back++;
	TAILQ_FOREACH_SAFE(jbd_buf, &trans->buf_queue, buf_node,
			tmp) {
		struct ext4_block block = jbd_buf->block;
		ext4_block_set(fs->bdev, &block);
	}
	fs->bdev->cache_write_back--;
}

/**@brief  Update the start block of the journal when
 *         all the contents in a transaction reach the disk.*/
static void jbd_trans_end_write(struct ext4_bcache *bc __unused,
			  struct ext4_buf *buf,
			  int res,
			  void *arg)
{
	struct jbd_buf *jbd_buf = arg;
	struct jbd_trans *trans = jbd_buf->trans;
	struct jbd_block_rec *block_rec = jbd_buf->block_rec;
	struct jbd_journal *journal = trans->journal;
	if (res != EOK) {
		trans->error = res;
		__atomic_store_n(&journal->error,res,__ATOMIC_RELEASE);
		return;
	}

	TAILQ_REMOVE(&trans->buf_queue, jbd_buf, buf_node);
	TAILQ_REMOVE(&block_rec->dirty_buf_queue,
			jbd_buf,
			dirty_buf_node);

	jbd_trans_finish_callback(journal,
			trans,
			jbd_buf->block_rec);
	if (block_rec->trans == trans && buf) {
		/* Clear the end_write and end_write_arg fields. */
		buf->end_write = NULL;
		buf->end_write_arg = NULL;
	}

    if (jbd_buf->after) {
        if (journal->grouped) jbd_unpin_version(journal, buf);
        jbd_image_free(jbd_buf->after);
    }
	jbd_free(jbd_buf);

	trans->written_cnt++;

}

/**@brief  Commit a transaction to the journal immediately.
 * @param  journal current journal session
 * @param  trans transaction
 * @return standard error code*/
static int __jbd_journal_commit_trans(struct jbd_journal *journal,
				      struct jbd_trans *trans)
{
	int rc = EOK;
	struct jbd_revoke_rec *rec, *tmp;
	if (jbd_journal_error(journal)) {
		/* A failed commit remains journal-owned even if a caller repeats
		 * the request; never free the pinned recovery owner here. */
		rc = jbd_journal_error(journal);
		if (journal->failed_trans != trans)
			jbd_journal_free_trans(journal, trans, true);
		return rc;
	}
	rc = trans->error;
	if (rc != EOK) {
		jbd_journal_free_trans(journal, trans, true);
		return rc;
	}

	if (TAILQ_EMPTY(&trans->buf_queue) &&
	    RB_EMPTY(&trans->revoke_root)) {
		/* Since there are no entries in both buffer list
		 * and revoke entry list, we do not consider trans as
		 * complete transaction and just return EOK.*/
		rc = jbd_trans_write_ordered(trans);
		if (!rc && trans->ordered_count)
			rc = ext4_blockdev_flush(journal->jbd_fs->bdev);
		if (rc != EOK) goto Finish;
        {
        JBD_STATE_SCOPE(journal);
        ext4_operation_completed(&trans->operation, journal->accepted_sequence);
        jbd_journal_free_trans_state_held(journal, trans, false);
        }
		return rc;
	}

	rc = jbd_encode_submit(trans);
	if (rc != EOK) goto Finish;
    {
    JBD_STATE_SCOPE(journal);
    ext4_orphan_trans_finish_state_held(trans, false);
    ext4_operation_publish_super(&trans->operation);
    journal->committed_id = trans->trans_id;
    ext4_operation_completed(&trans->operation, journal->accepted_sequence);
    journal->alloc_trans_id++;
    }

	/* Complete the checkpoint of buffers which are revoked. */
	RB_FOREACH_SAFE(rec, jbd_revoke_tree, &trans->revoke_root,
			tmp) {
		struct jbd_block_rec *block_rec =
			jbd_trans_block_rec_lookup(journal, rec->lba);
		struct jbd_buf *jbd_buf = NULL;
		if (block_rec)
			jbd_buf = TAILQ_LAST(&block_rec->dirty_buf_queue,
					jbd_buf_dirty);
		if (jbd_buf) {
			struct ext4_buf *buf;
			struct ext4_block block = EXT4_BLOCK_ZERO();
            bool owns_home = jbd_buf->trans == trans;
            struct ext4_block owned_home = jbd_buf->block;
			/*
			 * We do this to reset the ext4_buf::end_write and
			 * ext4_buf::end_write_arg fields so that the checkpoint
			 * callback won't be triggered again.
			 */
			buf = ext4_bcache_find_get(journal->jbd_fs->bdev->bc,
					&block,
					jbd_buf->block_rec->lba, NULL);
            if (buf) ext4_bcache_clear_dirty(buf);
			jbd_trans_end_write(journal->jbd_fs->bdev->bc,
					buf,
					EOK,
					jbd_buf);
            /* Earlier checkpoint records already returned their home borrow.
             * This operation has not reached cp_trans yet: the callback
             * removes its record, so return that borrow here exactly once. */
            if (owns_home) ext4_block_set(journal->jbd_fs->bdev, &owned_home);
			if (buf)
				ext4_block_set(journal->jbd_fs->bdev, &block);
		}
	}

	TAILQ_INSERT_TAIL(&journal->cp_queue, trans, trans_node);
	jbd_trans_release_data(trans, false);
	jbd_journal_cp_trans(journal, trans);
	return jbd_journal_error(journal);
Finish:
	if (rc != EOK) {
		/* Never recycle possibly committed log blocks after uncertain I/O.
		 * The journal owns this pinned transaction until recovery/remount. */
        JBD_STATE_SCOPE(journal);
		journal->failed_trans = trans;
		/* Its published cache images also remain visible in this failed
		 * session. Keep the in-memory allocation/orphan state with them. */
		ext4_operation_publish_super(&trans->operation);
		ext4_orphan_trans_finish_state_held(trans, false);
		__atomic_store_n(&journal->error,rc,__ATOMIC_RELEASE);
	}
	return rc;
}

/**@brief  Allocate a new transaction
 * @param  journal current journal session
 * @return transaction allocated*/
int jbd_journal_new_trans(struct jbd_journal *journal, struct jbd_trans **out)
{
	*out = NULL;
	struct jbd_trans *trans = NULL;
	if (jbd_journal_error(journal))
		return jbd_journal_error(journal);
	trans = jbd_alloc(journal, sizeof(struct jbd_trans));
	if (!trans)
		return ENOMEM;

	/* We will assign a trans_id to this transaction,
	 * once it has been committed.*/
	trans->journal = journal;
	trans->data_csum = EXT4_CRC32_INIT;
	trans->error = EOK;
	TAILQ_INIT(&trans->buf_queue);
	TAILQ_INIT(&trans->data_queue);
	*out = trans;
	return EOK;
}

int jbd_trans_prepare_operation(struct jbd_trans *trans)
{
    struct jbd_journal *journal = trans->journal;
    if (jbd_journal_error(journal) || trans->error)
        return jbd_journal_error(journal) ? jbd_journal_error(journal) : trans->error;
    if (trans->reserved_logs || trans->reservation_group_sequence || trans->frozen) __builtin_trap();
    jbd_trans_prune(journal, trans);
    if (TAILQ_EMPTY(&trans->buf_queue) && TAILQ_EMPTY(&trans->data_queue) &&
        RB_EMPTY(&trans->revoke_root)) return EOK;
    bool serial_writer = ext4_fs_serial_writer(journal->jbd_fs->inode_ref.fs);
    struct jbd_buf *jb;
    int result;
    for (;;) {
        uint32_t start;
        {
            JBD_STATE_SCOPE(journal);
            TAILQ_FOREACH(jb, &trans->buf_queue, buf_node)
                if (!jb->block_rec) jb->block_rec = jbd_borrow_block_rec(journal, jb->block.lb_id);
            result = jbd_reserve_logs(trans, serial_writer);
            start = journal->start;
        }
        if (result != ENOSPC || journal->grouped || TAILQ_EMPTY(&journal->cp_queue)) break;
        /* Immediate writers previously reclaimed a full ring while encoding.
         * Reclaim before publication, without STATE or private log mappings. */
        result = jbd_journal_purge_cp_trans(journal, true, true);
        if (result || journal->start == start) { if (!result) result = ENOSPC; break; }
    }
    if (!result) TAILQ_FOREACH(jb, &trans->buf_queue, buf_node) {
        if (jb->block_rec) continue;
        jb->reserved_rec = jbd_alloc(journal, sizeof(*jb->reserved_rec));
        if (!jb->reserved_rec) { result = ENOMEM; break; }
    }
    if (result) trans->error = result;
    return result;
}

/**@brief  Commit a transaction to the journal immediately.
 * @param  journal current journal session
 * @param  trans transaction
 * @return standard error code*/
int jbd_journal_commit_trans(struct jbd_journal *journal,
			     struct jbd_trans *trans)
{
    if (jbd_journal_error(journal) || trans->error)
        return __jbd_journal_commit_trans(journal, trans);
    int result;
    {
        JBD_STATE_SCOPE(journal);
        result = jbd_publish_private(trans);
        if (!result && (!TAILQ_EMPTY(&trans->buf_queue) || !RB_EMPTY(&trans->revoke_root))) {
            trans->trans_id = journal->alloc_trans_id;
            jbd_claim_logs(trans);
        }
    }
    if (result) { jbd_journal_free_trans(journal, trans, true); return result; }
    return __jbd_journal_commit_trans(journal, trans);
}

/* The operation has passed publication: its ranges can now share the same
 * checkpoint owner. Private operations and older groups retain their own
 * nodes. Index readers borrow only under STATE and never keep node pointers. */
static void jbd_quarantine_accept(struct jbd_journal *journal,
    struct jbd_trans *operation, struct jbd_trans *group)
{
    struct jbd_quarantine_list pending;
    LIST_INIT(&pending);
    /* A first operation becomes the group itself; keep its unprocessed nodes
     * separate from the output list even when operation == group. */
    LIST_SWAP(&operation->quarantine, &pending, jbd_quarantine, node);
    struct jbd_quarantine *q;
    while ((q = LIST_FIRST(&pending))) {
        if (q->owner != operation) __builtin_trap();
        LIST_REMOVE(q, node);
        struct jbd_quarantine_tree *index = &journal->quarantine_index[q->inode];
        struct jbd_quarantine *before = RB_PREV(jbd_quarantine_tree, index, q);
        struct jbd_quarantine *after = RB_NEXT(jbd_quarantine_tree, index, q);
        RB_REMOVE(jbd_quarantine_tree, index, q);
        q->owner = group;
        while (before && before->owner == group &&
            before->first + before->count == q->first &&
            before->count <= UINT32_MAX - q->count) {
            struct jbd_quarantine *old = before;
            before = RB_PREV(jbd_quarantine_tree, index, old);
            q->first = old->first; q->count += old->count;
            RB_REMOVE(jbd_quarantine_tree, index, old);
            LIST_REMOVE(old, node); jbd_free(old);
        }
        while (after && after->owner == group &&
            q->first + q->count == after->first &&
            after->count <= UINT32_MAX - q->count) {
            struct jbd_quarantine *old = after;
            after = RB_NEXT(jbd_quarantine_tree, index, old);
            q->count += old->count;
            RB_REMOVE(jbd_quarantine_tree, index, old);
            LIST_REMOVE(old, node); jbd_free(old);
        }
        if (RB_INSERT(jbd_quarantine_tree, index, q)) __builtin_trap();
        LIST_INSERT_HEAD(&group->quarantine, q, node);
    }
}

/* Acceptance validates and publishes already prepared private versions. */
int jbd_journal_accept_state_held(struct jbd_journal *journal, struct jbd_trans *operation,
	uint64_t now_ns)
{
	if (!journal->grouped || jbd_journal_error(journal)) return jbd_journal_error(journal) ? jbd_journal_error(journal) : EINVAL;
	if (operation->error) return operation->error;
	struct jbd_data *data, *next_data;
	if (TAILQ_EMPTY(&operation->buf_queue) && TAILQ_EMPTY(&operation->data_queue) && RB_EMPTY(&operation->revoke_root)) {
		/* An empty operation owns no accepted version. Its candidates must
		 * not inherit an unrelated inode's current running group. */
		ext4_operation_completed(&operation->operation, 0);
		jbd_journal_free_trans_state_held(journal, operation, false);
		return EOK;
	}
	struct jbd_trans *running = journal->running;
	if (!running && journal->accepted_sequence == UINT64_MAX) return EOVERFLOW;
	if (operation->reservation_group_sequence) {
		/* The sequence is a reservation token, not a reference. A public
		 * preparer can release its qualification before accepting: reject a
		 * changed target before touching any accepted metadata or its owner. */
		if (!ext4_fs_serial_writer(journal->jbd_fs->inode_ref.fs) || !running ||
		    running->sequence != operation->reservation_group_sequence) return EAGAIN;
		unsigned required, required_images;
		if (jbd_group_requirements(journal, running, operation, &required, &required_images) ||
		    running->reserved_logs + operation->reserved_logs < required)
			return EAGAIN;
	} else {
		unsigned required_images=jbd_log_images_needed(journal,operation->data_cnt,operation->revoke_count,true);
		if (operation->reserved_logs != operation->data_cnt+required_images)
			__builtin_trap();
	}
	/* Private reservations cover the actual serialized payload/control count.
	 * No accepted group can exhaust the circular log at sealing. */
	unsigned credits = operation->reserved_logs + (running ? running->reserved_logs : 0);
	uint32_t maxlen = jbd_get32(&journal->jbd_fs->sb, maxlen);
	if (credits >= maxlen - journal->first) __builtin_trap();
    int publication = jbd_publish_private(operation);
    if (publication) return publication;
    ext4_operation_publish_super(&operation->operation);
	ext4_orphan_trans_finish_state_held(operation, false);
	ext4_operation_completed(&operation->operation,
		running ? running->sequence : journal->accepted_sequence + 1);
	jbd_quarantine_accept(journal, operation, running ? running : operation);
	if (!running) {
		running = operation;
		running->sequence = ++journal->accepted_sequence;
		running->first_dirty_ns = now_ns;
		journal->running = running;
	} else {
		COST_ADD(JOURNAL_MERGED, 1);
		struct jbd_buf *jb, *next;
		TAILQ_FOREACH_SAFE(jb, &operation->buf_queue, buf_node, next) {
			struct jbd_buf *old = TAILQ_PREV(jb, jbd_buf_dirty, dirty_buf_node);
			if (old && old->trans != running) old = NULL;
			TAILQ_REMOVE(&operation->buf_queue, jb, buf_node);
			jbd_trans_change_ownership(jb->block_rec, running);
			if (old) {
				TAILQ_REMOVE(&jb->block_rec->dirty_buf_queue, jb, dirty_buf_node);
				jb->block.buf->end_write_arg = old;
				if (jb->after) jbd_unpin_version(journal, jb->block.buf);
				journal->jbd_fs->bdev->cache_write_back++;
				ext4_block_set(journal->jbd_fs->bdev, &jb->block);
				journal->jbd_fs->bdev->cache_write_back--;
            if (jb->after) {
                ext4_block_rebind(&old->block, &jb->after->payload);
                jbd_image_free(old->after); old->after = jb->after; jb->after = NULL;
            }
			jbd_free(jb);
			} else {
				jb->trans = running;
				TAILQ_INSERT_TAIL(&running->buf_queue, jb, buf_node);
				running->data_cnt++;
			}
		}
		TAILQ_FOREACH_SAFE(data, &operation->data_queue, node, next_data) {
			TAILQ_REMOVE(&operation->data_queue, data, node);
			RB_REMOVE(jbd_data_tree, &operation->data_root, data);
			struct jbd_data *old = jbd_trans_find_data(running, data->block.lb_id);
			if (old) {
				TAILQ_REMOVE(&running->data_queue, old, node);
				RB_REMOVE(jbd_data_tree, &running->data_root, old);
				jbd_data_free(journal, old);
			} else running->ordered_count++;
			RB_INSERT(jbd_data_tree, &running->data_root, data);
			TAILQ_INSERT_TAIL(&running->data_queue, data, node);
		}
		struct jbd_revoke_rec *rec, *next_rec;
		RB_FOREACH_SAFE(rec, jbd_revoke_tree, &operation->revoke_root, next_rec) {
			RB_REMOVE(jbd_revoke_tree, &operation->revoke_root, rec);
			if (RB_FIND(jbd_revoke_tree, &running->revoke_root, rec)) jbd_free(rec);
			else { RB_INSERT(jbd_revoke_tree, &running->revoke_root, rec); running->revoke_count++; }
		}
        running->reserved_logs += operation->reserved_logs;
        operation->reserved_logs = 0;
        jbd_free(operation);
	}
	unsigned images = jbd_log_images_needed(journal, running->data_cnt, running->revoke_count, true);
	jbd_trim_reserved_logs(running, running->data_cnt + images);
	COST_ADD(JOURNAL_ACCEPTED, 1);
	return EOK;
}

int jbd_journal_accept(struct jbd_journal *journal, struct jbd_trans *operation,
    uint64_t now_ns)
{
    JBD_STATE_SCOPE(journal);
    return jbd_journal_accept_state_held(journal, operation, now_ns);
}

int jbd_journal_freeze(struct jbd_journal *journal)
{
    JBD_STATE_SCOPE(journal);
	if (jbd_journal_error(journal)) return jbd_journal_error(journal);
	if (journal->sealed_count >= 2) return EAGAIN;
	struct jbd_trans *trans = journal->running;
	if (!trans) return EOK;
	struct jbd_buf *jb;
	TAILQ_FOREACH(jb, &trans->buf_queue, buf_node) {
        if (!jb->after || jb->private_access) __builtin_trap();
        ext4_block_rebind(&jb->block, &jb->after->payload);
	}

    jbd_claim_logs(trans);
    COST_ADD(JOURNAL_GROUPS, 1);
    COST_ADD(JOURNAL_MEMORY_PEAK, jbd_memory_peak(journal));
    trans->trans_id = journal->alloc_trans_id;
	journal->running = NULL;
	TAILQ_INSERT_TAIL(&journal->sealed_queue, trans, trans_node);
	journal->sealed_count++;
	journal->sealed_sequence = trans->sequence;
	journal->alloc_trans_id++;
	return EOK;
}

/* Selection lends one immutable group to this consumer. Checksums, wire
 * records and device I/O use its reserved storage without holding state. */
int jbd_journal_submit(struct jbd_journal *journal)
{
    COST_SCOPE(submit_cost, JOURNAL_SUBMIT_TICKS);
    struct jbd_trans *trans = journal->committing;
    if (!trans) return EOK;
    if (jbd_journal_error(journal)) return jbd_journal_error(journal);
    return jbd_encode_submit(trans);
}

bool jbd_journal_checkpoint_ready(const struct jbd_journal *journal)
{
    JBD_STATE_READ_SCOPE(journal);
	if (jbd_journal_error(journal) || journal->checkpointing || TAILQ_EMPTY(&journal->cp_queue))
		return false;
	if (journal->checkpoint_target > journal->checkpoint_sequence || journal->checkpoint_count >= 4)
		return true;
	struct jbd_memory_stats memory;
	jbd_memory_snapshot(journal->memory, &memory);
	return memory.used - memory.cached > memory.limit / 2;
}

enum jbd_journal_stage jbd_journal_select_stage(struct jbd_journal *journal,
	enum jbd_journal_stage stage)
{
    JBD_STATE_SCOPE(journal);
	if (jbd_journal_error(journal)) return JBD_JOURNAL_IDLE;
	struct jbd_trans *trans = TAILQ_FIRST(&journal->cp_queue);
	/* Reclaim pressure is shared with progress reporting. An empty sealed
	 * queue says nothing about a commit currently doing device I/O. */
	if (stage != JBD_JOURNAL_COMMIT && jbd_journal_checkpoint_ready(journal)) {
		journal->checkpointing = journal->checkpoint_last = trans;
		unsigned count = 1;
		while (count < 8 && TAILQ_NEXT(journal->checkpoint_last, trans_node)) {
			journal->checkpoint_last = TAILQ_NEXT(journal->checkpoint_last, trans_node);
			count++;
		}
		/* Selection holds the modification lock. Publish one immutable home
		 * version per LBA from this durable prefix, never from a later group.
		 * A revoke discards the older incarnation; later metadata may replace it. */
		TAILQ_INIT(&journal->checkpoint_bufs);
		for (;;) {
			struct jbd_buf *jb;
			TAILQ_FOREACH(jb, &trans->buf_queue, buf_node) {
				struct jbd_block_rec *rec = jb->block_rec;
				if (rec->checkpoint)
					TAILQ_REMOVE(&journal->checkpoint_bufs, rec->checkpoint, checkpoint_node);
				rec->checkpoint = jb;
				TAILQ_INSERT_TAIL(&journal->checkpoint_bufs, jb, checkpoint_node);
			}
			struct jbd_revoke_rec *revoke;
			RB_FOREACH(revoke, jbd_revoke_tree, &trans->revoke_root) {
				struct jbd_block_rec *rec = jbd_trans_block_rec_lookup(journal, revoke->lba);
				if (rec && rec->checkpoint) {
					TAILQ_REMOVE(&journal->checkpoint_bufs, rec->checkpoint, checkpoint_node);
					rec->checkpoint = NULL;
				}
			}
			if (trans == journal->checkpoint_last) break;
			trans = TAILQ_NEXT(trans, trans_node);
		}
        struct jbd_io_phase phase = {0};
        struct jbd_buf *jb;
        TAILQ_FOREACH(jb, &journal->checkpoint_bufs, checkpoint_node)
            jbd_phase_add(journal, &phase, &jb->home_io, jb->after->data,
                jb->block.lb_id, 0, journal->block_size);
        journal->checkpoint_io = phase.first;
		return JBD_JOURNAL_CHECKPOINT;
	}
	if (journal->committing || stage == JBD_JOURNAL_CHECKPOINT) return JBD_JOURNAL_IDLE;
	trans = TAILQ_FIRST(&journal->sealed_queue);
	if (!trans) return JBD_JOURNAL_IDLE;
	TAILQ_REMOVE(&journal->sealed_queue, trans, trans_node);
	journal->sealed_count--;
	journal->committing = trans;
	return JBD_JOURNAL_COMMIT;
}

bool jbd_journal_select(struct jbd_journal *journal)
{
	/* Sequential low-level callers have no completion token. Keep them from
	 * borrowing an owner already held by either runtime consumer. */
	if (journal->committing || journal->checkpointing) return false;
	if (TAILQ_EMPTY(&journal->sealed_queue) && !journal->running &&
	    journal->checkpoint_target < journal->durable_sequence)
		journal->checkpoint_target = journal->durable_sequence;
	return jbd_journal_select_stage(journal, JBD_JOURNAL_IDLE) != JBD_JOURNAL_IDLE;
}

int jbd_journal_checkpoint(struct jbd_journal *journal)
{
	COST_SCOPE(checkpoint_cost, JOURNAL_CHECKPOINT_TICKS);
	struct jbd_trans *trans = journal->checkpointing;
	if (!trans) return EOK;
	struct ext4_blockdev *bdev = journal->jbd_fs->bdev;
	int r = EOK;
	{
	COST_PHASE_SCOPE(home_phase, 3);
	r = ext4_blocks_set_phase(bdev, journal->checkpoint_io);
	if (r != EOK) return r;
	r = ext4_blockdev_flush(bdev);
	}
	trans = journal->checkpoint_last;
	COST_PHASE_SCOPE(checkpoint_phase, 4);
    if (r == EOK) {
        uint32_t end = trans->start_iblock + trans->alloc_blocks;
        wrap(&journal->jbd_fs->sb, end);
        struct jbd_sb *sb = journal->checkpoint_image->data;
        jbd_set32(sb, start, end);
        jbd_set32(sb, sequence, trans->trans_id + 1);
        memset((uint8_t *)sb + sizeof(*sb), 0, journal->block_size - sizeof(*sb));
        jbd_sb_csum_set(sb);
        r = ext4_blocks_set_direct(bdev, sb, journal->log_map[0], 1);
    }
	return r != EOK ? r : ext4_blockdev_flush(bdev);
}

static void jbd_group_release(struct jbd_journal *journal, struct jbd_trans *trans)
{
	jbd_trans_release_data(trans, false);
	struct jbd_buf *jb;
	while ((jb = TAILQ_FIRST(&trans->buf_queue))) {
		struct ext4_buf *buf = jb->block.buf;
		struct ext4_block block = jb->block;
		if (jb->block_rec->checkpoint == jb) {
			TAILQ_REMOVE(&journal->checkpoint_bufs, jb, checkpoint_node);
			jb->block_rec->checkpoint = NULL;
		}
		if (!buf->journal_pending) __builtin_trap();
		jbd_unpin_version(journal, buf);
		if (!buf->journal_pending && !memcmp(buf->data, jb->after->data, journal->block_size)) ext4_bcache_clear_dirty(buf);
		jbd_image_free(jb->after); jb->after = NULL;
		jbd_trans_end_write(journal->jbd_fs->bdev->bc, buf, EOK, jb);
		journal->jbd_fs->bdev->cache_write_back++;
		ext4_block_set(journal->jbd_fs->bdev, &block);
		journal->jbd_fs->bdev->cache_write_back--;
	}
	jbd_journal_free_trans_state_held(journal, trans, false);
}

int jbd_journal_retire_stage(struct jbd_journal *journal,
	enum jbd_journal_stage stage, int error)
{
    JBD_STATE_SCOPE(journal);
	struct jbd_trans *trans = stage == JBD_JOURNAL_COMMIT ? journal->committing : journal->checkpointing;
	if (!trans) return error;
	if (error) {
		if (!jbd_journal_error(journal)) {
			journal->failed_trans = trans;
			__atomic_store_n(&journal->error,error,__ATOMIC_RELEASE);
		}
		/* DMA has stopped, but live readers still own the accepted file
		 * contents. The failed mount keeps these versions and later groups. */
		return jbd_journal_error(journal);
	}
	/* A failure in the other consumer leaves this completed owner's recovery
	 * and live-read references intact as well. No lane can recycle the log. */
	if (jbd_journal_error(journal)) return jbd_journal_error(journal);
	if (stage == JBD_JOURNAL_COMMIT) {
		/* commit 屏障成功即形成恢复依据；home owner 继续归 checkpoint 队列。 */
		journal->committed_id = trans->trans_id;
		journal->durable_sequence = trans->sequence;
		ext4_fs_retire_inode_goals(journal->jbd_fs->inode_ref.fs, journal->durable_sequence);
		journal->committing = NULL;
		jbd_release_prepared(trans);
		/* ordered data 已到 home 持久边界，checkpoint 只再写 metadata。 */
		jbd_trans_release_data(trans, false);
		TAILQ_INSERT_TAIL(&journal->cp_queue, trans, trans_node);
		journal->checkpoint_count++;
		return EOK;
	}
	struct jbd_trans *last = journal->checkpoint_last;
	journal->checkpoint_sequence = last->sequence;
	journal->start = last->start_iblock + last->alloc_blocks;
	wrap(&journal->jbd_fs->sb, journal->start);
	journal->trans_id = last->trans_id + 1;
	jbd_journal_write_sb(journal);
	journal->jbd_fs->dirty = false;
	for (;;) {
		trans = TAILQ_FIRST(&journal->cp_queue);
		bool done = trans == last;
		TAILQ_REMOVE(&journal->cp_queue, trans, trans_node);
		journal->checkpoint_count--;
		jbd_group_release(journal, trans);
		if (done) break;
	}
	journal->checkpoint_io = NULL;
	journal->checkpointing = journal->checkpoint_last = NULL;
	return EOK;
}

int jbd_journal_retire(struct jbd_journal *journal, int error)
{
	if (journal->committing && journal->checkpointing) __builtin_trap();
	return jbd_journal_retire_stage(journal, journal->committing ?
		JBD_JOURNAL_COMMIT : JBD_JOURNAL_CHECKPOINT, error);
}

uint64_t jbd_journal_reclaim_target(struct jbd_journal *journal)
{
    JBD_STATE_READ_SCOPE(journal);
	struct jbd_trans *trans = TAILQ_FIRST(&journal->cp_queue);
	if (!trans) trans = journal->committing;
	if (!trans) trans = TAILQ_FIRST(&journal->sealed_queue);
	if (!trans) trans = journal->running;
	return trans ? trans->sequence : 0;
}

bool jbd_journal_operation_room(struct jbd_journal *journal, const struct jbd_trans *allocated,
    size_t payloads, size_t data_images,
	const struct file_data_source *const *sources, unsigned count)
{
	if (allocated && allocated->journal != journal) __builtin_trap();
	/* Private metadata needs undo/version/control space; borrowed file data
	 * needs records and one charge per distinct owner, never a block image.
	 * Allocators enforce the same hard budget for additional mapping changes. */
	size_t images = 5;
	if (data_images > SIZE_MAX - images) return false;
	images += data_images;
	size_t per_image = jbd_image_charge(journal) + 256;
	size_t fixed = (allocated ? 0 : jbd_allocation_charge(sizeof(struct jbd_trans))) + 4 * 256;
	if (images > (SIZE_MAX - fixed) / per_image) return false;
	size_t minimum = images * per_image + fixed;
	size_t record = jbd_allocation_charge(sizeof(struct jbd_data));
	if (payloads > (SIZE_MAX - minimum) / record) return false;
	minimum += payloads * record;
	for (unsigned i = 0; i < count; i++) {
		struct jbd_source key = { .owner.context = sources[i]->context };
		if (RB_FIND(jbd_source_tree, &journal->sources, &key)) continue;
		unsigned previous = 0;
		while (previous < i && sources[previous]->context != sources[i]->context) previous++;
		if (previous != i) continue;
		size_t control = jbd_allocation_charge(sizeof(struct jbd_source));
		if (sources[i]->bytes > SIZE_MAX - control ||
		    sources[i]->bytes + control > SIZE_MAX - minimum) return false;
		minimum += sources[i]->bytes + control;
	}
	unsigned logs = 1 + jbd_log_images_needed(journal, 1, 0, true);
	return jbd_budget_room(journal, minimum) &&
		jbd_log_free_blocks(journal) >= journal->log_reserved &&
		logs <= jbd_log_free_blocks(journal) - journal->log_reserved;
}

int jbd_trans_quarantine(struct ext4_fs *fs, ext4_fsblk_t first,
    uint32_t count, bool inode)
{
    struct jbd_journal *journal = fs->jbd_journal;
    if (!journal || !count) return EOK;
    JBD_STATE_SCOPE(journal);
    /* 即时提交同样持有私有 undo 和尚未结束的 checkpoint；这些块不能
     * 在原操作 abort 或旧版本回写之前进入新所有者的 unwritten 映射。 */
    struct jbd_trans *trans = ext4_current_trans(fs);
    if (!trans || count > UINT64_MAX - first) return EINVAL;
    struct jbd_quarantine_tree *index = &journal->quarantine_index[inode];
    struct jbd_quarantine key = {.first = first};
    struct jbd_quarantine *after = RB_NFIND(jbd_quarantine_tree, index, &key);
    struct jbd_quarantine *before = after ? RB_PREV(jbd_quarantine_tree, index, after) :
        RB_MAX(jbd_quarantine_tree, index);
    /* Reuse is excluded while an interval is indexed. A second free that
     * intersects it is therefore an actual double-free, not another owner. */
    if ((after && after->first - first < count) ||
        (before && first - before->first < before->count)) return EUCLEAN;
    struct jbd_quarantine *q = jbd_alloc(journal, sizeof(*q));
    if (!q) return ENOMEM;
    q->first = first; q->count = count; q->inode = inode; q->owner = trans;
    if (RB_INSERT(jbd_quarantine_tree, index, q)) __builtin_trap();
    LIST_INSERT_HEAD(&trans->quarantine, q, node);
    uint64_t *total = inode ? &journal->quarantined_inodes : &journal->quarantined_blocks;
    if (*total > UINT64_MAX - count) __builtin_trap();
    *total += count;
    return EOK;
}

int jbd_journal_find_reusable(struct ext4_fs *fs, const void *bitmap,
    ext4_fsblk_t base, uint32_t start, uint32_t end, uint32_t maximum,
    bool inode, uint32_t *found, uint32_t *length)
{
    if (!maximum || start > end || base > UINT64_MAX-end) __builtin_trap();
    *length=0;
    uint32_t bit;
    int result=ext4_bmap_bit_find_clr(bitmap,start,end,&bit);
    if (result) return result;
    struct jbd_journal *journal=fs->jbd_journal;
    struct jbd_quarantine_tree *index=NULL;
    struct jbd_quarantine *range=NULL;
    if (journal) {
        ext4_fs_state_read_lock(fs);
        index=&journal->quarantine_index[inode];
        struct jbd_quarantine key={.first=base+bit};
        range=RB_NFIND(jbd_quarantine_tree,index,&key);
        struct jbd_quarantine *previous=range ?
            RB_PREV(jbd_quarantine_tree,index,range) : RB_MAX(jbd_quarantine_tree,index);
        if (previous && base+bit-previous->first<previous->count) range=previous;
    }
    /* Keep the bitmap's real pin and group qualification. One state scope
     * covers the ordered walk; checkpoint may retire entries only afterwards.
     * Restarting the tree and its sleeping lock at every released inode turns
     * one allocation into hundreds of cross-CPU handoffs. */
    for (;;) {
        ext4_fsblk_t address=base+bit;
        while (range && range->first+range->count<=address)
            range=RB_NEXT(jbd_quarantine_tree,index,range);
        if (range && range->first<=address) {
            ext4_fsblk_t boundary=range->first+range->count;
            while ((range=RB_NEXT(jbd_quarantine_tree,index,range)) &&
                range->first==boundary) boundary+=range->count;
            uint64_t next=boundary-base;
            if (next>=end) { result=ENOSPC; break; }
            result=ext4_bmap_bit_find_clr(bitmap,(uint32_t)next,end,&bit);
            if (result) break;
            continue;
        }
        uint64_t available=range ? range->first-address : end-bit;
        uint32_t count=1;
        while (count<maximum && count<end-bit && count<available &&
            ext4_bmap_is_bit_clr(bitmap,bit+count)) count++;
        *found=bit; *length=count;
        break;
    }
    if (journal) ext4_fs_state_unlock(fs);
    return result;
}

int jbd_journal_group_init(struct jbd_journal *journal, size_t limit)
{
    if (!journal->log_map || !journal->commit_image || !journal->checkpoint_image) __builtin_trap();
    if (limit && jbd_memory_used(journal) > limit) return ENOMEM;
    jbd_memory_set_limit(journal, limit);
    journal->grouped = true;
    jbd_memory_set_grouped(journal, true);
    return EOK;
}

void jbd_journal_group_fini(struct jbd_journal *journal)
{
	if (journal->running || journal->committing || journal->checkpointing ||
	    !TAILQ_EMPTY(&journal->sealed_queue) || !TAILQ_EMPTY(&journal->cp_queue) ||
	    !RB_EMPTY(&journal->sources) || journal->log_reserved || jbd_journal_error(journal) ||
        !RB_EMPTY(&journal->quarantine_index[0]) || !RB_EMPTY(&journal->quarantine_index[1]) ||
        journal->quarantined_blocks || journal->quarantined_inodes) __builtin_trap();
	journal->grouped = false;
    jbd_memory_set_grouped(journal, false);
	jbd_trim_cache(journal, 0);
}

/**
 * @}
 */
