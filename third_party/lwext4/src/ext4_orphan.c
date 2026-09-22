/* SPDX-License-Identifier: BSD-3-Clause
 * Persistent orphan records. Format reference: Linux f4cdf7ca9a1f,
 * fs/ext4/orphan.c, fs/ext4/ialloc.c and Documentation/filesystems/ext4/orphan.rst.
 */
#include <ext4_orphan.h>
#include <ext4_bitmap.h>
#include <ext4_block_group.h>
#include <ext4_blockdev.h>
#include <ext4_crc32.h>
#include <ext4_errno.h>
#include <ext4_inode.h>
#include <ext4_ialloc.h>
#include <ext4_journal.h>
#include <ext4_super.h>
#include <ext4_trans.h>
#include <stdlib.h>
#include <string.h>

struct orphan_tail { uint32_t magic, checksum; };
struct orphan_seen { uint32_t *table; size_t capacity, count; };
struct orphan_scan {
    uint32_t first, target, previous, next, slot, free_slot, seed;
    ext4_fsblk_t block, free_block;
    bool found;
    struct orphan_seen seen;
};

static int seen_insert(struct orphan_seen *seen, uint32_t inode)
{
    if (seen->count >= seen->capacity / 2) {
        size_t cap = seen->capacity ? seen->capacity * 2 : 32;
        if (cap < seen->capacity || cap > SIZE_MAX / sizeof(uint32_t)) return ENOMEM;
        uint32_t *table = ext4_calloc(cap, sizeof(*table));
        if (!table) return ENOMEM;
        for (size_t i = 0; i < seen->capacity; i++) if (seen->table[i]) {
            size_t slot = (seen->table[i] * 2654435761U) & (cap - 1);
            while (table[slot]) slot = (slot + 1) & (cap - 1);
            table[slot] = seen->table[i];
        }
        ext4_free(seen->table);
        seen->table = table;
        seen->capacity = cap;
    }
    size_t slot = (inode * 2654435761U) & (seen->capacity - 1);
    while (seen->table[slot]) {
        if (seen->table[slot] == inode) return EUCLEAN;
        slot = (slot + 1) & (seen->capacity - 1);
    }
    seen->table[slot] = inode;
    seen->count++;
    return EOK;
}

static uint32_t inode_seed(struct ext4_inode_ref *ref)
{
    uint32_t ino = to_le32(ref->index);
    uint32_t gen = to_le32(ext4_inode_get_generation(ref->inode));
    uint32_t sum = ext4_sb_get_csum_seed(&ref->fs->sb);
    sum = ext4_crc32c(sum, &ino, sizeof(ino));
    return ext4_crc32c(sum, &gen, sizeof(gen));
}

static uint32_t inode_checksum(struct ext4_inode_ref *ref)
{
    uint32_t saved = ext4_inode_get_csum(&ref->fs->sb, ref->inode);
    uint16_t size = ext4_get16(&ref->fs->sb, inode_size);
    ext4_inode_set_csum(&ref->fs->sb, ref->inode, 0);
    uint32_t sum = ext4_crc32c(inode_seed(ref), ref->inode, size);
    ext4_inode_set_csum(&ref->fs->sb, ref->inode, saved);
    return size == EXT4_GOOD_OLD_INODE_SIZE ? sum & 0xffff : sum;
}

static void finish_inode(struct ext4_inode_ref *ref)
{
    if (ext4_sb_feature_ro_com(&ref->fs->sb, EXT4_FRO_COM_METADATA_CSUM))
        ext4_inode_set_csum(&ref->fs->sb, ref->inode, inode_checksum(ref));
    ref->dirty = true;
}

static int check_allocated(struct ext4_fs *fs, uint32_t ino, bool special)
{
    uint32_t first = special ? 1 : ext4_get32(&fs->sb, first_inode);
    uint32_t per_group = ext4_get32(&fs->sb, inodes_per_group);
    uint32_t inode_size = ext4_get16(&fs->sb, inode_size);
    uint32_t descriptor_size = ext4_sb_get_desc_size(&fs->sb);
    uint32_t block_size = fs->bdev->lg_bsize;
    if (inode_size < EXT4_GOOD_OLD_INODE_SIZE || inode_size > block_size ||
        block_size % inode_size || descriptor_size < EXT4_MIN_BLOCK_GROUP_DESCRIPTOR_SIZE ||
        descriptor_size > block_size || block_size % descriptor_size) return EUCLEAN;
    if (ino < first || ino > ext4_get32(&fs->sb, inodes_count) || !per_group ||
        per_group > (uint64_t)fs->bdev->lg_bsize * 8 ||
        (!special && (ino == ext4_get32(&fs->sb, orphan_file_inum) ||
                      ino == ext4_get32(&fs->sb, journal_inode_number)))) return EUCLEAN;
    struct ext4_block_group_ref group;
    int r = ext4_fs_get_block_group_ref(fs, (ino - 1) / per_group, &group);
    if (r != EOK) return r;
    if (ext4_bg_has_flag(group.block_group, EXT4_BLOCK_GROUP_INODE_UNINIT)) {
        ext4_fs_put_block_group_ref(&group);
        return EUCLEAN;
    }
    ext4_fsblk_t lba = ext4_bg_get_inode_bitmap(group.block_group, &fs->sb);
    if (!lba || lba >= fs->bdev->lg_bcnt) {
        ext4_fs_put_block_group_ref(&group);
        return EUCLEAN;
    }
    struct ext4_block bitmap;
    r = ext4_block_get(fs->bdev, &bitmap, lba);
    if (r != EOK) {
        ext4_fs_put_block_group_ref(&group);
        return r;
    }
    bool allocated = ext4_ialloc_verify_bitmap_csum(&fs->sb,
                         group.block_group, bitmap.data) &&
                     ext4_bmap_is_bit_set(bitmap.data, (ino - 1) % per_group);
    r = ext4_block_set(fs->bdev, &bitmap);
    int release = ext4_fs_put_block_group_ref(&group);
    if (r == EOK) r = release;
    return r != EOK ? r : allocated ? EOK : EUCLEAN;
}

static bool valid_orphan_type(struct ext4_fs *fs, struct ext4_inode *inode)
{
    uint32_t type = ext4_inode_get_mode(&fs->sb, inode) & EXT4_INODE_MODE_TYPE_MASK;
    bool data_type = type == EXT4_INODE_MODE_FILE || type == EXT4_INODE_MODE_DIRECTORY ||
                     type == EXT4_INODE_MODE_SOFTLINK;
    bool other_type = type == EXT4_INODE_MODE_FIFO || type == EXT4_INODE_MODE_CHARDEV ||
                      type == EXT4_INODE_MODE_BLOCKDEV || type == EXT4_INODE_MODE_SOCKET;
    return data_type || (other_type && !ext4_inode_get_links_cnt(inode));
}

static int load_inode(struct ext4_fs *fs, uint32_t ino,
                      struct ext4_inode_ref *ref, bool special)
{
    int r = check_allocated(fs, ino, special);
    if (r != EOK) return r;
    r = ext4_fs_get_inode_ref(fs, ino, ref);
    if (r != EOK) return r;
    if (!valid_orphan_type(fs, ref->inode)) r = EUCLEAN;
    if (ext4_sb_feature_ro_com(&fs->sb, EXT4_FRO_COM_METADATA_CSUM) &&
        ext4_inode_get_csum(&fs->sb, ref->inode) != inode_checksum(ref)) r = EIO;
    if (r != EOK) ext4_fs_put_inode_ref(ref);
    return r;
}

static uint32_t block_checksum(uint32_t seed, ext4_fsblk_t lba,
                               const void *data, uint32_t size)
{
    uint64_t block = to_le64(lba);
    uint32_t sum = ext4_crc32c(seed, &block, sizeof(block));
    return ext4_crc32c(sum, data, size - sizeof(struct orphan_tail));
}

static int check_block(struct ext4_fs *fs, struct ext4_block *block, uint32_t seed)
{
    uint32_t size = fs->bdev->lg_bsize;
    struct orphan_tail *tail = (void *)(block->data + size - sizeof(*tail));
    if (to_le32(tail->magic) != EXT4_ORPHAN_BLOCK_MAGIC) return EUCLEAN;
    if (ext4_sb_feature_ro_com(&fs->sb, EXT4_FRO_COM_METADATA_CSUM) &&
        to_le32(tail->checksum) != block_checksum(seed, block->lb_id, block->data, size))
        return EIO;
    return EOK;
}

static int scan_file(struct ext4_fs *fs, struct orphan_scan *scan,
                     struct ext4_inode_ref *caller)
{
    if (!ext4_sb_feature_com(&fs->sb, EXT4_ORPHAN_FILE_COMPAT)) {
        return ext4_sb_feature_ro_com(&fs->sb, EXT4_ORPHAN_PRESENT_RO_COMPAT) ? EUCLEAN : EOK;
    }
    struct ext4_inode_ref file;
    int r = load_inode(fs, ext4_get32(&fs->sb, orphan_file_inum), &file, true);
    if (r != EOK) return r;
    uint32_t size = fs->bdev->lg_bsize;
    uint64_t length = ext4_inode_get_size(&fs->sb, file.inode);
    uint64_t blocks = length / size;
    if ((ext4_inode_get_mode(&fs->sb, file.inode) & EXT4_INODE_MODE_TYPE_MASK) !=
        EXT4_INODE_MODE_FILE || length % size ||
        blocks > fs->bdev->lg_bcnt || blocks > UINT32_MAX) { r = EUCLEAN; goto Done; }
    scan->seed = inode_seed(&file);
    for (uint64_t i = 0; i < blocks; i++) {
        ext4_fsblk_t physical;
        r = ext4_fs_get_inode_dblk_idx(&file, (ext4_lblk_t)i, &physical, false);
        if (r == EINVAL) r = EUCLEAN;
        if (r != EOK) break;
        if (!physical || physical >= fs->bdev->lg_bcnt) { r = EUCLEAN; break; }
        struct ext4_block block;
        r = ext4_block_get(fs->bdev, &block, physical);
        if (r != EOK) break;
        r = check_block(fs, &block, scan->seed);
        uint32_t *entries = (void *)block.data;
        for (uint32_t j = 0; r == EOK && j < (size - sizeof(struct orphan_tail)) / 4; j++) {
            uint32_t ino = to_le32(entries[j]);
            if (!ino) {
                if (!scan->free_block) { scan->free_block = physical; scan->free_slot = j; }
                continue;
            }
            r = seen_insert(&scan->seen, ino);
            if (r != EOK) break;
            if (caller && ino == caller->index) r = check_allocated(fs, ino, false);
            else {
                struct ext4_inode_ref ref;
                r = load_inode(fs, ino, &ref, false);
                if (r == EOK) r = ext4_fs_put_inode_ref(&ref);
            }
            if (r != EOK) break;
            if (!scan->first) scan->first = ino;
            if (ino == scan->target) {
                scan->found = true; scan->block = physical; scan->slot = j;
            }
        }
        int release = ext4_block_set(fs->bdev, &block);
        if (r == EOK) r = release;
        if (r != EOK) break;
    }
Done:
    { int release = ext4_fs_put_inode_ref(&file); return r != EOK ? r : release; }
}

static int scan_all(struct ext4_fs *fs, struct orphan_scan *scan,
                    struct ext4_inode_ref *caller)
{
    uint32_t ino = ext4_get32(&fs->sb, last_orphan), previous = 0;
    int r = EOK;
    scan->first = ino;
    while (ino) {
        r = seen_insert(&scan->seen, ino); /* Detect every chain cycle too. */
        if (r != EOK) break;
        uint32_t next;
        if (caller && ino == caller->index) {
            r = check_allocated(fs, ino, false);
            if (r != EOK) break;
            next = ext4_inode_get_del_time(caller->inode);
        } else {
            struct ext4_inode_ref ref;
            r = load_inode(fs, ino, &ref, false);
            if (r != EOK) break;
            next = ext4_inode_get_del_time(ref.inode);
            r = ext4_fs_put_inode_ref(&ref);
            if (r != EOK) break;
        }
        if (ino == scan->target) {
            scan->found = true; scan->previous = previous; scan->next = next;
        }
        previous = ino;
        ino = next;
    }
    if (r == EOK) r = scan_file(fs, scan, caller);
    ext4_free(scan->seen.table);
    scan->seen.table = NULL;
    return r;
}

static int require_transaction(struct ext4_fs *fs)
{
    if (fs->read_only) return EROFS;
    if (!fs->jbd_journal || !fs->curr_trans) return EINVAL;
    return fs->jbd_journal->error;
}

int ext4_orphan_validate(struct ext4_fs *fs)
{
    struct orphan_scan scan = {0};
    return scan_all(fs, &scan, NULL);
}

int ext4_orphan_peek(struct ext4_fs *fs, uint32_t *inode)
{
    if (!inode) return EINVAL;
    struct orphan_scan scan = {0};
    int r = scan_all(fs, &scan, NULL);
    if (r == EOK) *inode = scan.first;
    return r;
}

int ext4_orphan_contains(struct ext4_fs *fs, uint32_t inode, bool *present)
{
    if (!fs || !inode || !present) return EINVAL;
    struct orphan_scan scan = { .target = inode };
    int r = scan_all(fs, &scan, NULL);
    if (r == EOK) *present = scan.found;
    return r;
}

static int update_slot(struct ext4_fs *fs, ext4_fsblk_t lba,
                       uint32_t slot, uint32_t seed, uint32_t value)
{
    struct ext4_block block;
    int r = ext4_block_get(fs->bdev, &block, lba);
    if (r != EOK) return r;
    r = check_block(fs, &block, seed);
    if (r == EOK) r = ext4_trans_set_block_dirty(block.buf);
    if (r == EOK) {
        ((uint32_t *)block.data)[slot] = to_le32(value);
        uint32_t size = fs->bdev->lg_bsize;
        struct orphan_tail *tail = (void *)(block.data + size - sizeof(*tail));
        if (ext4_sb_feature_ro_com(&fs->sb, EXT4_FRO_COM_METADATA_CSUM))
            tail->checksum = to_le32(block_checksum(seed, lba, block.data, size));
    }
    int release = ext4_block_set(fs->bdev, &block);
    return r != EOK ? r : release;
}

int ext4_orphan_add(struct ext4_inode_ref *inode)
{
    struct ext4_fs *fs = inode->fs;
    int r = require_transaction(fs);
    if (r != EOK) return r;
    r = check_allocated(fs, inode->index, false);
    if (r != EOK) return r;
    if (!valid_orphan_type(fs, inode->inode)) return EINVAL;
    struct orphan_scan scan = { .target = inode->index };
    r = scan_all(fs, &scan, inode);
    if (r != EOK || scan.found) return r;
    if (scan.free_block) {
        r = update_slot(fs, scan.free_block, scan.free_slot, scan.seed, inode->index);
        if (r == EOK) ext4_set32(&fs->sb, features_read_only,
            ext4_get32(&fs->sb, features_read_only) | EXT4_ORPHAN_PRESENT_RO_COMPAT);
        return r;
    }
    /* Linux uses the traditional list when the preallocated orphan file fills. */
    r = ext4_trans_set_block_dirty(inode->block.buf);
    if (r != EOK) return r;
    ext4_inode_set_del_time(inode->inode, ext4_get32(&fs->sb, last_orphan));
    ext4_set32(&fs->sb, last_orphan, inode->index);
    finish_inode(inode);
    if (ext4_sb_feature_com(&fs->sb, EXT4_ORPHAN_FILE_COMPAT))
        ext4_set32(&fs->sb, features_read_only,
            ext4_get32(&fs->sb, features_read_only) | EXT4_ORPHAN_PRESENT_RO_COMPAT);
    return EOK;
}

int ext4_orphan_remove(struct ext4_inode_ref *inode)
{
    struct ext4_fs *fs = inode->fs;
    int r = require_transaction(fs);
    if (r != EOK) return r;
    struct orphan_scan scan = { .target = inode->index };
    r = scan_all(fs, &scan, inode);
    if (r != EOK || !scan.found) return r;
    if (scan.block) return update_slot(fs, scan.block, scan.slot, scan.seed, 0);
    struct ext4_inode_ref previous;
    if (scan.previous) {
        r = load_inode(fs, scan.previous, &previous, false);
        if (r != EOK) return r;
        r = ext4_trans_set_block_dirty(previous.block.buf);
        if (r != EOK) { ext4_fs_put_inode_ref(&previous); return r; }
    }
    r = ext4_trans_set_block_dirty(inode->block.buf);
    if (r == EOK) {
        if (scan.previous) {
            ext4_inode_set_del_time(previous.inode, scan.next);
            finish_inode(&previous);
        } else ext4_set32(&fs->sb, last_orphan, scan.next);
        ext4_inode_set_del_time(inode->inode, 0);
        finish_inode(inode);
    }
    if (scan.previous) {
        int release = ext4_fs_put_inode_ref(&previous);
        if (r == EOK) r = release;
    }
    return r;
}

int ext4_orphan_set_present(struct ext4_fs *fs, bool present)
{
    int r = require_transaction(fs);
    if (r != EOK) return r;
    if (!present) {
        uint32_t inode;
        r = ext4_orphan_peek(fs, &inode);
        if (r != EOK) return r;
        if (inode) return ENOTEMPTY;
    }
    if (ext4_sb_feature_com(&fs->sb, EXT4_ORPHAN_FILE_COMPAT)) {
        uint32_t features = ext4_get32(&fs->sb, features_read_only);
        ext4_set32(&fs->sb, features_read_only, present ?
            features | EXT4_ORPHAN_PRESENT_RO_COMPAT : features & ~EXT4_ORPHAN_PRESENT_RO_COMPAT);
    }
    return EOK;
}
