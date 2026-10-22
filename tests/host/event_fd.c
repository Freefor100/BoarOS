/* Real counter/index/expiry code. Machine, FD table and usercopy boundaries
 * are modeled; pthread handshakes keep expiry notification alive during close. */
#define BOAROS_HOST_IRQ_STATE 1
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../fs/event_fd.c"
_Thread_local uintptr_t network_host_irq=RISCV_SSTATUS_SIE;
static uint64_t now_ns=1000000000;
static unsigned allocations,notifications,arms;
static size_t copy_limit=SIZE_MAX;
static struct kernel_open_file_description *slots[256];
static pthread_barrier_t callback_enter,callback_leave;
static struct kernel_wait_queue *paused;
static unsigned released;
static void barrier(pthread_barrier_t *b){int r=pthread_barrier_wait(b);assert(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);}
void kernel_cpu_flush_notifications(void){assert(!kernel_cpu_current()->outbound);}
void kernel_cpu_notify(struct kernel_cpu *c,unsigned e){(void)c;(void)e;arms++;}
struct kernel_cpu *riscv_smp_cpu(unsigned n){(void)n;return kernel_cpu_current();}
void kernel_scheduler_deadline_changed(void){assert(!kernel_cpu_current()->raw_locks);arms++;}
enum kernel_scheduler_status kernel_scheduler_yield_current(void){sched_yield();return 0;}
uint64_t riscv_time_read(void){return __atomic_load_n(&now_ns,__ATOMIC_RELAXED);}
uint64_t kernel_time_monotonic_ns(void){return __atomic_load_n(&now_ns,__ATOMIC_RELAXED);}
uint64_t kernel_time_realtime_ns(void){return kernel_time_monotonic_ns()+10000000000;}
uint64_t kernel_time_boot_realtime_offset(void){return 10000000000;}
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t n,uint64_t *t){*t=n;return n<=now_ns?KERNEL_TIME_STATUS_DEADLINE_PASSED:KERNEL_TIME_STATUS_OK;}
int kernel_files_is_live(const struct kernel_files *f){return f!=0;}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t z,void **out){(void)h;assert(!kernel_cpu_current()->raw_locks);*out=calloc(n,z);assert(*out);allocations++;return 0;}
void kernel_heap_release(struct kernel_heap *h,void *p){(void)h;assert(p&&allocations);allocations--;free(p);}
void kernel_rwlock_init(struct kernel_rwlock *m,uint32_t rank,uintptr_t key){(void)m;(void)rank;(void)key;}
uint32_t kernel_open_file_flags(const struct kernel_open_file_description *f){return f->open_flags;}
enum kernel_files_status kernel_files_find_free_fd(struct kernel_files *f,uint32_t *fd,int *error){(void)f;for(unsigned i=0;i<256;i++)if(!slots[i]){*fd=i;*error=0;return 0;}*error=-KERNEL_EMFILE;return 0;}
void kernel_files_cancel_reservation(struct kernel_files_fd_reservation *r){(void)r;}
enum kernel_files_status kernel_files_install_new_owned_at(struct kernel_files *f,uint32_t fd,uint32_t flags,struct kernel_open_file_description **o){(void)f;(void)flags;slots[fd]=*o;*o=0;return 0;}
struct kernel_open_file_description *kernel_files_hold_fd(struct kernel_files *f,int64_t fd,struct kernel_files_pin_guard *g){(void)f;g->description=fd>=0&&fd<256?slots[fd]:0;return g->description;}
void kernel_files_pin_guard_release(struct kernel_files_pin_guard *g){g->description=0;}
enum kernel_open_file_status kernel_open_file_release(struct kernel_open_file_description **o){kernel_event_fd_close(*o);kernel_heap_release((*o)->heap,*o);*o=0;return 0;}
void kernel_wait_queue_init(struct kernel_wait_queue *q){memset(q,0,sizeof(*q));q->initialized=1;}
int kernel_wait_cursor_prepare(struct kernel_wait_queue *q,struct kernel_wait_cursor *c){c->queue=q;__atomic_fetch_add(&q->borrows,1,__ATOMIC_RELAXED);return 1;}
void kernel_wait_cursor_wake(struct kernel_wait_cursor *c,int all){assert(!kernel_cpu_current()->raw_locks&&all);if(c->queue==paused){barrier(&callback_enter);barrier(&callback_leave);}__atomic_fetch_sub(&c->queue->borrows,1,__ATOMIC_RELAXED);notifications++;}
enum kernel_scheduler_status kernel_wait_queue_close(struct kernel_wait_queue *q){__atomic_store_n(&q->closed,1,__ATOMIC_RELEASE);return 0;}
enum kernel_scheduler_status kernel_wait_queue_destroy(struct kernel_wait_queue *q){return __atomic_load_n(&q->borrows,__ATOMIC_ACQUIRE)?KERNEL_SCHEDULER_STATUS_BUSY:KERNEL_SCHEDULER_STATUS_OK;}
static void (*on_park)(void);
static unsigned prepared;
enum kernel_scheduler_status kernel_wait_prepare(struct kernel_wait_queue *q,uint64_t d,int i,struct kernel_wait_token *t){(void)q;(void)d;(void)i;assert(!prepared);prepared=1;t->generation=1;return 0;}
enum kernel_scheduler_status kernel_wait_park(const struct kernel_wait_token *t,enum kernel_wait_wake_reason *r){(void)t;assert(prepared&&on_park&&!kernel_cpu_current()->raw_locks);on_park();*r=KERNEL_WAIT_WOKEN;return 0;}
enum kernel_scheduler_status kernel_wait_finish(struct kernel_wait_token *t){(void)t;assert(prepared);prepared=0;return 0;}
struct kernel_task *kernel_task_current(void){return (void *)&prepared;}
void kernel_signal_note_syscall_restart(struct kernel_task *t){(void)t;}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *m,void *d,uint64_t s,size_t n,size_t *c){(void)m;assert(!kernel_cpu_current()->raw_locks);*c=n<copy_limit?n:copy_limit;memcpy(d,(void *)(uintptr_t)s,*c);return *c==n?0:KERNEL_UACCESS_STATUS_FAULT;}
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *m,uint64_t d,const void *s,size_t n,size_t *c){return kernel_copy_from_user(m,(void *)(uintptr_t)d,(uintptr_t)s,n,c);}
static struct kernel_heap heap;
static struct kernel_files files={.heap=&heap};
static struct kernel_event_fd *make(unsigned kind)
{struct kernel_event_fd *e=0;assert(!allocate_event(&files,kind==KERNEL_OPEN_FILE_KIND_TIMERFD?sizeof(struct kernel_timer_fd):sizeof(struct kernel_counter_fd),kind,0,&e));return e;}
static struct kernel_event_fd *blocked;
static void satisfy(void){uint64_t n=7;assert(!event_transfer(blocked,&n,1,0));}
static void counters(void)
{
 struct kernel_event_fd *e=make(KERNEL_OPEN_FILE_KIND_EVENTFD);uint64_t n=0;
 assert(event_transfer(e,&n,0,EVENT_NONBLOCK)==-KERNEL_EAGAIN);
 blocked=e;on_park=satisfy;assert(!event_transfer(e,&n,0,0)&&n==7&&!prepared);
 e->count=UINT64_MAX-1;n=1;assert(event_transfer(e,&n,1,EVENT_NONBLOCK)==-KERNEL_EAGAIN);
 struct kernel_uaccess_iovec iov={(uintptr_t)&n,8};copy_limit=3;
 assert(kernel_event_fd_io(&e->file,0,&iov,1,8,0)==-KERNEL_EFAULT&&!e->count);
 copy_limit=SIZE_MAX;struct kernel_open_file_description *o=&e->file;assert(!kernel_open_file_release(&o));puts("PASS event counter registration/fault");
}
static int check_tree(struct kernel_timer_fd *t,uint64_t *last,unsigned *count)
{
 if(!t)return 0;
 int l=check_tree(t->left,last,count);assert(!*count||*last<=t->tick);*last=t->tick;(*count)++;
 int r=check_tree(t->right,last,count);assert(abs(l-r)<=1&&t->height==1+(l>r?l:r)&&t->indexed);return t->height;
}
static void index_cases(void)
{
 struct kernel_timer_fd nodes[97]={0};
 for(unsigned round=0;round<2;round++){
  for(unsigned i=0;i<97;i++){unsigned j=(i*31)%97;nodes[j].clock=1;nodes[j].expires=now_ns+1+(j%11);timer_insert(&nodes[j]);}
  for(unsigned i=0;i<97;i++){uint64_t last=0;unsigned count=0;check_tree(timer_root,&last,&count);assert(count==97-i);timer_remove(&nodes[(i*37)%97]);timer_publish();}
  assert(!timer_root&&!kernel_timer_fd_next);
 }puts("PASS timer AVL overlap/removal");
}
static void *expire_thread(void *p){(void)p;kernel_timer_fd_expire(now_ns);return 0;}
static void *close_thread(void *p){struct kernel_open_file_description *o=p;assert(!kernel_open_file_release(&o));__atomic_store_n(&released,1,__ATOMIC_RELEASE);return 0;}
static void lifecycle(void)
{
 struct kernel_event_fd *e=make(KERNEL_OPEN_FILE_KIND_TIMERFD);struct kernel_timer_fd *t=(void *)e;
 t->clock=1;t->expires=now_ns+10;timer_insert(t);timer_publish();now_ns+=10;
 paused=&e->waiters;assert(!pthread_barrier_init(&callback_enter,0,2)&&!pthread_barrier_init(&callback_leave,0,2));pthread_t expire,close;
 assert(!pthread_create(&expire,0,expire_thread,0));barrier(&callback_enter);
 assert(!pthread_create(&close,0,close_thread,&e->file));while(!__atomic_load_n(&e->waiters.closed,__ATOMIC_ACQUIRE))sched_yield();
 assert(!__atomic_load_n(&released,__ATOMIC_ACQUIRE)&&!kernel_timer_fd_deadline());barrier(&callback_leave);
 assert(!pthread_join(expire,0)&&!pthread_join(close,0)&&released);paused=0;
 pthread_barrier_destroy(&callback_enter);pthread_barrier_destroy(&callback_leave);puts("PASS timer expiry borrow versus final close");
}
int main(void){counters();index_cases();lifecycle();assert(!allocations&&!timer_root&&!prepared);printf("EVENT HOST PASS page=%lu\n",BOAROS_PAGE_SIZE);return 0;}
