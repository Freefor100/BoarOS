#define _POSIX_C_SOURCE 200809L
#include <kernel/hugetlb.h>
#include "../../kernel/sched/futex_internal.h"
#include <kernel/heap.h>
#include <kernel/sync.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Non-hugetlb key branches are deliberately unreachable in this harness. */
int kernel_vfs_node_acquire(struct kernel_vfs_node *node) { (void)node;abort(); }
int kernel_vfs_node_release(struct kernel_vfs_node **owner) { (void)owner;abort(); }
void mm_test_attach(unsigned);
void kernel_console_putc(char c) { fputc(c,stderr); }
void mm_test_fence(uint64_t address,int instructions) { (void)address;(void)instructions; }
void mm_test_unavailable(void) { abort(); }
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
static uint32_t order;
static size_t huge_size;
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int page_address(const void *p,uint64_t *address) { *address=(uintptr_t)p;return 1; }
static struct kernel_hstate_snapshot snapshot(struct kernel_hstate *h)
{ struct kernel_hstate_snapshot s;kernel_hstate_snapshot(h,&s);assert(s.reserved<=s.free && s.free<=s.total);return s; }
static uint64_t instantiate(struct kernel_hugetlb_map *m,uint64_t index)
{
    struct kernel_hugetlb_claim c={0};
    assert(!kernel_hugetlb_fault_begin(m,index,0,&c));
    assert(c.order==order && !((unsigned char *)(uintptr_t)c.physical_address)[huge_size-1]);
    uint64_t physical=c.physical_address;
    kernel_hugetlb_fault_finish(&c,1);assert(!c.map);return physical;
}
static struct kernel_hugetlb_mapping *other_registration;
static unsigned revoke_calls;
static enum kernel_hugetlb_status revoke(void *owner,struct kernel_hugetlb_map *m,uint64_t index,uint64_t physical)
{
    struct kernel_hugetlb_mapping *self=owner;
    (void)physical;
    uint64_t epoch;
    assert(kernel_hugetlb_map_epoch(m,&epoch)==KERNEL_HUGETLB_BUSY);
    struct kernel_hugetlb_claim c={0};
    assert(kernel_hugetlb_fault_begin(m,index,1,&c)==KERNEL_HUGETLB_BUSY);
    assert(self->borrows);
    kernel_hugetlb_mapping_stop(self);
    kernel_hugetlb_mapping_stop(other_registration);
    assert(!kernel_hugetlb_map_revoke(m,index));
    revoke_calls++;
    return KERNEL_HUGETLB_OK;
}
static pthread_barrier_t gate;
static struct kernel_hugetlb_map *parallel_map;
static uint64_t parallel_heads[2];
static void barrier(void) { int s=pthread_barrier_wait(&gate);assert(!s || s==PTHREAD_BARRIER_SERIAL_THREAD); }
static void *worker(void *arg)
{
    unsigned id=(uintptr_t)arg;mm_test_attach(id);
    struct kernel_hugetlb_claim c={0};
    assert(!kernel_hugetlb_fault_begin(parallel_map,id,0,&c));
    parallel_heads[id]=c.physical_address;
    memset((void *)(uintptr_t)c.physical_address,id+1,huge_size);
    barrier();
    assert(parallel_heads[0]!=parallel_heads[1]);
    kernel_hugetlb_fault_finish(&c,1);
    barrier();
    assert(((unsigned char *)(uintptr_t)parallel_heads[id])[huge_size-1]==id+1);
    physical_page_release_order(&allocator,parallel_heads[id],order);
    return NULL;
}
int main(void)
{
    mm_test_attach(0);order=BOAROS_PAGE_SHIFT==12?9:11;huge_size=(size_t)BOAROS_PAGE_SIZE<<order;
    void *pool;assert(!posix_memalign(&pool,huge_size,8*huge_size));
    struct boot_memory_layout layout={0};layout.usable_count=1;
    layout.usable[0].base=(uintptr_t)pool;layout.usable[0].size=8*huge_size;
    assert(!physical_page_allocator_init(&allocator,&layout));
    assert(!physical_page_allocator_bind_access(&allocator,access_page));
    assert(!physical_page_allocator_finalize(&allocator));
    assert(!kernel_heap_init(&heap,&allocator,page_address));
    uint64_t baseline=physical_page_available(&allocator);
    struct kernel_hstate *h=NULL,*borrow=NULL;
    assert(kernel_hstate_get(&allocator,&borrow)==KERNEL_HUGETLB_NOT_FOUND && !borrow);
    assert(!kernel_hstate_create(&heap,&allocator,order,&h));
    assert(!kernel_hstate_get(&allocator,&borrow) && borrow==h);
    assert(kernel_hstate_destroy(&h)==KERNEL_HUGETLB_BUSY);
    kernel_hstate_put(borrow);
    assert(!kernel_hstate_resize(h,3));assert(snapshot(h).free==3);
    struct kernel_hugetlb_map *owner=NULL,*nr=NULL,*child=NULL;
    assert(!kernel_hugetlb_map_create(h,2,KERNEL_HUGETLB_PRIVATE,0,&owner));
    assert(snapshot(h).reserved==2);
    assert(kernel_hugetlb_map_create(h,2,KERNEL_HUGETLB_PRIVATE,0,&child)==KERNEL_HUGETLB_NO_MEMORY && !child);
    assert(!kernel_hugetlb_map_create(h,2,KERNEL_HUGETLB_PRIVATE,1,&nr));
    uint64_t unreserved=instantiate(nr,0);
    struct kernel_hugetlb_claim c={0};
    assert(!kernel_hugetlb_fault_begin(owner,0,0,&c));
    assert(snapshot(h).reserved==1 && snapshot(h).free==1);
    kernel_hugetlb_fault_finish(&c,0);
    assert(snapshot(h).reserved==2 && snapshot(h).free==2);
    uint64_t first=instantiate(owner,0),second=instantiate(owner,1);
    assert(snapshot(h).free==0 && !snapshot(h).reserved);
    struct kernel_hugetlb_discard_claim discard={0};
    assert(!kernel_hugetlb_discard_begin(owner,0,first,1,&discard));
    assert(discard.exclusive && !snapshot(h).free);
    kernel_hugetlb_discard_finish(&discard,0);
    assert(kernel_hugetlb_discard_begin(owner,0,first,0,&discard)==KERNEL_HUGETLB_BUSY);
    physical_page_acquire_order(&allocator,first,order);
    assert(kernel_hugetlb_discard_begin(owner,0,first,1,&discard)==KERNEL_HUGETLB_BUSY);
    assert(!discard.map);
    assert(kernel_hugetlb_discard(owner,0,first)==KERNEL_HUGETLB_BUSY);
    uint32_t unchanged;
    assert(!physical_page_reference_count(&allocator,first,&unchanged) && unchanged==2);
    physical_page_release_order(&allocator,first,order);
    assert(kernel_hugetlb_fault_begin(nr,1,0,&c)==KERNEL_HUGETLB_BUS);
    assert(kernel_hugetlb_fault_begin(owner,0,1,&c)==KERNEL_HUGETLB_REVOKE_CHILDREN);
    assert(!kernel_hugetlb_map_fork(owner,&child));
    assert(kernel_hugetlb_map_is_child_of(child,owner));
    assert(kernel_hugetlb_fault_begin(child,0,1,&c)==KERNEL_HUGETLB_BUS);
    physical_page_release_order(&allocator,unreserved,order);
    assert(!kernel_hugetlb_fault_begin(child,0,1,&c));kernel_hugetlb_fault_finish(&c,0);
    assert(snapshot(h).free==1);
    uint32_t refs;physical_page_reference_count(&allocator,first,&refs);assert(refs==1);
    assert(!kernel_hugetlb_discard_begin(owner,0,first,1,&discard));
    assert(discard.exclusive && !discard.reserved);
    assert(kernel_hugetlb_map_unmap(owner,0,1)==KERNEL_HUGETLB_BUSY);
    assert(kernel_hugetlb_fault_begin(owner,0,1,&c)==KERNEL_HUGETLB_BUSY);
    kernel_hugetlb_discard_finish(&discard,0);
    assert(!physical_page_reference_count(&allocator,first,&refs) && refs==1);
    assert(!kernel_hugetlb_discard_begin(owner,0,first,1,&discard));
    kernel_hugetlb_discard_finish(&discard,1);
    assert(snapshot(h).free==2 && snapshot(h).reserved==1);
    first=instantiate(owner,0);
    physical_page_acquire_order(&allocator,first,order);
    assert(!kernel_hugetlb_discard_begin(owner,0,first,1,&discard));
    assert(!discard.exclusive && discard.reserved && snapshot(h).reserved==1);
    kernel_hugetlb_discard_finish(&discard,0);
    assert(!snapshot(h).reserved);
    assert(!physical_page_reference_count(&allocator,first,&refs) && refs==2);
    assert(!kernel_hugetlb_discard_begin(owner,0,first,0,&discard));
    kernel_hugetlb_discard_finish(&discard,1);
    assert(snapshot(h).reserved==1); /* extra capacity, old pin remains valid */
    assert(!physical_page_reference_count(&allocator,first,&refs) && refs==1);
    physical_page_release_order(&allocator,first,order);
    assert(!kernel_hugetlb_map_unmap(owner,0,1));assert(!snapshot(h).reserved);
    physical_page_release_order(&allocator,second,order);
    kernel_hugetlb_map_release(&child);kernel_hugetlb_map_release(&nr);kernel_hugetlb_map_release(&owner);
    assert(snapshot(h).free==3 && !snapshot(h).maps);

    struct kernel_hugetlb_map *shared=NULL,*shared_fork=NULL;
    assert(!kernel_hugetlb_map_create(h,1,KERNEL_HUGETLB_SHARED,0,&shared));
    assert(!kernel_hugetlb_map_resident(shared,0));
    assert(!kernel_hugetlb_map_resident(shared,1));
    assert(!kernel_hugetlb_map_fork(shared,&shared_fork) && shared==shared_fork);
    struct kernel_hugetlb_claim a={0},b={0};
    assert(!kernel_hugetlb_fault_begin(shared,0,0,&a));
    assert(!kernel_hugetlb_fault_begin(shared_fork,0,0,&b));
    assert(a.physical_address==b.physical_address);
    uint64_t cache=a.physical_address;
    assert(!kernel_hugetlb_map_resident(shared,0));
    kernel_hugetlb_fault_finish(&a,0);kernel_hugetlb_fault_finish(&b,1);
    assert(!physical_page_reference_count(&allocator,cache,&refs) && refs==2);
    assert(!kernel_hugetlb_map_unmap(shared,0,1));
    physical_page_release_order(&allocator,cache,order);
    assert(!physical_page_reference_count(&allocator,cache,&refs) && refs==1);
    assert(kernel_hugetlb_map_resident(shared,0));
    assert(!physical_page_reference_count(&allocator,cache,&refs) && refs==1);
    struct kernel_futex_key key={.kind=KERNEL_FUTEX_KEY_SHARED_HUGETLB,
        .identity=(uintptr_t)shared,.offset=BOAROS_PAGE_SIZE+4,.hugetlb_map=shared};
    futex_key_hold(&key); /* MM publishes an owning key while VMA is stable. */
    struct kernel_futex_key queued=key;
    futex_key_hold(&queued); /* requeue/PI state copy gets a distinct reference. */
    assert(futex_key_equal(&key,&queued));
    struct kernel_futex_key different=key;different.offset+=4;
    assert(!futex_key_equal(&key,&different));
    different=key;different.kind=KERNEL_FUTEX_KEY_PRIVATE;
    assert(!futex_key_equal(&key,&different));
    kernel_hugetlb_map_release(&shared);
    kernel_hugetlb_map_release(&shared_fork);
    assert(snapshot(h).free==2 && snapshot(h).maps==1);
    futex_key_release(&key);
    assert(!key.identity && !key.hugetlb_map && key.kind==KERNEL_FUTEX_KEY_PRIVATE);
    assert(snapshot(h).free==2 && snapshot(h).maps==1);
    futex_key_release(&queued);
    assert(snapshot(h).free==3 && !snapshot(h).maps);

    assert(!kernel_hugetlb_map_create(h,2,KERNEL_HUGETLB_PRIVATE,0,&parallel_map));
    assert(!pthread_barrier_init(&gate,NULL,2));pthread_t threads[2];
    for(unsigned i=0;i<2;i++) assert(!pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)i));
    for(unsigned i=0;i<2;i++) assert(!pthread_join(threads[i],NULL));
    assert(!pthread_barrier_destroy(&gate));
    assert(snapshot(h).free==3);kernel_hugetlb_map_release(&parallel_map);

    assert(!kernel_hugetlb_map_create(h,1,KERNEL_HUGETLB_PRIVATE,0,&owner));
    first=instantiate(owner,0);
    struct kernel_hugetlb_map *child2;
    assert(!kernel_hugetlb_map_fork(owner,&child));assert(!kernel_hugetlb_map_fork(child,&child2));
    struct kernel_hugetlb_mapping r1={.map=child,.revoke=revoke},r2={.map=child2,.revoke=revoke};
    r1.owner=&r1;r2.owner=&r2;uint64_t epoch;
    assert(!kernel_hugetlb_map_epoch(child,&epoch));
    assert(!kernel_hugetlb_mapping_register(&r1,epoch));assert(!kernel_hugetlb_mapping_register(&r2,epoch));
    other_registration=&r1;
    assert(!kernel_hugetlb_revoke_children(owner,0,first));assert(revoke_calls==1);
    kernel_hugetlb_mapping_drain(&r1);kernel_hugetlb_mapping_drain(&r2);
    assert(!r1.borrows && !r2.borrows);
    assert(kernel_hugetlb_mapping_register(&r1,epoch)==KERNEL_HUGETLB_BUSY);
    assert(kernel_hugetlb_fault_begin(child2,0,0,&c)==KERNEL_HUGETLB_BUS);
    kernel_hugetlb_map_release(&child);kernel_hugetlb_map_release(&child2);
    assert(!kernel_hstate_resize(h,0));
    struct kernel_hstate_snapshot s=snapshot(h);assert(s.total==1 && !s.persistent && s.surplus==1);
    physical_page_release_order(&allocator,first,order);assert(!snapshot(h).total);
    kernel_hugetlb_map_release(&owner);
    assert(!kernel_hstate_destroy(&h) && !h);
    assert(physical_page_available(&allocator)==baseline && !physical_page_allocator_audit(&allocator));
    free(pool);
    printf("hugetlb native order %u: reservation/abort/shared/private/fork/pin/custody/surplus/registry/concurrency passed\n",order);
}
