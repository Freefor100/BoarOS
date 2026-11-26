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
#include <kernel/cost.h>
#include <kernel/raw_lock.h>
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
#include <ext4_ialloc.h>
#include <ext4_balloc.h>
#include <ext4_super.h>
#include <ext4_block_group.h>
#include <ext4_dir_idx.h>
#include <ext4_xattr.h>
#include <ext4_journal.h>
#include <ext4_orphan.h>
#include <ext4_truncate.h>
#include <ext4_extent.h>
#include <ext4_operation.h>
#include <kernel/sync.h>


#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static int require_resource(struct ext4_fs *fs, enum ext4_resource_kind kind,
    uint32_t key, bool *shared)
{
    if (shared) *shared = false;
    const struct ext4_lock *locks = fs->os_locks;
    if (!fs->jbd_journal || !locks || !locks->resource_acquire) return EOK;
    struct ext4_operation *operation = ext4_current_operation(fs);
    int result = locks->resource_acquire(locks->context, operation, kind, key, shared);
    if (result) {
        if (!operation) __builtin_trap();
        if (!operation->error) operation->error = result;
        if (operation->trans && !operation->trans->error) operation->trans->error = result;
    }
    return result;
}

int ext4_operation_require_resource(struct ext4_fs *fs, enum ext4_resource_kind kind, uint32_t key)
{ return require_resource(fs, kind, key, NULL); }

int ext4_operation_require_shared_block_group(struct ext4_fs *fs, uint32_t key, bool *shared)
{
    if (!shared) __builtin_trap();
    return require_resource(fs, EXT4_RESOURCE_BLOCK_GROUP, key, shared);
}

int ext4_operation_claim_inode(struct ext4_fs *fs, uint32_t inode)
{
    const struct ext4_lock *locks = fs->os_locks;
    if (!fs->jbd_journal || !locks || !locks->resource_claim_inode) return EOK;
    struct ext4_operation *operation = ext4_current_operation(fs);
    if (!operation || !operation->trans) __builtin_trap();
    int result = locks->resource_claim_inode(locks->context, operation, inode);
    if (result) {
        if (!operation->error) operation->error = result;
        if (!operation->trans->error) operation->trans->error = result;
    }
    return result;
}

void ext4_operation_release_resources(struct ext4_fs *fs)
{
    const struct ext4_lock *locks = fs->os_locks;
    if (locks && locks->resource_release) locks->resource_release(locks->context);
}

bool ext4_operation_resource_retry(struct ext4_fs *fs)
{
    const struct ext4_lock *locks = fs->os_locks;
    bool retry=locks && locks->resource_retry && locks->resource_retry(locks->context);
    if (retry) COST_ADD(JOURNAL_RESOURCE_RETRIES,1);
    return retry;
}

struct ext4_state_guard { struct ext4_fs *fs; };
static inline struct ext4_state_guard ext4_state_begin(struct ext4_fs *fs)
{ ext4_fs_state_lock(fs); return (struct ext4_state_guard){fs}; }
static inline struct ext4_state_guard ext4_state_read_begin(struct ext4_fs *fs)
{ ext4_fs_state_read_lock(fs); return (struct ext4_state_guard){fs}; }
static inline void ext4_state_end(struct ext4_state_guard *guard)
{ ext4_fs_state_unlock(guard->fs); }
#define EXT4_STATE_SCOPE(name, fs) struct ext4_state_guard name \
    __attribute__((cleanup(ext4_state_end))) = ext4_state_begin(fs)
#define EXT4_STATE_READ_SCOPE(name, fs) struct ext4_state_guard name \
    __attribute__((cleanup(ext4_state_end))) = ext4_state_read_begin(fs)

struct ext4_space_guard {
    struct ext4_fs *serial;
#if BOAROS_MAX_CPUS > 1
    struct kernel_raw_guard raw;
#endif
};
static inline void ext4_space_enter(struct ext4_space_guard *guard,struct ext4_fs *fs,bool write)
{
    /* A serial backend can keep borrowing its existing STATE qualification.
     * The parallel port only protects the free/reserved/delayed block tuple. */
#if BOAROS_MAX_CPUS > 1
    if (fs->os_locks && fs->os_locks->resource_acquire) {
        kernel_raw_lock_acquire(&fs->block_space,&guard->raw);
        return;
    }
#endif
    if (write) ext4_fs_state_lock(fs);
    else ext4_fs_state_read_lock(fs);
    guard->serial=fs;
}
static inline void ext4_space_leave(struct ext4_space_guard *guard)
{
#if BOAROS_MAX_CPUS > 1
    if (guard->raw.lock) {
        kernel_raw_lock_release(&guard->raw);
        return;
    }
#endif
    ext4_fs_state_unlock(guard->serial);
}
#define EXT4_SPACE_GUARD(name, fs, write) \
    struct ext4_space_guard name __attribute__((cleanup(ext4_space_leave)))={0}; \
    ext4_space_enter(&name,fs,write)
#define EXT4_SPACE_SCOPE(name, fs) EXT4_SPACE_GUARD(name,fs,true)
#define EXT4_SPACE_READ_SCOPE(name, fs) EXT4_SPACE_GUARD(name,fs,false)

static inline uint64_t unreserved_blocks(struct ext4_fs *fs)
{
    uint64_t free=ext4_sb_get_free_blocks_cnt(&fs->sb);
    uint64_t reserved=fs->reserved_blocks+fs->delayed_blocks+fs->delayed_extent_blocks;
    if (reserved>free) __builtin_trap();
    return free-reserved;
}

static void release_delayed_blocks(struct ext4_fs *fs,uint64_t data,uint64_t extent)
{
    if (data>fs->delayed_blocks || extent>fs->delayed_extent_blocks) __builtin_trap();
    fs->delayed_blocks-=data;
    fs->delayed_extent_blocks-=extent;
}

struct ext4_delalloc_chunk {
    RB_ENTRY(ext4_delalloc_chunk) index;
    struct ext4_delalloc_chunk *next;
    uint32_t key;
    uint64_t mask, accepted, known_holes;
    uint32_t borrowers;
};
static int delalloc_compare(struct ext4_delalloc_chunk *a, struct ext4_delalloc_chunk *b)
{ return a->key < b->key ? -1 : a->key != b->key; }
RB_GENERATE_STATIC(ext4_delalloc_tree, ext4_delalloc_chunk, index, delalloc_compare)
static uint64_t delalloc_mask(unsigned first, unsigned count)
{ return count == 64 ? UINT64_MAX : ((UINT64_C(1) << count) - 1) << first; }
static unsigned delalloc_count(uint64_t mask)
{ unsigned n=0;for (;mask;mask&=mask-1)n++;return n; }
static struct ext4_delalloc_chunk *delalloc_find(struct ext4_delalloc *da, uint32_t key)
{
    struct ext4_delalloc_chunk find={.key=key};
    return RB_FIND(ext4_delalloc_tree,&da->tree,&find);
}
/* STATE owns the reservation index and aggregate counters. Empty nodes retire
 * here; the actual free is outside STATE and all transaction publication. */
static uint64_t delalloc_range(struct ext4_delalloc *da,uint64_t first,uint64_t last,bool clear)
{
    if (first>=last) return 0;
    __atomic_store_n(&da->fresh_extent,false,__ATOMIC_RELAXED);
    struct ext4_delalloc_chunk find={.key=first>>6};
    struct ext4_delalloc_chunk *chunk=RB_NFIND(ext4_delalloc_tree,&da->tree,&find);
    uint64_t count=0;
    while (chunk && (uint64_t)chunk->key*64<last) {
        struct ext4_delalloc_chunk *next=RB_NEXT(ext4_delalloc_tree,&da->tree,chunk);
        uint64_t base=(uint64_t)chunk->key*64;
        unsigned begin=first>base?first-base:0,end=last-base<64?last-base:64;
        uint64_t covered=delalloc_mask(begin,end-begin);
        uint64_t mask=chunk->mask&covered;
        count+=delalloc_count(mask);
        chunk->known_holes&=~covered;
        if (clear) {
            chunk->mask&=~mask;chunk->accepted&=~mask;
            if (!chunk->mask && !chunk->borrowers) {
                RB_REMOVE(ext4_delalloc_tree,&da->tree,chunk);
                chunk->next=__atomic_load_n(&da->retired,__ATOMIC_RELAXED);
                __atomic_store_n(&da->retired,chunk,__ATOMIC_RELAXED);
            }
        }
        chunk=next;
    }
    if (clear) {
        if (count>__atomic_load_n(&da->blocks,__ATOMIC_RELAXED)) __builtin_trap();
        __atomic_fetch_sub(&da->blocks,count,__ATOMIC_RELAXED);
    }
    return count;
}
static uint64_t delalloc_spans(struct ext4_delalloc *da,
    const struct file_write_span *spans,unsigned count,size_t block,bool clear)
{
    uint64_t total=0,last=0;
    for (unsigned i=0;i<count;i++) {
        const struct file_write_span *span=&spans[i];
        if (!span->size) continue;
        uint64_t first=span->offset/block,end=(span->offset+span->size+block-1)/block;
        if (first<last)first=last;
        total+=delalloc_range(da,first,end,clear);last=end;
    }
    return total;
}
struct ext4_small_writeback_result {
    uint64_t size;
    struct ext4_timestamp times[3];
    unsigned fields;
};

struct ext4_small_writeback {
    ext4_file *const *files;
    const struct file_write_set *sets;
    struct ext4_data_completion *completion;
    unsigned prepared;
    struct ext4_block_allocation allocation;
    struct ext4_small_writeback_result results[EXT4_SMALL_WRITEBACK_MAX];
};

struct ext4_block_allocation *ext4_small_writeback_allocation(struct ext4_small_writeback *batch)
{ return &batch->allocation; }

static uint64_t delalloc_publish(struct ext4_operation *operation)
{
    unsigned block=operation->fs->bdev->lg_bsize;
    uint64_t count=0;
    if (operation->small_writeback) {
        const struct ext4_small_writeback *batch=operation->small_writeback;
        for (unsigned i=0;i<batch->prepared;i++)
            count+=delalloc_spans(batch->files[i]->delalloc,batch->sets[i].spans,
                batch->sets[i].count,block,true);
        operation->small_writeback=NULL;
    } else if (operation->delalloc)
        count=delalloc_spans(operation->delalloc,operation->delalloc_spans,
            operation->delalloc_span_count,block,true);
    else return 0;
    if (count!=operation->delalloc_credit)__builtin_trap();
    return count;
}

struct ext4_inode_goal {
    RB_ENTRY(ext4_inode_goal) index;
    TAILQ_ENTRY(ext4_inode_goal) pending;
    uint32_t inode;
    uint64_t full_sequence, data_sequence;
    bool data, observed, allocated;
};
static int inode_goal_order(const struct ext4_inode_goal *left,
                            const struct ext4_inode_goal *right)
{ return left->inode < right->inode ? -1 : left->inode != right->inode; }
RB_GENERATE_STATIC(ext4_inode_goal_tree, ext4_inode_goal, index, inode_goal_order);

static int operation_record_inode(struct ext4_fs *fs, uint32_t inode, bool data, bool allocated)
{
    struct ext4_operation *operation = ext4_current_operation(fs);
    struct jbd_journal *journal = fs->jbd_journal;
    if (!journal || !journal->grouped) {
        /* Standalone writes may publish a prefix even on failure. */
        if (operation && fs->inode_observer && fs->inode_observer->changed)
            fs->inode_observer->changed(fs->inode_observer->context,inode);
        return operation ? operation->error : EOK;
    }
    if (!operation) __builtin_trap();
    if (operation->error) return operation->error;
    struct ext4_inode_goal probe = {.inode = inode};
    struct ext4_inode_goal *goal = RB_FIND(ext4_inode_goal_tree, &operation->inode_goals, &probe);
    if (goal && (goal->observed || goal->allocated)) {
        goal->data |= data;
        return EOK;
    }
    /* An allocation owns the new identity until acceptance. Observe it
     * once to invalidate any prior-incarnation cache entries. A regular
     * inode without a pinned observer can acquire a live owner later. */
    bool observed = fs->inode_observer && fs->inode_observer->changed &&
        fs->inode_observer->changed(fs->inode_observer->context, inode);
    if (operation->error) return operation->error;
    if (!goal) {
        /* Reserve before acceptance; an allocation failure still aborts only
         * this operation's private inode/namespace versions. */
        goal = jbd_alloc(journal, sizeof(*goal));
        if (!goal) {
            operation->error = ENOMEM;
            if (!operation->trans->error) operation->trans->error = ENOMEM;
            return ENOMEM;
        }
        goal->inode = inode;
        if (RB_INSERT(ext4_inode_goal_tree, &operation->inode_goals, goal)) __builtin_trap();
    }
    goal->observed=observed;
    goal->allocated=allocated;
    goal->data |= data;
    return EOK;
}

int ext4_operation_record_inode(struct ext4_fs *fs, uint32_t inode, bool data)
{ return operation_record_inode(fs, inode, data, false); }

int ext4_operation_record_allocated_inode(struct ext4_fs *fs, uint32_t inode)
{ return operation_record_inode(fs, inode, true, true); }

void ext4_operation_discard_inode_goals(struct ext4_operation *operation)
{
    struct ext4_inode_goal *goal;
    while ((goal = RB_MIN(ext4_inode_goal_tree, &operation->inode_goals))) {
        RB_REMOVE(ext4_inode_goal_tree, &operation->inode_goals, goal);
        jbd_free(goal);
    }
}

void ext4_fs_retire_inode_goals(struct ext4_fs *fs, uint64_t durable)
{
    struct ext4_inode_goal *goal;
    while ((goal = TAILQ_FIRST(&fs->pending_inode_goals)) && goal->full_sequence <= durable) {
        TAILQ_REMOVE(&fs->pending_inode_goals, goal, pending);
        RB_REMOVE(ext4_inode_goal_tree, &fs->inode_goals, goal);
        jbd_free(goal);
    }
}

void ext4_operation_completed(struct ext4_operation *operation, uint64_t sequence)
{
    if (!operation || !operation->fs || operation->completed) return;
    operation->completed = true;
    struct ext4_fs *fs = operation->fs;
    struct jbd_journal *journal = fs->jbd_journal;
    struct ext4_inode_goal *candidate;
    while ((candidate = RB_MIN(ext4_inode_goal_tree, &operation->inode_goals))) {
        RB_REMOVE(ext4_inode_goal_tree, &operation->inode_goals, candidate);
        struct ext4_inode_goal *goal = RB_FIND(ext4_inode_goal_tree, &fs->inode_goals, candidate);
        if (!journal->grouped || sequence <= journal->durable_sequence) {
            jbd_free(candidate);
            continue;
        }
        if (goal) {
            TAILQ_REMOVE(&fs->pending_inode_goals, goal, pending);
            if (candidate->data) goal->data_sequence = sequence;
            jbd_free(candidate);
        } else {
            goal = candidate;
            goal->data_sequence = candidate->data ? sequence : 0;
            if (RB_INSERT(ext4_inode_goal_tree, &fs->inode_goals, goal)) __builtin_trap();
        }
        goal->full_sequence = sequence;
        TAILQ_INSERT_TAIL(&fs->pending_inode_goals, goal, pending);
    }
}

uint64_t ext4_fs_available_blocks(struct ext4_fs *fs)
{
    struct ext4_operation *operation=ext4_current_operation(fs);
    EXT4_SPACE_READ_SCOPE(space,fs);
    return unreserved_blocks(fs)+(operation?operation->delalloc_credit-operation->delalloc_used:0);
}

uint32_t ext4_fs_available_inodes(struct ext4_fs *fs)
{
    EXT4_STATE_READ_SCOPE(state, fs);
    uint32_t count = ext4_get32(&fs->sb, free_inodes_count);
    if (fs->reserved_inodes > count) __builtin_trap();
    return count - fs->reserved_inodes;
}

static uint32_t reserve_block_range(struct ext4_fs *fs,uint32_t count,bool exact)
{
    if (!fs->jbd_journal)return count;
    struct ext4_operation *operation=ext4_current_operation(fs);
    if (!operation)__builtin_trap();
    EXT4_SPACE_SCOPE(space,fs);
    uint64_t *consumed=&operation->delalloc_used;
    uint64_t *delayed=&fs->delayed_blocks;
    uint64_t credit=0;
    if (operation->extent_allocation==EXT4_ALLOC_DATA)
        credit=operation->delalloc_credit-*consumed;
    else if (operation->extent_allocation==EXT4_ALLOC_DELAYED_EXTENT_METADATA) {
        consumed=&operation->delalloc_extent_used;
        delayed=&fs->delayed_extent_blocks;
        credit=operation->delalloc_credit*EXT4_EXTENT_MAX_DEPTH-*consumed;
    }
    uint64_t available=unreserved_blocks(fs)+credit;
    if (count>available) {if(exact)return 0;count=available;}
    uint64_t used=count<credit?count:credit;
    if (used>*delayed)__builtin_trap();
    *delayed-=used;*consumed+=used;
    fs->reserved_blocks+=count;operation->reserved_blocks+=count;
    return count;
}
int ext4_fs_reserve_blocks(struct ext4_fs *fs,uint32_t count)
{ return reserve_block_range(fs,count,true)==count?EOK:ENOSPC; }
uint32_t ext4_fs_reserve_block_range(struct ext4_fs *fs,uint32_t count)
{ return reserve_block_range(fs,count,false); }

int ext4_fs_reserve_inodes(struct ext4_fs *fs, uint32_t count)
{
    if (!fs->jbd_journal) return EOK;
    struct ext4_operation *operation = ext4_current_operation(fs);
    if (!operation) __builtin_trap();
    EXT4_STATE_SCOPE(state, fs);
    uint32_t available = ext4_get32(&fs->sb, free_inodes_count);
    if (fs->reserved_inodes > available) __builtin_trap();
    if (count > available - fs->reserved_inodes) return ENOSPC;
    fs->reserved_inodes += count;
    operation->reserved_inodes += count;
    return EOK;
}

void ext4_fs_account_blocks(struct ext4_fs *fs, int64_t change)
{
    if (fs->jbd_journal) {
        struct ext4_operation *operation = ext4_current_operation(fs);
        if (!operation || __builtin_add_overflow(operation->block_delta, change,
                                                 &operation->block_delta)) __builtin_trap();
    } else {
        EXT4_SPACE_SCOPE(space,fs);
        uint64_t current = ext4_sb_get_free_blocks_cnt(&fs->sb);
        if ((change < 0 && (uint64_t)-change > current) ||
            (change > 0 && (uint64_t)change > ext4_sb_get_blocks_cnt(&fs->sb) - current)) __builtin_trap();
        ext4_sb_set_free_blocks_cnt(&fs->sb, current + change);
    }
}

void ext4_fs_account_inodes(struct ext4_fs *fs, int64_t change)
{
    if (fs->jbd_journal) {
        struct ext4_operation *operation = ext4_current_operation(fs);
        if (!operation || __builtin_add_overflow(operation->inode_delta, change,
                                                 &operation->inode_delta)) __builtin_trap();
    } else {
        uint32_t current = ext4_get32(&fs->sb, free_inodes_count);
        if ((change < 0 && (uint64_t)-change > current) ||
            (change > 0 && (uint64_t)change > ext4_get32(&fs->sb, inodes_count) - current)) __builtin_trap();
        ext4_set32(&fs->sb, free_inodes_count, current + change);
    }
}

uint32_t ext4_fs_ro_flags(struct ext4_fs *fs)
{
    struct ext4_operation *operation = ext4_current_operation(fs);
    EXT4_STATE_READ_SCOPE(state, fs);
    uint32_t flags = ext4_get32(&fs->sb, features_read_only);
    if (!operation) return flags;
    flags |= operation->ro_flags_add;
    if (operation->orphan_metadata && ext4_sb_feature_com(&fs->sb, EXT4_ORPHAN_FILE_COMPAT)) {
        size_t count;
        if (ext4_orphan_trans_count(operation->trans, &count)) __builtin_trap();
        flags = count ? flags | EXT4_FRO_COM_ORPHAN_PRESENT : flags & ~EXT4_FRO_COM_ORPHAN_PRESENT;
    }
    return flags;
}

void ext4_fs_add_ro_flags(struct ext4_fs *fs, uint32_t flags)
{
    if (flags & ~EXT4_FRO_COM_DIR_NLINK) __builtin_trap();
    if (fs->jbd_journal) {
        struct ext4_operation *operation = ext4_current_operation(fs);
        if (!operation) __builtin_trap();
        operation->ro_flags_add |= flags & ~to_le32(__atomic_load_n(&fs->sb.features_read_only, __ATOMIC_RELAXED));
    } else ext4_set32(&fs->sb, features_read_only, ext4_get32(&fs->sb, features_read_only) | flags);
}

uint32_t ext4_fs_orphan_head(struct ext4_fs *fs)
{
    struct ext4_operation *operation = ext4_current_operation(fs);
    if (operation && operation->orphan_chain) return operation->orphan_head;
    EXT4_STATE_READ_SCOPE(state, fs);
    return ext4_get32(&fs->sb, last_orphan);
}

void ext4_fs_set_orphan_head(struct ext4_fs *fs, uint32_t head)
{
    struct ext4_operation *operation = ext4_current_operation(fs);
    if (!operation || !operation->orphan_chain) __builtin_trap();
    operation->orphan_head = head;
}

bool ext4_operation_super_changed(struct ext4_operation *operation)
{
    return operation && !operation->published && (operation->block_delta || operation->inode_delta || operation->ro_flags_add ||
        (operation->orphan_chain && operation->orphan_head != operation->orphan_base) ||
        (operation->orphan_metadata && ext4_sb_feature_com(&operation->fs->sb, EXT4_ORPHAN_FILE_COMPAT) &&
         (operation->orphan_presence_touched || ext4_orphan_trans_membership_changed(operation->trans))));
}

void ext4_operation_memory_denied(struct ext4_fs *fs)
{
    struct ext4_operation *operation = ext4_current_operation(fs);
    if (operation) operation->memory_reservation_failed = true;
    else {
        struct kernel_io_context *context = kernel_io_context_current();
        context->ext4_last_fs = fs;
        context->ext4_last_memory_reservation_failed = true;
    }
}

/* Keep transfer/release of block reservations and the published free count
 * under one short lock. Inode counts and the on-disk image still use STATE. */
static void return_block_reservations(struct ext4_operation *operation)
{
    struct ext4_fs *fs=operation->fs;
    if (operation->reserved_blocks>fs->reserved_blocks) __builtin_trap();
    if (!operation->published) {
        fs->delayed_blocks+=operation->delalloc_used;
        fs->delayed_extent_blocks+=operation->delalloc_extent_used;
    }
    operation->delalloc_used=operation->delalloc_extent_used=0;
    fs->reserved_blocks-=operation->reserved_blocks;
    operation->reserved_blocks=0;
}

void ext4_operation_discard_super_state_held(struct ext4_operation *operation)
{
    if (!operation || !operation->fs) return;
    struct ext4_fs *fs = operation->fs;
    if (operation->reserved_blocks) {
        EXT4_SPACE_SCOPE(space,fs);
        return_block_reservations(operation);
    }
    if (operation->reserved_inodes) {
        if (operation->reserved_inodes>fs->reserved_inodes) __builtin_trap();
        fs->reserved_inodes -= operation->reserved_inodes;
        operation->reserved_inodes = 0;
    }
}

void ext4_operation_publish_super(struct ext4_operation *operation)
{
    if (!operation || !operation->fs || operation->published) return;
    if (ext4_operation_super_changed(operation) && !operation->super_prepared) __builtin_trap();
    struct ext4_fs *fs=operation->fs;
    uint64_t completed=delalloc_publish(operation);
    operation->published=true;
    bool blocks_changed = operation->super_prepared && operation->block_delta;
    if (blocks_changed || completed || operation->reserved_blocks) {
        EXT4_SPACE_SCOPE(space,fs);
        if (blocks_changed)
            ext4_sb_set_free_blocks_cnt(&fs->sb,operation->super_publish.blocks);
        if (operation->delalloc_used>completed ||
            operation->delalloc_extent_used>completed*EXT4_EXTENT_MAX_DEPTH) __builtin_trap();
        release_delayed_blocks(fs,completed-operation->delalloc_used,
            completed*EXT4_EXTENT_MAX_DEPTH-operation->delalloc_extent_used);
        return_block_reservations(operation);
    }
    if (operation->super_prepared) {
        struct ext4_sblock *accepted = &fs->sb;
        ext4_set32(accepted, free_inodes_count, operation->super_publish.inodes);
        __atomic_store_n(&accepted->features_read_only, to_le32(operation->super_publish.ro_flags), __ATOMIC_RELAXED);
        ext4_set32(accepted, last_orphan, operation->super_publish.orphan_head);
        accepted->checksum = operation->super_publish.checksum;
    }
    ext4_operation_discard_super_state_held(operation);
}

static int ext4_super_add(uint64_t current, int64_t change, uint64_t maximum, uint64_t *merged)
{
    if (current > maximum || (change >= 0 ? (uint64_t)change > maximum-current : (uint64_t)-change > current))
        return EUCLEAN;
    *merged = current + change;
    return EOK;
}

int ext4_operation_prepare_super(struct ext4_operation *operation, struct ext4_sblock *merged)
{
    if (!operation || !operation->fs || !ext4_operation_super_changed(operation)) return EOK;
    struct ext4_sblock *current = &operation->fs->sb;
    uint64_t blocks, inodes;
    int result = ext4_super_add(ext4_sb_get_free_blocks_cnt(current), operation->block_delta,
        ext4_sb_get_blocks_cnt(current), &blocks);
    if (!result) result = ext4_super_add(ext4_get32(current, free_inodes_count), operation->inode_delta,
        ext4_get32(current, inodes_count), &inodes);
    if (result) return result;
    uint32_t flags = ext4_get32(current, features_read_only) | operation->ro_flags_add;
    if (operation->orphan_metadata && ext4_sb_feature_com(current, EXT4_ORPHAN_FILE_COMPAT)) {
        size_t count;
        result = ext4_orphan_trans_count(operation->trans, &count);
        if (result) return result;
        flags = count ? flags | EXT4_FRO_COM_ORPHAN_PRESENT : flags & ~EXT4_FRO_COM_ORPHAN_PRESENT;
    }
    uint32_t head = ext4_get32(current, last_orphan);
    if (operation->orphan_chain && operation->orphan_head != operation->orphan_base) {
        if (head != operation->orphan_base && head != operation->orphan_head) return EAGAIN;
        head = operation->orphan_head;
    }
    uint32_t checksum = current->checksum;
    if (merged) {
        *merged = *current;
        ext4_sb_set_free_blocks_cnt(merged, blocks);
        ext4_set32(merged, free_inodes_count, inodes);
        ext4_set32(merged, features_read_only, flags);
        ext4_set32(merged, last_orphan, head);
        ext4_sb_set_csum(merged);
        checksum = merged->checksum;
    } else checksum = ext4_sb_inode_count_csum(current, (uint32_t)inodes);
    operation->super_publish.blocks = blocks;
    operation->super_publish.inodes = inodes;
    operation->super_publish.ro_flags = flags;
    operation->super_publish.orphan_head = head;
    operation->super_publish.checksum = checksum;
    operation->super_prepared = true;
    return EOK;
}

/* Caller holds STATE through admission and active-owner publication. */
static void ext4_operation_attach_locked(struct ext4_fs *fs, struct ext4_operation *operation,
                                  struct jbd_trans *trans)
{
    struct kernel_io_context *context = kernel_io_context_current();
    if (ext4_current_operation(fs) || fs->active_operations == UINT32_MAX) __builtin_trap();
    *operation = (struct ext4_operation){.fs = fs, .trans = trans,
        .next = context->ext4_operations};
    context->ext4_operations = operation;
    fs->active_operations++;
    context->ext4_last_fs = fs;
    context->ext4_last_aborted = 0;
    context->ext4_last_log_reservation_failed = false;
    context->ext4_last_memory_reservation_failed = false;
#if BOAROS_COST_DIAGNOSTICS
    operation->prepare_cost=kernel_cost_enter(COST_JOURNAL_PREPARE_TICKS);
#endif
}

/* The caller owns STATE and keeps active_operations until accept/abort or
 * synchronous commit has consumed the transaction containing this context. */
static void ext4_operation_detach_locked(struct ext4_operation *operation, bool aborted)
{
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_leave(&operation->prepare_cost);
#endif
    struct kernel_io_context *context = kernel_io_context_current();
    struct ext4_operation **link = &context->ext4_operations;
    while (*link && *link != operation) link = &(*link)->next;
    if (!*link || !operation->fs->active_operations) __builtin_trap();
    *link = operation->next;
    context->ext4_last_fs = operation->fs;
    context->ext4_last_aborted = aborted;
    context->ext4_last_log_reservation_failed = operation->log_reservation_failed;
    context->ext4_last_memory_reservation_failed = operation->memory_reservation_failed;
}

static uint32_t ext4_operation_depth(struct ext4_fs *fs)
{
    struct ext4_operation *operation = ext4_current_operation(fs);
    return operation ? operation->depth : 0;
}

static bool ext4_operation_aborted(struct ext4_fs *fs)
{
    struct ext4_operation *operation = ext4_current_operation(fs);
    struct kernel_io_context *context = kernel_io_context_current();
    return operation ? operation->aborted :
        context->ext4_last_fs == fs && context->ext4_last_aborted;
}

/**@brief   Mount point OS dependent lock*/
#define EXT4_MP_LOCK(_m)                                                       \
	do {                                                                   \
		if ((_m)->os_locks)                                            \
			(_m)->os_locks->lock((_m)->os_locks->context);       \
	} while (0)

/**@brief   Mount point OS dependent unlock*/
#define EXT4_MP_UNLOCK(_m)                                                     \
	do {                                                                   \
		if ((_m)->os_locks)                                            \
			(_m)->os_locks->unlock((_m)->os_locks->context);     \
	} while (0)

/**@brief   Mount point descriptor.*/
struct ext4_mountpoint {

	/**@brief   Mount done flag.*/
	bool mounted;
	bool reserved;

	/**@brief   Mount point name (@ref ext4_mount)*/
	char name[CONFIG_EXT4_MAX_MP_NAME + 1];

	/**@brief   OS dependent lock/unlock functions.*/
	const struct ext4_lock *os_locks;

	/* Optional adapter-owned realtime source; cleared on mount reuse. */
	ext4_clock_read clock;
	struct ext4_inode_observer inode_observer;
	struct ext4_journal_runtime journal_runtime;

	/**@brief   Ext4 filesystem internals.*/
	struct ext4_fs fs;

	/**@brief   JBD fs.*/
	struct jbd_fs jbd_fs;

	/**@brief   Journal.*/
	struct jbd_journal jbd_journal;

	/**@brief   Block cache.*/
	struct ext4_bcache bc;

	bool journal_stopped;
	bool overhead_valid;
	uint64_t overhead_blocks;
};

static int ext4_result(int result, int cleanup)
{
	return result == EOK ? cleanup : result;
}

static uint64_t ext4_legacy_max_blocks(struct ext4_fs *fs)
{
	uint64_t block_size = ext4_sb_get_block_size(ext4_fs_super(fs));
	uint64_t pointers_per_block = block_size / sizeof(uint32_t);
	uint64_t pointers_squared = pointers_per_block * pointers_per_block;
	uint64_t inode_blocks_limit;
	uint64_t metadata_blocks;
	uint64_t remaining;
	uint64_t result = fs->inode_block_limits[3];

	/* i_blocks is either a 32-bit count of 512-byte sectors, or, with the
	 * huge_file feature, a 48-bit count that can switch to filesystem-block
	 * units. Keep enough room for every indirect block of a dense file. */
	if (ext4_sb_feature_ro_com(ext4_fs_super(fs), EXT4_FRO_COM_HUGE_FILE))
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
	uint32_t block_size = ext4_sb_get_block_size(ext4_fs_super(fs));

#if CONFIG_EXTENT_ENABLE && CONFIG_EXTENTS_ENABLE
	if (ext4_sb_feature_incom(ext4_fs_super(fs), EXT4_FINCOM_EXTENTS) &&
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
/* Names and publication belong to the registry, not to any one mount lock. */
static struct kernel_raw_lock mount_index_lock = {
    .rank=KERNEL_RAW_RANK_OBJECT, .initialized=KERNEL_RAW_INITIALIZED
};

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
	struct ext4_sblock *sb = ext4_fs_super(enode->fs);

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
        ext4_inode_get_extra_isize(ext4_fs_super(ref->fs), ref->inode);
    return offset + sizeof(uint32_t) <= present &&
        offset + sizeof(uint32_t) <= ext4_get16(ext4_fs_super(ref->fs), inode_size);
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
    if (time.seconds <= INT32_MIN) {
        time.seconds = INT32_MIN;
        time.nanoseconds = 0;
    } else if (time.seconds >= maximum) {
        time.seconds = maximum;
        time.nanoseconds = 0;
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

static void ext4_publish_times(struct ext4_mountpoint *mp,
                              struct ext4_inode_ref *ref, unsigned fields, void *owner)
{
    static const size_t base[3] = {offsetof(struct ext4_inode, access_time),
        offsetof(struct ext4_inode, modification_time), offsetof(struct ext4_inode, change_inode_time)};
    static const size_t extra[3] = {offsetof(struct ext4_inode, atime_extra),
        offsetof(struct ext4_inode, mtime_extra), offsetof(struct ext4_inode, ctime_extra)};
    fields &= EXT4_TIME_ATIME | EXT4_TIME_MTIME | EXT4_TIME_CTIME;
    if (!fields || !mp->inode_observer.store) return;
    struct ext4_timestamp values[3];
    for (unsigned i = 0; i < 3; i++) if (fields & (1U << i))
        values[i] = ext4_time_load(ref, base[i], extra[i]);
    mp->inode_observer.store(mp->inode_observer.context, owner, ref->index, fields, values);
}

static bool ext4_relatime_expired(struct ext4_inode_ref *ref, struct ext4_timestamp now)
{
    struct ext4_timestamp atime = ext4_time_load(ref,
        offsetof(struct ext4_inode, access_time), offsetof(struct ext4_inode, atime_extra));
    struct ext4_timestamp mtime = ext4_time_load(ref,
        offsetof(struct ext4_inode, modification_time), offsetof(struct ext4_inode, mtime_extra));
    struct ext4_timestamp ctime = ext4_time_load(ref,
        offsetof(struct ext4_inode, change_inode_time), offsetof(struct ext4_inode, ctime_extra));
    return ext4_time_compare(mtime, atime) >= 0 ||
        ext4_time_compare(ctime, atime) >= 0 || now.seconds - atime.seconds >= 86400;
}

static unsigned ext4_store_current_times(struct ext4_mountpoint *mp,
    struct ext4_inode_ref *ref, unsigned fields)
{
    struct ext4_timestamp now;
    if (mp->fs.read_only || !mp->clock || !mp->clock(&now)) return 0;
    if (ext4_inode_has_flag(ref->inode, EXT4_INODE_FLAG_NOATIME) ||
        ((fields & (EXT4_TIME_ATIME | EXT4_TIME_RELATIME)) ==
         (EXT4_TIME_ATIME | EXT4_TIME_RELATIME) && !ext4_relatime_expired(ref, now)))
        fields &= ~EXT4_TIME_ATIME;
    if (fields & EXT4_TIME_ATIME)
        ext4_time_store(ref, offsetof(struct ext4_inode, access_time),
                        offsetof(struct ext4_inode, atime_extra), now);
    if (fields & EXT4_TIME_MTIME)
        ext4_time_store(ref, offsetof(struct ext4_inode, modification_time),
                        offsetof(struct ext4_inode, mtime_extra), now);
    if (fields & EXT4_TIME_CTIME)
        ext4_time_store(ref, offsetof(struct ext4_inode, change_inode_time),
                        offsetof(struct ext4_inode, ctime_extra), now);
    return fields;
}

static void ext4_touch_inode(struct ext4_mountpoint *mp,
    struct ext4_inode_ref *ref, unsigned fields, void *owner)
{
    ext4_publish_times(mp,ref,ext4_store_current_times(mp,ref,fields),owner);
}

static void ext4_initialize_inode_times(struct ext4_mountpoint *mp,struct ext4_inode_ref *ref)
{
    /* The caller will publish the newly allocated inode and its initial
     * attributes together. There is no live timestamp owner yet. */
    (void)ext4_store_current_times(mp,ref,EXT4_TIME_ATIME | EXT4_TIME_MTIME | EXT4_TIME_CTIME);
}

static int ext4_touch_create_parent(struct ext4_mountpoint *mp,
    struct ext4_inode_ref *parent, bool read_parent)
{
    const unsigned fields = EXT4_TIME_MTIME | EXT4_TIME_CTIME;
    if (!read_parent) {
        ext4_touch_inode(mp, parent, fields, NULL);
        return EOK;
    }

    /* A regular child in an existing linear leaf touches only these times.
     * Check and checksum one record without taking a private table image. */
    struct {
        struct ext4_inode inode;
        uint8_t rest[256 - sizeof(struct ext4_inode)];
    } bytes;
    uint16_t size = ext4_get16(ext4_fs_super(parent->fs), inode_size);
    if (size > sizeof(bytes)) __builtin_trap();
    memcpy(&bytes, parent->inode, size);
    struct ext4_inode_ref updated = *parent;
    updated.inode = &bytes.inode;
    unsigned stored = ext4_store_current_times(mp, &updated, fields);
    if (!updated.dirty) {
        ext4_publish_times(mp, parent, stored, NULL);
        return EOK;
    }
    if (jbd_trans_stage_parent_times(ext4_current_trans(parent->fs), parent, updated.inode)) {
        ext4_publish_times(mp, &updated, stored, NULL);
        return EOK;
    }
    int result = ext4_fs_inode_ref_write(parent);
    if (result) return result;
    /* Serial qualification covers the parent record until this operation
     * finishes. A private table image can also contain the new child's slot. */
    memcpy(parent->inode, &bytes, size);
    parent->dirty = true;
    ext4_publish_times(mp, parent, stored, NULL);
    return EOK;
}

static int ext4_link(struct ext4_mountpoint *mp, struct ext4_inode_ref *parent,
		     struct ext4_inode_ref *ch, const char *n,
		     uint32_t len, bool rename, bool *read_parent)
{
	/* Check maximum name length */
	if (len > EXT4_DIRECTORY_FILENAME_LEN)
		return EINVAL;

	/* Add entry to parent directory */
	int r = read_parent ? ext4_dir_add_entry_with_read_parent(parent, n, len, ch, read_parent) :
	    ext4_dir_add_entry(parent, n, len, ch);
	if (r != EOK)
		return r;

	/* Fill new dir -> add '.' and '..' entries.
	 * Also newly allocated inode should have 0 link count.
	 */

	bool is_dir = ext4_inode_is_type(ext4_fs_super(&mp->fs), ch->inode,
			       EXT4_INODE_MODE_DIRECTORY);
	if (is_dir && !rename) {
		if (ext4_current_trans(&mp->fs)) {
			r = ext4_dir_init_child(ch, parent);
			if (r != EOK) return r;
		} else
#if CONFIG_DIR_INDEX_ENABLE
		/* Initialize directory index if supported */
		if (ext4_sb_feature_com(ext4_fs_super(&mp->fs), EXT4_FCOM_DIR_INDEX)) {
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
		ext4_touch_inode(mp, parent, EXT4_TIME_MTIME | EXT4_TIME_CTIME, NULL);
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
			r = ext4_dir_find_entry(&res, ch, "..", strlen(".."), true);
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

	return ext4_touch_create_parent(mp, parent, read_parent && *read_parent);
}

static void ext4_unlink_update_links(struct ext4_mountpoint *mp,
    struct ext4_inode_ref *parent, struct ext4_inode_ref *child)
{
	if (ext4_inode_is_type(ext4_fs_super(&mp->fs), child->inode,
			      EXT4_INODE_MODE_DIRECTORY)) {
		ext4_fs_inode_links_count_dec(parent);
		parent->dirty = true;
	}
	ext4_touch_inode(mp, parent, EXT4_TIME_MTIME | EXT4_TIME_CTIME, NULL);
	ext4_touch_inode(mp, child, EXT4_TIME_CTIME, NULL);
	if (ext4_inode_get_links_cnt(child->inode)) {
		ext4_fs_inode_links_count_dec(child);
		child->dirty = true;
	}
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

	ext4_unlink_update_links(mp, parent, child);
	return EOK;
}

/****************************************************************************/

static int ext4_mount_format(const char *dev_name, const char *mount_point,
	       bool read_only, enum ext4_disk_format format)
{
	int r;
	uint32_t bsize;
	struct ext4_bcache *bc;
	struct ext4_blockdev *bd = 0;
	struct ext4_mountpoint *mp = 0;

	ext4_assert(mount_point && dev_name);

	size_t mp_len = strlen(mount_point);

	if (!mp_len || mp_len > CONFIG_EXT4_MAX_MP_NAME)
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

	{
	KERNEL_TASK_LOCK_SCOPE(index, &mount_index_lock);
	for (size_t i = 0; i < CONFIG_EXT4_MOUNTPOINTS_COUNT; ++i) {
		if (!s_mp[i].reserved && !s_mp[i].mounted) {
			memset(&s_mp[i], 0, sizeof(s_mp[i]));
			s_mp[i].reserved = true;
			strcpy(s_mp[i].name, mount_point);
			s_mp[i].clock = NULL;
			s_mp[i].journal_stopped = false;
			s_mp[i].overhead_valid = false;
			mp = &s_mp[i];
			break;
		}

		if (!strcmp(s_mp[i].name, mount_point))
			return s_mp[i].mounted ? EOK : EBUSY;
	}

	}
	if (!mp)
		return ENOMEM;

	r = ext4_block_init(bd);
	if (r != EOK) { { KERNEL_TASK_LOCK_SCOPE(index, &mount_index_lock); mp->reserved = false; } return r; }

	r = ext4_fs_init(&mp->fs, bd, read_only, format);
	if (r != EOK) {
		ext4_block_fini(bd);
		{ KERNEL_TASK_LOCK_SCOPE(index, &mount_index_lock); mp->reserved = false; }
		return r;
	}

	bsize = ext4_sb_get_block_size(ext4_fs_super(&mp->fs));
	ext4_block_set_lb_size(bd, bsize);
	bc = &mp->bc;

	r = ext4_bcache_init_dynamic(bc, CONFIG_BLOCK_DEV_CACHE_SIZE, bsize);
	if (r != EOK) {
		ext4_block_fini(bd);
		{ KERNEL_TASK_LOCK_SCOPE(index, &mount_index_lock); mp->reserved = false; }
		return r;
	}

	if (bsize != bc->itemsize) {
		ext4_bcache_fini_dynamic(bc);
		ext4_block_fini(bd);
		{ KERNEL_TASK_LOCK_SCOPE(index, &mount_index_lock); mp->reserved = false; }
		return ENOTSUP;
	}

	/*Bind block cache to block device*/
	r = ext4_block_bind_bcache(bd, bc);
	if (r == EOK && format == EXT4_DISK_EXT2 && !read_only) {
		struct ext4_sblock *sb = ext4_fs_super(&mp->fs);
		/* Publish a durable dirty marker before allowing namespace changes. */
		ext4_set16(sb, mount_count, ext4_get16(sb, mount_count) + 1);
		ext4_set16(sb, state, ext4_get16(sb, state) & ~EXT4_SUPERBLOCK_STATE_VALID_FS);
		r = ext4_sb_write(bd, sb);
		if (r == EOK) r = ext4_blockdev_flush(bd);
	}
	if (r != EOK) {
		ext4_bcache_cleanup(bc);
		ext4_block_fini(bd);
		ext4_bcache_fini_dynamic(bc);
		{ KERNEL_TASK_LOCK_SCOPE(index, &mount_index_lock); mp->reserved = false; }
		return r;
	}

	bd->fs = &mp->fs;
	/* Binding is complete; optional geometry storage adds no failed-mount
	 * owner. Inode data and mutable descriptors still use the normal cache. */
	ext4_fs_prepare_inode_locations(&mp->fs);
	{ KERNEL_TASK_LOCK_SCOPE(index, &mount_index_lock); mp->mounted = 1; }
	return r;
}

int ext4_mount(const char *dev_name, const char *mount_point, bool read_only)
{ return ext4_mount_format(dev_name, mount_point, read_only, EXT4_DISK_EXT4); }

int ext4_mount_ext2(const char *dev_name, const char *mount_point, bool read_only)
{ return ext4_mount_format(dev_name, mount_point, read_only, EXT4_DISK_EXT2); }

int ext4_mount_ext3(const char *dev_name, const char *mount_point, bool read_only)
{ return ext4_mount_format(dev_name, mount_point, read_only, EXT4_DISK_EXT3); }


static struct ext4_mountpoint *ext4_get_mount(const char *path);

static int ext4_umount_locked(const char *mount_point)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENODEV;
	if (mp->fs.active_operations) return EBUSY;
    int nojournal_error=__atomic_load_n(&mp->fs.nojournal_error,__ATOMIC_ACQUIRE);
    if (nojournal_error) return nojournal_error;
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
	/* The clean marker may reach the device only after directory and
	 * allocation updates have reached stable storage. */
	if (mp->fs.ext2_format && !mp->fs.read_only) {
		r = ext4_blockdev_flush(mp->fs.bdev);
		if (r != EOK) return r;
	}
	r = ext4_fs_fini(&mp->fs);
	if (r != EOK) return r;
	if (!mp->fs.read_only) {
		r = ext4_blockdev_flush(mp->fs.bdev);
        if (r != EOK) {
            if (!ext4_sb_feature_com(&mp->fs.sb,EXT4_FCOM_HAS_JOURNAL)) {
                /* The clean superblock write completed, but durability did
                 * not. Keep this mounted owner failed; no extra FLUSH or
                 * retry can turn the uncertain session back into writable. */
                __atomic_store_n(&mp->fs.nojournal_error,r,__ATOMIC_RELEASE);
                ext4_set16(&mp->fs.sb,state,ext4_get16(&mp->fs.sb,state)&
                    ~EXT4_SUPERBLOCK_STATE_VALID_FS);
            }
            return r;
        }
	}
	r = ext4_block_fini(mp->fs.bdev);
	if (r != EOK) return r;
	ext4_bcache_cleanup(mp->fs.bdev->bc);
	ext4_bcache_fini_dynamic(mp->fs.bdev->bc);
	mp->fs.bdev->fs = NULL;
	{ KERNEL_TASK_LOCK_SCOPE(index, &mount_index_lock); mp->mounted = false; }
	return EOK;
}

int ext4_umount(const char *mount_point)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	EXT4_MP_LOCK(mp);
	int result = ext4_umount_locked(mount_point);
	EXT4_MP_UNLOCK(mp);
	if (result == EOK) { KERNEL_TASK_LOCK_SCOPE(index, &mount_index_lock); mp->reserved = false; }
	return result;
}

int ext4_remount(const char *mount_point, bool read_only)
{
    struct ext4_mountpoint *mp=ext4_get_mount(mount_point);
    if (!mp) return ENOENT;
    EXT4_MP_LOCK(mp);
    int result=EOK;
    bool nojournal_publication=false;
    struct ext4_fs *fs=&mp->fs;
    if (fs->active_operations) { result=EBUSY; goto Finish; }
    result=__atomic_load_n(&fs->nojournal_error,__ATOMIC_ACQUIRE);
    if (result) goto Finish;
    if (fs->read_only==read_only) goto Finish;
    if (read_only) {
        if (!ext4_sb_feature_com(&fs->sb,EXT4_FCOM_HAS_JOURNAL) &&
            (ext4_get16(&fs->sb,state)&EXT4_SUPERBLOCK_STATE_ERROR_FS)) {
            result=EUCLEAN;goto Finish;
        }
        /* journal_stop checkpoints and flushes the clean superblock before
         * releasing the session. No fallible teardown follows publication. */
        result=ext4_block_cache_flush(fs->bdev);
        if (!result) result=ext4_journal_stop(mount_point);
        if (!result && fs->ext2_format) result=ext4_blockdev_flush(fs->bdev);
        if (!result && !ext4_sb_feature_com(&fs->sb,EXT4_FCOM_HAS_JOURNAL)) {
            struct ext4_sblock clean=fs->sb;
            ext4_set16(&clean,state,EXT4_SUPERBLOCK_STATE_VALID_FS);
            nojournal_publication=true;
            result=ext4_sb_write(fs->bdev,&clean);
            if (!result) result=ext4_blockdev_flush(fs->bdev);
            if (!result) fs->sb=clean;
        }
        if (!result) fs->read_only=true;
    } else {
        bool required_read_only=false;
        uint16_t state=ext4_get16(&fs->sb,state);
        if (!ext4_sb_feature_com(&fs->sb,EXT4_FCOM_HAS_JOURNAL) &&
            (!(state & EXT4_SUPERBLOCK_STATE_VALID_FS) || (state & EXT4_SUPERBLOCK_STATE_ERROR_FS))) {
            result=EUCLEAN;goto Finish;
        }
        result=ext4_fs_check_features(fs,&required_read_only);
        if (!result && required_read_only) result=EROFS;
        if (result) goto Finish;
        fs->read_only=false;
        if (ext4_sb_feature_com(&fs->sb,EXT4_FCOM_HAS_JOURNAL)) {
            result=ext4_orphan_validate(fs);
            if (!result) result=ext4_journal_start(mount_point);
        } else {
            struct ext4_sblock dirty=fs->sb;
            ext4_set16(&dirty,state,ext4_get16(&dirty,state)&~EXT4_SUPERBLOCK_STATE_VALID_FS);
            nojournal_publication=true;
            result=ext4_sb_write(fs->bdev,&dirty);
            if (!result) result=ext4_blockdev_flush(fs->bdev);
            if (!result) fs->sb=dirty;
        }
        if (result) fs->read_only=true;
    }
Finish:
    if (result && nojournal_publication)
        __atomic_store_n(&fs->nojournal_error,result,__ATOMIC_RELEASE);
    EXT4_MP_UNLOCK(mp);
    return result;
}

static struct ext4_mountpoint *ext4_get_mount(const char *path)
{
	KERNEL_TASK_LOCK_SCOPE(index, &mount_index_lock);
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
	if (mp->fs.read_only || !ext4_sb_feature_com(ext4_fs_super(&mp->fs),
						EXT4_FCOM_HAS_JOURNAL)) return EOK;
	if (mp->fs.jbd_fs)
		return mp->fs.jbd_journal && !mp->journal_stopped
		       ? jbd_journal_error(&mp->jbd_journal) : EBUSY;
	int r = jbd_get_fs(&mp->fs, &mp->jbd_fs);
	if (r != EOK) return r;
	mp->fs.jbd_fs = &mp->jbd_fs;
	r = jbd_journal_start(&mp->jbd_fs, &mp->jbd_journal);
	if (r != EOK) {
		if (jbd_journal_error(&mp->jbd_journal)) {
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
	if (mp->fs.active_operations) return EBUSY;
	if (!mp->fs.jbd_fs) return EOK;
	int r;
	if (mp->fs.jbd_journal && !mp->journal_stopped) {
		if (jbd_journal_error(&mp->jbd_journal)) return jbd_journal_error(&mp->jbd_journal);
		if (mp->jbd_journal.grouped) {
			r = ext4_journal_group_drain(mount_point);
			if (r != EOK) return r;
			jbd_journal_group_fini(&mp->jbd_journal);
		}
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
	if (ext4_sb_feature_com(ext4_fs_super(&mp->fs), EXT4_FCOM_HAS_JOURNAL)) {
		struct jbd_fs *jbd_fs = &mp->jbd_fs;
		if (mp->fs.jbd_fs) { r = EBUSY; goto Finish; }
		r = jbd_get_fs(&mp->fs, jbd_fs);
		if (r != EOK) goto Finish;
		mp->fs.jbd_fs = jbd_fs;
		mp->journal_stopped = true;
		r = jbd_recover(jbd_fs);
		/* Replay may have restored only a prefix before reporting an error. */
		ext4_fs_reset_inode_locations(&mp->fs);
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
		for (bgid = 0;bgid < ext4_block_group_cnt(ext4_fs_super(&mp->fs));bgid++) {
			r = ext4_fs_get_block_group_ref(&mp->fs, bgid, &bg_ref);
			if (r != EOK)
				goto Finish;

			free_blocks_count +=
				ext4_bg_get_free_blocks_count(bg_ref.block_group,
						ext4_fs_super(&mp->fs));
			free_inodes_count +=
				ext4_bg_get_free_inodes_count(bg_ref.block_group,
						ext4_fs_super(&mp->fs));

			r = ext4_fs_put_block_group_ref(&bg_ref);
			if (r != EOK) goto Finish;
		}
		{ EXT4_SPACE_SCOPE(space,&mp->fs);
		  ext4_sb_set_free_blocks_cnt(ext4_fs_super(&mp->fs), free_blocks_count); }
		ext4_set32(ext4_fs_super(&mp->fs), free_inodes_count, free_inodes_count);
		/* We don't need to save the superblock stats immediately. */
	}

Finish:
	EXT4_MP_UNLOCK(mp);
	return r;
}

static uint64_t ext4_group_age_deadline(const struct jbd_trans *running)
{
	const uint64_t idle_ns = 100000000;
	if (running->ordered_count || running->revoke_count || !LIST_EMPTY(&running->quarantine))
		return running->first_dirty_ns + idle_ns;
	/* Keep repeated background snapshots joinable. An explicit sync target,
	 * resource pressure or an ordinary metadata operation can seal earlier. */
	if (running->background_times_only)
		return running->first_dirty_ns + UINT64_C(5000000000);
	/* Metadata that keeps arriving can share its current log record. Bound
	 * both the wait after the last edit and the lifetime of the group. */
	uint64_t idle = running->last_dirty_ns + idle_ns;
	uint64_t maximum = running->first_dirty_ns + 500000000;
	return idle < maximum ? idle : maximum;
}

__unused
static bool ext4_group_due(struct ext4_mountpoint *mp, bool force)
{
	/* All consumers hold STATE while borrowing the running group. */
	struct jbd_journal *journal = mp->fs.jbd_journal;
	struct jbd_trans *running = journal->running;
	if (!running) return false;
	uint64_t now = mp->journal_runtime.now_ns ? mp->journal_runtime.now_ns(mp->journal_runtime.context) : 0;
	/* A sync target requests durability, not another queued commit boundary.
	 * Keep the next group joinable while an older group occupies the consumer;
	 * its fixed target is sealed as soon as that consumer becomes available. */
	bool sync_due = journal->seal_target >= running->sequence &&
		!journal->committing && TAILQ_EMPTY(&journal->sealed_queue) &&
		now >= journal->sync_join_deadline_ns;
	return force || sync_due ||
		(uint64_t)running->reserved_logs * journal->block_size >= 256 * 1024 ||
		now >= ext4_group_age_deadline(running);
}

__unused
static int ext4_group_freeze(struct ext4_mountpoint *mp, bool force)
{
    /* The due check and freeze share the caller's writer qualification. */
    if (!mp->fs.jbd_journal->running) return EOK;
#if BOAROS_COST_DIAGNOSTICS
    struct jbd_journal *journal = mp->fs.jbd_journal;
    struct jbd_trans *running = journal->running;
    uint64_t now = mp->journal_runtime.now_ns(mp->journal_runtime.context);
    bool bytes = (uint64_t)running->reserved_logs * journal->block_size >= 256 * 1024;
    bool age = now >= ext4_group_age_deadline(running);
    bool target = journal->seal_target >= running->sequence;
#endif
    int result = jbd_journal_freeze(mp->fs.jbd_journal);
#if BOAROS_COST_DIAGNOSTICS
    /* 只记成功封口；条件可重叠，不能相加推导提交组数。 */
    if (result == EOK) {
        if (bytes) COST_ADD(JOURNAL_SEAL_BYTES, 1);
        if (age) COST_ADD(JOURNAL_SEAL_AGE, 1);
        if (target) COST_ADD(JOURNAL_SEAL_TARGET, 1);
        if (force) COST_ADD(JOURNAL_SEAL_FORCE, 1);
    }
#else
    (void)force;
#endif
    return result;
}

__unused
static uint64_t ext4_group_reached(const struct jbd_journal *journal, enum ext4_journal_wait kind)
{
	if (kind == EXT4_JOURNAL_WAIT_SEALED) return journal->sealed_sequence;
	if (kind == EXT4_JOURNAL_WAIT_CHECKPOINT) return journal->checkpoint_sequence;
	if (kind == EXT4_JOURNAL_WAIT_ORDERED_RELEASE &&
		journal->ordered_release_sequence > journal->durable_sequence)
		return journal->ordered_release_sequence;
	return journal->durable_sequence;
}

__unused
static int ext4_group_wait(struct ext4_mountpoint *mp, uint64_t target, enum ext4_journal_wait kind)
{
	struct jbd_journal *journal = mp->fs.jbd_journal;
	{
	EXT4_STATE_SCOPE(state, &mp->fs);
	uint64_t reached = ext4_group_reached(journal, kind);
	int error = jbd_journal_error(journal);
	if (error || reached >= target) return error;
	if (journal->seal_target < target) journal->seal_target = target;
	journal->sync_join_deadline_ns = 0;
	if (kind == EXT4_JOURNAL_WAIT_ORDERED_RELEASE && journal->ordered_release_target < target)
		__atomic_store_n(&journal->ordered_release_target,target,__ATOMIC_RELEASE);
	if (kind == EXT4_JOURNAL_WAIT_CHECKPOINT && journal->checkpoint_target < target)
		journal->checkpoint_target = target;
	}
	if (mp->journal_runtime.request) mp->journal_runtime.request(mp->journal_runtime.context,
		EXT4_JOURNAL_WORK_COMMIT | (kind == EXT4_JOURNAL_WAIT_CHECKPOINT ? EXT4_JOURNAL_WORK_CHECKPOINT : 0));
	if (mp->journal_runtime.wait) return mp->journal_runtime.wait(mp->journal_runtime.context, target, kind);
	for (;;) {
		{
		EXT4_STATE_SCOPE(state, &mp->fs);
		uint64_t reached = ext4_group_reached(journal, kind);
		int error = jbd_journal_error(journal);
		if (error || reached >= target) return error;
		}
		EXT4_MP_UNLOCK(mp);
		int r = ext4_journal_group_service(mp->name, false);
		EXT4_MP_LOCK(mp);
		if (r != EOK) return r;
	}
}

__unused
static bool ext4_space_waits_checkpoint(struct ext4_mountpoint *mp)
{
	/* Caller holds STATE while comparing reservations with quarantine. */
	struct jbd_journal *journal = mp->fs.jbd_journal;
	if (journal->quarantined_blocks) {
		EXT4_SPACE_READ_SCOPE(space,&mp->fs);
		if (unreserved_blocks(&mp->fs) <= journal->quarantined_blocks + 32) return true;
	}
	if (journal->quarantined_inodes) {
		uint32_t inodes = ext4_get32(&mp->fs.sb, free_inodes_count);
		if (mp->fs.reserved_inodes > inodes) __builtin_trap();
		if (inodes - mp->fs.reserved_inodes <= journal->quarantined_inodes + 1) return true;
	}
	return false;
}

static int ext4_group_reclaim_goal(struct ext4_mountpoint *mp, unsigned resources,
    struct jbd_journal_reclaim *goal)
{
    EXT4_STATE_READ_SCOPE(state, &mp->fs);
    struct jbd_journal *journal = mp->fs.jbd_journal;
    int error = jbd_journal_error(journal);
    if (error) return error;
    return jbd_journal_reclaim_goal(journal, resources, goal) ? EOK :
        (resources & JBD_JOURNAL_LOG ? ENOSPC : ENOMEM);
}

static enum ext4_journal_wait ext4_reclaim_wait_kind(const struct ext4_mountpoint *mp,
    const struct jbd_journal_reclaim *goal)
{
    if (goal->stage != JBD_JOURNAL_COMMIT) return EXT4_JOURNAL_WAIT_CHECKPOINT;
    return mp->journal_runtime.ordered_released ?
        EXT4_JOURNAL_WAIT_ORDERED_RELEASE : EXT4_JOURNAL_WAIT_DURABLE;
}

static int ext4_group_wait_reclaim(struct ext4_mountpoint *mp,
    const struct jbd_journal_reclaim *goal)
{
    return ext4_group_wait(mp, goal->sequence, ext4_reclaim_wait_kind(mp, goal));
}

__unused
static int __ext4_trans_start(struct ext4_mountpoint *mp, bool wait)
{
    struct jbd_journal *journal = mp->fs.jbd_journal;
    struct ext4_operation *operation = ext4_current_operation(&mp->fs);
    if (!journal) return EOK;
    if (jbd_journal_error(journal)) return jbd_journal_error(journal);
    if (mp->journal_stopped) return EBUSY;
    if (operation) {
        if (operation->error) return operation->error;
        if (operation->depth == UINT32_MAX) return EOVERFLOW;
        operation->depth++;
        return EOK;
    }
    if (!journal->grouped) {
        bool reuse;
        {
        EXT4_STATE_READ_SCOPE(state, &mp->fs);
        reuse = ext4_space_waits_checkpoint(mp);
        }
        /* Immediate commit can retain old checkpoints whose blocks cannot
         * be reused by a new private operation before checkpoint completes. */
        if (reuse) {
            int result = jbd_journal_purge_cp_trans(journal, true, false);
            if (result) return result;
        }
    }
    for (;;) {
        struct kernel_io_context *context = kernel_io_context_current();
        context->ext4_last_fs = &mp->fs;
        context->ext4_last_memory_reservation_failed = false;
        context->ext4_last_log_reservation_failed = false;
        /* The foreground call/backend qualification already keeps this mount
         * and journal alive. Allocate only its private control before STATE;
         * no preparation, wait or backend release occurs while it is held. */
        struct jbd_trans *trans;
        int allocation = jbd_journal_new_trans(journal, &trans);
        bool budget_denied = context->ext4_last_memory_reservation_failed;
        int result = EOK;
        uint64_t target = 0;
        enum ext4_journal_wait kind = EXT4_JOURNAL_WAIT_CHECKPOINT;
        unsigned request = 0;
        bool attached = false, retry_allocation = false;
        {
        EXT4_STATE_SCOPE(state, &mp->fs);
        if (jbd_journal_error(journal)) result = jbd_journal_error(journal);
        else if (mp->journal_stopped) result = EBUSY;
        if (!result && journal->grouped && ext4_group_due(mp, false)) {
            uint64_t sequence = journal->running->sequence;
            result = ext4_group_freeze(mp, false);
            if (!result) request = EXT4_JOURNAL_WORK_COMMIT;
            else if (result == EAGAIN && wait) {
                result = EOK;
                target = sequence;
                kind = EXT4_JOURNAL_WAIT_SEALED;
            }
        }
        /* An allocator OOM is not a retention-quota denial. */
        if (!result && allocation && allocation == ENOMEM && !budget_denied) result = allocation;
        if (!result && !target && journal->grouped) {
            unsigned resources = jbd_journal_operation_resources(journal, trans, 0, 0, NULL, 0);
            if (ext4_space_waits_checkpoint(mp)) resources |= JBD_JOURNAL_REUSE;
            if (resources) {
                if (!wait) result = EAGAIN;
                else {
                    struct jbd_journal_reclaim goal;
                    result = ext4_group_reclaim_goal(mp, resources, &goal);
                    if (!result) {
                        target = goal.sequence;
                        kind = ext4_reclaim_wait_kind(mp, &goal);
                    }
                }
            }
        }
        if (!result && !target && allocation) {
            result = allocation;
            bool reclaimable = allocation == ENOSPC || (allocation == ENOMEM && budget_denied);
            if (journal->grouped && reclaimable && !jbd_journal_error(journal)) {
                if (!wait) result = EAGAIN;
                else {
                    struct jbd_journal_reclaim goal;
                    int reclaim = ext4_group_reclaim_goal(mp,
                        allocation == ENOSPC ? JBD_JOURNAL_LOG : JBD_JOURNAL_MEMORY, &goal);
                    if (reclaim) result = reclaim == ENOMEM ? allocation : reclaim;
                    else {
                        target = goal.sequence;
                        kind = ext4_reclaim_wait_kind(mp, &goal);
                        result = EOK; retry_allocation = true;
                    }
                }
            }
        }
        if (!result && !target) {
            ext4_operation_attach_locked(&mp->fs, &trans->operation, trans);
            trans->operation.depth = 1;
            attached = true;
        }
        }
        if (trans && !attached) jbd_journal_free_trans(journal, trans, true);
        /* A successful freeze must wake its consumer even if this caller
         * cannot attach, or subsequently returns an allocation/space error. */
        if (request && mp->journal_runtime.request)
            mp->journal_runtime.request(mp->journal_runtime.context, request);
        if (result || attached) return result;
        result = ext4_group_wait(mp, target, kind);
        if (result) return result;
        if (retry_allocation) {
            if (allocation == ENOSPC) COST_ADD(JOURNAL_LOG_RETRIES, 1);
            else COST_ADD(JOURNAL_MEMORY_RETRIES, 1);
        }
    }
}

__unused
static int ext4_trans_record_super(struct ext4_mountpoint *mp)
{
    struct ext4_operation *operation = ext4_current_operation(&mp->fs);
    if (!ext4_operation_super_changed(operation)) return EOK;
    if (jbd_trans_try_reuse_superblock(operation->trans)) return EOK;
    struct ext4_block block = EXT4_BLOCK_ZERO();
    uint32_t block_size = ext4_sb_get_block_size(&mp->fs.sb);
    int result = ext4_block_get_range(mp->fs.bdev, &block,
        EXT4_SUPERBLOCK_OFFSET / block_size, EXT4_SUPERBLOCK_OFFSET % block_size,
        sizeof(mp->fs.sb), EXT4_BLOCK_READ);
    if (result) return result;
    /* Commit accounting still reserves this block. Acceptance initializes
     * the superblock range from the latest counters and checksum. */
    result = jbd_trans_reserve_superblock(operation->trans, &block);
    int cleanup = ext4_block_set(mp->fs.bdev, &block);
    return result ? result : cleanup;
}

__unused
static int __ext4_trans_finish(struct ext4_mountpoint *mp,int error,unsigned *work)
{
    struct jbd_journal *journal = mp->fs.jbd_journal;
    struct ext4_operation *operation = ext4_current_operation(&mp->fs);
    struct jbd_trans *trans = operation ? operation->trans : NULL;
    int result = error;
    if (!journal) return error;
    if (!operation || !operation->depth || !trans)
        return jbd_journal_error(journal) ? jbd_journal_error(journal) : (error ? error : EINVAL);
    if (!operation->error && error) operation->error = error;
    if (!operation->error && trans->error) operation->error = trans->error;
    if (--operation->depth) {
        operation->aborted = operation->error != EOK;
        return operation->error;
    }
    if (!operation->error) operation->error = jbd_journal_error(journal);
    if (!operation->error) operation->error = ext4_trans_record_super(mp);
    if (!operation->error) operation->error = trans->error;
    if (!operation->error)
        operation->error = jbd_trans_prepare_operation(trans);
    result = operation->error;
    if (journal->grouped) {
        uint64_t now = !result && mp->journal_runtime.now_ns ?
            mp->journal_runtime.now_ns(mp->journal_runtime.context) : 0;
        unsigned request = 0;
        {
        EXT4_STATE_SCOPE(state, &mp->fs);
        /* accept may free the embedded operation. Remove its caller link,
         * but retain the active owner through acceptance or complete abort. */
        ext4_operation_detach_locked(operation, result != EOK);
        if (result == EOK) {
            bool running = journal->running != NULL;
            bool due = ext4_group_due(mp, false);
            result = jbd_journal_accept_state_held(journal, trans, now);
            /* 首组要登记原有期限；普通合并不改变期限，也不重复叫醒消费者。 */
            if (result == EOK) {
                if ((!running && journal->running) || (!due && ext4_group_due(mp, false)))
                    request |= EXT4_JOURNAL_WORK_COMMIT;
                /* 私有准备已占预算；接纳前压力可能已满足，不能只比较接纳边沿。 */
                if (jbd_journal_checkpoint_ready(journal)) request |= EXT4_JOURNAL_WORK_CHECKPOINT;
            }
        }
        if (result != EOK) {
            jbd_journal_free_trans_state_held(journal, trans, true);
            if (jbd_journal_error(journal)) result = jbd_journal_error(journal);
        }
        if (!mp->fs.active_operations) __builtin_trap();
        mp->fs.active_operations--;
        }
        *work=request;
    } else {
        {
        EXT4_STATE_SCOPE(state, &mp->fs);
        ext4_operation_detach_locked(operation, result != EOK);
        if (result != EOK) {
            jbd_journal_free_trans_state_held(journal, trans, true);
            if (jbd_journal_error(journal)) result = jbd_journal_error(journal);
        }
        }
        if (result == EOK) result = jbd_journal_commit_trans(journal, trans);
        {
        EXT4_STATE_SCOPE(state, &mp->fs);
        if (!mp->fs.active_operations) __builtin_trap();
        mp->fs.active_operations--;
        }
    }
    if (result != EOK && !jbd_journal_error(journal)) {
        struct kernel_io_context *context = kernel_io_context_current();
        context->ext4_last_fs = &mp->fs;
        context->ext4_last_aborted = 1;
    }
    return result;
}

static int ext4_journal_start_locked(const char *mount_point __unused)
{
	int r = EOK;
#if CONFIG_JOURNALING_ENABLE
	r = __ext4_journal_start(mount_point);
#endif
	return r;
}

int ext4_journal_start(const char *mount_point __unused)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	EXT4_MP_LOCK(mp);
	int result = ext4_journal_start_locked(mount_point);
	EXT4_MP_UNLOCK(mp);
	return result;
}

int ext4_journal_group_enable(const char *mount_point,
	const struct ext4_journal_runtime *runtime, size_t memory_limit)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	EXT4_MP_LOCK(mp);
	struct jbd_journal *journal = mp->fs.jbd_journal;
	int r = !journal || mp->fs.read_only ? EROFS : EOK;
	if (!runtime || !runtime->now_ns || !memory_limit) r = EINVAL;
	{
	EXT4_STATE_SCOPE(state, &mp->fs);
	if (r == EOK && (journal->grouped || mp->fs.active_operations)) r = EBUSY;
	}
	if (r == EOK) r = jbd_journal_purge_cp_trans(journal, true, false);
	if (r == EOK) {
		mp->journal_runtime = *runtime;
		r = jbd_journal_group_init(journal, memory_limit);
		if (r == EOK) {
			journal->ordered_released = runtime->ordered_released;
			journal->ordered_release_context = runtime->context;
		}
	}
	EXT4_MP_UNLOCK(mp);
	return r;
}

static int ext4_journal_group_run(const char *mount_point, bool force,
	enum jbd_journal_stage stage, bool background)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	EXT4_MP_LOCK(mp);
	struct jbd_journal *journal = mp->fs.jbd_journal;
	int r;
	enum jbd_journal_stage selected;
	{
	EXT4_STATE_SCOPE(state, &mp->fs);
	r = !journal || !journal->grouped ? EINVAL : jbd_journal_error(journal);
	if (r == EOK && ext4_operation_depth(&mp->fs)) r = EBUSY;
	/* An explicit checkpoint call requests the durable prefix that exists
	 * now. The target survives a bounded selection until every batch retires. */
	if (r == EOK && stage == JBD_JOURNAL_CHECKPOINT) {
        /* Aged home writes are optional while newer accepted work needs the
         * same device. Explicit targets and actual retention pressure win. */
        if (background && (journal->sync_drains ||
            journal->accepted_sequence > journal->durable_sequence) &&
            !jbd_journal_checkpoint_ready(journal)) r = EAGAIN;
        else if (journal->checkpoint_target < journal->durable_sequence)
            journal->checkpoint_target = journal->durable_sequence;
    }
	if (r == EOK && stage != JBD_JOURNAL_CHECKPOINT && ext4_group_due(mp, force)) {
		r = ext4_group_freeze(mp, force);
		if (r == EAGAIN) r = EOK; /* 已有提交 owner，先让队首进展。 */
	}
	selected = r == EOK ?
		jbd_journal_select_stage(journal, stage) : JBD_JOURNAL_IDLE;
	}
	EXT4_MP_UNLOCK(mp);
	if (r != EOK || !selected) return r;
	r = selected == JBD_JOURNAL_CHECKPOINT ? jbd_journal_checkpoint(journal) : jbd_journal_submit(journal);
	EXT4_MP_LOCK(mp);
	r = jbd_journal_retire_stage(journal, selected, r);
	EXT4_MP_UNLOCK(mp);
	return r;
}

int ext4_journal_group_service(const char *mount_point, bool force)
{
	return ext4_journal_group_run(mount_point, force, JBD_JOURNAL_IDLE, false);
}

int ext4_journal_group_commit(const char *mount_point, bool force)
{
	return ext4_journal_group_run(mount_point, force, JBD_JOURNAL_COMMIT, false);
}

int ext4_journal_group_checkpoint(const char *mount_point)
{
	return ext4_journal_group_run(mount_point, false, JBD_JOURNAL_CHECKPOINT, false);
}

int ext4_journal_group_checkpoint_background(const char *mount_point)
{ return ext4_journal_group_run(mount_point, false, JBD_JOURNAL_CHECKPOINT, true); }

static int ext4_journal_wait_reclaim(const char *mount_point, bool drain, bool memory_pressure)
{
    struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
    if (!mp) return ENOENT;
    EXT4_MP_LOCK(mp);
    struct jbd_journal *journal = mp->fs.jbd_journal;
    int r;
    struct jbd_journal_reclaim goal = {0};
    {
    EXT4_STATE_SCOPE(state, &mp->fs);
    r = !journal || !journal->grouped ? EINVAL : jbd_journal_error(journal);
    if (!r) {
        if (drain) {
            if (journal->accepted_sequence > journal->checkpoint_sequence)
                goal = (struct jbd_journal_reclaim){journal->accepted_sequence, JBD_JOURNAL_CHECKPOINT};
        } else jbd_journal_reclaim_goal(journal, JBD_JOURNAL_MEMORY, &goal);
        /* Keep the normal group deadline unless actual memory pressure needs
         * the running owner's ordered-data loan returned now. */
        if (!drain && !memory_pressure && journal->running &&
            goal.sequence == journal->running->sequence && !ext4_group_due(mp, false))
            goal.sequence = 0;
    }
    }
    if (!r && goal.sequence) r = ext4_group_wait_reclaim(mp, &goal);
    EXT4_MP_UNLOCK(mp);
    return r;
}

int ext4_journal_group_drain(const char *mount_point)
{ return ext4_journal_wait_reclaim(mount_point,true,true); }
int ext4_journal_group_reclaim(const char *mount_point,bool memory_pressure)
{ return ext4_journal_wait_reclaim(mount_point,false,memory_pressure); }

int ext4_journal_group_progress(const char *mount_point, struct ext4_journal_progress *progress)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp || !progress) return EINVAL;
	EXT4_MP_LOCK(mp);
	struct jbd_journal *journal = mp->fs.jbd_journal;
	int r;
	{
	EXT4_STATE_READ_SCOPE(state, &mp->fs);
	r = !journal || !journal->grouped ? ENOTSUP : EOK;
	if (r == EOK) {
		struct jbd_trans *running = journal->running;
		bool commit_ready = !journal->committing &&
			(!TAILQ_EMPTY(&journal->sealed_queue) || ext4_group_due(mp, false));
		bool checkpoint_ready = jbd_journal_checkpoint_ready(journal);
		uint64_t deadline = running ? ext4_group_age_deadline(running) : 0;
		if (running && !journal->committing && TAILQ_EMPTY(&journal->sealed_queue) &&
		    journal->seal_target >= running->sequence && journal->sync_join_deadline_ns &&
		    journal->sync_join_deadline_ns < deadline)
			deadline = journal->sync_join_deadline_ns;
		*progress = (struct ext4_journal_progress){
			.accepted = journal->accepted_sequence, .sealed = journal->sealed_sequence,
			.durable = journal->durable_sequence, .checkpoint = journal->checkpoint_sequence,
			.deadline_ns = deadline,
			.memory_used = jbd_memory_used(journal), .memory_peak = jbd_memory_peak(journal),
			.ready = commit_ready || checkpoint_ready,
			.commit_ready = commit_ready, .checkpoint_ready = checkpoint_ready,
			.checkpoint_pending = !TAILQ_EMPTY(&journal->cp_queue),
			.error = jbd_journal_error(journal)};
	}
	}
	EXT4_MP_UNLOCK(mp);
	return r;
}

static int ext4_journal_stop_locked(const char *mount_point __unused)
{
	int r = EOK;
#if CONFIG_JOURNALING_ENABLE
	r = __ext4_journal_stop(mount_point);
#endif
	return r;
}

int ext4_journal_stop(const char *mount_point __unused)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	EXT4_MP_LOCK(mp);
	int result = ext4_journal_stop_locked(mount_point);
	EXT4_MP_UNLOCK(mp);
	return result;
}

int ext4_recover(const char *mount_point __unused)
{
	int r = EOK;
#if CONFIG_JOURNALING_ENABLE
	r = __ext4_recover(mount_point);
#endif
	return r;
}

static int ext4_trans_start_mode(struct ext4_mountpoint *mp __unused, bool wait __unused)
{
    if (!mp) return ENOENT;
    if (mp->fs.super_replay_required) return EUCLEAN;
    int nojournal_error=__atomic_load_n(&mp->fs.nojournal_error,__ATOMIC_ACQUIRE);
    if (nojournal_error) return nojournal_error;
    if (mp->fs.read_only) return EROFS;
    struct ext4_operation *operation = ext4_current_operation(&mp->fs);
#if CONFIG_JOURNALING_ENABLE
    if (!wait && operation) return EAGAIN;
    if (mp->fs.jbd_journal) return __ext4_trans_start(mp, wait);
#endif
    if (operation && operation->depth == UINT32_MAX) return EOVERFLOW;
    if (!operation) {
        bool batch = mp->fs.ext2_format && mp->fs.bdev->bdif->bwrite_batch;
        size_t size = sizeof(*operation) + (batch ? sizeof(struct ext4_scope_batch) : 0);
        operation = ext4_calloc(1, size);
        if (!operation) return ENOMEM;
        {
        EXT4_STATE_SCOPE(state, &mp->fs);
        ext4_operation_attach_locked(&mp->fs, operation, NULL);
        }
        if (batch) operation->scope.batch = (struct ext4_scope_batch *)(operation + 1);
        ext4_bcache_scope_begin(&mp->bc, &operation->scope);
    }
    operation->depth++;
    return EOK;
}

static int ext4_trans_start(struct ext4_mountpoint *mp)
{ return ext4_trans_start_mode(mp, true); }

static int ext4_trans_finish_accept(struct ext4_mountpoint *mp __unused, int result,
    void (*accepted)(void *), void *context)
{
    struct ext4_operation *operation = ext4_current_operation(&mp->fs);
    if (!operation || !operation->depth) return result ? result : EINVAL;
    bool outer = operation->depth == 1;
    if (accepted && !outer) __builtin_trap();
    void *staged_inodes = outer ? operation->staged_inodes : NULL;
#if CONFIG_JOURNALING_ENABLE
    unsigned work=0;
    if (mp->fs.jbd_journal) result = __ext4_trans_finish(mp,result,&work);
    else
#endif
    if (--operation->depth == 0) {
        if (result == EOK) result = operation->error;
        int cleanup = ext4_bcache_scope_end(&mp->bc, &operation->scope);
        if (result == EOK) result = cleanup;
        /* Nonjournal operations may have completed a persistent prefix even
         * when later device I/O failed; retain their resulting counters. */
        {
        EXT4_STATE_SCOPE(state, &mp->fs);
        ext4_operation_detach_locked(operation, result != EOK);
        if (!mp->fs.active_operations) __builtin_trap();
        mp->fs.active_operations--;
        }
        ext4_free(operation);
    }
    if (accepted && result == EOK) accepted(context);
    if (outer) ext4_operation_release_resources(&mp->fs);
#if CONFIG_JOURNALING_ENABLE
    /* Acceptance already published the work. Waking its consumer does not
     * require the operation's inode or allocation-group qualification. */
    if (work && mp->journal_runtime.request)
        mp->journal_runtime.request(mp->journal_runtime.context,work);
#endif
    if (outer && mp->inode_observer.finish)
        mp->inode_observer.finish(mp->inode_observer.context, staged_inodes, result == EOK);
    return result;
}

static int ext4_trans_finish(struct ext4_mountpoint *mp, int result)
{ return ext4_trans_finish_accept(mp, result, NULL, NULL); }

static bool ext4_file_rolled_back(ext4_file *file, int result)
{
	return result != EOK && file->mp->fs.jbd_journal &&
	       ext4_operation_aborted(&file->mp->fs);
}

static int ext4_track_nested_file(ext4_file *file)
{
    const struct ext4_inode_observer *observer = &file->mp->inode_observer;
    if (!file->time_owner || !observer->track_file || !file->mp->fs.jbd_journal ||
        !ext4_operation_depth(&file->mp->fs))
        return EOK;
    return observer->track_file(observer->context, file);
}

static int ext4_file_data_finish(ext4_file *file, int result)
{
    if (!result) result = ext4_operation_record_inode(&file->mp->fs, file->inode, true);
    return ext4_trans_finish(file->mp, result);
}

int ext4_transaction_begin(const char *mount_point)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	if (!mp->fs.jbd_journal) return ENOTSUP;
	EXT4_MP_LOCK(mp);
	int result = ext4_trans_start(mp);
	if (result != EOK) EXT4_MP_UNLOCK(mp);
	return result;
}

int ext4_transaction_end(const char *mount_point)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	if (!mp->fs.jbd_journal) return ENOTSUP;
	if (!ext4_operation_depth(&mp->fs)) __builtin_trap();
	int result = ext4_trans_finish(mp, EOK);
	EXT4_MP_UNLOCK(mp);
	return result;
}

int ext4_transaction_abort(const char *mount_point, int error)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	if (!mp->fs.jbd_journal) return ENOTSUP;
	if (!ext4_operation_depth(&mp->fs)) __builtin_trap();
	int result = ext4_trans_finish(mp, error ? error : ECANCELED);
	EXT4_MP_UNLOCK(mp);
	return result;
}

/* Only the failed caller's allocation source permits a reclaim wait. The
 * operation has already aborted and returned its inode/group qualifications. */
static bool ext4_operation_retry_reclaim(struct ext4_mountpoint *mp, int *result)
{
    struct jbd_journal *journal = mp->fs.jbd_journal;
    struct kernel_io_context *context = kernel_io_context_current();
    if (!journal || !journal->grouped || jbd_journal_error(journal) ||
        ext4_operation_depth(&mp->fs) || !ext4_operation_aborted(&mp->fs) ||
        context->ext4_last_fs != &mp->fs ||
        !((*result == ENOSPC && context->ext4_last_log_reservation_failed) ||
          (*result == ENOMEM && context->ext4_last_memory_reservation_failed))) return false;
    struct jbd_journal_reclaim goal;
    int reclaim = ext4_group_reclaim_goal(mp,
        *result == ENOSPC ? JBD_JOURNAL_LOG : JBD_JOURNAL_MEMORY, &goal);
    if (reclaim) {
        if (reclaim != ENOMEM) *result = reclaim;
        return false;
    }
#if BOAROS_COST_DIAGNOSTICS
    int retry_error=*result;
#endif
    *result = ext4_group_wait_reclaim(mp, &goal);
    if (*result!=EOK) return false;
#if BOAROS_COST_DIAGNOSTICS
    if (retry_error==ENOSPC) COST_ADD(JOURNAL_LOG_RETRIES,1);
    else COST_ADD(JOURNAL_MEMORY_RETRIES,1);
#endif
    return true;
}

static int ext4_metadata_execute_locked(struct ext4_mountpoint *mp,
    int (*apply)(void *),void *context)
{
    struct jbd_journal *journal=mp->fs.jbd_journal;
    bool outer=!ext4_operation_depth(&mp->fs);
    int result;
    if (!journal) result=apply(context);
    else for (;;) {
        result=ext4_trans_start(mp);
        if (result) break;
        result=apply(context);
        result=ext4_trans_finish(mp,result);
        if (result == EAGAIN && outer && ext4_operation_resource_retry(&mp->fs)) continue;
        if (!outer || !ext4_operation_retry_reclaim(mp, &result)) break;
    }
    return result;
}

int ext4_metadata_execute(const char *mount_point,int (*apply)(void *),void *context)
{
    if (!mount_point || !apply) return EINVAL;
    struct ext4_mountpoint *mp=ext4_get_mount(mount_point);
    if (!mp) return ENOENT;
    EXT4_MP_LOCK(mp);
    int result=ext4_metadata_execute_locked(mp,apply,context);
    EXT4_MP_UNLOCK(mp);
    return result;
}


int ext4_mount_point_stats(const char *mount_point,
                           struct ext4_mount_stats *stats)
{
    if (!mount_point || !stats) return EINVAL;
    struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
    if (!mp) return ENOENT;
    EXT4_MP_LOCK(mp);
    ext4_fs_state_lock(&mp->fs);
    int result = mp->fs.super_replay_required ? EUCLEAN : EOK;
    if (!result && mp->fs.jbd_journal) result = jbd_journal_error(mp->fs.jbd_journal);
    bool prepare = !mp->overhead_valid;
    ext4_fs_state_unlock(&mp->fs);
    if (!result && prepare) {
        uint64_t candidate;
        result = ext4_fs_calculate_overhead(&mp->fs, &candidate);
        if (!result) {
            ext4_fs_state_lock(&mp->fs);
            if (!mp->overhead_valid) {
                mp->overhead_blocks = candidate;
                mp->overhead_valid = true;
            }
            ext4_fs_state_unlock(&mp->fs);
        }
    }
    /* Geometry stays immutable after mount. Only the counter/error/overhead
     * tuple needs a publication snapshot; no state lock spans device I/O. */
    struct ext4_sblock *sb = &mp->fs.sb;
    ext4_fs_state_lock(&mp->fs);
    uint64_t free_blocks;
    {
        EXT4_SPACE_READ_SCOPE(space,&mp->fs);
        free_blocks=ext4_sb_get_free_blocks_cnt(sb);
        if (mp->fs.delayed_blocks>free_blocks) __builtin_trap();
        free_blocks-=mp->fs.delayed_blocks;
    }
    uint32_t free_inodes = ext4_get32(sb, free_inodes_count);
    uint64_t overhead = mp->overhead_blocks;
    if (!result && mp->fs.jbd_journal) result = jbd_journal_error(mp->fs.jbd_journal);
    ext4_fs_state_unlock(&mp->fs);
    uint64_t total = ext4_sb_get_blocks_cnt(sb);
    uint64_t reserved = ((uint64_t)ext4_get32(sb, reserved_blocks_count_hi) << 32) |
                        ext4_get32(sb, reserved_blocks_count_lo);
    if (!result && (overhead > total || free_blocks > total - overhead ||
        reserved > total || free_inodes > ext4_get32(sb, inodes_count))) result = EUCLEAN;
    if (!result) {
        stats->inodes_count = ext4_get32(sb, inodes_count);
        stats->free_inodes_count = free_inodes;
        stats->blocks_count = total;
        stats->free_blocks_count = free_blocks;
        stats->overhead_blocks = overhead;
        stats->reserved_blocks_count = reserved;
        memcpy(stats->uuid, sb->uuid, sizeof(stats->uuid));
        stats->block_size = ext4_sb_get_block_size(sb);
        stats->block_group_count = ext4_block_group_cnt(sb);
        stats->blocks_per_group = ext4_get32(sb, blocks_per_group);
        stats->inodes_per_group = ext4_get32(sb, inodes_per_group);
        memcpy(stats->volume_name, sb->volume_name, 16);
    }
    EXT4_MP_UNLOCK(mp);
    return result;
}

int ext4_mount_setup_locks(const char *mount_point,
			   const struct ext4_lock *locks)
{
	KERNEL_TASK_LOCK_SCOPE(index, &mount_index_lock);
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
	mp->fs.os_locks = locks;
	return EOK;
}

int ext4_mount_setup_clock(const char *mount_point, ext4_clock_read clock)
{
    struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
    if (!mp) return ENOENT;
    mp->clock = clock;
    return EOK;
}

bool ext4_file_read_clock(const ext4_file *file, struct ext4_timestamp *now)
{
    return file && file->mp && file->mp->mounted && file->mp->clock &&
        file->mp->clock(now);
}

int ext4_mount_setup_inode_observer(const char *mount_point,
                                  const struct ext4_inode_observer *observer)
{
    struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
    if (!mp) return ENOENT;
    mp->inode_observer = observer ? *observer : (struct ext4_inode_observer){0};
    mp->fs.inode_observer=&mp->inode_observer;
    return EOK;
}

int ext4_file_relatime_needed(ext4_file *file, bool *needed)
{
    if (!file || !file->mp || !file->mp->mounted || !needed) return EINVAL;
    struct ext4_mountpoint *mp = file->mp;
    struct ext4_timestamp now;
    *needed = false;
    if (mp->fs.read_only || !mp->clock || !mp->clock(&now)) return EOK;
    if (mp->os_locks && mp->os_locks->read_lock)
        mp->os_locks->read_lock(mp->os_locks->context);
    else EXT4_MP_LOCK(mp);
    struct ext4_inode_ref ref;
    int result = ext4_fs_get_inode_ref_read(&mp->fs, file->inode, &ref);
    if (result == EOK) {
        *needed = !ext4_inode_has_flag(ref.inode, EXT4_INODE_FLAG_NOATIME) &&
            ext4_relatime_expired(&ref, now);
        result = ext4_fs_put_inode_ref(&ref);
    }
    EXT4_MP_UNLOCK(mp);
    return result;
}

int ext4_file_touch(ext4_file *file, unsigned int fields)
{
    struct ext4_inode_ref ref;
    struct ext4_mountpoint *mp;
    int result;
    if (!file || !file->mp || !file->mp->mounted) return EINVAL;
    mp = file->mp;
    bool relatime_checked=(fields&EXT4_TIME_RELATIME_CHECKED)!=0;
    fields&=~EXT4_TIME_RELATIME_CHECKED;
    if (mp->fs.read_only)
        return fields & (EXT4_TIME_MTIME | EXT4_TIME_CTIME) ? EROFS : EOK;
    struct ext4_timestamp now;
    if (!mp->clock || !mp->clock(&now)) return EOK;
    if (!relatime_checked && fields == (EXT4_TIME_ATIME | EXT4_TIME_RELATIME)) {
        bool needed;
        result = ext4_file_relatime_needed(file, &needed);
        if (result != EOK || !needed) return result;
    }
    EXT4_MP_LOCK(mp);
    result = ext4_trans_start(mp);
    if (result != EOK) { EXT4_MP_UNLOCK(mp); return result; }
    result = ext4_fs_get_inode_ref(&mp->fs, file->inode, &ref);
    if (result == EOK) {
        /* Shared query grants no upgrade; ext4_touch_inode rechecks current
         * inode times under the exclusive gate after any competing edit. */
        ext4_touch_inode(mp, &ref, fields, file->time_owner);
        result = ext4_fs_put_inode_ref(&ref);
    }
    result = ext4_trans_finish(mp, result);
    EXT4_MP_UNLOCK(mp);
    return result;
}

static int ext4_file_set_times_common(ext4_file *file, unsigned fields,
                       const struct ext4_timestamp supplied[3], bool publish, bool wait)
{
    static const size_t base[3] = {
        offsetof(struct ext4_inode, access_time),
        offsetof(struct ext4_inode, modification_time),
        offsetof(struct ext4_inode, change_inode_time)
    };
    static const size_t extra[3] = {
        offsetof(struct ext4_inode, atime_extra),
        offsetof(struct ext4_inode, mtime_extra),
        offsetof(struct ext4_inode, ctime_extra)
    };
    if (!file || !file->mp || !file->mp->mounted) return EINVAL;
    struct ext4_mountpoint *mp = file->mp;
    struct ext4_inode_ref ref;
    struct ext4_timestamp current[3];
    const struct ext4_timestamp *times = supplied;
    int r = EOK;
    EXT4_MP_LOCK(mp);
    if (!publish) {
        times = current;
        fields = mp->inode_observer.snapshot
            ? mp->inode_observer.snapshot(file->time_owner, current) : 0;
    }
    if (fields & ~(EXT4_TIME_ATIME | EXT4_TIME_MTIME | EXT4_TIME_CTIME) ||
        (fields && !times)) { r = EINVAL; goto Unlock; }
    for (unsigned i = 0; i < 3; i++) {
        if ((fields & (1U << i)) && times[i].nanoseconds >= 1000000000U) {
            r = EINVAL;
            goto Unlock;
        }
    }
    if (!fields) goto Unlock;
    if (mp->fs.read_only) { r = EROFS; goto Unlock; }
    r = ext4_trans_start_mode(mp, wait);
    if (r != EOK) goto Unlock;
    if (!publish && !wait) {
        struct jbd_trans *trans = ext4_current_trans(&mp->fs);
        if (trans) trans->background_times_only = true;
    }
    r = ext4_fs_get_inode_ref(&mp->fs, file->inode, &ref);
    if (!publish && mp->inode_observer.snapshot)
        fields = mp->inode_observer.snapshot(file->time_owner, current);
    if (r == EOK) {
        for (unsigned i = 0; i < 3; i++)
            if (fields & (1U << i)) ext4_time_store(&ref, base[i], extra[i], times[i]);
        if (publish) ext4_publish_times(mp, &ref, fields, file->time_owner);
        r = ext4_fs_put_inode_ref(&ref);
    }
    r = ext4_trans_finish(mp, r);
    if (!publish && !ext4_operation_depth(&mp->fs) && mp->inode_observer.written)
        mp->inode_observer.written(file->time_owner, fields, times, r);
Unlock:
    if (r != EOK && ext4_current_trans(&mp->fs) && !ext4_current_trans(&mp->fs)->error)
        ext4_current_trans(&mp->fs)->error = r;
    EXT4_MP_UNLOCK(mp);
    return r;
}

int ext4_file_set_times(ext4_file *file, unsigned fields,
                       const struct ext4_timestamp times[3])
{ return ext4_file_set_times_common(file, fields, times, true, true); }

int ext4_file_writeback_times(ext4_file *file, bool wait)
{ return ext4_file_set_times_common(file, 0, NULL, false, wait); }

static void ext4_inode_set_mode_initial(struct ext4_inode_ref *ref, uint32_t mode)
{
    uint32_t previous = ext4_inode_get_mode(ext4_fs_super(ref->fs), ref->inode);
    if ((previous & 0170000U) == 0040000U) mode |= previous & 02000U;
    ext4_inode_set_mode(ext4_fs_super(ref->fs), ref->inode,
        (previous & ~0xfffU) | (mode & 0xfffU));
    ref->dirty = true;
}

struct ext4_flags_change {
    ext4_file *file;
    uint32_t mask, flags;
    struct ext4_timestamp ctime;
    bool changed;
};

static int ext4_apply_flags(void *argument)
{
    struct ext4_flags_change *change=argument;
    struct ext4_mountpoint *mp=change->file->mp;
    change->changed=false;
    int result=ext4_trans_start(mp);
    if (result) return result;
    struct ext4_inode_ref ref;
    result=ext4_fs_get_inode_ref(&mp->fs,change->file->inode,&ref);
    if (!result) {
        struct ext4_timestamp now;
        if (!mp->clock || !mp->clock(&now)) result=EIO;
        else {
            uint32_t flags=ext4_inode_get_flags(ref.inode);
            ext4_inode_set_flags(ref.inode,(flags&~change->mask)|(change->flags&change->mask));
            ext4_time_store(&ref,offsetof(struct ext4_inode,change_inode_time),
                offsetof(struct ext4_inode,ctime_extra),now);
            change->ctime=ext4_time_load(&ref,offsetof(struct ext4_inode,change_inode_time),
                offsetof(struct ext4_inode,ctime_extra));
            ref.dirty=true;
            change->changed=true;
        }
        result=ext4_result(result,ext4_fs_put_inode_ref(&ref));
    }
    return ext4_trans_finish(mp,result);
}

int ext4_file_set_flags(ext4_file *file,uint32_t mask,uint32_t flags,
    struct ext4_timestamp *ctime,bool *published)
{
    *published=false;
    if (!file || !file->mp || !file->mp->mounted) return EINVAL;
    struct ext4_mountpoint *mp=file->mp;
    struct ext4_flags_change change={.file=file,.mask=mask,.flags=flags};
    EXT4_MP_LOCK(mp);
    int result=mp->fs.read_only ? EROFS : ext4_metadata_execute_locked(mp,ext4_apply_flags,&change);
    /* A nonjournal home-write error, or a failed synchronous journal submit,
     * can retain the changed cache image. Grouped preparation errors undo it. */
    if (change.changed && (!result || (!mp->fs.jbd_journal ||
        (!mp->fs.jbd_journal->grouped && !ext4_file_rolled_back(file,result))))) {
        *published=true;
        *ctime=change.ctime;
    }
    EXT4_MP_UNLOCK(mp);
    return result;
}

int ext4_file_set_mode(ext4_file *file, uint32_t mode)
{
    if (!file || !file->mp || !file->mp->mounted) return EINVAL;
    struct ext4_mountpoint *mp = file->mp;
    struct ext4_inode_ref ref;
    int r = EOK;
    EXT4_MP_LOCK(mp);
    if (mp->fs.read_only) { r = EROFS; goto Unlock; }
    r = ext4_trans_start(mp);
    if (r != EOK) goto Unlock;
    r = ext4_fs_get_inode_ref(&mp->fs, file->inode, &ref);
    if (r == EOK) {
        uint32_t previous = ext4_inode_get_mode(ext4_fs_super(&mp->fs), ref.inode);
        ext4_inode_set_mode(ext4_fs_super(&mp->fs), ref.inode,
            (previous & ~0xfffU) | (mode & 0xfffU));
        ref.dirty = true;
        ext4_touch_inode(mp, &ref, EXT4_TIME_CTIME, file->time_owner);
        r = ext4_fs_put_inode_ref(&ref);
    }
    r = ext4_trans_finish(mp, r);
Unlock:
    if (r != EOK && ext4_current_trans(&mp->fs) && !ext4_current_trans(&mp->fs)->error)
        ext4_current_trans(&mp->fs)->error = r;
    EXT4_MP_UNLOCK(mp);
    return r;
}

static int ext4_file_set_owner_once(ext4_file *file, uint32_t uid, uint32_t gid, uint32_t privilege_mode)
{
    if (!file || !file->mp || !file->mp->mounted) return EINVAL;
    struct ext4_mountpoint *mp = file->mp;
    struct ext4_inode_ref ref;
    int r = EOK;
    EXT4_MP_LOCK(mp);
    if (mp->fs.read_only) { r = EROFS; goto Unlock; }
    r = ext4_trans_start(mp);
    if (r != EOK) goto Unlock;
    r = ext4_fs_get_inode_ref(&mp->fs, file->inode, &ref);
    if (r == EOK) {
        uint32_t mode = ext4_inode_get_mode(ext4_fs_super(&mp->fs), ref.inode);
        if ((mode & 0170000U) != 0040000U) {
            if (privilege_mode!=UINT32_MAX) mode=privilege_mode;
            else {mode &= ~04000U;if (mode & 0010U) mode &= ~02000U;}
            /* 同一 undo 边界删除文件 capability，失败不能留下半次 chown。 */
            r = ext4_xattr_remove(&ref, 6, "capability", 10);
            if (r == ENODATA) r = EOK;
        }
        if (r == EOK) {
            if (uid != UINT32_MAX) ext4_inode_set_uid(ref.inode, uid);
            if (gid != UINT32_MAX) ext4_inode_set_gid(ref.inode, gid);
            ext4_inode_set_mode(ext4_fs_super(&mp->fs), ref.inode, mode);
            ref.dirty = true;
            ext4_touch_inode(mp, &ref, EXT4_TIME_CTIME, file->time_owner);
        }
        r = ext4_result(r, ext4_fs_put_inode_ref(&ref));
    }
    r = ext4_trans_finish(mp, r);
Unlock:
    if (r != EOK && ext4_current_trans(&mp->fs) && !ext4_current_trans(&mp->fs)->error)
        ext4_current_trans(&mp->fs)->error = r;
    EXT4_MP_UNLOCK(mp);
    return r;
}

struct ext4_owner_change { ext4_file *file;uint32_t uid,gid,mode; };
static int ext4_apply_owner(void *context)
{
    struct ext4_owner_change *request=context;
    return ext4_file_set_owner_once(request->file,request->uid,request->gid,request->mode);
}
int ext4_file_set_owner(ext4_file *file,uint32_t uid,uint32_t gid, uint32_t privilege_mode)
{
    if (!file || !file->mp || !file->mp->mounted) return EINVAL;
    struct ext4_mountpoint *mp=file->mp;
    struct ext4_owner_change request={file,uid,gid,privilege_mode};
    EXT4_MP_LOCK(mp);
    int result=ext4_metadata_execute_locked(mp,ext4_apply_owner,&request);
    EXT4_MP_UNLOCK(mp);
    return result;
}

int ext4_file_capability(ext4_file *file,void *data,size_t capacity,size_t *size)
{
    if (!file || !file->mp || !size) return EINVAL;
    struct ext4_mountpoint *mp=file->mp;
    EXT4_MP_LOCK(mp);
    struct ext4_inode_ref ref;
    int result=ext4_fs_get_inode_ref_read(&mp->fs,file->inode,&ref);
    if (!result) {
        result=ext4_xattr_get(&ref,6,"capability",10,data,capacity,size);
        result=ext4_result(result,ext4_fs_put_inode_ref(&ref));
    }
    EXT4_MP_UNLOCK(mp);
    return result;
}
struct ext4_privilege_change { ext4_file *file; uint32_t mode; };
static int ext4_remove_privileges(void *context)
{
    struct ext4_privilege_change *change=context;
    struct ext4_mountpoint *mp=change->file->mp;
    int result=ext4_trans_start(mp);
    if (result) return result;
    struct ext4_inode_ref ref;
    result=ext4_fs_get_inode_ref(&mp->fs,change->file->inode,&ref);
    if (!result) {
        result=ext4_xattr_remove(&ref,6,"capability",10);
        if (result==ENODATA) result=EOK;
        if (!result) {
            ext4_inode_set_mode(ext4_fs_super(&mp->fs),ref.inode,change->mode);
            ref.dirty=true;
            ext4_touch_inode(mp,&ref,EXT4_TIME_CTIME,change->file->time_owner);
        }
        result=ext4_result(result,ext4_fs_put_inode_ref(&ref));
    }
    return ext4_trans_finish(mp,result);
}
int ext4_file_remove_privileges(ext4_file *file,uint32_t mode)
{
    if (!file || !file->mp) return EINVAL;
    struct ext4_privilege_change change={file,mode};
    struct ext4_mountpoint *mp=file->mp;
    EXT4_MP_LOCK(mp);
    int result=ext4_metadata_execute_locked(mp,ext4_remove_privileges,&change);
    EXT4_MP_UNLOCK(mp);
    return result;
}

/********************************FILE OPERATIONS*****************************/

static int ext4_path_check(const char *path, bool *is_goal)
{
	int i;

	for (i = 0; i <= EXT4_DIRECTORY_FILENAME_LEN; ++i) {

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
	uint64_t size = ext4_inode_get_size(ext4_fs_super(&mp->fs), ref.inode);
    /* Preallocation can extend mappings past unchanged i_size. A truncate
     * retires those reservations even when its requested size is identical. */
    bool tail=new_size<=size;
	/* The public operation owns the transaction. Never commit an enclosing
	 * unlink/rename halfway through a truncate. */
	while (r == EOK && size > new_size) {
		size = size - new_size > CONFIG_MAX_TRUNCATE_SIZE
		       ? size - CONFIG_MAX_TRUNCATE_SIZE : new_size;
		r = ext4_fs_truncate_inode(&ref, size);
	}
    /* legacy 的已初始化指针也可能来自写入准备，而逻辑 EOF 尚未推进。
     * 按实际映射回收，不能以 old_size==new_size 判断已经没有尾部 owner。 */
    bool done=false;
    while (r==EOK && tail && !done) r=ext4_inode_truncate_step(&ref,&done);
	int cleanup = ext4_fs_put_inode_ref(&ref);
	return r == EOK ? cleanup : r;
}

/* Finish an accepted truncate or unlink in bounded transactions. Live unlinked
 * inodes keep their orphan record until their last VFS owner calls free. */
static int ext4_reclaim_orphan(struct ext4_mountpoint *mp, uint32_t index,
			       bool release_inode)
{
	bool done = false;
	bool expand = release_inode;
	int r;
	if (!mp->fs.jbd_journal) return EOK;
	if (jbd_journal_error(mp->fs.jbd_journal)) return jbd_journal_error(mp->fs.jbd_journal);
	if (ext4_operation_depth(&mp->fs)) return EBUSY;
	if (!release_inode) {
		/* 活的无链接 owner 本身不代表有缩短映射需要回收。 */
		bool present;
		r = ext4_orphan_truncate_pending(&mp->fs, index, &present);
		if (r != EOK || !present) return r;
	}
	/* 意图与回收按同一 journal FIFO 提交；同组则原子合并。已释放的
	 * 块/inode由quarantine保留到checkpoint，只有实际预算或复用压力
	 * 才在trans_start等待，普通操作不排空整个挂载点。 */
	do {
		done = false;
		bool expanded = false;
		struct ext4_inode_ref ref;
		r = ext4_trans_start(mp);
		if (r != EOK) return r;
		r = ext4_fs_get_inode_ref(&mp->fs, index, &ref);
        if (r != EOK) goto FinishWindow;
        /* 映射释放会取得组资格，先保留本窗口结束时要修改的 orphan 记录。 */
        r = ext4_orphan_prepare_reclaim(&ref);
        if (r != EOK) {
            r = ext4_result(r, ext4_fs_put_inode_ref(&ref));
            goto FinishWindow;
        }
		bool dead = !ext4_inode_get_links_cnt(ref.inode);
		if (dead && release_inode) {
			ext4_inode_set_size(ref.inode, 0);
			ref.dirty = true;
		}
		/* The truncate/unlink already passed permission checks. A surviving
		 * descriptor may since have set immutable or append-only. */
		r = ext4_inode_truncate_orphan_step(&ref, &done, expand ? &expanded : NULL);
        if (r == EOK && done) r = ext4_orphan_finish_reclaim(&ref, release_inode);
		r = ext4_result(r, ext4_fs_put_inode_ref(&ref));
FinishWindow:
		r = ext4_trans_finish(mp, r);
		if (r == EAGAIN && ext4_operation_resource_retry(&mp->fs)) {
			done = false;
			continue;
		}
		if (expanded && (r == ENOMEM || r == ENOSPC) &&
		    ext4_operation_aborted(&mp->fs) && !jbd_journal_error(mp->fs.jbd_journal)) {
			expand = false;
			done = false;
			continue;
		}
        if (ext4_operation_retry_reclaim(mp, &r)) { done = false; continue; }
		if (r != EOK) return r;
	} while (!done);
	return EOK;
}

/* A persisted shrink can leave old mappings past i_size until its orphan is
 * reclaimed. Remove those mappings before a later grow can expose them. An
 * ordinary call uses bounded transactions; an explicitly grouped operation
 * must retain its caller's atomic boundary and rollback on resource failure.
 * Completed linked truncates can leave that same operation's orphan set;
 * live unlinked records remain until their last VFS owner releases them. */
static int ext4_prepare_growth(ext4_file *file, uint64_t visible_end)
{
	struct ext4_mountpoint *mp = file->mp;
	struct ext4_fs *fs = &mp->fs;
	struct ext4_inode_ref ref;
	bool present, done = false;
	int r;
	if (!fs->jbd_journal) return EOK;
	if (jbd_journal_error(fs->jbd_journal)) return jbd_journal_error(fs->jbd_journal);
	if (ext4_current_operation(fs) && ext4_current_operation(fs)->error) return ext4_current_operation(fs)->error;
	if (ext4_current_trans(fs) && ext4_current_trans(fs)->error) return ext4_current_trans(fs)->error;
	r = ext4_orphan_truncate_pending(fs, file->inode, &present);
	if (r != EOK || !present) goto Finish;
	r = ext4_fs_get_inode_ref(fs, file->inode, &ref);
	if (r != EOK) goto Finish;
	if (visible_end <= ext4_inode_get_size(ext4_fs_super(fs), ref.inode)) {
		r = ext4_fs_put_inode_ref(&ref);
		goto Finish;
	}
	if (!ext4_operation_depth(&mp->fs)) {
		r = ext4_fs_put_inode_ref(&ref);
		if (r == EOK) r = ext4_reclaim_orphan(mp, file->inode, false);
	} else {
        r = ext4_orphan_prepare_reclaim(&ref);
		while (r == EOK && !done) {
			r = ext4_inode_truncate_step(&ref, &done);
		}
        if (r == EOK) r = ext4_orphan_finish_reclaim(&ref, false);
		r = ext4_result(r, ext4_fs_put_inode_ref(&ref));
	}
Finish:
	/* This preflight precedes the operation's nested begin, so latch errors
	 * here too: the caller must not commit a partly reclaimed outer group. */
	if (r != EOK && ext4_current_trans(fs) && !ext4_current_trans(fs)->error)
		ext4_current_trans(fs)->error = r;
	return r;
}

static int ext4_orphan_recover_locked(const char *mount_point)
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
	if (r == EOK && (ext4_fs_ro_flags(&mp->fs) & EXT4_FRO_COM_ORPHAN_PRESENT)) {
		r = ext4_trans_start(mp);
		if (r == EOK) {
			r = ext4_orphan_set_present(&mp->fs, false);
			r = ext4_trans_finish(mp, r);
		}
	}
	return r;
}

int ext4_orphan_recover(const char *mount_point)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	EXT4_MP_LOCK(mp);
	int result = ext4_orphan_recover_locked(mount_point);
	EXT4_MP_UNLOCK(mp);
	return result;
}

static int ext4_trunc_dir(struct ext4_mountpoint *mp,
			  struct ext4_inode_ref *parent,
			  struct ext4_inode_ref *dir)
{
	int r = EOK;
	bool is_dir = ext4_inode_is_type(ext4_fs_super(&mp->fs), dir->inode,
			EXT4_INODE_MODE_DIRECTORY);
	uint32_t block_size = ext4_sb_get_block_size(ext4_fs_super(&mp->fs));
	if (!is_dir)
		return EINVAL;

#if CONFIG_DIR_INDEX_ENABLE
	/* Initialize directory index if supported */
	if (ext4_inode_has_flag(dir->inode, EXT4_INODE_FLAG_INDEX)) {
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

static int ext4_alloc_linked_child(struct ext4_mountpoint *mp,
    struct ext4_inode_ref *parent, const char *name, uint32_t length, int type,
    struct ext4_inode_ref *child, bool *read_parent)
{
    struct ext4_fs *fs = parent->fs;
    struct ext4_sblock *sb = ext4_fs_super(fs);
    int result = ext4_fs_alloc_inode(fs, child, type, parent->index, name, length);
    if (result) return result;

    ext4_fs_inode_blocks_init(fs, child);
    if (ext4_inode_has_flag(parent->inode, EXT4_INODE_FLAG_NODUMP))
        ext4_inode_set_flag(child->inode, EXT4_INODE_FLAG_NODUMP);
    if (ext4_inode_get_mode(sb, parent->inode) & 02000U) {
        ext4_inode_set_gid(child->inode, ext4_inode_get_gid(parent->inode));
        if (type == EXT4_DE_DIR)
            ext4_inode_set_mode(sb, child->inode, ext4_inode_get_mode(sb, child->inode) | 02000U);
    }
    ext4_initialize_inode_times(mp,child);
    result = ext4_link(mp, parent, child, name, length, false, read_parent);
    if (result) {
        ext4_fs_free_inode(child);
        child->dirty = false;
        result = ext4_result(result, ext4_fs_put_inode_ref(child));
    }
    return result;
}

/*
 * NOTICE: if filetype is equal to EXT4_DIRENTRY_UNKNOWN,
 * any filetype of the target dir entry will be accepted.
 */
static int ext4_open_relative(ext4_file *f, struct ext4_mountpoint *mp,
    uint32_t origin, const char *path, int flags, int ftype,
    uint32_t *parent_inode, uint32_t *name_off, struct ext4_inode_ref *opened,
    struct ext4_inode_ref *opened_parent)
{
	bool is_goal = false;
	bool created_goal = false;
	uint32_t imode = EXT4_INODE_MODE_DIRECTORY;
	uint32_t next_inode;

	int r;
	int len;
	struct ext4_dir_search_result result;
	struct ext4_inode_ref ref;

	if (f) f->mp = 0;
    if (opened_parent) *opened_parent = (struct ext4_inode_ref){0};

	if (!mp)
		return ENOENT;

	struct ext4_fs *const fs = &mp->fs;
	struct ext4_sblock *const sb = ext4_fs_super(&mp->fs);
	if (fs->super_replay_required) return EUCLEAN;

	if (fs->read_only && flags & O_CREAT)
		return EROFS;

	if (f) f->flags = flags;

    if (name_off) *name_off = 0;
    r = ext4_fs_get_inode_ref(fs, origin, &ref);
	if (r != EOK)
		return r;

    if (!ext4_inode_is_type(sb, ref.inode, EXT4_INODE_MODE_DIRECTORY) ||
        !ext4_inode_get_links_cnt(ref.inode)) {
        r = ext4_inode_get_links_cnt(ref.inode) ? ENOTDIR : ENOENT;
        return ext4_result(r, ext4_fs_put_inode_ref(&ref));
    }
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

		r = ext4_dir_find_entry(&result, &ref, path, len, false);
		if (r != EOK) {

			/*Destroy last result*/
			r = ext4_result(r, ext4_dir_destroy_result(&ref, &result));
			if (r != ENOENT)
				break;

			if (!(flags & O_CREAT))
				break;

			struct ext4_inode_ref child_ref;
			r = ext4_alloc_linked_child(mp, &ref, path, len,
				is_goal ? ftype : EXT4_DE_DIR, &child_ref, NULL);
			if (r != EOK)
				break;

            if (parent_inode) *parent_inode = ref.index;
            imode = ext4_inode_type(sb, child_ref.inode);
            if (is_goal && opened_parent) *opened_parent = ref;
            else {
                r = ext4_fs_put_inode_ref(&ref);
                if (r != EOK) {
                    r = ext4_result(r, ext4_fs_put_inode_ref(&child_ref));
                    break;
                }
            }
            /* The allocated inode is the object just linked. Transfer its
             * actual reference to the next path step or final initializer. */
            ref = child_ref;
            if (is_goal) { created_goal = true; break; }
            path += len + 1;
            if (name_off) *name_off += len + 1;
            continue;
		}

        if (is_goal && (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
            r = ext4_result(EEXIST, ext4_dir_destroy_result(&ref, &result));
            break;
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

        if (is_goal && opened_parent) {
            *opened_parent = ref;
            ref = (struct ext4_inode_ref){0};
        } else {
            r = ext4_fs_put_inode_ref(&ref);
            if (r != EOK) break;
        }

		r = ext4_fs_get_inode_ref(fs, next_inode, &ref);
		if (r != EOK)
			break;

		if (is_goal)
			break;

		path += len + 1;

		if (name_off)
			*name_off += len + 1;
	}

    if (r != EOK) goto PutRefs;
    /* A root lookup has one identity serving as both parent and target;
     * each output still owns its own actual buffer reference. */
    if (opened_parent && !opened_parent->block.data) {
        r = ext4_fs_get_inode_ref(fs, ref.index, opened_parent);
        if (r != EOK) goto PutRefs;
    }

	if (is_goal) {

        /* The new inode already satisfies O_TRUNC; existing objects still
         * need the usual mapping retirement and orphan boundary. */
		if ((flags & O_TRUNC) && imode == EXT4_INODE_MODE_FILE && !created_goal) {
			r = ext4_operation_record_inode(fs, ref.index, true);
			if (r != EOK) goto PutRefs;
			if (mp->fs.jbd_journal) {
				r = ext4_orphan_add(&ref);
                if (r == EOK) r = ext4_orphan_mark_truncate(&ref,true);
				if (r == EOK) {
					ext4_inode_set_size(ref.inode, 0);
					ref.dirty = true;
				}
			} else if (!mp->fs.jbd_journal)
				r = ext4_trunc_inode(mp, ref.index, 0);
            if (r != EOK) goto PutRefs;
        }

        if (f) {
		f->mp = mp;
		f->fsize = ext4_inode_get_size(sb, ref.inode);
		f->fmax = ext4_inode_max_size(fs, ref.inode);
		f->inode = ref.index;
		f->fpos = 0;
		f->time_owner = NULL;
		f->delalloc = NULL;

		if (flags & O_APPEND)
			f->fpos = f->fsize;
        }
	}

    if (opened) {
        *opened = ref;
        return EOK;
    }
    r = ext4_fs_put_inode_ref(&ref);
    if (r != EOK && opened_parent) {
        r = ext4_result(r, ext4_fs_put_inode_ref(opened_parent));
        *opened_parent = (struct ext4_inode_ref){0};
    }
    return r;
PutRefs:
    if (ref.block.data) r = ext4_result(r, ext4_fs_put_inode_ref(&ref));
    if (opened_parent && opened_parent->block.data) {
        r = ext4_result(r, ext4_fs_put_inode_ref(opened_parent));
        *opened_parent = (struct ext4_inode_ref){0};
    }
    return r;
}

static int ext4_generic_open2(ext4_file *f, const char *path, int flags,
    int type, uint32_t *parent_inode, uint32_t *name_off)
{
    struct ext4_mountpoint *mp = ext4_get_mount(path);
    if (!mp) return ENOENT;
    size_t prefix = strlen(mp->name);
    int result = ext4_open_relative(f, mp, EXT4_INODE_ROOT_INDEX,
        path + prefix, flags, type, parent_inode, name_off, NULL, NULL);
    if (name_off) *name_off += prefix;
    return result;
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
	    !ext4_operation_depth(&mp->fs))
		r = ext4_reclaim_orphan(mp, f->inode, false);
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
	struct ext4_sblock *const sb = ext4_fs_super(&mp->fs);

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

		r = ext4_dir_find_entry(&result, &ref, path, len, false);
		if (r != EOK) {

			/*Destroy last result*/
			r = ext4_result(r, ext4_dir_destroy_result(&ref, &result));

			if (r != ENOENT || !is_goal)
				break;

			/*Link with root dir.*/
			r = ext4_link(mp, &ref, child_ref, path, len, rename, NULL);
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

	if (ext4_inode_is_type(ext4_fs_super(&mp->fs), child_ref->inode,
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
	if (ext4_inode_is_type(ext4_fs_super(&mp->fs), child_ref.inode,
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

/* Rename operates on stable directory identities; pathname reconstruction is
 * deliberately confined to the legacy API below. Reference: Linux f4cdf7ca9a1f
 * fs/namei.c:vfs_rename and fs/ext4/namei.c:ext4_rename. */
static bool ext4_rename_name_valid(const char *name, uint32_t length)
{
	if (!name || !length || length > EXT4_DIRECTORY_FILENAME_LEN ||
	    ext4_is_dots((const uint8_t *)name, length)) return false;
	for (uint32_t i = 0; i < length; i++)
		if (!name[i] || name[i] == '/') return false;
	return true;
}

static int ext4_rename_lookup(struct ext4_inode_ref *parent, const char *name,
			     uint32_t length, uint32_t *inode)
{
	struct ext4_dir_search_result result = {0};
	int r = ext4_dir_find_entry(&result, parent, name, length, false);
	if (r == EOK) *inode = ext4_dir_en_get_inode(result.dentry);
	return ext4_result(r, ext4_dir_destroy_result(parent, &result));
}

static int ext4_link_child_mode(const char *mount_point, uint32_t source_inode,
                   uint32_t parent_inode, const char *name, uint32_t length, bool tmpfile)
{
    if (!mount_point) return EINVAL;
    struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
    if (!mp) return ENOENT;
    struct ext4_fs *fs = &mp->fs;
    struct ext4_inode_ref source = {0}, parent = {0};
    uint32_t existing = 0;
    int r;
    EXT4_MP_LOCK(mp);
    if (!source_inode || !parent_inode || !ext4_rename_name_valid(name, length)) {
        r = EINVAL; goto Unlock;
    }
    if (fs->read_only) { r = EROFS; goto Unlock; }
    if (!fs->jbd_journal) { r = ENOTSUP; goto Unlock; }
    r = ext4_trans_start(mp);
    if (r != EOK) goto Unlock;
    r = ext4_fs_get_inode_ref(fs, source_inode, &source);
    if (r != EOK) goto Put;
    if (ext4_inode_is_type(ext4_fs_super(fs), source.inode, EXT4_INODE_MODE_DIRECTORY)) {
        r = EPERM; goto Put;
    }
    /* Only the VFS-owned, unpublished tmpfile entry point may link zero links. */
    bool unpublished = !ext4_inode_get_links_cnt(source.inode);
    if (unpublished != tmpfile) { r = ENOENT; goto Put; }
    if (unpublished) {
        bool present = false;
        r = ext4_orphan_contains(fs, source_inode, &present);
        if (r != EOK || !present) { if (!r) r = EUCLEAN; goto Put; }
        r = ext4_orphan_prepare_change(&source);
        if (r != EOK) goto Put;
    }
    if (ext4_inode_get_links_cnt(source.inode) >= EXT4_LINK_MAX) { r = EMLINK; goto Put; }
    r = ext4_fs_get_inode_ref(fs, parent_inode, &parent);
    if (r != EOK) goto Put;
    if (!ext4_inode_is_type(ext4_fs_super(fs), parent.inode, EXT4_INODE_MODE_DIRECTORY)) {
        r = ENOTDIR; goto Put;
    }
    if (!ext4_inode_get_links_cnt(parent.inode)) { r = ENOENT; goto Put; }
    r = ext4_rename_lookup(&parent, name, length, &existing);
    if (r == EOK) { r = EEXIST; goto Put; }
    if (r != ENOENT) goto Put;
    r = ext4_dir_add_entry(&parent, name, length, &source);
    if (r != EOK) goto Put;
    ext4_fs_inode_links_count_inc(&source);
    source.dirty = true;
    if (unpublished) {
        r = ext4_orphan_remove(&source);
        if (r != EOK) goto Put;
        ext4_inode_set_del_time(source.inode, 0);
    }
    ext4_touch_inode(mp, &source, EXT4_TIME_CTIME, NULL);
    ext4_touch_inode(mp, &parent, EXT4_TIME_MTIME | EXT4_TIME_CTIME, NULL);
Put:
    if (parent.block.data) r = ext4_result(r, ext4_fs_put_inode_ref(&parent));
    if (source.block.data) r = ext4_result(r, ext4_fs_put_inode_ref(&source));
    r = ext4_trans_finish(mp, r);
Unlock:
    if (r != EOK && ext4_current_trans(fs) && !ext4_current_trans(fs)->error)
        ext4_current_trans(fs)->error = r;
    EXT4_MP_UNLOCK(mp);
    return r;
}

int ext4_link_child(const char *mount, uint32_t source, uint32_t parent,
                    const char *name, uint32_t length)
{ return ext4_link_child_mode(mount, source, parent, name, length, false); }

int ext4_tmpfile_link_child(const char *mount, uint32_t source, uint32_t parent,
                            const char *name, uint32_t length)
{ return ext4_link_child_mode(mount, source, parent, name, length, true); }

static int ext4_rename_ancestry(struct ext4_fs *fs, uint32_t source, uint32_t current)
{
	/* Brent's cycle detector needs no allocation and bounds corrupt parent
	 * chains by their actual cycle length rather than the filesystem size. */
	uint32_t anchor = current;
	uint64_t power = 1, length = 0;
	for (;;) {
		if (current == source) return EINVAL;
		struct ext4_inode_ref ref;
		int r = ext4_fs_get_inode_ref_read(fs, current, &ref);
		if (r != EOK) return r;
		uint32_t parent = 0;
		if (!ext4_inode_get_links_cnt(ref.inode)) r = EUCLEAN;
		else r = ext4_dir_parent_inode(&ref, &parent);
		r = ext4_result(r, ext4_fs_put_inode_ref(&ref));
		if (r != EOK) return r;
		if (current == EXT4_INODE_ROOT_INDEX)
			return parent == current ? EOK : EUCLEAN;
		current = parent;
		if (current == anchor) return EUCLEAN;
		if (++length == power) {
			anchor = current;
			power *= 2;
			length = 0;
		}
	}
}

static int ext4_rename_replace_found(struct ext4_inode_ref *parent,
		const char *name, uint32_t length, struct ext4_inode_ref *source,
		uint32_t victim, struct ext4_dir_search_result *found)
{
	if (ext4_dir_en_get_inode(found->dentry) != victim) return EUCLEAN;
	ext4_dir_write_entry(ext4_fs_super(parent->fs), found->dentry,
	    ext4_dir_en_get_entry_len(found->dentry), source, name, length);
	return ext4_dir_mark_dirty(parent, &found->block);
}

static int ext4_rename_replace(struct ext4_inode_ref *parent, const char *name,
		uint32_t length, struct ext4_inode_ref *source, uint32_t victim,
		struct ext4_dir_search_result *target, const struct ext4_block *source_leaf)
{
	int r = ext4_operation_record_inode(parent->fs, parent->index, true);
	if (!r) r = ext4_operation_record_inode(parent->fs, source->index, false);
	if (r != EOK) return r;
	if (!target->block.lb_id) {
		r = ext4_dir_find_entry(target, parent, name, length, true);
	} else if (target->block.payload != source_leaf->payload) {
		/* The parent namespace stays locked across discovery and upgrade.
		 * Use the same slot in the transaction's private leaf. */
		size_t offset = (uint8_t *)target->dentry - target->block.data;
		r = jbd_trans_get_write_access(ext4_current_trans(parent->fs), &target->block);
		if (!r) target->dentry = (void *)(target->block.data + offset);
	}
	if (!r) r = ext4_rename_replace_found(parent, name, length, source, victim, target);
	return ext4_result(r, ext4_dir_destroy_result(parent, target));
}

static bool ext4_rename_can_add_directory(struct ext4_inode_ref *parent)
{
	struct ext4_sblock *sb = ext4_fs_super(parent->fs);
	return ext4_inode_get_links_cnt(parent->inode) < EXT4_LINK_MAX ||
	    (ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_DIR_NLINK) &&
	     ext4_inode_has_flag(parent->inode, EXT4_INODE_FLAG_INDEX));
}

static void ext4_latch_nojournal_error(struct ext4_fs *fs, int error);

int ext4_rename_child(const char *mount_point,
	uint32_t old_parent, const char *old_name, uint32_t old_len,
	uint32_t new_parent, const char *new_name, uint32_t new_len,
	unsigned flags, struct ext4_rename_result *result)
{
	if (!mount_point) return EINVAL;
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	struct ext4_fs *fs = &mp->fs;
	struct ext4_sblock *sb = ext4_fs_super(fs);
	struct ext4_inode_ref old = {0}, new = {0}, source = {0}, victim = {0};
	struct ext4_inode_ref *destination = old_parent == new_parent ? &old : &new;
	struct ext4_rename_result output = {0};
	struct ext4_dir_search_result found = {0}, target = {0};
	uint32_t source_ino = 0, victim_ino = 0, parent;
	bool is_dir = false, target_dir = false;
	bool exchange = flags == EXT4_RENAME_EXCHANGE;
	bool ordered = fs->jbd_journal && fs->os_locks && fs->os_locks->resource_acquire;
	bool publishing = false;
	int r;
	EXT4_MP_LOCK(mp);
	if (!result || flags > EXT4_RENAME_EXCHANGE || !old_parent || !new_parent ||
	    !ext4_rename_name_valid(old_name, old_len) ||
	    !ext4_rename_name_valid(new_name, new_len)) { r = EINVAL; goto Unlock; }
	if (fs->read_only) { r = EROFS; goto Unlock; }
	if (!fs->jbd_journal && (!fs->ext2_format || exchange)) { r = ENOTSUP; goto Unlock; }
	r = ext4_trans_start(mp);
	if (r != EOK) goto Unlock;
	/* Without per-object resources the caller's serial qualification spans
	 * discovery and modification; create writable records on the first borrow. */
	r = ordered ? ext4_fs_get_inode_ref_read(fs, old_parent, &old) :
	              ext4_fs_get_inode_ref(fs, old_parent, &old);
	if (r != EOK) goto Put;
	if (destination == &new) {
		r = ordered ? ext4_fs_get_inode_ref_read(fs, new_parent, &new) :
		              ext4_fs_get_inode_ref(fs, new_parent, &new);
		if (r != EOK) goto Put;
	}
	if (!ext4_inode_is_type(sb, old.inode, EXT4_INODE_MODE_DIRECTORY) ||
	    !ext4_inode_is_type(sb, destination->inode, EXT4_INODE_MODE_DIRECTORY)) {
		r = ENOTDIR; goto Put;
	}
	if (!ext4_inode_get_links_cnt(old.inode) ||
	    !ext4_inode_get_links_cnt(destination->inode)) { r = ENOENT; goto Put; }
	r = ext4_dir_find_entry(&found, &old, old_name, old_len, true);
	if (r != EOK) goto Put;
	source_ino = ext4_dir_en_get_inode(found.dentry);
	if (fs->jbd_journal) {
		r = ext4_dir_find_entry(&target, destination, new_name, new_len, false);
		if (!r) victim_ino = ext4_dir_en_get_inode(target.dentry);
	} else r = ext4_rename_lookup(destination, new_name, new_len, &victim_ino);
	if (r != EOK && r != ENOENT) goto Put;
	if (victim_ino && flags == EXT4_RENAME_NOREPLACE) { r = EEXIST; goto Put; }
	if (exchange && !victim_ino) { r = ENOENT; goto Put; }
	if (source_ino == EXT4_INODE_ROOT_INDEX || source_ino == old_parent ||
	    victim_ino == old_parent || victim_ino == new_parent) { r = EINVAL; goto Put; }
	r = ordered ? ext4_fs_get_inode_ref_read(fs, source_ino, &source) :
	              ext4_fs_get_inode_ref(fs, source_ino, &source);
	if (r != EOK) goto Put;
	if (!ext4_inode_get_links_cnt(source.inode)) { r = EUCLEAN; goto Put; }
	if (source_ino == victim_ino) { r = EOK; goto Put; }
	is_dir = ext4_inode_is_type(sb, source.inode, EXT4_INODE_MODE_DIRECTORY);
	if (victim_ino) {
		r = ordered ? ext4_fs_get_inode_ref_read(fs, victim_ino, &victim) :
		              ext4_fs_get_inode_ref(fs, victim_ino, &victim);
		if (r != EOK) goto Put;
		if (!ext4_inode_get_links_cnt(victim.inode)) { r = EUCLEAN; goto Put; }
		target_dir = ext4_inode_is_type(sb, victim.inode, EXT4_INODE_MODE_DIRECTORY);
		if (!exchange && is_dir != target_dir) { r = is_dir ? ENOTDIR : EISDIR; goto Put; }
		if (target_dir) {
			r = ext4_dir_parent_inode(&victim, &parent);
			if (r == EOK && parent != new_parent) r = EUCLEAN;
			if (r == EOK && !exchange) {
				bool empty;
				r = ext4_dir_check_empty(&victim, &empty);
				if (r == EOK && !empty) r = ENOTEMPTY;
			} else if (r == EOK && old_parent != new_parent) {
				r = ext4_rename_ancestry(fs, victim_ino, old_parent);
				if (r == EOK && !is_dir && !ext4_rename_can_add_directory(&old))
					r = EMLINK;
			}
			if (r != EOK) goto Put;
		}
	}
	if (is_dir) {
		r = ext4_dir_parent_inode(&source, &parent);
		if (r == EOK && parent != old_parent) r = EUCLEAN;
		if (r == EOK && old_parent != new_parent)
			r = ext4_rename_ancestry(fs, source_ino, new_parent);
		if (r != EOK) goto Put;
		if (old_parent != new_parent && (!victim_ino || (exchange && !target_dir)) &&
		    !ext4_rename_can_add_directory(destination)) {
			r = EMLINK; goto Put;
		}
	}
    /* Discovery borrows stable records without taking write qualifications.
     * Acquire the complete known set in inode order before creating private
     * records, including a source allocated below either parent. */
    if (ordered) {
        uint32_t inodes[] = {old_parent, new_parent, source_ino, victim_ino};
        for (unsigned i = 1; i < 4; i++) {
            uint32_t inode = inodes[i];
            unsigned j = i;
            while (j && inodes[j - 1] > inode) {
                inodes[j] = inodes[j - 1]; j--;
            }
            inodes[j] = inode;
        }
        for (unsigned i = 0; i < 4; i++) {
            if (!inodes[i] || (i && inodes[i] == inodes[i - 1])) continue;
            r = ext4_operation_require_resource(fs, EXT4_RESOURCE_INODE, inodes[i]);
            if (r != EOK) goto Put;
        }
        r = ext4_fs_inode_ref_write(&old);
        if (r == EOK && destination != &old) r = ext4_fs_inode_ref_write(destination);
        if (r == EOK) r = ext4_fs_inode_ref_write(&source);
        if (r == EOK && victim_ino) r = ext4_fs_inode_ref_write(&victim);
        if (r != EOK) goto Put;
        /* The caller retains namespace qualifications across resource waits:
         * names and parentage stay fixed. Only inode metadata can have advanced. */
        if (!ext4_inode_get_links_cnt(old.inode) ||
            !ext4_inode_get_links_cnt(destination->inode) ||
            !ext4_inode_get_links_cnt(source.inode) ||
            (victim_ino && !ext4_inode_get_links_cnt(victim.inode))) r = EUCLEAN;
        if (r != EOK) goto Put;
    }
	if (exchange) {
		r = ext4_operation_record_inode(fs, old_parent, true);
		if (r == EOK) r = ext4_operation_record_inode(fs, victim_ino, false);
		if (r == EOK) r = ext4_rename_replace(destination, new_name, new_len,
		    &source, victim_ino, &target, &found.block);
		if (r == EOK) r = ext4_rename_replace_found(&old, old_name, old_len,
		    &victim, source_ino, &found);
		if (r != EOK) goto Put;
		if (old_parent != new_parent) {
			if (is_dir) r = ext4_dir_reparent(&source, old_parent, new_parent);
			if (r == EOK && target_dir) r = ext4_dir_reparent(&victim, new_parent, old_parent);
			if (r != EOK) goto Put;
			if (is_dir != target_dir) {
				ext4_fs_inode_links_count_dec(is_dir ? &old : destination);
				ext4_fs_inode_links_count_inc(is_dir ? destination : &old);
				old.dirty = destination->dirty = true;
			}
		}
		ext4_touch_inode(mp, &victim, EXT4_TIME_CTIME, NULL);
		goto Touch;
	}
	uint64_t previous_size = fs->ext2_format ? ext4_inode_get_size(sb, destination->inode) : 0;
	if (victim_ino)
		r = ext4_rename_replace(destination, new_name, new_len, &source, victim_ino,
		    &target, &found.block);
	else r = ext4_dir_add_entry(destination, new_name, new_len, &source);
	if (fs->ext2_format) {
		/* An index split or an appended leaf can already have changed
		 * directory structure even if creating the new name failed. */
		publishing = !r || previous_size != ext4_inode_get_size(sb, destination->inode);
		if (!r) r = ext4_fs_stage_inode_ref(destination);
		if (!r) r = ext4_bcache_scope_flush(&mp->bc, &ext4_current_operation(fs)->scope);
		if (r != EOK) goto Put;
	}
	/* The held block borrows this transaction's private payload. Same-parent
	 * insertion may repack it or move the source to a new leaf; discard the
	 * old entry and predecessor and scan the current records before deleting. */
	if (r == EOK && old_parent == new_parent && (!victim_ino || fs->ext2_format)) {
		found.dentry = NULL;
		r = ext4_dir_find_in_block(&found.block, sb, old_len, old_name, &found);
		if (r == ENOENT) {
			r = ext4_dir_destroy_result(&old, &found);
			if (r == EOK) r = ext4_dir_find_entry(&found, &old, old_name, old_len, true);
		}
	}
	if (r == EOK && ext4_dir_en_get_inode(found.dentry) != source_ino) r = EUCLEAN;
	if (r == EOK) r = ext4_dir_remove_found(&old, &found);
	if (r == EOK && is_dir && old_parent != new_parent)
		r = ext4_dir_reparent(&source, old_parent, new_parent);
	if (r != EOK) goto Put;
	if (is_dir) {
		if (old_parent != new_parent || victim_ino) {
			ext4_fs_inode_links_count_dec(&old); old.dirty = true;
		}
		if (old_parent != new_parent && !victim_ino) {
			ext4_fs_inode_links_count_inc(destination); destination->dirty = true;
		}
	}
	if (victim_ino) {
		if (target_dir) ext4_inode_set_links_cnt(victim.inode, 0);
		else ext4_fs_inode_links_count_dec(&victim);
		victim.dirty = true;
		ext4_touch_inode(mp, &victim, EXT4_TIME_CTIME, NULL);
		output.replaced_inode = victim_ino;
		output.replaced_last_link = !ext4_inode_get_links_cnt(victim.inode);
	}
Touch:
	ext4_touch_inode(mp, &source, EXT4_TIME_CTIME, NULL);
	ext4_touch_inode(mp, &old, EXT4_TIME_MTIME | EXT4_TIME_CTIME, NULL);
	if (destination != &old)
		ext4_touch_inode(mp, destination, EXT4_TIME_MTIME | EXT4_TIME_CTIME, NULL);
	output.changed = true;
Put:
	if (target.block.lb_id) r = ext4_result(r, ext4_dir_destroy_result(destination, &target));
	if (found.block.lb_id) r = ext4_result(r, ext4_dir_destroy_result(&old, &found));
	if (victim.block.data) r = ext4_result(r, ext4_fs_put_inode_ref(&victim));
	if (source.block.data) r = ext4_result(r, ext4_fs_put_inode_ref(&source));
	if (new.block.data) r = ext4_result(r, ext4_fs_put_inode_ref(&new));
	if (old.block.data) r = ext4_result(r, ext4_fs_put_inode_ref(&old));
	/* Release the modified references before orphan enrollment so its initial
	 * index load sees completed inode checksums in the shared table blocks. */
	if (r == EOK && output.replaced_last_link && fs->jbd_journal) {
		r = ext4_fs_get_inode_ref(fs, victim_ino, &victim);
		if (r == EOK) {
			r = ext4_orphan_add(&victim);
			r = ext4_result(r, ext4_fs_put_inode_ref(&victim));
		}
	}
	if (r && publishing) ext4_latch_nojournal_error(fs, r);
	r = ext4_trans_finish(mp, r);
	if (r && publishing) ext4_latch_nojournal_error(fs, r);
	if (r == EOK) *result = output;
Unlock:
	if (r != EOK && ext4_current_trans(fs) && !ext4_current_trans(fs)->error)
		ext4_current_trans(fs)->error = r;
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

	/* The adapter retains this pointer for the mount lifetime. */
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

struct ext4_unlink_target {
    uint32_t inode;
    bool allow_reclaim, reclaim;
};

static int ext4_free_unlinked_inode(struct ext4_inode_ref *child);

/* The caller owns the victim inode. A single inline extent gives a fixed
 * journal footprint: directory block, up to two inode blocks, up to two
 * descriptors, the related bitmaps, and the superblock. */
static int ext4_unlink_prepare_reclaim(struct ext4_inode_ref *child __unused,
    struct ext4_unlink_target *target __unused, bool directory __unused)
{
#if CONFIG_EXTENT_ENABLE && CONFIG_EXTENTS_ENABLE
    struct ext4_fs *fs = child->fs;
    struct ext4_sblock *sb = ext4_fs_super(fs);
    if (!target || !target->allow_reclaim || !fs->jbd_journal ||
        !ext4_inode_has_flag(child->inode, EXT4_INODE_FLAG_EXTENTS) ||
        ext4_inode_get_file_acl(child->inode, sb) ||
        (!directory && (!ext4_inode_is_type(sb, child->inode, EXT4_INODE_MODE_FILE) ||
                        ext4_inode_get_links_cnt(child->inode) != 1))) return EOK;

    int result = ext4_orphan_check_user_inode(fs, child->index);
    if (result) return result;
    uint32_t inode_group = (child->index - 1) / ext4_get32(sb, inodes_per_group);
    uint32_t data_group = inode_group;
    if (!ext4_extent_small_root(child, 32, &data_group)) return EOK;
    target->reclaim = true;
    bool orphan;
    result = ext4_orphan_contains(fs, child->index, &orphan);
    if (result) return result;
    if (orphan) {
        target->reclaim = false;
        return EOK;
    }

    uint32_t first = inode_group < data_group ? inode_group : data_group;
    uint32_t second = inode_group < data_group ? data_group : inode_group;
    result = ext4_operation_require_resource(fs, EXT4_RESOURCE_BLOCK_GROUP, first);
    if (!result && first != second)
        result = ext4_operation_require_resource(fs, EXT4_RESOURCE_BLOCK_GROUP, second);
    return result;
#else
    return EOK;
#endif
}

static int ext4_unlink_dentry_core(struct ext4_mountpoint *mp, uint32_t parent_inode,
    const char *path, uint32_t *out_inode, bool *out_is_orphan, bool allow_directory,
    struct ext4_unlink_target *target)
{
	uint32_t child_inode;
	uint32_t name_off;
	bool is_goal;
	int r;
	int len;
	struct ext4_inode_ref child = {0};
	struct ext4_inode_ref parent = {0};
	struct ext4_dir_search_result found = {0};

	if (!mp)
		return ENOENT;

	if (mp->fs.read_only)
		return EROFS;

	EXT4_MP_LOCK(mp);
    bool private_refs = ext4_current_trans(&mp->fs) != NULL;
    bool reuse_dirent = parent_inode && (private_refs || mp->fs.ext2_format);
    bool publishing = false;
    size_t prefix = parent_inode ? 0 : strlen(mp->name);
    if (reuse_dirent) {
        /* The caller owns the private metadata operation and the parent
         * namespace. Resolve and keep its writable dirent through removal. */
        if (mp->fs.super_replay_required) {
            EXT4_MP_UNLOCK(mp);
            return EUCLEAN;
        }
        r = ext4_trans_start(mp);
        if (r != EOK) goto PutRefs;
        if (target) {
            uint32_t first = parent_inode < target->inode ? parent_inode : target->inode;
            uint32_t second = parent_inode < target->inode ? target->inode : parent_inode;
            r = ext4_operation_require_resource(&mp->fs, EXT4_RESOURCE_INODE, first);
            if (r == EOK && first != second)
                r = ext4_operation_require_resource(&mp->fs, EXT4_RESOURCE_INODE, second);
            if (r != EOK) goto Finish;
        }
        r = ext4_fs_get_inode_ref(&mp->fs, parent_inode, &parent);
        if (r != EOK) goto Finish;
        if (!ext4_inode_get_links_cnt(parent.inode) ||
            !ext4_inode_is_type(&mp->fs.sb, parent.inode, EXT4_INODE_MODE_DIRECTORY)) {
            r = ext4_inode_get_links_cnt(parent.inode) ? ENOTDIR : ENOENT;
            goto Finish;
        }
        len = strlen(path);
        r = ext4_dir_find_entry(&found, &parent, path, len, true);
        if (r != EOK) goto Finish;
        if (target && ext4_dir_en_get_inode(found.dentry) != target->inode) {
            r = EUCLEAN;
            goto Finish;
        }
        r = ext4_fs_get_inode_ref(&mp->fs, ext4_dir_en_get_inode(found.dentry), &child);
        if (r != EOK) goto Finish;
    } else {
        r = ext4_open_relative(NULL, mp, parent_inode ? parent_inode : EXT4_INODE_ROOT_INDEX,
            path + prefix, O_RDONLY, EXT4_DE_UNKNOWN, NULL, &name_off, &child, &parent);
        if (r != EOK) { EXT4_MP_UNLOCK(mp); return r; }
        path += name_off + prefix;
        len = ext4_path_check(path, &is_goal);
        r = ext4_trans_start(mp);
        if (r != EOK) goto PutRefs;
    }
    child_inode = child.index;
    if (target && child_inode != target->inode) { r = EUCLEAN; goto Finish; }
    /* Lookup precedes an independent operation's begin. Upgrade its existing
     * accepted loans in place; an outer operation already owns private refs. */
    if (!reuse_dirent && !private_refs) {
        r = ext4_fs_inode_ref_write(&parent);
        if (r == EOK) r = ext4_fs_inode_ref_write(&child);
        if (r != EOK) goto Finish;
    }

    if (!allow_directory && ext4_inode_type(ext4_fs_super(&mp->fs), child.inode) ==
        EXT4_INODE_MODE_DIRECTORY) { r = EISDIR; goto Finish; }
    if (allow_directory) {
        bool empty = false;
        if (!ext4_inode_is_type(ext4_fs_super(&mp->fs), child.inode, EXT4_INODE_MODE_DIRECTORY))
            r = ENOTDIR;
        else r = ext4_dir_check_empty(&child, &empty);
        if (r == EOK && !empty) r = ENOTEMPTY;
        if (r != EOK) goto Finish;
    }

    r = ext4_unlink_prepare_reclaim(&child, target, allow_directory);
    if (r != EOK) goto Finish;

	r = reuse_dirent ? ext4_dir_remove_found(&parent, &found) :
        ext4_dir_remove_entry(&parent, path, len);
	if (r == EOK) {
        publishing = mp->fs.ext2_format;
		ext4_unlink_update_links(mp, &parent, &child);
		if (allow_directory) {
			ext4_inode_set_links_cnt(child.inode, 0);
			child.dirty = true;
		}
		if (out_inode)
			*out_inode = child_inode;
		if (!ext4_inode_get_links_cnt(child.inode)) {
			if (target && target->reclaim) r = ext4_free_unlinked_inode(&child);
			else {
				if (out_is_orphan) *out_is_orphan = true;
				if (mp->fs.jbd_journal) r = ext4_orphan_add(&child);
			}
		} else if (out_is_orphan) *out_is_orphan = false;
	}

Finish:
    if (found.block.lb_id) r = ext4_result(r, ext4_dir_destroy_result(&parent, &found));
    if (child.block.data) r = ext4_result(r, ext4_fs_put_inode_ref(&child));
    if (parent.block.data) r = ext4_result(r, ext4_fs_put_inode_ref(&parent));
    if (r && publishing) ext4_latch_nojournal_error(&mp->fs, r);
    r = ext4_trans_finish(mp, r);
    if (r && publishing) ext4_latch_nojournal_error(&mp->fs, r);
    EXT4_MP_UNLOCK(mp);
    return r;
PutRefs:
    r = ext4_result(r, ext4_fs_put_inode_ref(&child));
    r = ext4_result(r, ext4_fs_put_inode_ref(&parent));
    EXT4_MP_UNLOCK(mp);
	return r;
}

int ext4_funlink_dentry(const char *path, uint32_t *out_inode,
                        bool *out_is_orphan)
{
    return ext4_unlink_dentry_core(ext4_get_mount(path), 0, path, out_inode, out_is_orphan, false, NULL);
}

int ext4_fdir_unlink_dentry(const char *path, uint32_t *out_inode)
{
    bool orphan = false;
    return ext4_unlink_dentry_core(ext4_get_mount(path), 0, path, out_inode, &orphan, true, NULL);
}

int ext4_unlink_child(const char *mount, uint32_t parent, const char *name,
    bool directory, uint32_t *inode, bool *orphan)
{
    if (!parent || !name || !ext4_rename_name_valid(name, strlen(name))) return EINVAL;
    return ext4_unlink_dentry_core(ext4_get_mount(mount), parent, name,
        inode, orphan, directory, NULL);
}

/* The caller has proved that no directory entry or live file owns this inode. */
static int ext4_free_unlinked_inode(struct ext4_inode_ref *child)
{
    ext4_inode_set_size(child->inode, 0);
    child->dirty = true;
    bool done = false;
    int result = EOK;
    while (!result && !done) result = ext4_inode_truncate_step(child, &done);
    if (result) return result;
    ext4_inode_set_del_time(child->inode, -1L);
    return ext4_fs_free_inode(child);
}

int ext4_unlink_child_resolved(const char *mount, uint32_t parent, const char *name,
    bool directory, uint32_t inode, bool *orphan, void (*retire)(void *), void *context)
{
    if (!mount || !parent || !inode || !orphan || !name ||
        !ext4_rename_name_valid(name, strlen(name))) return EINVAL;
    struct ext4_mountpoint *mp = ext4_get_mount(mount);
    if (!mp) return ENOENT;

    struct ext4_unlink_target target = {.inode = inode, .allow_reclaim = retire != NULL};
    *orphan = false;
    EXT4_MP_LOCK(mp);
    int result;
    if (!mp->fs.jbd_journal)
        result = ext4_unlink_dentry_core(mp, parent, name, NULL, orphan, directory, &target);
    else if (ext4_operation_depth(&mp->fs)) result = EBUSY;
    else for (;;) {
        result = ext4_trans_start(mp);
        if (result) break;
        target.reclaim = false;
        *orphan = false;
        result = ext4_unlink_dentry_core(mp, parent, name, NULL, orphan, directory, &target);
        result = ext4_trans_finish_accept(mp, result, target.reclaim ? retire : NULL, context);
        if (result == EAGAIN && ext4_operation_resource_retry(&mp->fs)) continue;
        /* Deletion used to succeed before reporting a later retirement error.
         * Keep that behavior by retrying the ordinary orphan path only after
         * rollback, before any new VFS state has been published. */
        if (result && target.reclaim && ext4_operation_aborted(&mp->fs) &&
            !jbd_journal_error(mp->fs.jbd_journal)) {
            target.allow_reclaim = false;
            continue;
        }
        if (!ext4_operation_retry_reclaim(mp, &result)) break;
    }
    EXT4_MP_UNLOCK(mp);
    return result;
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
	EXT4_MP_LOCK(mp);
	if (mp->fs.jbd_journal) {
		r = ext4_reclaim_orphan(mp, inode, true);
		EXT4_MP_UNLOCK(mp);
		return r;
	}
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

	/* Reclaim the accepted orphan even if the last description changed flags. */
	if (!ext4_inode_get_links_cnt(child.inode))
		r = ext4_free_unlinked_inode(&child);

	r = ext4_result(r, ext4_fs_put_inode_ref(&child));

	r = ext4_trans_finish(mp, r);

	EXT4_MP_UNLOCK(mp);
	return r;
}

static int ext4_fremove_locked(const char *path)
{
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	bool grouped = mp && mp->fs.jbd_journal && ext4_operation_depth(&mp->fs);
	uint32_t inode = 0;
	bool is_orphan = false;
	int r = ext4_funlink_dentry(path, &inode, &is_orphan);
	if (grouped) {
		/* The unlink and its persistent orphan belong to the outer group.
		 * Reclamation must wait until that group commits. Also latch errors
		 * from lookup/prechecks, which precede unlink's nested begin. */
		if (r != EOK && ext4_current_trans(&mp->fs) && !ext4_current_trans(&mp->fs)->error)
			ext4_current_trans(&mp->fs)->error = r;
		return r;
	}
	if (r != EOK)
		return r;
	if (is_orphan)
		return ext4_orphan_free(path, inode);
	return EOK;
}

int ext4_fremove(const char *path)
{
	struct ext4_mountpoint *mp = ext4_get_mount(path);
	if (!mp) return ENOENT;
	EXT4_MP_LOCK(mp);
	int result = ext4_fremove_locked(path);
	EXT4_MP_UNLOCK(mp);
	return result;
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

	if (r == EOK && (flags & O_TRUNC) && mp->fs.jbd_journal &&
	    !ext4_operation_depth(&mp->fs))
		r = ext4_reclaim_orphan(mp, file->inode, false);
	EXT4_MP_UNLOCK(mp);
	return r;
}

int ext4_fclose(ext4_file *file)
{
	ext4_assert(file && file->mp);

	file->mp = 0;
	file->flags = 0;
	file->inode = 0;
	file->fpos = file->fsize = file->fmax = 0;
	file->time_owner = NULL;
	file->delalloc = NULL;

	return EOK;
}


static int ext4_initialize_fblock(struct ext4_inode_ref *ref, ext4_fsblk_t fblock)
{
	return ext4_trans_write_data(ref->fs->bdev, fblock, 0, NULL,
		ext4_sb_get_block_size(ext4_fs_super(ref->fs)), true, NULL);
}

static int ext4_zero_fblock_range(struct ext4_inode_ref *ref,
	ext4_fsblk_t fblock, uint32_t offset, uint32_t length)
{
	COST_PHASE_SCOPE(read_source, 7);
	return ext4_trans_write_data(ref->fs->bdev, fblock, offset, NULL, length, false, NULL);
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

static int ext4_zero_allocated_eof_tail_mode(struct ext4_inode_ref *ref,
					uint64_t old_size,
					uint64_t visible_end, bool synchronous)
{
	uint32_t block_size = ext4_sb_get_block_size(ext4_fs_super(ref->fs));
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

	if (synchronous)
		return ext4_trans_write_data_sync(ref->fs->bdev,fblock,offset,NULL,length,false);
	return ext4_zero_fblock_range(ref, fblock, offset, length);
}
static int ext4_zero_allocated_eof_tail(struct ext4_inode_ref *ref,
    uint64_t old_size,uint64_t visible_end)
{ return ext4_zero_allocated_eof_tail_mode(ref,old_size,visible_end,false); }

static int ext4_ftruncate_no_lock(ext4_file *file, uint64_t size, uint32_t privilege_mode,
                                 bool prune_tail)
{
	struct ext4_inode_ref ref;
	struct ext4_sblock *sb = ext4_fs_super(&file->mp->fs);
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
	prune_tail |= size <= old_size;
	if (file->delalloc && prune_tail)
		file->delalloc->mapped_end = 0;
	/* 外部 capability 的修改会取得组资格；缩短意图先选定 orphan 记录，
	 * 仍与 capability、EOF 修改共享同一个失败回滚边界。 */
	if (prune_tail && file->mp->fs.jbd_journal) {
		r = ext4_orphan_add(&ref);
        if (r == EOK) r = ext4_orphan_mark_truncate(&ref,true);
		if (r != EOK) goto Finish;
	}
	/* 发起者给出的 set-ID 结果与 capability、大小共用操作 undo。 */
	r = ext4_xattr_remove(&ref, 6, "capability", 10);
	if (r == ENODATA) r = EOK;
	if (r != EOK) goto Finish;
    if (privilege_mode!=UINT32_MAX) {
        ext4_inode_set_mode(sb,ref.inode,privilege_mode);ref.dirty=true;
    }
	if (old_size < size) {
		r = ext4_zero_allocated_eof_tail(&ref, old_size, size);
		if (r == EOK) {
			ext4_inode_set_size(ref.inode, size);
			ref.dirty = true;
		}
	} else {
		if (file->mp->fs.jbd_journal) {
			ext4_inode_set_size(ref.inode, size);
			ref.dirty = true;
		} else r = ext4_trunc_inode(file->mp, ref.index, size);
	}
	if (r == EOK && prune_tail && file->fpos > size)
		file->fpos = size;

Finish:
	if (r == EOK || ext4_inode_get_size(sb, ref.inode) != old_size) {
		ext4_touch_inode(file->mp, &ref, EXT4_TIME_MTIME | EXT4_TIME_CTIME, file->time_owner);
	}
	file->fsize = ext4_inode_get_size(sb, ref.inode);
	cleanup_r = ext4_fs_put_inode_ref(&ref);
	if (r == EOK)
		r = cleanup_r;
	return r;
}

static int ext4_ftruncate_mode(ext4_file *f, uint64_t size, uint32_t privilege_mode,
                               bool prune_tail)
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

Retry:
	r = ext4_track_nested_file(f);
	if (r == EOK) r = ext4_prepare_growth(f, size);
	if (r == EOK) r = ext4_trans_start(f->mp);
	if (r != EOK) {
		EXT4_MP_UNLOCK(f->mp);
		return r;
	}
	saved_size = f->fsize;
	saved_pos = f->fpos;
	r = ext4_ftruncate_no_lock(f, size, privilege_mode, prune_tail);
	r = ext4_file_data_finish(f, r);
	if (ext4_file_rolled_back(f, r)) {
		f->fsize = saved_size;
		f->fpos = saved_pos;
	}
	if (r == EAGAIN && !ext4_operation_depth(&f->mp->fs) &&
	    ext4_operation_resource_retry(&f->mp->fs)) goto Retry;
    if (ext4_operation_retry_reclaim(f->mp, &r)) goto Retry;
	if (r == EOK && f->mp->fs.jbd_journal && !ext4_operation_depth(&f->mp->fs))
		r = ext4_reclaim_orphan(f->mp, f->inode, false);

	EXT4_MP_UNLOCK(f->mp);
	return r;
}

int ext4_ftruncate(ext4_file *file, uint64_t size, uint32_t privilege_mode)
{ return ext4_ftruncate_mode(file, size, privilege_mode, false); }

int ext4_ftruncate_prune_tail(ext4_file *file, uint64_t size, uint32_t privilege_mode)
{ return ext4_ftruncate_mode(file, size, privilege_mode, true); }

/* Holes and unwritten mappings already read as zero and must not acquire
 * storage or become initialized just to erase a partial boundary. */
static int ext4_zero_file_range(struct ext4_inode_ref *ref,
    uint64_t begin, uint64_t end)
{
    uint32_t block_size = ext4_sb_get_block_size(ext4_fs_super(ref->fs));
    while (begin < end) {
        struct ext4_inode_block_range range;
        int r = ext4_extent_get_range(ref, (ext4_lblk_t)(begin / block_size),
            1, false, &range);
        if (r != EOK) return r;
        uint32_t offset = begin % block_size;
        uint32_t count = block_size - offset;
        if (count > end - begin) count = end - begin;
        if (range.block && !range.unwritten) {
            r = ext4_zero_fblock_range(ref, range.block, offset, count);
            if (r != EOK) return r;
        }
        begin += count;
    }
    return EOK;
}

void ext4_file_drain_reservations(ext4_file *file)
{
    /* Empty overwrite batches have no retired reservation storage. The hint
     * never borrows a node: a nonempty list is still taken under STATE. */
    if (!file->delalloc || !__atomic_load_n(&file->delalloc->retired,__ATOMIC_RELAXED))return;
    struct ext4_delalloc_chunk *retired;
    { EXT4_STATE_SCOPE(state,&file->mp->fs);
      retired=__atomic_exchange_n(&file->delalloc->retired,NULL,__ATOMIC_RELAXED); }
    while (retired) {struct ext4_delalloc_chunk *next=retired->next;ext4_free(retired);retired=next;}
}
void ext4_file_release_reservations(ext4_file *file,uint64_t size)
{
    if (!file->delalloc)return;
    struct ext4_fs *fs=&file->mp->fs;size_t block=fs->bdev->lg_bsize;
    { EXT4_STATE_SCOPE(state,fs);
      uint64_t count=delalloc_range(file->delalloc,(size+block-1)/block,UINT64_C(1)<<32,true);
      if (count) {
          EXT4_SPACE_SCOPE(space,fs);
          release_delayed_blocks(fs,count,count*EXT4_EXTENT_MAX_DEPTH);
      } }
    ext4_file_drain_reservations(file);
}

bool ext4_file_overwrite_mapped(const ext4_file *file, uint64_t offset, size_t length)
{
    const struct ext4_delalloc *da = file->delalloc;
    struct ext4_fs *fs = &file->mp->fs;
    if (!da || !da->mapped_end || !fs->jbd_journal || fs->read_only || ext4_current_operation(fs))
        return false;
    uint64_t block = fs->bdev->lg_bsize;
    uint64_t end = da->mapped_end * block;
    return offset >= da->mapped_first * block && offset <= end && length <= end - offset;
}

int ext4_file_reserve_write(ext4_file *file,uint64_t offset,size_t length,size_t *ready,
    struct file_write_reservation *ticket)
{
    *ready=0;
    if (ticket->owner || ticket->next) __builtin_trap();
    if (!file->delalloc || !file->mp->fs.jbd_journal)return ENOTSUP;
    if (file->mp->fs.read_only)return EROFS;
    if (offset>file->fmax || length>file->fmax-offset)return EFBIG;
    struct ext4_fs *fs=&file->mp->fs;
    /* 私有事务看到的空洞可能随 abort 消失，不能作为后续写入的依据。 */
    bool stable_mapping=!ext4_current_operation(fs);
    struct ext4_delalloc *da=file->delalloc;
    size_t block=fs->bdev->lg_bsize;
    /* The adapter's inode writer already protects these accepted mappings.
     * An overwrite entirely inside them needs no backend scope or ticket. */
    if (!length || (stable_mapping && offset>=da->mapped_first*block &&
        offset+length<=da->mapped_end*block)) {
        *ready=length;
        return EOK;
    }
    /* Creation already initialized and published a valid empty extent root.
     * On that inode, the I/O writer and space-counter guard suffice until a
     * mapping change invalidates this fact through delalloc_range(). */
    bool fresh_extent=stable_mapping && __atomic_load_n(&da->fresh_extent,__ATOMIC_RELAXED);
    if (!fresh_extent) EXT4_MP_LOCK(file->mp);
    struct ext4_inode_ref ref;
    bool have_ref=false;
    int r=EOK;
    while (!r && *ready<length) {
        uint64_t position=offset+*ready,logical=position/block;
        size_t rest=length-*ready;
        if (stable_mapping && logical>=da->mapped_first && logical<da->mapped_end) {
            uint64_t bytes=da->mapped_end*block-position;
            *ready+=bytes<rest?(size_t)bytes:rest;
            continue;
        }
        uint32_t key=logical>>6;unsigned bit=logical&63;
        uint64_t needed=(rest-1+position%block)/block+1;
        unsigned maximum=needed<64-bit?needed:64-bit;
        /* inode I/O 写资格排除本 inode 的映射修改和预留回收。 */
        struct ext4_delalloc_chunk *existing=delalloc_find(da,key);
        bool missing_chunk=!existing;
        uint64_t known=existing?(existing->mask|(stable_mapping?existing->known_holes:0))>>bit:0;
        uint64_t holes=0;
        struct ext4_inode_block_range range={0};
        while (range.count<maximum && (known&1)) {range.count++;known>>=1;}
        if (!range.count && fresh_extent) {
            range.count=maximum;
            holes=delalloc_mask(bit,maximum);
        } else if (!range.count) {
            if (!have_ref) {
                r=ext4_fs_get_inode_ref_read(fs,file->inode,&ref);
                if (r)break;
                have_ref=true;
                if (!ext4_inode_has_flag(ref.inode,EXT4_INODE_FLAG_EXTENTS)) {r=ENOTSUP;break;}
            }
            unsigned query=maximum;
            if (stable_mapping) {
                uint64_t last=file->fmax/block+(file->fmax%block!=0);
                uint64_t available=last-logical;
                query=available>UINT32_MAX?UINT32_MAX:(unsigned)available;
            }
            r=ext4_extent_get_range(&ref,logical,query,false,&range);
            if (!r && range.count>query)__builtin_trap();
            if (!r && stable_mapping && range.count) {
                if (!range.block) {
                    unsigned known=range.count<64-bit?range.count:64-bit;
                    holes=delalloc_mask(bit,known);
                } else {
                    /* Orphan recovery may reclaim preallocation beyond i_size.
                     * Remember only whole blocks that survive that cleanup. */
                    uint64_t last=ext4_inode_get_size(ext4_fs_super(fs),ref.inode)/block;
                    uint64_t end=logical+range.count;
                    if (end>last)end=last;
                    if (end>logical) {
                        if (!da->mapped_end || da->mapped_end<logical || da->mapped_first>end) {
                            da->mapped_first=logical;
                            da->mapped_end=end;
                        } else {
                            if (logical<da->mapped_first)da->mapped_first=logical;
                            if (end>da->mapped_end)da->mapped_end=end;
                        }
                    }
                }
            }
        }
        if (r)break;
        if (!range.count) {r=EIO;break;}
        if (range.count>maximum)range.count=maximum;
        if (!range.block) {
            struct file_write_reservation *change=ticket->owner?ext4_calloc(1,sizeof(*change)):ticket;
            struct ext4_delalloc_chunk *candidate=missing_chunk?ext4_calloc(1,sizeof(*candidate)):NULL;
            if (!change || (missing_chunk && !candidate)) {
                if (change!=ticket) ext4_free(change);
                ext4_free(candidate);r=ENOMEM;break;
            }
            if (candidate)candidate->key=key;
            uint64_t mask=delalloc_mask(bit,range.count),missing=mask&~(existing?existing->mask:0);
            unsigned charged=delalloc_count(missing);
            bool pending_extent_space=false;
            if (charged) {
                EXT4_SPACE_SCOPE(space,fs);
                /* Physical fragmentation can turn each new logical block
                 * into its own insertion. Hold the worst-case tree growth
                 * until the actual writeback transaction publishes. */
                uint64_t available=unreserved_blocks(fs)/(1+EXT4_EXTENT_MAX_DEPTH);
                pending_extent_space=fs->delayed_extent_blocks!=0;
                if (charged>available) {
                    unsigned n=0;
                    for (;n<range.count;n++) {
                        uint64_t one=UINT64_C(1)<<(bit+n);
                        if (!(missing&one))continue;
                        if (!available)break;
                        available--;
                    }
                    range.count=n;
                    mask=delalloc_mask(bit,n);
                    missing&=mask;
                    charged=delalloc_count(missing);
                }
                fs->delayed_blocks+=charged;
                fs->delayed_extent_blocks+=charged*EXT4_EXTENT_MAX_DEPTH;
            }
            /* The inode I/O writer owns the tree. Only newly charged blocks
             * visit the mount counter; tree publication cannot allocate. */
            if (range.count) {
                struct ext4_delalloc_chunk *chunk=existing;
                if (!chunk) {
                    if (!candidate)__builtin_trap();
                    chunk=candidate;candidate=NULL;RB_INSERT(ext4_delalloc_tree,&file->delalloc->tree,chunk);
                }
                chunk->known_holes|=holes;
                chunk->mask|=missing;
                if (chunk->borrowers==UINT32_MAX)__builtin_trap();
                /* The live request is the first owner; other records are only
                 * needed if this request borrows more than one range. */
                chunk->borrowers++;
                *change=(struct file_write_reservation){.owner=chunk,.mask=mask};
                if (change!=ticket) {change->next=ticket->next;ticket->next=change;}
                __atomic_fetch_add(&file->delalloc->blocks,charged,__ATOMIC_RELAXED);
            }
            ext4_free(candidate);
            if (!range.count) {
                if (change!=ticket) ext4_free(change);
                ticket->space_pressure=pending_extent_space;
                r=ENOSPC;break;
            }
        }
        size_t bytes=(size_t)range.count*block-position%block;
        if (bytes>rest)bytes=rest;
        *ready+=bytes;
    }
    int put=have_ref?ext4_fs_put_inode_ref(&ref):EOK;if(!r)r=put;
    if (!fresh_extent) EXT4_MP_UNLOCK(file->mp);
    return r;
}
void ext4_file_accept_reservation(ext4_file *file,struct file_write_reservation *ticket,uint64_t start,uint64_t accepted_end)
{
    struct ext4_fs *fs=&file->mp->fs;
    size_t block=fs->bdev->lg_bsize;
    EXT4_STATE_SCOPE(state,fs);
    for (struct file_write_reservation *change=ticket;change;change=change->next) {
        struct ext4_delalloc_chunk *chunk=change->owner;
        if (!chunk || !chunk->borrowers)__builtin_trap();
        uint64_t last=accepted_end>start?(accepted_end+block-1)/block:start/block;
        uint64_t base=(uint64_t)chunk->key*64;
        unsigned kept=last<=base?0:last-base<64?last-base:64;
        chunk->accepted|=change->mask&chunk->mask&delalloc_mask(0,kept);
        change->committed=true;
    }
}
void ext4_finish_full_reservation(struct file_write_reservation *ticket)
{
    if (ticket->next) return;
    struct ext4_delalloc_chunk *chunk = ticket->owner;
    uint64_t accepted = chunk->accepted | (ticket->mask & chunk->mask);
    /* The inode writer holds this chunk stable. Keep final retirement and
     * unused-space return in the ordinary completion path. */
    if (chunk->borrowers == 1 && (!chunk->mask || (chunk->mask & ~accepted)))
        return;
    chunk->accepted = accepted;
    chunk->borrowers--;
    ticket->owner = NULL;
}

void ext4_file_finish_reservation(ext4_file *file,struct file_write_reservation *ticket,uint64_t start,uint64_t accepted_end)
{
    struct ext4_fs *fs=&file->mp->fs;
    size_t block=fs->bdev->lg_bsize;
    for (struct file_write_reservation *change=ticket,*next;change;change=next) {
        next=change->next;
        uint64_t last=accepted_end>start?(accepted_end+block-1)/block:start/block;
        /* The adapter holds this inode's I/O writer, excluding mapping
         * acceptance and writeback of its reservation tree. Finishing a fully
         * accepted range changes no global allocation or retirement state. */
        struct ext4_delalloc_chunk *chunk=change->owner;
        if (!chunk || !chunk->borrowers)__builtin_trap();
        uint64_t base=(uint64_t)chunk->key*64;
        unsigned kept=last<=base?0:last-base<64?last-base:64;
        if (!change->committed)chunk->accepted|=change->mask&chunk->mask&delalloc_mask(0,kept);
        if (!--chunk->borrowers &&
            (!chunk->mask || (chunk->mask&~chunk->accepted))) {
            /* Another source or mapping can have accepted the same block.
             * Only the last preparation releases still-unaccepted bits. */
            uint64_t unused=chunk->mask&~chunk->accepted;
            unsigned count=delalloc_count(unused);
            chunk->mask&=~unused;
            __atomic_fetch_sub(&file->delalloc->blocks,count,__ATOMIC_RELAXED);
            if (count) {
                EXT4_SPACE_SCOPE(space,fs);
                release_delayed_blocks(fs,count,count*EXT4_EXTENT_MAX_DEPTH);
            }
            if (!chunk->mask) {
                /* A separate worker may drain the retired list without the
                 * inode writer. Serialize the retirement, not the charge. */
                EXT4_STATE_SCOPE(state,fs);
                RB_REMOVE(ext4_delalloc_tree,&file->delalloc->tree,chunk);
                chunk->next=__atomic_load_n(&file->delalloc->retired,__ATOMIC_RELAXED);
                __atomic_store_n(&file->delalloc->retired,chunk,__ATOMIC_RELAXED);
            }
        }
        if (change!=ticket) ext4_free(change);
    }
    *ticket=(struct file_write_reservation){0};
    ext4_file_drain_reservations(file);
}

int ext4_file_prepare_write(ext4_file *file,uint64_t offset,size_t length,size_t *ready,bool *complete)
{
    ext4_assert(file && file->mp && ready && complete);
    *ready=0; *complete=false;
    if (file->mp->fs.read_only) return EROFS;
    if (offset>file->fmax || length>file->fmax-offset) return EFBIG;
    EXT4_MP_LOCK(file->mp);
    struct ext4_fs *fs=&file->mp->fs;
    struct jbd_journal *journal=fs->jbd_journal;
    uint32_t block=fs->bdev->lg_bsize;
    if (length && file->delalloc && journal) {
        EXT4_STATE_SCOPE(state,fs);
        (void)delalloc_range(file->delalloc,offset/block,(offset+length-1)/block+1,false);
    }
    int r=ext4_prepare_growth(file,offset+length);
    while (!r && *ready<length) {
        uint64_t position=offset+*ready;
        ext4_lblk_t logical=position/block;
        uint64_t needed=(position%block+length-*ready+block-1)/block;
        uint32_t maximum=needed<EXT4_BLOCK_BATCH_MAX?needed:EXT4_BLOCK_BATCH_MAX;
        struct ext4_inode_ref ref;
        struct ext4_inode_block_range range;
        r=ext4_fs_get_inode_ref(fs,file->inode,&ref);
        if (r) break;
        bool extent=ext4_inode_has_flag(ref.inode,EXT4_INODE_FLAG_EXTENTS);
        uint64_t eof_blocks=(ext4_inode_get_size(ext4_fs_super(fs),ref.inode)+block-1)/block;
        r=ext4_fs_get_inode_dblk_range(&ref,logical,maximum,false,&range);
        int cleanup=ext4_fs_put_inode_ref(&ref);
        if (!r) r=cleanup;
        if (r) break;
        if (!range.block) {
Retry:
            if (journal && journal->grouped && !ext4_operation_depth(&file->mp->fs)) {
                for (;;) {
                    struct jbd_journal_reclaim goal;
                    {
                    EXT4_STATE_READ_SCOPE(state, fs);
                    unsigned resources = jbd_journal_operation_resources(journal, NULL,
                        maximum, extent ? 0 : maximum, NULL, 0);
                    if (!resources) break;
                    r=ext4_group_reclaim_goal(file->mp,resources,&goal);
                    }
                    if (r) break;
                    r=ext4_group_wait_reclaim(file->mp,&goal);
                    if (r) break;
                }
            }
            if (r) break;
            r=ext4_trans_start(file->mp);
            if (r) break;
            r=ext4_fs_get_inode_ref(fs,file->inode,&ref);
            if (!r) {
                if (journal && (uint64_t)logical+maximum>eof_blocks) {
                    r=ext4_orphan_add(&ref);
                    if (!r) r=ext4_orphan_mark_truncate(&ref,false);
                }
                /* 无日志分配可在发布指针后的 buffer 归还处报错，并清空
                 * range 输出；在创建前保留责任，不能依赖成功输出找 owner。 */
                if (!extent && !journal && (uint64_t)logical+maximum>eof_blocks) *complete=true;
                if (!r) r=extent?ext4_extent_reserve_range(&ref,logical,maximum,&range):
                    ext4_fs_get_inode_dblk_range(&ref,logical,maximum,true,&range);
                if (!r && !extent && range.allocated) {
                    for (uint32_t i=0;i<range.count && !r;i++)
                        r=ext4_initialize_fblock(&ref,range.block+i);
                    if (r && !journal) {
                        for (uint32_t i=range.count;i;i--) {
                            int discarded=ext4_fs_release_inode_dblk_idx(&ref,logical+i-1,range.block+i-1);
                            if (discarded) { r=discarded; break; }
                        }
                    }
                }
                cleanup=ext4_fs_put_inode_ref(&ref);
                if (!r) r=cleanup;
            }
            r=ext4_trans_finish(file->mp,r);
            if (r==EAGAIN && !ext4_operation_depth(fs) && ext4_operation_resource_retry(fs)) {
                r=EOK;
                goto Retry;
            }
            if (ext4_operation_retry_reclaim(file->mp, &r)) goto Retry;
            if (r) break;
        }
        if (!range.block || !range.count) { r=EIO; break; }
        if ((!extent || journal) && (uint64_t)logical+range.count>eof_blocks) *complete=true;
        size_t bytes=(size_t)range.count*block-position%block;
        if (bytes>length-*ready) bytes=length-*ready;
        *ready+=bytes;
    }
    EXT4_MP_UNLOCK(file->mp);
    return r;
}

int ext4_file_complete_write(ext4_file *file)
{
    ext4_assert(file && file->mp);
    EXT4_MP_LOCK(file->mp);
    struct ext4_fs *fs=&file->mp->fs;
Retry:;
    int r=EOK;
    if (fs->jbd_journal) {
        bool present;
        r=ext4_orphan_contains(fs,file->inode,&present);
        if (!r && present) {
            r=ext4_trans_start(file->mp);
            if (!r) {
                struct ext4_inode_ref ref;
                r=ext4_fs_get_inode_ref(fs,file->inode,&ref);
                if (!r) {
                    r=ext4_orphan_mark_truncate(&ref,true);
                    int put=ext4_fs_put_inode_ref(&ref); if (!r) r=put;
                }
                r=ext4_trans_finish(file->mp,r);
            }
            if (!r) r=ext4_reclaim_orphan(file->mp,file->inode,false);
        }
    } else {
        r=ext4_trans_start(file->mp);
        if (!r) {
            struct ext4_inode_ref ref;
            r=ext4_fs_get_inode_ref(fs,file->inode,&ref);
            uint64_t size=0;
            if (!r) {
                size=ext4_inode_get_size(ext4_fs_super(fs),ref.inode);
                r=ext4_fs_put_inode_ref(&ref);
            }
            if (!r) r=ext4_trunc_inode(file->mp,file->inode,size);
            r=ext4_trans_finish(file->mp,r);
        }
    }
    if (r==EAGAIN && !ext4_operation_depth(fs) && ext4_operation_resource_retry(fs)) goto Retry;
    if (ext4_operation_retry_reclaim(file->mp, &r)) goto Retry;
    EXT4_MP_UNLOCK(file->mp);
    return r;
}

int ext4_fallocate(ext4_file *file, unsigned mode, uint64_t offset,
                   uint64_t length, bool *changed, uint32_t privilege_mode)
{
    ext4_assert(file && file->mp && changed);
    *changed = false;
    unsigned operation = mode & ~EXT4_FALLOC_KEEP_SIZE;
    bool punch = operation == EXT4_FALLOC_PUNCH_HOLE;
    bool zero = operation == EXT4_FALLOC_ZERO_RANGE;
    if (operation && !zero && !(punch && (mode & EXT4_FALLOC_KEEP_SIZE))) return ENOTSUP;
    if (!length || offset > INT64_MAX || length > INT64_MAX) return EINVAL;
    if (length > INT64_MAX - offset) return EFBIG;
    if (file->mp->fs.read_only) return EROFS;
    if (file->flags & O_RDONLY) return EPERM;
    uint64_t end = offset + length;
    if (end > file->fmax) return EFBIG;
    EXT4_MP_LOCK(file->mp);
    struct ext4_fs *fs=&file->mp->fs;
    if (file->delalloc && fs->jbd_journal) {
        if (punch || zero) file->delalloc->mapped_end=0;
        size_t block=fs->bdev->lg_bsize;
        EXT4_STATE_SCOPE(state,fs);
        (void)delalloc_range(file->delalloc,offset/block,(end-1)/block+1,false);
    }
Retry:
    end = offset + length;
    *changed = false;
    int r = ext4_track_nested_file(file);
    if (r == EOK) r = ext4_prepare_growth(file, end);
    if (r == EOK) r = ext4_trans_start(file->mp);
    if (r != EOK) { EXT4_MP_UNLOCK(file->mp); return r; }
    uint64_t saved_size = file->fsize;
    struct ext4_inode_ref ref;
    r = ext4_fs_get_inode_ref(&file->mp->fs, file->inode, &ref);
    if (r != EOK) goto Finish;
    struct ext4_sblock *sb = ext4_fs_super(&file->mp->fs);
    uint32_t block_size = ext4_sb_get_block_size(sb);
    uint64_t size = ext4_inode_get_size(sb, ref.inode);
    if (!ext4_inode_has_flag(ref.inode, EXT4_INODE_FLAG_EXTENTS)) {
        r = ENOTSUP;
        goto Put;
    }
    if (ext4_inode_type(sb, ref.inode) != EXT4_INODE_MODE_FILE) {
        r = EINVAL;
        goto Put;
    }
    if (ext4_inode_has_flag(ref.inode, EXT4_INODE_FLAG_IMMUTABLE) ||
        ((punch || zero) && ext4_inode_has_flag(ref.inode, EXT4_INODE_FLAG_APPEND))) {
        r = EPERM;
        goto Put;
    }
    /* A nonjournal failure may retain a prefix. Journal abort restores every
     * modified bitmap, extent and boundary data version together. */
    *changed = true;
    r = ext4_xattr_remove(&ref, 6, "capability", 10);
    if (r == ENODATA) r = EOK;
    if (r != EOK) goto Put;
    if (privilege_mode!=UINT32_MAX) {
        ext4_inode_set_mode(sb,ref.inode,privilege_mode);ref.dirty=true;
    }
    if (punch && offset >= size) goto Touch;
    if (punch) {
        uint64_t eof_block = (size + block_size - 1) / block_size * block_size;
        if (end > eof_block) end = eof_block;
        uint64_t first = (offset + block_size - 1) / block_size;
        uint64_t last = end / block_size;
        uint64_t left_end = first * block_size;
        if (left_end > end) left_end = end;
        if (offset < left_end) r = ext4_zero_file_range(&ref, offset, left_end);
        if (r == EOK && first < last)
            r = ext4_extent_remove_space(&ref, (ext4_lblk_t)first,
                (ext4_lblk_t)(last - 1));
        uint64_t right = last * block_size;
        if (r == EOK && right >= left_end && right < end)
            r = ext4_zero_file_range(&ref, right, end);
    } else {
        uint64_t first = offset / block_size;
        uint64_t last = (end + block_size - 1) / block_size;
        uint64_t full_first = (offset + block_size - 1) / block_size;
        uint64_t full_end = end / block_size;
        while (r == EOK && first < last) {
            struct ext4_inode_block_range range;
            bool convert = zero && first >= full_first && first < full_end;
            uint64_t run_end = convert ? full_end : last;
            if (zero && first < full_first) run_end = full_first;
            uint64_t count = run_end - first;
            if (count > UINT32_MAX) count = UINT32_MAX;
            if (convert)
                r = ext4_extent_zero_range(&ref, (ext4_lblk_t)first, (uint32_t)count, &range);
            else
                r = ext4_extent_reserve_range(&ref, (ext4_lblk_t)first, (uint32_t)count, &range);
            if (r != EOK) break;
            if (!range.count) { r = EIO; break; }
            first += range.count;
        }
        if (r == EOK && zero) {
            uint64_t left_end = full_first * block_size;
            uint64_t right = full_end * block_size;
            if (left_end > end) left_end = end;
            r = ext4_zero_file_range(&ref, offset, left_end);
            if (r == EOK && right >= left_end && right < end)
                r = ext4_zero_file_range(&ref, right, end);
        }
        if (r == EOK && !(mode & EXT4_FALLOC_KEEP_SIZE) && end > size) {
            if (!zero)
                r = ext4_zero_allocated_eof_tail(&ref, size, end);
            else if (size < offset && size % block_size) {
                /* Clear a newly visible old EOF tail without allocating a gap. */
                uint64_t tail = (size / block_size + 1) * block_size;
                r = ext4_zero_file_range(&ref, size, offset < tail ? offset : tail);
            }
            if (r == EOK) {
                ext4_inode_set_size(ref.inode, end);
                ref.dirty = true;
            }
        }
    }
Touch:
    if (r == EOK) ext4_touch_inode(file->mp, &ref, EXT4_TIME_MTIME | EXT4_TIME_CTIME, file->time_owner);
Put:
    file->fsize = ext4_inode_get_size(sb, ref.inode);
    r = ext4_result(r, ext4_fs_put_inode_ref(&ref));
Finish:
    r = ext4_file_data_finish(file, r);
    if (ext4_file_rolled_back(file, r)) {
        file->fsize = saved_size;
        *changed = false;
    }
    if (r == EAGAIN && !ext4_operation_depth(&file->mp->fs) &&
        ext4_operation_resource_retry(&file->mp->fs)) goto Retry;
    if (ext4_operation_retry_reclaim(file->mp, &r)) goto Retry;
    EXT4_MP_UNLOCK(file->mp);
    return r;
}

/* Copy only an unpublished file version or an explicitly deferred standalone
 * cache write. A clean file block has no second cache owner. */
static bool ext4_copy_pending_data(struct ext4_blockdev *bdev, ext4_fsblk_t block,
	uint32_t offset, void *out, size_t length)
{
    struct jbd_data_borrow borrow;
    if (jbd_journal_borrow_data(bdev->fs, block, &borrow)) {
        if (out) memcpy(out, (const uint8_t *)borrow.data + offset, length);
        jbd_journal_release_borrow(&borrow);
        return true;
    }
	struct ext4_block cached = EXT4_BLOCK_ZERO();
	struct ext4_buf *buf = ext4_bcache_find_get(bdev->bc, &cached, block, NULL);
	if (!buf) return false;
	bool current = ext4_bcache_test_flag(buf, BC_DIRTY) || buf->journal_pending;
    if (current && out) memcpy(out, cached.data + offset, length);
	/* Lookup is only a borrow: it must not flush unrelated data in a read. */
    if (ext4_bcache_free_read(bdev->bc,&cached)) __builtin_trap();
    return current;
}

static int ext4_read_pending_data(struct ext4_blockdev *bdev,
	ext4_fsblk_t first, uint32_t offset, uint8_t *out, size_t length)
{
	while (length) {
		size_t chunk = bdev->lg_bsize - offset;
		if (chunk > length) chunk = length;
		if (!ext4_copy_pending_data(bdev, first, offset, out, chunk)) {
			unsigned sector = bdev->bdif->ph_bsize;
			void *scratch = NULL;
			void *target = out;
			unsigned skip = offset, bytes = chunk;
			if (offset % sector || chunk % sector) {
				/* Partial sectors need private scratch across a sleeping read. */
				scratch = ext4_malloc(bdev->lg_bsize);
				if (!scratch) return ENOMEM;
				target = scratch; skip = 0; bytes = bdev->lg_bsize;
			}
			struct block_read_span read = {bdev->part_offset + first * bdev->lg_bsize + skip,
				target, bytes, 0, EIO};
			int r = ext4_blockdev_read_batch(bdev, &read, 1, NULL);
			if (!r && scratch) memcpy(out, (uint8_t *)scratch + offset, chunk);
			ext4_free(scratch);
			if (r) return r;
		}
		first++; out += chunk; length -= chunk; offset = 0;
	}
	return EOK;
}

static int ext4_fread_body(ext4_file *file, void *buf, size_t size, size_t *rcnt, bool pure)
{
	COST_PHASE_SCOPE(read_source, 5);
	uint32_t block_size;
	uint8_t *u8_buf = buf;
	int r;
	struct ext4_inode_ref ref;

	ext4_assert(file && file->mp);

	if (file->flags & O_WRONLY)
		return EPERM;

	if (!size)
		return EOK;


	struct ext4_fs *const fs = &file->mp->fs;
	struct ext4_sblock *const sb = ext4_fs_super(&file->mp->fs);

	if (rcnt)
		*rcnt = 0;

	r = ext4_fs_get_inode_ref_read(fs, file->inode, &ref);
	if (r != EOK) {
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
		} else if (pure || ext4_current_trans(&file->mp->fs) ||
			   (file->mp->fs.jbd_journal && file->mp->fs.jbd_journal->grouped)) {
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
		} else if (pure || ext4_current_trans(&file->mp->fs) ||
			   (file->mp->fs.jbd_journal && file->mp->fs.jbd_journal->grouped)) {
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
		} else if (pure || ext4_current_trans(&file->mp->fs) ||
			   (file->mp->fs.jbd_journal && file->mp->fs.jbd_journal->grouped)) {
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
	return r;
}

int ext4_fread(ext4_file *file, void *buf, size_t size, size_t *rcnt)
{
    EXT4_MP_LOCK(file->mp);
    int result = ext4_fread_body(file, buf, size, rcnt, false);
    EXT4_MP_UNLOCK(file->mp);
    return result;
}
struct ext4_data_completion {
    struct jbd_data_range ranges[JBD_COMPLETE_RANGES_MAX];
    unsigned count;
};

static bool ext4_collect_physical(struct ext4_data_completion *batch,
    const struct ext4_inode_block_range *range)
{
    if (batch->count == JBD_COMPLETE_RANGES_MAX || range->block > UINT64_MAX - range->count)
        return false;
    unsigned i = batch->count++;
    while (i && batch->ranges[i - 1].first > range->block) {
        batch->ranges[i] = batch->ranges[i - 1];
        i--;
    }
    batch->ranges[i] = (struct jbd_data_range){range->block, range->count};
    return true;
}

static int ext4_complete_physical(struct jbd_journal *journal, struct ext4_data_completion *batch)
{
    /* The input is sorted by physical block. Even a repeated mapping must
     * never link the same ordered record into a device phase twice. */
    uint64_t end = 0;
    unsigned used = 0;
    for (unsigned i = 0; i < batch->count; i++) {
        struct jbd_data_range range = batch->ranges[i];
        uint64_t next = range.first + range.count;
        if (next <= end) continue;
        if (range.first < end) {
            range.first = end;
            range.count = next - end;
        }
        batch->ranges[used++] = range;
        end = next;
    }
    batch->count = 0;
    return jbd_journal_complete_data_batch(journal, batch->ranges, used);
}

static int ext4_complete_file_data(ext4_file *file, uint64_t offset, uint64_t end,
    struct ext4_data_completion *batch)
{
    struct ext4_fs *fs = &file->mp->fs;
    struct ext4_inode_ref ref;
    int result = ext4_fs_get_inode_ref_read(fs, file->inode, &ref);
    if (result) return result;
    uint64_t eof = ext4_inode_get_size(ext4_fs_super(fs), ref.inode);
    if (end > eof) end = eof;
    unsigned block = fs->bdev->lg_bsize;
    uint64_t logical = offset / block, limit = end / block + !!(end % block);
    while (!result && logical < limit) {
        struct ext4_inode_block_range range = {0};
        uint64_t remaining = limit - logical;
        result = ext4_fs_get_inode_dblk_range(&ref, logical,
            remaining > UINT32_MAX ? UINT32_MAX : remaining, false, &range);
        if (result) break;
        if (!range.count) __builtin_trap();
        logical += range.count;
        if (!range.block || range.unwritten) continue;
        if (!batch) {
            result = jbd_journal_complete_data(fs->jbd_journal, range.block, range.count);
            continue;
        }
        if (range.block > UINT64_MAX - range.count) {
            result = EUCLEAN;
            break;
        }
        if (batch->count == JBD_COMPLETE_RANGES_MAX) {
            result = ext4_complete_physical(fs->jbd_journal, batch);
            if (result) break;
        }
        if (!ext4_collect_physical(batch, &range)) __builtin_trap();
    }
    return ext4_result(result, ext4_fs_put_inode_ref(&ref));
}

int ext4_fdata_complete(ext4_file *file, uint64_t offset, size_t size)
{
    if (!file || !file->mp || offset > UINT64_MAX - size) return EINVAL;
    struct ext4_fs *fs = &file->mp->fs;
    if (!size || !fs->jbd_journal) return EOK;
    EXT4_MP_LOCK(file->mp);
    int result;
    if (ext4_operation_depth(fs)) { result = EBUSY; goto Exit; }
    if (ext4_fs_serial_writer(fs)) {
        /* The mount writer protects accepted mappings through home I/O. */
        result = fs->super_replay_required ? EUCLEAN : fs->read_only ? EROFS :
            jbd_journal_error(fs->jbd_journal);
        if (!result) result = ext4_complete_file_data(file, offset, offset + size, NULL);
        goto Exit;
    }
Retry:
    result = ext4_trans_start(file->mp);
    if (result) goto Exit;
    result = ext4_operation_require_resource(fs, EXT4_RESOURCE_INODE, file->inode);
    if (!result) result = ext4_complete_file_data(file, offset, offset + size, NULL);
    result = ext4_trans_finish(file->mp, result);
    if (result == EAGAIN && ext4_operation_resource_retry(fs)) goto Retry;
Exit:
    EXT4_MP_UNLOCK(file->mp);
    return result;
}

int ext4_fdata_complete_batch(const struct ext4_file_data_range *files, unsigned count)
{
    if (!files || !count || count > EXT4_SMALL_WRITEBACK_MAX ||
        !files[0].file || !files[0].file->mp) return EINVAL;
    struct ext4_mountpoint *mp = files[0].file->mp;
    struct ext4_fs *fs = &mp->fs;
    struct jbd_journal *journal = fs->jbd_journal;
    int result = ENOTSUP;
    EXT4_MP_LOCK(mp);
    if (!journal || !journal->grouped || !ext4_fs_serial_writer(fs) ||
        ext4_current_operation(fs) || !fs->bdev->bdif->bwrite_phase ||
        fs->read_only || fs->super_replay_required) goto Unlock;
    for (unsigned i = 0; i < count; i++)
        if (!files[i].file || files[i].file->mp != mp || files[i].start >= files[i].end) goto Unlock;
    struct ext4_data_completion batch = {0};
    result = jbd_journal_error(journal);
    for (unsigned i = 0; i < count && !result; i++)
        result = ext4_complete_file_data(files[i].file, files[i].start, files[i].end, &batch);
    if (!result) result = ext4_complete_physical(journal, &batch);
    /* Retry mapping failures one file at a time so only the failed inode
     * receives its error. A device failure is already sticky in the journal. */
    if (result && !jbd_journal_error(journal)) result = ENOTSUP;
Unlock:
    EXT4_MP_UNLOCK(mp);
    return result;
}

int ext4_file_seek_sparse(const ext4_file *file, uint64_t offset, uint64_t end,
    bool seek_hole, uint64_t (*cache_seek)(void *, uint64_t, uint64_t, bool, bool),
    void *context, uint64_t *position)
{
    if (offset >= end) return ENXIO;
    const struct ext4_lock *locks = file->mp->os_locks;
    if (locks && locks->read_lock) locks->read_lock(locks->context);
    else EXT4_MP_LOCK(file->mp);

    struct ext4_fs *fs = &file->mp->fs;
    unsigned block_size = ext4_sb_get_block_size(&fs->sb);
    struct ext4_inode_ref ref;
    int result = ext4_fs_get_inode_ref_read(fs, file->inode, &ref);
    if (result) goto unlock;
    while (offset < end) {
        uint64_t logical = offset / block_size;
        uint64_t blocks = (end - 1) / block_size - logical + 1;
        struct ext4_inode_block_range range;
        result = ext4_fs_get_inode_dblk_range(&ref, (ext4_lblk_t)logical,
            blocks > UINT32_MAX ? UINT32_MAX : (uint32_t)blocks, false, &range);
        if (result) break;
        if (!range.count) { result = EIO; break; }
        uint64_t next = (logical + range.count) * block_size;
        if (next > end) next = end;
        if (!range.block || range.unwritten) {
            uint64_t found = cache_seek(context, offset, next, seek_hole, range.unwritten);
            if (found < next) { offset = found; break; }
        } else if (!seek_hole) break;
        offset = next;
    }
    int released = ext4_fs_put_inode_ref(&ref);
    if (!result) result = released;
    if (!result) {
        if (offset == end && !seek_hole) result = ENXIO;
        else *position = offset;
    }
unlock:
    EXT4_MP_UNLOCK(file->mp);
    return result;
}

int ext4_fpread(const ext4_file *file, uint64_t offset, void *buf, size_t size, size_t *rcnt)
{
    if (!rcnt) return EINVAL;
    struct file_read_span span = {offset, buf, size, 0, EOK};
    int result = ext4_fpread_batch(file, &span, 1, NULL, UINT64_MAX, NULL, false);
    *rcnt = span.completed;
    return result;
}

/* The mount read lock stabilizes inode mappings and current cache versions. */
/* One span can cover several mappings. Publish only its contiguous result
 * after every fragment already exposed to DMA has returned. */
static void ext4_file_read_publish(struct file_read_workspace *progress, unsigned index)
{
    if (progress->completion_emitted & (UINT32_C(1) << index)) return;
    struct file_read_span *span = &progress->completion_spans[index];
    unsigned *cursor = &progress->completion_cursor[index];
    while (*cursor < progress->completion_parts && progress->fragments[*cursor].span == index) {
        if (!(progress->completion_ready & (UINT32_C(1) << *cursor))) return;
        struct file_read_fragment *fragment = &progress->fragments[(*cursor)++];
        if (!span->error) {
            if (fragment->error) span->error = fragment->error;
            else span->completed += fragment->length;
        }
    }
    if (!span->error) span->error = progress->mapping_errors[index];
    if (span->error || span->completed == progress->limits[index]) {
        progress->completion_emitted |= UINT32_C(1) << index;
        progress->completion->complete(progress->completion->context, index, span);
    }
}
static void ext4_file_read_completed(void *context, unsigned index, const struct block_read_span *span)
{
    struct file_read_workspace *progress = context;
    unsigned part = progress->owners[index];
    progress->fragments[part].error = span->status;
    progress->completion_ready |= UINT32_C(1) << part;
    ext4_file_read_publish(progress, progress->fragments[part].span);
}

struct ext4_file_read_plan {
    size_t *limits;
    struct file_read_fragment *fragments;
    struct block_read_span *reads;
    unsigned *owners;
    int *mapping_errors;
    unsigned capacity;
    struct file_read_workspace *progress;
};

static __attribute__((noinline)) int ext4_fpread_planned(const ext4_file *file,
    struct file_read_span *spans, unsigned count, const struct ext4_file_read_plan *plan,
    uint64_t size_limit, unsigned flags)
{
    size_t *limits = plan->limits;
    struct file_read_fragment *fragments = plan->fragments;
    struct block_read_span *reads = plan->reads;
    unsigned *owners = plan->owners;
    int *mapping_errors = plan->mapping_errors;
    unsigned capacity = plan->capacity;
    struct file_read_workspace *progress = plan->progress;
    const struct block_read_completion forward = {ext4_file_read_completed, progress};
    const struct ext4_lock *locks = file->mp->os_locks;
    if (!(flags & EXT4_PREAD_LOCK_HELD)) {
        if (locks && locks->read_lock) locks->read_lock(locks->context);
        else EXT4_MP_LOCK(file->mp);
    }
    int result = EOK;
    if (file->flags & O_WRONLY) { result = EPERM; goto Failed; }
    struct ext4_fs *fs = &file->mp->fs;
    struct ext4_inode_ref ref;
    result = ext4_fs_get_inode_ref_read(fs, file->inode, &ref);
    if (result) goto Failed;
    uint64_t size = ext4_inode_get_size(ext4_fs_super(fs), ref.inode);
    if (size > file->fmax) { result = EFBIG; goto Put; }
    if (size > size_limit) size = size_limit;
    unsigned block_size = ext4_sb_get_block_size(ext4_fs_super(fs));
    bool inline_link = ext4_inode_is_type(ext4_fs_super(fs), ref.inode, EXT4_INODE_MODE_SOFTLINK) &&
        size < sizeof(ref.inode->blocks) && !ext4_inode_get_blocks_count(ext4_fs_super(fs), ref.inode);
    for (unsigned i = 0; i < count; i++) {
        limits[i] = spans[i].offset >= size ? 0 : size - spans[i].offset < spans[i].size ?
            (size_t)(size - spans[i].offset) : spans[i].size;
        if (inline_link && limits[i]) {
            memcpy(spans[i].buffer, (uint8_t *)ref.inode->blocks + spans[i].offset, limits[i]);
            spans[i].completed = limits[i];
        }
    }
    struct ext4_inode_block_range range = {0};
    ext4_lblk_t range_start = 0;
    uint64_t range_offset = UINT64_MAX;
    while (!inline_link) {
        memset(mapping_errors, 0, count * sizeof(*mapping_errors));
        unsigned parts = 0, pending = 0;
        if (progress) for (unsigned i = 0; i < count; i++) progress->completion_cursor[i] = capacity;
        for (unsigned i = 0; i < count && parts < capacity; i++) {
            if (progress) progress->completion_cursor[i] = parts;
            size_t planned = spans[i].completed;
            while (!spans[i].error && planned < limits[i]) {
                uint64_t offset = spans[i].offset + planned;
                unsigned skip = offset % block_size;
                size_t length = block_size - skip;
                if (length > limits[i] - planned) length = limits[i] - planned;
                ext4_lblk_t logical = offset / block_size;
                if (offset != range_offset) range.count = 0;
                if (!range.count || logical - range_start >= range.count) {
                    uint64_t end = spans[i].offset + limits[i];
                    /* Output buffers do not bound an inode mapping run. Only
                     * adjacent requested file bytes may extend this window. */
                    for (unsigned j = i + 1; j < count && !(end % block_size); j++) {
                        if (!limits[j]) continue;
                        if (spans[j].error || spans[j].offset != end) break;
                        end += limits[j];
                    }
                    uint64_t needed = (end - 1) / block_size - logical + 1;
                    uint32_t maximum = needed > UINT32_MAX ? UINT32_MAX : needed;
                    int r = ext4_fs_get_inode_dblk_range(&ref, logical, maximum, false, &range);
                    if (r) { mapping_errors[i] = r; break; }
                    range_start = logical;
                }
                ext4_fsblk_t physical = range.unwritten || !range.block ? 0 :
                    range.block + (logical - range_start);
                if (!physical) {
                    uint64_t bytes = (uint64_t)(range.count - (logical - range_start)) * block_size - skip;
                    length = bytes < limits[i] - planned ? bytes : limits[i] - planned;
                }
                void *out = (uint8_t *)spans[i].buffer + planned;
                unsigned sector = fs->bdev->bdif->ph_bsize;
                uint64_t block = fs->bdev->part_offset + physical * block_size + skip;
                /* Cache targets own their EOF suffix. Use that real capacity
                 * for the last sector instead of allocating/copying scratch;
                 * logical completion remains length and the cache zeros EOF. */
                size_t io_length=length;
                if ((flags & EXT4_PREAD_PAD_EOF) && physical && !(skip%sector) && offset+length==size) {
                    size_t rounded=(length+sector-1)/sector*sector;
                    if (rounded<=block_size-skip && rounded<=spans[i].size-planned)
                        io_length=rounded;
                }
                bool aligned=physical && !(skip%sector) && !(io_length%sector);
                bool extend = parts && pending && aligned &&
                    fragments[parts - 1].span == i && owners[pending - 1] == parts - 1 &&
                    reads[pending - 1].offset + reads[pending - 1].size == block &&
                    (uint8_t *)reads[pending - 1].buffer + reads[pending - 1].size == out &&
                    io_length <= (uint64_t)UINT32_MAX * sector - reads[pending - 1].size;
                /* A sparse run has one immediate completion fragment, even
                 * when it spans many filesystem blocks. Keep its neighboring
                 * disk fragments in the same submission wave. */
                if (!physical && parts && fragments[parts - 1].span == i &&
                    !fragments[parts - 1].error &&
                    (!pending || owners[pending - 1] != parts - 1)) {
                    memset(out, 0, length);
                    fragments[parts - 1].length += length;
                    planned += length;
                    range_offset = offset + length;
                    continue;
                }
                if (parts == capacity) {
                    /* 片段已满时仍可扩展最后一个设备区间。空输出只查询当前
                     * 版本，不能把未接纳片段提前复制进调用者。 */
                    if (!extend || ext4_copy_pending_data(fs->bdev, physical, skip, NULL, 0)) break;
                    reads[pending - 1].size += io_length;
                    fragments[parts - 1].length += length;
                    planned += length;
                    range_offset = offset + length;
                    continue;
                }
                unsigned part = parts++;
                fragments[part] = (struct file_read_fragment){i, length, EOK};
                if (!physical) memset(out, 0, length);
                else if (!aligned)
                    fragments[part].error = ext4_read_pending_data(fs->bdev, physical, skip, out, length);
                else if (!ext4_copy_pending_data(fs->bdev, physical, skip, out, length)) {
                    if (extend) {
                        parts--;
                        reads[pending - 1].size += io_length;
                        fragments[parts - 1].length += length;
                    }
                    else {
                        owners[pending] = part;
                        reads[pending++] = (struct block_read_span){block, out, io_length, 0, EIO};
                    }
                }
                planned += length;
                range_offset = offset + length;
            }
        }
        if (progress) {
            progress->completion_parts = parts;
            progress->completion_ready = parts == 32 ? UINT32_MAX : (UINT32_C(1) << parts) - 1;
            for (unsigned j = 0; j < pending; j++) progress->completion_ready &= ~(UINT32_C(1) << owners[j]);
            if (pending && progress->completion_ready)
                for (unsigned i = 0; i < count; i++) ext4_file_read_publish(progress, i);
        }
        ext4_blockdev_read_batch(fs->bdev, reads, pending, progress ? &forward : NULL);
        for (unsigned j = 0; j < pending; j++) fragments[owners[j]].error = reads[j].status;
        /* DMA is drained: fold remaining fragments once. Early callbacks have
         * already consumed each span's prefix; do not replay its publisher. */
        for (unsigned j = 0; j < parts; j++) {
            struct file_read_fragment *part = &fragments[j];
            if (progress) {
                unsigned *cursor = &progress->completion_cursor[part->span];
                if (j < *cursor) continue;
                *cursor = j + 1;
            }
            struct file_read_span *span = &spans[part->span];
            if (!span->error) {
                if (part->error) span->error = part->error;
                else span->completed += part->length;
            }
        }
        int more = 0;
        for (unsigned i = 0; i < count; i++) {
            if (!spans[i].error) spans[i].error = mapping_errors[i];
            more |= !spans[i].error && spans[i].completed < limits[i];
        }
        /* 本轮已排空但还要继续映射/读下一窗时，先交付已经完整的页。 */
        if (progress && more)
            for (unsigned i = 0; i < count; i++) ext4_file_read_publish(progress, i);
        if (!more) break;
    }
Put:
    { int r = ext4_fs_put_inode_ref(&ref); if (!result) result = r; }
Failed:
    for (unsigned i = 0; i < count; i++) {
        if (progress && (progress->completion_emitted & (UINT32_C(1) << i))) continue;
        if (result && !spans[i].error) spans[i].error = result;
    }
    for (unsigned i = 0; i < count; i++) if (!result) result = spans[i].error;
    if (!(flags & EXT4_PREAD_LOCK_HELD)) EXT4_MP_UNLOCK(file->mp);
    return result;
}

/* Keep small positioned reads off the largest scalar frame. The fixed-size
 * entry owns its descriptors until the common executor has drained DMA. */
#define EXT4_SCALAR_READ(capacity) \
static __attribute__((noinline)) int ext4_fpread_scalar##capacity(const ext4_file *file, \
    struct file_read_span *span, uint64_t size_limit, unsigned flags) \
{ \
    size_t limit; \
    struct file_read_fragment fragments[capacity]; \
    struct block_read_span reads[capacity]; \
    unsigned owners[capacity]; \
    int mapping_error; \
    const struct ext4_file_read_plan plan = { \
        &limit, fragments, reads, owners, &mapping_error, capacity, NULL \
    }; \
    return ext4_fpread_planned(file, span, 1, &plan, size_limit, flags); \
}
EXT4_SCALAR_READ(1)
EXT4_SCALAR_READ(2)
EXT4_SCALAR_READ(4)
EXT4_SCALAR_READ(8)
#undef EXT4_SCALAR_READ

int ext4_fpread_batch(const ext4_file *file, struct file_read_span *spans, unsigned count,
    struct file_read_workspace *work, uint64_t size_limit, const struct file_read_completion *completion, unsigned flags)
{
    if (!file || !file->mp || count > EXT4_BLOCK_BATCH_MAX || (count && !spans)) return EINVAL;
    for (unsigned i = 0; i < count; i++) { spans[i].completed = 0; spans[i].error = EINVAL; }
    uintptr_t descriptors = (uintptr_t)spans, workspace = (uintptr_t)work;
    if (work && count && (descriptors < workspace ? workspace - descriptors < count * sizeof(*spans) :
        descriptors - workspace < sizeof(*work))) return EINVAL;
    uintptr_t first = UINTPTR_MAX, last = 0;
    for (unsigned i = 0; i < count; i++) {
        uintptr_t a = (uintptr_t)spans[i].buffer;
        size_t size = spans[i].size;
        if (spans[i].offset > UINT64_MAX - size || (size && (!a || size - 1 > UINTPTR_MAX - a)))
            return spans[i].error = EINVAL;
        uintptr_t b = (uintptr_t)spans;
        if (size && (a < b ? b - a < size : a - b < count * sizeof(*spans)))
            return spans[i].error = EINVAL;
        if (work && size && (a < workspace ? workspace - a < size : a - workspace < sizeof(*work)))
            return spans[i].error = EINVAL;
        if (!size) continue;
        uintptr_t end = a + size - 1;
        /* Sorted SG targets lie outside the envelope. Check individual spans
         * only when a target enters its gaps or overlaps an earlier target. */
        if (a <= last && end >= first) {
            for (unsigned j = 0; j < i; j++) {
                b = (uintptr_t)spans[j].buffer;
                if (spans[j].size && (a < b ? b - a < size : a - b < spans[j].size))
                    return spans[i].error = EINVAL;
            }
        }
        if (a < first) first = a;
        if (end > last) last = end;
    }
    for (unsigned i = 0; i < count; i++) spans[i].error = EOK;
    if (!count) return EOK;
    if (count > 1 && !work) {
        for (unsigned i = 0; i < count; i++) spans[i].error = ENOMEM;
        return ENOMEM;
    }
    if (work) {
        struct file_read_workspace *progress = completion && count > 1 ? work : NULL;
        if (progress) {
            progress->completion_spans = spans;
            progress->completion = completion;
            progress->completion_emitted = 0;
        }
        const struct ext4_file_read_plan plan = {
            work->limits, work->fragments, work->reads, work->owners,
            work->mapping_errors, EXT4_BLOCK_BATCH_MAX, progress
        };
        return ext4_fpread_planned(file, spans, count, &plan, size_limit, flags);
    }
    unsigned bs = ext4_sb_get_block_size(&file->mp->fs.sb);
    uint64_t blocks = spans[0].size / bs;
    if (blocks < 8)
        blocks += (spans[0].offset % bs + spans[0].size % bs + bs - 1) / bs;
    if (blocks <= 1) return ext4_fpread_scalar1(file, spans, size_limit, flags);
    if (blocks <= 2) return ext4_fpread_scalar2(file, spans, size_limit, flags);
    if (blocks <= 4) return ext4_fpread_scalar4(file, spans, size_limit, flags);
    return ext4_fpread_scalar8(file, spans, size_limit, flags);
}

/* Direct writes borrow the input until device completion. A private journaled
 * allocation remains unreachable until its metadata is committed. */
struct ext4_direct_write_batch {
    struct kernel_block_span blocks[EXT4_BLOCK_BATCH_MAX];
    struct {
        struct file_write_span *span;
        uint64_t offset;
        size_t length;
        bool allocated;
    } logical[EXT4_BLOCK_BATCH_MAX];
    unsigned count;
    size_t bytes;
};
/* Drop the highest mapped blocks before any queued prefix. Releasing a middle
 * block can split an extent and require metadata allocation during error repair. */
static int ext4_write_discard_range(struct ext4_inode_ref *ref,
    struct ext4_inode_block_range *range, ext4_lblk_t next, int result)
{
    if (!range->allocated) return result;
    while (range->count) {
        range->count--;
        int cleanup = ext4_fs_release_inode_dblk_idx(ref,
            next + range->count, range->block + range->count);
        if (!result) result = cleanup;
    }
    return result;
}
static int ext4_direct_write_flush(struct ext4_inode_ref *ref, ext4_file *file,
    struct ext4_direct_write_batch *batch, struct ext4_inode_block_range *range,
    ext4_lblk_t range_next)
{
    if (!batch || !batch->count) return EOK;
    int r = ext4_trans_write_data_batch(ref->fs->bdev, batch->blocks, batch->count);
    bool rollback = ext4_current_trans(ref->fs) != NULL;
    if (r && !rollback) r = ext4_write_discard_range(ref, range, range_next, r);
    unsigned block_size = ref->fs->bdev->lg_bsize;
    for (unsigned step = 0; step < batch->count; step++) {
        unsigned i = r ? batch->count - 1 - step : step;
        if (!r) {
            batch->logical[i].span->written += batch->logical[i].length;
            uint64_t end = batch->logical[i].offset + batch->logical[i].length;
            if (end > ext4_inode_get_size(ext4_fs_super(ref->fs), ref->inode)) {
                ext4_inode_set_size(ref->inode, end); ref->dirty = true;
            }
        } else if (!rollback && batch->logical[i].allocated) {
            /* The mapped-ahead suffix is gone; now remove queued blocks from
             * the end while preserving the device error as the operation error. */
            for (uint32_t j = batch->blocks[i].size / block_size; j > 0; j--) {
                int cleanup = ext4_fs_release_inode_dblk_idx(ref,
                    batch->logical[i].offset / block_size + j - 1, batch->blocks[i].offset / block_size + j - 1);
                if (!r) r = cleanup;
            }
        }
    }
    if (r) file->fpos = batch->logical[0].offset;
    batch->count = 0; batch->bytes = 0;
    return r;
}

static int ext4_write_spans(ext4_file *file, ext4_file *owner,
    struct file_write_span *spans, unsigned count, bool synchronous,
    const struct file_data_source **sources)
{
	if (!file || !file->mp || count > EXT4_FILE_WRITEBACK_MAX || (count && !spans)) return EINVAL;
	for (unsigned i = 0; i < count; i++) spans[i].written = 0;
	if (file->mp->fs.read_only) return EROFS;
	if (file->flags & O_RDONLY) return EPERM;
	uint64_t write_end = 0;
	size_t blocks = 0, data_images = 0;
	unsigned source_count = 0;
	unsigned block_size = ext4_sb_get_block_size(ext4_fs_super(&file->mp->fs));
	for (unsigned i = 0; i < count; i++) {
		struct file_write_span *span = &spans[i];
		if ((!span->buffer && span->size) || !!span->source.retain != !!span->source.release ||
		    !!span->source.retain != !!span->source.bytes) return EINVAL;
		if (span->offset > file->fmax || span->size > file->fmax - span->offset) return EFBIG;
		if (i && span->offset < spans[i-1].offset + spans[i-1].size) return EINVAL;
		if (!span->size) continue;
        size_t leading = span->offset % block_size;
        size_t n = (span->size - 1 + leading) / block_size + 1;
        if (n > SIZE_MAX - blocks) return ENOMEM;
        blocks += n;
        size_t borrowed = 0;
        if (span->source_buffer || span->source_size) {
            uintptr_t base = (uintptr_t)span->source_buffer, input = (uintptr_t)span->buffer;
            if (!base || !span->source.retain || input < base ||
                input - base > span->source_size || span->size > span->source_size - (input - base) ||
                span->source_size > span->source.bytes || base > UINTPTR_MAX - span->source_size ||
                input - base > span->offset) return EINVAL;
            size_t delta = input - base;
            size_t first = delta >= leading ? delta - leading : block_size - (leading - delta);
            size_t end = delta + span->size;
            if (first < end && first <= span->source_size && block_size <= span->source_size - first) {
                size_t covered = (span->source_size - first) / block_size;
                size_t touched = (end - first - 1) / block_size + 1;
                borrowed = covered < touched ? covered : touched;
            }
        } else if (span->source.retain) {
            size_t skip = leading ? block_size - leading : 0;
            if (skip < span->size) borrowed = (span->size - skip) / block_size;
        }
        if (borrowed) sources[source_count++] = &span->source;
        data_images += n - borrowed;
        write_end = span->offset + span->size;
	}
	if (!blocks) return EOK;
	struct jbd_journal *journal = file->mp->fs.jbd_journal;
	struct ext4_operation *operation=ext4_current_operation(&file->mp->fs);
	bool outer=!operation;
	struct ext4_small_writeback *batch=outer?NULL:operation->small_writeback;
	int r = outer || batch ? EOK : ext4_track_nested_file(owner);
	if (r) return r;
Retry:
	r = batch ? EOK : ext4_prepare_growth(file, write_end);
	if (!r && journal && journal->grouped && outer) {
		while (!r) {
            struct jbd_journal_reclaim goal;
            {
            EXT4_STATE_READ_SCOPE(state, &file->mp->fs);
            unsigned resources = jbd_journal_operation_resources(journal, NULL,
                blocks, data_images, sources, source_count);
            if (!resources) break;
            r = ext4_group_reclaim_goal(file->mp, resources, &goal);
            }
            if (r) break;
            r = ext4_group_wait_reclaim(file->mp, &goal);
		}
	}
	if (!r) r = ext4_trans_start(file->mp);
	if (r) return r;
	struct ext4_fs *fs = &file->mp->fs;
	struct ext4_inode_ref ref;
	uint64_t saved_size = file->fsize, saved_pos = file->fpos;
	r = ext4_fs_get_inode_ref(fs, file->inode, &ref);
	if (r) { r = ext4_file_data_finish(file, r); goto Completed; }
	saved_size = ext4_inode_get_size(ext4_fs_super(fs), ref.inode);
    if (batch && batch->completion && write_end < saved_size) batch->completion=NULL;
    if (file->delalloc && journal) {
        EXT4_STATE_SCOPE(state,fs);
        /* 在同一次预留遍历中清除即将修改的映射知识；abort 后重新查询。 */
        uint64_t credit=delalloc_spans(file->delalloc,spans,count,block_size,false);
        if (batch) operation->delalloc_credit+=credit;
        else if (__atomic_load_n(&file->delalloc->blocks,__ATOMIC_RELAXED)) {
            /* Adapter writeback commits its own operation before returning spans. */
            if (!outer)__builtin_trap();
            struct ext4_operation *operation=ext4_current_operation(fs);
            if (operation->delalloc)__builtin_trap();
            operation->delalloc=file->delalloc;operation->delalloc_spans=spans;
            operation->delalloc_span_count=count;operation->delalloc_credit=credit;
        }
    }
    struct ext4_direct_write_batch *direct = NULL;
    bool fresh_ordered = journal && journal->grouped && outer && file->delalloc &&
#if BOAROS_MAX_CPUS == 1
        /* A full page-cache window can use one existing device batch. */
        (file->mp->journal_runtime.prewrite_fresh || blocks >= (128U * 1024U) / block_size) &&
        source_count &&
#else
        file->mp->journal_runtime.prewrite_fresh && source_count &&
#endif
        spans[0].offset >= saved_size && fs->bdev->bdif->bwrite_batch;
    if ((!journal || fresh_ordered) && blocks > 1 && (source_count || synchronous)) {
        direct = ext4_calloc(1, sizeof(*direct));
        if (!direct && !journal) r = ENOMEM;
    }
    struct ext4_inode_block_range range = {0};
    ext4_lblk_t range_next = 0;
    uint64_t mapping_end = 0;
	for (unsigned i = 0; i < count && !r; i++) {
		struct file_write_span *span = &spans[i];
		if (!span->size) continue;
		file->fpos = span->offset;
        uint64_t old_size = ext4_inode_get_size(ext4_fs_super(fs), ref.inode);
        if (direct && direct->count) {
            unsigned last = direct->count - 1;
            uint64_t pending_end = direct->logical[last].offset + direct->logical[last].length;
            if (old_size < pending_end) old_size = pending_end;
        }
        size_t source_offset = span->source_buffer ?
            (uintptr_t)span->buffer - (uintptr_t)span->source_buffer : 0;
        size_t block_offset = span->offset % block_size;
        bool covers_eof = span->source_buffer && old_size / block_size == span->offset / block_size &&
            source_offset >= block_offset && source_offset - block_offset <= span->source_size &&
            block_size <= span->source_size - (source_offset - block_offset);
        /* The supplied block already includes the zero gap after old EOF. */
        if (!covers_eof && span->offset > old_size && old_size % block_size) {
            r = ext4_direct_write_flush(&ref, file, direct, &range, range_next);
            if (!r) r = ext4_zero_allocated_eof_tail_mode(&ref, old_size, span->offset, synchronous);
        }
        size_t consumed = 0;
		while (consumed < span->size && !r) {
            if (direct && (direct->bytes == 131072 || direct->count == EXT4_BLOCK_BATCH_MAX)) {
                r = ext4_direct_write_flush(&ref, file, direct, &range, range_next);
                if (r) break;
            }
			uint32_t offset = file->fpos % block_size;
			size_t length = block_size - offset;
			if (length > span->size - consumed) length = span->size - consumed;
			ext4_fsblk_t physical;
			bool allocated;
			ext4_lblk_t logical_block = file->fpos / block_size;
            ext4_assert(!range.count || range_next == logical_block);
			if (!range.count) {
                /* Form each contiguous request run once. A fragmented physical
                 * map may need several queries without rescanning later spans. */
                if (file->fpos >= mapping_end) {
                    mapping_end = span->offset + span->size;
                    for (unsigned j = i + 1; j < count && !(mapping_end % block_size); j++) {
                        if (!spans[j].size) continue;
                        if (spans[j].offset != mapping_end) break;
                        mapping_end += spans[j].size;
                    }
                }
                uint64_t needed = (mapping_end - 1) / block_size - logical_block + 1;
                uint32_t maximum = fresh_ordered ?
                    (UINT32_C(524288) / block_size) :
                    (UINT32_C(131072) - (direct ? direct->bytes : 0)) / block_size;
                if (needed < maximum) maximum = needed;
                r = ext4_fs_get_inode_dblk_range(&ref, logical_block, maximum, true, &range);
                if (r) break;
                if (batch && batch->completion && !ext4_collect_physical(batch->completion,&range))
                    batch->completion=NULL;
                range_next = logical_block;
            }
            physical = range.block;
            allocated = range.allocated;
            const uint8_t *input = (const uint8_t *)span->buffer + consumed;
            uint32_t data_offset = offset, data_length = length;
            if (span->source_buffer) {
                size_t position = (uintptr_t)input - (uintptr_t)span->source_buffer;
                if (position >= offset && position - offset <= span->source_size &&
                    block_size <= span->source_size - (position - offset)) {
                    input -= offset; data_offset = 0; data_length = block_size;
                }
            }
            /* Only new bitmap claims may bypass the ordered index. Quarantine
             * prevents reuse of an old journal incarnation. The private extent
             * remains hidden through the write, and the journal's precommit
             * flush orders these device-completed bytes before the commit. */
            bool queued = direct && (!journal || (allocated && !offset && length == block_size)) &&
                !data_offset && data_length == block_size && (span->source.retain || synchronous);
            if (queued) {
                unsigned n = direct->count;
                bool merge = n && direct->logical[n-1].span == span &&
                    direct->logical[n-1].allocated == allocated &&
                    direct->blocks[n-1].offset + direct->blocks[n-1].size == physical * block_size &&
                    (const uint8_t *)direct->blocks[n-1].buffer + direct->blocks[n-1].size == input;
                bool overlap = n && physical * block_size >= direct->blocks[n-1].offset &&
                    physical * block_size - direct->blocks[n-1].offset < direct->blocks[n-1].size;
                if ((!merge && n == EXT4_BLOCK_BATCH_MAX) || overlap || direct->bytes + block_size > 131072) {
                    r = ext4_direct_write_flush(&ref, file, direct, &range, range_next); n = 0; merge = false;
                }
                if (!r && merge) {
                    direct->blocks[n-1].size += block_size;
                    direct->logical[n-1].length += length;
                } else if (!r) {
                    direct->blocks[n] = (struct kernel_block_span){physical * block_size, input, block_size};
                    direct->logical[n].span = span; direct->logical[n].offset = file->fpos;
                    direct->logical[n].length = length; direct->logical[n].allocated = allocated;
                    direct->count++;
                }
                if (!r) direct->bytes += block_size;
            } else {
                r = ext4_direct_write_flush(&ref, file, direct, &range, range_next);
                if (!r) r = synchronous
                    ? ext4_trans_write_data_sync(fs->bdev, physical, data_offset, input, data_length, allocated)
                    : ext4_trans_write_data(fs->bdev, physical, data_offset, input, data_length, allocated, &span->source);
            }
			if (r) break;
            consumed += length;
            range.block++; range.count--; range_next++;
			if (!queued) span->written += length;
			file->fpos += length;
			if (!queued && file->fpos > ext4_inode_get_size(ext4_fs_super(fs), ref.inode)) {
				ext4_inode_set_size(ref.inode, file->fpos); ref.dirty = true;
			}
		}
	}
    /* A journal abort drops the private extent tree and every physical claim.
     * The direct path instead keeps any device-completed prefix. */
    if (r && !journal) r = ext4_write_discard_range(&ref, &range, range_next, r);
    int direct_result = journal && r ? r : ext4_direct_write_flush(&ref, file, direct, &range, range_next);
    if (!r) r = direct_result;
    ext4_free(direct);
    struct ext4_timestamp times[3];
    unsigned fields = !r && file->mp->inode_observer.snapshot
        ? file->mp->inode_observer.snapshot(file->time_owner, times) : 0;
    if (!r && fields) {
        if (fields & EXT4_TIME_ATIME) ext4_time_store(&ref,
            offsetof(struct ext4_inode, access_time), offsetof(struct ext4_inode, atime_extra), times[0]);
        if (fields & EXT4_TIME_MTIME) ext4_time_store(&ref,
            offsetof(struct ext4_inode, modification_time), offsetof(struct ext4_inode, mtime_extra), times[1]);
        if (fields & EXT4_TIME_CTIME) ext4_time_store(&ref,
            offsetof(struct ext4_inode, change_inode_time), offsetof(struct ext4_inode, ctime_extra), times[2]);
    }
	file->fsize = ext4_inode_get_size(ext4_fs_super(fs), ref.inode);
    if (batch && !r) {
        struct ext4_small_writeback_result *saved=&batch->results[batch->prepared];
        saved->size=file->fsize;
        saved->fields=fields;
        if (fields) memcpy(saved->times,times,sizeof(saved->times));
    }
	int cleanup = ext4_fs_put_inode_ref(&ref);
	if (!r) r = cleanup;
	r = ext4_file_data_finish(file, r);
	if (!ext4_operation_depth(&file->mp->fs) && file->mp->inode_observer.written) {
		file->mp->inode_observer.written(file->time_owner, fields, times, r);
	}
Completed:
	if (ext4_file_rolled_back(file, r)) {
		file->fsize = saved_size; file->fpos = saved_pos;
		for (unsigned i = 0; i < count; i++) spans[i].written = 0;
		if (r == EAGAIN && outer && ext4_operation_resource_retry(&file->mp->fs)) goto Retry;
        if (outer && ext4_operation_retry_reclaim(file->mp, &r)) goto Retry;
	}
	return r;
}

static __attribute__((noinline)) int ext4_write_spans_small(ext4_file *file,
    ext4_file *owner, struct file_write_span *spans, unsigned count, bool synchronous)
{
    const struct file_data_source *sources[EXT4_BLOCK_BATCH_MAX];
    return ext4_write_spans(file, owner, spans, count, synchronous, sources);
}

int ext4_fwrite(ext4_file *file, const void *buffer, size_t size, size_t *written)
{
	if (!file || !file->mp) return EINVAL;
	EXT4_MP_LOCK(file->mp);
	struct file_write_span span = {.offset=file->fpos, .buffer=buffer, .size=size};
	int r = ext4_write_spans_small(file, file, &span, 1, false);
	if (written) *written = span.written;
	EXT4_MP_UNLOCK(file->mp);
	return r;
}

static int ext4_fpwrite_batch_mode(ext4_file *file, struct file_write_span *spans, unsigned count, bool synchronous)
{
	if (!file || !file->mp || count > EXT4_FILE_WRITEBACK_MAX || (count && !spans)) return EINVAL;
    const struct file_data_source **sources = NULL;
    if (count > EXT4_BLOCK_BATCH_MAX) {
        sources = ext4_malloc(count * sizeof(*sources));
        if (!sources) {
            for (unsigned i = 0; i < count; i++) spans[i].written = 0;
            return ENOMEM;
        }
    }
	EXT4_MP_LOCK(file->mp);
    ext4_file local;
    uint64_t initial_size;
    {
        EXT4_STATE_SCOPE(state, &file->mp->fs);
        local = *file;
        initial_size = file->fsize;
    }
    int r = sources ? ext4_write_spans(&local, file, spans, count, synchronous, sources) :
        ext4_write_spans_small(&local, file, spans, count, synchronous);
    ext4_file_drain_reservations(file);
    {
        EXT4_STATE_SCOPE(state, &file->mp->fs);
        /* Positioned data writes only extend EOF. Shrink/space operations hold
         * exclusive inode I/O qualification until this shared writer returns. */
        if (file->fsize == initial_size || file->fsize < local.fsize) file->fsize = local.fsize;
    }
	EXT4_MP_UNLOCK(file->mp);
    ext4_free(sources);
	return r;
}

int ext4_fpwrite_batch(ext4_file *file, struct file_write_span *spans, unsigned count)
{ return ext4_fpwrite_batch_mode(file, spans, count, false); }

int ext4_fpwrite_files(ext4_file *const *files, const struct file_write_set *sets,
    unsigned count, int *data_result)
{
    if (count<2 || count>EXT4_SMALL_WRITEBACK_MAX) return EINVAL;
    if (data_result) *data_result=ENOTSUP;
    struct ext4_mountpoint *mp=files[0]->mp;
    struct ext4_fs *fs=&mp->fs;
    EXT4_MP_LOCK(mp);
    int result=ENOTSUP;
    if (!fs->jbd_journal || !fs->jbd_journal->grouped ||
        !ext4_fs_serial_writer(fs) || ext4_current_operation(fs)) goto Unlock;
    /* Only complete from the new mappings when the prepared spans cover
     * every logical block through EOF. The serial mount qualification keeps
     * those mappings stable from transaction acceptance through home I/O. */
    bool collect=data_result && fs->bdev->bdif->bwrite_phase;
    for (unsigned i=0;i<count;i++) {
        ext4_file *file=files[i];
        bool pending;
        if (file->mp!=mp || !file->delalloc || !sets[i].count ||
            sets[i].count>EXT4_BLOCK_BATCH_MAX) goto Unlock;
        bool journaled=false;
        unsigned block=fs->bdev->lg_bsize;
        for (unsigned j=0;j<sets[i].count;j++) {
            const struct file_write_span *span=&sets[i].spans[j];
            if (span->offset>file->fsize || span->size>file->fsize-span->offset ||
                span->offset%block || span->size%block) {
                journaled=true;
                break;
            }
        }
        if (!journaled) {
            /* Leave alignable stable mappings to the direct path. */
            struct ext4_inode_goal probe={.inode=file->inode};
            struct ext4_inode_goal *goal=RB_FIND(ext4_inode_goal_tree,&fs->inode_goals,&probe);
            if (!goal || goal->data_sequence<=fs->jbd_journal->durable_sequence) goto Unlock;
        }
        if (collect) {
            uint64_t next=0;
            for (unsigned j=0;j<sets[i].count;j++) {
                const struct file_write_span *span=&sets[i].spans[j];
                if (!span->size || span->offset!=next || span->size>UINT64_MAX-next) {
                    collect=false;
                    break;
                }
                next+=span->size;
            }
            if (next<file->fsize) collect=false;
        }
        result=ext4_orphan_truncate_pending(fs,file->inode,&pending);
        if (result) goto Unlock;
        result=ENOTSUP;
        if (pending) goto Unlock;
    }
    struct ext4_data_completion completion={0};
    struct ext4_small_writeback *batch=ext4_malloc(sizeof(*batch));
    if (!batch) {result=ENOMEM;goto Unlock;}
    result=ext4_trans_start_mode(mp,false);
    if (!result) {
        batch->files=files;
        batch->sets=sets;
        batch->completion=collect?&completion:NULL;
        batch->prepared=0;
        batch->allocation.allocated=0;
        ext4_current_operation(fs)->small_writeback=batch;
        while (batch->prepared<count) {
            unsigned i=batch->prepared;
            ext4_file local=*files[i];
            result=ext4_write_spans_small(&local,files[i],sets[i].spans,sets[i].count,false);
            if (result) break;
            batch->prepared++;
        }
        result=ext4_balloc_finish_allocation(&batch->allocation,result);
        result=ext4_trans_finish(mp,result);
        if (!result) {
            EXT4_STATE_SCOPE(state,fs);
            for (unsigned i=0;i<count;i++) files[i]->fsize=batch->results[i].size;
        }
        for (unsigned i=0;i<batch->prepared;i++) {
            ext4_file_drain_reservations(files[i]);
            if (!result && mp->inode_observer.written) {
                const struct ext4_small_writeback_result *saved=&batch->results[i];
                mp->inode_observer.written(files[i]->time_owner,saved->fields,saved->times,EOK);
            }
        }
        if (!result && batch->completion) {
            *data_result=ext4_complete_physical(fs->jbd_journal,&completion);
            if (*data_result==ENOTSUP) *data_result=EIO;
        }
    }
    ext4_free(batch);
Unlock:
    EXT4_MP_UNLOCK(mp);
    if (result) for (unsigned i=0;i<count;i++)
        for (unsigned j=0;j<sets[i].count;j++) sets[i].spans[j].written=0;
    return result;
}

static int ext4_write_mapped(ext4_file *file, struct file_write_span *spans, unsigned count,
    struct ext4_block_write *writes)
{
    struct ext4_fs *fs = &file->mp->fs;
    struct jbd_journal *journal = fs->jbd_journal;
    size_t block = fs->bdev->lg_bsize;
    if (!journal || !journal->grouped || !fs->bdev->bdif->bwrite_phase ||
        !count || fs->read_only)
        return ENOTSUP;
    for (unsigned i = 0; i < count; i++)
        if (spans[i].offset % block || !spans[i].size || spans[i].size % block) return ENOTSUP;

    EXT4_MP_LOCK(file->mp);
    int result;
    {
        EXT4_STATE_READ_SCOPE(state, fs);
        result = jbd_journal_error(journal);
        struct ext4_inode_goal probe = {.inode = file->inode};
        struct ext4_inode_goal *goal = RB_FIND(ext4_inode_goal_tree, &fs->inode_goals, &probe);
        if (!result && (ext4_current_operation(fs) ||
            (goal && goal->data_sequence > journal->durable_sequence))) result = ENOTSUP;
    }
    if (result) goto out;

    struct ext4_inode_ref ref;
    result = ext4_fs_get_inode_ref_read(fs, file->inode, &ref);
    if (result) goto out;
    uint64_t end = ext4_inode_get_size(ext4_fs_super(fs), ref.inode);
    struct ext4_inode_block_range range = {0};
    uint64_t mapped = 0, run_end = 0;
    for (unsigned i = 0; i < count; i++) {
        uint64_t offset = spans[i].offset;
        size_t size = spans[i].size;
        if (offset > end || size > end - offset || (offset + size - 1) / block > UINT32_MAX ||
            size / block > UINT32_MAX) {
            result = ENOTSUP;
            break;
        }
        unsigned blocks = size / block;
        if (range.count < blocks || mapped != offset) {
            if (mapped != offset || offset >= run_end) {
                run_end = offset + size;
                for (unsigned j = i + 1; j < count && spans[j].offset == run_end &&
                    spans[j].size <= end - run_end; j++)
                    run_end += spans[j].size;
            }
            uint64_t consecutive = (run_end - offset) / block;
            if (consecutive > UINT32_MAX) consecutive = UINT32_MAX;
            result = ext4_fs_get_inode_dblk_range(&ref, offset / block,
                consecutive, false, &range);
            if (result) break;
            if (!range.block || range.unwritten || range.count < blocks) {
                result = ENOTSUP;
                break;
            }
        }
        writes[i] = (struct ext4_block_write){
            .offset = fs->bdev->part_offset + range.block * block,
            .data = spans[i].buffer, .size = size,
            .next = i + 1 < count ? &writes[i+1] : NULL};
        mapped = offset + size;
        range.block += blocks;
        range.count -= blocks;
    }
    result = ext4_result(result, ext4_fs_put_inode_ref(&ref));
out:
    EXT4_MP_UNLOCK(file->mp);
    if (result) return result;
    /* The VFS inode reader excludes truncate and hole punching through I/O.
     * The durable data goal excludes older ordered payloads for these blocks. */
    result = ext4_blocks_set_phase(fs->bdev, writes);
    if (!result) {
        for (unsigned i = 0; i < count; i++) spans[i].written = spans[i].size;
    } else if (result == ENOTSUP) result = EIO;
    return result;
}

static __attribute__((noinline)) int ext4_write_mapped_small(ext4_file *file,
    struct file_write_span *spans, unsigned count)
{
    struct ext4_block_write writes[EXT4_BLOCK_BATCH_MAX];
    return ext4_write_mapped(file, spans, count, writes);
}

int ext4_fpwrite_mapped_batch(ext4_file *file, struct file_write_span *spans, unsigned count)
{
    if (count <= EXT4_BLOCK_BATCH_MAX)
        return ext4_write_mapped_small(file, spans, count);
    if (count > EXT4_FILE_WRITEBACK_MAX) return ENOTSUP;
    struct ext4_block_write *writes = ext4_malloc(count * sizeof(*writes));
    if (!writes) return ENOTSUP;
    int result = ext4_write_mapped(file, spans, count, writes);
    ext4_free(writes);
    return result;
}

/* Nonjournal synchronous storage is borrowed only until device completion.
 * Journal users need the ordered-owner completion protocol instead. */
int ext4_fpwrite_direct(ext4_file *file, struct file_write_span *span)
{
    if (!file || !file->mp || file->mp->fs.jbd_journal) return EINVAL;
    return ext4_fpwrite_batch_mode(file, span, 1, true);
}

int ext4_fpwrite(ext4_file *file, uint64_t offset, const void *buffer, size_t size, size_t *written)
{
	struct file_write_span span = {.offset=offset, .buffer=buffer, .size=size};
	int r = ext4_fpwrite_batch(file, &span, 1);
	if (written) *written = span.written;
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

static void copy_inode_snapshot(const struct ext4_inode_ref *ref,struct ext4_inode *inode)
{
    size_t size=ext4_get16(ext4_fs_super(ref->fs),inode_size);
    if (size>sizeof(*inode)) size=sizeof(*inode);
    memcpy(inode,ref->inode,size);
    memset((uint8_t *)inode+size,0,sizeof(*inode)-size);
}

static int read_inode_snapshot(struct ext4_mountpoint *mp,uint32_t number,struct ext4_inode *inode)
{
    int r;struct ext4_inode_ref inode_ref;
	EXT4_MP_LOCK(mp);
	r = ext4_fs_get_inode_ref_read(&mp->fs, number, &inode_ref);
	if (r != EOK) {
		EXT4_MP_UNLOCK(mp);
		return r;
	}
	copy_inode_snapshot(&inode_ref,inode);
	r = ext4_fs_put_inode_ref(&inode_ref);
	EXT4_MP_UNLOCK(mp);
	return r;
}
int ext4_fraw_inode_fill(const ext4_file *file,struct ext4_inode *inode)
{
    if (!file || !inode || !file->mp || !file->mp->mounted) return EINVAL;
    return read_inode_snapshot(file->mp,file->inode,inode);
}

bool ext4_file_copy_cached_inode(const ext4_file *file,uint32_t index,struct ext4_inode *inode)
{ return ext4_fs_copy_cached_inode(&file->mp->fs,index,inode); }

int ext4_file_sync_metadata_prepare(ext4_file *file, bool data_only, uint64_t *target)
{
	struct ext4_inode_ref ref;
	int r, cleanup;
	if (!file || !file->mp || !file->mp->mounted || !target)
		return EINVAL;
	*target = 0;
#if CONFIG_JOURNALING_ENABLE
    struct ext4_journal_runtime *runtime = &file->mp->journal_runtime;
    bool join = runtime->join_pending_metadata && runtime->now_ns &&
        runtime->join_pending_metadata(runtime->context);
#endif
	EXT4_MP_LOCK(file->mp);
#if CONFIG_JOURNALING_ENABLE
    if (file->mp->fs.jbd_journal) {
        struct jbd_journal *journal = file->mp->fs.jbd_journal;
        unsigned work = EXT4_JOURNAL_WORK_COMMIT;
        bool request = false;
        {
            EXT4_STATE_SCOPE(state, &file->mp->fs);
            r = ext4_operation_depth(&file->mp->fs) ? EBUSY : jbd_journal_error(journal);
            struct ext4_inode_goal probe = {.inode = file->inode};
            struct ext4_inode_goal *goal = RB_FIND(ext4_inode_goal_tree,
                &file->mp->fs.inode_goals, &probe);
            uint64_t sequence = goal ? (data_only ? goal->data_sequence : goal->full_sequence) : 0;
            if (!r && journal->grouped && journal->durable_sequence < sequence) {
                *target = sequence;
                if (journal->seal_target < sequence) {
                    journal->seal_target = sequence;
                    journal->sync_join_deadline_ns = 0;
                    if (join && journal->running && journal->running->sequence == sequence &&
                        !journal->running->ordered_count && !journal->committing &&
                        TAILQ_EMPTY(&journal->sealed_queue)) {
                        /* Let already-dirty inode timestamps join before
                         * paying for another metadata-only commit. */
                        uint64_t now = runtime->now_ns(runtime->context);
                        journal->sync_join_deadline_ns = now + UINT64_C(4000000);
                        work |= EXT4_JOURNAL_WORK_JOIN_TIMES;
                    }
                }
                if (!join && journal->running && journal->running->sequence == sequence)
                    journal->sync_join_deadline_ns = 0;
                request = true;
            } else if (!r && !journal->grouped) r = jbd_journal_sync(journal, journal->committed_id);
        }
        if (request && file->mp->journal_runtime.request)
            file->mp->journal_runtime.request(file->mp->journal_runtime.context, work);
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

int ext4_file_sync_metadata_mode(ext4_file *file, bool data_only)
{
	uint64_t target;
	int r = ext4_file_sync_metadata_prepare(file, data_only, &target);
#if CONFIG_JOURNALING_ENABLE
	if (r == EOK && target) {
		EXT4_MP_LOCK(file->mp);
		r = ext4_group_wait(file->mp, target, EXT4_JOURNAL_WAIT_DURABLE);
		EXT4_MP_UNLOCK(file->mp);
	}
#endif
	return r;
}

int ext4_file_sync_metadata(ext4_file *file)
{ return ext4_file_sync_metadata_mode(file, false); }

int ext4_filesystem_sync_metadata(const char *mount_point)
{
	struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
	if (!mp) return ENOENT;
	EXT4_MP_LOCK(mp);
	struct jbd_journal *journal = mp->fs.jbd_journal;
	int result = EOK;
	uint64_t target = 0;
	if (journal) {
		{
		EXT4_STATE_READ_SCOPE(state, &mp->fs);
		result = jbd_journal_error(journal);
		if (!result && journal->grouped && journal->accepted_sequence > journal->durable_sequence)
			target = journal->accepted_sequence;
		}
		if (!result && target) result = ext4_group_wait(mp, target, EXT4_JOURNAL_WAIT_DURABLE);
	} else result = ext4_block_cache_flush(mp->fs.bdev);
	EXT4_MP_UNLOCK(mp);
	return result;
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

static int ext4_mode_set_common(const char *path, uint32_t mode, bool initial)
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

	orig_mode = ext4_inode_get_mode(ext4_fs_super(&mp->fs), inode_ref.inode);
	if (initial && (orig_mode & 0170000U) == 0040000U)
		mode |= orig_mode & 02000U;
	orig_mode &= ~0xFFF;
	orig_mode |= mode & 0xFFF;
	ext4_inode_set_mode(ext4_fs_super(&mp->fs), inode_ref.inode, orig_mode);

	inode_ref.dirty = true;
	r = ext4_trans_put_inode_ref(mp, &inode_ref);

	Finish:
	EXT4_MP_UNLOCK(mp);

	return r;
}

int ext4_mode_set(const char *path, uint32_t mode)
{ return ext4_mode_set_common(path, mode, false); }
int ext4_mode_set_initial(const char *path, uint32_t mode)
{ return ext4_mode_set_common(path, mode, true); }

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

	*mode = ext4_inode_get_mode(ext4_fs_super(&mp->fs), inode_ref.inode);
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

/* The caller owns an empty inode and has checked the target length. */
static int ext4_symlink_inode_set(struct ext4_inode_ref *ref, const void *buf,
                                  uint32_t size, bool synchronous)
{
	uint32_t sblock;
	ext4_fsblk_t fblock;
	if (!size)
		return EOK;
	/*If the size of symlink is smaller than 60 bytes*/
	if (size < sizeof(ref->inode->blocks)) {
		memset(ref->inode->blocks, 0, sizeof(ref->inode->blocks));
		memcpy(ref->inode->blocks, buf, size);
		ext4_inode_clear_flag(ref->inode, EXT4_INODE_FLAG_EXTENTS);
	} else {
		ext4_fs_inode_blocks_init(ref->fs, ref);
		int r = ext4_fs_append_inode_dblk(ref, &fblock, &sblock);
		if (r != EOK)
			return r;

		if (synchronous) {
			struct ext4_blockdev *bdev = ref->fs->bdev;
			void *target = ext4_calloc(1, bdev->lg_bsize);
			if (!target)
				return ENOMEM;
			memcpy(target, buf, size);
			/* The unnamed inode owns the new block. Keep a target write
			 * error separate from an uncertain metadata writeback. */
			r = ext4_block_flush_lba(bdev, fblock);
			if (!r) {
				ext4_bcache_invalidate_lba(bdev->bc, fblock, 1);
				r = ext4_blocks_set_direct(bdev, target, fblock, 1);
			}
			ext4_free(target);
		} else {
			r = ext4_initialize_fblock(ref, fblock);
			if (r == EOK)
				r = ext4_trans_write_data(ref->fs->bdev, fblock, 0, buf, size, false, NULL);
		}
		if (r != EOK)
			return r;
	}

	ext4_inode_set_size(ref->inode, size);
	ref->dirty = true;
	return EOK;
}

static int ext4_fsymlink_set(ext4_file *f, const void *buf, uint32_t size)
{
    ext4_assert(f && f->mp);
    if (!size) return EOK;
    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(&f->mp->fs, f->inode, &ref);
    if (r != EOK) return r;
    if (size > ext4_sb_get_block_size(ext4_fs_super(&f->mp->fs))) r = EINVAL;
    else r = ext4_ftruncate_no_lock(f, 0, UINT32_MAX, false);
    if (!r) r = ext4_symlink_inode_set(&ref, buf, size, false);

    if (!r) {
        f->fsize = size;
        if (f->fpos > size) f->fpos = size;
    }

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

static void ext4_mknod_inode_set(struct ext4_inode_ref *ref, uint32_t dev)
{
    ext4_inode_set_dev(ref->inode, dev);
    ext4_inode_set_size(ref->inode, 0);
    ref->dirty = true;
}

static int ext4_mknod_set(ext4_file *f, uint32_t dev)
{
	struct ext4_inode_ref ref;
	int r;

	ext4_assert(f && f->mp);

	r = ext4_fs_get_inode_ref(&f->mp->fs, f->inode, &ref);
	if (r != EOK)
		return r;

    ext4_mknod_inode_set(&ref, dev);

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

static void ext4_copy_initial_inode(struct ext4_inode_ref *ref, struct ext4_inode *initial)
{
    size_t size = ext4_get16(ext4_fs_super(ref->fs), inode_size);
    if (size > sizeof(*initial)) size = sizeof(*initial);
    memcpy(initial, ref->inode, size);
    memset((uint8_t *)initial + size, 0, sizeof(*initial) - size);
}

static void ext4_latch_nojournal_error(struct ext4_fs *fs, int error)
{
    int expected = EOK;
    __atomic_compare_exchange_n(&fs->nojournal_error, &expected, error, false,
        __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

static int ext2_init_directory(struct ext4_inode_ref *child, struct ext4_inode_ref *parent)
{
    ext4_fsblk_t physical;
    ext4_lblk_t logical;
    int result = ext4_fs_append_inode_dblk(child, &physical, &logical);
    if (result) return result;
    struct ext4_block block;
    struct ext4_blockdev *device = child->fs->bdev;
    result = ext4_trans_block_get_noread(device, &block, physical);
    if (result) return result;
    uint32_t block_size = device->lg_bsize;
    struct ext4_sblock *sb = ext4_fs_super(child->fs);
    memset(block.data, 0, block_size);
    ext4_dir_write_entry(sb, (void *)block.data, 12, child, ".", 1);
    ext4_dir_write_entry(sb, (void *)(block.data + 12), block_size - 12, parent, "..", 2);
    result = ext4_trans_set_block_dirty(block.buf);
    return ext4_result(result, ext4_block_set(device, &block));
}

static int ext4_create_child_nojournal(struct ext4_mountpoint *mp, uint32_t parent,
    const char *name, uint32_t mode, int type, const char *target, ext4_file *file,
    struct ext4_inode *initial, uint32_t uid, uint32_t gid,
    const struct ext4_create_acl *acl)
{
    struct ext4_fs *fs = &mp->fs;
    struct ext4_sblock *sb = ext4_fs_super(fs);
    size_t length = target ? strlen(target) : 0, name_len = strlen(name);
    if (length > ext4_sb_get_block_size(sb)) return EINVAL;
    /* The writeback scope must contain this creation's changes only. */
    if (ext4_operation_depth(fs)) return EBUSY;
    int result = ext4_trans_start(mp);
    if (result) return result;

    struct ext4_inode_ref directory = {0}, child = {0};
    bool publishing = false;
    result = ext4_fs_get_inode_ref(fs, parent, &directory);
    if (result) goto Finish;
    if (!ext4_inode_is_type(sb, directory.inode, EXT4_INODE_MODE_DIRECTORY)) {
        result = ENOTDIR;
        goto Finish;
    }
    if (!ext4_inode_get_links_cnt(directory.inode)) {
        result = ENOENT;
        goto Finish;
    }
    if (type == EXT4_DE_DIR && !ext4_rename_can_add_directory(&directory)) {
        result = EMLINK;
        goto Finish;
    }
    struct ext4_dir_search_result found;
    int lookup = ext4_dir_find_entry(&found, &directory, name, name_len, false);
    result = ext4_dir_destroy_result(&directory, &found);
    if (lookup != ENOENT) result = ext4_result(lookup ? lookup : EEXIST, result);
    if (result) goto Finish;

    /* Reserve the live parent's metadata observer memory before publication. */
    result = ext4_operation_record_inode(fs, parent, true);
    if (result) goto Finish;
    result = ext4_fs_alloc_inode(fs, &child, type, parent, name, name_len);
    if (result) goto Finish;
    ext4_inode_set_mode_initial(&child, mode);
    ext4_inode_set_uid(child.inode, uid);
    ext4_inode_set_gid(child.inode, gid);
    if (ext4_inode_has_flag(directory.inode, EXT4_INODE_FLAG_NODUMP))
        ext4_inode_set_flag(child.inode, EXT4_INODE_FLAG_NODUMP);
    ext4_initialize_inode_times(mp,&child);
    if (type == EXT4_DE_SYMLINK)
        result = ext4_symlink_inode_set(&child, target, (uint32_t)length, true);
    else if (type == EXT4_DE_DIR)
        result = ext2_init_directory(&child, &directory);
    if (!result && acl && acl->access)
        result = ext4_xattr_set(&child, EXT4_XATTR_INDEX_POSIX_ACL_ACCESS,
            "", 0, acl->access, acl->access_size);
    if (!result && acl && acl->defaults)
        result = ext4_xattr_set(&child, EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT,
            "", 0, acl->defaults, acl->default_size);
    if (result) goto Reclaim;

    ext4_inode_set_links_cnt(child.inode, type == EXT4_DE_DIR ? 2 : 1);
    result = ext4_fs_stage_inode_ref(&child);
    if (result) goto Reclaim;
    /* Complete the target and all inode/allocator writes before a directory
     * can name the inode. This orders write submission, not device persistence. */
    result = ext4_bcache_scope_flush(&mp->bc, &ext4_current_operation(fs)->scope);
    if (result) goto Reclaim;

    uint64_t previous_size = ext4_inode_get_size(sb, directory.inode);
    result = ext4_dir_add_entry(&directory, name, name_len, &child);
    publishing = true;
    if (fs->ext2_format && (result == ENOSPC || result == ENOMEM) &&
        previous_size == ext4_inode_get_size(sb, directory.inode)) {
        publishing = false;
        goto Reclaim;
    }
    if (result) goto Finish;
    if (type == EXT4_DE_DIR) {
        ext4_fs_inode_links_count_inc(&directory);
        directory.dirty = true;
    }
    ext4_touch_inode(mp, &directory, EXT4_TIME_MTIME | EXT4_TIME_CTIME, NULL);
    if (initial) ext4_copy_initial_inode(&child, initial);
    *file = (ext4_file){.mp = mp, .inode = child.index, .flags = O_RDWR,
        .fsize = ext4_inode_get_size(sb, child.inode),
        .fmax = ext4_inode_max_size(fs, child.inode)};
    goto Finish;

Reclaim:
    if (!__atomic_load_n(&fs->nojournal_error, __ATOMIC_ACQUIRE)) {
        ext4_inode_set_links_cnt(child.inode, 0);
        int cleanup = ext4_free_unlinked_inode(&child);
        if (cleanup) ext4_latch_nojournal_error(fs, cleanup);
    }
Finish:
    if (child.block.data) result = ext4_result(result, ext4_fs_put_inode_ref(&child));
    if (directory.block.data) result = ext4_result(result, ext4_fs_put_inode_ref(&directory));
    /* Even a failed directory update can have written a name or new block. */
    if (publishing && result) ext4_latch_nojournal_error(fs, result);
    result = ext4_trans_finish(mp, result);
    if (publishing && result) ext4_latch_nojournal_error(fs, result);
    return result;
}

/* One already resolved parent and basename share the existing create undo
 * boundary. Initial attributes belong to caller scratch until acceptance. */
int ext4_create_child(const char *mount, uint32_t parent, const char *name,
    uint32_t mode, uint32_t device, const char *target, ext4_file *output,
    struct ext4_inode *initial,uint32_t uid,uint32_t gid)
{
    return ext4_create_child_acl(mount, parent, name, mode, device, target, output,
        initial, uid, gid, NULL);
}

int ext4_create_child_acl(const char *mount, uint32_t parent, const char *name,
    uint32_t mode, uint32_t device, const char *target, ext4_file *output,
    struct ext4_inode *initial, uint32_t uid, uint32_t gid,
    const struct ext4_create_acl *acl)
{
    struct ext4_mountpoint *mp = ext4_get_mount(mount);
    if (!mp) return ENOENT;
    if (!parent || !name || !ext4_rename_name_valid(name, strlen(name))) return EINVAL;
    unsigned kind = mode & 0170000U;
    int type = kind == 0100000U ? EXT4_DE_REG_FILE : kind == 0040000U ? EXT4_DE_DIR :
        kind == 0120000U ? EXT4_DE_SYMLINK : kind == 0010000U ? EXT4_DE_FIFO :
        kind == 0020000U ? EXT4_DE_CHRDEV : kind == 0060000U ? EXT4_DE_BLKDEV :
        kind == 0140000U ? EXT4_DE_SOCK : EXT4_DE_UNKNOWN;
    if (type == EXT4_DE_UNKNOWN || (type == EXT4_DE_SYMLINK && !target)) return EINVAL;
    EXT4_MP_LOCK(mp);
    ext4_file file = {0};
    struct ext4_inode_ref ref;
    int result;
    if ((type == EXT4_DE_SYMLINK && !ext4_sb_feature_com(&mp->fs.sb, EXT4_FCOM_HAS_JOURNAL)) ||
        (mp->fs.ext2_format && (type == EXT4_DE_REG_FILE || type == EXT4_DE_DIR)))
        result = ext4_create_child_nojournal(mp, parent, name, mode, type, target, &file,
            initial, uid, gid, acl);
    else if (mp->fs.ext2_format || acl) result = ENOTSUP;
    else if ((result = ext4_trans_start(mp)) == EOK) {
        result = ext4_open_relative(&file, mp, parent, name, O_CREAT | O_EXCL | O_RDWR,
            type, NULL, NULL, &ref, NULL);
        if (result == EOK) {
            ext4_inode_set_mode_initial(&ref, mode);
            ext4_inode_set_uid(ref.inode,uid);
            ext4_inode_set_gid(ref.inode,gid);
            if (type == EXT4_DE_SYMLINK) {
                size_t length = strlen(target);
                if (length > ext4_sb_get_block_size(ext4_fs_super(&mp->fs))) result = EINVAL;
                else result = ext4_symlink_inode_set(&ref, target, (uint32_t)length, false);
            } else if (type == EXT4_DE_CHRDEV || type == EXT4_DE_BLKDEV)
                ext4_mknod_inode_set(&ref, device);
            file.fsize = ext4_inode_get_size(ext4_fs_super(&mp->fs), ref.inode);
            if (result == EOK && initial) ext4_copy_initial_inode(&ref, initial);
            result = ext4_result(result, ext4_fs_put_inode_ref(&ref));
        }
        result = ext4_trans_finish(mp, result);
    }
    if (result == EOK && output) *output = file;
    else if (file.mp) result = ext4_result(result, ext4_fclose(&file));
    EXT4_MP_UNLOCK(mp);
    return result;
}

int ext4_create_regular_missing(const ext4_file *parent, const char *name,
    uint32_t mode, uint32_t uid, uint32_t gid, ext4_file *output,
    struct ext4_inode *initial, bool final_parent_update)
{
    struct ext4_mountpoint *mp = parent->mp;
    struct ext4_fs *fs = &mp->fs;
    struct ext4_inode_ref directory, child;
    size_t length = strlen(name);
    if (!ext4_rename_name_valid(name, length)) return EINVAL;
    uint16_t size = ext4_get16(&fs->sb, inode_size);
    bool read_parent = final_parent_update &&
        (size == EXT4_GOOD_OLD_INODE_SIZE || size == 256) &&
        fs->jbd_journal && fs->jbd_journal->grouped && fs->jbd_journal->running &&
        ext4_fs_serial_writer(fs) && ext4_current_trans(fs);
    int result = read_parent ? ext4_fs_get_inode_ref_read(fs, parent->inode, &directory) :
        ext4_fs_get_inode_ref(fs, parent->inode, &directory);
    if (result) return result;
    if (!ext4_inode_is_type(&fs->sb, directory.inode, EXT4_INODE_MODE_DIRECTORY))
        result = ENOTDIR;
    else if (!ext4_inode_get_links_cnt(directory.inode)) result = ENOENT;
    else result = ext4_alloc_linked_child(mp, &directory, name, length, EXT4_DE_REG_FILE, &child,
        read_parent ? &read_parent : NULL);
    int release = ext4_fs_put_inode_ref(&directory);
    if (result) return result;
    ext4_inode_set_mode_initial(&child, mode);
    ext4_inode_set_uid(child.inode, uid);
    ext4_inode_set_gid(child.inode, gid);
    if (initial) ext4_copy_initial_inode(&child, initial);
    ext4_file file = {.mp = mp, .inode = child.index, .flags = O_CREAT | O_EXCL | O_RDWR,
        .fmax = ext4_inode_max_size(fs, child.inode)};
    result = ext4_result(release, ext4_fs_put_inode_ref(&child));
    if (!result) *output = file;
    return result;
}

/* Allocate an anonymous regular inode directly: no transient directory name,
 * lookup, or unlink. Enrollment and allocation share one journal operation. */
int ext4_tmpfile_child(const char *mount, uint32_t parent, uint32_t mode,
    ext4_file *output, struct ext4_inode *initial, uint32_t uid, uint32_t gid)
{
    struct ext4_mountpoint *mp = ext4_get_mount(mount);
    if (!mp || !parent || !output || (mode & 0170000U) != 0100000U) return EINVAL;
    EXT4_MP_LOCK(mp);
    struct ext4_inode_ref directory = {0}, ref = {0};
    ext4_file file = {0};
    int result = !mp->fs.jbd_journal ? ENOTSUP : ext4_trans_start(mp);
    if (result) goto Unlock;
    result = ext4_fs_get_inode_ref_read(&mp->fs, parent, &directory);
    if (!result && !ext4_inode_is_type(ext4_fs_super(&mp->fs), directory.inode,
                                      EXT4_INODE_MODE_DIRECTORY)) result = ENOTDIR;
    if (!result && !ext4_inode_get_links_cnt(directory.inode)) result = ENOENT;
    bool nodump = !result && ext4_inode_has_flag(directory.inode, EXT4_INODE_FLAG_NODUMP);
    if (directory.block.data) result = ext4_result(result, ext4_fs_put_inode_ref(&directory));
    if (!result) result = ext4_fs_alloc_inode(&mp->fs, &ref, EXT4_DE_REG_FILE, parent, "", 0);
    if (!result) {
        ext4_fs_inode_blocks_init(&mp->fs, &ref);
        ext4_inode_set_mode_initial(&ref, mode);
        ext4_inode_set_uid(ref.inode, uid);
        ext4_inode_set_gid(ref.inode, gid);
        if (nodump) ext4_inode_set_flag(ref.inode, EXT4_INODE_FLAG_NODUMP);
        ext4_initialize_inode_times(mp,&ref);
        result = ext4_orphan_add(&ref);
        if (!result) {
            file.mp = mp; file.inode = ref.index; file.flags = O_RDWR;
            file.fmax = ext4_inode_max_size(&mp->fs, ref.inode);
            if (initial) {
                size_t bytes = ext4_get16(ext4_fs_super(&mp->fs), inode_size);
                if (bytes > sizeof(*initial)) bytes = sizeof(*initial);
                memcpy(initial, ref.inode, bytes);
                memset((uint8_t *)initial + bytes, 0, sizeof(*initial) - bytes);
            }
        }
        result = ext4_result(result, ext4_fs_put_inode_ref(&ref));
    }
    result = ext4_trans_finish(mp, result);
    if (!result) *output = file;
Unlock:
    EXT4_MP_UNLOCK(mp);
    return result;
}

struct ext4_attribute_change {
    ext4_file *file;const char *name;size_t length;const void *value;size_t size;unsigned flags;bool remove;
};
static int ext4_attribute_name(const char *name,size_t length,uint8_t *index,const char **suffix,size_t *count)
{
    bool found;
    *suffix=ext4_extract_xattr_name(name,length,index,count,&found);
    if (!found) return EINVAL;
    if (*index==EXT4_XATTR_INDEX_POSIX_ACL_ACCESS ||
        *index==EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT) {
        if (*count) return EINVAL;
        /* ACLs use an empty on-disk name, not the system. suffix. */
        *suffix="";
        return EOK;
    }
    return *count ? EOK : EINVAL;
}
static int ext4_apply_attribute(void *argument)
{
    struct ext4_attribute_change *change=argument;
    struct ext4_mountpoint *mp=change->file->mp;
    uint8_t index;const char *suffix;size_t count;
    int result=ext4_attribute_name(change->name,change->length,&index,&suffix,&count);
    if (result) return result;
    result=ext4_trans_start(mp);
    if (result) return result;
    struct ext4_inode_ref ref;
    result=ext4_fs_get_inode_ref(&mp->fs,change->file->inode,&ref);
    if (!result) {
        if (change->remove) result=ext4_xattr_remove(&ref,index,suffix,count);
        else {
            size_t old_size=0;
            if (change->flags) {
                result=ext4_xattr_get(&ref,index,suffix,count,NULL,0,&old_size);
                if (!result && (change->flags & 1U)) result=EEXIST;
                else if (result==ENODATA && !(change->flags & 2U)) result=EOK;
            }
            if (!result) result=ext4_xattr_set(&ref,index,suffix,count,
                change->value ? change->value : "",change->size);
        }
        if (!result) ext4_touch_inode(mp,&ref,EXT4_TIME_CTIME,change->file->time_owner);
        result=ext4_result(result,ext4_fs_put_inode_ref(&ref));
    }
    return ext4_trans_finish(mp,result);
}
static int ext4_change_attribute(struct ext4_attribute_change *change)
{
    if (!change->file || !change->file->mp) return EINVAL;
    struct ext4_mountpoint *mp=change->file->mp;
    EXT4_MP_LOCK(mp);
    int result=mp->fs.read_only ? EROFS : ext4_metadata_execute_locked(mp,ext4_apply_attribute,change);
    EXT4_MP_UNLOCK(mp);
    return result;
}
int ext4_file_setxattr(ext4_file *file,const char *name,size_t length,const void *value,size_t size,unsigned flags)
{
    struct ext4_attribute_change change={file,name,length,value,size,flags,false};
    return ext4_change_attribute(&change);
}
int ext4_file_removexattr(ext4_file *file,const char *name,size_t length)
{
    struct ext4_attribute_change change={file,name,length,NULL,0,0,true};
    return ext4_change_attribute(&change);
}
struct ext4_acl_change {
    ext4_file *file;
    const void *value;
    size_t size;
    uint32_t mode;
    uint8_t index;
    bool remove,update_mode;
};
static int ext4_apply_acl(void *argument)
{
    struct ext4_acl_change *change=argument;
    struct ext4_mountpoint *mp=change->file->mp;
    int result=ext4_trans_start(mp);
    if (result) return result;
    struct ext4_inode_ref ref;
    result=ext4_fs_get_inode_ref(&mp->fs,change->file->inode,&ref);
    if (!result) {
        if (change->remove) {
            result=ext4_xattr_remove(&ref,change->index,"",0);
            if (result==ENODATA) result=EOK;
        } else result=ext4_xattr_set(&ref,change->index,"",0,change->value,change->size);
        if (!result && change->update_mode) {
            uint32_t previous=ext4_inode_get_mode(ext4_fs_super(&mp->fs),ref.inode);
            ext4_inode_set_mode(ext4_fs_super(&mp->fs),ref.inode,
                                (previous & ~07777U) | (change->mode & 07777U));
            ref.dirty=true;
        }
        if (!result) ext4_touch_inode(mp,&ref,EXT4_TIME_CTIME,change->file->time_owner);
        result=ext4_result(result,ext4_fs_put_inode_ref(&ref));
    }
    return ext4_trans_finish(mp,result);
}
int ext4_file_set_acl(ext4_file *file,uint8_t index,const void *value,size_t size,
                      bool remove,bool update_mode,uint32_t mode)
{
    if (!file || !file->mp || (index!=EXT4_XATTR_INDEX_POSIX_ACL_ACCESS &&
        index!=EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT) || (!remove && !value)) return EINVAL;
    struct ext4_acl_change change={file,value,size,mode,index,remove,update_mode};
    EXT4_MP_LOCK(file->mp);
    int result=file->mp->fs.read_only ? EROFS :
        ext4_metadata_execute_locked(file->mp,ext4_apply_acl,&change);
    EXT4_MP_UNLOCK(file->mp);
    return result;
}
int ext4_file_getxattr(ext4_file *file,const char *name,size_t length,void *value,size_t capacity,size_t *size)
{
    if (!file || !file->mp || !size) return EINVAL;
    uint8_t index;const char *suffix;size_t count;
    int result=ext4_attribute_name(name,length,&index,&suffix,&count);
    if (result) return result;
    struct ext4_mountpoint *mp=file->mp;
    EXT4_MP_LOCK(mp);
    struct ext4_inode_ref ref;
    result=ext4_fs_get_inode_ref_read(&mp->fs,file->inode,&ref);
    if (!result) {
        result=ext4_xattr_get(&ref,index,suffix,count,value,capacity,size);
        result=ext4_result(result,ext4_fs_put_inode_ref(&ref));
    }
    EXT4_MP_UNLOCK(mp);
    return result;
}
int ext4_file_listxattr(ext4_file *file,char *output,size_t capacity,size_t *size,bool trusted)
{
    if (!file || !file->mp || !size) return EINVAL;
    struct ext4_mountpoint *mp=file->mp;
    struct ext4_xattr_list_entry *list=NULL;
    *size=0;
    EXT4_MP_LOCK(mp);
    struct ext4_inode_ref ref;
    int result=ext4_fs_get_inode_ref_read(&mp->fs,file->inode,&ref);
    if (!result) {
        size_t length=0;
        result=ext4_xattr_list(&ref,NULL,&length);
        if (!result && length) {
            list=ext4_malloc(length);
            if (!list) result=ENOMEM;
            else result=ext4_xattr_list(&ref,list,&length);
        }
        if (!result) for (struct ext4_xattr_list_entry *entry=list;entry;entry=entry->next) {
            if (entry->name_index!=1 && entry->name_index!=2 && entry->name_index!=3 &&
                entry->name_index!=6 && !(trusted && entry->name_index==4)) continue;
            size_t prefix_size;const char *prefix=ext4_get_xattr_name_prefix(entry->name_index,&prefix_size);
            size_t count=prefix_size+entry->name_len+1;
            if (capacity && capacity-*size<count) {result=ERANGE;break;}
            if (capacity) {
                memcpy(output+*size,prefix,prefix_size);
                memcpy(output+*size+prefix_size,entry->name,entry->name_len);
                output[*size+count-1]=0;
            }
            *size+=count;
        }
        result=ext4_result(result,ext4_fs_put_inode_ref(&ref));
    }
    EXT4_MP_UNLOCK(mp);
    ext4_free(list);
    return result;
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
				inode_type = ext4_inode_type(ext4_fs_super(&mp->fs),
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
	if (r != ENOENT)
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

static int fopen_inode(ext4_file *file, const char *mount_point, uint32_t inode_number,
    struct ext4_inode *initial, const uint32_t *generation)
{
    struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
    struct ext4_inode_ref ref;
    int result;

    if (!mp || !file || inode_number == 0)
        return EINVAL;
    EXT4_MP_LOCK(mp);
    struct ext4_sblock *sb = ext4_fs_super(&mp->fs);
    if (generation) {
        uint32_t first = ext4_get32(sb, rev_level) ? ext4_get32(sb, first_inode) : EXT4_GOOD_OLD_FIRST_INO;
        result = inode_number > ext4_get32(sb, inodes_count) ||
                 (inode_number != EXT4_ROOT_INO && inode_number < first) ?
            ENOENT : ext4_ialloc_inode_present(&mp->fs, inode_number);
        if (result) goto unlock;
    }
    result = ext4_fs_get_inode_ref_read(&mp->fs, inode_number, &ref);
    if (result == EOK) {
        if (generation && (ext4_inode_get_generation(ref.inode) != *generation ||
            !ext4_inode_get_links_cnt(ref.inode) || ext4_inode_has_flag(ref.inode, EXT4_INODE_FLAG_EA_INODE))) {
            ext4_fs_put_inode_ref(&ref);
            result = ENOENT;
            goto unlock;
        }
        file->mp = mp;
        file->inode = inode_number;
        file->flags = mp->fs.read_only ? O_RDONLY : O_RDWR;
        file->fpos = 0;
        file->time_owner = NULL;
        file->delalloc = NULL;
        file->fsize = ext4_inode_get_size(ext4_fs_super(&mp->fs), ref.inode);
        file->fmax = ext4_inode_max_size(&mp->fs, ref.inode);
        if (initial) copy_inode_snapshot(&ref,initial);
        result = ext4_fs_put_inode_ref(&ref);
    }
unlock:
    EXT4_MP_UNLOCK(mp);
    return result;
}

int ext4_fopen_inode(ext4_file *file, const char *mount, uint32_t inode,
    struct ext4_inode *initial)
{ return fopen_inode(file, mount, inode, initial, NULL); }

int ext4_fopen_handle(ext4_file *file, const char *mount, uint32_t inode,
    uint32_t generation, struct ext4_inode *initial)
{ return fopen_inode(file, mount, inode, initial, &generation); }

static int lookup_child(struct ext4_mountpoint *mp, uint32_t parent_inode,
                      const char *name, uint32_t name_length,
                      uint32_t *child_inode, uint32_t *child_mode,
                      struct ext4_inode *snapshot,bool *current,uint32_t *lookup_block)
{
    struct ext4_inode_ref parent, child;
    struct ext4_dir_search_result found;
    int result;

    if (!mp || !name || name_length == 0 || name_length > 255 ||
        !child_inode || (snapshot!=NULL)!=(current!=NULL))
        return EINVAL;
    EXT4_MP_LOCK(mp);
    bool verified = false;
    bool cached=current && *current;
    if (cached) parent=(struct ext4_inode_ref){.inode=snapshot,.fs=&mp->fs,.index=parent_inode};
    else {
        result=ext4_fs_get_inode_ref_read(&mp->fs,parent_inode,&parent);
        if (result!=EOK) goto Unlock;
    }
    if (ext4_inode_type(ext4_fs_super(&mp->fs), parent.inode) !=
        EXT4_INODE_MODE_DIRECTORY) {
        result = ENOTDIR;
        goto PutParent;
    }
    result = ext4_dir_find_entry_from(&found, &parent, name, name_length, false, lookup_block);
    verified = result == EOK || result == ENOENT;
    if (result == EOK) {
        uint32_t index = ext4_dir_en_get_inode(found.dentry);
        if (!child_mode) *child_inode=index;
        else {
            result=ext4_fs_get_inode_ref_read(&mp->fs,index,&child);
            if (result==EOK) {
                *child_inode=index;
                *child_mode=ext4_inode_get_mode(ext4_fs_super(&mp->fs),child.inode);
                result=ext4_fs_put_inode_ref(&child);
            }
        }
    }
    {
        int cleanup = ext4_dir_destroy_result(&parent, &found);
        if (cleanup) verified = false;
        if (result == EOK) result = cleanup;
    }
PutParent:
    if (!cached) {
        bool copy=snapshot && verified && (result==EOK || result==ENOENT);
        if (copy) copy_inode_snapshot(&parent,snapshot);
        int cleanup = ext4_fs_put_inode_ref(&parent);
        if (copy && cleanup==EOK) *current=true;
        if (result == EOK) result = cleanup;
    }
Unlock:
    EXT4_MP_UNLOCK(mp);
    return result;
}

int ext4_lookup_child(const char *mount_point, uint32_t parent_inode,
    const char *name,uint32_t name_length,uint32_t *child_inode,uint32_t *child_mode)
{
    return lookup_child(ext4_get_mount(mount_point),parent_inode,name,name_length,child_inode,child_mode,NULL,NULL,NULL);
}

int ext4_file_lookup_child(const ext4_file *parent,const char *name,uint32_t name_length,
    uint32_t *child_inode,uint32_t *child_mode,struct ext4_inode *snapshot,bool *current,
    uint32_t *lookup_block)
{
    if (!parent || !parent->mp || !parent->mp->mounted) return EINVAL;
    return lookup_child(parent->mp,parent->inode,name,name_length,child_inode,child_mode,
        snapshot,current,lookup_block);
}


int ext4_readlink_inode(const char *mount_point, uint32_t inode_number,
                        char *buffer, size_t capacity, size_t *bytes_read)
{
    struct ext4_mountpoint *mp = ext4_get_mount(mount_point);
    ext4_file file = {0};
    int result;

    if (!mp || !buffer || !bytes_read) return EINVAL;
    result = ext4_fopen_inode(&file, mount_point, inode_number, NULL);
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

static int dir_iterate(ext4_file *file, uint64_t *position,
    ext4_dir_emit_fn emit, void *context, bool *atime_current, bool scan)
{
    struct ext4_inode_ref dir_inode;
    struct ext4_dir_iter it = {0};
    uint64_t directory_size;
    int result = EOK, finish_result;

    if (!file || !file->mp || !position || !emit) return EINVAL;
    EXT4_MP_LOCK(file->mp);
    if (*position == UINT64_MAX) {
        EXT4_MP_UNLOCK(file->mp);
        return EOK;
    }
    result = ext4_fs_get_inode_ref_read(&file->mp->fs, file->inode, &dir_inode);
    if (result != EOK) {
        EXT4_MP_UNLOCK(file->mp);
        return result;
    }
    directory_size = ext4_inode_get_size(&dir_inode.fs->sb, dir_inode.inode);
    file->fsize = directory_size;
    if (!ext4_inode_is_type(&dir_inode.fs->sb, dir_inode.inode, EXT4_INODE_MODE_DIRECTORY)) {
        result = ENOTDIR;
        goto Finish;
    }
    if (!ext4_inode_get_links_cnt(dir_inode.inode)) {
        result = ENOENT;
        goto Finish;
    }
    if (atime_current) {
        struct ext4_timestamp now;
        *atime_current = ext4_inode_has_flag(dir_inode.inode, EXT4_INODE_FLAG_NOATIME) ||
            !ext4_file_read_clock(file, &now) || !ext4_relatime_expired(&dir_inode, now);
    }
    if (*position >= directory_size) {
        *position = UINT64_MAX;
        goto Finish;
    }
    result=scan && !ext4_current_operation(dir_inode.fs) ?
        ext4_dir_iterator_init_scan(&it,&dir_inode,*position) :
        ext4_dir_iterator_init(&it,&dir_inode,*position);
    while (result == EOK && it.curr) {
        uint64_t offset = it.curr_off;
        uint64_t next = it.next_off;
        uint32_t inode = ext4_dir_en_get_inode(it.curr);
        if (inode) {
            uint16_t name_length = ext4_dir_en_get_name_len(&dir_inode.fs->sb, it.curr);
            if (name_length > 255U) {
                result = EIO;
                break;
            }
            int accepted = emit(context, offset, next, inode,
                ext4_dir_en_get_inode_type(&dir_inode.fs->sb, it.curr),
                (const char *)it.curr->name, name_length);
            if (!accepted) break;
            *position = next;
            if (accepted == 2) break;
        } else {
            *position = next;
        }
        result = ext4_dir_iterator_next_raw(&it);
    }
    if (result == EOK && !it.curr) *position = UINT64_MAX;
Finish:
    if (it.inode_ref) {
        finish_result = ext4_dir_iterator_fini(&it);
        if (result == EOK) result = finish_result;
    }
    finish_result = ext4_fs_put_inode_ref(&dir_inode);
    if (result == EOK) result = finish_result;
    EXT4_MP_UNLOCK(file->mp);
    return result;
}

int ext4_dir_iterate(ext4_file *file,uint64_t *position,
    ext4_dir_emit_fn emit,void *context,bool *atime_current)
{ return dir_iterate(file,position,emit,context,atime_current,true); }

struct ext4_dir_one {
    ext4_direntry *entry;
    uint64_t *offset;
};
static int ext4_dir_emit_one(void *context, uint64_t offset, uint64_t next,
    uint32_t inode, uint8_t type, const char *name, size_t length)
{
    struct ext4_dir_one *one = context;
    one->entry->inode = inode;
    one->entry->entry_length = (uint16_t)(next - offset);
    one->entry->name_length = (uint8_t)length;
    one->entry->inode_type = type;
    memcpy(one->entry->name, name, length);
    *one->offset = offset;
    return 2;
}
int ext4_dir_entry_next_status(ext4_dir *dir, ext4_direntry *entry,
    uint64_t *entry_offset)
{
    if (!dir || !entry || !entry_offset) return EINVAL;
    memset(entry, 0, sizeof(*entry));
    *entry_offset = UINT64_MAX;
    struct ext4_dir_one one = {entry, entry_offset};
    return dir_iterate(&dir->f,&dir->next_off,ext4_dir_emit_one,&one,NULL,false);
}

void ext4_dir_entry_rewind(ext4_dir *dir)
{
	dir->next_off = 0;
}

/**
 * @}
 */
