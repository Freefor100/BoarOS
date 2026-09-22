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
 * @file  ext4.h
 * @brief Ext4 high level operations (file, directory, mountpoints...)
 */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_oflags.h>
#include <ext4_debug.h>

#include <ext4.h>
#include <ext4_trans.h>
#include <ext4_blockdev.h>
#include <ext4_fs.h>
#include <ext4_dir.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <ext4_block_group.h>
#include <ext4_dir_idx.h>
#include <ext4_xattr.h>
#include <ext4_journal.h>
#include <ext4_orphan.h>
#include <ext4_truncate.h>


#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/**@brief   Mount point OS dependent lock*/
#define EXT4_MP_LOCK(_m)                                                       \
	do {                                                                   \
		if ((_m)->os_locks)                                            \
			(_m)->os_locks->lock();                                \
	} while (0)

/**@brief   Mount point OS dependent unlock*/
#define EXT4_MP_UNLOCK(_m)                                                     \
	do {                                                                   \
		if ((_m)->os_locks)                                            \
			(_m)->os_locks->unlock();                              \
	} while (0)

/**@brief   Mount point descriptor.*/
struct ext4_mountpoint {

	/**@brief   Mount done flag.*/
	bool mounted;

	/**@brief   Mount point name (@ref ext4_mount)*/
	char name[CONFIG_EXT4_MAX_MP_NAME + 1];

	/**@brief   OS dependent lock/unlock functions.*/
	const struct ext4_lock *os_locks;

	/* Optional adapter-owned realtime source; cleared on mount reuse. */
	ext4_clock_read clock;

	/**@brief   Ext4 filesystem internals.*/
	struct ext4_fs fs;

	/**@brief   JBD fs.*/
	struct jbd_fs jbd_fs;

	/**@brief   Journal.*/
	struct jbd_journal jbd_journal;

	/**@brief   Block cache.*/
	struct ext4_bcache bc;

	/* Transaction-wide state belongs to the mount, not the kernel stack. */
	struct ext4_sblock transaction_sb;
	struct ext4_writeback_scope transaction_scope;
	uint32_t transaction_depth;
	int transaction_error;
	bool transaction_aborted;
	bool journal_stopped;
};

static int ext4_result(int result, int cleanup)
{
	return result == EOK ? cleanup : result;
}

static uint64_t ext4_legacy_max_blocks(struct ext4_fs *fs)
{
	uint64_t block_size = ext4_sb_get_block_size(&fs->sb);
	uint64_t pointers_per_block = block_size / sizeof(uint32_t);
	uint64_t pointers_squared = pointers_per_block * pointers_per_block;
	uint64_t inode_blocks_limit;
	uint64_t metadata_blocks;
	uint64_t remaining;
	uint64_t result = fs->inode_block_limits[3];

	/* i_blocks is either a 32-bit count of 512-byte sectors, or, with the
	 * huge_file feature, a 48-bit count that can switch to filesystem-block
	 * units. Keep enough room for every indirect block of a dense file. */
	if (ext4_sb_feature_ro_com(&fs->sb, EXT4_FRO_COM_HUGE_FILE))
		inode_blocks_limit = (UINT64_C(1) << 48) - 1;
	else
		inode_blocks_limit = UINT32_MAX /
			(block_size / EXT4_INODE_BLOCK_SIZE);

	metadata_blocks = 3 + pointers_per_block + pointers_squared;
	if (result + metadata_blocks > inode_blocks_limit) {
		result = inode_blocks_limit;
		remaining = inode_blocks_limit - EXT4_INODE_DIRECT_BLOCK_COUNT;

		metadata_blocks = 1;
		remaining -= pointers_per_block;
		if (remaining < pointers_squared) {
			metadata_blocks += 1 +
				EXT4_DIV_ROUND_UP(remaining, pointers_per_block);
		} else {
			metadata_blocks += 1 + pointers_per_block;
			remaining -= pointers_squared;
			metadata_blocks += 1 +
				EXT4_DIV_ROUND_UP(remaining, pointers_per_block) +
				EXT4_DIV_ROUND_UP(remaining, pointers_squared);
		}
		result -= metadata_blocks;
	}

	/* EXT_MAX_BLOCKS is both the extent walker's sentinel and the largest
	 * block count that fits the legacy truncate path's ext4_lblk_t counts.
	 * Thus 0xfffffffe is the last usable logical block. */
	if (result > (uint64_t)EXT_MAX_BLOCKS)
		result = (uint64_t)EXT_MAX_BLOCKS;

	return result;
}

static uint64_t ext4_inode_max_size(struct ext4_fs *fs,
				    struct ext4_inode *inode)
{
	uint32_t block_size = ext4_sb_get_block_size(&fs->sb);

#if CONFIG_EXTENT_ENABLE && CONFIG_EXTENTS_ENABLE
	if (ext4_sb_feature_incom(&fs->sb, EXT4_FINCOM_EXTENTS) &&
	    ext4_inode_has_flag(inode, EXT4_INODE_FLAG_EXTENTS)) {
		/* EXT_MAX_BLOCKS is the extent walker's sentinel, not a usable
		 * logical block number. */
		return (uint64_t)EXT_MAX_BLOCKS * block_size;
	}
#endif

	return ext4_legacy_max_blocks(fs) * block_size;
}

/**@brief   Block devices descriptor.*/
struct ext4_block_devices {

	/**@brief   Block device name.*/
	char name[CONFIG_EXT4_MAX_BLOCKDEV_NAME + 1];

	/**@brief   Block device handle.*/
	struct ext4_blockdev *bd;
};

/**@brief   Block devices.*/
static struct ext4_block_devices s_bdevices[CONFIG_EXT4_BLOCKDEVS_COUNT];

/**@brief   Mountpoints.*/
static struct ext4_mountpoint s_mp[CONFIG_EXT4_MOUNTPOINTS_COUNT];

int ext4_device_register(struct ext4_blockdev *bd,
			 const char *dev_name)
{
	ext4_assert(bd && dev_name);

	if (strlen(dev_name) > CONFIG_EXT4_MAX_BLOCKDEV_NAME)
		return EINVAL;

	for (size_t i = 0; i < CONFIG_EXT4_BLOCKDEVS_COUNT; ++i) {
		if (!strcmp(s_bdevices[i].name, dev_name))
			return EEXIST;
	}

	for (size_t i = 0; i < CONFIG_EXT4_BLOCKDEVS_COUNT; ++i) {
		if (!s_bdevices[i].bd) {
			strcpy(s_bdevices[i].name, dev_name);
			s_bdevices[i].bd = bd;
			return EOK;
		}
	}

	return ENOSPC;
}

int ext4_device_unregister(const char *dev_name)
{
	ext4_assert(dev_name);

	for (size_t i = 0; i < CONFIG_EXT4_BLOCKDEVS_COUNT; ++i) {
		if (strcmp(s_bdevices[i].name, dev_name))
			continue;

		memset(&s_bdevices[i], 0, sizeof(s_bdevices[i]));
		return EOK;
	}

	return ENOENT;
}

int ext4_device_unregister_all(void)
{
	memset(s_bdevices, 0, sizeof(s_bdevices));

	return EOK;
}

/****************************************************************************/

static bool ext4_is_dots(const uint8_t *name, size_t name_size)
{
	if ((name_size == 1) && (name[0] == '.'))
		return true;

	if ((name_size == 2) && (name[0] == '.') && (name[1] == '.'))
		return true;

	return false;
}

static int ext4_has_children(bool *has_children, struct ext4_inode_ref *enode)
{
	struct ext4_sblock *sb = &enode->fs->sb;

	/* Check if node is directory */
	if (!ext4_inode_is_type(sb, enode->inode, EXT4_INODE_MODE_DIRECTORY)) {
		*has_children = false;
		return EOK;
	}

	struct ext4_dir_iter it;
	int rc = ext4_dir_iterator_init(&it, enode, 0);
	if (rc != EOK)
		return rc;

	/* Find a non-empty directory entry */
	bool found = false;
	while (it.curr != NULL) {
		if (ext4_dir_en_get_inode(it.curr) != 0) {
			uint16_t nsize;
			nsize = ext4_dir_en_get_name_len(sb, it.curr);
			if (!ext4_is_dots(it.curr->name, nsize)) {
				found = true;
				break;
			}
		}

		rc = ext4_dir_iterator_next(&it);
		if (rc != EOK) {
			ext4_dir_iterator_fini(&it);
			return rc;
		}
	}

	rc = ext4_dir_iterator_fini(&it);
	if (rc != EOK)
		return rc;

	*has_children = found;

	return EOK;
}

/* ext4 stores signed 32-bit seconds plus a two-bit epoch in each extra
 * field. Old 128-byte inodes have only seconds; never overwrite their tail. */
static bool ext4_time_has_extra(struct ext4_inode_ref *ref, size_t offset)
{
    size_t present = EXT4_GOOD_OLD_INODE_SIZE +
        ext4_inode_get_extra_isize(&ref->fs->sb, ref->inode);
    return offset + sizeof(uint32_t) <= present &&
        offset + sizeof(uint32_t) <= ext4_get16(&ref->fs->sb, inode_size);
}

static struct ext4_timestamp ext4_time_load(struct ext4_inode_ref *ref,
                                           size_t base_offset,
                                           size_t extra_offset)
{
    uint32_t base, extra = 0;
    struct ext4_timestamp time;
    memcpy(&base, (unsigned char *)ref->inode + base_offset, sizeof(base));
    time.seconds = (int32_t)to_le32(base);
    if (ext4_time_has_extra(ref, extra_offset)) {
        memcpy(&extra, (unsigned char *)ref->inode + extra_offset, sizeof(extra));
        extra = to_le32(extra);
        time.seconds += (int64_t)(extra & 3U) << 32;
    }
    time.nanoseconds = extra >> 2;
    return time;
}

static int ext4_time_compare(struct ext4_timestamp a, struct ext4_timestamp b)
{
    if (a.seconds != b.seconds) return a.seconds > b.seconds ? 1 : -1;
    if (a.nanoseconds != b.nanoseconds)
        return a.nanoseconds > b.nanoseconds ? 1 : -1;
    return 0;
}

static void ext4_time_store(struct ext4_inode_ref *ref, size_t base_offset,
                            size_t extra_offset, struct ext4_timestamp time)
{
    bool extended = ext4_time_has_extra(ref, extra_offset);
    int64_t maximum = extended ? INT64_C(15032385535) : INT32_MAX;
    uint32_t base, extra;
    if (time.seconds < INT32_MIN) {
        time.seconds = INT32_MIN;
        time.nanoseconds = 0;
    } else if (time.seconds > maximum) {
        time.seconds = maximum;
        time.nanoseconds = 999999999U;
    }
    if (!extended) time.nanoseconds = 0;
    if (!ext4_time_compare(time, ext4_time_load(ref, base_offset, extra_offset)))
        return;
    base = to_le32((uint32_t)time.seconds);
    memcpy((unsigned char *)ref->inode + base_offset, &base, sizeof(base));
    if (extended) {
        uint32_t epoch = (uint32_t)((time.seconds - (int32_t)time.seconds) >> 32);
        extra = to_le32((time.nanoseconds << 2) | epoch);
        memcpy((unsigned char *)ref->inode + extra_offset, &extra, sizeof(extra));
    }
    ref->dirty = true;
}

static void ext4_touch_inode(struct ext4_mountpoint *mp,
                             struct ext4_inode_ref *ref, unsigned int fields)
{
    struct ext4_timestamp now;
    if (mp->fs.read_only || !mp->clock || !mp->clock(&now)) return;
    if (fields & EXT4_TIME_RELATIME) {
        struct ext4_timestamp atime = ext4_time_load(ref,
            offsetof(struct ext4_inode, access_time), offsetof(struct ext4_inode, atime_extra));
        struct ext4_timestamp mtime = ext4_time_load(ref,
            offsetof(struct ext4_inode, modification_time), offsetof(struct ext4_inode, mtime_extra));
        struct ext4_timestamp ctime = ext4_time_load(ref,
            offsetof(struct ext4_inode, change_inode_time), offsetof(struct ext4_inode, ctime_extra));
        if (ext4_time_compare(mtime, atime) < 0 &&
            ext4_time_compare(ctime, atime) < 0 &&
            now.seconds - atime.seconds < 86400)
            fields &= ~EXT4_TIME_ATIME;
    }
    if (fields & EXT4_TIME_ATIME)
        ext4_time_store(ref, offsetof(struct ext4_inode, access_time),
                        offsetof(struct ext4_inode, atime_extra), now);
    if (fields & EXT4_TIME_MTIME)
        ext4_time_store(ref, offsetof(struct ext4_inode, modification_time),
                        offsetof(struct ext4_inode, mtime_extra), now);
    if (fields & EXT4_TIME_CTIME)
        ext4_time_store(ref, offsetof(struct ext4_inode, change_inode_time),
                        offsetof(struct ext4_inode, ctime_extra), now);
}

static int ext4_link(struct ext4_mountpoint *mp, struct ext4_inode_ref *parent,
		     struct ext4_inode_ref *ch, const char *n,
		     uint32_t len, bool rename)
{
	/* Check maximum name length */
	if (len > EXT4_DIRECTORY_FILENAME_LEN)
		return EINVAL;

	/* Add entry to parent directory */
	int r = ext4_dir_add_entry(parent, n, len, ch);
	if (r != EOK)
		return r;

	/* Fill new dir -> add '.' and '..' entries.
	 * Also newly allocated inode should have 0 link count.
	 */

	bool is_dir = ext4_inode_is_type(&mp->fs.sb, ch->inode,
			       EXT4_INODE_MODE_DIRECTORY);
	if (is_dir && !rename) {

#if CONFIG_DIR_INDEX_ENABLE
		/* Initialize directory index if supported */
		if (ext4_sb_feature_com(&mp->fs.sb, EXT4_FCOM_DIR_INDEX)) {
			r = ext4_dir_dx_init(ch, parent);
			if (r != EOK)
				return r;

			ext4_inode_set_flag(ch->inode, EXT4_INODE_FLAG_INDEX);
			ch->dirty = true;
		} else
#endif
		{
			r = ext4_dir_add_entry(ch, ".", strlen("."), ch);
			if (r != EOK) {
				ext4_dir_remove_entry(parent, n, strlen(n));
				return r;
			}

			r = ext4_dir_add_entry(ch, "..", strlen(".."), parent);
			if (r != EOK) {
				ext4_dir_remove_entry(parent, n, strlen(n));
				ext4_dir_remove_entry(ch, ".", strlen("."));
				return r;
			}
		}

		/*New empty directory. Two links (. and ..) */
		ext4_inode_set_links_cnt(ch->inode, 2);
		ext4_fs_inode_links_count_inc(parent);
		ch->dirty = true;
		parent->dirty = true;
		ext4_touch_inode(mp, parent, EXT4_TIME_MTIME | EXT4_TIME_CTIME);
		return r;
	}
	/*
	 * In case we want to rename a directory,
	 * we reset the original '..' pointer.
	 */
	if (is_dir) {
		bool idx;
		idx = ext4_inode_has_flag(ch->inode, EXT4_INODE_FLAG_INDEX);
		struct ext4_dir_search_result res;
		if (!idx) {
			r = ext4_dir_find_entry(&res, ch, "..", strlen(".."));
			if (r != EOK)
				return r;

			ext4_dir_en_set_inode(res.dentry, parent->index);
			r = ext4_trans_set_block_dirty(res.block.buf);
			r = ext4_result(r, ext4_dir_destroy_result(ch, &res));
			if (r != EOK)
				return r;

		} else {
#if CONFIG_DIR_INDEX_ENABLE
			r = ext4_dir_dx_reset_parent_inode(ch, parent->index);
			if (r != EOK)
				return r;

#endif
		}

		ext4_fs_inode_links_count_inc(parent);
		parent->dirty = true;
	}
	if (!rename) {
		ext4_fs_inode_links_count_inc(ch);
		ch->dirty = true;
	}

	ext4_touch_inode(mp, parent, EXT4_TIME_MTIME | EXT4_TIME_CTIME);
	return r;
}

static int ext4_unlink(struct ext4_mountpoint *mp,
		       struct ext4_inode_ref *parent,
		       struct ext4_inode_ref *child, const char *name,
		       uint32_t name_len)
{
	bool has_children;
	int rc = ext4_has_children(&has_children, child);
	if (rc != EOK)
		return rc;

	/* Cannot unlink non-empty node */
	if (has_children)
		return ENOTEMPTY;

	/* Remove entry from parent directory */
	rc = ext4_dir_remove_entry(parent, name, name_len);
	if (rc != EOK)
		return rc;

	bool is_dir = ext4_inode_is_type(&mp->fs.sb, child->inode,
					 EXT4_INODE_MODE_DIRECTORY);

	/* If directory - handle links from parent */
	if (is_dir) {
		ext4_fs_inode_links_count_dec(parent);
		parent->dirty = true;
	}

    ext4_touch_inode(mp, parent, EXT4_TIME_MTIME | EXT4_TIME_CTIME);
    ext4_touch_inode(mp, child, EXT4_TIME_CTIME);
	if (ext4_inode_get_links_cnt(child->inode)) {
		ext4_fs_inode_links_count_dec(child);
		child->dirty = true;
	}

	return EOK;
}

/****************************************************************************/

int ext4_mount(const char *dev_name, const char *mount_point,
	       bool read_only)
{
	int r;
	uint32_t bsize;
	struct ext4_bcache *bc;
	struct ext4_blockdev *bd = 0;
	struct ext4_mountpoint *mp = 0;

	ext4_assert(mount_point && dev_name);

	size_t mp_len = strlen(mount_point);

	if (mp_len > CONFIG_EXT4_MAX_MP_NAME)
		return EINVAL;

	if (mount_point[mp_len - 1] != '/')
		return ENOTSUP;

	for (size_t i = 0; i < CONFIG_EXT4_BLOCKDEVS_COUNT; ++i) {
		if (!strcmp(dev_name, s_bdevices[i].name)) {
			bd = s_bdevices[i].bd;
			break;
		}
	}

	if (!bd)
		return ENODEV;

	for (size_t i = 0; i < CONFIG_EXT4_MOUNTPOINTS_COUNT; ++i) {
		if (!s_mp[i].mounted) {
			strcpy(s_mp[i].name, mount_point);
			s_mp[i].clock = NULL;
			s_mp[i].transaction_depth = 0;
			s_mp[i].transaction_error = EOK;
			s_mp[i].transaction_aborted = false;
			s_mp[i].journal_stopped = false;
			mp = &s_mp[i];
			break;
		}

		if (!strcmp(s_mp[i].name, mount_point))
			return EOK;
	}

	if (!mp)
		return ENOMEM;

	r = ext4_block_init(bd);
	if (r != EOK)
		return r;

	r = ext4_fs_init(&mp->fs, bd, read_only);
	if (r != EOK) {
		ext4_block_fini(bd);
		return r;
	}

	bsize = ext4_sb_get_block_size(&mp->fs.sb);
	ext4_block_set_lb_size(bd, bsize);
	bc = &mp->bc;

	r = ext4_bcache_init_dynamic(bc, CONFIG_BLOCK_DEV_CACHE_SIZE, bsize);
	if (r != EOK) {
		ext4_block_fini(bd);
		return r;
	}

	if (bsize != bc->itemsize)
		return ENOTSUP;

	/*Bind block cache to block device*/
	r = ext4_block_bind_bcache(bd, bc);
	if (r != EOK) {
		ext4_bcache_cleanup(bc);
		ext4_block_fini(bd);
		ext4_bcache_fini_dynamic(bc);
		return r;
	}

	bd->fs = &mp->fs;
	mp->mounted = 1;
	return r;
}


int ext4_umount(const char *mount_point)
{
	struct ext4_mountpoint *mp = NULL;
	for (size_t i = 0; i < CONFIG_EXT4_MOUNTPOINTS_COUNT; i++) {
		if (s_mp[i].mounted && !strcmp(s_mp[i].name, mount_point)) {
			mp = &s_mp[i];
			break;
		}
	}
	if (!mp) return ENODEV;
	if (mp->transaction_depth) return EBUSY;
	int r = EOK;
	if (mp->fs.jbd_journal && !mp->journal_stopped)
		r = ext4_orphan_recover(mount_point);
	if (r != EOK) return r;
	r = ext4_journal_stop(mount_point);
	if (r != EOK) return r;
	/* A failed write retains the mounted cache, device binding and inode
	 * owner. Only the successful final teardown makes the mount reusable. */
	r = ext4_block_cache_flush(mp->fs.bdev);
	if (r != EOK) return r;
	r = ext4_fs_fini(&mp->fs);
	if (r != EOK) return r;
	if (!mp->fs.read_only) {
		r = ext4_blockdev_flush(mp->fs.bdev);
		if (r != EOK) return r;
	}
	r = ext4_block_fini(mp->fs.bdev);
	if (r != EOK) return r;
	ext4_bcache_cleanup(mp->fs.bdev->bc);
	ext4_bcache_fini_dynamic(mp->fs.bdev->bc);
	mp->fs.bdev->fs = NULL;
	mp->mounted = false;
	return EOK;
}

static struct ext4_mountpoint *ext4_get_mount(const char *path)
{
	for (size_t i = 0; i < CONFIG_EXT4_MOUNTPOINTS_COUNT; ++i) {

		if (!s_mp[i].mounted)
			continue;

		if (!strncmp(s_mp[i].name, path, strlen(s_mp[i].name)))
			return &s_mp[i];
	}

	return NULL;
}

__unused
static int __ext4_journal_start(const char *mount_point)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	if (mp->fs.read_only || !ext4_sb_feature_com(&mp->fs.sb,
						EXT4_FCOM_HAS_JOURNAL)) return EOK;
	if (mp->fs.jbd_fs)
		return mp->fs.jbd_journal && !mp->journal_stopped
		       ? mp->jbd_journal.error : EBUSY;
	int r = jbd_get_fs(&mp->fs, &mp->jbd_fs);
	if (r != EOK) return r;
	mp->fs.jbd_fs = &mp->jbd_fs;
	r = jbd_journal_start(&mp->jbd_fs, &mp->jbd_journal);
	if (r != EOK) {
		if (mp->jbd_journal.error) {
			mp->fs.jbd_journal = &mp->jbd_journal;
			mp->journal_stopped = false;
			return r;
		}
		mp->jbd_fs.dirty = false;
		mp->journal_stopped = true;
		/* Keep a failed release reachable by journal_stop/unmount. */
		if (jbd_put_fs(&mp->jbd_fs) == EOK)
			mp->fs.jbd_fs = NULL;
		return r;
	}
	mp->journal_stopped = false;
	mp->fs.jbd_journal = &mp->jbd_journal;
	return EOK;
}

__unused
static int __ext4_journal_stop(const char *mount_point)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	if (mp->transaction_depth) return EBUSY;
	if (!mp->fs.jbd_fs) return EOK;
	int r;
	if (mp->fs.jbd_journal && !mp->journal_stopped) {
		if (mp->jbd_journal.error) return mp->jbd_journal.error;
		r = jbd_journal_stop(&mp->jbd_journal);
		if (r != EOK) return r;
		mp->journal_stopped = true;
	}
	/* An inode put can consume its reference while reporting a buffer I/O
	 * error. Its dirty buffer is then cache-owned, not a second inode ref. */
	if (mp->jbd_fs.inode_ref.block.data)
		r = jbd_put_fs(&mp->jbd_fs);
	else
		r = ext4_block_cache_flush(mp->fs.bdev);
	if (r != EOK) return r;
	mp->fs.jbd_journal = NULL;
	mp->fs.jbd_fs = NULL;
	mp->journal_stopped = false;
	return EOK;
}

__unused
static int __ext4_recover(const char *mount_point)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	int r = ENOTSUP;

	if (!mp)
		return ENOENT;

	EXT4_MP_LOCK(mp);
	if (ext4_sb_feature_com(&mp->fs.sb, EXT4_FCOM_HAS_JOURNAL)) {
		struct jbd_fs *jbd_fs = &mp->jbd_fs;
		if (mp->fs.jbd_fs) { r = EBUSY; goto Finish; }
		r = jbd_get_fs(&mp->fs, jbd_fs);
		if (r != EOK) goto Finish;
		mp->fs.jbd_fs = jbd_fs;
		mp->journal_stopped = true;
		r = jbd_recover(jbd_fs);
		int cleanup = jbd_put_fs(jbd_fs);
		if (cleanup == EOK) {
			mp->fs.jbd_fs = NULL;
			mp->journal_stopped = false;
		}
		r = ext4_result(r, cleanup);
	}
	if (r == EOK && !mp->fs.read_only) {
		uint32_t bgid;
		uint64_t free_blocks_count = 0;
		uint32_t free_inodes_count = 0;
		struct ext4_block_group_ref bg_ref;

		/* Update superblock's stats */
		for (bgid = 0;bgid < ext4_block_group_cnt(&mp->fs.sb);bgid++) {
			r = ext4_fs_get_block_group_ref(&mp->fs, bgid, &bg_ref);
			if (r != EOK)
				goto Finish;

			free_blocks_count +=
				ext4_bg_get_free_blocks_count(bg_ref.block_group,
						&mp->fs.sb);
			free_inodes_count +=
				ext4_bg_get_free_inodes_count(bg_ref.block_group,
						&mp->fs.sb);

			r = ext4_fs_put_block_group_ref(&bg_ref);
			if (r != EOK) goto Finish;
		}
		ext4_sb_set_free_blocks_cnt(&mp->fs.sb, free_blocks_count);
		ext4_set32(&mp->fs.sb, free_inodes_count, free_inodes_count);
		/* We don't need to save the superblock stats immediately. */
	}

Finish:
	EXT4_MP_UNLOCK(mp);
	return r;
}

__unused
static int __ext4_trans_start(struct ext4_mountpoint *mp)
{
	struct jbd_journal *journal = mp->fs.jbd_journal;
	if (!journal) return EOK;
	if (journal->error) return journal->error;
	if (mp->journal_stopped) return EBUSY;
	if (mp->transaction_error) return mp->transaction_error;
	if (mp->transaction_depth == UINT32_MAX) return EOVERFLOW;
	if (mp->transaction_depth == 0) {
		struct jbd_trans *trans = jbd_journal_new_trans(journal);
		if (!trans) return journal->error ? journal->error : ENOMEM;
		mp->transaction_sb = mp->fs.sb;
		mp->transaction_aborted = false;
		mp->fs.curr_trans = trans;
	}
	mp->transaction_depth++;
	return EOK;
}

__unused
static int ext4_trans_record_super(struct ext4_mountpoint *mp)
{
	struct ext4_block block = EXT4_BLOCK_ZERO();
	uint32_t block_size = ext4_sb_get_block_size(&mp->fs.sb);
	int r, cleanup;
	if (!memcmp(&mp->transaction_sb, &mp->fs.sb, sizeof(mp->fs.sb)))
		return EOK;
	r = ext4_trans_block_get(mp->fs.bdev, &block,
				EXT4_SUPERBLOCK_OFFSET / block_size);
	if (r != EOK) return r;
	ext4_sb_set_csum(&mp->fs.sb);
	memcpy(block.data + EXT4_SUPERBLOCK_OFFSET % block_size,
	       &mp->fs.sb, sizeof(mp->fs.sb));
	r = ext4_trans_set_block_dirty(block.buf);
	cleanup = ext4_block_set(mp->fs.bdev, &block);
	return r == EOK ? cleanup : r;
}

__unused
static int __ext4_trans_finish(struct ext4_mountpoint *mp, int error)
{
	struct jbd_journal *journal = mp->fs.jbd_journal;
	struct jbd_trans *trans = mp->fs.curr_trans;
	int r = error;
	if (!journal) return error;
	if (!mp->transaction_depth || !trans)
		return journal->error ? journal->error : (error ? error : EINVAL);
	if (!mp->transaction_error && error) mp->transaction_error = error;
	if (!mp->transaction_error && trans->error)
		mp->transaction_error = trans->error;
	if (--mp->transaction_depth) {
		mp->transaction_aborted = mp->transaction_error != EOK;
		return mp->transaction_error;
	}
	if (!mp->transaction_error)
		mp->transaction_error = journal->error;
	if (!mp->transaction_error)
		mp->transaction_error = ext4_trans_record_super(mp);
	if (!mp->transaction_error)
		mp->transaction_error = trans->error;
	r = mp->transaction_error;
	mp->transaction_error = EOK;
	mp->fs.curr_trans = NULL;
	if (r != EOK) {
		jbd_journal_free_trans(journal, trans, true);
		mp->fs.sb = mp->transaction_sb;
		mp->transaction_aborted = true;
		if (journal->error) r = journal->error;
	} else {
		/* A failed commit may already be durable. The journal retains that
		 * transaction; do not turn an uncertain commit into an abort. */
		r = jbd_journal_commit_trans(journal, trans);
		if (r != EOK && !journal->error) {
			/* Ordered-data failures happen before logging and the journal
			 * has already restored the buffer beforeimages. */
			mp->fs.sb = mp->transaction_sb;
			mp->transaction_aborted = true;
		}
	}
	mp->fs.curr_trans = NULL;
	return r;
}

int ext4_journal_start(const char *mount_point __unused)
{
	int r = EOK;
#if CONFIG_JOURNALING_ENABLE
	r = __ext4_journal_start(mount_point);
#endif
	return r;
}

int ext4_journal_stop(const char *mount_point __unused)
{
	int r = EOK;
#if CONFIG_JOURNALING_ENABLE
	r = __ext4_journal_stop(mount_point);
#endif
	return r;
}

int ext4_recover(const char *mount_point __unused)
{
	int r = EOK;
#if CONFIG_JOURNALING_ENABLE
	r = __ext4_recover(mount_point);
#endif
	return r;
}

static int ext4_trans_start(struct ext4_mountpoint *mp __unused)
{
	if (!mp) return ENOENT;
	if (mp->fs.super_replay_required) return EUCLEAN;
	if (mp->fs.read_only) return EROFS;
#if CONFIG_JOURNALING_ENABLE
	if (mp->fs.jbd_journal) return __ext4_trans_start(mp);
#endif
	if (mp->transaction_depth == UINT32_MAX) return EOVERFLOW;
	if (mp->transaction_depth++ == 0)
		ext4_bcache_scope_begin(&mp->bc, &mp->transaction_scope);
	return EOK;
}

static int ext4_trans_finish(struct ext4_mountpoint *mp __unused, int result)
{
#if CONFIG_JOURNALING_ENABLE
	if (mp->fs.jbd_journal) return __ext4_trans_finish(mp, result);
#endif
	if (!mp->transaction_depth) return result ? result : EINVAL;
	if (--mp->transaction_depth == 0) {
		int cleanup = ext4_bcache_scope_end(&mp->bc, &mp->transaction_scope);
		if (result == EOK) result = cleanup;
	}
	return result;
}

static bool ext4_file_rolled_back(ext4_file *file, int result)
{
	return result != EOK && file->mp->fs.jbd_journal &&
	       file->mp->transaction_aborted;
}

static void ext4_file_completed(ext4_file *file, int result)
{
	if (result == EOK && file->mp && file->mp->fs.jbd_journal &&
	    !file->mp->transaction_depth)
		file->sync_tid = file->mp->fs.jbd_journal->committed_id;
}

int ext4_transaction_begin(const char *mount_point)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	if (!mp->fs.jbd_journal) return ENOTSUP;
	return ext4_trans_start(mp);
}

int ext4_transaction_end(const char *mount_point)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	if (!mp->fs.jbd_journal) return ENOTSUP;
	return ext4_trans_finish(mp, EOK);
}

int ext4_transaction_abort(const char *mount_point, int error)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	if (!mp->fs.jbd_journal) return ENOTSUP;
	return ext4_trans_finish(mp, error ? error : ECANCELED);
}


int ext4_mount_point_stats(const char *mount_point,
			   struct ext4_mount_stats *stats)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);

	if (!mp)
		return ENOENT;

	EXT4_MP_LOCK(mp);
	stats->inodes_count = ext4_get32(&mp->fs.sb, inodes_count);
	stats->free_inodes_count = ext4_get32(&mp->fs.sb, free_inodes_count);
	stats->blocks_count = ext4_sb_get_blocks_cnt(&mp->fs.sb);
	stats->free_blocks_count = ext4_sb_get_free_blocks_cnt(&mp->fs.sb);
	stats->block_size = ext4_sb_get_block_size(&mp->fs.sb);

	stats->block_group_count = ext4_block_group_cnt(&mp->fs.sb);
	stats->blocks_per_group = ext4_get32(&mp->fs.sb, blocks_per_group);
	stats->inodes_per_group = ext4_get32(&mp->fs.sb, inodes_per_group);

	memcpy(stats->volume_name, mp->fs.sb.volume_name, 16);
	EXT4_MP_UNLOCK(mp);

	return EOK;
}

int ext4_mount_setup_locks(const char *mount_point,
			   const struct ext4_lock *locks)
{
	uint32_t i;
	struct ext4_mountpoint *mp = 0;

	for (i = 0; i < CONFIG_EXT4_MOUNTPOINTS_COUNT; ++i) {
		if (!strcmp(s_mp[i].name, mount_point)) {
			mp = &s_mp[i];
			break;
		}
	}
	if (!mp)
		return ENOENT;

	mp->os_locks = locks;
	return EOK;
}

int ext4_mount_setup_clock(const char *mount_point, ext4_clock_read clock)
{
    struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
    if (!mp) return ENOENT;
    mp->clock = clock;
    return EOK;
}

int ext4_file_touch(ext4_file *file, unsigned int fields)
{
    struct ext4_inode_ref ref;
    struct ext4_mountpoint *mp;
    struct ext4_timestamp now;
    int result;
    if (!file || !file->mp || !file->mp->mounted) return EINVAL;
    mp = file->mp;
    if (mp->fs.read_only)
        return fields & (EXT4_TIME_MTIME | EXT4_TIME_CTIME) ? EROFS : EOK;
    if (!mp->clock || !mp->clock(&now)) return EOK;
    EXT4_MP_LOCK(mp);
    result = ext4_trans_start(mp);
    if (result != EOK) { EXT4_MP_UNLOCK(mp); return result; }
    result = ext4_fs_get_inode_ref(&mp->fs, file->inode, &ref);
    if (result == EOK) {
        ext4_touch_inode(mp, &ref, fields);
        result = ext4_fs_put_inode_ref(&ref);
    }
    result = ext4_trans_finish(mp, result);
    ext4_file_completed(file, result);
    EXT4_MP_UNLOCK(mp);
    return result;
}

/********************************FILE OPERATIONS*****************************/

static int ext4_path_check(const char *path, bool *is_goal)
{
	int i;

	for (i = 0; i < EXT4_DIRECTORY_FILENAME_LEN; ++i) {

		if (path[i] == '/') {
			*is_goal = false;
			return i;
		}

		if (path[i] == 0) {
			*is_goal = true;
			return i;
		}
	}

	return 0;
}

static bool ext4_parse_flags(const char *flags, uint32_t *file_flags)
{
	if (!flags)
		return false;

	if (!strcmp(flags, "r") || !strcmp(flags, "rb")) {
		*file_flags = O_RDONLY;
		return true;
	}

	if (!strcmp(flags, "w") || !strcmp(flags, "wb")) {
		*file_flags = O_WRONLY | O_CREAT | O_TRUNC;
		return true;
	}

	if (!strcmp(flags, "a") || !strcmp(flags, "ab")) {
		*file_flags = O_WRONLY | O_CREAT | O_APPEND;
		return true;
	}

	if (!strcmp(flags, "r+") || !strcmp(flags, "rb+") ||
	    !strcmp(flags, "r+b")) {
		*file_flags = O_RDWR;
		return true;
	}

	if (!strcmp(flags, "w+") || !strcmp(flags, "wb+") ||
	    !strcmp(flags, "w+b")) {
		*file_flags = O_RDWR | O_CREAT | O_TRUNC;
		return true;
	}

	if (!strcmp(flags, "a+") || !strcmp(flags, "ab+") ||
	    !strcmp(flags, "a+b")) {
		*file_flags = O_RDWR | O_CREAT | O_APPEND;
		return true;
	}

	return false;
}

static int ext4_trunc_inode(struct ext4_mountpoint *mp,
			    uint32_t index, uint64_t new_size)
{
	struct ext4_inode_ref ref;
	int r = ext4_fs_get_inode_ref(&mp->fs, index, &ref);
	if (r != EOK) return r;
	uint64_t size = ext4_inode_get_size(&mp->fs.sb, ref.inode);
	/* The public operation owns the transaction. Never commit an enclosing
	 * unlink/rename halfway through a truncate. */
	while (r == EOK && size > new_size) {
		size = size - new_size > CONFIG_MAX_TRUNCATE_SIZE
		       ? size - CONFIG_MAX_TRUNCATE_SIZE : new_size;
		r = ext4_fs_truncate_inode(&ref, size);
	}
	int cleanup = ext4_fs_put_inode_ref(&ref);
	return r == EOK ? cleanup : r;
}

/* Finish a persisted truncate or unlink in bounded transactions. Live unlinked
 * inodes keep their orphan record until their last VFS owner calls free. */
static int ext4_reclaim_orphan(struct ext4_mountpoint *mp, uint32_t index,
			       bool release_inode)
{
	bool done = false;
	int r;
	if (!mp->fs.jbd_journal) return EOK;
	if (mp->transaction_depth) return EBUSY;
	do {
		struct ext4_inode_ref ref;
		r = ext4_trans_start(mp);
		if (r != EOK) return r;
		r = ext4_fs_get_inode_ref(&mp->fs, index, &ref);
		if (r != EOK) return ext4_trans_finish(mp, r);
		bool dead = !ext4_inode_get_links_cnt(ref.inode);
		if (dead && release_inode) {
			ext4_inode_set_size(ref.inode, 0);
			ref.dirty = true;
		}
		if (ext4_inode_can_truncate(&mp->fs.sb, ref.inode))
			r = ext4_orphan_truncate_step(&ref, &done);
		else done = true;
		if (r == EOK && done && (!dead || release_inode)) {
			r = ext4_orphan_remove(&ref);
			if (r == EOK && dead) {
				/* Once detached from the orphan chain, dtime resumes its
				 * deleted-inode meaning and must be nonzero. */
				ext4_inode_set_del_time(ref.inode, UINT32_MAX);
				r = ext4_fs_free_inode(&ref);
			}
		}
		r = ext4_result(r, ext4_fs_put_inode_ref(&ref));
		if (r == EOK && done) {
			uint32_t remaining;
			r = ext4_orphan_peek(&mp->fs, &remaining);
			if (r == EOK && !remaining)
				r = ext4_orphan_set_present(&mp->fs, false);
		}
		r = ext4_trans_finish(mp, r);
		if (r != EOK) return r;
	} while (!done);
	return EOK;
}

/* A persisted shrink can leave old mappings past i_size until its orphan is
 * reclaimed. Remove those mappings before a later grow can expose them. An
 * ordinary call uses bounded transactions; an explicitly grouped operation
 * must retain its caller's atomic boundary and rollback on resource failure.
 * Keep the orphan record in that outer transaction, especially for a live
 * unlinked inode whose last owner has not released it yet. */
static int ext4_prepare_growth(ext4_file *file, uint64_t visible_end)
{
	struct ext4_mountpoint *mp = file->mp;
	struct ext4_fs *fs = &mp->fs;
	struct ext4_inode_ref ref;
	bool present, done = false;
	int r;
	if (!fs->jbd_journal) return EOK;
	if (fs->jbd_journal->error) return fs->jbd_journal->error;
	if (mp->transaction_error) return mp->transaction_error;
	if (fs->curr_trans && fs->curr_trans->error) return fs->curr_trans->error;
	if (!ext4_get32(&fs->sb, last_orphan) &&
	    !ext4_sb_feature_ro_com(&fs->sb, EXT4_FRO_COM_ORPHAN_PRESENT))
		return EOK;
	r = ext4_orphan_contains(fs, file->inode, &present);
	if (r != EOK || !present) goto Finish;
	r = ext4_fs_get_inode_ref(fs, file->inode, &ref);
	if (r != EOK) goto Finish;
	if (visible_end <= ext4_inode_get_size(&fs->sb, ref.inode)) {
		r = ext4_fs_put_inode_ref(&ref);
		goto Finish;
	}
	if (!mp->transaction_depth) {
		r = ext4_fs_put_inode_ref(&ref);
		if (r == EOK) r = ext4_reclaim_orphan(mp, file->inode, false);
	} else {
		do {
			r = ext4_orphan_truncate_step(&ref, &done);
		} while (r == EOK && !done);
		r = ext4_result(r, ext4_fs_put_inode_ref(&ref));
	}
Finish:
	/* This preflight precedes the operation's nested begin, so latch errors
	 * here too: the caller must not commit a partly reclaimed outer group. */
	if (r != EOK && fs->curr_trans && !fs->curr_trans->error)
		fs->curr_trans->error = r;
	return r;
}

int ext4_orphan_recover(const char *mount_point)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	int r = ext4_orphan_validate(&mp->fs);
	if (r != EOK) return r;
	uint32_t inode;
	while ((r = ext4_orphan_peek(&mp->fs, &inode)) == EOK && inode) {
		if (mp->fs.read_only) return EROFS;
		if (!mp->fs.jbd_journal) return ENOTSUP;
		r = ext4_reclaim_orphan(mp, inode, true);
		if (r != EOK) return r;
	}
	if (r == EOK && ext4_sb_feature_ro_com(&mp->fs.sb,
					      EXT4_FRO_COM_ORPHAN_PRESENT)) {
		r = ext4_trans_start(mp);
		if (r == EOK) {
			r = ext4_orphan_set_present(&mp->fs, false);
			r = ext4_trans_finish(mp, r);
		}
	}
	return r;
}

static int ext4_trunc_dir(struct ext4_mountpoint *mp,
			  struct ext4_inode_ref *parent,
			  struct ext4_inode_ref *dir)
{
	int r = EOK;
	bool is_dir = ext4_inode_is_type(&mp->fs.sb, dir->inode,
			EXT4_INODE_MODE_DIRECTORY);
	uint32_t block_size = ext4_sb_get_block_size(&mp->fs.sb);
	if (!is_dir)
		return EINVAL;

#if CONFIG_DIR_INDEX_ENABLE
	/* Initialize directory index if supported */
	if (ext4_sb_feature_com(&mp->fs.sb, EXT4_FCOM_DIR_INDEX)) {
		r = ext4_dir_dx_init(dir, parent);
		if (r != EOK)
			return r;

		r = ext4_trunc_inode(mp, dir->index,
				     EXT4_DIR_DX_INIT_BCNT * block_size);
		if (r != EOK)
			return r;
	} else
#endif
	{
		r = ext4_trunc_inode(mp, dir->index, block_size);
		if (r != EOK)
			return r;
	}

	return ext4_fs_truncate_inode(dir, 0);
}

/*
 * NOTICE: if filetype is equal to EXT4_DIRENTRY_UNKNOWN,
 * any filetype of the target dir entry will be accepted.
 */
static int ext4_generic_open2(ext4_file *f, const char *path, int flags,
			      int ftype, uint32_t *parent_inode,
			      uint32_t *name_off)
{
	bool is_goal = false;
	uint32_t imode = EXT4_INODE_MODE_DIRECTORY;
	uint32_t next_inode;

	int r;
	int len;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	struct ext4_dir_search_result result;
	struct ext4_inode_ref ref;

	f->mp = 0;

	if (!mp)
		return ENOENT;

	struct ext4_fs *const fs = &mp->fs;
	struct ext4_sblock *const sb = &mp->fs.sb;
	if (fs->super_replay_required) return EUCLEAN;

	if (fs->read_only && flags & O_CREAT)
		return EROFS;

	f->flags = flags;

	/*Skip mount point*/
	path += strlen(mp->name);

	if (name_off)
		*name_off = strlen(mp->name);

	/*Load root*/
	r = ext4_fs_get_inode_ref(fs, EXT4_INODE_ROOT_INDEX, &ref);
	if (r != EOK)
		return r;

	if (parent_inode)
		*parent_inode = ref.index;

	len = ext4_path_check(path, &is_goal);
	while (1) {

		len = ext4_path_check(path, &is_goal);
		if (!len) {
			/*If root open was request.*/
			if (ftype == EXT4_DE_DIR || ftype == EXT4_DE_UNKNOWN)
				if (is_goal)
					break;

			r = ENOENT;
			break;
		}

		r = ext4_dir_find_entry(&result, &ref, path, len);
		if (r != EOK) {

			/*Destroy last result*/
			r = ext4_result(r, ext4_dir_destroy_result(&ref, &result));
			if (r != ENOENT)
				break;

			if (!(f->flags & O_CREAT))
				break;

			/*O_CREAT allows create new entry*/
			struct ext4_inode_ref child_ref;
			r = ext4_fs_alloc_inode(fs, &child_ref,
					is_goal ? ftype : EXT4_DE_DIR);

			if (r != EOK)
				break;

			ext4_fs_inode_blocks_init(fs, &child_ref);
			ext4_touch_inode(mp, &child_ref,
			    EXT4_TIME_ATIME | EXT4_TIME_MTIME | EXT4_TIME_CTIME);

			/*Link with root dir.*/
			r = ext4_link(mp, &ref, &child_ref, path, len, false);
			if (r != EOK) {
				/*Fail. Free new inode.*/
				ext4_fs_free_inode(&child_ref);
				/*We do not want to write new inode.
				  But block has to be released.*/
				child_ref.dirty = false;
				r = ext4_result(r, ext4_fs_put_inode_ref(&child_ref));
				break;
			}

			r = ext4_result(r, ext4_fs_put_inode_ref(&child_ref));
			if (r != EOK) break;
			continue;
		}

		if (parent_inode)
			*parent_inode = ref.index;

		next_inode = ext4_dir_en_get_inode(result.dentry);
		if (ext4_sb_feature_incom(sb, EXT4_FINCOM_FILETYPE)) {
			uint8_t t;
			t = ext4_dir_en_get_inode_type(sb, result.dentry);
			imode = ext4_fs_correspond_inode_mode(t);
		} else {
			struct ext4_inode_ref child_ref;
			r = ext4_fs_get_inode_ref(fs, next_inode, &child_ref);
			if (r != EOK) {
				ext4_dir_destroy_result(&ref, &result);
				break;
			}

			imode = ext4_inode_type(sb, child_ref.inode);
			r = ext4_result(r, ext4_fs_put_inode_ref(&child_ref));
		}

		r = ext4_result(r, ext4_dir_destroy_result(&ref, &result));
		if (r != EOK)
			break;

		/*If expected file error*/
		if (imode != EXT4_INODE_MODE_DIRECTORY && !is_goal) {
			r = ENOENT;
			break;
		}
		if (ftype != EXT4_DE_UNKNOWN) {
			bool df = imode != ext4_fs_correspond_inode_mode(ftype);
			if (df && is_goal) {
				r = ENOENT;
				break;
			}
		}

		r = ext4_fs_put_inode_ref(&ref);
		if (r != EOK)
			break;

		r = ext4_fs_get_inode_ref(fs, next_inode, &ref);
		if (r != EOK)
			break;

		if (is_goal)
			break;

		path += len + 1;

		if (name_off)
			*name_off += len + 1;
	}

	if (r != EOK) {
		if (ref.block.data)
			r = ext4_result(r, ext4_fs_put_inode_ref(&ref));
		return r;
	}

	if (is_goal) {

		if ((f->flags & O_TRUNC) && (imode == EXT4_INODE_MODE_FILE)) {
			if (mp->fs.jbd_journal && ext4_inode_get_size(sb, ref.inode)) {
				r = ext4_orphan_add(&ref);
				if (r == EOK) {
					ext4_inode_set_size(ref.inode, 0);
					ref.dirty = true;
				}
			} else if (!mp->fs.jbd_journal)
				r = ext4_trunc_inode(mp, ref.index, 0);
			if (r != EOK) {
				r = ext4_result(r, ext4_fs_put_inode_ref(&ref));
				return r;
			}
		}

		f->mp = mp;
		f->fsize = ext4_inode_get_size(sb, ref.inode);
		f->fmax = ext4_inode_max_size(fs, ref.inode);
		f->inode = ref.index;
		f->fpos = 0;
		f->sync_tid = mp->fs.jbd_journal ? mp->fs.jbd_journal->committed_id : 0;

		if (f->flags & O_APPEND)
			f->fpos = f->fsize;
	}

	return ext4_fs_put_inode_ref(&ref);
}

/****************************************************************************/

static int ext4_generic_open(ext4_file *f, const char *path, const char *flags,
			     bool file_expect, uint32_t *parent_inode,
			     uint32_t *name_off)
{
	uint32_t iflags;
	int filetype;
	int r;
	struct ext4_mountpoint *mp = ext4_get_mount(path);

	if (ext4_parse_flags(flags, &iflags) == false)
		return EINVAL;

	if (file_expect == true)
		filetype = EXT4_DE_REG_FILE;
	else
		filetype = EXT4_DE_DIR;

	if (iflags & (O_CREAT | O_TRUNC)) {
		r = ext4_trans_start(mp);
		if (r != EOK) return r;
	}

	r = ext4_generic_open2(f, path, iflags, filetype, parent_inode,
				name_off);

	if (iflags & (O_CREAT | O_TRUNC)) {
		r = ext4_trans_finish(mp, r);
	}

	if (r == EOK && (iflags & O_TRUNC) && mp->fs.jbd_journal &&
	    !mp->transaction_depth)
		r = ext4_reclaim_orphan(mp, f->inode, false);
	ext4_file_completed(f, r);
	return r;
}

static int ext4_create_hardlink(const char *path,
		struct ext4_inode_ref *child_ref, bool rename)
{
	bool is_goal = false;
	uint32_t inode_mode = EXT4_INODE_MODE_DIRECTORY;
	uint32_t next_inode;

	int r;
	int len;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	struct ext4_dir_search_result result;
	struct ext4_inode_ref ref;

	if (!mp)
		return ENOENT;

	struct ext4_fs *const fs = &mp->fs;
	struct ext4_sblock *const sb = &mp->fs.sb;

	/*Skip mount point*/
	path += strlen(mp->name);

	/*Load root*/
	r = ext4_fs_get_inode_ref(fs, EXT4_INODE_ROOT_INDEX, &ref);
	if (r != EOK)
		return r;

	len = ext4_path_check(path, &is_goal);
	while (1) {

		len = ext4_path_check(path, &is_goal);
		if (!len) {
			/*If root open was request.*/
			r = is_goal ? EINVAL : ENOENT;
			break;
		}

		r = ext4_dir_find_entry(&result, &ref, path, len);
		if (r != EOK) {

			/*Destroy last result*/
			r = ext4_result(r, ext4_dir_destroy_result(&ref, &result));

			if (r != ENOENT || !is_goal)
				break;

			/*Link with root dir.*/
			r = ext4_link(mp, &ref, child_ref, path, len, rename);
			break;
		} else if (r == EOK && is_goal) {
			/*Destroy last result*/
			r = ext4_result(r, ext4_dir_destroy_result(&ref, &result));
			r = EEXIST;
			break;
		}

		next_inode = result.dentry->inode;
		if (ext4_sb_feature_incom(sb, EXT4_FINCOM_FILETYPE)) {
			uint8_t t;
			t = ext4_dir_en_get_inode_type(sb, result.dentry);
			inode_mode = ext4_fs_correspond_inode_mode(t);
		} else {
			struct ext4_inode_ref child_ref;
			r = ext4_fs_get_inode_ref(fs, next_inode, &child_ref);
			if (r != EOK) {
				ext4_dir_destroy_result(&ref, &result);
				break;
			}

			inode_mode = ext4_inode_type(sb, child_ref.inode);
			r = ext4_result(r, ext4_fs_put_inode_ref(&child_ref));
		}

		r = ext4_result(r, ext4_dir_destroy_result(&ref, &result));
		if (r != EOK)
			break;

		if (inode_mode != EXT4_INODE_MODE_DIRECTORY) {
			r = is_goal ? EEXIST : ENOENT;
			break;
		}

		r = ext4_fs_put_inode_ref(&ref);
		if (r != EOK)
			break;

		r = ext4_fs_get_inode_ref(fs, next_inode, &ref);
		if (r != EOK)
			break;

		if (is_goal)
			break;

		path += len + 1;
	};

	if (r != EOK) {
		if (ref.block.data)
			r = ext4_result(r, ext4_fs_put_inode_ref(&ref));
		return r;
	}

	r = ext4_fs_put_inode_ref(&ref);
	return r;
}

static int ext4_remove_orig_reference(const char *path, uint32_t name_off,
				      struct ext4_inode_ref *parent_ref,
				      struct ext4_inode_ref *child_ref)
{
	bool is_goal;
	int r;
	int len;
	struct ext4_mountpoint *mp = ext4_get_mount(path);

	if (!mp)
		return ENOENT;

	/*Set path*/
	path += name_off;

	len = ext4_path_check(path, &is_goal);

	/* Remove entry from parent directory */
	r = ext4_dir_remove_entry(parent_ref, path, len);
	if (r != EOK)
		goto Finish;

	if (ext4_inode_is_type(&mp->fs.sb, child_ref->inode,
			       EXT4_INODE_MODE_DIRECTORY)) {
		ext4_fs_inode_links_count_dec(parent_ref);
		parent_ref->dirty = true;
	}
Finish:
	return r;
}

int ext4_flink(const char *path, const char *hardlink_path)
{
	int r;
	ext4_file f;
	uint32_t name_off;
	bool child_loaded = false;
	uint32_t parent_inode, child_inode;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	struct ext4_mountpoint *target_mp = ext4_get_mount(hardlink_path);
	struct ext4_inode_ref child_ref;

	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	/* Will that happen? Anyway return EINVAL for such case. */
	if (mp != target_mp)
		return EINVAL;

	EXT4_MP_LOCK(mp);
	r = ext4_generic_open2(&f, path, O_RDONLY, EXT4_DE_UNKNOWN,
			       &parent_inode, &name_off);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	child_inode = f.inode;
	ext4_fclose(&f);
	r = ext4_trans_start(mp);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	/*We have file to unlink. Load it.*/
	r = ext4_fs_get_inode_ref(&mp->fs, child_inode, &child_ref);
	if (r != EOK)
		goto Finish;

	child_loaded = true;

	/* Creating hardlink for directory is not allowed. */
	if (ext4_inode_is_type(&mp->fs.sb, child_ref.inode,
			       EXT4_INODE_MODE_DIRECTORY)) {
		r = EINVAL;
		goto Finish;
	}

	r = ext4_create_hardlink(hardlink_path, &child_ref, false);

Finish:
	if (child_loaded)
		r = ext4_result(r, ext4_fs_put_inode_ref(&child_ref));

	r = ext4_trans_finish(mp, r);

	EXT4_MP_UNLOCK(mp);
	return r;

}

int ext4_frename(const char *path, const char *new_path)
{
	int r;
	ext4_file f;
	uint32_t name_off;
	bool parent_loaded = false, child_loaded = false;
	uint32_t parent_inode, child_inode;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	struct ext4_inode_ref child_ref, parent_ref;

	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	EXT4_MP_LOCK(mp);

	r = ext4_generic_open2(&f, path, O_RDONLY, EXT4_DE_UNKNOWN,
				&parent_inode, &name_off);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	child_inode = f.inode;
	ext4_fclose(&f);
	r = ext4_trans_start(mp);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	/*Load parent*/
	r = ext4_fs_get_inode_ref(&mp->fs, parent_inode, &parent_ref);
	if (r != EOK)
		goto Finish;

	parent_loaded = true;

	/*We have file to unlink. Load it.*/
	r = ext4_fs_get_inode_ref(&mp->fs, child_inode, &child_ref);
	if (r != EOK)
		goto Finish;

	child_loaded = true;

	r = ext4_create_hardlink(new_path, &child_ref, true);
	if (r != EOK)
		goto Finish;

	r = ext4_remove_orig_reference(path, name_off, &parent_ref, &child_ref);
	if (r != EOK)
		goto Finish;

Finish:
	if (parent_loaded)
		r = ext4_result(r, ext4_fs_put_inode_ref(&parent_ref));

	if (child_loaded)
		r = ext4_result(r, ext4_fs_put_inode_ref(&child_ref));

	r = ext4_trans_finish(mp, r);

	EXT4_MP_UNLOCK(mp);
	return r;

}

/****************************************************************************/

int ext4_get_sblock(const char *mount_point, struct ext4_sblock **sb)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);

	if (!mp)
		return ENOENT;

	*sb = &mp->fs.sb;
	return EOK;
}

int ext4_cache_write_back(const char *path, bool on)
{
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	int ret;

	if (!mp)
		return ENOENT;

	EXT4_MP_LOCK(mp);
	ret = ext4_block_cache_write_back(mp->fs.bdev, on);
	EXT4_MP_UNLOCK(mp);
	return ret;
}

int ext4_cache_flush(const char *path)
{
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	int ret;

	if (!mp)
		return ENOENT;

	EXT4_MP_LOCK(mp);
	ret = ext4_block_cache_flush(mp->fs.bdev);
	EXT4_MP_UNLOCK(mp);
	return ret;
}

static int ext4_unlink_dentry_core(const char *path, uint32_t *out_inode,
                                  bool *out_is_orphan, bool allow_directory)
{
	ext4_file f;
	uint32_t parent_inode;
	uint32_t child_inode;
	uint32_t name_off;
	bool is_goal;
	int r;
	int len;
	struct ext4_inode_ref child;
	struct ext4_inode_ref parent;
	struct ext4_mountpoint *mp = ext4_get_mount(path);

	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	EXT4_MP_LOCK(mp);
	r = ext4_generic_open2(&f, path, O_RDONLY, EXT4_DE_UNKNOWN,
			       &parent_inode, &name_off);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	child_inode = f.inode;
	ext4_fclose(&f);
	r = ext4_trans_start(mp);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	/*Load parent*/
	r = ext4_fs_get_inode_ref(&mp->fs, parent_inode, &parent);
	if (r != EOK) {
		r = ext4_trans_finish(mp, r);
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	/*We have file to delete. Load it.*/
	r = ext4_fs_get_inode_ref(&mp->fs, child_inode, &child);
	if (r != EOK) {
		r = ext4_result(r, ext4_fs_put_inode_ref(&parent));
		r = ext4_trans_finish(mp, r);
		EXT4_MP_UNLOCK(mp);
		return r;
	}
	/* We do not allow unlinking directories via this call. */
	if (!allow_directory &&
	    ext4_inode_type(&mp->fs.sb, child.inode) ==
	    EXT4_INODE_MODE_DIRECTORY) {
		r = ext4_result(r, ext4_fs_put_inode_ref(&parent));
		r = ext4_result(r, ext4_fs_put_inode_ref(&child));
		r = ext4_trans_finish(mp, EISDIR);
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	/*Set path*/
	path += name_off;

	len = ext4_path_check(path, &is_goal);

	/*Unlink from parent*/
	r = ext4_unlink(mp, &parent, &child, path, len);
	if (r == EOK) {
		if (allow_directory) {
			ext4_inode_set_links_cnt(child.inode, 0);
			child.dirty = true;
		}
		if (out_inode)
			*out_inode = child_inode;
		if (out_is_orphan)
			*out_is_orphan = (ext4_inode_get_links_cnt(child.inode) == 0);
		if (!ext4_inode_get_links_cnt(child.inode) && mp->fs.jbd_journal)
			r = ext4_orphan_add(&child);
	}

	r = ext4_result(r, ext4_fs_put_inode_ref(&child));
	r = ext4_result(r, ext4_fs_put_inode_ref(&parent));

	r = ext4_trans_finish(mp, r);

	EXT4_MP_UNLOCK(mp);
	return r;
}

int ext4_funlink_dentry(const char *path, uint32_t *out_inode,
                        bool *out_is_orphan)
{
    return ext4_unlink_dentry_core(path, out_inode, out_is_orphan, false);
}

int ext4_fdir_unlink_dentry(const char *path, uint32_t *out_inode)
{
    bool orphan = false;
    return ext4_unlink_dentry_core(path, out_inode, &orphan, true);
}

int ext4_orphan_free(const char *path, uint32_t inode)
{
	int r;
	struct ext4_inode_ref child;
	struct ext4_mountpoint *mp = ext4_get_mount(path);

	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;
	if (mp->fs.jbd_journal)
		return ext4_reclaim_orphan(mp, inode, true);

	EXT4_MP_LOCK(mp);
	r = ext4_trans_start(mp);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	r = ext4_fs_get_inode_ref(&mp->fs, inode, &child);
	if (r != EOK) {
		r = ext4_trans_finish(mp, r);
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	if (!ext4_inode_get_links_cnt(child.inode)) {
		r = ext4_trunc_inode(mp, child.index, 0);
		if (r != EOK) {
			r = ext4_result(r, ext4_fs_put_inode_ref(&child));
			r = ext4_trans_finish(mp, r);
			EXT4_MP_UNLOCK(mp);
			return r;
		}

		ext4_inode_set_del_time(child.inode, -1L);
		r = ext4_fs_free_inode(&child);
	}

	r = ext4_result(r, ext4_fs_put_inode_ref(&child));

	r = ext4_trans_finish(mp, r);

	EXT4_MP_UNLOCK(mp);
	return r;
}

int ext4_fremove(const char *path)
{
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	bool grouped = mp && mp->fs.jbd_journal && mp->transaction_depth;
	uint32_t inode = 0;
	bool is_orphan = false;
	int r = ext4_funlink_dentry(path, &inode, &is_orphan);
	if (grouped) {
		/* The unlink and its persistent orphan belong to the outer group.
		 * Reclamation must wait until that group commits. Also latch errors
		 * from lookup/prechecks, which precede unlink's nested begin. */
		if (r != EOK && mp->fs.curr_trans && !mp->fs.curr_trans->error)
			mp->fs.curr_trans->error = r;
		return r;
	}
	if (r != EOK)
		return r;
	if (is_orphan)
		return ext4_orphan_free(path, inode);
	return EOK;
}

int ext4_fopen(ext4_file *file, const char *path, const char *flags)
{
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	int r;

	if (!mp)
		return ENOENT;

	EXT4_MP_LOCK(mp);

	r = ext4_generic_open(file, path, flags, true, 0, 0);

	EXT4_MP_UNLOCK(mp);
	return r;
}

int ext4_fopen2(ext4_file *file, const char *path, int flags)
{
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	int r;
	int filetype;

	if (!mp)
		return ENOENT;

        filetype = EXT4_DE_REG_FILE;

	EXT4_MP_LOCK(mp);

	if (flags & (O_CREAT | O_TRUNC)) {
		r = ext4_trans_start(mp);
		if (r != EOK) {
			EXT4_MP_UNLOCK(mp);
			return r;
		}
	}

	r = ext4_generic_open2(file, path, flags, filetype, NULL, NULL);

	if (flags & (O_CREAT | O_TRUNC)) {
		r = ext4_trans_finish(mp, r);
	}

	EXT4_MP_UNLOCK(mp);

	if (r == EOK && (flags & O_TRUNC) && mp->fs.jbd_journal &&
	    !mp->transaction_depth)
		r = ext4_reclaim_orphan(mp, file->inode, false);
	ext4_file_completed(file, r);
	return r;
}

int ext4_fclose(ext4_file *file)
{
	ext4_assert(file && file->mp);

	file->mp = 0;
	file->flags = 0;
	file->inode = 0;
	file->fpos = file->fsize = file->fmax = 0;
	file->sync_tid = 0;

	return EOK;
}


static int ext4_initialize_fblock(struct ext4_inode_ref *ref,
				  ext4_fsblk_t fblock)
{
	struct ext4_block block = EXT4_BLOCK_ZERO();
	uint32_t block_size = ext4_sb_get_block_size(&ref->fs->sb);
	int r = ext4_trans_data_get_noread(ref->fs->bdev, &block, fblock);
	if (r != EOK) return r;
	memset(block.data, 0, block_size);
	r = ext4_trans_set_data_dirty(block.buf);
	return ext4_result(r, ext4_block_set(ref->fs->bdev, &block));
}

static int ext4_zero_fblock_range(struct ext4_inode_ref *ref,
				  ext4_fsblk_t fblock, uint32_t offset,
				  uint32_t length)
{
	struct ext4_block block = EXT4_BLOCK_ZERO();
	int r = ext4_trans_data_get(ref->fs->bdev, &block, fblock);
	if (r != EOK) return r;
	memset(block.data + offset, 0, length);
	r = ext4_trans_set_data_dirty(block.buf);
	return ext4_result(r, ext4_block_set(ref->fs->bdev, &block));
}

static int ext4_write_fblock_range(struct ext4_inode_ref *ref,
				   ext4_fsblk_t fblock, uint32_t offset,
				   const uint8_t *buf, uint32_t length)
{
	uint32_t block_size = ext4_sb_get_block_size(&ref->fs->sb);
	struct ext4_block block = EXT4_BLOCK_ZERO();
	int r;

	if (offset == 0 && length == block_size)
		r = ext4_trans_data_get_noread(ref->fs->bdev, &block, fblock);
	else
		r = ext4_trans_data_get(ref->fs->bdev, &block, fblock);
	if (r != EOK) return r;
	memcpy(block.data + offset, buf, length);
	r = ext4_trans_set_data_dirty(block.buf);
	return ext4_result(r, ext4_block_set(ref->fs->bdev, &block));
}

static int ext4_flush_fblock_range(struct ext4_blockdev *bdev,
				   ext4_fsblk_t first, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		int r = ext4_block_flush_lba(bdev, first + i);
		if (r != EOK)
			return r;
	}
	return EOK;
}

static int ext4_zero_allocated_eof_tail(struct ext4_inode_ref *ref,
					uint64_t old_size,
					uint64_t visible_end)
{
	uint32_t block_size = ext4_sb_get_block_size(&ref->fs->sb);
	uint32_t offset = (uint32_t)(old_size % block_size);
	ext4_fsblk_t fblock;
	uint64_t block_end;
	uint32_t length;
	int r;

	if (!offset || visible_end <= old_size)
		return EOK;

	block_end = old_size - offset + block_size;
	if (visible_end < block_end)
		block_end = visible_end;
	length = (uint32_t)(block_end - old_size);
	r = ext4_fs_get_inode_dblk_idx(ref,
			(ext4_lblk_t)(old_size / block_size), &fblock, true);
	if (r != EOK || !fblock)
		return r;

	return ext4_zero_fblock_range(ref, fblock, offset, length);
}

static int ext4_ftruncate_no_lock(ext4_file *file, uint64_t size)
{
	struct ext4_inode_ref ref;
	struct ext4_sblock *sb = &file->mp->fs.sb;
	uint64_t old_size;
	int r;
	int cleanup_r;

	if (size > file->fmax)
		return EFBIG;

	r = ext4_fs_get_inode_ref(&file->mp->fs, file->inode, &ref);
	if (r != EOK)
		return r;


	old_size = ext4_inode_get_size(sb, ref.inode);
	file->fsize = old_size;
	if (old_size == size)
		goto Finish;

	if (old_size < size) {
		r = ext4_zero_allocated_eof_tail(&ref, old_size, size);
		if (r == EOK) {
			ext4_inode_set_size(ref.inode, size);
			ref.dirty = true;
		}
	} else {
		if (file->mp->fs.jbd_journal) {
			r = ext4_orphan_add(&ref);
			if (r == EOK) {
				ext4_inode_set_size(ref.inode, size);
				ref.dirty = true;
			}
		} else r = ext4_trunc_inode(file->mp, ref.index, size);
		if (r == EOK && file->fpos > size)
			file->fpos = size;
	}

Finish:
	if (r == EOK || ext4_inode_get_size(sb, ref.inode) != old_size) {
		ext4_touch_inode(file->mp, &ref, EXT4_TIME_MTIME | EXT4_TIME_CTIME);
	}
	file->fsize = ext4_inode_get_size(sb, ref.inode);
	cleanup_r = ext4_fs_put_inode_ref(&ref);
	if (r == EOK)
		r = cleanup_r;
	return r;
}

int ext4_ftruncate(ext4_file *f, uint64_t size)
{
	int r;
	uint64_t saved_size, saved_pos;
	ext4_assert(f && f->mp);

	if (f->mp->fs.read_only)
		return EROFS;

	if (f->flags & O_RDONLY)
		return EPERM;
	if (size > f->fmax)
		return EFBIG;

	EXT4_MP_LOCK(f->mp);

	r = ext4_prepare_growth(f, size);
	if (r == EOK) r = ext4_trans_start(f->mp);
	if (r != EOK) {
		EXT4_MP_UNLOCK(f->mp);
		return r;
	}
	saved_size = f->fsize;
	saved_pos = f->fpos;
	r = ext4_ftruncate_no_lock(f, size);
	r = ext4_trans_finish(f->mp, r);
	if (ext4_file_rolled_back(f, r)) {
		f->fsize = saved_size;
		f->fpos = saved_pos;
	}
	if (r == EOK && f->mp->fs.jbd_journal && !f->mp->transaction_depth)
		r = ext4_reclaim_orphan(f->mp, f->inode, false);
	ext4_file_completed(f, r);

	EXT4_MP_UNLOCK(f->mp);
	return r;
}

static int ext4_read_pending_data(struct ext4_blockdev *bdev,
				  ext4_fsblk_t first, uint32_t offset,
				  uint8_t *out, size_t length)
{
	while (length) {
		struct ext4_block block = EXT4_BLOCK_ZERO();
		size_t chunk = bdev->lg_bsize - offset;
		if (chunk > length) chunk = length;
		int r = ext4_block_get(bdev, &block, first++);
		if (r != EOK) return r;
		memcpy(out, block.data + offset, chunk);
		r = ext4_block_set(bdev, &block);
		if (r != EOK) return r;
		out += chunk;
		length -= chunk;
		offset = 0;
	}
	return EOK;
}

int ext4_fread(ext4_file *file, void *buf, size_t size, size_t *rcnt)
{
	uint32_t block_size;
	uint8_t *u8_buf = buf;
	int r;
	struct ext4_inode_ref ref;

	ext4_assert(file && file->mp);

	if (file->flags & O_WRONLY)
		return EPERM;

	if (!size)
		return EOK;

	EXT4_MP_LOCK(file->mp);

	struct ext4_fs *const fs = &file->mp->fs;
	struct ext4_sblock *const sb = &file->mp->fs.sb;

	if (rcnt)
		*rcnt = 0;

	r = ext4_fs_get_inode_ref(fs, file->inode, &ref);
	if (r != EOK) {
		EXT4_MP_UNLOCK(file->mp);
		return r;
	}

	/*Sync file size*/
	file->fsize = ext4_inode_get_size(sb, ref.inode);
	if (file->fsize > file->fmax) {
		r = EFBIG;
		goto Finish;
	}
	if (file->fpos >= file->fsize) {
		r = EOK;
		goto Finish;
	}

	block_size = ext4_sb_get_block_size(sb);
	size = ((uint64_t)size > (file->fsize - file->fpos))
		? ((size_t)(file->fsize - file->fpos)) : size;

	/*If the size of symlink is smaller than 60 bytes*/
	bool softlink;
	softlink = ext4_inode_is_type(sb, ref.inode, EXT4_INODE_MODE_SOFTLINK);
	if (softlink && file->fsize < sizeof(ref.inode->blocks)
		     && !ext4_inode_get_blocks_count(sb, ref.inode)) {

		char *content = (char *)ref.inode->blocks;
		if (file->fpos < file->fsize) {
			size_t len = size;
			memcpy(buf, content + file->fpos, len);
			file->fpos += len;
			if (rcnt)
				*rcnt = len;

		}

		r = EOK;
		goto Finish;
	}

	while (size && file->fpos % block_size) {
		uint32_t offset = (uint32_t)(file->fpos % block_size);
		ext4_lblk_t iblock = (ext4_lblk_t)(file->fpos / block_size);
		size_t length = block_size - offset;
		ext4_fsblk_t fblock;

		if (length > size)
			length = size;
		r = ext4_fs_get_inode_dblk_idx(&ref, iblock, &fblock, true);
		if (r != EOK)
			goto Finish;

		if (!fblock) {
			memset(u8_buf, 0, length);
		} else if (file->mp->fs.curr_trans) {
			r = ext4_read_pending_data(file->mp->fs.bdev, fblock,
						   offset, u8_buf, length);
			if (r != EOK) goto Finish;
		} else {
			uint64_t disk_offset = fblock * block_size + offset;
			r = ext4_flush_fblock_range(file->mp->fs.bdev, fblock, 1);
			if (r != EOK)
				goto Finish;
			r = ext4_block_readbytes(file->mp->fs.bdev, disk_offset,
						 u8_buf, (uint32_t)length);
			if (r != EOK)
				goto Finish;
		}

		u8_buf += length;
		size -= length;
		file->fpos += length;
		if (rcnt)
			*rcnt += length;
	}

	while (size >= block_size) {
		ext4_lblk_t iblock = (ext4_lblk_t)(file->fpos / block_size);
		size_t block_count = size / block_size;
		size_t run_count = 1;
		ext4_fsblk_t fblock;

		r = ext4_fs_get_inode_dblk_idx(&ref, iblock, &fblock, true);
		if (r != EOK)
			goto Finish;
		while (run_count < block_count) {
			ext4_fsblk_t next;

			r = ext4_fs_get_inode_dblk_idx(&ref,
							iblock + (ext4_lblk_t)run_count,
							&next, true);
			if (r != EOK)
				goto Finish;
			if ((!fblock && next) ||
			    (fblock && next != fblock + run_count))
				break;
			run_count++;
		}

		size_t length = run_count * block_size;
		if (!fblock) {
			memset(u8_buf, 0, length);
		} else if (file->mp->fs.curr_trans) {
			r = ext4_read_pending_data(file->mp->fs.bdev, fblock,
						   0, u8_buf, length);
			if (r != EOK) goto Finish;
		} else {
			r = ext4_flush_fblock_range(file->mp->fs.bdev, fblock,
						    run_count);
			if (r != EOK)
				goto Finish;
			r = ext4_blocks_get_direct(file->mp->fs.bdev, u8_buf,
						   fblock, (uint32_t)run_count);
			if (r != EOK)
				goto Finish;
		}

		u8_buf += length;
		size -= length;
		file->fpos += length;
		if (rcnt)
			*rcnt += length;
	}

	if (size) {
		ext4_lblk_t iblock = (ext4_lblk_t)(file->fpos / block_size);
		ext4_fsblk_t fblock;

		r = ext4_fs_get_inode_dblk_idx(&ref, iblock, &fblock, true);
		if (r != EOK)
			goto Finish;
		if (!fblock) {
			memset(u8_buf, 0, size);
		} else if (file->mp->fs.curr_trans) {
			r = ext4_read_pending_data(file->mp->fs.bdev, fblock,
						   0, u8_buf, size);
			if (r != EOK) goto Finish;
		} else {
			r = ext4_flush_fblock_range(file->mp->fs.bdev, fblock, 1);
			if (r != EOK)
				goto Finish;
			r = ext4_block_readbytes(file->mp->fs.bdev,
						 fblock * block_size,
						 u8_buf, (uint32_t)size);
			if (r != EOK)
				goto Finish;
		}
		file->fpos += size;
		if (rcnt)
			*rcnt += size;
	}

Finish:
	{
		int cleanup_r = ext4_fs_put_inode_ref(&ref);
		if (r == EOK)
			r = cleanup_r;
	}
	EXT4_MP_UNLOCK(file->mp);
	return r;
}

int ext4_fwrite(ext4_file *file, const void *buf, size_t size, size_t *wcnt)
{
	uint32_t block_size;
	struct ext4_inode_ref ref;
	const uint8_t *u8_buf = buf;
	uint64_t write_end, saved_size, saved_pos;
	int r;
	int cleanup_r;

	ext4_assert(file && file->mp);

	if (file->mp->fs.read_only)
		return EROFS;

	if (file->flags & O_RDONLY)
		return EPERM;

	if (!size)
		return EOK;
	if (file->fpos > UINT64_MAX - size)
		return EFBIG;
	write_end = file->fpos + size;
	if (write_end > file->fmax)
		return EFBIG;
	if (wcnt)
		*wcnt = 0;

	EXT4_MP_LOCK(file->mp);
	r = ext4_prepare_growth(file, write_end);
	if (r == EOK) r = ext4_trans_start(file->mp);
	if (r != EOK) {
		EXT4_MP_UNLOCK(file->mp);
		return r;
	}

	struct ext4_fs *const fs = &file->mp->fs;
	struct ext4_sblock *const sb = &file->mp->fs.sb;

	if (wcnt)
		*wcnt = 0;

	r = ext4_fs_get_inode_ref(fs, file->inode, &ref);
	if (r != EOK) {
		r = ext4_trans_finish(file->mp, r);
		EXT4_MP_UNLOCK(file->mp);
		return r;
	}


	/*Sync file size*/
	file->fsize = ext4_inode_get_size(sb, ref.inode);
	saved_size = file->fsize;
	saved_pos = file->fpos;
	block_size = ext4_sb_get_block_size(sb);

	if (write_end > file->fmax) {
		r = EFBIG;
		goto Finish;
	}

	r = ext4_zero_allocated_eof_tail(&ref, file->fsize, write_end);
	if (r != EOK)
		goto Finish;

	while (size) {
		uint32_t offset = (uint32_t)(file->fpos % block_size);
		ext4_lblk_t iblock = (ext4_lblk_t)(file->fpos / block_size);
		size_t length = block_size - offset;
		ext4_fsblk_t fblock;
		bool allocated;

		if (length > size)
			length = size;
		r = ext4_fs_get_or_alloc_inode_dblk_idx(&ref, iblock,
							&fblock, &allocated);
		if (r != EOK)
			goto Finish;
		if (allocated) {
			r = ext4_initialize_fblock(&ref, fblock);
			if (r != EOK) {
				cleanup_r = ext4_fs_release_inode_dblk_idx(
				    &ref, iblock, fblock);
				if (cleanup_r != EOK)
					r = cleanup_r;
				goto Finish;
			}
		}

		r = ext4_write_fblock_range(&ref, fblock, offset, u8_buf,
					    (uint32_t)length);
		if (r != EOK)
			goto Finish;

		u8_buf += length;
		size -= length;
		file->fpos += length;
		if (wcnt)
			*wcnt += length;
		if (file->fpos > ext4_inode_get_size(sb, ref.inode)) {
			ext4_inode_set_size(ref.inode, file->fpos);
			ref.dirty = true;
		}
	}

Finish:
	file->fsize = ext4_inode_get_size(sb, ref.inode);
	cleanup_r = ext4_fs_put_inode_ref(&ref);
	if (r == EOK)
		r = cleanup_r;

	r = ext4_trans_finish(file->mp, r);
	if (ext4_file_rolled_back(file, r)) {
		file->fsize = saved_size;
		file->fpos = saved_pos;
		if (wcnt) *wcnt = 0;
	}
	ext4_file_completed(file, r);

	EXT4_MP_UNLOCK(file->mp);
	return r;
}

int ext4_fseek(ext4_file *file, int64_t offset, uint32_t origin)
{
	uint64_t max_size = file->fmax;

	switch (origin) {
	case SEEK_SET:
		if (offset < 0 || (uint64_t)offset > max_size)
			return EINVAL;

		file->fpos = offset;
		return EOK;
	case SEEK_CUR: {
		uint64_t magnitude;

		if (file->fpos > max_size)
			return EINVAL;
		if (offset < 0) {
			magnitude = (uint64_t)(-(offset + 1)) + 1;
			if (magnitude > file->fpos)
				return EINVAL;
			file->fpos -= magnitude;
		} else {
			magnitude = (uint64_t)offset;
			if (magnitude > max_size - file->fpos)
				return EINVAL;
			file->fpos += magnitude;
		}
		return EOK;
	}
	case SEEK_END:
		if (file->fsize > max_size || offset < 0 ||
		    (uint64_t)offset > file->fsize)
			return EINVAL;

		file->fpos = file->fsize - offset;
		return EOK;
	}
	return EINVAL;
}

uint64_t ext4_ftell(ext4_file *file)
{
	return file->fpos;
}

uint64_t ext4_fsize(ext4_file *file)
{
	return file->fsize;
}


static int ext4_trans_get_inode_ref(const char *path,
				    struct ext4_mountpoint *mp,
				    struct ext4_inode_ref *inode_ref)
{
	int r;
	ext4_file f;

	r = ext4_generic_open2(&f, path, O_RDONLY, EXT4_DE_UNKNOWN, NULL, NULL);
	if (r != EOK)
		return r;

	r = ext4_trans_start(mp);
	if (r != EOK) return r;

	r = ext4_fs_get_inode_ref(&mp->fs, f.inode, inode_ref);
	if (r != EOK) {
		r = ext4_trans_finish(mp, r);
		return r;
	}

	return r;
}

static int ext4_trans_put_inode_ref(struct ext4_mountpoint *mp,
				    struct ext4_inode_ref *inode_ref)
{
	int r;

	r = ext4_fs_put_inode_ref(inode_ref);
	r = ext4_trans_finish(mp, r);

	return r;
}


int ext4_raw_inode_fill(const char *path, uint32_t *ret_ino,
			struct ext4_inode *inode)
{
	int r;
	ext4_file f;
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);

	if (!mp)
		return ENOENT;

	EXT4_MP_LOCK(mp);

	r = ext4_generic_open2(&f, path, O_RDONLY, EXT4_DE_UNKNOWN, NULL, NULL);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	/*Load parent*/
	r = ext4_fs_get_inode_ref(&mp->fs, f.inode, &inode_ref);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	if (ret_ino)
		*ret_ino = f.inode;

	memcpy(inode, inode_ref.inode, sizeof(struct ext4_inode));
	r = ext4_result(r, ext4_fs_put_inode_ref(&inode_ref));
	EXT4_MP_UNLOCK(mp);

	return r;
}

int ext4_fraw_inode_fill(const ext4_file *file, struct ext4_inode *inode)
{
	int r;
	size_t inode_size;
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp;

	if (!file || !inode || !file->mp || !file->mp->mounted)
		return EINVAL;
	mp = file->mp;
	EXT4_MP_LOCK(mp);
	r = ext4_fs_get_inode_ref(&mp->fs, file->inode, &inode_ref);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}
	inode_size = ext4_get16(&mp->fs.sb, inode_size);
	if (inode_size > sizeof(*inode))
		inode_size = sizeof(*inode);
	memset(inode, 0, sizeof(*inode));
	memcpy(inode, inode_ref.inode, inode_size);
	r = ext4_fs_put_inode_ref(&inode_ref);
	EXT4_MP_UNLOCK(mp);
	return r;
}

int ext4_file_sync_metadata(ext4_file *file)
{
	struct ext4_inode_ref ref;
	int r, cleanup;
	if (!file || !file->mp || !file->mp->mounted)
		return EINVAL;
	EXT4_MP_LOCK(file->mp);
#if CONFIG_JOURNALING_ENABLE
	if (file->mp->fs.jbd_journal) {
		r = file->mp->transaction_depth ? EBUSY :
		    jbd_journal_sync(file->mp->fs.jbd_journal, file->sync_tid);
		EXT4_MP_UNLOCK(file->mp);
		return r;
	}
#endif
	r = ext4_fs_get_inode_ref(&file->mp->fs, file->inode, &ref);
	if (r == EOK) {
		r = ext4_block_flush_buf(file->mp->fs.bdev, ref.block.buf);
		cleanup = ext4_fs_put_inode_ref(&ref);
		if (r == EOK) r = cleanup;
	}
	EXT4_MP_UNLOCK(file->mp);
	return r;
}

int ext4_inode_exist(const char *path, int type)
{
	int r;
	ext4_file f;
	struct ext4_mountpoint *mp = ext4_get_mount(path);

	if (!mp)
		return ENOENT;

	EXT4_MP_LOCK(mp);
	r = ext4_generic_open2(&f, path, O_RDONLY, type, NULL, NULL);
	EXT4_MP_UNLOCK(mp);

	return r;
}

int ext4_mode_set(const char *path, uint32_t mode)
{
	int r;
	uint32_t orig_mode;
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);

	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	EXT4_MP_LOCK(mp);

	r = ext4_trans_get_inode_ref(path, mp, &inode_ref);
	if (r != EOK)
		goto Finish;

	orig_mode = ext4_inode_get_mode(&mp->fs.sb, inode_ref.inode);
	orig_mode &= ~0xFFF;
	orig_mode |= mode & 0xFFF;
	ext4_inode_set_mode(&mp->fs.sb, inode_ref.inode, orig_mode);

	inode_ref.dirty = true;
	r = ext4_trans_put_inode_ref(mp, &inode_ref);

	Finish:
	EXT4_MP_UNLOCK(mp);

	return r;
}

int ext4_owner_set(const char *path, uint32_t uid, uint32_t gid)
{
	int r;
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);

	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	EXT4_MP_LOCK(mp);

	r = ext4_trans_get_inode_ref(path, mp, &inode_ref);
	if (r != EOK)
		goto Finish;

	ext4_inode_set_uid(inode_ref.inode, uid);
	ext4_inode_set_gid(inode_ref.inode, gid);

	inode_ref.dirty = true;
	r = ext4_trans_put_inode_ref(mp, &inode_ref);

	Finish:
	EXT4_MP_UNLOCK(mp);

	return r;
}

int ext4_mode_get(const char *path, uint32_t *mode)
{
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	ext4_file f;
	int r;

	if (!mp)
		return ENOENT;

	EXT4_MP_LOCK(mp);

	r = ext4_generic_open2(&f, path, O_RDONLY, EXT4_DE_UNKNOWN, NULL, NULL);
	if (r != EOK)
		goto Finish;

	r = ext4_fs_get_inode_ref(&mp->fs, f.inode, &inode_ref);
	if (r != EOK)
		goto Finish;

	*mode = ext4_inode_get_mode(&mp->fs.sb, inode_ref.inode);
	r = ext4_fs_put_inode_ref(&inode_ref);

	Finish:
	EXT4_MP_UNLOCK(mp);

	return r;
}

int ext4_owner_get(const char *path, uint32_t *uid, uint32_t *gid)
{
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	ext4_file f;
	int r;

	if (!mp)
		return ENOENT;

	EXT4_MP_LOCK(mp);

	r = ext4_generic_open2(&f, path, O_RDONLY, EXT4_DE_UNKNOWN, NULL, NULL);
	if (r != EOK)
		goto Finish;

	r = ext4_fs_get_inode_ref(&mp->fs, f.inode, &inode_ref);
	if (r != EOK)
		goto Finish;

	*uid = ext4_inode_get_uid(inode_ref.inode);
	*gid = ext4_inode_get_gid(inode_ref.inode);
	r = ext4_fs_put_inode_ref(&inode_ref);

	Finish:
	EXT4_MP_UNLOCK(mp);

	return r;
}

int ext4_atime_set(const char *path, uint32_t atime)
{
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	int r;

	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	EXT4_MP_LOCK(mp);

	r = ext4_trans_get_inode_ref(path, mp, &inode_ref);
	if (r != EOK)
		goto Finish;

	ext4_inode_set_access_time(inode_ref.inode, atime);
	inode_ref.dirty = true;
	r = ext4_trans_put_inode_ref(mp, &inode_ref);

	Finish:
	EXT4_MP_UNLOCK(mp);

	return r;
}

int ext4_mtime_set(const char *path, uint32_t mtime)
{
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	int r;

	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	EXT4_MP_LOCK(mp);

	r = ext4_trans_get_inode_ref(path, mp, &inode_ref);
	if (r != EOK)
		goto Finish;

	ext4_inode_set_modif_time(inode_ref.inode, mtime);
	inode_ref.dirty = true;
	r = ext4_trans_put_inode_ref(mp, &inode_ref);

	Finish:
	EXT4_MP_UNLOCK(mp);

	return r;
}

int ext4_ctime_set(const char *path, uint32_t ctime)
{
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	int r;

	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	EXT4_MP_LOCK(mp);

	r = ext4_trans_get_inode_ref(path, mp, &inode_ref);
	if (r != EOK)
		goto Finish;

	ext4_inode_set_change_inode_time(inode_ref.inode, ctime);
	inode_ref.dirty = true;
	r = ext4_trans_put_inode_ref(mp, &inode_ref);

	Finish:
	EXT4_MP_UNLOCK(mp);

	return r;
}

int ext4_atime_get(const char *path, uint32_t *atime)
{
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	ext4_file f;
	int r;

	if (!mp)
		return ENOENT;

	EXT4_MP_LOCK(mp);

	r = ext4_generic_open2(&f, path, O_RDONLY, EXT4_DE_UNKNOWN, NULL, NULL);
	if (r != EOK)
		goto Finish;

	r = ext4_fs_get_inode_ref(&mp->fs, f.inode, &inode_ref);
	if (r != EOK)
		goto Finish;

	*atime = ext4_inode_get_access_time(inode_ref.inode);
	r = ext4_fs_put_inode_ref(&inode_ref);

	Finish:
	EXT4_MP_UNLOCK(mp);

	return r;
}

int ext4_mtime_get(const char *path, uint32_t *mtime)
{
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	ext4_file f;
	int r;

	if (!mp)
		return ENOENT;

	EXT4_MP_LOCK(mp);

	r = ext4_generic_open2(&f, path, O_RDONLY, EXT4_DE_UNKNOWN, NULL, NULL);
	if (r != EOK)
		goto Finish;

	r = ext4_fs_get_inode_ref(&mp->fs, f.inode, &inode_ref);
	if (r != EOK)
		goto Finish;

	*mtime = ext4_inode_get_modif_time(inode_ref.inode);
	r = ext4_fs_put_inode_ref(&inode_ref);

	Finish:
	EXT4_MP_UNLOCK(mp);

	return r;
}

int ext4_ctime_get(const char *path, uint32_t *ctime)
{
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	ext4_file f;
	int r;

	if (!mp)
		return ENOENT;

	EXT4_MP_LOCK(mp);

	r = ext4_generic_open2(&f, path, O_RDONLY, EXT4_DE_UNKNOWN, NULL, NULL);
	if (r != EOK)
		goto Finish;

	r = ext4_fs_get_inode_ref(&mp->fs, f.inode, &inode_ref);
	if (r != EOK)
		goto Finish;

	*ctime = ext4_inode_get_change_inode_time(inode_ref.inode);
	r = ext4_fs_put_inode_ref(&inode_ref);

	Finish:
	EXT4_MP_UNLOCK(mp);

	return r;
}

static int ext4_fsymlink_set(ext4_file *f, const void *buf, uint32_t size)
{
	struct ext4_inode_ref ref;
	uint32_t sblock;
	ext4_fsblk_t fblock;
	uint32_t block_size;
	int r;

	ext4_assert(f && f->mp);

	if (!size)
		return EOK;

	r = ext4_fs_get_inode_ref(&f->mp->fs, f->inode, &ref);
	if (r != EOK)
		return r;

	/*Sync file size*/
	block_size = ext4_sb_get_block_size(&f->mp->fs.sb);
	if (size > block_size) {
		r = EINVAL;
		goto Finish;
	}
	r = ext4_ftruncate_no_lock(f, 0);
	if (r != EOK)
		goto Finish;

	/*If the size of symlink is smaller than 60 bytes*/
	if (size < sizeof(ref.inode->blocks)) {
		memset(ref.inode->blocks, 0, sizeof(ref.inode->blocks));
		memcpy(ref.inode->blocks, buf, size);
		ext4_inode_clear_flag(ref.inode, EXT4_INODE_FLAG_EXTENTS);
	} else {
		ext4_fs_inode_blocks_init(&f->mp->fs, &ref);
		r = ext4_fs_append_inode_dblk(&ref, &fblock, &sblock);
		if (r != EOK)
			goto Finish;

		r = ext4_initialize_fblock(&ref, fblock);
		if (r == EOK)
			r = ext4_write_fblock_range(&ref, fblock, 0, buf, size);
		if (r != EOK)
			goto Finish;
	}

	ext4_inode_set_size(ref.inode, size);
	ref.dirty = true;

	f->fsize = size;
	if (f->fpos > size)
		f->fpos = size;

Finish:
	r = ext4_result(r, ext4_fs_put_inode_ref(&ref));
	return r;
}

int ext4_fsymlink(const char *target, const char *path)
{
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	int r;
	ext4_file f;
	int filetype;

	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	filetype = EXT4_DE_SYMLINK;

	EXT4_MP_LOCK(mp);
	r = ext4_trans_start(mp);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	r = ext4_generic_open2(&f, path, O_RDWR | O_CREAT, filetype, NULL, NULL);
	if (r == EOK)
		r = ext4_fsymlink_set(&f, target, strlen(target));
	else
		goto Finish;

	ext4_fclose(&f);

Finish:
	r = ext4_trans_finish(mp, r);

	EXT4_MP_UNLOCK(mp);
	return r;
}

int ext4_readlink(const char *path, char *buf, size_t bufsize, size_t *rcnt)
{
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	int r;
	ext4_file f;
	int filetype;

	if (!mp)
		return ENOENT;

	if (!buf)
		return EINVAL;

	filetype = EXT4_DE_SYMLINK;

	EXT4_MP_LOCK(mp);
	r = ext4_generic_open2(&f, path, O_RDONLY, filetype, NULL, NULL);
	if (r == EOK)
		r = ext4_fread(&f, buf, bufsize, rcnt);
	else
		goto Finish;

	ext4_fclose(&f);

Finish:
	EXT4_MP_UNLOCK(mp);
	return r;
}

static int ext4_mknod_set(ext4_file *f, uint32_t dev)
{
	struct ext4_inode_ref ref;
	int r;

	ext4_assert(f && f->mp);

	r = ext4_fs_get_inode_ref(&f->mp->fs, f->inode, &ref);
	if (r != EOK)
		return r;

	ext4_inode_set_dev(ref.inode, dev);

	ext4_inode_set_size(ref.inode, 0);
	ref.dirty = true;

	f->fsize = 0;
	f->fpos = 0;

	r = ext4_fs_put_inode_ref(&ref);
	return r;
}

int ext4_mknod(const char *path, int filetype, uint32_t dev)
{
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	int r;
	ext4_file f;

	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	/*
	 * The filetype shouldn't be normal file, directory or
	 * unknown.
	 */
	if (filetype == EXT4_DE_UNKNOWN ||
	    filetype == EXT4_DE_REG_FILE ||
	    filetype == EXT4_DE_DIR ||
	    filetype == EXT4_DE_SYMLINK)
		return EINVAL;

	/*
	 * Nor should it be any bogus value.
	 */
	if (filetype != EXT4_DE_CHRDEV &&
	    filetype != EXT4_DE_BLKDEV &&
	    filetype != EXT4_DE_FIFO &&
	    filetype != EXT4_DE_SOCK)
		return EINVAL;

	EXT4_MP_LOCK(mp);
	r = ext4_trans_start(mp);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	r = ext4_generic_open2(&f, path, O_RDWR | O_CREAT, filetype, NULL, NULL);
	if (r == EOK) {
		if (filetype == EXT4_DE_CHRDEV ||
		    filetype == EXT4_DE_BLKDEV)
			r = ext4_mknod_set(&f, dev);
	} else {
		goto Finish;
	}

	ext4_fclose(&f);

Finish:
	r = ext4_trans_finish(mp, r);

	EXT4_MP_UNLOCK(mp);
	return r;
}

int ext4_setxattr(const char *path, const char *name, size_t name_len,
		  const void *data, size_t data_size)
{
	bool found;
	int r = EOK;
	ext4_file f;
	uint32_t inode;
	uint8_t name_index;
	const char *dissected_name = NULL;
	size_t dissected_len = 0;
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	dissected_name = ext4_extract_xattr_name(name, name_len,
				&name_index, &dissected_len,
				&found);
	if (!found)
		return EINVAL;

	EXT4_MP_LOCK(mp);
	r = ext4_generic_open2(&f, path, O_RDONLY, EXT4_DE_UNKNOWN, NULL, NULL);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	inode = f.inode;
	ext4_fclose(&f);
	r = ext4_trans_start(mp);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	r = ext4_fs_get_inode_ref(&mp->fs, inode, &inode_ref);
	if (r != EOK)
		goto Finish;

	r = ext4_xattr_set(&inode_ref, name_index, dissected_name,
			dissected_len, data, data_size);

	r = ext4_result(r, ext4_fs_put_inode_ref(&inode_ref));
Finish:
	r = ext4_trans_finish(mp, r);

	EXT4_MP_UNLOCK(mp);
	return r;
}

int ext4_getxattr(const char *path, const char *name, size_t name_len,
		  void *buf, size_t buf_size, size_t *data_size)
{
	bool found;
	int r = EOK;
	ext4_file f;
	uint32_t inode;
	uint8_t name_index;
	const char *dissected_name = NULL;
	size_t dissected_len = 0;
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	if (!mp)
		return ENOENT;

	dissected_name = ext4_extract_xattr_name(name, name_len,
				&name_index, &dissected_len,
				&found);
	if (!found)
		return EINVAL;

	EXT4_MP_LOCK(mp);
	r = ext4_generic_open2(&f, path, O_RDONLY, EXT4_DE_UNKNOWN, NULL, NULL);
	if (r != EOK)
		goto Finish;

	inode = f.inode;
	ext4_fclose(&f);

	r = ext4_fs_get_inode_ref(&mp->fs, inode, &inode_ref);
	if (r != EOK)
		goto Finish;

	r = ext4_xattr_get(&inode_ref, name_index, dissected_name,
				dissected_len, buf, buf_size, data_size);

	r = ext4_result(r, ext4_fs_put_inode_ref(&inode_ref));
Finish:
	EXT4_MP_UNLOCK(mp);
	return r;
}

int ext4_listxattr(const char *path, char *list, size_t size, size_t *ret_size)
{
	int r = EOK;
	ext4_file f;
	uint32_t inode;
	size_t list_len, list_size = 0;
	struct ext4_inode_ref inode_ref;
	struct ext4_xattr_list_entry *xattr_list = NULL,
				     *entry = NULL;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	if (!mp)
		return ENOENT;

	EXT4_MP_LOCK(mp);
	r = ext4_generic_open2(&f, path, O_RDONLY, EXT4_DE_UNKNOWN, NULL, NULL);
	if (r != EOK)
		goto Finish;
	inode = f.inode;
	ext4_fclose(&f);

	r = ext4_fs_get_inode_ref(&mp->fs, inode, &inode_ref);
	if (r != EOK)
		goto Finish;

	r = ext4_xattr_list(&inode_ref, NULL, &list_len);
	if (r == EOK && list_len) {
		xattr_list = ext4_malloc(list_len);
		if (!xattr_list) {
			r = ext4_result(r, ext4_fs_put_inode_ref(&inode_ref));
			r = ENOMEM;
			goto Finish;
		}
		entry = xattr_list;
		r = ext4_xattr_list(&inode_ref, entry, &list_len);
		if (r != EOK) {
			r = ext4_result(r, ext4_fs_put_inode_ref(&inode_ref));
			goto Finish;
		}

		for (;entry;entry = entry->next) {
			size_t prefix_len;
			const char *prefix =
				ext4_get_xattr_name_prefix(entry->name_index,
							   &prefix_len);
			if (size) {
				if (prefix_len + entry->name_len + 1 > size) {
					r = ext4_result(r, ext4_fs_put_inode_ref(&inode_ref));
					r = ERANGE;
					goto Finish;
				}
			}

			if (list && size) {
				memcpy(list, prefix, prefix_len);
				list += prefix_len;
				memcpy(list, entry->name,
					entry->name_len);
				list[entry->name_len] = 0;
				list += entry->name_len + 1;

				size -= prefix_len + entry->name_len + 1;
			}

			list_size += prefix_len + entry->name_len + 1;
		}
		if (ret_size)
			*ret_size = list_size;

	}
	r = ext4_result(r, ext4_fs_put_inode_ref(&inode_ref));
Finish:
	EXT4_MP_UNLOCK(mp);
	if (xattr_list)
		ext4_free(xattr_list);

	return r;

}

int ext4_removexattr(const char *path, const char *name, size_t name_len)
{
	bool found;
	int r = EOK;
	ext4_file f;
	uint32_t inode;
	uint8_t name_index;
	const char *dissected_name = NULL;
	size_t dissected_len = 0;
	struct ext4_inode_ref inode_ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	dissected_name = ext4_extract_xattr_name(name, name_len,
						&name_index, &dissected_len,
						&found);
	if (!found)
		return EINVAL;

	EXT4_MP_LOCK(mp);
	r = ext4_generic_open2(&f, path, O_RDONLY, EXT4_DE_UNKNOWN, NULL, NULL);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	inode = f.inode;
	ext4_fclose(&f);
	r = ext4_trans_start(mp);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	r = ext4_fs_get_inode_ref(&mp->fs, inode, &inode_ref);
	if (r != EOK)
		goto Finish;

	r = ext4_xattr_remove(&inode_ref, name_index, dissected_name,
			      dissected_len);

	r = ext4_result(r, ext4_fs_put_inode_ref(&inode_ref));
Finish:
	r = ext4_trans_finish(mp, r);

	EXT4_MP_UNLOCK(mp);
	return r;

}

/*********************************DIRECTORY OPERATION************************/

int ext4_dir_rm(const char *path)
{
	int r;
	int len;
	ext4_file f;

	struct ext4_mountpoint *mp = ext4_get_mount(path);
	struct ext4_inode_ref act;
	struct ext4_inode_ref child;
	struct ext4_dir_iter it;

	uint32_t name_off;
	uint32_t inode_up;
	uint32_t inode_current;
	uint32_t depth = 1;

	bool has_children;
	bool is_goal;
	bool dir_end;

	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	EXT4_MP_LOCK(mp);
	r = ext4_trans_start(mp);
	if (r != EOK) { EXT4_MP_UNLOCK(mp); return r; }

	struct ext4_fs *const fs = &mp->fs;

	/*Check if exist.*/
	r = ext4_generic_open(&f, path, "r", false, &inode_up, &name_off);
	if (r != EOK) {
		r = ext4_trans_finish(mp, r);
		EXT4_MP_UNLOCK(mp);
		return r;
	}

	path += name_off;
	len = ext4_path_check(path, &is_goal);
	inode_current = f.inode;


	do {

		uint64_t act_curr_pos = 0;
		has_children = false;
		dir_end = false;

		while (r == EOK && !has_children && !dir_end) {

			/*Load directory node.*/
			r = ext4_fs_get_inode_ref(fs, inode_current, &act);
			if (r != EOK) {
				break;
			}

			/*Initialize iterator.*/
			r = ext4_dir_iterator_init(&it, &act, act_curr_pos);
			if (r != EOK) {
				r = ext4_result(r, ext4_fs_put_inode_ref(&act));
				break;
			}

			if (!it.curr) {
				dir_end = true;
				goto End;
			}


			/*Get up directory inode when ".." entry*/
			if ((it.curr->name_len == 2) &&
			    ext4_is_dots(it.curr->name, it.curr->name_len)) {
				inode_up = ext4_dir_en_get_inode(it.curr);
			}

			/*If directory or file entry,  but not "." ".." entry*/
			if (!ext4_is_dots(it.curr->name, it.curr->name_len)) {

				/*Get child inode reference do unlink
				 * directory/file.*/
				uint32_t cinode;
				uint32_t inode_type;
				cinode = ext4_dir_en_get_inode(it.curr);
				r = ext4_fs_get_inode_ref(fs, cinode, &child);
				if (r != EOK)
					goto End;

				/*If directory with no leaf children*/
				r = ext4_has_children(&has_children, &child);
				if (r != EOK) {
					r = ext4_result(r, ext4_fs_put_inode_ref(&child));
					goto End;
				}

				if (has_children) {
					/*Has directory children. Go into this
					 * directory.*/
					inode_up = inode_current;
					inode_current = cinode;
					depth++;
					r = ext4_result(r, ext4_fs_put_inode_ref(&child));
					goto End;
				}
				inode_type = ext4_inode_type(&mp->fs.sb,
						child.inode);

				/* Truncate */
				if (inode_type != EXT4_INODE_MODE_DIRECTORY)
					r = ext4_trunc_inode(mp, child.index, 0);
				else
					r = ext4_trunc_dir(mp, &act, &child);

				if (r != EOK) {
					r = ext4_result(r, ext4_fs_put_inode_ref(&child));
					goto End;
				}

				/*No children in child directory or file. Just
				 * unlink.*/
				r = ext4_unlink(f.mp, &act, &child,
						(char *)it.curr->name,
						it.curr->name_len);
				if (r != EOK) {
					r = ext4_result(r, ext4_fs_put_inode_ref(&child));
					goto End;
				}

				ext4_inode_set_del_time(child.inode, -1L);
				ext4_inode_set_links_cnt(child.inode, 0);
				child.dirty = true;

				r = ext4_fs_free_inode(&child);
				if (r != EOK) {
					r = ext4_result(r, ext4_fs_put_inode_ref(&child));
					goto End;
				}

				r = ext4_fs_put_inode_ref(&child);
				if (r != EOK)
					goto End;

			}

			r = ext4_dir_iterator_next(&it);
			if (r != EOK)
				goto End;

			act_curr_pos = it.curr_off;
End:
			r = ext4_result(r, ext4_dir_iterator_fini(&it));
			if (r == EOK)
				r = ext4_fs_put_inode_ref(&act);
			else
				r = ext4_result(r, ext4_fs_put_inode_ref(&act));

		}

		if (dir_end) {
			/*Directory iterator reached last entry*/
			depth--;
			if (depth)
				inode_current = inode_up;

		}

		if (r != EOK)
			break;

	} while (depth);

	/*Last unlink*/
	if (r == EOK && !depth) {
		/*Load parent.*/
		struct ext4_inode_ref parent;
		r = ext4_fs_get_inode_ref(&f.mp->fs, inode_up,
				&parent);
		if (r != EOK)
			goto Finish;
		r = ext4_fs_get_inode_ref(&f.mp->fs, inode_current,
				&act);
		if (r != EOK) {
			r = ext4_result(r, ext4_fs_put_inode_ref(&parent));
			goto Finish;
		}


		/* In this place all directories should be
		 * unlinked.
		 * Last unlink from root of current directory*/
		r = ext4_unlink(f.mp, &parent, &act,
				(char *)path, len);
		if (r != EOK) {
			r = ext4_result(r, ext4_fs_put_inode_ref(&parent));
			r = ext4_result(r, ext4_fs_put_inode_ref(&act));
			goto Finish;
		}

		if (ext4_inode_get_links_cnt(act.inode) == 2) {
			ext4_inode_set_del_time(act.inode, -1L);
			ext4_inode_set_links_cnt(act.inode, 0);
			act.dirty = true;
			/*Truncate*/
			r = ext4_trunc_dir(mp, &parent, &act);
			if (r != EOK) {
				r = ext4_result(r, ext4_fs_put_inode_ref(&parent));
				r = ext4_result(r, ext4_fs_put_inode_ref(&act));
				goto Finish;
			}

			r = ext4_fs_free_inode(&act);
			if (r != EOK) {
				r = ext4_result(r, ext4_fs_put_inode_ref(&parent));
				r = ext4_result(r, ext4_fs_put_inode_ref(&act));
				goto Finish;
			}
		}

		r = ext4_fs_put_inode_ref(&parent);
		r = ext4_result(r, ext4_fs_put_inode_ref(&act));
	}

Finish:

	r = ext4_trans_finish(mp, r);
	EXT4_MP_UNLOCK(mp);

	return r;
}

int ext4_dir_mv(const char *path, const char *new_path)
{
	return ext4_frename(path, new_path);
}

int ext4_dir_mk(const char *path)
{
	int r;
	ext4_file f;
	struct ext4_mountpoint *mp = ext4_get_mount(path);

	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	EXT4_MP_LOCK(mp);

	/*Check if exist.*/
	r = ext4_generic_open(&f, path, "r", false, 0, 0);
	if (r == EOK)
		goto Finish;

	/*Create new directory.*/
	r = ext4_generic_open(&f, path, "w", false, 0, 0);

Finish:
	EXT4_MP_UNLOCK(mp);
	return r;
}

int ext4_dir_open_file(ext4_file *file, const char *path)
{
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	int r;

	if (!mp)
		return ENOENT;

	EXT4_MP_LOCK(mp);
	r = ext4_generic_open(file, path, "r", false, 0, 0);
	EXT4_MP_UNLOCK(mp);
	return r;
}

int ext4_chrdev_open_file(ext4_file *file, const char *path)
{
    struct ext4_mountpoint *mp = ext4_get_mount(path);
    int result;

    if (!mp)
        return ENOENT;
    EXT4_MP_LOCK(mp);
    result = ext4_generic_open2(file, path, O_RDONLY, EXT4_DE_CHRDEV,
                                NULL, NULL);
    EXT4_MP_UNLOCK(mp);
    return result;
}

int ext4_fopen_inode(ext4_file *file, const char *mount_point,
                     uint32_t inode_number)
{
    struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
    struct ext4_inode_ref ref;
    int result;

    if (!mp || !file || inode_number == 0)
        return EINVAL;
    EXT4_MP_LOCK(mp);
    result = ext4_fs_get_inode_ref(&mp->fs, inode_number, &ref);
    if (result == EOK) {
        file->mp = mp;
        file->inode = inode_number;
        file->flags = mp->fs.read_only ? O_RDONLY : O_RDWR;
        file->fpos = 0;
        file->sync_tid = mp->fs.jbd_journal ? mp->fs.jbd_journal->committed_id : 0;
        file->fsize = ext4_inode_get_size(&mp->fs.sb, ref.inode);
        file->fmax = ext4_inode_max_size(&mp->fs, ref.inode);
        result = ext4_fs_put_inode_ref(&ref);
    }
    EXT4_MP_UNLOCK(mp);
    return result;
}

int ext4_lookup_child(const char *mount_point, uint32_t parent_inode,
                      const char *name, uint32_t name_length,
                      uint32_t *child_inode, uint32_t *child_mode)
{
    struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
    struct ext4_inode_ref parent, child;
    struct ext4_dir_search_result found;
    int result;

    if (!mp || !name || name_length == 0 || name_length > 255 ||
        !child_inode || !child_mode)
        return EINVAL;
    EXT4_MP_LOCK(mp);
    result = ext4_fs_get_inode_ref(&mp->fs, parent_inode, &parent);
    if (result != EOK) goto Unlock;
    if (ext4_inode_type(&mp->fs.sb, parent.inode) !=
        EXT4_INODE_MODE_DIRECTORY) {
        result = ENOTDIR;
        goto PutParent;
    }
    result = ext4_dir_find_entry(&found, &parent, name, name_length);
    if (result == EOK) {
        uint32_t index = ext4_dir_en_get_inode(found.dentry);
        result = ext4_fs_get_inode_ref(&mp->fs, index, &child);
        if (result == EOK) {
            *child_inode = index;
            *child_mode = ext4_inode_get_mode(&mp->fs.sb, child.inode);
            result = ext4_fs_put_inode_ref(&child);
        }
    }
    {
        int cleanup = ext4_dir_destroy_result(&parent, &found);
        if (result == EOK) result = cleanup;
    }
PutParent:
    {
        int cleanup = ext4_fs_put_inode_ref(&parent);
        if (result == EOK) result = cleanup;
    }
Unlock:
    EXT4_MP_UNLOCK(mp);
    return result;
}

int ext4_readlink_inode(const char *mount_point, uint32_t inode_number,
                        char *buffer, size_t capacity, size_t *bytes_read)
{
    struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
    ext4_file file = {0};
    int result;

    if (!mp || !buffer || !bytes_read) return EINVAL;
    result = ext4_fopen_inode(&file, mount_point, inode_number);
    if (result != EOK) return result;
    EXT4_MP_LOCK(mp);
    result = ext4_fread(&file, buffer, capacity, bytes_read);
    ext4_fclose(&file);
    EXT4_MP_UNLOCK(mp);
    return result;
}

int ext4_dir_open(ext4_dir *dir, const char *path)
{
	int r;

	if (!dir)
		return ENOENT;

	r = ext4_dir_open_file(&dir->f, path);
	dir->next_off = 0;
	return r;
}

int ext4_dir_close(ext4_dir *dir)
{
    return ext4_fclose(&dir->f);
}

const ext4_direntry *ext4_dir_entry_next(ext4_dir *dir)
{
    uint64_t entry_offset;

    if (ext4_dir_entry_next_status(dir, &dir->de, &entry_offset) != EOK ||
        entry_offset == UINT64_MAX) {
        return 0;
    }
    return &dir->de;
}

int ext4_dir_entry_next_status(ext4_dir *dir,
                               ext4_direntry *entry,
                               uint64_t *entry_offset)
{
    struct ext4_inode_ref dir_inode;
    struct ext4_dir_iter it = {0};
    uint64_t directory_size;
    int result = EOK;
    int finish_result;

    if (dir == 0 || entry == 0 || entry_offset == 0 || dir->f.mp == 0) {
        return EINVAL;
    }
    memset(entry, 0, sizeof(*entry));
    *entry_offset = UINT64_MAX;

    EXT4_MP_LOCK(dir->f.mp);
    if (dir->next_off == UINT64_MAX) {
        EXT4_MP_UNLOCK(dir->f.mp);
        return EOK;
    }

    result = ext4_fs_get_inode_ref(&dir->f.mp->fs, dir->f.inode,
                                   &dir_inode);
    if (result != EOK) {
        EXT4_MP_UNLOCK(dir->f.mp);
        return result;
    }
    directory_size = ext4_inode_get_size(&dir_inode.fs->sb,
                                         dir_inode.inode);
    result = ext4_dir_iterator_init(&it, &dir_inode, dir->next_off);
    if (result == EOK && it.curr != 0) {
        for (;;) {
            uint64_t current_offset = it.curr_off;
            uint16_t record_length =
                ext4_dir_en_get_entry_len(it.curr);

            if (record_length < 8U ||
                current_offset > UINT64_MAX - record_length ||
                current_offset > directory_size ||
                record_length > directory_size - current_offset) {
                result = EIO;
                break;
            }
            dir->next_off = current_offset + record_length;
            if (ext4_dir_en_get_inode(it.curr) != 0U) {
                uint16_t name_length =
                    ext4_dir_en_get_name_len(&dir_inode.fs->sb, it.curr);

                if (name_length > sizeof(entry->name)) {
                    result = EIO;
                    break;
                }
                entry->inode = ext4_dir_en_get_inode(it.curr);
                entry->entry_length = record_length;
                entry->name_length = (uint8_t)name_length;
                entry->inode_type = ext4_dir_en_get_inode_type(
                    &dir_inode.fs->sb, it.curr);
                memcpy(entry->name, it.curr->name, name_length);
                *entry_offset = current_offset;
                break;
            }
            result = ext4_dir_iterator_next_raw(&it);
            if (result != EOK || it.curr == 0) {
                break;
            }
        }
    }
    if (result == EOK && it.curr == 0) {
        dir->next_off = UINT64_MAX;
    }
    if (it.inode_ref != 0) {
        finish_result = ext4_dir_iterator_fini(&it);
        if (result == EOK && finish_result != EOK) {
            result = finish_result;
        }
    }
    finish_result = ext4_fs_put_inode_ref(&dir_inode);
    if (result == EOK && finish_result != EOK) {
        result = finish_result;
    }
    EXT4_MP_UNLOCK(dir->f.mp);
    return result;
}

void ext4_dir_entry_rewind(ext4_dir *dir)
{
	dir->next_off = 0;
}

/**
 * @}
 */
