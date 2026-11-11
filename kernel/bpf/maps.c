#include <kernel/bpf_map.h>
#include <kernel/heap.h>
#include <kernel/errno.h>
#include <kernel/cpu.h>
#include <string.h>

#define F_RDONLY (1U << 3)
#define F_WRONLY (1U << 4)
#define F_RDONLY_PROG (1U << 7)
#define F_WRONLY_PROG (1U << 8)
#define F_MMAPABLE (1U << 10)
#define ACCESS_FLAGS (F_RDONLY | F_WRONLY | F_RDONLY_PROG | F_WRONLY_PROG)
struct bpf_entry { struct bpf_entry *next; unsigned char bytes[]; };
struct bpf_map {
    struct bpf_map_spec spec;
    struct kernel_heap *heap;
    struct physical_page_allocator *pages;
    struct kernel_raw_lock lock;
    uint32_t references, frozen, borrowers, count, bucket_count, cpus;
    size_t stride, value_bytes, entry_size, key_bytes;
    struct bpf_entry **buckets, *free, *retired;
    void *entries, *array;
    uint64_t physical, page_count;
    struct bpf_ring *ring;
};
static size_t align8(size_t n) { return (n + 7U) & ~(size_t)7U; }
static int is_array(const struct bpf_map *m)
{ return m->spec.type==BPF_STORAGE_ARRAY || m->spec.type==BPF_STORAGE_PERCPU_ARRAY; }
static int is_percpu(const struct bpf_map *m)
{ return m->spec.type==BPF_STORAGE_PERCPU_ARRAY || m->spec.type==BPF_STORAGE_PERCPU_HASH; }
static int access_ok(const struct bpf_map *m, int user, int write)
{
    if (write && user && m->frozen) return -KERNEL_EPERM;
    unsigned denied=write ? (user ? F_RDONLY : F_RDONLY_PROG) : (user ? F_WRONLY : F_WRONLY_PROG);
    return m->spec.map_flags & denied ? -KERNEL_EPERM : 0;
}
static uint32_t bucket(const struct bpf_map *m, const void *key)
{
    uint32_t h=2166136261U;
    for (uint32_t i=0;i<m->spec.key_size;i++) h=(h^((const unsigned char *)key)[i])*16777619U;
    return h & (m->bucket_count-1U);
}
static struct bpf_entry **find_entry(struct bpf_map *m, const void *key)
{
    struct bpf_entry **p=&m->buckets[bucket(m,key)];
    while (*p && memcmp((*p)->bytes,key,m->spec.key_size)) p=&(*p)->next;
    return p;
}
static void *entry_value(struct bpf_map *m, struct bpf_entry *e)
{ return e->bytes+m->key_bytes; }
static void *lookup_locked(struct bpf_map *m, const void *key)
{
    if (is_array(m)) {
        uint32_t i; memcpy(&i,key,sizeof i);
        return i<m->spec.max_entries ? (unsigned char *)m->array+i*m->value_bytes : 0;
    }
    struct bpf_entry *e=*find_entry(m,key);
    return e ? entry_value(m,e) : 0;
}
static void copy_value(struct bpf_map *m, void *dst, const void *src, int user)
{
    if (!is_percpu(m) || !user) { memcpy(dst,src,m->spec.value_size); return; }
    for (unsigned i=0;i<m->cpus;i++)
        memcpy((unsigned char *)dst+i*m->stride,(const unsigned char *)src+i*m->stride,m->spec.value_size);
}
static void retire(struct bpf_map *m, struct bpf_entry *e)
{
    struct bpf_entry **list=m->borrowers ? &m->retired : &m->free;
    e->next=*list; *list=e;
}
const struct bpf_map_spec *bpf_map_spec(const struct bpf_map *m) { return &m->spec; }
size_t bpf_map_user_value_size(const struct bpf_map *m)
{ return is_percpu(m) ? m->value_bytes : m->spec.value_size; }
unsigned bpf_map_cpu_count(const struct bpf_map *m) { return m->cpus; }
int bpf_map_is_frozen(const struct bpf_map *m) { return __atomic_load_n(&m->frozen,__ATOMIC_ACQUIRE); }
void bpf_map_get(struct bpf_map *m)
{ if (!__atomic_fetch_add(&m->references,1,__ATOMIC_RELAXED)) __builtin_trap(); }
void bpf_map_put(struct bpf_map *m)
{
    if (!m || __atomic_sub_fetch(&m->references,1,__ATOMIC_ACQ_REL)) return;
    if (m->borrowers) __builtin_trap();
    if (m->ring) bpf_ring_destroy(m->ring);
    if (m->physical) {
        for (uint64_t i=0;i<m->page_count;i++)
            physical_page_release(m->pages,m->physical+i*BOAROS_PAGE_SIZE);
    } else if (m->array) kernel_heap_release(m->heap,m->array);
    if (m->entries) kernel_heap_release(m->heap,m->entries);
    if (m->buckets) kernel_heap_release(m->heap,m->buckets);
    kernel_heap_release(m->heap,m);
}
int bpf_map_create(struct kernel_heap *heap, struct physical_page_allocator *pages,
    const struct bpf_map_spec *s, struct bpf_map **out)
{
    if (!heap || !pages || !s || !out || !s->max_entries) return -KERNEL_EINVAL;
    int array=s->type==BPF_STORAGE_ARRAY || s->type==BPF_STORAGE_PERCPU_ARRAY;
    int hash=s->type==BPF_STORAGE_HASH || s->type==BPF_STORAGE_PERCPU_HASH;
    int ring=s->type==BPF_STORAGE_RINGBUF;
    int pc=s->type==BPF_STORAGE_PERCPU_ARRAY || s->type==BPF_STORAGE_PERCPU_HASH;
    unsigned allowed=ring ? 0 : ACCESS_FLAGS | (s->type==BPF_STORAGE_ARRAY ? F_MMAPABLE : 0);
    if ((!array && !hash && !ring) || (s->map_flags & ~allowed) ||
        ((s->map_flags & (F_RDONLY|F_WRONLY))==(F_RDONLY|F_WRONLY)) ||
        ((s->map_flags & (F_RDONLY_PROG|F_WRONLY_PROG))==(F_RDONLY_PROG|F_WRONLY_PROG))) return -KERNEL_EINVAL;
    if (ring ? (s->key_size || s->value_size || (s->max_entries & (s->max_entries-1U)) ||
               (s->max_entries & (BOAROS_PAGE_SIZE-1U))) :
        (!s->key_size || !s->value_size || (array && s->key_size!=4))) return -KERNEL_EINVAL;
    if (!ring && (s->key_size>BOAROS_PAGE_SIZE || s->value_size>UINT32_MAX-7U)) return -KERNEL_E2BIG;
    struct bpf_map *m=0;
    if (kernel_heap_allocate_zeroed(heap,1,sizeof *m,(void **)&m)) return -KERNEL_ENOMEM;
    m->heap=heap; m->pages=pages; m->spec=*s; m->references=1;
    m->cpus=kernel_cpu_get_boot_count(); m->stride=align8(s->value_size);
    m->key_bytes=align8(s->key_size);
    m->value_bytes=m->stride*(pc ? m->cpus : 1U);
    kernel_raw_lock_init(&m->lock,KERNEL_RAW_RANK_OBJECT);
    int err=-KERNEL_ENOMEM;
    if (ring) { err=bpf_ring_create(heap,pages,s->max_entries,&m->ring); if (err) goto fail; }
    else if (array) {
        if (m->value_bytes && s->max_entries>SIZE_MAX/m->value_bytes) goto fail;
        size_t bytes=m->value_bytes*s->max_entries;
        if (s->map_flags & F_MMAPABLE) {
            uint64_t n=(bytes+BOAROS_PAGE_SIZE-1U)/BOAROS_PAGE_SIZE;
            unsigned order=0; while ((UINT64_C(1)<<order)<n) order++;
            if (physical_page_allocate_contiguous(pages,order,&m->physical)) goto fail;
            m->page_count=n;
            for (uint64_t i=n;i<(UINT64_C(1)<<order);i++)
                physical_page_release(pages,m->physical+i*BOAROS_PAGE_SIZE);
            if (physical_page_map_owned(pages,m->physical,&m->array)) __builtin_trap();
            memset(m->array,0,m->page_count*BOAROS_PAGE_SIZE);
        } else if (kernel_heap_allocate_zeroed(heap,1,bytes,&m->array)) goto fail;
    } else {
        uint64_t count=(uint64_t)s->max_entries+m->cpus;
        m->entry_size=align8(sizeof(struct bpf_entry)+m->key_bytes+m->value_bytes);
        if (count>SIZE_MAX/m->entry_size) goto fail;
        m->bucket_count=1;
        while (m->bucket_count<s->max_entries && m->bucket_count<(1U<<30)) m->bucket_count<<=1;
        if (kernel_heap_allocate_zeroed(heap,m->bucket_count,sizeof *m->buckets,(void **)&m->buckets) ||
            kernel_heap_allocate_zeroed(heap,count,m->entry_size,&m->entries)) goto fail;
        for (uint64_t i=0;i<count;i++) {
            struct bpf_entry *e=(struct bpf_entry *)((unsigned char *)m->entries+i*m->entry_size);
            e->next=m->free; m->free=e;
        }
    }
    *out=m; return 0;
fail: bpf_map_put(m); return err;
}
int bpf_map_freeze(struct bpf_map *m)
{
    KERNEL_RAW_SCOPE(g,&m->lock);
    if (m->spec.map_flags&F_RDONLY) return -KERNEL_EPERM;
    if (m->frozen) return -KERNEL_EBUSY;
    __atomic_store_n(&m->frozen,1,__ATOMIC_RELEASE); return 0;
}
int bpf_map_lookup(struct bpf_map *m,const void *key,void *value,int user,unsigned cpu)
{
    if (m->ring) return -KERNEL_EINVAL;
    if (!key || !value || (!user && cpu>=m->cpus)) return -KERNEL_EINVAL;
    KERNEL_RAW_SCOPE(g,&m->lock);
    int err=access_ok(m,user,0); if (err) return err;
    void *p=lookup_locked(m,key); if (!p) return -KERNEL_ENOENT;
    if (!user && is_percpu(m)) p=(unsigned char *)p+cpu*m->stride;
    if (user && is_percpu(m)) memset(value,0,m->value_bytes);
    copy_value(m,value,p,user); return 0;
}
int bpf_map_update(struct bpf_map *m,const void *key,const void *value,uint64_t flags,int user,unsigned cpu)
{
    if (flags>2 || m->ring || !key || !value || (!user && cpu>=m->cpus)) return -KERNEL_EINVAL;
    KERNEL_RAW_SCOPE(g,&m->lock);
    int err=access_ok(m,user,1); if (err) return err;
    void *p;
    if (is_array(m)) {
        uint32_t i; memcpy(&i,key,4); if (i>=m->spec.max_entries) return -KERNEL_E2BIG;
        if (flags==1) return -KERNEL_EEXIST;
        p=(unsigned char *)m->array+i*m->value_bytes;
    } else {
        struct bpf_entry **link=find_entry(m,key), *e=*link;
        if (e && flags==1) return -KERNEL_EEXIST;
        if (!e && flags==2) return -KERNEL_ENOENT;
        if (!e || !is_percpu(m)) {
            if ((!e && m->count==m->spec.max_entries) || !m->free) return -KERNEL_E2BIG;
            struct bpf_entry *old=e;
            e=m->free; m->free=e->next;
            memcpy(e->bytes,key,m->spec.key_size);
            memset(entry_value(m,e),0,m->value_bytes);
            e->next=old ? old->next : *link; *link=e;
            if (old) retire(m,old); else m->count++;
        }
        p=entry_value(m,e);
    }
    if (!user && is_percpu(m)) p=(unsigned char *)p+cpu*m->stride;
    copy_value(m,p,value,user); return 0;
}
int bpf_map_delete(struct bpf_map *m,const void *key,int user)
{
    if (m->ring || !key) return -KERNEL_EINVAL;
    KERNEL_RAW_SCOPE(g,&m->lock);
    int err=access_ok(m,user,1); if (err) return err;
    if (is_array(m)) return -KERNEL_EINVAL;
    struct bpf_entry **link=find_entry(m,key), *e=*link;
    if (!e) return -KERNEL_ENOENT;
    *link=e->next; m->count--; retire(m,e); return 0;
}
int bpf_map_next_key(struct bpf_map *m,const void *key,void *next)
{
    if (m->ring || !next) return -KERNEL_EINVAL;
    KERNEL_RAW_SCOPE(g,&m->lock);
    int error=access_ok(m,1,0);if(error)return error;
    if (is_array(m)) {
        uint32_t i=UINT32_MAX; if (key) memcpy(&i,key,4);
        i=i>=m->spec.max_entries ? 0 : i+1;
        if (i==m->spec.max_entries) return -KERNEL_ENOENT;
        memcpy(next,&i,4); return 0;
    }
    unsigned b=0;
    if (key) {
        struct bpf_entry *e=*find_entry(m,key);
        if (e) { if (e->next) { memcpy(next,e->next->bytes,m->spec.key_size); return 0; } b=bucket(m,key)+1; }
    }
    for (;b<m->bucket_count;b++) if (m->buckets[b]) {
        memcpy(next,m->buckets[b]->bytes,m->spec.key_size); return 0;
    }
    return -KERNEL_ENOENT;
}
int bpf_map_lookup_and_delete(struct bpf_map *m,const void *key,void *value)
{
    if(!key || !value)return -KERNEL_EINVAL;
    KERNEL_RAW_SCOPE(g,&m->lock);
    int error=access_ok(m,1,0);if(!error)error=access_ok(m,1,1);
    if(error)return error;
    /* Linux exposes its internal ENOTSUPP for non-pop map types. */
    if(m->ring || is_array(m))return -524;
    struct bpf_entry **link=find_entry(m,key),*entry=*link;
    if(!entry)return -KERNEL_ENOENT;
    if(is_percpu(m))memset(value,0,m->value_bytes);
    copy_value(m,value,entry_value(m,entry),1);
    *link=entry->next;m->count--;retire(m,entry);return 0;
}
void bpf_map_borrow_begin(struct bpf_map *m)
{ KERNEL_RAW_SCOPE(g,&m->lock); if (++m->borrowers==0) __builtin_trap(); }
void bpf_map_borrow_end(struct bpf_map *m)
{
    KERNEL_RAW_SCOPE(g,&m->lock);
    if (!m->borrowers) __builtin_trap();
    if (--m->borrowers) return;
    while (m->retired) {
        struct bpf_entry *e=m->retired; m->retired=e->next; e->next=m->free; m->free=e;
    }
}
void *bpf_map_lookup_borrowed(struct bpf_map *m,const void *key,unsigned cpu)
{
    if (m->ring || !key || cpu>=m->cpus) return 0;
    KERNEL_RAW_SCOPE(g,&m->lock);
    if (!m->borrowers) __builtin_trap();
    void *p=lookup_locked(m,key);
    return p && is_percpu(m) ? (unsigned char *)p+cpu*m->stride : p;
}
int bpf_map_lookup_batch(struct bpf_map *m,const uint32_t *in,uint32_t *out,
    void *keys,void *values,uint32_t *count,int remove)
{
    uint32_t capacity=*count; *count=0;
    if (m->ring || !out || !keys || !values || (remove && is_array(m))) return -KERNEL_EINVAL;
    KERNEL_RAW_SCOPE(g,&m->lock);
    int err=access_ok(m,1,0); if (!err && remove) err=access_ok(m,1,1); if (err) return err;
    uint32_t cursor=in ? *in : 0;
    if (is_array(m)) {
        while (cursor<m->spec.max_entries && *count<capacity) {
            memcpy((unsigned char *)keys+*count*4,&cursor,4);
            void *dst=(unsigned char *)values+*count*bpf_map_user_value_size(m);
            if (is_percpu(m)) memset(dst,0,m->value_bytes);
            copy_value(m,dst,(unsigned char *)m->array+cursor*m->value_bytes,1);
            cursor++; (*count)++;
        }
        *out=cursor; return cursor==m->spec.max_entries ? -KERNEL_ENOENT : 0;
    }
    for (;cursor<m->bucket_count;cursor++) {
        unsigned n=0; for (struct bpf_entry *e=m->buckets[cursor];e;e=e->next) n++;
        if (n>capacity-*count) { *out=cursor; return *count ? 0 : -KERNEL_ENOSPC; }
        struct bpf_entry *e=m->buckets[cursor];
        while (e) {
            struct bpf_entry *next=e->next;
            memcpy((unsigned char *)keys+*count*m->spec.key_size,e->bytes,m->spec.key_size);
            void *dst=(unsigned char *)values+*count*bpf_map_user_value_size(m);
            if (is_percpu(m)) memset(dst,0,m->value_bytes);
            copy_value(m,dst,entry_value(m,e),1); (*count)++;
            if (remove) { retire(m,e); m->count--; }
            e=next;
        }
        if (remove) m->buckets[cursor]=0;
    }
    *out=cursor; return -KERNEL_ENOENT;
}
int bpf_map_update_batch(struct bpf_map *m,const void *keys,const void *values,uint32_t *count,uint64_t flags)
{
    uint32_t n=*count; *count=0;
    for (;*count<n;(*count)++) {
        int e=bpf_map_update(m,(const unsigned char *)keys+*count*m->spec.key_size,
            (const unsigned char *)values+*count*bpf_map_user_value_size(m),flags,1,0);
        if (e) return e;
    }
    return 0;
}
int bpf_map_delete_batch(struct bpf_map *m,const void *keys,uint32_t *count)
{
    uint32_t n=*count; *count=0;
    for (;*count<n;(*count)++) {
        int e=bpf_map_delete(m,(const unsigned char *)keys+*count*m->spec.key_size,1);
        if (e) return e;
    }
    return 0;
}
uint64_t bpf_map_mmap_size(const struct bpf_map *m)
{
    if (m->ring) return 2*BOAROS_PAGE_SIZE+2*(uint64_t)m->spec.max_entries;
    if (!(m->spec.map_flags&F_MMAPABLE)) return 0;
    return (m->value_bytes*m->spec.max_entries+BOAROS_PAGE_SIZE-1U)&~(uint64_t)(BOAROS_PAGE_SIZE-1U);
}
int bpf_map_mmap_check(struct bpf_map *m,uint64_t offset,uint64_t length,int writable)
{
    uint64_t size=bpf_map_mmap_size(m);
    if (!length || (offset|length)&(BOAROS_PAGE_SIZE-1U) || offset>size || length>size-offset) return -KERNEL_EINVAL;
    if (writable && bpf_map_is_frozen(m)) return -KERNEL_EPERM;
    if (m->ring) return writable && (offset || length!=BOAROS_PAGE_SIZE) ? -KERNEL_EPERM : 0;
    if (writable && (m->spec.map_flags&F_RDONLY_PROG)) return -KERNEL_EACCES;
    if (writable && (m->spec.map_flags&F_RDONLY)) return -KERNEL_EPERM;
    return 0;
}
int bpf_map_page_get(struct bpf_map *m,uint64_t offset,int writable,uint64_t *physical)
{
    int e=bpf_map_mmap_check(m,offset,BOAROS_PAGE_SIZE,writable); if (e) return e;
    if (m->ring) return bpf_ring_page_get(m->ring,offset,writable,physical);
    uint64_t p=m->physical+offset;
    if (physical_page_acquire(m->pages,p)) __builtin_trap();
    *physical=p; return 0;
}
struct bpf_ring *bpf_map_ring(struct bpf_map *m) { return m->ring; }
struct kernel_wait_queue *bpf_map_wait_queue(struct bpf_map *m)
{ return m->ring ? bpf_ring_wait_queue(m->ring) : 0; }
int bpf_map_poll_readable(struct bpf_map *m)
{ return m->ring ? bpf_ring_poll_readable(m->ring) : 0; }
