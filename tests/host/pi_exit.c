/* Real PI exit, bucket mutex and wait-token code; hosted CPU switches only. */
#include "../../kernel/sched/futex_internal.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
extern _Thread_local uintptr_t sync_test_irq;
void mm_test_attach(unsigned);
unsigned mm_test_parked(unsigned);
static struct kernel_task owner,next;
static struct futex_bucket bucket;
static struct kernel_futex_pi_state state;
static unsigned begun,done,owner_refs,next_refs;
void kernel_console_putc(char c){fputc(c,stderr);}
void mm_test_fence(uint64_t address,int instructions){(void)address;(void)instructions;}
void mm_test_unavailable(void){abort();}
void scheduler_task_get_locked(struct kernel_task *t)
{assert(t==&owner||t==&next);(*(t==&owner?&owner_refs:&next_refs))++;}
void scheduler_task_put(struct kernel_task *t)
{KERNEL_RAW_SCOPE(g,&kernel_wait_domain);unsigned *r=t==&owner?&owner_refs:&next_refs;assert((t==&owner||t==&next)&&*r);--*r;}
void scheduler_pi_priority_locked(struct kernel_task *t,unsigned base,unsigned inherited)
{assert(!base&&!inherited&&!t->scheduling.priority);}
/* This fixture retains the state in caller storage; releasing it is a defect. */
void kernel_heap_release(struct kernel_heap *h,void *p){(void)h;(void)p;abort();}
int kernel_vfs_node_release(struct kernel_vfs_node **p){(void)p;abort();}
void kernel_memory_object_release(struct kernel_memory_object **p){(void)p;abort();}
void kernel_mm_unpin_user_page(struct kernel_mm_user_pin *p){(void)p;abort();}
static void *depart(void *unused)
{
    (void)unused;mm_test_attach(1);sync_test_irq=0;
    __atomic_store_n(&begun,1,__ATOMIC_RELEASE);
    kernel_futex_pi_cleanup_end(&owner);
    __atomic_store_n(&done,1,__ATOMIC_RELEASE);return 0;
}
int main(void)
{
    mm_test_attach(0);sync_test_irq=0;kernel_mutex_init(&bucket.operation,26,1);
    owner.pi_owned=&state;owner.futex_exit_state=1;owner_refs=1;
    state=(struct kernel_futex_pi_state){.bucket=&bucket,.owner=&owner,.references=1,.attached=1};bucket.pi_states=&state;
    KERNEL_LOCK_SCOPE(publisher);kernel_mutex_lock(&bucket.operation,&publisher);
    pthread_t thread;unsigned before=mm_test_parked(1);assert(!pthread_create(&thread,0,depart,0));
    while(!__atomic_load_n(&begun,__ATOMIC_ACQUIRE))sched_yield();
    while(mm_test_parked(1)==before&&!__atomic_load_n(&done,__ATOMIC_ACQUIRE))sched_yield();
    assert(!__atomic_load_n(&done,__ATOMIC_ACQUIRE));
    struct kernel_futex_pi_request waiter={.task=&next,.state=&state,.queued=1,.order=1};
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
     assert(state.attached&&state.owner==&owner&&bucket.pi_states==&state);
     next.pi_request=&waiter;next.wait.generation=123;next.wait.phase=KERNEL_WAIT_PREPARED;
     waiter.token=(struct kernel_wait_token){&next,123};state.head=state.tail=&waiter;state.references++;}
    kernel_lock_release(&publisher);assert(!pthread_join(thread,0));
    assert(state.owner==&next&&state.attached&&!owner.pi_owned&&next.pi_owned==&state);
    assert(waiter.granted==2&&!waiter.queued&&!owner_refs&&next_refs==1&&state.references==2);
    kernel_futex_pi_cleanup_end(&next);
    assert(!bucket.pi_states&&!state.attached&&!state.owner&&!next_refs);
    next.wait.phase=KERNEL_WAIT_FINISHED;kernel_futex_pi_abort(&next);
    assert(!next.pi_request&&state.references==1);
    puts("PI owner exit waits for actual bucket publisher; subsequent waiter receives owner and references balance PASS");
}
