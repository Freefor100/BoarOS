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
static uint64_t wall_offset=10000000000;
static unsigned allocations,notifications,arms;
static size_t copy_limit=SIZE_MAX;
static struct kernel_open_file_description *slots[256];
static pthread_barrier_t callback_enter,callback_leave;
static struct kernel_wait_queue *paused;
static unsigned released;
static struct kernel_credentials credentials;
static unsigned credential_references;
const struct kernel_credentials *kernel_credentials_current(void){return &credentials;}
const struct kernel_credentials *kernel_credentials_get(const struct kernel_credentials *c)
{assert(c==&credentials);credential_references++;return c;}
void kernel_credentials_put(const struct kernel_credentials *c)
{assert(c==&credentials&&credential_references);credential_references--;}
static void barrier(pthread_barrier_t *b){int r=pthread_barrier_wait(b);assert(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);}
void kernel_cpu_flush_notifications(void){assert(!kernel_cpu_current()->outbound);}
void kernel_cpu_notify(struct kernel_cpu *c,unsigned e){(void)c;(void)e;arms++;}
struct kernel_cpu *riscv_smp_cpu(unsigned n){(void)n;return kernel_cpu_current();}
void kernel_scheduler_deadline_changed(void){assert(!kernel_cpu_current()->raw_locks);arms++;}
enum kernel_scheduler_status kernel_scheduler_yield_current(void){sched_yield();return 0;}
uint64_t riscv_time_read(void){return __atomic_load_n(&now_ns,__ATOMIC_RELAXED);}
uint64_t kernel_time_monotonic_ns(void){return __atomic_load_n(&now_ns,__ATOMIC_RELAXED);}
uint64_t kernel_time_realtime_ns(void){return kernel_time_monotonic_ns()+wall_offset;}
uint64_t kernel_time_boot_realtime_offset(void){return wall_offset;}
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t n,uint64_t *t){*t=n;return n<=now_ns?KERNEL_TIME_STATUS_DEADLINE_PASSED:KERNEL_TIME_STATUS_OK;}
enum kernel_time_status kernel_time_deadline_from_realtime(uint64_t n,uint64_t *t)
{if(n<=kernel_time_realtime_ns()){*t=now_ns+1;return KERNEL_TIME_STATUS_DEADLINE_PASSED;}*t=n-wall_offset;return 0;}
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
enum kernel_open_file_status kernel_open_file_release(struct kernel_open_file_description **o){kernel_event_fd_close(*o);kernel_credentials_put((*o)->credentials);kernel_heap_release((*o)->heap,*o);*o=0;return 0;}
void kernel_wait_queue_init(struct kernel_wait_queue *q){memset(q,0,sizeof(*q));q->initialized=1;}
int kernel_wait_cursor_prepare(struct kernel_wait_queue *q,struct kernel_wait_cursor *c){c->queue=q;__atomic_fetch_add(&q->borrows,1,__ATOMIC_RELAXED);return 1;}
void kernel_wait_cursor_wake(struct kernel_wait_cursor *c,int all){assert(!kernel_cpu_current()->raw_locks&&all);if(c->queue==paused){barrier(&callback_enter);barrier(&callback_leave);}__atomic_fetch_sub(&c->queue->borrows,1,__ATOMIC_RELAXED);notifications++;}
enum kernel_scheduler_status kernel_wait_queue_close(struct kernel_wait_queue *q){__atomic_store_n(&q->closed,1,__ATOMIC_RELEASE);return 0;}
enum kernel_scheduler_status kernel_wait_queue_destroy(struct kernel_wait_queue *q){return __atomic_load_n(&q->borrows,__ATOMIC_ACQUIRE)?KERNEL_SCHEDULER_STATUS_BUSY:KERNEL_SCHEDULER_STATUS_OK;}
static void (*on_park)(void);
static enum kernel_wait_wake_reason park_reason=KERNEL_WAIT_WOKEN;
static unsigned prepared;
enum kernel_scheduler_status kernel_wait_prepare(struct kernel_wait_queue *q,uint64_t d,int i,struct kernel_wait_token *t){(void)q;(void)d;(void)i;assert(!prepared);prepared=1;t->generation=1;return 0;}
enum kernel_scheduler_status kernel_wait_park(const struct kernel_wait_token *t,enum kernel_wait_wake_reason *r){(void)t;assert(prepared&&on_park&&!kernel_cpu_current()->raw_locks);on_park();*r=park_reason;return 0;}
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
static int check_tree(struct kernel_avl_node *t,uint64_t *last,unsigned *count)
{
 if(!t)return 0;
 struct kernel_timer_fd *timer=(void *)((char *)t- offsetof(struct kernel_timer_fd,deadline));
 int l=check_tree(t->left,last,count);assert(!*count||*last<=t->key);*last=t->key;(*count)++;
 int r=check_tree(t->right,last,count);assert(abs(l-r)<=1&&t->height==1+(l>r?l:r)&&timer->indexed);return t->height;
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
static int open_timer(int clock)
{int64_t fd;assert(!kernel_files_timerfd_create(&files,clock,EVENT_NONBLOCK,&fd)&&fd>=0);return (int)fd;}
static void close_timer(int fd)
{struct kernel_open_file_description *owner=slots[fd];slots[fd]=0;assert(!kernel_open_file_release(&owner));}
static int64_t set_timer(int fd,unsigned flags,uint64_t value,uint64_t interval)
{
 struct event_itimerspec input={.value=time_spec(value),.interval=time_spec(interval)};int64_t result;
 assert(!kernel_files_timerfd_settime(&files,0,fd,flags,(uintptr_t)&input,0,&result));return result;
}
static uint64_t remaining(int fd)
{
 struct event_itimerspec output;int64_t result;
 assert(!kernel_files_timerfd_gettime(&files,0,fd,(uintptr_t)&output,&result)&&!result);return time_ns(output.value);
}
static int64_t read_timer(int fd,uint64_t *value)
{struct kernel_uaccess_iovec iov={(uintptr_t)value,8};return kernel_event_fd_io(slots[fd],0,&iov,1,8,0);}
static void clock_steps(void)
{
 int relative=open_timer(0),absolute=open_timer(0),monotonic=open_timer(1);uint64_t value;
 uint64_t real=kernel_time_realtime_ns(),start=now_ns;
 assert(!set_timer(relative,0,1000,0)&&!set_timer(absolute,1,real+2000,0)&&!set_timer(monotonic,1,start+3000,0));
 uint64_t first=kernel_timer_fd_deadline();assert(first==start+1000);
 wall_offset-=500;kernel_timer_fd_clock_changed();
 assert(remaining(relative)==1000&&remaining(absolute)==2500&&remaining(monotonic)==3000&&kernel_timer_fd_deadline()==first);
 wall_offset+=3000;kernel_timer_fd_clock_changed();kernel_timer_fd_expire(now_ns+1);
 assert(read_timer(absolute,&value)==8&&value==1);
 assert(read_timer(relative,&value)==-KERNEL_EAGAIN&&remaining(relative)==1000&&remaining(monotonic)==3000);
 close_timer(relative);close_timer(absolute);close_timer(monotonic);
 /* An expiry already selected using the old ticks must recheck wall time. */
 absolute=open_timer(0);assert(!set_timer(absolute,1,kernel_time_realtime_ns()+100,0));
 uint64_t old=kernel_timer_fd_deadline();wall_offset-=200;now_ns+=100;
 kernel_timer_fd_expire(old);assert(read_timer(absolute,&value)==-KERNEL_EAGAIN&&remaining(absolute)==200);
 kernel_timer_fd_clock_changed();now_ns+=200;kernel_timer_fd_expire(now_ns);
 assert(read_timer(absolute,&value)==8&&value==1);close_timer(absolute);
 puts("PASS timerfd relative/absolute forward/backward clock steps");
}
static void clock_cancellation(void)
{
 int fd=open_timer(0);uint64_t value;
 assert(!set_timer(fd,3,kernel_time_realtime_ns()+10000,0));
 wall_offset++;kernel_timer_fd_clock_changed();
 assert(kernel_event_fd_poll(slots[fd],KERNEL_POLLIN,0)&KERNEL_POLLIN);
 assert(read_timer(fd,&value)==-KERNEL_ECANCELED&&read_timer(fd,&value)==-KERNEL_EAGAIN);
 wall_offset++;kernel_timer_fd_clock_changed();assert(read_timer(fd,&value)==-KERNEL_ECANCELED);
 wall_offset++;kernel_timer_fd_clock_changed();
 struct event_itimerspec input={.value=time_spec(kernel_time_realtime_ns()+20)},output;
 memset(&output,0x5a,sizeof(output));struct event_itimerspec unchanged=output;int64_t result;
 assert(!kernel_files_timerfd_settime(&files,0,fd,3,(uintptr_t)&input,(uintptr_t)&output,&result));
 assert(result==-KERNEL_ECANCELED&&!memcmp(&output,&unchanged,sizeof(output))&&remaining(fd)==20);
 assert(read_timer(fd,&value)==-KERNEL_EAGAIN);now_ns+=20;kernel_timer_fd_expire(now_ns);
 assert(read_timer(fd,&value)==8&&value==1);
 assert(!set_timer(fd,3,0,0));wall_offset++;kernel_timer_fd_clock_changed();
 assert(read_timer(fd,&value)==-KERNEL_ECANCELED&&read_timer(fd,&value)==-KERNEL_EAGAIN);
 close_timer(fd);puts("PASS timerfd cancel read/reset/disarmed registration");
}
static void periodic_clock_rewind(void)
{
 int fd=open_timer(0);uint64_t value;
 assert(!set_timer(fd,1,kernel_time_realtime_ns()+100,100));now_ns+=100;kernel_timer_fd_expire(now_ns);
 wall_offset-=200;kernel_timer_fd_clock_changed();
 assert(read_timer(fd,&value)==0&&remaining(fd)==200);
 now_ns+=200;kernel_timer_fd_expire(now_ns);assert(read_timer(fd,&value)==8&&value==1&&remaining(fd)==100);
 close_timer(fd);puts("PASS timerfd pending periodic expiry before wall rewind");
}
static void signal_then_cancel(void)
{park_reason=KERNEL_WAIT_SIGNALLED;wall_offset++;kernel_timer_fd_clock_changed();}
static void signal_then_expire(void)
{park_reason=KERNEL_WAIT_SIGNALLED;now_ns+=100;kernel_timer_fd_expire(now_ns);}
static void signal_then_rewind_expiry(void)
{signal_then_expire();wall_offset-=200;kernel_timer_fd_clock_changed();}
static void signal_only(void)
{park_reason=KERNEL_WAIT_SIGNALLED;}
static void interrupted_timer_read(void)
{
 int fd=open_timer(0);uint64_t value;
 slots[fd]->open_flags&=~EVENT_NONBLOCK;
 assert(!set_timer(fd,3,kernel_time_realtime_ns()+10000,0));
 /* The token reports the signal winner; cancellation is published before
  * read reacquires its condition lock, as in Linux timerfd_read_iter. */
 on_park=signal_then_cancel;assert(read_timer(fd,&value)==-KERNEL_ECANCELED);
 slots[fd]->open_flags|=EVENT_NONBLOCK;assert(read_timer(fd,&value)==-KERNEL_EAGAIN);
 slots[fd]->open_flags&=~EVENT_NONBLOCK;
 on_park=signal_only;assert(read_timer(fd,&value)==-KERNEL_ERESTARTSYS);
 assert(!set_timer(fd,1,kernel_time_realtime_ns()+100,0));
 on_park=signal_then_expire;assert(read_timer(fd,&value)==8&&value==1);
 assert(!set_timer(fd,1,kernel_time_realtime_ns()+100,100));
 on_park=signal_then_rewind_expiry;assert(read_timer(fd,&value)==-KERNEL_ERESTARTSYS&&remaining(fd)==200);
 on_park=0;park_reason=KERNEL_WAIT_WOKEN;close_timer(fd);
 puts("PASS timerfd published cancel/expiry after signal winner");
}
int main(void){counters();index_cases();lifecycle();clock_steps();clock_cancellation();periodic_clock_rewind();interrupted_timer_read();assert(!allocations&&!timer_root&&!cancel_timers&&!prepared&&!credential_references);printf("EVENT HOST PASS page=%lu\n",BOAROS_PAGE_SIZE);return 0;}
