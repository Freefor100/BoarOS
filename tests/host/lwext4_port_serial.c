#include "../../fs/lwext4_port.h"
#include <ext4.h>
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <kernel/cpu.h>
#include <kernel/wait_internal.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>

void mm_test_attach(unsigned);
void kernel_console_putc(char byte) { fputc(byte,stderr); }
/* pthread actors model preemption on one CPU. Serialize the production raw
 * regions, whose UP implementation correctly has no cross-CPU atomic word. */
static pthread_mutex_t cpu_execution=PTHREAD_MUTEX_INITIALIZER;
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *,struct kernel_raw_guard *);
void __real_kernel_raw_lock_release(struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock,struct kernel_raw_guard *guard)
{
    if(!kernel_cpu_current()->raw_locks)assert(!pthread_mutex_lock(&cpu_execution));
    __real_kernel_raw_lock_acquire(lock,guard);
}
void __wrap_kernel_raw_lock_release(struct kernel_raw_guard *guard)
{
    __real_kernel_raw_lock_release(guard);
    if(!kernel_cpu_current()->raw_locks)assert(!pthread_mutex_unlock(&cpu_execution));
}
static unsigned char arena[2*1024*1024] __attribute__((aligned(16384)));
static void *page_access(uint64_t address) { return (void *)(uintptr_t)address; }
static int page_address(const void *page,uint64_t *address) { *address=(uintptr_t)page;return 1; }
static void returned(void)
{
    struct kernel_io_context *context=kernel_io_context_current();
    assert(!context->locks && !context->backend_depth && !context->backend_domain &&
           !context->backend_state_depth && !context->backend_state_guard.lock &&
           !kernel_cpu_current()->raw_locks && !kernel_cpu_current()->preempt_depth);
}
static void resource_callbacks_absent(struct kernel_heap *heap)
{
    struct ext4_lock callbacks={0};struct boaros_lwext4_domain domain={0};
    struct jbd_journal journal={0};
    struct ext4_fs fs={.jbd_journal=&journal,.os_locks=&callbacks};
    boaros_lwext4_lock_init(&callbacks,&domain,heap);
    assert(!callbacks.resource_acquire && !callbacks.resource_claim_inode &&
           !callbacks.resource_release && !callbacks.resource_retry);
    assert(callbacks.serial_writer && !callbacks.serial_writer(&domain));
    callbacks.lock(&domain);
    assert(callbacks.serial_writer(&domain));
    /* Serial qualification is the actual exclusion. These helpers need no
     * operation lookup or per-resource owner, including before journal use. */
    for(unsigned journaled=0;journaled<2;journaled++) {
        fs.jbd_journal=journaled?&journal:NULL;
        assert(!ext4_operation_require_resource(&fs,EXT4_RESOURCE_INODE,17) &&
               !ext4_operation_require_resource(&fs,EXT4_RESOURCE_BLOCK_GROUP,2) &&
               !ext4_operation_claim_inode(&fs,18));
        ext4_operation_release_resources(&fs);
        assert(!ext4_operation_resource_retry(&fs));
        assert(kernel_lock_held(&domain.serial,1) && !domain.resources.rbh_root &&
               !kernel_io_context_current()->backend_resources.request_count);
    }
    callbacks.unlock(&domain);boaros_lwext4_lock_fini(&domain);returned();
    puts("PASS actual one-CPU mode installs no resource callbacks; journal/nonjournal resource helpers retain the serial qualification and create no resource owners");
}
static void queued(struct kernel_rwlock *lock,unsigned count)
{
    for(;;) {
        unsigned registrations;
        { KERNEL_RAW_SCOPE(guard,&lock->metadata);registrations=lock->waiters.registrations; }
        if(registrations==count)return;
        sched_yield();
    }
}
struct state_actor {
    struct boaros_lwext4_domain *domain;
    struct ext4_lock *callbacks;
    unsigned id,reader,parallel,state_read,entered,leave;
};
static void *state_actor(void *argument)
{
    struct state_actor *actor=argument;
    mm_test_attach(actor->id);
    struct kernel_rwlock inode_io;struct kernel_lock_guard inode_guard={0};
    kernel_rwlock_init(&inode_io,30,actor->id);
    kernel_rwlock_write(&inode_io,&inode_guard);
    if(actor->reader)actor->callbacks->read_lock(actor->domain);
    if(actor->state_read)actor->callbacks->state_read_lock(actor->domain);
    else boaros_lwext4_state_lock(actor->domain);
    assert(!actor->callbacks->serial_writer(actor->domain));
    assert(kernel_io_context_current()->backend_state_guard.lock==
        (actor->reader || actor->parallel?&actor->domain->state.lock:&actor->domain->serial));
    __atomic_store_n(&actor->entered,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&actor->leave,__ATOMIC_ACQUIRE))sched_yield();
    boaros_lwext4_state_unlock(actor->domain);
    if(actor->reader)actor->callbacks->unlock(actor->domain);
    kernel_lock_release(&inode_guard);
    returned();return NULL;
}
static void wait_entered(struct state_actor *actor)
{ while(!__atomic_load_n(&actor->entered,__ATOMIC_ACQUIRE))sched_yield(); }
static void leave_actor(struct state_actor *actor,pthread_t thread)
{
    __atomic_store_n(&actor->leave,1,__ATOMIC_RELEASE);
    assert(!pthread_join(thread,NULL));
}
static void borrowed_writer(struct kernel_heap *heap)
{
    struct ext4_lock callbacks={0};struct boaros_lwext4_domain domain={0};
    boaros_lwext4_lock_init(&callbacks,&domain,heap);
    callbacks.lock(&domain);
    struct kernel_lock_guard *writer=kernel_io_context_current()->locks;
    callbacks.state_lock(&domain);callbacks.state_read_lock(&domain);callbacks.state_lock(&domain);
    assert(kernel_io_context_current()->locks==writer &&
           !kernel_io_context_current()->backend_state_guard.lock &&
           kernel_io_context_current()->backend_state_depth==3);
    /* Existing state scopes allocate; borrowing the sleeping qualification
     * must not disable preemption or turn valid allocation into a trap. */
    void *allocation=NULL;
    assert(!kernel_heap_allocate_zeroed(heap,1,65536,&allocation));
    kernel_heap_release(heap,allocation);
    struct state_actor actor={.domain=&domain,.callbacks=&callbacks,.id=1};
    pthread_t thread;assert(!pthread_create(&thread,NULL,state_actor,&actor));
    queued(&domain.serial,1);
    assert(!__atomic_load_n(&actor.entered,__ATOMIC_ACQUIRE));
    callbacks.state_unlock(&domain);callbacks.state_unlock(&domain);callbacks.state_unlock(&domain);
    assert(callbacks.serial_writer(&domain));
    assert(kernel_io_context_current()->locks==writer);
    callbacks.unlock(&domain);
    wait_entered(&actor);leave_actor(&actor,thread);
    boaros_lwext4_lock_fini(&domain);returned();
    puts("PASS serial-qualified state borrows the actual backend writer, permits allocation, and excludes external inode-IO state callers");
}
static void readers_and_external(struct kernel_heap *heap)
{
    struct ext4_lock callbacks={0};struct boaros_lwext4_domain domain={0};
    boaros_lwext4_lock_init(&callbacks,&domain,heap);
    callbacks.read_lock(&domain);callbacks.state_lock(&domain);
    assert(kernel_io_context_current()->backend_state_guard.lock==&domain.state.lock);
    struct state_actor reader={.domain=&domain,.callbacks=&callbacks,.id=2,.reader=1};
    pthread_t thread;assert(!pthread_create(&thread,NULL,state_actor,&reader));
    queued(&domain.state.lock,1);
    assert(!__atomic_load_n(&reader.entered,__ATOMIC_ACQUIRE));
    callbacks.state_unlock(&domain);
    wait_entered(&reader);leave_actor(&reader,thread);
    callbacks.unlock(&domain);
    /* The external holder also admits same-domain recursion, without
     * manufacturing a backend owner or a second lock guard. */
    callbacks.state_lock(&domain);callbacks.state_lock(&domain);
    assert(!kernel_io_context_current()->backend_depth &&
           kernel_io_context_current()->backend_state_guard.lock==&domain.serial);
    callbacks.state_unlock(&domain);callbacks.state_unlock(&domain);
    boaros_lwext4_lock_fini(&domain);returned();
    puts("PASS serial-qualified backend readers retain state mutex exclusion; external nested state uses the serial writer");
}
static void shared_state_readers(struct kernel_heap *heap)
{
    struct ext4_lock callbacks={0};struct boaros_lwext4_domain domain={0};
    boaros_lwext4_lock_init(&callbacks,&domain,heap);
    callbacks.read_lock(&domain);callbacks.state_read_lock(&domain);callbacks.state_read_lock(&domain);
    assert(kernel_io_context_current()->backend_state_depth==2 &&
           kernel_io_context_current()->backend_state_guard.lock==&domain.state.lock &&
           !kernel_io_context_current()->backend_state_guard.write);
    struct state_actor reader={.domain=&domain,.callbacks=&callbacks,.id=2,.reader=1,.state_read=1};
    pthread_t thread;assert(!pthread_create(&thread,NULL,state_actor,&reader));
    wait_entered(&reader);
    {KERNEL_RAW_SCOPE(guard,&domain.state.lock.metadata);assert(domain.state.lock.readers==2);}
    leave_actor(&reader,thread);
    callbacks.state_unlock(&domain);callbacks.state_unlock(&domain);callbacks.unlock(&domain);
    /* External state still excludes the actual serial writer, including when
     * its first nested request is read-only. No mode transition is guessed. */
    callbacks.state_read_lock(&domain);callbacks.state_lock(&domain);
    assert(kernel_io_context_current()->backend_state_guard.lock==&domain.serial &&
           kernel_io_context_current()->backend_state_guard.write);
    callbacks.state_unlock(&domain);callbacks.state_unlock(&domain);
    boaros_lwext4_lock_fini(&domain);returned();
    puts("PASS serial-qualified backend state readers coexist; read queries borrow the actual serial writer and external nested state retains its exclusion");
}
struct transition_actor { struct boaros_lwext4_domain *domain;struct ext4_fs *fs; };
static void *transition_actor(void *argument)
{
    struct transition_actor *actor=argument;mm_test_attach(3);
    boaros_lwext4_enable_parallel(actor->domain,actor->fs);returned();return NULL;
}
static void transition_recheck(struct kernel_heap *heap)
{
    struct ext4_lock callbacks={0};struct boaros_lwext4_domain domain={0};
    struct jbd_journal journal={.grouped=true};struct ext4_fs fs={.jbd_journal=&journal};
    fs.orphan_index=(struct ext4_orphan_index *)&fs;
    boaros_lwext4_lock_init(&callbacks,&domain,heap);callbacks.lock(&domain);
    struct transition_actor transition={&domain,&fs};pthread_t publisher,waiter;
    assert(!pthread_create(&publisher,NULL,transition_actor,&transition));
    queued(&domain.serial,1);
    struct state_actor external={.domain=&domain,.callbacks=&callbacks,.id=4,.parallel=1};
    assert(!pthread_create(&waiter,NULL,state_actor,&external));queued(&domain.serial,2);
    callbacks.unlock(&domain);assert(!pthread_join(publisher,NULL));
    wait_entered(&external);leave_actor(&external,waiter);
    boaros_lwext4_lock_fini(&domain);returned();
    puts("PASS serial-qualified external state queued before parallel publication rechecks mode and acquires the real state mutex");
}
static void rejected_ownership(struct kernel_heap *heap,unsigned failure)
{
    pid_t child=fork();assert(child>=0);
    if(!child) {
        struct rlimit limit={0};assert(!setrlimit(RLIMIT_CORE,&limit));
        struct ext4_lock callbacks={0},other_callbacks={0};
        struct boaros_lwext4_domain domain={0},other={0};
        boaros_lwext4_lock_init(&callbacks,&domain,heap);
        boaros_lwext4_lock_init(&other_callbacks,&other,heap);
        if(failure==8) {
            callbacks.read_lock(&domain);callbacks.state_read_lock(&domain);
            callbacks.state_lock(&domain);_exit(0);
        }
        if(failure>=6) {
            struct kernel_mutex inner;struct kernel_lock_guard inner_guard={0};
            callbacks.state_lock(&domain);
            if(failure==6)kernel_io_context_current()->backend_state_guard.owner=NULL;
            else {
                kernel_mutex_init(&inner,40,(uintptr_t)&inner);
                kernel_mutex_lock(&inner,&inner_guard);
            }
            callbacks.state_unlock(&domain);
            _exit(0);
        }
        callbacks.lock(&domain);callbacks.state_lock(&domain);
        switch(failure) {
        case 0: callbacks.state_lock(&other);break;
        case 1: callbacks.state_unlock(&other);break;
        case 2: callbacks.unlock(&domain);break;
        case 3: boaros_lwext4_lock_fini(&domain);break;
        case 4: (void)boaros_lwext4_pause();break;
        case 5:
            kernel_io_context_current()->backend_guard.owner=NULL;
            callbacks.state_unlock(&domain);
            callbacks.unlock(&domain);
            break;
        default: __builtin_trap();
        }
        _exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) &&
                     (WTERMSIG(status)==SIGILL || WTERMSIG(status)==SIGTRAP));
}
extern unsigned mm_test_boot_cpus;
static void rejected_boot_configuration(unsigned failure)
{
    pid_t child=fork();assert(child>=0);
    if(!child) {
        struct rlimit limit={0};assert(!setrlimit(RLIMIT_CORE,&limit));
        if(!failure)(void)kernel_cpu_get_boot_count();
        else kernel_cpu_set_boot_count(failure==1?0:failure==2?3:mm_test_boot_cpus);
        _exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) &&
                     (WTERMSIG(status)==SIGILL || WTERMSIG(status)==SIGTRAP));
}
int main(int argc,char **argv)
{
    if(argc==2)mm_test_boot_cpus=strtoul(argv[1],NULL,10);
#if BOAROS_MAX_CPUS > 1
    rejected_boot_configuration(0);
#endif
    rejected_boot_configuration(1);rejected_boot_configuration(2);
    mm_test_attach(0);
    assert(kernel_cpu_get_boot_count()==mm_test_boot_cpus);
#if BOAROS_MAX_CPUS > 1
    rejected_boot_configuration(3);
#endif
    struct physical_page_allocator pages={0};struct kernel_heap heap={0};
    struct boot_memory_layout layout={.usable_count=1,.usable={{(uintptr_t)arena,sizeof(arena)}}};
    assert(!physical_page_allocator_init(&pages,&layout) &&
           !physical_page_allocator_bind_access(&pages,page_access) &&
           !physical_page_allocator_finalize(&pages) && !kernel_heap_init(&heap,&pages,page_address));
    struct kernel_heap_statistics before,after;kernel_heap_get_statistics(&heap,&before);
    if(mm_test_boot_cpus==1)resource_callbacks_absent(&heap);
    borrowed_writer(&heap);readers_and_external(&heap);shared_state_readers(&heap);
    if(mm_test_boot_cpus>1)transition_recheck(&heap);
    for(unsigned failure=0;failure<9;failure++)rejected_ownership(&heap,failure);
    kernel_heap_get_statistics(&heap,&after);
    assert(before.live_allocations==after.live_allocations && before.current_pages==after.current_pages);
    puts("PASS serial-qualified state rejects cross-domain nesting/unlock, premature backend release/pause/destruction and wrong final guard owner/LIFO; all allocations return");
#if BOAROS_MAX_CPUS > 1
    puts("PASS unpublished topology and boot-count republication rejected");
#endif
    printf("PASS boot topology MAX=%u actual=%u; invalid counts rejected\n",BOAROS_MAX_CPUS,mm_test_boot_cpus);
    return 0;
}
