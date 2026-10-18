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
#define ORPHAN_NO_BLOCK UINT32_MAX

struct orphan_record {
    uint32_t inode, previous, next, file_block, slot;
    uint32_t list_previous, list_next;
    uint8_t state, truncate_pending;
};
struct orphan_file_block {
    ext4_fsblk_t lba;
    uint32_t free_count, free_previous, free_next;
};
struct ext4_orphan_index {
    struct orphan_record *table;
    size_t capacity, occupied, count, pending;
    uint32_t head, chain_head, seed, file_blocks, slots, words, free_head;
    struct orphan_file_block *blocks;
    uint64_t *free_slots;
};
struct ext4_orphan_undo {
    struct ext4_orphan_undo *next;
    struct ext4_orphan_index *index;
    struct orphan_record old;
    uint32_t inode;
    bool restore, state_only;
};

static struct orphan_record *index_find(struct ext4_orphan_index *index, uint32_t inode)
{
    if (!index->capacity) return NULL;
    size_t slot = (inode * 2654435761U) & (index->capacity - 1);
    while (index->table[slot].state) {
        if (index->table[slot].state == 1 && index->table[slot].inode == inode)
            return &index->table[slot];
        slot = (slot + 1) & (index->capacity - 1);
    }
    return NULL;
}

static void index_store(struct ext4_orphan_index *index, struct orphan_record record)
{
    size_t slot = (record.inode * 2654435761U) & (index->capacity - 1);
    while (index->table[slot].state == 1) slot = (slot + 1) & (index->capacity - 1);
    if (!index->table[slot].state) index->occupied++;
    record.state = 1;
    index->table[slot] = record;
    index->count++;
}

static int index_reserve(struct ext4_orphan_index *index)
{
    if (index->occupied + 1 < index->capacity / 2) return EOK;
    size_t capacity = index->capacity ? index->capacity : 32;
    if (index->count + 1 >= capacity / 2) {
        if (capacity > SIZE_MAX / 2) return ENOMEM;
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*index->table)) return ENOMEM;
    struct orphan_record *table = ext4_calloc(capacity, sizeof(*table));
    if (!table) return ENOMEM;
    struct orphan_record *old = index->table;
    size_t old_capacity = index->capacity;
    index->table = table;
    index->capacity = capacity;
    index->occupied = index->count = 0;
    for (size_t i = 0; i < old_capacity; i++)
        if (old[i].state == 1) index_store(index, old[i]);
    ext4_free(old);
    return EOK;
}

static int index_load_record(struct ext4_orphan_index *index, struct orphan_record record)
{
    if (index_find(index, record.inode)) return EUCLEAN;
    int r = index_reserve(index);
    if (r != EOK) return r;
    record.truncate_pending = 1; /* Restart recovery must inspect persisted EOF. */
    record.list_next = index->head;
    if (index->head) index_find(index, index->head)->list_previous = record.inode;
    index->head = record.inode;
    index_store(index, record);
    return EOK;
}

static void index_free(struct ext4_orphan_index *index)
{
    if (!index) return;
    ext4_free(index->free_slots);
    ext4_free(index->blocks);
    ext4_free(index->table);
    ext4_free(index);
}

void ext4_orphan_index_fini(struct ext4_fs *fs)
{
    if (fs->orphan_index && fs->orphan_index->pending) __builtin_trap();
    index_free(fs->orphan_index);
    fs->orphan_index = NULL;
}

static void free_block_link(struct ext4_orphan_index *index, uint32_t block)
{
    struct orphan_file_block *entry = &index->blocks[block];
    entry->free_previous = ORPHAN_NO_BLOCK;
    entry->free_next = index->free_head;
    if (index->free_head != ORPHAN_NO_BLOCK)
        index->blocks[index->free_head].free_previous = block;
    index->free_head = block;
}

static void free_slot_set(struct ext4_orphan_index *index, uint32_t block,
                          uint32_t slot, bool free_slot)
{
    struct orphan_file_block *entry = &index->blocks[block];
    uint64_t *word = &index->free_slots[(size_t)block * index->words + slot / 64];
    uint64_t mask = UINT64_C(1) << (slot % 64);
    if (((*word & mask) != 0) == free_slot) __builtin_trap();
    if (free_slot) {
        *word |= mask;
        if (!entry->free_count++) free_block_link(index, block);
    } else {
        *word &= ~mask;
        if (!entry->free_count) __builtin_trap();
        if (!--entry->free_count) {
            if (entry->free_previous != ORPHAN_NO_BLOCK)
                index->blocks[entry->free_previous].free_next = entry->free_next;
            else index->free_head = entry->free_next;
            if (entry->free_next != ORPHAN_NO_BLOCK)
                index->blocks[entry->free_next].free_previous = entry->free_previous;
        }
    }
}

/* Rollback uses the same location edits in reverse; hash capacity and the
 * free-slot bitmap were reserved before touching journaled metadata. */
static void index_insert(struct ext4_orphan_index *index, struct orphan_record record)
{
    if (record.list_previous)
        index_find(index, record.list_previous)->list_next = record.inode;
    else index->head = record.inode;
    if (record.list_next)
        index_find(index, record.list_next)->list_previous = record.inode;
    if (record.file_block != ORPHAN_NO_BLOCK)
        free_slot_set(index, record.file_block, record.slot, false);
    else {
        if (record.previous) index_find(index, record.previous)->next = record.inode;
        else index->chain_head = record.inode;
        if (record.next) index_find(index, record.next)->previous = record.inode;
    }
    index_store(index, record);
}

static void index_remove(struct ext4_orphan_index *index, uint32_t inode)
{
    struct orphan_record *record = index_find(index, inode);
    if (!record) __builtin_trap();
    if (record->list_previous)
        index_find(index, record->list_previous)->list_next = record->list_next;
    else index->head = record->list_next;
    if (record->list_next)
        index_find(index, record->list_next)->list_previous = record->list_previous;
    if (record->file_block != ORPHAN_NO_BLOCK)
        free_slot_set(index, record->file_block, record->slot, true);
    else {
        if (record->previous) index_find(index, record->previous)->next = record->next;
        else index->chain_head = record->next;
        if (record->next) index_find(index, record->next)->previous = record->previous;
    }
    record->state = 2;
    index->count--;
}

void ext4_orphan_trans_finish(struct jbd_trans *trans, bool abort)
{
    struct ext4_orphan_undo *undo;
    while ((undo = trans->orphan_undo)) {
        trans->orphan_undo = undo->next;
        if (abort) {
            if (undo->state_only) {
                struct orphan_record *record = index_find(undo->index, undo->inode);
                if (!record) __builtin_trap();
                record->truncate_pending = undo->old.truncate_pending;
            } else if (undo->restore) index_insert(undo->index, undo->old);
            else index_remove(undo->index, undo->inode);
        }
        if (!undo->index->pending) __builtin_trap();
        undo->index->pending--;
        ext4_free(undo);
    }
}

static struct ext4_orphan_undo *reserve_undo(struct ext4_fs *fs,
                                           struct orphan_record *old, uint32_t inode)
{
    struct ext4_orphan_undo *undo = ext4_malloc(sizeof(*undo));
    if (!undo) return NULL;
    *undo = (struct ext4_orphan_undo){.index = fs->orphan_index,
                                    .inode = inode, .restore = old != NULL};
    if (old) undo->old = *old;
    return undo;
}

static void record_undo(struct ext4_fs *fs, struct ext4_orphan_undo *undo)
{
    undo->next = fs->curr_trans->orphan_undo;
    fs->curr_trans->orphan_undo = undo;
    undo->index->pending++;
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
    uint16_t size = ext4_get16(&ref->fs->sb, inode_size);
    uint32_t sum = ext4_crc32c_zeroed(inode_seed(ref), ref->inode, size,
        offsetof(struct ext4_inode, osd2.linux2.checksum_lo),
        size > EXT4_GOOD_OLD_INODE_SIZE ? offsetof(struct ext4_inode, checksum_hi) : size);
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

static int scan_file(struct ext4_fs *fs, struct ext4_orphan_index *index,
                     struct ext4_inode_ref *caller)
{
    if (!ext4_sb_feature_com(&fs->sb, EXT4_ORPHAN_FILE_COMPAT))
        return ext4_sb_feature_ro_com(&fs->sb, EXT4_ORPHAN_PRESENT_RO_COMPAT) ? EUCLEAN : EOK;
    struct ext4_inode_ref file;
    int r = load_inode(fs, ext4_get32(&fs->sb, orphan_file_inum), &file, true);
    if (r != EOK) return r;
    uint32_t size = fs->bdev->lg_bsize;
    uint64_t length = ext4_inode_get_size(&fs->sb, file.inode);
    uint64_t blocks = length / size;
    if ((ext4_inode_get_mode(&fs->sb, file.inode) & EXT4_INODE_MODE_TYPE_MASK) !=
        EXT4_INODE_MODE_FILE || length % size ||
        blocks > fs->bdev->lg_bcnt || blocks >= ORPHAN_NO_BLOCK) { r = EUCLEAN; goto Done; }
    index->seed = inode_seed(&file);
    index->file_blocks = (uint32_t)blocks;
    index->slots = (size - sizeof(struct orphan_tail)) / 4;
    index->words = (index->slots + 63) / 64;
    if (blocks > SIZE_MAX / sizeof(*index->blocks) ||
        blocks > SIZE_MAX / sizeof(*index->free_slots) / index->words) { r = ENOMEM; goto Done; }
    if (blocks) {
        index->blocks = ext4_calloc((size_t)blocks, sizeof(*index->blocks));
        index->free_slots = ext4_calloc((size_t)blocks * index->words, sizeof(*index->free_slots));
        if (!index->blocks || !index->free_slots) { r = ENOMEM; goto Done; }
    }
    for (uint32_t i = 0; i < blocks; i++) {
        ext4_fsblk_t physical;
        r = ext4_fs_get_inode_dblk_idx(&file, i, &physical, false);
        if (r == EINVAL) r = EUCLEAN;
        if (r != EOK) break;
        if (!physical || physical >= fs->bdev->lg_bcnt) { r = EUCLEAN; break; }
        index->blocks[i].lba = physical;
        index->blocks[i].free_previous = index->blocks[i].free_next = ORPHAN_NO_BLOCK;
        struct ext4_block block;
        r = ext4_block_get(fs->bdev, &block, physical);
        if (r != EOK) break;
        r = check_block(fs, &block, index->seed);
        uint32_t *entries = (void *)block.data;
        for (uint32_t j = 0; r == EOK && j < index->slots; j++) {
            uint32_t ino = to_le32(entries[j]);
            if (!ino) {
                index->free_slots[(size_t)i * index->words + j / 64] |= UINT64_C(1) << (j % 64);
                index->blocks[i].free_count++;
                continue;
            }
            struct orphan_record record = {.inode = ino, .file_block = i, .slot = j};
            r = index_load_record(index, record);
            if (r != EOK) break;
            if (caller && ino == caller->index) r = check_allocated(fs, ino, false);
            else {
                struct ext4_inode_ref ref;
                r = load_inode(fs, ino, &ref, false);
                if (r == EOK) r = ext4_fs_put_inode_ref(&ref);
            }
        }
        int release = ext4_block_set(fs->bdev, &block);
        if (r == EOK) r = release;
        if (r != EOK) break;
    }
    if (r == EOK) for (uint32_t i = index->file_blocks; i; i--)
        if (index->blocks[i - 1].free_count) free_block_link(index, i - 1);
Done:
    { int release = ext4_fs_put_inode_ref(&file); return r != EOK ? r : release; }
}

static int load_index(struct ext4_fs *fs, struct ext4_inode_ref *caller)
{
    struct ext4_orphan_index *index = ext4_calloc(1, sizeof(*index));
    if (!index) return ENOMEM;
    index->free_head = ORPHAN_NO_BLOCK;
    uint32_t ino = ext4_get32(&fs->sb, last_orphan), previous = 0;
    index->chain_head = ino;
    int r = EOK;
    while (ino) {
        struct orphan_record record = {.inode = ino, .previous = previous,
                                       .file_block = ORPHAN_NO_BLOCK};
        /* Insert before following next so duplicate records also detect cycles. */
        r = index_load_record(index, record);
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
        index_find(index, ino)->next = next;
        previous = ino;
        ino = next;
    }
    if (r == EOK) r = scan_file(fs, index, caller);
    if (r != EOK) { index_free(index); return r; }
    fs->orphan_index = index;
    return EOK;
}

static int ensure_index(struct ext4_fs *fs, struct ext4_inode_ref *caller)
{
    return fs->orphan_index ? EOK : load_index(fs, caller);
}

static int require_transaction(struct ext4_fs *fs)
{
    if (fs->read_only) return EROFS;
    if (!fs->jbd_journal || !fs->curr_trans) return EINVAL;
    return fs->jbd_journal->error;
}

int ext4_orphan_validate(struct ext4_fs *fs)
{
    if (!fs) return EINVAL;
    if (fs->jbd_journal && fs->jbd_journal->error) return fs->jbd_journal->error;
    if (fs->orphan_index && fs->orphan_index->pending) return EBUSY;
    ext4_orphan_index_fini(fs);
    return load_index(fs, NULL);
}

int ext4_orphan_peek(struct ext4_fs *fs, uint32_t *inode)
{
    if (!fs || !inode) return EINVAL;
    int r = ensure_index(fs, NULL);
    if (r == EOK) *inode = fs->orphan_index->chain_head ?
        fs->orphan_index->chain_head : fs->orphan_index->head;
    return r;
}

int ext4_orphan_contains(struct ext4_fs *fs, uint32_t inode, bool *present)
{
    if (!fs || !inode || !present) return EINVAL;
    int r = ensure_index(fs, NULL);
    if (r == EOK) *present = index_find(fs->orphan_index, inode) != NULL;
    return r;
}

int ext4_orphan_truncate_pending(struct ext4_fs *fs, uint32_t inode, bool *pending)
{
    if (!fs || !inode || !pending) return EINVAL;
    int r = ensure_index(fs, NULL);
    if (r != EOK) return r;
    struct orphan_record *record = index_find(fs->orphan_index, inode);
    *pending = record && record->truncate_pending;
    return EOK;
}

int ext4_orphan_mark_truncate(struct ext4_inode_ref *inode, bool pending)
{
    struct ext4_fs *fs = inode->fs;
    int r = require_transaction(fs);
    if (r != EOK) return r;
    r = ensure_index(fs, inode);
    if (r != EOK) return r;
    struct orphan_record *record = index_find(fs->orphan_index, inode->index);
    if (!record) return ENOENT;
    if (record->truncate_pending == pending) return EOK;
    struct ext4_orphan_undo *undo = reserve_undo(fs, record, inode->index);
    if (!undo) return ENOMEM;
    undo->state_only = true;
    record->truncate_pending = pending;
    record_undo(fs,undo);
    return EOK;
}

static int update_slot(struct ext4_fs *fs, struct orphan_record *record,
                       uint32_t expected, uint32_t value, bool *changed)
{
    struct ext4_orphan_index *index = fs->orphan_index;
    ext4_fsblk_t lba = index->blocks[record->file_block].lba;
    struct ext4_block block;
    /* Reserve undo/version ownership and validate the affected on-disk slot. */
    int r = ext4_trans_block_get(fs->bdev, &block, lba);
    if (r != EOK) return r;
    r = check_block(fs, &block, index->seed);
    if (r == EOK && to_le32(((uint32_t *)block.data)[record->slot]) != expected) r = EUCLEAN;
    if (r == EOK) r = ext4_trans_set_block_dirty(block.buf);
    if (r == EOK) {
        ((uint32_t *)block.data)[record->slot] = to_le32(value);
        uint32_t size = fs->bdev->lg_bsize;
        struct orphan_tail *tail = (void *)(block.data + size - sizeof(*tail));
        if (ext4_sb_feature_ro_com(&fs->sb, EXT4_FRO_COM_METADATA_CSUM))
            tail->checksum = to_le32(block_checksum(index->seed, lba, block.data, size));
        *changed = true;
    }
    int release = ext4_block_set(fs->bdev, &block);
    return r != EOK ? r : release;
}

int ext4_orphan_add(struct ext4_inode_ref *inode)
{
    struct ext4_fs *fs = inode->fs;
    int r = require_transaction(fs);
    if (r != EOK) return r;
    r = ensure_index(fs, inode);
    if (r != EOK) return r;
    struct ext4_orphan_index *index = fs->orphan_index;
    r = check_allocated(fs, inode->index, false);
    if (r != EOK) return r;
    if (!valid_orphan_type(fs, inode->inode)) return EINVAL;
    if (index_find(index, inode->index)) return EOK;
    if (ext4_get32(&fs->sb, last_orphan) != index->chain_head) return EUCLEAN;
    r = index_reserve(index);
    if (r != EOK) return r;
    struct ext4_orphan_undo *undo = reserve_undo(fs, NULL, inode->index);
    if (!undo) return ENOMEM;
    struct orphan_record record = {.inode = inode->index, .file_block = index->free_head,
                                   .list_next = index->head,
                                   .truncate_pending = ext4_inode_get_links_cnt(inode->inode) != 0};
    bool changed = false;
    if (record.file_block != ORPHAN_NO_BLOCK) {
        uint64_t *bits = &index->free_slots[(size_t)record.file_block * index->words];
        uint32_t word = 0;
        while (word < index->words && !bits[word]) word++;
        if (word == index->words) __builtin_trap();
        uint32_t bit = 0;
        while (!(bits[word] & (UINT64_C(1) << bit))) bit++;
        record.slot = word * 64 + bit;
        r = update_slot(fs, &record, 0, inode->index, &changed);
    } else {
        /* Linux falls back to the legacy chain once the orphan file is full. */
        record.next = index->chain_head;
        r = jbd_trans_get_write_access(fs->curr_trans, &inode->block);
        if (r == EOK) r = ext4_trans_set_block_dirty(inode->block.buf);
        if (r == EOK) {
            ext4_inode_set_del_time(inode->inode, record.next);
            ext4_set32(&fs->sb, last_orphan, inode->index);
            finish_inode(inode);
            changed = true;
        }
    }
    if (changed) {
        index_insert(index, record);
        record_undo(fs, undo);
        if (ext4_sb_feature_com(&fs->sb, EXT4_ORPHAN_FILE_COMPAT))
            ext4_set32(&fs->sb, features_read_only,
                ext4_get32(&fs->sb, features_read_only) | EXT4_ORPHAN_PRESENT_RO_COMPAT);
    } else ext4_free(undo);
    return r;
}

int ext4_orphan_remove(struct ext4_inode_ref *inode)
{
    struct ext4_fs *fs = inode->fs;
    int r = require_transaction(fs);
    if (r != EOK) return r;
    r = ensure_index(fs, inode);
    if (r != EOK) return r;
    struct ext4_orphan_index *index = fs->orphan_index;
    struct orphan_record *found = index_find(index, inode->index);
    if (!found) return index->count ? EOK : ext4_orphan_set_present(fs, false);
    struct orphan_record record = *found;
    r = check_allocated(fs, inode->index, false);
    if (r != EOK) return r;
    if (!valid_orphan_type(fs, inode->inode) ||
        ext4_get32(&fs->sb, last_orphan) != index->chain_head) return EUCLEAN;
    struct ext4_orphan_undo *undo = reserve_undo(fs, found, inode->index);
    if (!undo) return ENOMEM;
    bool changed = false;
    if (record.file_block != ORPHAN_NO_BLOCK) {
        r = update_slot(fs, &record, inode->index, 0, &changed);
    } else {
        struct ext4_inode_ref previous;
        bool held = false;
        if (ext4_inode_get_del_time(inode->inode) != record.next) { r = EUCLEAN; goto Finish; }
        if (record.previous) {
            r = load_inode(fs, record.previous, &previous, false);
            if (r != EOK) goto Finish;
            held = true;
            if (ext4_inode_get_del_time(previous.inode) != inode->index) r = EUCLEAN;
            if (r == EOK) r = ext4_trans_set_block_dirty(previous.block.buf);
        }
        if (r == EOK) r = jbd_trans_get_write_access(fs->curr_trans, &inode->block);
        if (r == EOK) r = ext4_trans_set_block_dirty(inode->block.buf);
        if (r == EOK) {
            if (record.previous) {
                ext4_inode_set_del_time(previous.inode, record.next);
                finish_inode(&previous);
            } else ext4_set32(&fs->sb, last_orphan, record.next);
            ext4_inode_set_del_time(inode->inode, 0);
            finish_inode(inode);
            changed = true;
        }
        if (held) {
            int release = ext4_fs_put_inode_ref(&previous);
            if (r == EOK) r = release;
        }
    }
Finish:
    if (changed) {
        index_remove(index, inode->index);
        record_undo(fs, undo);
        if (!index->count && ext4_sb_feature_com(&fs->sb, EXT4_ORPHAN_FILE_COMPAT))
            ext4_set32(&fs->sb, features_read_only,
                ext4_get32(&fs->sb, features_read_only) & ~EXT4_ORPHAN_PRESENT_RO_COMPAT);
    } else ext4_free(undo);
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
