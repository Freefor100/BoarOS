/* Exercise the real OFD subscription and wait-cursor lifetime together. */
#include "../../fs/async_io.c"
#include "../../kernel/sched/private.h"
#include <kernel/time.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Thread_local uintptr_t sync_test_irq=2;
static _Thread_local struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED};
static _Thread_local struct kernel_io_context io;
struct kernel_scheduler scheduler={.initialized=KERNEL_SCHEDULER_INITIALIZED};
static struct kernel_heap heap;
static struct kernel_credentials credentials={.uid=17,.euid=29};
static struct kernel_pid identities[2]={{.number=41,.references=1},{.number=42,.references=1}};
static struct kernel_pid_member members[2];
static unsigned allocations,allocation_failure,signals,signal_hold,signal_entered,signal_release;
static unsigned closing,closed;
static int delivered_fd,delivered_type;
static unsigned delivered_signum,delivered_reason;
static struct kernel_pid *delivered_owner;
void *sync_test_cpu(void){cpu.bootstrap_io=&io;return &cpu;}
void *sync_test_current_task(void){return NULL;}
struct kernel_task *kernel_task_current(void){return NULL;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
void kernel_assert_can_block(void){assert(!cpu.raw_locks);}
void kernel_preempt_disable(void){assert(!cpu.raw_locks);cpu.preempt_depth++;}
void kernel_preempt_enable(void){assert(!cpu.raw_locks&&cpu.preempt_depth);cpu.preempt_depth--;}
const struct kernel_credentials *kernel_credentials_current(void){return &credentials;}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t z,void **p)
{assert(h==&heap&&!cpu.raw_locks);if(allocation_failure)return KERNEL_HEAP_STATUS_EMPTY;*p=calloc(n,z);assert(*p);allocations++;return 0;}
void kernel_heap_release(struct kernel_heap *h,void *p)
{assert(h==&heap&&p&&!cpu.raw_locks&&allocations);allocations--;free(p);}
int kernel_task_identity_get(int32_t number,struct kernel_pid **out)
{*out=NULL;for(unsigned i=0;i<2;i++)if(identities[i].number==number){kernel_task_identity_retain(&identities[i]);*out=&identities[i];return 0;}return -KERNEL_ESRCH;}
void kernel_task_identity_retain(struct kernel_pid *id)
{KERNEL_RAW_SCOPE(g,&kernel_wait_domain);assert(id->references);id->references++;}
void kernel_task_identity_put(struct kernel_pid *id)
{KERNEL_RAW_SCOPE(g,&kernel_wait_domain);assert(id->references>1);id->references--;}
void kernel_signal_sigio(struct kernel_pid *id,int type,uint32_t uid,uint32_t euid,unsigned sig,int fd,unsigned reason)
{
    assert(!cpu.raw_locks&&uid==17&&euid==29&&id->references>=3);
    signals++;delivered_owner=id;delivered_type=type;delivered_signum=sig;delivered_fd=fd;delivered_reason=reason;
    if(__atomic_load_n(&signal_hold,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&signal_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&signal_release,__ATOMIC_ACQUIRE))sched_yield();
    }
}
struct kernel_wait_queue *kernel_socket_async_source(struct kernel_socket *s){(void)s;abort();}
void kernel_open_file_set_status_flags(struct kernel_open_file_description *f,uint32_t flags)
{const unsigned mask=02000|04000|020000|01000000;__atomic_store_n(&f->open_flags,(f->open_flags&~mask)|(flags&mask),__ATOMIC_RELAXED);}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *mm,void *to,uint64_t from,size_t size,size_t *copied)
{(void)mm;if(from<4096)return KERNEL_UACCESS_STATUS_FAULT;memcpy(to,(void *)(uintptr_t)from,size);*copied=size;return 0;}
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *mm,uint64_t to,const void *from,size_t size,size_t *copied)
{if(to<4096)return KERNEL_UACCESS_STATUS_FAULT;return kernel_copy_from_user(mm,(void *)(uintptr_t)to,(uintptr_t)from,size,copied);}
struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *t){(void)t;abort();}
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *t){(void)t;abort();}
struct kernel_task *kernel_wait_current_task(void){return NULL;}
int kernel_wait_backend_initialized(void){return 1;}
int kernel_wait_backend_signal(struct kernel_task *t){(void)t;return 0;}
uint64_t kernel_wait_backend_time(void){return 0;}
void kernel_wait_backend_commit(struct kernel_task *t,uint64_t d,int i){(void)t;(void)d;(void)i;abort();}
void kernel_wait_backend_notify(struct kernel_task *t,uint32_t r){(void)t;(void)r;abort();}
void kernel_wait_backend_ready(struct kernel_task *t){(void)t;abort();}
void kernel_wait_backend_quiesce(void){sched_yield();}
uint64_t riscv_time_read(void){return 1;}
uint64_t kernel_time_monotonic_ns(void){return 1;}
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns,uint64_t *deadline){*deadline=ns;return 0;}
enum kernel_time_status kernel_time_deadline_from_realtime(uint64_t ns,uint64_t *deadline){*deadline=ns;return 0;}
enum kernel_scheduler_status kernel_wait_backend_switch_locked(struct kernel_task *t,struct kernel_raw_guard *g){(void)t;(void)g;abort();}
static void emit_event(struct kernel_wait_queue *queue,uint64_t reason)
{struct kernel_wait_cursor c={0};assert(kernel_wait_cursor_prepare_event(queue,&c,reason,NULL));kernel_wait_cursor_wake(&c,1);}
static void *producer(void *p){emit_event(p,KERNEL_ASYNC_POLL_IN);return NULL;}
static void *closer(void *p)
{__atomic_store_n(&closing,1,__ATOMIC_RELEASE);kernel_open_file_async_close(p);__atomic_store_n(&closed,1,__ATOMIC_RELEASE);return NULL;}
static struct kernel_wait_queue *expired_source(void *instance){(void)instance;return NULL;}
static struct kernel_pid *expired_owner(void *instance,struct kernel_task *task,int *type)
{(void)instance;(void)task;(void)type;abort();}
int main(void)
{
    kernel_wait_domain_init();
    for(unsigned i=0;i<2;i++){members[i].task=(void *)(uintptr_t)1;identities[i].members[KERNEL_PID_TGID]=&members[i];identities[i].members[KERNEL_PID_TID]=&members[i];identities[i].members[KERNEL_PID_PGID]=&members[i];}
    struct kernel_open_file_description regular={.heap=&heap,.references=1,.kind=KERNEL_OPEN_FILE_KIND_REGULAR};
    assert(!kernel_open_file_async_flags(&regular,5,020000|04000) && regular.open_flags==04000 && !regular.async_io);
    const struct kernel_char_device expired={.async_source=expired_source,.async_owner=expired_owner};
    struct kernel_open_file_description terminal={.heap=&heap,.references=1,.kind=KERNEL_OPEN_FILE_KIND_CONSOLE,.open_flags=04000,.device=&expired};
    assert(kernel_open_file_async_flags(&terminal,6,020000)==-KERNEL_ENOTTY && terminal.open_flags==04000 && !terminal.async_io);
    assert(!kernel_open_file_async_flags(&terminal,6,0) && !terminal.open_flags);
    struct kernel_pipe pipe={0};kernel_wait_queue_init(&pipe.async_events);
    struct kernel_open_file_description file={.heap=&heap,.references=1,.kind=KERNEL_OPEN_FILE_KIND_PIPE,.pipe=&pipe,.pipe_endpoint=1};
    assert(kernel_open_file_async_fcntl(&file,KERNEL_FILES_F_GETOWN,0,NULL)==0);
    allocation_failure=1;
    assert(kernel_open_file_async_flags(&file,3,020000|04000)==-KERNEL_ENOMEM&&!file.open_flags&&!file.async_io);
    allocation_failure=0;
    assert(!kernel_open_file_async_fcntl(&file,KERNEL_FILES_F_SETOWN,41,NULL));
    assert(kernel_open_file_async_fcntl(&file,KERNEL_FILES_F_GETOWN,0,NULL)==41);
    assert(kernel_open_file_async_fcntl(&file,KERNEL_FILES_F_SETOWN_EX,1,(void *)(uintptr_t)1)==-KERNEL_EFAULT);
    assert(kernel_open_file_async_fcntl(&file,KERNEL_FILES_F_GETOWN_EX,1,(void *)(uintptr_t)1)==-KERNEL_EFAULT);
    assert(kernel_open_file_async_fcntl(&file,KERNEL_FILES_F_GETOWN,0,NULL)==41);
    assert(kernel_open_file_async_fcntl(&file,KERNEL_FILES_F_SETOWN,INT32_MIN,NULL)==-KERNEL_EINVAL);
    assert(kernel_open_file_async_fcntl(&file,KERNEL_FILES_F_SETOWN,99,NULL)==-KERNEL_ESRCH);
    assert(!kernel_open_file_async_fcntl(&file,KERNEL_FILES_F_SETSIG,35,NULL));
    assert(!kernel_open_file_async_flags(&file,3,020000));
    emit_event(&pipe.async_events,KERNEL_ASYNC_POLL_IN);
    assert(signals==1&&delivered_owner==&identities[0]&&delivered_fd==3&&delivered_type==1&&delivered_signum==35&&delivered_reason==1);
    emit_event(&pipe.async_events,KERNEL_ASYNC_POLL_OUT);assert(signals==1);
    assert(!kernel_open_file_async_flags(&file,8,020000|04000));
    emit_event(&pipe.async_events,KERNEL_ASYNC_POLL_IN);assert(signals==2&&delivered_fd==3);
    assert(!kernel_open_file_async_flags(&file,8,04000)&&!kernel_wait_queue_registrations(&pipe.async_events));
    assert(!kernel_open_file_async_flags(&file,8,020000));
    emit_event(&pipe.async_events,KERNEL_ASYNC_POLL_IN);assert(signals==3&&delivered_fd==8);
    assert(!kernel_open_file_async_fcntl(&file,KERNEL_FILES_F_SETOWN,(uint32_t)-42,NULL));
    emit_event(&pipe.async_events,KERNEL_ASYNC_POLL_IN);assert(signals==4&&delivered_owner==&identities[1]&&delivered_type==2);
    identities[1].members[KERNEL_PID_PGID]=NULL;
    assert(kernel_open_file_async_fcntl(&file,KERNEL_FILES_F_GETOWN,0,NULL)==0);
    identities[1].members[KERNEL_PID_PGID]=&members[1];
    emit_event(&pipe.async_events,KERNEL_ASYNC_INVALIDATE);
    assert(!kernel_wait_queue_registrations(&pipe.async_events) && !(file.open_flags&020000) && signals==4);
    assert(!kernel_open_file_async_flags(&file,9,020000));
    emit_event(&pipe.async_events,KERNEL_ASYNC_POLL_IN);assert(signals==5 && delivered_fd==9);
    signal_hold=1;pthread_t sending,closing_thread;
    assert(!pthread_create(&sending,NULL,producer,&pipe.async_events));
    while(!__atomic_load_n(&signal_entered,__ATOMIC_ACQUIRE))sched_yield();
    assert(!pthread_create(&closing_thread,NULL,closer,&file));
    while(!__atomic_load_n(&closing,__ATOMIC_ACQUIRE))sched_yield();
    while(kernel_wait_queue_registrations(&pipe.async_events))sched_yield();
    assert(!__atomic_load_n(&closed,__ATOMIC_ACQUIRE));
    __atomic_store_n(&signal_release,1,__ATOMIC_RELEASE);
    assert(!pthread_join(sending,NULL)&&!pthread_join(closing_thread,NULL));
    assert(closed&&!file.async_io&&!allocations&&identities[0].references==1&&identities[1].references==1);
    assert(!kernel_wait_queue_close(&pipe.async_events)&&!kernel_wait_queue_destroy(&pipe.async_events));
    puts("PASS async OFD: owner/signum, allocation rollback, saved fd, pipe direction, and close/callback drain");
}
