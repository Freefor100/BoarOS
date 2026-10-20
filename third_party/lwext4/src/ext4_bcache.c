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
 * @file  ext4_bcache.c
 * @brief Block cache allocator.
 */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_bcache.h>
#include <ext4_blockdev.h>
#include <ext4_debug.h>
#include <ext4_errno.h>
#include <kernel/cost.h>

#include <string.h>
#include <stdlib.h>

static int ext4_bcache_lba_compare(struct ext4_buf *a, struct ext4_buf *b)
{
	 if (a->lba > b->lba)
		 return 1;
	 else if (a->lba < b->lba)
		 return -1;
	 return 0;
}

RB_GENERATE_INTERNAL(ext4_buf_lba, ext4_buf, lba_node,
		     ext4_bcache_lba_compare, static inline)

static inline __attribute__((always_inline)) void idle_remove_locked(struct ext4_bcache *bc, struct ext4_buf *buf)
{
    if (!bc->idle_blocks) __builtin_trap();
    bc->idle_blocks--;
    if (buf->lru_previous) buf->lru_previous->lru_next=buf->lru_next;
    else {
        if (bc->lru_head!=buf) __builtin_trap();
        bc->lru_head=buf->lru_next;
    }
    if (buf->lru_next) buf->lru_next->lru_previous=buf->lru_previous;
    else {
        if (bc->lru_tail!=buf) __builtin_trap();
        bc->lru_tail=buf->lru_previous;
    }
    buf->lru_previous=buf->lru_next=NULL;
}
static inline __attribute__((always_inline)) void idle_append_locked(struct ext4_bcache *bc, struct ext4_buf *buf)
{
    if (buf->lru_previous || buf->lru_next || bc->lru_head==buf) __builtin_trap();
    if (bc->idle_blocks==UINT32_MAX) __builtin_trap();
    bc->idle_blocks++;
    buf->lru_previous=bc->lru_tail;
    if (bc->lru_tail) bc->lru_tail->lru_next=buf;
    else bc->lru_head=buf;
    bc->lru_tail=buf;
}

static struct ext4_buf *lookup_locked(struct ext4_bcache *bc, uint64_t lba)
{
    struct ext4_buf key = {.lba=lba};
    return RB_FIND(ext4_buf_lba,&bc->lba_root,&key);
}
static void dirty_remove_locked(struct ext4_bcache *bc, struct ext4_buf *buf)
{
    if (!buf->on_dirty_list) return;
    SLIST_REMOVE(&bc->dirty_list,buf,ext4_buf,dirty_node);
    buf->on_dirty_list=false;
}
static void dirty_insert_locked(struct ext4_bcache *bc, struct ext4_buf *buf)
{
    if (buf->on_dirty_list) return;
    SLIST_INSERT_HEAD(&bc->dirty_list,buf,dirty_node);
    buf->on_dirty_list=true;
}
void ext4_bcache_remove_dirty_node(struct ext4_bcache *bc, struct ext4_buf *buf)
{ KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata); dirty_remove_locked(bc,buf); }
void ext4_bcache_insert_dirty_node(struct ext4_bcache *bc, struct ext4_buf *buf)
{ KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata); dirty_insert_locked(bc,buf); }
static void get_locked(struct ext4_bcache *bc, struct ext4_block *block, struct ext4_buf *buf)
{
    if (!buf->refctr) {
        idle_remove_locked(bc,buf);
        dirty_remove_locked(bc,buf);
    }
    if (buf->refctr==UINT32_MAX) __builtin_trap();
    buf->refctr++;
    *block=(struct ext4_block){buf->lba,buf,buf->data};
}
static void drop_locked(struct ext4_bcache *bc, struct ext4_buf *buf)
{
    if (buf->refctr || buf->loading) __builtin_trap();
    idle_remove_locked(bc,buf);
    RB_REMOVE(ext4_buf_lba,&bc->lba_root,buf);
    dirty_remove_locked(bc,buf);
    if (!bc->ref_blocks) __builtin_trap();
    bc->ref_blocks--;
}
static void free_buffer(struct ext4_buf *buf)
{ ext4_free(buf->data); ext4_free(buf); }

int ext4_bcache_init_dynamic(struct ext4_bcache *bc, uint32_t cnt, uint32_t itemsize)
{
    ext4_assert(bc && cnt && itemsize);
    memset(bc,0,sizeof(*bc));
    kernel_raw_lock_init(&bc->metadata,KERNEL_RAW_RANK_OBJECT);
    bc->cnt=cnt;bc->itemsize=itemsize;
    return EOK;
}
int ext4_bcache_fini_dynamic(struct ext4_bcache *bc)
{
    if (bc->ref_blocks) __builtin_trap();
    memset(bc,0,sizeof(*bc));
    return EOK;
}
void ext4_bcache_cleanup(struct ext4_bcache *bc)
{
    /* Unmount owns the mount writer and has drained inode/journal borrowers. */
    for (;;) {
        struct ext4_buf *buf;
        {
            KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);
            buf=RB_MIN(ext4_buf_lba,&bc->lba_root);
            if (!buf) return;
            if (buf->refctr || buf->loading) __builtin_trap();
            struct ext4_block borrowed;
            get_locked(bc,&borrowed,buf);
        }
        ext4_block_flush_buf(bc->bdev,buf);
        {
            KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);
            if (buf->refctr!=1) __builtin_trap();
            buf->refctr=0;
            idle_append_locked(bc,buf);
            drop_locked(bc,buf);
        }
        free_buffer(buf);
    }
}

static void read_state_locked(struct ext4_buf *buf, enum ext4_bcache_read_state *read)
{
    if (!read) return;
    if (buf->loading || buf->load_error) *read=EXT4_BCACHE_READ_WAIT;
    else if (ext4_bcache_test_flag(buf,BC_UPTODATE)) *read=EXT4_BCACHE_READ_READY;
    else { buf->loading=true;*read=EXT4_BCACHE_READ_LOAD; }
}
struct ext4_buf *ext4_bcache_find_get(struct ext4_bcache *bc, struct ext4_block *b,
    uint64_t lba, enum ext4_bcache_read_state *read)
{
    KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);
    struct ext4_buf *buf=lookup_locked(bc,lba);
    if (buf) { get_locked(bc,b,buf);read_state_locked(buf,read); }
    return buf;
}
void ext4_bcache_inc_ref(struct ext4_buf *buf)
{
    KERNEL_TASK_LOCK_SCOPE(guard,&buf->bc->metadata);
    if (!buf->refctr || buf->refctr==UINT32_MAX) __builtin_trap();
    buf->refctr++;
}
int ext4_bcache_alloc(struct ext4_bcache *bc, struct ext4_block *b,
    enum ext4_bcache_read_state *read)
{
    if (ext4_bcache_find_get(bc,b,b->lb_id,read)) return EOK;
    /* Candidates are private until the second lookup. Allocation may reclaim
     * or sleep while another CPU publishes the same block. */
    struct ext4_buf *candidate=ext4_calloc(1,sizeof(*candidate));
    if (candidate) {
        candidate->data=ext4_malloc(bc->itemsize);
        if (!candidate->data) { ext4_free(candidate);candidate=NULL; }
    }
    struct ext4_buf *buf;
    {
        KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);
        buf=lookup_locked(bc,b->lb_id);
        if (buf) {
            get_locked(bc,b,buf);
        } else if (candidate) {
            buf=candidate;candidate=NULL;
            buf->bc=bc;buf->lba=b->lb_id;buf->refctr=1;
            if (RB_INSERT(ext4_buf_lba,&bc->lba_root,buf)) __builtin_trap();
            bc->ref_blocks++;
            if (bc->max_ref_blocks<bc->ref_blocks) bc->max_ref_blocks=bc->ref_blocks;
            *b=(struct ext4_block){buf->lba,buf,buf->data};
        }
        if (buf) read_state_locked(buf,read);
    }
    if (candidate) free_buffer(candidate);
    return buf ? EOK : ENOMEM;
}

static void invalidate_locked(struct ext4_bcache *bc, struct ext4_buf *buf)
{
    buf->end_write=NULL;buf->end_write_arg=NULL;
    dirty_remove_locked(bc,buf);
    ext4_bcache_clear_dirty(buf);
}
void ext4_bcache_invalidate_buf(struct ext4_bcache *bc, struct ext4_buf *buf)
{ KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);invalidate_locked(bc,buf); }
void ext4_bcache_invalidate_lba(struct ext4_bcache *bc, uint64_t from, uint32_t cnt)
{
    if (!cnt) return;
    KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);
    struct ext4_buf key={.lba=from};
    struct ext4_buf *buf=RB_NFIND(ext4_buf_lba,&bc->lba_root,&key);
    while (buf && buf->lba-from<cnt) {
        invalidate_locked(bc,buf);
        buf=RB_NEXT(ext4_buf_lba,&bc->lba_root,buf);
    }
}

/* Every metadata read returns here. Keep the RV translated hot path in one
 * code page; the existing layout check also bounds this function's size. */
static int __attribute__((aligned(1024))) put_buffer(struct ext4_bcache *bc, struct ext4_block *b, bool read_only)
{
    struct ext4_buf *buf=b->buf;
    bool flush=false,drop=false;
    int result=EOK;
    KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);
    if (!buf || !buf->refctr || buf->bc!=bc) __builtin_trap();
    flush=buf->refctr==1 && !read_only && ext4_bcache_test_flag(buf,BC_DIRTY) &&
        ext4_bcache_test_flag(buf,BC_UPTODATE) && (!bc->bdev->cache_write_back ||
        ext4_bcache_test_flag(buf,BC_FLUSH) || ext4_bcache_test_flag(buf,BC_TMP));
    /* The context callback is a nonblocking query. Clean/shared references and
     * deferred dirty buffers need no flush decision from the calling task. */
    if (flush && bc->bdev->bdif->read_context && bc->bdev->bdif->read_context()) flush=false;
    /* The final writer keeps its reference across flush. A concurrent lookup
     * cannot detach this buffer while the device borrows its payload. */
    if (flush) {
        kernel_task_lock_release(&guard);
        result=ext4_block_flush_buf(bc->bdev,buf);
        kernel_task_lock_acquire(&bc->metadata,&guard);
    }
    {
        if (flush && !result) ext4_bcache_clear_flag(buf,BC_FLUSH);
        if (!--buf->refctr) {
            /* A zero-reference return is newest; no search or age counter. */
            idle_append_locked(bc,buf);
            if (ext4_bcache_test_flag(buf,BC_DIRTY) && ext4_bcache_test_flag(buf,BC_UPTODATE))
                dirty_insert_locked(bc,buf);
            drop=!result && (!ext4_bcache_test_flag(buf,BC_UPTODATE) || ext4_bcache_test_flag(buf,BC_TMP));
            if (drop) drop_locked(bc,buf);
        }
        b->lb_id=0;b->data=NULL;
    }
    kernel_task_lock_release(&guard);
    if (drop) free_buffer(buf);
    return result;
}
int ext4_bcache_free_read(struct ext4_bcache *bc, struct ext4_block *b)
{ return put_buffer(bc,b,true); }
int ext4_bcache_free(struct ext4_bcache *bc, struct ext4_block *b)
{
    return put_buffer(bc,b,false);
}
struct ext4_buf *ext4_bcache_dirty_get(struct ext4_bcache *bc, struct ext4_block *b)
{
    KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);
    struct ext4_buf *buf=SLIST_FIRST(&bc->dirty_list);
    if (buf) get_locked(bc,b,buf);
    return buf;
}
int ext4_bcache_shake(struct ext4_bcache *bc)
{
    bool read_only=bc->bdev->bdif->read_context && bc->bdev->bdif->read_context();
    int result=EOK;
    {
        KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);
        if (bc->dont_shake) return EOK;
        bc->dont_shake=true;
    }
    for (;;) {
        struct ext4_buf *buf;
        struct ext4_block owned;
        bool dirty;
        {
            KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);
            buf=bc->lru_head;
            /* Borrowed journal/in-flight buffers cannot be reclaimed here.
             * Charging them against idle retention evicts every clean path
             * block while a transaction owns more than cnt buffers. */
            if (!buf || bc->idle_blocks<bc->cnt) break;
            dirty=ext4_bcache_test_flag(buf,BC_DIRTY);
            if (dirty && read_only) break;
            if (dirty) get_locked(bc,&owned,buf);
            else drop_locked(bc,buf);
        }
        if (dirty) {
            result=ext4_block_flush_buf(bc->bdev,buf);
            /* Do not retry a failed flush through put_buffer. */
            ext4_bcache_free_read(bc,&owned);
            if (result) break;
            continue;
        }
        COST_ADD(BLOCK_CACHE_EVICTIONS,1);
        free_buffer(buf);
    }
    { KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);bc->dont_shake=false; }
    return result;
}

void ext4_bcache_scope_begin(struct ext4_bcache *bc, struct ext4_writeback_scope *scope)
{
    KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);
    scope->head=NULL;scope->previous=bc->writeback_scope;
    if (!bc->writeback_scope) bc->writeback_scope=scope;
    bc->bdev->cache_write_back++;
}
void ext4_bcache_scope_track(struct ext4_buf *buf)
{
    KERNEL_TASK_LOCK_SCOPE(guard,&buf->bc->metadata);
    struct ext4_writeback_scope *scope=buf->bc->writeback_scope;
    if (!scope || buf->writeback_scope) return;
    if (!buf->refctr || buf->refctr==UINT32_MAX) __builtin_trap();
    buf->writeback_scope=scope;buf->writeback_next=scope->head;scope->head=buf;
    buf->refctr++;
}
int ext4_bcache_scope_end(struct ext4_bcache *bc, struct ext4_writeback_scope *scope)
{
    int result=EOK;
    struct ext4_buf *buf;
    { KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);buf=scope->head;if (!scope->previous) bc->writeback_scope=NULL; }
    while (buf) {
        struct ext4_buf *next;
        struct ext4_block b={buf->lba,buf,buf->data};
        if (bc->bdev->cache_write_back==1) {
            int r=ext4_block_flush_buf(bc->bdev,buf);if (!result) result=r;
        }
        { KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);next=buf->writeback_next;buf->writeback_scope=NULL;buf->writeback_next=NULL; }
        int r=ext4_bcache_free(bc,&b);if (!result) result=r;
        buf=next;
    }
    { KERNEL_TASK_LOCK_SCOPE(guard,&bc->metadata);bc->bdev->cache_write_back--;scope->head=NULL; }
    return result;
}
