/* Actual poll core, with deterministic readiness and wait boundaries. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define BOAROS_ARCH_CONTEXT_H
static void *arch_current_cpu_get(void) { return NULL; }
static uintptr_t arch_interrupt_save(void) { return 0; }
static void arch_interrupt_restore(uintptr_t saved) { (void)saved; }
#ifndef POLL_SOURCE
#define POLL_SOURCE "../../fs/files/poll.c"
#endif
#include POLL_SOURCE
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"poll line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#include "../../fs/open_file_internal.h"
#include "../../kernel/sched/private.h"
static struct kernel_open_file_description target;
static struct kernel_wait_queue queue;
static unsigned polls, binds, parks, finishes, refs, live, initialized, allocs;
static size_t allocated_bytes;
static int readable, at_bind, at_park, signalled, stale_hint;
static unsigned hints;
static unsigned service_entries, service_exits, protocol_entries, protocol_depth;
static enum kernel_open_file_kind target_kind;
void kernel_socket_protocol_enter(struct kernel_socket_protocol_scope *s)
{ CHECK(!live && !protocol_depth); *s=(struct kernel_socket_protocol_scope){0}; protocol_depth=1;protocol_entries++; }
void kernel_socket_protocol_leave(struct kernel_socket_protocol_scope *s)
{ (void)s;CHECK(protocol_depth==1);protocol_depth=0; }
enum kernel_open_file_kind kernel_open_file_kind(const struct kernel_open_file_description *f)
{ CHECK(f==&target);return target_kind; }
static uint64_t now_ns = 1000, wait_deadline;
void kernel_service_scope_begin(struct kernel_service_scope *scope)
{ CHECK(!scope->entered);scope->entered=1;service_entries++; }
void kernel_service_scope_end(struct kernel_service_scope *scope)
{ if(scope->entered){service_exits++;scope->entered=0;} }
int kernel_files_is_live(const struct kernel_files *f) { return f != NULL; }
enum kernel_files_status kernel_files_pin(struct kernel_files *f,int64_t fd,
    struct kernel_open_file_description **out,int64_t *result)
{ (void)f; *out=fd==7?&target:NULL; *result=*out?0:-KERNEL_EBADF; if(*out)refs++;return KERNEL_FILES_STATUS_OK; }
enum kernel_open_file_status kernel_open_file_release(struct kernel_open_file_description **out)
{ CHECK(*out==&target && refs);refs--;*out=NULL;return KERNEL_OPEN_FILE_STATUS_OK; }
uint32_t kernel_open_file_poll(struct kernel_open_file_description *f,uint32_t requested,struct kernel_wait_queue **out)
{ CHECK(f==&target);CHECK(target_kind!=KERNEL_OPEN_FILE_KIND_EPOLL || (!protocol_depth && !live));CHECK(!live || target_kind!=KERNEL_OPEN_FILE_KIND_SOCKET || protocol_depth);polls++;*out=&queue;return readable?(requested&(KERNEL_POLLIN|KERNEL_POLLRDNORM)):0; }
int kernel_epoll_wait_pending(struct kernel_open_file_description *f,struct kernel_wait_queue **out)
{ CHECK(f==&target && target_kind==KERNEL_OPEN_FILE_KIND_EPOLL && live && protocol_depth);if(out)*out=&queue;hints++;if(!out && stale_hint){stale_hint--;return 1;}return readable; }
void kernel_wait_node_init(struct kernel_wait_node *n,struct kernel_task *t)
{ initialized++;*n=(struct kernel_wait_node){.task=t}; }
enum kernel_scheduler_status kernel_wait_prepare(struct kernel_wait_queue *q,uint64_t d,int i,struct kernel_wait_token *t)
{ (void)q;(void)i;CHECK(protocol_depth == (target_kind==KERNEL_OPEN_FILE_KIND_SOCKET || target_kind==KERNEL_OPEN_FILE_KIND_EPOLL));wait_deadline=d;*t=(struct kernel_wait_token){0};live++;return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_scheduler_status kernel_wait_node_bind(struct kernel_wait_queue *q,struct kernel_wait_node *n,const struct kernel_wait_token *t)
{ (void)n;(void)t;CHECK(q==&queue && live==1);binds++;if(at_bind)readable=1;return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_scheduler_status kernel_wait_park(const struct kernel_wait_token *t,enum kernel_wait_wake_reason *r)
{ (void)t;CHECK(!protocol_depth);parks++;if(at_park)readable=1;*r=signalled?KERNEL_WAIT_SIGNALLED:(at_park?KERNEL_WAIT_WOKEN:KERNEL_WAIT_TIMEOUT);if(*r==KERNEL_WAIT_TIMEOUT && wait_deadline>now_ns)now_ns=wait_deadline;return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_scheduler_status kernel_wait_finish(struct kernel_wait_token *t)
{ (void)t;CHECK(live==1);live--;finishes++;return KERNEL_SCHEDULER_STATUS_OK; }
void kernel_signal_restore_temporary_mask(struct kernel_task *t,uint64_t m,int i)
{ if(t && !i)t->signal_blocked=m; }
enum kernel_signal_status kernel_signal_set_temporary_mask(struct kernel_task *t,uint64_t m,uint64_t *saved)
{ CHECK(t && saved);*saved=t->signal_blocked;t->signal_blocked=m;return KERNEL_SIGNAL_STATUS_OK; }
uint64_t kernel_time_monotonic_ns(void) { return now_ns; }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns,uint64_t *deadline)
{ CHECK(ns>now_ns);*deadline=ns;return KERNEL_TIME_STATUS_OK; }
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *mm,void *to,uint64_t from,size_t n,size_t *copied)
{ (void)mm;*copied=0;if(from<4096)return KERNEL_UACCESS_STATUS_FAULT;memcpy(to,(void *)(uintptr_t)from,n);*copied=n;return KERNEL_UACCESS_STATUS_OK; }
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *mm,uint64_t to,const void *from,size_t n,size_t *copied)
{ (void)mm;*copied=0;if(to<4096)return KERNEL_UACCESS_STATUS_FAULT;memcpy((void *)(uintptr_t)to,from,n);*copied=n;return KERNEL_UACCESS_STATUS_OK; }
int kernel_files_fd_matches(struct kernel_files *f,int64_t fd,const struct kernel_open_file_description *file)
{ (void)f;return fd==7 && file==&target; }
enum kernel_heap_status kernel_heap_allocate(struct kernel_heap *h,size_t n,void **out)
{ (void)h;*out=malloc(n);CHECK(*out);allocs++;allocated_bytes=n;return KERNEL_HEAP_STATUS_OK; }
enum kernel_heap_status kernel_heap_release(struct kernel_heap *h,void *p)
{ (void)h;CHECK(allocs==1);allocs--;free(p);return KERNEL_HEAP_STATUS_OK; }
static int run(struct kernel_pollfd *p,unsigned n,int immediate)
{ struct kernel_files f={0};int ret=core_poll_run(&f,NULL,p,n,123,immediate,0,0);CHECK(!refs&&!live&&!allocs);return ret; }
static void reset(void)
{ polls=binds=parks=finishes=initialized=0;allocated_bytes=0;readable=at_bind=at_park=signalled=stale_hint=0;hints=0;service_entries=service_exits=protocol_entries=protocol_depth=0;target_kind=KERNEL_OPEN_FILE_KIND_PIPE;now_ns=1000; }
static void empty_waits(void)
{
    struct kernel_files files={0};struct kernel_mm mm={0};struct kernel_task task={0};
    struct kernel_timespec ts={0};int64_t result;
    reset();
    CHECK(!kernel_files_ppoll(&files,&mm,&task,0,0,(uintptr_t)&ts,0,0,&result) && !result);
    CHECK(!service_entries && !parks && !allocs);
    reset();ts=(struct kernel_timespec){0,100};
    CHECK(!kernel_files_ppoll(&files,&mm,&task,0,0,(uintptr_t)&ts,0,0,&result) && !result);
    CHECK(!service_entries && parks==1 && finishes==1 && !ts.tv_sec && !ts.tv_nsec);
    reset();signalled=1;
    CHECK(!kernel_files_ppoll(&files,&mm,&task,0,0,0,0,0,&result) && result==-KERNEL_EINTR);
    CHECK(!service_entries && parks==1 && finishes==1);
    reset();ts=(struct kernel_timespec){0,100};
    struct kernel_sigset_argpack pack={0,8};
    CHECK(!kernel_files_pselect6(&files,&mm,&task,0,0,0,0,(uintptr_t)&ts,(uintptr_t)&pack,&result) && !result);
    CHECK(!service_entries && parks==1 && finishes==1 && !ts.tv_nsec);
    reset();uint64_t mask=4;task.signal_blocked=2;pack.sigmask_ptr=(uintptr_t)&mask;ts=(struct kernel_timespec){0};
    CHECK(!kernel_files_pselect6(&files,&mm,&task,0,0,0,0,(uintptr_t)&ts,(uintptr_t)&pack,&result) && !result);
    CHECK(service_entries==1 && service_exits==1 && task.signal_blocked==2);
    reset();
    CHECK(!kernel_files_ppoll(&files,&mm,&task,0,0,1,0,0,&result) && result==-KERNEL_EFAULT);
    CHECK(!service_entries && !parks);
    reset();struct kernel_pollfd fd={7,KERNEL_POLLIN,0};readable=1;
    CHECK(!kernel_files_ppoll(&files,&mm,&task,(uintptr_t)&fd,1,0,0,0,&result) && result==1);
    CHECK(service_entries==1 && service_exits==1 && !refs && !live && !allocs);
    reset();uint64_t readfds=UINT64_C(1)<<7;ts=(struct kernel_timespec){0};readable=1;
    CHECK(!kernel_files_pselect6(&files,&mm,&task,8,(uintptr_t)&readfds,0,0,(uintptr_t)&ts,0,&result) && result==1);
    CHECK(service_entries==1 && service_exits==1 && readfds==(UINT64_C(1)<<7) && !refs && !live && !allocs);
    puts("empty ppoll/pselect: local timeout and copy, retained signal/FD service scopes passed");
}
int main(void)
{
    empty_waits();
    struct kernel_pollfd p[5];
    reset();p[0]=(struct kernel_pollfd){7,KERNEL_POLLIN,0};readable=1;
    CHECK(run(p,1,0)==1 && p[0].revents==KERNEL_POLLIN);
    CHECK(polls==1 && !binds && !parks);
    reset();for(unsigned i=0;i<5;i++)p[i]=(struct kernel_pollfd){7,KERNEL_POLLIN,0};
    CHECK(run(p,5,1)==0 && polls==5 && initialized==0);
    CHECK(allocated_bytes==5*sizeof(struct kernel_open_file_description *));
    reset();p[0]=(struct kernel_pollfd){7,KERNEL_POLLIN,0};at_bind=1;
    CHECK(run(p,1,0)==1 && p[0].revents==KERNEL_POLLIN);
    CHECK(polls==3 && binds==1 && parks==0 && finishes==1);
    reset();p[0]=(struct kernel_pollfd){7,KERNEL_POLLIN,0};at_park=1;
    CHECK(run(p,1,0)==1 && p[0].revents==KERNEL_POLLIN);
    CHECK(polls==4 && binds==1 && parks==1 && finishes==1);
    reset();p[0]=(struct kernel_pollfd){7,KERNEL_POLLIN,123};
    CHECK(run(p,1,0)==0 && p[0].revents==0 && polls==4 && parks==1);
    reset();p[0]=(struct kernel_pollfd){7,KERNEL_POLLIN,123};signalled=1;
    CHECK(run(p,1,0)==-KERNEL_EINTR && finishes==1);
    reset();p[0]=(struct kernel_pollfd){-1,KERNEL_POLLIN,123};p[1]=(struct kernel_pollfd){8,KERNEL_POLLIN,123};
    CHECK(run(p,2,0)==1 && p[0].revents==0 && p[1].revents==KERNEL_POLLNVAL && !polls&&!parks);
    CHECK(!protocol_entries);
    for (unsigned i=0;i<2;i++) {
        reset();target_kind=i?KERNEL_OPEN_FILE_KIND_EPOLL:KERNEL_OPEN_FILE_KIND_SOCKET;
        p[0]=(struct kernel_pollfd){7,KERNEL_POLLIN,0};at_park=1;
        CHECK(run(p,1,0)==1 && protocol_entries==1 && !protocol_depth && finishes==1);
        reset();target_kind=i?KERNEL_OPEN_FILE_KIND_EPOLL:KERNEL_OPEN_FILE_KIND_SOCKET;
        p[0]=(struct kernel_pollfd){7,KERNEL_POLLIN,0};at_bind=1;
        CHECK(run(p,1,0)==1 && protocol_entries==1 && !protocol_depth && finishes==1 && !parks);
    }
    CHECK(hints==2 && polls==2);
    reset();target_kind=KERNEL_OPEN_FILE_KIND_EPOLL;stale_hint=1;
    p[0]=(struct kernel_pollfd){7,KERNEL_POLLIN,0};
    CHECK(run(p,1,0)==0 && !p[0].revents && parks==1 && finishes==2 && polls==3);
    CHECK(hints==4 && protocol_entries==2 && !protocol_depth);
    reset();target_kind=KERNEL_OPEN_FILE_KIND_EPOLL;at_bind=1;
    p[0]=(struct kernel_pollfd){7,KERNEL_POLLRDNORM,0};
    CHECK(run(p,1,0)==1 && p[0].revents==KERNEL_POLLRDNORM && !parks && finishes==1);
    puts("poll snapshots: ready, immediate, register/recheck, wake, timeout, signal, invalid passed");
}
