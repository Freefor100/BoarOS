/* Journal storage is charged to a live budget owner, independent of journal
 * records. Cache readers and DMA can release their last payload after stop. */
#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <ext4_journal_memory.h>
#include <ext4_errno.h>
#include <kernel/raw_lock.h>
#include <string.h>
#include <stdlib.h>

/* Control allocator classes share the existing bounded pool. */
enum { JBD_ALLOCATION_CACHE_BUCKETS = 9 };

struct jbd_memory {
    /* Home pinning acquires cache metadata (rank 30) before this short lock. */
    struct kernel_raw_lock metadata;
    uint32_t references, block_size;
    size_t used, peak, limit, cached, reserved, borrowed;
    struct jbd_memory_account account;
    uint64_t budget_failures;
    struct jbd_image *image_cache;
    void *allocation_cache[JBD_ALLOCATION_CACHE_BUCKETS];
    bool grouped, cache_disabled, closing;
};
union jbd_allocation {
    struct { struct jbd_memory *memory; size_t bytes; } owner;
    long double alignment;
};
_Static_assert(sizeof(struct jbd_trans) + sizeof(union jbd_allocation) <=
    (16U << (JBD_ALLOCATION_CACHE_BUCKETS - 1)), "transaction control exceeds journal cache classes");
static inline size_t allocation_class(size_t bytes, int *index)
{
    if (bytes<sizeof(void *)) bytes=sizeof(void *);
    if (bytes>SIZE_MAX-sizeof(union jbd_allocation)) return 0;
    bytes+=sizeof(union jbd_allocation);
    size_t charge=16;unsigned bucket=0;
    while (charge<bytes) { if (charge>SIZE_MAX/2) return 0;charge*=2;bucket++; }
    if (index) *index=bucket<JBD_ALLOCATION_CACHE_BUCKETS?(int)bucket:-1;
    return charge;
}
size_t jbd_allocation_charge(size_t bytes) { return allocation_class(bytes,NULL); }
static int cache_index(size_t charge)
{
    unsigned index=0;
    while (charge>16) { charge/=2;index++; }
    return index<JBD_ALLOCATION_CACHE_BUCKETS?(int)index:-1;
}
static size_t cache_limit(const struct jbd_memory *memory)
{ size_t limit=memory->limit/4;return limit<256*1024?limit:256*1024; }
static void reference_locked(struct jbd_memory *memory)
{
    if (!memory->references || memory->references==UINT32_MAX) __builtin_trap();
    memory->references++;
}
static bool dereference_locked(struct jbd_memory *memory)
{
    if (!memory->references) __builtin_trap();
    if (--memory->references) return false;
    if (memory->used || memory->reserved || memory->cached || memory->borrowed || memory->image_cache) __builtin_trap();
    for (unsigned i=0;i<JBD_ALLOCATION_CACHE_BUCKETS;i++) if (memory->allocation_cache[i]) __builtin_trap();
    return true;
}
static void account_change(struct jbd_memory *memory,size_t bytes,bool acquiring)
{
    if (bytes && memory->account.context)
        memory->account.change(memory->account.context,bytes,acquiring);
}
static void destroy_memory(struct jbd_memory *memory)
{
    if (memory->account.context) memory->account.release(memory->account.context);
    ext4_free(memory);
}
void jbd_memory_set_account(struct jbd_memory *memory,const struct jbd_memory_account *account)
{
    if (!account || !account->context || !account->retain || !account->release || !account->change) __builtin_trap();
    account->retain(account->context);
    KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);
    if (memory->account.context || memory->closing || memory->used<memory->cached ||
        memory->used-memory->cached<memory->borrowed) __builtin_trap();
    memory->account=*account;
    account_change(memory,memory->used-memory->cached-memory->borrowed,true);
}
void jbd_memory_get(struct jbd_memory *memory)
{ KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);reference_locked(memory); }
void jbd_memory_put(struct jbd_memory *memory)
{
    bool destroy;
    { KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);destroy=dereference_locked(memory); }
    if (destroy) destroy_memory(memory);
}
int jbd_memory_start(struct jbd_journal *journal,uint32_t block_size)
{
    if (!journal || journal->memory || !block_size) __builtin_trap();
    struct jbd_memory *memory=ext4_calloc(1,sizeof(*memory));
    if (!memory) return ENOMEM;
    kernel_raw_lock_init(&memory->metadata,KERNEL_RAW_RANK_OBJECT+1);
    memory->references=1;memory->block_size=block_size;journal->memory=memory;
    return EOK;
}
void jbd_memory_snapshot(struct jbd_memory *memory,struct jbd_memory_stats *stats)
{
    KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);
    *stats=(struct jbd_memory_stats){.used=memory->used,.peak=memory->peak,.limit=memory->limit,
        .cached=memory->cached,.reserved=memory->reserved,.borrowed=memory->borrowed,.budget_failures=memory->budget_failures};
}
static struct jbd_memory_stats journal_stats(const struct jbd_journal *journal)
{
    struct jbd_memory_stats stats={0};
    if (journal && journal->memory) jbd_memory_snapshot(journal->memory,&stats);
    return stats;
}
size_t jbd_memory_used(const struct jbd_journal *journal) { return journal_stats(journal).used; }
size_t jbd_memory_peak(const struct jbd_journal *journal) { return journal_stats(journal).peak; }
size_t jbd_memory_limit(const struct jbd_journal *journal) { return journal_stats(journal).limit; }
size_t jbd_memory_cached(const struct jbd_journal *journal) { return journal_stats(journal).cached; }
uint64_t jbd_memory_budget_failures(const struct jbd_journal *journal) { return journal_stats(journal).budget_failures; }
void jbd_memory_set_limit(struct jbd_journal *journal,size_t limit)
{ KERNEL_TASK_LOCK_SCOPE(guard,&journal->memory->metadata);journal->memory->limit=limit; }
void jbd_memory_set_grouped(struct jbd_journal *journal,bool grouped)
{ KERNEL_TASK_LOCK_SCOPE(guard,&journal->memory->metadata);journal->memory->grouped=grouped; }
void jbd_memory_set_cache_disabled(struct jbd_journal *journal,bool disabled)
{ KERNEL_TASK_LOCK_SCOPE(guard,&journal->memory->metadata);journal->memory->cache_disabled=disabled; }
unsigned jbd_memory_cached_images(const struct jbd_journal *journal)
{
    unsigned count=0;struct jbd_memory *memory=journal->memory;
    KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);
    for (struct jbd_image *image=memory->image_cache;image;image=image->next) count++;
    return count;
}
static bool room_locked(struct jbd_memory *memory,size_t bytes)
{
    if (!bytes) return true;
    if (memory->used>SIZE_MAX-memory->reserved) __builtin_trap();
    size_t total=memory->used+memory->reserved;
    return bytes<=SIZE_MAX-total &&
        (!memory->limit || (bytes<=memory->limit && total<=memory->limit-bytes));
}
static void charge_locked(struct jbd_memory *memory,size_t bytes)
{
    if (!room_locked(memory,bytes)) __builtin_trap();
    memory->used+=bytes;if (memory->peak<memory->used) memory->peak=memory->used;
    account_change(memory,bytes,true);
}
static void reserve_locked(struct jbd_memory *memory,size_t bytes)
{
    if (!room_locked(memory,bytes)) __builtin_trap();
    memory->reserved+=bytes;
}
static void finish_reservation(struct jbd_memory *memory,size_t bytes,bool committed)
{
    KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);
    if (memory->reserved<bytes) __builtin_trap();
    memory->reserved-=bytes;
    if (committed) {
        if (memory->used>SIZE_MAX-bytes) __builtin_trap();
        memory->used+=bytes;
        account_change(memory,bytes,true);
        if (memory->peak<memory->used) memory->peak=memory->used;
    }
}
static void uncharge(struct jbd_memory *memory,size_t bytes,bool idle)
{
    KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);
    if (memory->used<bytes) __builtin_trap();
    memory->used-=bytes;
    if (idle) {if (memory->cached<bytes) __builtin_trap();memory->cached-=bytes;}
    else account_change(memory,bytes,false);
}
void jbd_memory_owner_uncharge(struct jbd_memory *memory,size_t bytes)
{ uncharge(memory,bytes,false); }
void jbd_memory_release_borrow(struct jbd_memory *memory,size_t bytes)
{
    KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);
    if (memory->used<bytes || memory->borrowed<bytes) __builtin_trap();
    memory->used-=bytes;memory->borrowed-=bytes;
}
static void budget_failure(struct jbd_journal *journal)
{
    struct jbd_memory *memory=journal->memory;
    { KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);memory->budget_failures++; }
    if (journal->jbd_fs) ext4_operation_memory_denied(journal->jbd_fs->inode_ref.fs);
}
static void free_allocation(union jbd_allocation *allocation,bool idle)
{
    struct jbd_memory *memory=allocation->owner.memory;size_t charge=allocation->owner.bytes;
    /* The allocation reference keeps the owner valid across the actual free.
     * Quota is reusable only after these bytes have reached the allocator. */
    ext4_free(allocation);
    bool destroy;
    {
        KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);
        if (memory->used<charge) __builtin_trap();
        memory->used-=charge;
        if (idle) {if (memory->cached<charge) __builtin_trap();memory->cached-=charge;}
        else account_change(memory,charge,false);
        destroy=dereference_locked(memory);
    }
    if (destroy) destroy_memory(memory);
}
static void trim_owner(struct jbd_memory *memory,size_t target)
{
    struct jbd_image *images;void *allocations[JBD_ALLOCATION_CACHE_BUCKETS];
    {
        KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);
        if (memory->cached<=target) return;
        images=memory->image_cache;memory->image_cache=NULL;
        memcpy(allocations,memory->allocation_cache,sizeof(allocations));
        memset(memory->allocation_cache,0,sizeof(memory->allocation_cache));
        /* Detached idle objects stay classified as idle until actual free. */
    }
    /* Detach one bounded cache snapshot. Concurrent returns can populate the
     * new cache without making this reclaim loop chase fresh producers. */
    while (images) {
        struct jbd_image *next=images->next;
        if (images->active) __builtin_trap();
        ext4_free(images->data);uncharge(memory,memory->block_size,true);
        free_allocation((union jbd_allocation *)images-1,true);images=next;
    }
    for (unsigned i=0;i<JBD_ALLOCATION_CACHE_BUCKETS;i++) while (allocations[i]) {
        void *pointer=allocations[i];allocations[i]=*(void **)pointer;
        free_allocation((union jbd_allocation *)pointer-1,true);
    }
}
void jbd_trim_cache(struct jbd_journal *journal,size_t target)
{ trim_owner(journal->memory,target); }
void jbd_memory_stop(struct jbd_journal *journal)
{
    struct jbd_memory *memory=journal->memory;
    if (!memory) return;
    { KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);memory->closing=true;memory->grouped=false; }
    trim_owner(memory,0);journal->memory=NULL;jbd_memory_put(memory);
}
bool jbd_budget_room(struct jbd_journal *journal,size_t bytes)
{
    struct jbd_memory *memory=journal->memory;
    for (unsigned attempt=0;attempt<2;attempt++) {
        { KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);if (!memory->closing && room_locked(memory,bytes)) return true; }
        if (!attempt) trim_owner(memory,0);
    }
    budget_failure(journal);return false;
}
bool jbd_memory_borrow(struct jbd_journal *journal,size_t bytes)
{
    struct jbd_memory *memory=journal->memory;
    for (unsigned attempt=0;attempt<2;attempt++) {
        {
            KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);
            if (!memory->closing && room_locked(memory,bytes)) {
                memory->used+=bytes;memory->borrowed+=bytes;
                if (memory->peak<memory->used) memory->peak=memory->used;
                return true;
            }
        }
        if (!attempt) trim_owner(memory,0);
    }
    budget_failure(journal);return false;
}
static void *allocate_extra(struct jbd_journal *journal,size_t bytes,size_t extra)
{
    /* Rounding already identifies the control pool's class. Keep both results
     * from that pass instead of counting the same powers of two again. */
    int index;
    size_t charge=allocation_class(bytes,&index);if (!charge || extra>SIZE_MAX-charge) return NULL;
    struct jbd_memory *memory=journal->memory;
    for (unsigned attempt=0;attempt<2;attempt++) {
        void *cached=NULL;bool fresh=false;
        {
            KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);
            if (!memory->closing) {
                if (index>=0 && memory->allocation_cache[index] && room_locked(memory,extra)) {
                    cached=memory->allocation_cache[index];memory->allocation_cache[index]=*(void **)cached;
                    if (memory->cached<charge) __builtin_trap();
                    memory->cached-=charge;account_change(memory,charge,true);
                    reserve_locked(memory,extra);
                } else if (room_locked(memory,charge+extra)) {
                    reserve_locked(memory,charge+extra);reference_locked(memory);fresh=true;
                }
            }
        }
        if (cached) { memset(cached,0,bytes);return cached; }
        if (fresh) {
            union jbd_allocation *allocation=ext4_calloc(1,charge);
            if (!allocation) {finish_reservation(memory,charge+extra,false);jbd_memory_put(memory);return NULL;}
            finish_reservation(memory,charge,true);
            allocation->owner.memory=memory;allocation->owner.bytes=charge;return allocation+1;
        }
        if (!attempt) trim_owner(memory,0);
    }
    budget_failure(journal);return NULL;
}
void *jbd_alloc(struct jbd_journal *journal,size_t bytes) { return allocate_extra(journal,bytes,0); }
void jbd_free(void *pointer)
{
    if (!pointer) return;
    union jbd_allocation *allocation=(union jbd_allocation *)pointer-1;
    struct jbd_memory *memory=allocation->owner.memory;size_t charge=allocation->owner.bytes;
    int index=cache_index(charge);
    {
        KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);size_t limit=cache_limit(memory);
        if (!memory->closing && memory->grouped && !memory->cache_disabled && index>=0 &&
            charge<=limit && memory->cached<=limit-charge) {
            *(void **)pointer=memory->allocation_cache[index];memory->allocation_cache[index]=pointer;
            memory->cached+=charge;account_change(memory,charge,false);return;
        }
    }
    free_allocation(allocation,false);
}
void *jbd_journal_alloc(struct jbd_journal *journal,size_t bytes) { return jbd_alloc(journal,bytes); }
void jbd_journal_free(void *pointer) { jbd_free(pointer); }
size_t jbd_image_charge(const struct jbd_journal *journal)
{ return journal->block_size+jbd_allocation_charge(sizeof(struct jbd_image)); }
static void payload_release(struct ext4_block_payload *payload)
{
    struct jbd_image *image=(void *)((uint8_t *)payload-offsetof(struct jbd_image,payload));
    struct jbd_memory *memory=image->memory;
    {
        KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);size_t limit=cache_limit(memory);
        if (!image->active) __builtin_trap();
        image->active=false;
        size_t charge=memory->block_size+((union jbd_allocation *)image-1)->owner.bytes;
        if (!memory->closing && memory->grouped && !memory->cache_disabled &&
            charge<=limit && memory->cached<=limit-charge) {
            image->next=memory->image_cache;memory->image_cache=image;memory->cached+=charge;
            account_change(memory,charge,false);return;
        }
    }
    ext4_free(image->data);jbd_memory_owner_uncharge(memory,memory->block_size);jbd_free(image);
}
struct jbd_image *jbd_image_alloc(struct jbd_journal *journal)
{
    struct jbd_memory *memory=journal->memory;struct jbd_image *image=NULL;
    {
        KERNEL_TASK_LOCK_SCOPE(guard,&memory->metadata);
        image=memory->image_cache;
        if (image) {
            memory->image_cache=image->next;
            size_t charge=memory->block_size+((union jbd_allocation *)image-1)->owner.bytes;
            if (image->active || memory->cached<charge) __builtin_trap();
            memory->cached-=charge;account_change(memory,charge,true);
        }
    }
    if (!image) {
        image=allocate_extra(journal,sizeof(*image),memory->block_size);if (!image) return NULL;
        image->data=ext4_malloc(memory->block_size);
        if (!image->data) {finish_reservation(memory,memory->block_size,false);jbd_free(image);return NULL;}
        finish_reservation(memory,memory->block_size,true);
        image->memory=memory;
    }
    ext4_block_payload_init(&image->payload,image->data,payload_release);
    image->active=true;image->next=NULL;return image;
}
void jbd_image_free(struct jbd_image *image)
{ if (image) {if (!image->active) __builtin_trap();ext4_block_payload_put(&image->payload);} }
static size_t home_charge(struct jbd_memory *memory)
{
    size_t header=16;while (header<sizeof(struct ext4_buf)) header*=2;
    return memory->block_size+header;
}
bool jbd_memory_pin_home(struct jbd_journal *journal,struct ext4_buf *buf)
{
    struct jbd_memory *memory=journal->memory;
    for (unsigned attempt=0;attempt<2;attempt++) {
        {
            KERNEL_TASK_LOCK_SCOPE(cache_guard,&buf->bc->metadata);
            if (buf->journal_pending==UINT32_MAX) __builtin_trap();
            if (buf->journal_pending) {buf->journal_pending++;return true;}
            KERNEL_TASK_LOCK_SCOPE(memory_guard,&memory->metadata);
            if (!memory->closing && room_locked(memory,home_charge(memory))) {
                charge_locked(memory,home_charge(memory));buf->journal_pending++;return true;
            }
        }
        if (!attempt) trim_owner(memory,0);
    }
    budget_failure(journal);return false;
}
void jbd_memory_unpin_home(struct jbd_journal *journal,struct ext4_buf *buf)
{
    KERNEL_TASK_LOCK_SCOPE(cache_guard,&buf->bc->metadata);
    if (!buf->journal_pending) __builtin_trap();
    if (!--buf->journal_pending) jbd_memory_owner_uncharge(journal->memory,home_charge(journal->memory));
}
