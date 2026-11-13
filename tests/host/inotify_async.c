/* Real enqueue/merge/overflow and mark traversal, with only host boundaries. */
#include "../../fs/inotify.c"
#include "../../kernel/sched/private.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <kernel/time.h>

_Thread_local uintptr_t sync_test_irq=2;
static _Thread_local struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED};
static struct kernel_heap heap;
static struct kernel_uid_account account={.references=1};
static unsigned allocations,async_count,poll_count,hold,entered,resume,close_done;
void *sync_test_cpu(void){return &cpu;}
void *sync_test_current_task(void){return NULL;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
void kernel_assert_can_block(void){assert(!cpu.raw_locks);}
void kernel_preempt_disable(void){assert(!cpu.raw_locks);cpu.preempt_depth++;}
void kernel_preempt_enable(void){assert(!cpu.raw_locks&&cpu.preempt_depth);cpu.preempt_depth--;}
void kernel_heap_release(struct kernel_heap *h,void *p)
{assert(h==&heap&&p&&!cpu.raw_locks);__atomic_fetch_sub(&allocations,1,__ATOMIC_RELAXED);free(p);}
void kernel_uid_account_put(struct kernel_uid_account *a)
{assert(a==&account&&a->references>1);__atomic_fetch_sub(&a->references,1,__ATOMIC_RELAXED);}
struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *t){(void)t;abort();}
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *t){(void)t;abort();}
struct kernel_task *kernel_wait_current_task(void){return NULL;}
int kernel_wait_backend_initialized(void){return 1;}
void kernel_wait_backend_notify(struct kernel_task *t,uint32_t reason){(void)t;(void)reason;abort();}
void kernel_wait_backend_ready(struct kernel_task *t){(void)t;abort();}
void kernel_wait_backend_quiesce(void){sched_yield();}
void kernel_wait_backend_commit(struct kernel_task *t,uint64_t d,int i){(void)t;(void)d;(void)i;abort();}
enum kernel_scheduler_status kernel_wait_backend_switch_locked(struct kernel_task *t,struct kernel_raw_guard *g)
{(void)t;(void)g;abort();}
uint64_t kernel_wait_backend_time(void){return 0;}
int kernel_wait_backend_signal(struct kernel_task *t){(void)t;return 0;}
enum kernel_time_status kernel_time_deadline_from_realtime(uint64_t ns,uint64_t *deadline)
{*deadline=ns;return 0;}
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *mm,uint64_t to,const void *from,size_t size,size_t *copied)
{(void)mm;(void)to;(void)from;(void)size;(void)copied;abort();}
static void *allocate(size_t size)
{void *p=calloc(1,size);assert(p);__atomic_fetch_add(&allocations,1,__ATOMIC_RELAXED);return p;}
static struct inotify_group *new_group(void)
{
    struct inotify_group *g=allocate(sizeof(*g));g->heap=&heap;g->references=1;g->account=&account;
    account.inotify_groups++;account.references++;
    kernel_wait_queue_init(&g->queue);kernel_wait_queue_init(&g->async_events);return g;
}
static void new_mark(struct inotify_group *g,struct kernel_vfs_instance *instance,unsigned inode,unsigned mask)
{
    struct inotify_mark *m=allocate(sizeof(*m));memset(m,0xa5,sizeof(*m));m->heap=&heap;
    KERNEL_RAW_SCOPE(guard,&notify_lock);assert(watch_install(g,instance,inode,mask,m)>0);
}
static void notified(struct kernel_wait_node *node,uint32_t reason,uint64_t event)
{
    assert(!cpu.raw_locks&&reason==KERNEL_WAIT_WOKEN&&event==KERNEL_ASYNC_POLL_IN);
    __atomic_fetch_add(&async_count,1,__ATOMIC_RELAXED);
    if (node->context && __atomic_load_n(&hold,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&resume,__ATOMIC_ACQUIRE))sched_yield();
    }
}
static void polled(struct kernel_wait_node *node,uint32_t reason,uint64_t event)
{(void)node;assert(!cpu.raw_locks&&reason==KERNEL_WAIT_WOKEN&&!event);poll_count++;}
static void subscribe(struct inotify_group *g,struct kernel_wait_node *node,void *context)
{kernel_wait_node_init_callback(node,notified,context);kernel_wait_queue_add(&g->async_events,node);}
static void accepted_events(void)
{
    struct kernel_vfs_instance instance={0};struct inotify_group *g=new_group();
    struct kernel_wait_node node={0},poll={0};subscribe(g,&node,NULL);
    kernel_wait_node_init_callback(&poll,polled,NULL);kernel_wait_queue_add(&g->queue,&poll);
    new_mark(g,&instance,11,VFS_NOTIFY_ACCESS|VFS_NOTIFY_MODIFY);
    emit(&instance,11,VFS_NOTIFY_ACCESS,0,NULL,0);assert(async_count==1&&poll_count==1);
    emit(&instance,11,VFS_NOTIFY_ACCESS,0,NULL,0);assert(async_count==1&&poll_count==1);
    emit(&instance,11,VFS_NOTIFY_MODIFY,0,NULL,0);assert(async_count==2&&poll_count==2);
    for(unsigned cookie=1;!g->overflow;cookie++)emit(&instance,11,VFS_NOTIFY_MODIFY,cookie,NULL,0);
    unsigned count=async_count;
    emit(&instance,11,VFS_NOTIFY_ACCESS,0,NULL,0);assert(async_count==count);
    assert(!kernel_wait_node_remove_sync(&node)&&!kernel_wait_node_remove_sync(&poll));release_group(g);
    assert(!instance.notify_marks&&!account.inotify_marks&&!allocations);
    g=new_group();node=(struct kernel_wait_node){0};poll=(struct kernel_wait_node){0};subscribe(g,&node,NULL);
    kernel_wait_node_init_callback(&poll,polled,NULL);kernel_wait_queue_add(&g->queue,&poll);
    new_mark(g,&instance,12,VFS_NOTIFY_ACCESS|IN_ONESHOT);
    count=async_count;unsigned polls=poll_count;
    emit(&instance,12,VFS_NOTIFY_ACCESS,0,NULL,0);
    assert(async_count==count+2&&poll_count==polls+1&&!g->marks&&!instance.notify_marks);
    assert(!kernel_wait_node_remove_sync(&node)&&!kernel_wait_node_remove_sync(&poll));release_group(g);
    assert(!allocations&&account.references==1&&!account.inotify_groups);
}
struct close_args {struct inotify_group *b,*c;struct kernel_wait_node *bn,*cn;};
static void *close_successors(void *p)
{
    struct close_args *a=p;
    assert(!kernel_wait_node_remove_sync(a->bn));release_group(a->b);
    assert(!kernel_wait_node_remove_sync(a->cn));release_group(a->c);
    __atomic_store_n(&close_done,1,__ATOMIC_RELEASE);return NULL;
}
static void *emit_first(void *p){emit(p,21,VFS_NOTIFY_ACCESS,0,NULL,0);return NULL;}
static void concurrent_retirement(void)
{
    struct kernel_vfs_instance instance={0};struct inotify_group *a=new_group(),*b=new_group(),*c=new_group();
    struct kernel_wait_node an={0},bn={0},cn={0};subscribe(a,&an,(void *)(uintptr_t)1);subscribe(b,&bn,NULL);subscribe(c,&cn,NULL);
    new_mark(c,&instance,21,VFS_NOTIFY_ACCESS);new_mark(b,&instance,21,VFS_NOTIFY_ACCESS);new_mark(a,&instance,21,VFS_NOTIFY_ACCESS);
    unsigned count=async_count;hold=1;pthread_t emitting,closing;
    assert(!pthread_create(&emitting,NULL,emit_first,&instance));
    while(!__atomic_load_n(&entered,__ATOMIC_ACQUIRE))sched_yield();
    struct close_args args={b,c,&bn,&cn};assert(!pthread_create(&closing,NULL,close_successors,&args));
    while(!__atomic_load_n(&close_done,__ATOMIC_ACQUIRE))sched_yield();
    assert(instance.notify_marks==1&&account.inotify_groups==3&&account.inotify_marks==1);
    __atomic_store_n(&resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(emitting,NULL)&&!pthread_join(closing,NULL));
    assert(async_count==count+1&&account.inotify_groups==1&&account.inotify_marks==1);
    assert(!kernel_wait_node_remove_sync(&an));release_group(a);
    assert(!allocations&&account.references==1&&!account.inotify_groups&&!account.inotify_marks&&!instance.notify_marks);
}
int main(void)
{
    kernel_wait_domain_init();accepted_events();concurrent_retirement();
    puts("PASS inotify async: accepted/merged/overflow/ignored cadence, poll dedup, concurrent successor/group retirement");
}
