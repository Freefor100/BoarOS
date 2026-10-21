#include "../../fs/lwext4_port.h"
#include <ext4.h>
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <kernel/cpu.h>
#include <kernel/sync.h>
#include <kernel/physical_page.h>
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

void mm_test_attach(unsigned);
void kernel_console_putc(char byte) { fputc(byte,stderr); }
static unsigned char arena[8*1024*1024] __attribute__((aligned(16384)));
static void *page_access(uint64_t address) { return (void *)(uintptr_t)address; }
static int page_address(const void *page,uint64_t *address) { *address=(uintptr_t)page;return 1; }
static int allocation_fail=-1;
static _Thread_local unsigned allocation_actor,allocation_count,allocation_pause;
static _Thread_local unsigned allocation_calls;
static unsigned candidates[2],candidate_resume,resources[2],resource_resume;
static void *last_release_target;
static unsigned last_release_pause,last_release_entered,last_release_resume;
void __real_kernel_heap_release(struct kernel_heap *,void *);
void __wrap_kernel_heap_release(struct kernel_heap *heap,void *pointer)
{
    if(pointer==last_release_target && __atomic_exchange_n(&last_release_pause,0,__ATOMIC_ACQ_REL)) {
        assert(!kernel_cpu_current()->raw_locks && !kernel_io_context_current()->locks);
        __atomic_store_n(&last_release_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&last_release_resume,__ATOMIC_ACQUIRE))sched_yield();
    }
    __real_kernel_heap_release(heap,pointer);
}
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *heap,size_t count,
    size_t size,void **owner)
{
    allocation_calls++;
    if (allocation_fail==0) return KERNEL_HEAP_STATUS_EMPTY;
    if (allocation_fail>0) allocation_fail--;
    enum kernel_heap_status result=__real_kernel_heap_allocate_zeroed(heap,count,size,owner);
    if (!result && allocation_pause && ++allocation_count==1) {
        __atomic_store_n(&candidates[allocation_actor],1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&candidate_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return result;
}
static void resources_case(struct kernel_heap *heap)
{
    struct kernel_heap_statistics before,after;
    kernel_heap_get_statistics(heap,&before);
    struct ext4_lock callbacks={0};struct boaros_lwext4_domain domain={0};
    struct jbd_journal journal={.grouped=true};struct ext4_fs fs={.jbd_journal=&journal};
    fs.orphan_index=(struct ext4_orphan_index *)&fs;
    struct ext4_operation operations[5]={{.fs=&fs},{.fs=&fs},{.fs=&fs},{.fs=&fs},{.fs=&fs}};
    boaros_lwext4_lock_init(&callbacks,&domain,heap);
    boaros_lwext4_enable_parallel(&domain,&fs);
    callbacks.lock(&domain);
    assert(callbacks.serial_writer && !callbacks.serial_writer(&domain));
    assert(!kernel_lock_held(&domain.serial,0));
    allocation_fail=0;
    assert(callbacks.resource_acquire(&domain,&operations[0],EXT4_RESOURCE_BLOCK_GROUP,8)==ENOMEM);
    allocation_fail=-1;
    assert(!callbacks.resource_acquire(&domain,&operations[0],EXT4_RESOURCE_BLOCK_GROUP,8));
    struct kernel_lock_guard *held=kernel_io_context_current()->locks;
    assert(held && held->lock->rank==35 && held->lock->key==8);
    allocation_fail=0;
    assert(callbacks.resource_acquire(&domain,&operations[0],EXT4_RESOURCE_BLOCK_GROUP,9)==ENOMEM);
    allocation_fail=-1;
    assert(kernel_io_context_current()->locks==held);
    assert(callbacks.resource_acquire(&domain,&operations[0],EXT4_RESOURCE_ORPHAN,1)==EAGAIN);
    callbacks.resource_release(&domain);
    assert(!kernel_io_context_current()->locks && callbacks.resource_retry(&domain) &&
           !callbacks.resource_retry(&domain));
    assert(!callbacks.resource_acquire(&domain,&operations[1],EXT4_RESOURCE_BLOCK_GROUP,8));
    held=kernel_io_context_current()->locks;
    assert(held->lock->rank==35 && held->lock->key==8 && held->previous &&
           held->previous->lock->rank==32 && held->previous->lock->key==1);
    assert(!callbacks.resource_acquire(&domain,&operations[1],EXT4_RESOURCE_ORPHAN,1));
    assert(callbacks.resource_acquire(&domain,&operations[1],EXT4_RESOURCE_BLOCK_GROUP,2)==EAGAIN);
    callbacks.resource_release(&domain);
    assert(callbacks.resource_retry(&domain));
    assert(!callbacks.resource_acquire(&domain,&operations[2],EXT4_RESOURCE_BLOCK_GROUP,8));
    held=kernel_io_context_current()->locks;
    assert(held->lock->key==8 && held->previous->lock->key==2 &&
           held->previous->previous->lock->rank==32);
    assert(callbacks.resource_acquire(&domain,&operations[2],EXT4_RESOURCE_INODE,17)==EAGAIN);
    callbacks.resource_release(&domain);
    assert(callbacks.resource_retry(&domain));
    assert(!callbacks.resource_acquire(&domain,&operations[3],EXT4_RESOURCE_BLOCK_GROUP,8));
    held=kernel_io_context_current()->locks;
    assert(held->previous->previous->previous->lock->rank==31 &&
           held->previous->previous->previous->lock->key==17);
    callbacks.state_lock(&domain);callbacks.state_lock(&domain);
    assert(kernel_io_context_current()->locks->lock->rank==40);
    callbacks.state_unlock(&domain);callbacks.state_unlock(&domain);
    allocation_fail=0;
    assert(callbacks.resource_claim_inode(&domain,&operations[3],777)==ENOMEM &&
           kernel_io_context_current()->locks==held);
    allocation_fail=1;
    assert(callbacks.resource_claim_inode(&domain,&operations[3],777)==ENOMEM &&
           kernel_io_context_current()->locks==held);
    allocation_fail=-1;
    assert(!callbacks.resource_claim_inode(&domain,&operations[3],777) &&
           !callbacks.resource_acquire(&domain,&operations[3],EXT4_RESOURCE_INODE,777) &&
           kernel_io_context_current()->locks==held);
    assert(callbacks.resource_acquire(&domain,&operations[3],EXT4_RESOURCE_BLOCK_GROUP,1)==EAGAIN);
    callbacks.resource_release(&domain);
    assert(callbacks.resource_retry(&domain) &&
           !callbacks.resource_acquire(&domain,&operations[4],EXT4_RESOURCE_BLOCK_GROUP,8));
    unsigned claimed_locked=0;
    for(struct kernel_lock_guard *guard=kernel_io_context_current()->locks;guard;guard=guard->previous)
        if(guard->lock->rank==31 && guard->lock->key==777)claimed_locked++;
    assert(claimed_locked==1 && !callbacks.resource_acquire(&domain,&operations[4],EXT4_RESOURCE_INODE,777));
    callbacks.resource_release(&domain);
    assert(!callbacks.resource_retry(&domain));
    callbacks.unlock(&domain);
    assert(!domain.resources.rbh_root);
    boaros_lwext4_lock_fini(&domain);
    kernel_heap_get_statistics(heap,&after);
    assert(after.live_allocations==before.live_allocations && after.current_pages==before.current_pages &&
           !kernel_io_context_current()->backend_resources.request_count &&
           !kernel_io_context_current()->backend_resources.head);
    puts("PASS parallel operation resources: nested state, ENOMEM, complete release, sorted inode/orphan/group replay and returned allocations");
}
static void resource_reclamation_case(struct kernel_heap *heap)
{
    struct kernel_heap_statistics before,active,after;kernel_heap_get_statistics(heap,&before);
    struct ext4_lock callbacks={0};struct boaros_lwext4_domain domain={0};
    struct jbd_journal journal={.grouped=true};struct ext4_fs fs={.jbd_journal=&journal};
    fs.orphan_index=(struct ext4_orphan_index *)&fs;
    boaros_lwext4_lock_init(&callbacks,&domain,heap);boaros_lwext4_enable_parallel(&domain,&fs);
    uint64_t peak_pages=0,peak_allocations=0;
    unsigned calls=allocation_calls;
    for(unsigned i=0;i<2048;i++) {
        struct ext4_operation operation={.fs=&fs};callbacks.lock(&domain);
        assert(!callbacks.resource_acquire(&domain,&operation,EXT4_RESOURCE_INODE,0x100000U+i) &&
               !callbacks.resource_acquire(&domain,&operation,EXT4_RESOURCE_BLOCK_GROUP,i%16) &&
               !callbacks.resource_claim_inode(&domain,&operation,0x80000000U+i));
        kernel_heap_get_statistics(heap,&active);
        if(active.current_pages-before.current_pages>peak_pages)peak_pages=active.current_pages-before.current_pages;
        if(active.live_allocations-before.live_allocations>peak_allocations)
            peak_allocations=active.live_allocations-before.live_allocations;
        callbacks.resource_release(&domain);callbacks.unlock(&domain);
        assert(!domain.resources.rbh_root);
        kernel_heap_get_statistics(heap,&after);
        assert(after.live_allocations==before.live_allocations && after.current_pages==before.current_pages);
    }
    fprintf(stderr,"resource storage: 2048 operations, %u allocation calls, peak=%llu allocations/%llu pages\n",
        allocation_calls-calls,(unsigned long long)peak_allocations,(unsigned long long)peak_pages);
    assert(peak_pages<=4 && peak_allocations==3 && allocation_calls-calls==2048*3);
    boaros_lwext4_lock_fini(&domain);
    printf("PASS 2048 distinct high inode keys: no request heap allocation; each operation returns registry/heap/pages; active peak=%llu allocations/%llu pages\n",
        (unsigned long long)peak_allocations,(unsigned long long)peak_pages);
}
struct last_resource_actor {
    struct ext4_lock *callbacks;
    struct ext4_fs *fs;
    struct kernel_rwlock *lock;
    unsigned actor,entered,resume;
};
static void *last_resource_owner(void *argument)
{
    struct last_resource_actor *actor=argument;mm_test_attach(actor->actor+1);
    struct ext4_operation operation={.fs=actor->fs};
    actor->callbacks->lock(actor->callbacks->context);
    assert(!actor->callbacks->resource_acquire(actor->callbacks->context,&operation,EXT4_RESOURCE_INODE,777));
    actor->lock=kernel_io_context_current()->locks->lock;
    __atomic_store_n(&actor->entered,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&actor->resume,__ATOMIC_ACQUIRE))sched_yield();
    actor->callbacks->resource_release(actor->callbacks->context);
    actor->callbacks->unlock(actor->callbacks->context);
    return NULL;
}
static void last_resource_case(struct kernel_heap *heap)
{
    struct kernel_heap_statistics before,after;kernel_heap_get_statistics(heap,&before);
    struct ext4_lock callbacks={0};struct boaros_lwext4_domain domain={0};
    struct jbd_journal journal={.grouped=true};struct ext4_fs fs={.jbd_journal=&journal};
    fs.orphan_index=(struct ext4_orphan_index *)&fs;
    boaros_lwext4_lock_init(&callbacks,&domain,heap);boaros_lwext4_enable_parallel(&domain,&fs);
    struct last_resource_actor actors[2]={{.callbacks=&callbacks,.fs=&fs},{.callbacks=&callbacks,.fs=&fs,.actor=1}};
    pthread_t threads[2];assert(!pthread_create(&threads[0],NULL,last_resource_owner,&actors[0]));
    while(!__atomic_load_n(&actors[0].entered,__ATOMIC_ACQUIRE))sched_yield();
    {KERNEL_RAW_SCOPE(guard,&domain.resource_index);last_release_target=domain.resources.rbh_root;}
    assert(last_release_target);last_release_entered=last_release_resume=0;last_release_pause=1;
    __atomic_store_n(&actors[0].resume,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&last_release_entered,__ATOMIC_ACQUIRE))sched_yield();
    {KERNEL_RAW_SCOPE(guard,&domain.resource_index);assert(!domain.resources.rbh_root);}
    assert(!pthread_create(&threads[1],NULL,last_resource_owner,&actors[1]));
    while(!__atomic_load_n(&actors[1].entered,__ATOMIC_ACQUIRE))sched_yield();
    assert(actors[0].lock!=actors[1].lock);
    __atomic_store_n(&last_release_resume,1,__ATOMIC_RELEASE);assert(!pthread_join(threads[0],NULL));
    {KERNEL_RAW_SCOPE(guard,&domain.resource_index);assert(domain.resources.rbh_root);}
    __atomic_store_n(&actors[1].resume,1,__ATOMIC_RELEASE);assert(!pthread_join(threads[1],NULL));
    last_release_target=NULL;assert(!domain.resources.rbh_root);boaros_lwext4_lock_fini(&domain);
    kernel_heap_get_statistics(heap,&after);
    assert(after.live_allocations==before.live_allocations && after.current_pages==before.current_pages);
    puts("PASS last resource owner detaches before lock-free free; a same-key candidate publishes while old storage is still borrowed, with all owners returned");
}
static void inode_lifetime_case(struct kernel_heap *heap)
{
    struct kernel_heap_statistics before,after;kernel_heap_get_statistics(heap,&before);
    struct ext4_lock callbacks={0};struct boaros_lwext4_domain domain={0};
    struct jbd_journal journal={.grouped=true};struct ext4_fs fs={.jbd_journal=&journal};
    fs.orphan_index=(struct ext4_orphan_index *)&fs;
    boaros_lwext4_lock_init(&callbacks,&domain,heap);
    struct boaros_lwext4_resource *inode=NULL;
    assert(!boaros_lwext4_inode_get(&domain,777,&inode) && inode);
    boaros_lwext4_enable_parallel(&domain,&fs);
    unsigned calls=allocation_calls;
    for(unsigned i=0;i<8;i++) {
        struct ext4_operation operation={.fs=&fs};
        callbacks.lock(&domain);
        assert(!callbacks.resource_acquire(&domain,&operation,EXT4_RESOURCE_INODE,777));
        assert(kernel_io_context_current()->locks->lock->rank==31);
        callbacks.resource_release(&domain);callbacks.unlock(&domain);
    }
    assert(allocation_calls==calls);
    struct last_resource_actor actors[2]={{.callbacks=&callbacks,.fs=&fs},
        {.callbacks=&callbacks,.fs=&fs,.actor=1}};
    pthread_t threads[2];assert(!pthread_create(&threads[0],NULL,last_resource_owner,&actors[0]));
    while(!__atomic_load_n(&actors[0].entered,__ATOMIC_ACQUIRE))sched_yield();
    assert(!pthread_create(&threads[1],NULL,last_resource_owner,&actors[1]));
    for(;;) {
        unsigned queued;
        {KERNEL_RAW_SCOPE(guard,&actors[0].lock->metadata);queued=actors[0].lock->pending_head!=NULL;}
        if(queued)break;
        sched_yield();
    }
    boaros_lwext4_inode_put(&domain,&inode);
    assert(!inode && !actors[1].entered && domain.resources.rbh_root);
    __atomic_store_n(&actors[0].resume,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&actors[1].entered,__ATOMIC_ACQUIRE))sched_yield();
    assert(actors[0].lock==actors[1].lock);
    __atomic_store_n(&actors[1].resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(threads[0],NULL) && !pthread_join(threads[1],NULL));
    assert(!domain.resources.rbh_root);boaros_lwext4_lock_fini(&domain);
    kernel_heap_get_statistics(heap,&after);
    assert(after.live_allocations==before.live_allocations && after.current_pages==before.current_pages);
    puts("PASS inode reference retains one shared object across eight transactions; final node put during writer/queued-owner handoff preserves exclusion and returns all memory");
}
struct transition_actor {
    struct ext4_lock *callbacks;
    struct boaros_lwext4_domain *domain;
    struct ext4_fs *fs;
    unsigned enable,read,entered,resume;
};
static void *transition_enter(void *argument)
{
    struct transition_actor *actor=argument;
    mm_test_attach(actor->enable?1:2);
    if(actor->enable)boaros_lwext4_enable_parallel(actor->domain,actor->fs);
    else {
        if(actor->read)actor->callbacks->read_lock(actor->domain);
        else actor->callbacks->lock(actor->domain);
        assert(!kernel_lock_held(&actor->domain->serial,0) && !kernel_io_context_current()->backend_guard.lock);
    }
    __atomic_store_n(&actor->entered,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&actor->resume,__ATOMIC_ACQUIRE))sched_yield();
    if(!actor->enable)actor->callbacks->unlock(actor->domain);
    assert(!kernel_io_context_current()->locks && !kernel_io_context_current()->backend_depth);
    return NULL;
}
static void transition_case(struct kernel_heap *heap)
{
    for(unsigned read=0;read<2;read++) {
        struct ext4_lock callbacks={0};struct boaros_lwext4_domain domain={0};
        struct jbd_journal journal={.grouped=true};struct ext4_fs fs={.jbd_journal=&journal};
        fs.orphan_index=(struct ext4_orphan_index *)&fs;
        boaros_lwext4_lock_init(&callbacks,&domain,heap);callbacks.lock(&domain);
        struct transition_actor actors[2]={{.callbacks=&callbacks,.domain=&domain,.fs=&fs,.enable=1},
                                          {.callbacks=&callbacks,.domain=&domain,.fs=&fs,.read=read}};
        pthread_t threads[2];
        for(unsigned i=0;i<2;i++) {
            assert(!pthread_create(&threads[i],NULL,transition_enter,&actors[i]));
            for(;;) {
                unsigned queued;
                {KERNEL_RAW_SCOPE(guard,&domain.serial.metadata);
                 queued=domain.serial.pending_head?(domain.serial.pending_head==domain.serial.pending_tail?1:2):0;}
                if(queued==i+1)break;
                sched_yield();
            }
        }
        assert(!actors[0].entered && !actors[1].entered);
        callbacks.unlock(&domain);
        for(unsigned i=0;i<2;i++)while(!__atomic_load_n(&actors[i].entered,__ATOMIC_ACQUIRE))sched_yield();
        assert(domain.parallel && !domain.serial.writer && !domain.serial.readers);
        for(unsigned i=0;i<2;i++)__atomic_store_n(&actors[i].resume,1,__ATOMIC_RELEASE);
        for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
        boaros_lwext4_lock_fini(&domain);
    }
    puts("PASS startup transition drains legacy owners; queued read/write callers recheck mode and enter without the old serial guard");
}
struct resource_actor {
    struct ext4_lock *callbacks;
    struct ext4_fs *fs;
    unsigned actor;
    uint32_t key;
    struct kernel_rwlock *lock;
};
static void *acquire_resource(void *argument)
{
    struct resource_actor *actor=argument;
    mm_test_attach(actor->actor+1);kernel_cpu_current()->logical_id=actor->actor;
    allocation_actor=actor->actor;allocation_count=0;allocation_pause=1;
    struct ext4_operation operation={.fs=actor->fs};
    struct ext4_lock *callbacks=actor->callbacks;
    callbacks->lock(callbacks->context);
    assert(!callbacks->resource_acquire(callbacks->context,&operation,EXT4_RESOURCE_BLOCK_GROUP,actor->key));
    actor->lock=kernel_io_context_current()->locks->lock;
    __atomic_store_n(&resources[actor->actor],1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&resource_resume,__ATOMIC_ACQUIRE)) sched_yield();
    callbacks->resource_release(callbacks->context);
    callbacks->unlock(callbacks->context);
    assert(!kernel_io_context_current()->locks && !kernel_io_context_current()->backend_resources.request_count &&
           !kernel_io_context_current()->backend_resources.head &&
           !kernel_io_context_current()->backend_depth && !kernel_cpu_current()->raw_locks);
    return NULL;
}
static void shared_resource_case(struct kernel_heap *heap,int shared)
{
    struct kernel_heap_statistics before,after;kernel_heap_get_statistics(heap,&before);
    struct ext4_lock callbacks={0};struct boaros_lwext4_domain domain={0};
    struct jbd_journal journal={.grouped=true};struct ext4_fs fs={.jbd_journal=&journal};
    fs.orphan_index=(struct ext4_orphan_index *)&fs;
    boaros_lwext4_lock_init(&callbacks,&domain,heap);boaros_lwext4_enable_parallel(&domain,&fs);
    memset(candidates,0,sizeof(candidates));memset(resources,0,sizeof(resources));
    candidate_resume=resource_resume=0;
    struct resource_actor actors[2]={{.callbacks=&callbacks,.fs=&fs,.actor=0,.key=7},
                                    {.callbacks=&callbacks,.fs=&fs,.actor=1,.key=shared?7:8}};
    pthread_t threads[2];
    for(unsigned i=0;i<2;i++)assert(!pthread_create(&threads[i],NULL,acquire_resource,&actors[i]));
    while (!__atomic_load_n(&candidates[0],__ATOMIC_ACQUIRE) ||
           !__atomic_load_n(&candidates[1],__ATOMIC_ACQUIRE)) sched_yield();
    __atomic_store_n(&candidate_resume,1,__ATOMIC_RELEASE);
    unsigned first=0;
    if (!shared) {
        while (!__atomic_load_n(&resources[0],__ATOMIC_ACQUIRE) ||
               !__atomic_load_n(&resources[1],__ATOMIC_ACQUIRE)) sched_yield();
    } else {
    for (;;) {
        if (__atomic_load_n(&resources[0],__ATOMIC_ACQUIRE)) { first=0;break; }
        if (__atomic_load_n(&resources[1],__ATOMIC_ACQUIRE)) { first=1;break; }
        sched_yield();
    }
    for (;;) {
        unsigned waiting;
        { KERNEL_RAW_SCOPE(guard,&actors[first].lock->metadata);
          waiting=actors[first].lock->pending_head!=NULL; }
        if (waiting) break;
        sched_yield();
    }
    assert(!__atomic_load_n(&resources[!first],__ATOMIC_ACQUIRE));
    }
    __atomic_store_n(&resource_resume,1,__ATOMIC_RELEASE);
    for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
    assert((actors[0].lock==actors[1].lock)==!!shared);
    boaros_lwext4_lock_fini(&domain);
    kernel_heap_get_statistics(heap,&after);
    assert(after.live_allocations==before.live_allocations && after.current_pages==before.current_pages);
    puts(shared?
        "PASS competing resource candidates publish one mutex; second operation waits and all candidate/request storage returns":
        "PASS independent group operations hold different qualifications simultaneously and return all resources");
}
struct state_actor {
    struct ext4_lock *callbacks;
    unsigned id,write,entered,leave;
};
static void state_returned(void)
{
    struct kernel_io_context *context=kernel_io_context_current();
    assert(!context->locks && !context->backend_depth && !context->backend_domain &&
           !context->backend_state_depth && !context->backend_state_guard.lock &&
           !kernel_cpu_current()->raw_locks);
}
static void *state_actor(void *argument)
{
    struct state_actor *actor=argument;mm_test_attach(actor->id);
    struct ext4_lock *callbacks=actor->callbacks;
    callbacks->lock(callbacks->context);
    if(actor->write)callbacks->state_lock(callbacks->context);
    else callbacks->state_read_lock(callbacks->context);
    assert(kernel_io_context_current()->backend_state_guard.write==!!actor->write);
    __atomic_store_n(&actor->entered,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&actor->leave,__ATOMIC_ACQUIRE))sched_yield();
    callbacks->state_unlock(callbacks->context);callbacks->unlock(callbacks->context);
    state_returned();return NULL;
}
static void state_wait_entered(struct state_actor *actor)
{ while(!__atomic_load_n(&actor->entered,__ATOMIC_ACQUIRE))sched_yield(); }
static void state_leave_actor(struct state_actor *actor,pthread_t thread)
{
    __atomic_store_n(&actor->leave,1,__ATOMIC_RELEASE);
    assert(!pthread_join(thread,NULL));
}
static void state_wait_queued(struct kernel_rwlock *lock,unsigned count)
{
    for(;;) {
        unsigned pending=0;
        {KERNEL_RAW_SCOPE(guard,&lock->metadata);pending=lock->waiters.registrations;}
        if(pending==count)return;
        sched_yield();
    }
}
static void state_readers_case(struct kernel_heap *heap)
{
    struct ext4_lock callbacks={0};struct boaros_lwext4_domain domain={0};
    struct jbd_journal journal={.grouped=true};struct ext4_fs fs={.jbd_journal=&journal};
    fs.orphan_index=(struct ext4_orphan_index *)&fs;
    boaros_lwext4_lock_init(&callbacks,&domain,heap);boaros_lwext4_enable_parallel(&domain,&fs);
    callbacks.lock(&domain);
    callbacks.state_lock(&domain);callbacks.state_read_lock(&domain);callbacks.state_lock(&domain);
    assert(kernel_io_context_current()->backend_state_depth==3 &&
           kernel_io_context_current()->backend_state_guard.write);
    for(unsigned i=0;i<3;i++)callbacks.state_unlock(&domain);
    callbacks.state_read_lock(&domain);callbacks.state_read_lock(&domain);
    assert(kernel_io_context_current()->backend_state_depth==2 &&
           !kernel_io_context_current()->backend_state_guard.write);
    callbacks.state_unlock(&domain);
    struct state_actor actors[3]={{.callbacks=&callbacks,.id=1},
        {.callbacks=&callbacks,.id=2,.write=1},{.callbacks=&callbacks,.id=3}};
    pthread_t threads[3];assert(!pthread_create(&threads[0],NULL,state_actor,&actors[0]));
    state_wait_entered(&actors[0]);
    {KERNEL_RAW_SCOPE(guard,&domain.state.lock.metadata);
     assert(domain.state.lock.readers==2 && !domain.state.lock.writer);}
    assert(!pthread_create(&threads[1],NULL,state_actor,&actors[1]));
    state_wait_queued(&domain.state.lock,1);
    assert(!pthread_create(&threads[2],NULL,state_actor,&actors[2]));
    state_wait_queued(&domain.state.lock,2);
    assert(!__atomic_load_n(&actors[1].entered,__ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&actors[2].entered,__ATOMIC_ACQUIRE));
    state_leave_actor(&actors[0],threads[0]);
    assert(!__atomic_load_n(&actors[1].entered,__ATOMIC_ACQUIRE));
    callbacks.state_unlock(&domain);state_wait_entered(&actors[1]);
    assert(!__atomic_load_n(&actors[2].entered,__ATOMIC_ACQUIRE));
    state_leave_actor(&actors[1],threads[1]);state_wait_entered(&actors[2]);
    state_leave_actor(&actors[2],threads[2]);
    callbacks.unlock(&domain);boaros_lwext4_lock_fini(&domain);state_returned();
    puts("PASS simultaneous state readers retain distinct guards; writer waits for both and queued late reader follows writer; nested read/write owners return");
}
static void state_rejected_case(struct kernel_heap *heap,unsigned failure)
{
    pid_t child=fork();assert(child>=0);
    if(!child) {
        struct rlimit limit={0};assert(!setrlimit(RLIMIT_CORE,&limit));
        struct ext4_lock callbacks={0},other_callbacks={0};
        struct boaros_lwext4_domain domain={0},other={0};
        boaros_lwext4_lock_init(&callbacks,&domain,heap);
        boaros_lwext4_lock_init(&other_callbacks,&other,heap);
        callbacks.lock(&domain);callbacks.state_read_lock(&domain);
        switch(failure) {
        case 0: callbacks.state_lock(&domain);break;
        case 1: callbacks.state_read_lock(&other);break;
        case 2: callbacks.state_unlock(&other);break;
        case 3:
            kernel_io_context_current()->backend_state_guard.owner=NULL;
            callbacks.state_unlock(&domain);break;
        case 4: callbacks.unlock(&domain);break;
        default:__builtin_trap();
        }
        _exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) &&
                     (WTERMSIG(status)==SIGILL || WTERMSIG(status)==SIGTRAP));
}
int main(void)
{
    mm_test_attach(0);
    struct ext4_lock callbacks={0};
    struct boaros_lwext4_domain domain={0};
    boaros_lwext4_lock_init(&callbacks,&domain,NULL);
    assert(callbacks.state_lock && callbacks.state_unlock && callbacks.resource_acquire &&
           callbacks.resource_release && callbacks.resource_retry);
    puts("PASS ext4 publication and resource callbacks are available");
    callbacks.lock(callbacks.context);
    assert(callbacks.serial_writer && callbacks.serial_writer(callbacks.context));
    callbacks.state_lock(callbacks.context);
    callbacks.state_lock(callbacks.context);
    assert(kernel_lock_held(&domain.serial,1) && kernel_lock_held(&domain.state.lock,1));
    callbacks.state_unlock(callbacks.context);
    assert(kernel_lock_held(&domain.state.lock,1));
    callbacks.state_unlock(callbacks.context);
    callbacks.unlock(callbacks.context);
    boaros_lwext4_lock_fini(&domain);
    assert(!kernel_io_context_current()->locks && !kernel_io_context_current()->backend_depth &&
           !kernel_io_context_current()->backend_domain && !kernel_io_context_current()->backend_state_depth);
    puts("PASS legacy call qualification and nested publication use separate ranks and return all guards");
    struct physical_page_allocator pages={0};struct kernel_heap heap={0};
    struct boot_memory_layout layout={.usable_count=1,.usable={{(uintptr_t)arena,sizeof(arena)}}};
    assert(!physical_page_allocator_init(&pages,&layout) &&
           !physical_page_allocator_bind_access(&pages,page_access) &&
           !physical_page_allocator_finalize(&pages) && !kernel_heap_init(&heap,&pages,page_address));
    uint64_t available=physical_page_available(&pages);
    resources_case(&heap);shared_resource_case(&heap,1);shared_resource_case(&heap,0);transition_case(&heap);
    resource_reclamation_case(&heap);last_resource_case(&heap);inode_lifetime_case(&heap);state_readers_case(&heap);
    for(unsigned failure=0;failure<5;failure++)state_rejected_case(&heap,failure);
    puts("PASS state read qualification rejects upgrade, wrong domain/owner and premature backend release");
    assert(physical_page_available(&pages)==available);
    return 0;
}
