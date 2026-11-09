#define _GNU_SOURCE
#include "../../kernel/bpf/private.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/task.h>
#include <kernel/trace_events.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Hosted dependencies model FD/task ownership and an explicitly blocked VM
 * invocation. The production trace registration, targeting, ioctl, links,
 * user ABI and object information code is linked without replacement. */
struct kernel_task {int tid,borrows;};
static struct kernel_task actor={.tid=42},other={.tid=43};
static struct kernel_heap heap;
static struct kernel_files files={.heap=&heap};
static struct bpf_object *fds[128];
static uint32_t next_id=1;
static unsigned allocations,invocations,privileged=1;
static _Thread_local struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED};
static _Thread_local unsigned locks;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t admitted,resume;
static int pause_vm;
void *sync_test_cpu(void){return &cpu;}
void kernel_preempt_disable(void){cpu.preempt_depth++;}
void kernel_preempt_enable(void){assert(cpu.preempt_depth);cpu.preempt_depth--;}
unsigned kernel_cpu_get_boot_count(void){return 2;}
void kernel_raw_lock_acquire(struct kernel_raw_lock *l,struct kernel_raw_guard *g)
{assert(!locks);pthread_mutex_lock(&mutex);locks++;g->lock=l;}
void kernel_raw_lock_release(struct kernel_raw_guard *g)
{assert(locks==1);locks--;g->lock=NULL;pthread_mutex_unlock(&mutex);}
static void barrier(pthread_barrier_t *b){int r=pthread_barrier_wait(b);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);}
static const struct kernel_credentials creds;
const struct kernel_credentials *kernel_credentials_current(void){return &creds;}
int kernel_credentials_capable(const struct kernel_credentials *c,unsigned cap){(void)c;(void)cap;return privileged;}
enum kernel_task_status kernel_task_mm_borrow_mutable(struct kernel_task *t,struct kernel_mm **mm)
{assert(t==&actor);*mm=NULL;return 0;}
enum kernel_task_status kernel_task_files_borrow(struct kernel_task *t,struct kernel_files **f)
{assert(t==&actor);*f=&files;return 0;}
enum kernel_task_status kernel_task_tid(const struct kernel_task *t,kernel_pid_t *id){*id=t->tid;return 0;}
struct kernel_task *kernel_task_find_by_tid(kernel_pid_t id)
{struct kernel_task *t=id==42?&actor:id==43?&other:NULL;if(t)t->borrows++;return t;}
int kernel_task_has_user_context(const struct kernel_task *t){return !!t;}
void kernel_task_lookup_put(struct kernel_task *t){assert(t->borrows>0);t->borrows--;}
int bpf_user_copy(struct kernel_mm *mm,uint64_t a,void *p,size_t n,int from)
{(void)mm;assert(!locks);if(n && a<4096)return -KERNEL_EFAULT;if(from)memcpy(p,(void *)(uintptr_t)a,n);else memcpy((void *)(uintptr_t)a,p,n);return 0;}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t s,void **p)
{(void)h;assert(!locks);*p=calloc(n,s);if(!*p)return KERNEL_HEAP_STATUS_EMPTY;allocations++;return 0;}
void kernel_heap_release(struct kernel_heap *h,void *p){(void)h;assert(!locks&&allocations);allocations--;free(p);}
void bpf_object_initialize(struct bpf_object *o,struct kernel_heap *h,unsigned k){o->kind=k;o->heap=h;o->references=1;}
int bpf_object_publish(struct bpf_object *o){o->id=__atomic_fetch_add(&next_id,1,__ATOMIC_RELAXED);return 0;}
void bpf_object_get(struct bpf_object *o){assert(__atomic_fetch_add(&o->references,1,__ATOMIC_RELAXED)>0);}
void bpf_object_put(struct bpf_object *o)
{
    if(!o)return;
    assert(!locks);unsigned n=__atomic_fetch_sub(&o->references,1,__ATOMIC_ACQ_REL);assert(n);
    if(n!=1)return;
    if(o->kind==BPF_OBJECT_PERF)bpf_perf_destroy((void *)o);
    else if(o->kind==BPF_OBJECT_LINK){struct bpf_link *l=(void *)o;bpf_link_detach(l);kernel_bpf_program_put(l->program);bpf_object_put(&l->event->object);}
    kernel_heap_release(&heap,o);
}
void kernel_bpf_program_get(struct kernel_bpf_program *p){bpf_object_get(&p->object);}
void kernel_bpf_program_put(struct kernel_bpf_program *p){if(p)bpf_object_put(&p->object);}
struct bpf_object *bpf_object_fd(struct kernel_files *f,int32_t fd,unsigned kind,int *error)
{(void)f;*error=fd<0 || fd>=128 || !fds[fd]?-KERNEL_EBADF:fds[fd]->kind!=kind?-KERNEL_EINVAL:0;if(*error)return NULL;bpf_object_get(fds[fd]);return fds[fd];}
int kernel_bpf_program_fd(struct kernel_files *f,int32_t fd,uint32_t type,struct kernel_bpf_program **out)
{int e;struct bpf_object *o=bpf_object_fd(f,fd,BPF_OBJECT_PROGRAM,&e);if(!o)return e;*out=(void *)o;if((*out)->type!=type){bpf_object_put(o);return -KERNEL_EINVAL;}return 0;}
int bpf_object_install(struct kernel_files *f,struct bpf_object *o,uint32_t mode,uint32_t flags)
{(void)f;assert(mode==2 && flags<=1);for(int i=3;i<128;i++)if(!fds[i]){bpf_object_get(o);fds[i]=o;return i;}return -KERNEL_EMFILE;}
static void close_fd(int fd)
{struct bpf_object *o=fds[fd];assert(o);fds[fd]=NULL;if(o->kind==BPF_OBJECT_PERF)bpf_perf_close_file((void *)o);bpf_object_put(o);}
int bpf_program_trace_context(struct kernel_bpf_program *p,uint32_t type)
{assert(!locks);return p->type==BPF_TRACEPOINT && type<=2?0:-KERNEL_EINVAL;}
uint64_t bpf_runtime_run(struct kernel_bpf_program *p,struct kernel_task *t,void *ctx,uint32_t size,const struct kernel_bpf_packet *packet,uint64_t cookie)
{
    assert(!cookie || cookie==987);
    assert(!locks && !packet && t);assert(p->object.references>=2);
    struct kernel_trace_sys_enter *e=ctx;assert(e->common.pid==t->tid && e->id==155);
    assert(size==sizeof(*e) && e->args[0]==123 && e->args[5]==456);
    __atomic_fetch_add(&invocations,1,__ATOMIC_RELAXED);
    if(__atomic_load_n(&pause_vm,__ATOMIC_ACQUIRE)){barrier(&admitted);barrier(&resume);}
    return p->run_time;
}
struct perf_attr {uint32_t type,size;uint64_t config,period,sample,read_format,flags;uint32_t wakeup,bp;uint64_t rest[11];};
static int open_event(int pid,int cpu_id)
{struct perf_attr a={.type=2,.size=sizeof(a),.config=1,.flags=1};return kernel_bpf_perf_open(&actor,(uintptr_t)&a,pid,cpu_id,-1,8);}
static void emit(struct kernel_task *t)
{uint64_t args[6]={123,0,0,0,0,456};kernel_bpf_trace_enter(t,155,args);}
static void *running(void *unused){(void)unused;emit(&actor);return NULL;}
static int new_program(void)
{
    struct kernel_bpf_program *p;assert(!kernel_heap_allocate_zeroed(&heap,1,sizeof(*p),(void **)&p));
    bpf_object_initialize(&p->object,&heap,BPF_OBJECT_PROGRAM);p->type=BPF_TRACEPOINT;p->run_time=1;bpf_object_publish(&p->object);
    int fd=bpf_object_install(&files,&p->object,2,1);bpf_object_put(&p->object);return fd;
}
static void io(int fd,uint64_t command,uint64_t arg){assert(!bpf_perf_ioctl((void *)fds[fd],&actor,&files,command,arg));}
static uint64_t count(int fd)
{uint64_t value=0;size_t n=0;assert(!bpf_perf_read((void *)fds[fd],&value,8,&n)&&n==8);return value;}
static void lifecycle(void)
{
    int prog=new_program(),fd=open_event(0,-1);assert(fd>=0 && actor.borrows==1);
    io(fd,0x40042408,prog);emit(&actor);assert(invocations==1 && count(fd)==0);
    io(fd,0x2400,0);emit(&actor);assert(invocations==2 && count(fd)==1);
    emit(&other);assert(invocations==3 && count(fd)==1);
    cpu.logical_id=1;emit(&actor);assert(invocations==4 && count(fd)==2);cpu.logical_id=0;
    io(fd,0x2401,0);emit(&actor);assert(invocations==5 && count(fd)==2);io(fd,0x2400,0);
    assert(!pthread_barrier_init(&admitted,NULL,2));assert(!pthread_barrier_init(&resume,NULL,2));
    pause_vm=1;pthread_t thread;assert(!pthread_create(&thread,NULL,running,NULL));barrier(&admitted);
    close_fd(fd);assert(actor.borrows==1);assert(invocations==6);
    __atomic_store_n(&pause_vm,0,__ATOMIC_RELEASE);emit(&actor);assert(invocations==6);
    barrier(&resume);assert(!pthread_join(thread,NULL));
    assert(!actor.borrows);pthread_barrier_destroy(&admitted);pthread_barrier_destroy(&resume);
    fd=open_event(-1,1);io(fd,0x40042408,prog);io(fd,0x2400,0);emit(&actor);assert(invocations==7 && count(fd)==0);
    cpu.logical_id=1;emit(&other);assert(invocations==8 && count(fd)==1);cpu.logical_id=0;close_fd(fd);
    fd=open_event(0,-1);union bpf_attribute a={.link={.program_fd=prog,.target_fd=fd,.attach_type=41,.cookie=987}};
    int link=bpf_link_create(&files,&a);assert(link>=0);io(fd,0x2400,0);close_fd(fd);close_fd(prog);
    emit(&actor);assert(invocations==9);assert(!bpf_link_detach((void *)fds[link]));emit(&actor);assert(invocations==9);
    close_fd(link);assert(!actor.borrows && !allocations);
    /* The return value combines all attached programs, not just the program
     * belonging to a qualified perf event; no early stop skips later BPF. */
    int p1=new_program(),p2=new_program(),f1=open_event(0,-1),f2=open_event(0,-1);
    ((struct kernel_bpf_program *)fds[p1])->run_time=0;
    io(f1,0x40042408,p1);io(f2,0x40042408,p2);io(f1,0x2400,0);io(f2,0x2400,0);
    unsigned before=invocations;emit(&actor);assert(invocations==before+2 && !count(f1) && !count(f2));
    close_fd(f1);close_fd(f2);close_fd(p1);close_fd(p2);assert(!actor.borrows && !allocations);
}
static void attributes(void)
{
    struct perf_attr a={.type=2,.size=sizeof(a),.config=1};
    assert(kernel_bpf_perf_open(&actor,1,0,-1,-1,16)==-KERNEL_EINVAL);
    assert(kernel_bpf_perf_open(&actor,1,0,-1,-1,0)==-KERNEL_EFAULT);
    a.size=8;assert(kernel_bpf_perf_open(&actor,(uintptr_t)&a,0,-1,-1,0)==-KERNEL_E2BIG && a.size==144);
    a.size=sizeof(a);a.flags=UINT64_C(1)<<63;assert(kernel_bpf_perf_open(&actor,(uintptr_t)&a,0,-1,-1,0)==-KERNEL_EINVAL);
    a.flags=0;privileged=0;assert(kernel_bpf_perf_open(&actor,(uintptr_t)&a,0,-1,-1,0)==-KERNEL_EACCES);privileged=1;
    assert(open_event(-1,-1)==-KERNEL_EINVAL && open_event(999,0)==-KERNEL_ESRCH && open_event(0,2)==-KERNEL_ENODEV);
    a.flags=2;assert(kernel_bpf_perf_open(&actor,(uintptr_t)&a,0,-1,-1,0)==-KERNEL_EOPNOTSUPP);
    assert(!actor.borrows && !allocations);
}
int main(void){attributes();lifecycle();puts("BPF TRACE HOST PASS ABI targeting lifecycle concurrent close");return 0;}
