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
};
struct ext4_orphan_index {
    struct orphan_record *table;
    size_t capacity, occupied, count, pending;
    uint32_t head, chain_head, seed, file_blocks, slots, words;
    struct orphan_file_block *blocks;
    uint64_t *free_slots;
};
struct ext4_orphan_undo {
    struct ext4_orphan_undo *next;
    struct ext4_orphan_index *index;
    struct orphan_record before, after;
    bool before_present, after_present;
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

static void free_slot_set(struct ext4_orphan_index *index, uint32_t block,
                          uint32_t slot, bool free_slot)
{
    uint64_t *word = &index->free_slots[(size_t)block * index->words + slot / 64];
    uint64_t mask = UINT64_C(1) << (slot % 64);
    if (((*word & mask) != 0) == free_slot) __builtin_trap();
    if (free_slot) *word |= mask;
    else *word &= ~mask;
}

/* Only accepted records are linked into this index. Private operations retain
 * desired records, including slot and chain edits, in their own delta list. */
static void index_insert(struct ext4_orphan_index *index, struct orphan_record record)
{
    record.list_previous = 0;
    record.list_next = index->head;
    if (index->head) index_find(index, index->head)->list_previous = record.inode;
    index->head = record.inode;
    if (record.file_block != ORPHAN_NO_BLOCK)
        free_slot_set(index, record.file_block, record.slot, false);
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
    record->state = 2;
    index->count--;
}

static struct ext4_orphan_undo *operation_change(struct jbd_trans *trans, uint32_t inode)
{
    if (trans) for (struct ext4_orphan_undo *change = trans->orphan_undo;
                   change; change = change->next)
        if (change->before.inode == inode) return change;
    return NULL;
}

static bool operation_record(struct ext4_fs *fs, uint32_t inode, struct orphan_record *record)
{
    struct ext4_orphan_undo *change = operation_change(ext4_current_trans(fs), inode);
    if (change) {
        if (change->after_present) *record = change->after;
        return change->after_present;
    }
    ext4_fs_state_read_lock(fs);
    struct orphan_record *accepted = index_find(fs->orphan_index, inode);
    if (accepted) *record = *accepted;
    ext4_fs_state_unlock(fs);
    return accepted != NULL;
}

/* Account for all in-flight inserts before private metadata changes. A hash
 * replacement is allocated outside the state lock and published after recheck. */
static int index_reserve_pending(struct ext4_fs *fs, struct ext4_orphan_index *index)
{
    struct orphan_record *candidate = NULL;
    size_t candidate_capacity = 0;
    for (;;) {
        ext4_fs_state_lock(fs);
        if (index->pending == SIZE_MAX || index->occupied > SIZE_MAX - index->pending - 1) __builtin_trap();
        size_t needed = index->occupied + index->pending + 1;
        if (needed < index->capacity / 2) {
            index->pending++;
            ext4_fs_state_unlock(fs);
            ext4_free(candidate);
            return EOK;
        }
        size_t capacity = index->capacity ? index->capacity : 32;
        while (index->count + index->pending + 1 >= capacity / 2) {
            if (capacity > SIZE_MAX / 2 / sizeof(*candidate)) {
                ext4_fs_state_unlock(fs); ext4_free(candidate); return ENOMEM;
            }
            capacity *= 2;
        }
        if (candidate && candidate_capacity >= capacity) {
            struct orphan_record *previous = index->table;
            size_t previous_capacity = index->capacity;
            index->table = candidate; index->capacity = candidate_capacity;
            index->occupied = index->count = 0;
            for (size_t i = 0; i < previous_capacity; i++)
                if (previous[i].state == 1) index_store(index, previous[i]);
            index->pending++;
            ext4_fs_state_unlock(fs);
            ext4_free(previous);
            return EOK;
        }
        ext4_fs_state_unlock(fs);
        ext4_free(candidate);
        candidate = ext4_calloc(capacity, sizeof(*candidate));
        if (!candidate) return ENOMEM;
        candidate_capacity = capacity;
    }
}

static struct ext4_orphan_undo *reserve_change(struct ext4_fs *fs, uint32_t inode)
{
    struct jbd_trans *trans = ext4_current_trans(fs);
    struct ext4_orphan_undo *change = operation_change(trans, inode);
    if (change) return change;
    change = jbd_journal_alloc(fs->jbd_journal, sizeof(*change));
    if (!change) return NULL;
    if (index_reserve_pending(fs, fs->orphan_index) != EOK) {
        jbd_journal_free(change); return NULL;
    }
    *change = (struct ext4_orphan_undo){.index = fs->orphan_index,
        .before.inode = inode, .after.inode = inode, .next = trans->orphan_undo};
    ext4_fs_state_read_lock(fs);
    struct orphan_record *accepted = index_find(fs->orphan_index, inode);
    if (accepted) {
        change->before = change->after = *accepted;
        change->before_present = change->after_present = true;
    }
    ext4_fs_state_unlock(fs);
    trans->orphan_undo = change;
    return change;
}

bool ext4_orphan_trans_membership_changed(struct jbd_trans *trans)
{
    if (trans) for (struct ext4_orphan_undo *change = trans->orphan_undo;
                   change; change = change->next)
        if (change->before_present != change->after_present) return true;
    return false;
}

int ext4_orphan_trans_count(struct jbd_trans *trans, size_t *count)
{
    struct ext4_fs *fs = trans ? trans->operation.fs : NULL;
    if (!fs || !fs->orphan_index) { *count = 0; return EOK; }
    ext4_fs_state_read_lock(fs);
    size_t resulting = fs->orphan_index->count;
    for (struct ext4_orphan_undo *change = trans->orphan_undo; change; change = change->next) {
        if (change->before_present && !change->after_present) {
            if (!resulting) { ext4_fs_state_unlock(fs); return EUCLEAN; }
            resulting--;
        } else if (!change->before_present && change->after_present) {
            if (resulting == SIZE_MAX) { ext4_fs_state_unlock(fs); return EOVERFLOW; }
            resulting++;
        }
    }
    ext4_fs_state_unlock(fs);
    *count = resulting;
    return EOK;
}

void ext4_orphan_trans_finish_state_held(struct jbd_trans *trans, bool abort)
{
    struct ext4_orphan_undo *change = trans->orphan_undo;
    if (!change) return;
    struct ext4_fs *fs = trans->operation.fs;
    /* One operation may give A's slot to B. Release all previous positions
     * before installing the final owners; the private list is newest first. */
    if (!abort) for (struct ext4_orphan_undo *entry = change; entry; entry = entry->next) {
        struct orphan_record *accepted = index_find(entry->index, entry->before.inode);
        if ((accepted != NULL) != entry->before_present) __builtin_trap();
        if (accepted && (!entry->after_present ||
            accepted->file_block != entry->after.file_block || accepted->slot != entry->after.slot))
            index_remove(entry->index, entry->before.inode);
    }
    if (!abort) for (struct ext4_orphan_undo *entry = change; entry; entry = entry->next) {
        if (!entry->after_present) continue;
        struct orphan_record *accepted = index_find(entry->index, entry->before.inode);
        if (!accepted) index_insert(entry->index, entry->after);
        else {
            uint32_t previous = accepted->list_previous, next = accepted->list_next;
            *accepted = entry->after;
            accepted->list_previous = previous; accepted->list_next = next;
        }
    }
    if (change && !abort && fs && fs->orphan_index && trans->operation.orphan_chain)
        fs->orphan_index->chain_head = trans->operation.orphan_head;
    for (struct ext4_orphan_undo *entry = change; entry; entry = entry->next) {
        if (!entry->index->pending) __builtin_trap();
        entry->index->pending--;
    }
    trans->orphan_undo = NULL;
    while (change) {
        struct ext4_orphan_undo *next = change->next;
        jbd_journal_free(change);
        change = next;
    }
}

static uint32_t inode_seed(struct ext4_inode_ref *ref)
{
    uint32_t ino = to_le32(ref->index);
    uint32_t gen = to_le32(ext4_inode_get_generation(ref->inode));
    uint32_t sum = ext4_sb_get_csum_seed(ext4_fs_super(ref->fs));
    sum = ext4_crc32c(sum, &ino, sizeof(ino));
    return ext4_crc32c(sum, &gen, sizeof(gen));
}

static uint32_t inode_checksum(struct ext4_inode_ref *ref)
{
    uint16_t size = ext4_get16(ext4_fs_super(ref->fs), inode_size);
    uint32_t sum = ext4_crc32c_zeroed(inode_seed(ref), ref->inode, size,
        offsetof(struct ext4_inode, osd2.linux2.checksum_lo),
        size > EXT4_GOOD_OLD_INODE_SIZE ? offsetof(struct ext4_inode, checksum_hi) : size);
    return size == EXT4_GOOD_OLD_INODE_SIZE ? sum & 0xffff : sum;
}

static void finish_inode(struct ext4_inode_ref *ref)
{
    if (ext4_sb_feature_ro_com(ext4_fs_super(ref->fs), EXT4_FRO_COM_METADATA_CSUM))
        ext4_inode_set_csum(ext4_fs_super(ref->fs), ref->inode, inode_checksum(ref));
    ref->dirty = true;
}

static int check_allocated(struct ext4_fs *fs, uint32_t ino, bool special)
{
    uint32_t first = special ? 1 : ext4_get32(ext4_fs_super(fs), first_inode);
    uint32_t per_group = ext4_get32(ext4_fs_super(fs), inodes_per_group);
    uint32_t inode_size = ext4_get16(ext4_fs_super(fs), inode_size);
    uint32_t descriptor_size = ext4_sb_get_desc_size(ext4_fs_super(fs));
    uint32_t block_size = fs->bdev->lg_bsize;
    if (inode_size < EXT4_GOOD_OLD_INODE_SIZE || inode_size > block_size ||
        block_size % inode_size || descriptor_size < EXT4_MIN_BLOCK_GROUP_DESCRIPTOR_SIZE ||
        descriptor_size > block_size || block_size % descriptor_size) return EUCLEAN;
    if (ino < first || ino > ext4_get32(ext4_fs_super(fs), inodes_count) || !per_group ||
        per_group > (uint64_t)fs->bdev->lg_bsize * 8 ||
        (!special && (ino == ext4_get32(ext4_fs_super(fs), orphan_file_inum) ||
                      ino == ext4_get32(ext4_fs_super(fs), journal_inode_number)))) return EUCLEAN;
    /* The bitmap checksum and its descriptor must describe one accepted
     * allocation state. An unrelated inode in this group can otherwise be
     * accepted between the two reads and manufacture a corruption error. */
    /* Mount/recovery validation runs after its external users are drained,
     * without a modifying operation. Only a live operation can race another
     * allocator and owns the qualification until acceptance or abort. */
    int r = ext4_current_operation(fs) ?
        ext4_operation_require_resource(fs, EXT4_RESOURCE_BLOCK_GROUP,
                                        (ino - 1) / per_group) : EOK;
    if (r) return r;
    struct ext4_block_group_ref group;
    r = ext4_fs_get_block_group_ref(fs, (ino - 1) / per_group, &group);
    if (r != EOK) return r;
    if (ext4_bg_has_flag(group.block_group, EXT4_BLOCK_GROUP_INODE_UNINIT)) {
        ext4_fs_put_block_group_ref(&group);
        return EUCLEAN;
    }
    ext4_fsblk_t lba = ext4_bg_get_inode_bitmap(group.block_group, ext4_fs_super(fs));
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
    bool allocated = ext4_ialloc_verify_bitmap_csum(ext4_fs_super(fs),
                         group.block_group, bitmap.data) &&
                     ext4_bmap_is_bit_set(bitmap.data, (ino - 1) % per_group);
    r = ext4_block_set(fs->bdev, &bitmap);
    int release = ext4_fs_put_block_group_ref(&group);
    if (r == EOK) r = release;
    return r != EOK ? r : allocated ? EOK : EUCLEAN;
}

static bool valid_orphan_type(struct ext4_fs *fs, struct ext4_inode *inode)
{
    uint32_t type = ext4_inode_get_mode(ext4_fs_super(fs), inode) & EXT4_INODE_MODE_TYPE_MASK;
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
    if (ext4_sb_feature_ro_com(ext4_fs_super(fs), EXT4_FRO_COM_METADATA_CSUM) &&
        ext4_inode_get_csum(ext4_fs_super(fs), ref->inode) != inode_checksum(ref)) r = EIO;
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
    if (ext4_sb_feature_ro_com(ext4_fs_super(fs), EXT4_FRO_COM_METADATA_CSUM) &&
        to_le32(tail->checksum) != block_checksum(seed, block->lb_id, block->data, size))
        return EIO;
    return EOK;
}

static int scan_file(struct ext4_fs *fs, struct ext4_orphan_index *index,
                     struct ext4_inode_ref *caller)
{
    if (!ext4_sb_feature_com(ext4_fs_super(fs), EXT4_ORPHAN_FILE_COMPAT))
        return ext4_sb_feature_ro_com(ext4_fs_super(fs), EXT4_ORPHAN_PRESENT_RO_COMPAT) ? EUCLEAN : EOK;
    struct ext4_inode_ref file;
    int r = load_inode(fs, ext4_get32(ext4_fs_super(fs), orphan_file_inum), &file, true);
    if (r != EOK) return r;
    uint32_t size = fs->bdev->lg_bsize;
    uint64_t length = ext4_inode_get_size(ext4_fs_super(fs), file.inode);
    uint64_t blocks = length / size;
    if ((ext4_inode_get_mode(ext4_fs_super(fs), file.inode) & EXT4_INODE_MODE_TYPE_MASK) !=
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
        struct ext4_block block;
        r = ext4_block_get(fs->bdev, &block, physical);
        if (r != EOK) break;
        r = check_block(fs, &block, index->seed);
        uint32_t *entries = (void *)block.data;
        for (uint32_t j = 0; r == EOK && j < index->slots; j++) {
            uint32_t ino = to_le32(entries[j]);
            if (!ino) {
                index->free_slots[(size_t)i * index->words + j / 64] |= UINT64_C(1) << (j % 64);
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
Done:
    { int release = ext4_fs_put_inode_ref(&file); return r != EOK ? r : release; }
}

static int load_index(struct ext4_fs *fs, struct ext4_inode_ref *caller)
{
    struct ext4_orphan_index *index = ext4_calloc(1, sizeof(*index));
    if (!index) return ENOMEM;
    uint32_t ino = ext4_fs_orphan_head(fs), previous = 0;
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
    if (!fs->jbd_journal || !ext4_current_trans(fs)) return EINVAL;
    ext4_current_operation(fs)->orphan_metadata = true;
    return jbd_journal_error(fs->jbd_journal);
}

static int require_chain(struct ext4_fs *fs)
{
    int result = ext4_operation_require_resource(fs, EXT4_RESOURCE_ORPHAN, 0);
    if (result) return result;
    struct ext4_operation *operation = ext4_current_operation(fs);
    if (!operation->orphan_chain) {
        ext4_fs_state_lock(fs);
        operation->orphan_base = ext4_get32(&fs->sb, last_orphan);
        operation->orphan_head = operation->orphan_base;
        ext4_fs_state_unlock(fs);
        operation->orphan_chain = true;
    }
    return EOK;
}

static int require_record(struct ext4_fs *fs, const struct orphan_record *record)
{
    return record->file_block == ORPHAN_NO_BLOCK ? require_chain(fs) :
        ext4_operation_require_resource(fs, EXT4_RESOURCE_ORPHAN, record->file_block + 1);
}

int ext4_orphan_prepare_change(struct ext4_inode_ref *inode)
{
    struct ext4_fs *fs = inode->fs;
    int result = require_transaction(fs);
    if (result) return result;
    result = ensure_index(fs, inode);
    if (result) return result;
    result = ext4_fs_inode_ref_write(inode);
    if (result) return result;
    struct orphan_record record;
    return operation_record(fs, inode->index, &record) ? require_record(fs, &record) : EOK;
}

int ext4_orphan_validate(struct ext4_fs *fs)
{
    if (!fs) return EINVAL;
    if (fs->jbd_journal && fs->jbd_journal->error) return jbd_journal_error(fs->jbd_journal);
    if (fs->orphan_index && fs->orphan_index->pending) return EBUSY;
    ext4_orphan_index_fini(fs);
    return load_index(fs, NULL);
}

int ext4_orphan_peek(struct ext4_fs *fs, uint32_t *inode)
{
    if (!fs || !inode) return EINVAL;
    int result = ensure_index(fs, NULL);
    if (result) return result;
    struct jbd_trans *trans = ext4_current_trans(fs);
    struct ext4_operation *operation = ext4_current_operation(fs);
    ext4_fs_state_read_lock(fs);
    uint32_t head = operation && operation->orphan_chain ?
        operation->orphan_head : fs->orphan_index->chain_head;
    if (!head && trans) for (struct ext4_orphan_undo *change = trans->orphan_undo;
                            change; change = change->next)
        if (change->after_present && !change->before_present) { head = change->after.inode; break; }
    uint32_t accepted = fs->orphan_index->head;
    while (!head && accepted) {
        struct orphan_record *record = index_find(fs->orphan_index, accepted);
        if (!record) __builtin_trap();
        struct ext4_orphan_undo *change = operation_change(trans, accepted);
        if (!change || change->after_present) head = accepted;
        accepted = record->list_next;
    }
    ext4_fs_state_unlock(fs);
    *inode = head;
    return EOK;
}

int ext4_orphan_contains(struct ext4_fs *fs, uint32_t inode, bool *present)
{
    if (!fs || !inode || !present) return EINVAL;
    int result = ensure_index(fs, NULL);
    if (result) return result;
    struct orphan_record record;
    *present = operation_record(fs, inode, &record);
    return EOK;
}

int ext4_orphan_truncate_pending(struct ext4_fs *fs, uint32_t inode, bool *pending)
{
    if (!fs || !inode || !pending) return EINVAL;
    int result = ensure_index(fs, NULL);
    if (result) return result;
    struct orphan_record record;
    *pending = operation_record(fs, inode, &record) && record.truncate_pending;
    return EOK;
}

int ext4_orphan_mark_truncate(struct ext4_inode_ref *inode, bool pending)
{
    struct ext4_fs *fs = inode->fs;
    int result = ext4_orphan_prepare_change(inode);
    if (result) return result;
    struct orphan_record record;
    if (!operation_record(fs, inode->index, &record)) return ENOENT;
    if (record.truncate_pending == pending) return EOK;
    struct ext4_orphan_undo *change = reserve_change(fs, inode->index);
    if (!change) return ENOMEM;
    change->after.truncate_pending = pending;
    return EOK;
}

static int update_slot(struct ext4_fs *fs, struct orphan_record *record,
                       uint32_t expected, uint32_t value, bool *changed)
{
    struct ext4_orphan_index *index = fs->orphan_index;
    ext4_fsblk_t lba = index->blocks[record->file_block].lba;
    struct ext4_block block;
    int result = ext4_trans_block_get(fs->bdev, &block, lba);
    if (result) return result;
    result = check_block(fs, &block, index->seed);
    if (!result && to_le32(((uint32_t *)block.data)[record->slot]) != expected) result = EUCLEAN;
    if (!result) result = ext4_trans_set_block_dirty(block.buf);
    if (!result) {
        ((uint32_t *)block.data)[record->slot] = to_le32(value);
        uint32_t size = fs->bdev->lg_bsize;
        struct orphan_tail *tail = (void *)(block.data + size - sizeof(*tail));
        if (ext4_sb_feature_ro_com(ext4_fs_super(fs), EXT4_FRO_COM_METADATA_CSUM))
            tail->checksum = to_le32(block_checksum(index->seed, lba, block.data, size));
        *changed = true;
    }
    int release = ext4_block_set(fs->bdev, &block);
    return result ? result : release;
}

static uint64_t operation_free_word(struct ext4_fs *fs, uint32_t block, uint32_t word)
{
    struct ext4_orphan_index *index = fs->orphan_index;
    ext4_fs_state_read_lock(fs);
    uint64_t bits = index->free_slots[(size_t)block * index->words + word];
    ext4_fs_state_unlock(fs);
    struct jbd_trans *trans = ext4_current_trans(fs);
    for (struct ext4_orphan_undo *change = trans->orphan_undo; change; change = change->next) {
        if (change->before_present && change->before.file_block == block &&
            change->before.slot / 64 == word) bits |= UINT64_C(1) << (change->before.slot % 64);
    }
    /* Subtract the complete final owner set after returning old positions.
     * A later record can reuse an earlier record's accepted slot. */
    for (struct ext4_orphan_undo *change = trans->orphan_undo; change; change = change->next) {
        if (change->after_present && change->after.file_block == block &&
            change->after.slot / 64 == word) bits &= ~(UINT64_C(1) << (change->after.slot % 64));
    }
    return bits;
}

static int find_file_slot(struct ext4_fs *fs, uint32_t inode, struct orphan_record *record)
{
    struct ext4_orphan_index *index = fs->orphan_index;
    if (!index->file_blocks) { record->file_block = ORPHAN_NO_BLOCK; return EOK; }
    uint32_t hash = inode * 2654435761U;
    hash ^= hash >> 16;
    uint32_t first = hash % index->file_blocks;
    for (uint32_t step = 0; step < index->file_blocks; step++) {
        uint32_t block = (first + step) % index->file_blocks;
        bool available = false;
        for (uint32_t word = 0; word < index->words; word++)
            if (operation_free_word(fs, block, word)) { available = true; break; }
        if (!available) continue;
        int result = ext4_operation_require_resource(fs, EXT4_RESOURCE_ORPHAN, block + 1);
        if (result) return result;
        /* A waiter rechecks after the previous owner has actually accepted
         * or aborted. Its private slots are never published as free slots. */
        for (uint32_t word = 0; word < index->words; word++) {
            uint64_t bits = operation_free_word(fs, block, word);
            if (!bits) continue;
            unsigned bit = 0;
            while (!(bits & (UINT64_C(1) << bit))) bit++;
            record->file_block = block; record->slot = word * 64 + bit;
            return EOK;
        }
    }
    record->file_block = ORPHAN_NO_BLOCK;
    return EOK;
}

static int inode_mark_dirty(struct ext4_inode_ref *inode)
{
    return ext4_trans_set_block_dirty_range(&inode->block,
        (unsigned char *)inode->inode - inode->block.data,
        ext4_get16(&inode->fs->sb, inode_size));
}

int ext4_orphan_add(struct ext4_inode_ref *inode)
{
    struct ext4_fs *fs = inode->fs;
    int result = require_transaction(fs);
    if (result) return result;
    result = ensure_index(fs, inode);
    if (result) return result;
    result = ext4_fs_inode_ref_write(inode);
    if (result) return result;
    struct orphan_record record;
    bool present = operation_record(fs, inode->index, &record);
    if (present) result = require_record(fs, &record);
    else {
        record = (struct orphan_record){.inode = inode->index,
            .truncate_pending = ext4_inode_get_links_cnt(inode->inode) != 0};
        result = find_file_slot(fs, inode->index, &record);
        if (!result && record.file_block == ORPHAN_NO_BLOCK) result = require_chain(fs);
    }
    if (result) return result;
    /* 先选定记录的槽位或链资格，再验证同一组版本的描述符和位图。
     * 验证前没有修改记录，坏盘仍会使整个调用者操作失败并回滚。 */
    result = check_allocated(fs, inode->index, false);
    if (result) return result;
    if (!valid_orphan_type(fs, inode->inode)) return EINVAL;
    if (present) return EOK;
    struct ext4_orphan_undo *change = reserve_change(fs, inode->index);
    if (!change) return ENOMEM;
    bool changed = false;
    if (record.file_block != ORPHAN_NO_BLOCK) result = update_slot(fs, &record, 0, inode->index, &changed);
    else {
        record.next = ext4_fs_orphan_head(fs);
        if (record.next) {
            struct ext4_orphan_undo *next = reserve_change(fs, record.next);
            if (!next) return ENOMEM;
            if (!next->after_present || next->after.file_block != ORPHAN_NO_BLOCK) return EUCLEAN;
            next->after.previous = inode->index;
        }
        result = inode_mark_dirty(inode);
        if (!result) {
            ext4_inode_set_del_time(inode->inode, record.next);
            ext4_fs_set_orphan_head(fs, inode->index);
            finish_inode(inode); changed = true;
        }
    }
    if (changed) { change->after = record; change->after_present = true; }
    return result;
}

int ext4_orphan_remove(struct ext4_inode_ref *inode)
{
    struct ext4_fs *fs = inode->fs;
    int result = ext4_orphan_prepare_change(inode);
    if (result) return result;
    struct orphan_record record;
    if (!operation_record(fs, inode->index, &record)) return EOK;
    result = check_allocated(fs, inode->index, false);
    if (result) return result;
    if (!valid_orphan_type(fs, inode->inode)) return EUCLEAN;
    struct ext4_orphan_undo *change = reserve_change(fs, inode->index);
    if (!change) return ENOMEM;
    bool changed = false;
    if (record.file_block != ORPHAN_NO_BLOCK) result = update_slot(fs, &record, inode->index, 0, &changed);
    else {
        struct ext4_inode_ref previous;
        bool held = false;
        if (ext4_inode_get_del_time(inode->inode) != record.next) return EUCLEAN;
        if (record.previous) {
            struct ext4_orphan_undo *before = reserve_change(fs, record.previous);
            if (!before) return ENOMEM;
            if (!before->after_present || before->after.file_block != ORPHAN_NO_BLOCK) return EUCLEAN;
            before->after.next = record.next;
            result = load_inode(fs, record.previous, &previous, false);
            if (result) return result;
            held = true;
            if (ext4_inode_get_del_time(previous.inode) != inode->index) result = EUCLEAN;
            if (!result) result = inode_mark_dirty(&previous);
        }
        if (!result && record.next) {
            struct ext4_orphan_undo *next = reserve_change(fs, record.next);
            if (!next) result = ENOMEM;
            else if (!next->after_present || next->after.file_block != ORPHAN_NO_BLOCK) result = EUCLEAN;
            else next->after.previous = record.previous;
        }
        if (!result) result = inode_mark_dirty(inode);
        if (!result) {
            if (record.previous) {
                ext4_inode_set_del_time(previous.inode, record.next); finish_inode(&previous);
            } else ext4_fs_set_orphan_head(fs, record.next);
            ext4_inode_set_del_time(inode->inode, 0);
            finish_inode(inode); changed = true;
        }
        if (held) {
            int release = ext4_fs_put_inode_ref(&previous);
            if (!result) result = release;
        }
    }
    if (changed) change->after_present = false;
    return result;
}

int ext4_orphan_set_present(struct ext4_fs *fs, bool present)
{
    int result = require_transaction(fs);
    if (result) return result;
    result = ensure_index(fs, NULL);
    if (result) return result;
    if (!present) {
        size_t count;
        result = ext4_orphan_trans_count(ext4_current_trans(fs), &count);
        if (result) return result;
        if (count) return ENOTEMPTY;
    }
    if (ext4_sb_feature_com(ext4_fs_super(fs), EXT4_ORPHAN_FILE_COMPAT))
        ext4_current_operation(fs)->orphan_presence_touched = true;
    return EOK;
}
