/* SPDX-License-Identifier: BSD-3-Clause
 * Persisted-size orphan reclamation. Format reference: Linux f4cdf7ca9a1f,
 * fs/ext4/indirect.c:ext4_ind_truncate and fs/ext4/extents.c:ext4_ext_truncate.
 */
#include <ext4_truncate.h>
#include <ext4_balloc.h>
#include <ext4_blockdev.h>
#include <ext4_errno.h>
#include <ext4_extent.h>
#include <ext4_inode.h>
#include <ext4_journal.h>
#include <ext4_super.h>
#include <ext4_trans.h>
#include <string.h>

#define TRUNCATE_BATCH 32U

static bool valid_block(struct ext4_inode_ref *ref, ext4_fsblk_t block)
{
    return block && block < ref->fs->bdev->lg_bcnt;
}

static int free_block(struct ext4_inode_ref *ref, ext4_fsblk_t block)
{
    if (!valid_block(ref, block) ||
        ext4_inode_get_blocks_count(&ref->fs->sb, ref->inode) <
        ref->fs->bdev->lg_bsize / EXT4_INODE_BLOCK_SIZE) return EUCLEAN;
    return ext4_balloc_free_block(ref, block);
}

/* Remove the highest data pointer, or one empty subtree, in this range.
 * Empty children are freed after releasing their buffer references. One walk
 * touches at most three indirect levels and never iterates over logical holes. */
static int indirect_tail(struct ext4_inode_ref *ref, ext4_fsblk_t physical,
                         unsigned level, uint64_t base, uint64_t keep,
                         ext4_fsblk_t ancestors[3], bool *changed, bool *empty)
{
    struct ext4_block block = EXT4_BLOCK_ZERO();
    struct ext4_blockdev *dev = ref->fs->bdev;
    uint32_t count = dev->lg_bsize / sizeof(uint32_t);
    uint64_t span = 1;
    if (!level || level > 3 || !valid_block(ref, physical)) return EUCLEAN;
    for (unsigned i = 0; i < 3 - level; ++i)
        if (ancestors[i] == physical) return EUCLEAN;
    ancestors[3 - level] = physical;
    for (unsigned i = 1; i < level; ++i) span *= count;
    int r = ext4_trans_block_get(dev, &block, physical);
    if (r != EOK) return r;
    uint32_t *entries = (uint32_t *)block.data;
    for (uint32_t end = count; end > 0; --end) {
        uint32_t i = end - 1;
        uint64_t first = base + (uint64_t)i * span;
        if (first + span <= keep) break;
        ext4_fsblk_t child = to_le32(entries[i]);
        if (!child) continue;
        if (!valid_block(ref, child)) { r = EUCLEAN; break; }
        bool child_empty = level == 1;
        if (level > 1) {
            r = indirect_tail(ref, child, level - 1, first, keep,
                              ancestors, changed, &child_empty);
            if (r != EOK) break;
        } else {
            for (unsigned j = 0; j <= 3 - level; ++j)
                if (ancestors[j] == child) r = EUCLEAN;
            if (r != EOK) break;
        }
        if (child_empty) {
            r = free_block(ref, child);
            if (r != EOK) break;
            entries[i] = 0;
            r = ext4_trans_set_block_dirty(block.buf);
            *changed = true;
        }
        if (r != EOK || *changed) break;
    }
    *empty = true;
    if (r == EOK)
        for (uint32_t i = 0; i < count; ++i)
            if (entries[i]) { *empty = false; break; }
    int release = ext4_block_set(dev, &block);
    return r != EOK ? r : release;
}

static int legacy_tail(struct ext4_inode_ref *ref, uint64_t keep, bool *changed)
{
    uint64_t count = ref->fs->bdev->lg_bsize / sizeof(uint32_t);
    uint64_t span = count * count * count;
    uint64_t base = EXT4_INODE_DIRECT_BLOCK_COUNT + count + count * count;
    *changed = false;
    for (unsigned level = 3; level > 0; --level) {
        ext4_fsblk_t physical = ext4_inode_get_indirect_block(ref->inode, level - 1);
        if (physical && base + span > keep) {
            ext4_fsblk_t ancestors[3] = {0};
            bool empty = false;
            int r = indirect_tail(ref, physical, level, base, keep,
                                  ancestors, changed, &empty);
            if (r != EOK) return r;
            if (empty) {
                r = free_block(ref, physical);
                if (r != EOK) return r;
                ext4_inode_set_indirect_block(ref->inode, level - 1, 0);
                ref->dirty = true;
                *changed = true;
            }
            if (*changed) return EOK;
        }
        span /= count;
        base -= span;
    }
    for (unsigned end = EXT4_INODE_DIRECT_BLOCK_COUNT; end > 0; --end) {
        unsigned i = end - 1;
        if (i < keep) break;
        ext4_fsblk_t physical = ext4_inode_get_direct_block(ref->inode, i);
        if (!physical) continue;
        int r = free_block(ref, physical);
        if (r != EOK) return r;
        ext4_inode_set_direct_block(ref->inode, i, 0);
        ref->dirty = true;
        *changed = true;
        break;
    }
    return EOK;
}

int ext4_orphan_truncate_step(struct ext4_inode_ref *ref, bool *done)
{
    if (!ref || !ref->fs || !ref->inode || !done) return EINVAL;
    *done = false;
    struct ext4_fs *fs = ref->fs;
    struct ext4_sblock *sb = &fs->sb;
    if (fs->read_only) return EROFS;
    if (!fs->jbd_journal || !fs->curr_trans) return EINVAL;
    if (fs->jbd_journal->error) return fs->jbd_journal->error;
    if (fs->curr_trans->error) return fs->curr_trans->error;
    uint32_t size = fs->bdev->lg_bsize;
    uint64_t target = ext4_inode_get_size(sb, ref->inode);
    uint64_t keep = target / size + (target % size != 0);
    uint32_t type = ext4_inode_type(sb, ref->inode);
    if (type != EXT4_INODE_MODE_FILE && type != EXT4_INODE_MODE_DIRECTORY &&
        type != EXT4_INODE_MODE_SOFTLINK) {
        /* Device numbers and special files have no data-block pointer tree. */
        if (type != EXT4_INODE_MODE_CHARDEV && type != EXT4_INODE_MODE_BLOCKDEV &&
            type != EXT4_INODE_MODE_FIFO && type != EXT4_INODE_MODE_SOCKET) return EUCLEAN;
        memset(ref->inode->blocks, 0, sizeof(ref->inode->blocks));
        ref->dirty = true;
        *done = true;
        return EOK;
    }
    uint64_t acl_blocks = ext4_inode_get_file_acl(ref->inode, sb) ? size / 512 : 0;
    if (type == EXT4_INODE_MODE_SOFTLINK &&
        ext4_inode_get_blocks_count(sb, ref->inode) == acl_blocks) {
        if (target > sizeof(ref->inode->blocks)) return EUCLEAN;
        memset((unsigned char *)ref->inode->blocks + target, 0,
               sizeof(ref->inode->blocks) - (size_t)target);
        ref->dirty = true;
        *done = true;
        return EOK;
    }
#if CONFIG_EXTENT_ENABLE && CONFIG_EXTENTS_ENABLE
    if (ext4_inode_has_flag(ref->inode, EXT4_INODE_FLAG_EXTENTS)) {
        if (!ext4_sb_feature_incom(sb, EXT4_FINCOM_EXTENTS)) return EUCLEAN;
        ext4_lblk_t last = 0;
        bool found;
        int r = ext4_extent_last_block(ref, &last, &found);
        if (r != EOK) return r;
        if (!found || last < keep) { *done = true; return EOK; }
        uint64_t first = last >= TRUNCATE_BATCH - 1 ? last - (TRUNCATE_BATCH - 1) : 0;
        if (first < keep) first = keep;
        return ext4_extent_remove_space(ref, (ext4_lblk_t)first, last);
    }
#endif
    for (unsigned i = 0; i < TRUNCATE_BATCH; ++i) {
        bool changed;
        int r = legacy_tail(ref, keep, &changed);
        if (r != EOK) return r;
        if (!changed) { *done = true; break; }
    }
    return EOK;
}
