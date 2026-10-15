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
static struct kernel_open_file_description target;
static struct kernel_wait_queue queue;
static unsigned polls, binds, parks, finishes, refs, live, initialized, allocs;
static size_t allocated_bytes;
static int readable, at_bind, at_park, signalled;
int kernel_files_is_live(const struct kernel_files *f) { return f != NULL; }
enum kernel_files_status kernel_files_pin(struct kernel_files *f,int64_t fd,
    struct kernel_open_file_description **out,int64_t *result)
{ (void)f; *out=fd==7?&target:NULL; *result=*out?0:-KERNEL_EBADF; if(*out)refs++;return KERNEL_FILES_STATUS_OK; }
enum kernel_open_file_status kernel_open_file_release(struct kernel_open_file_description **out)
{ CHECK(*out==&target && refs);refs--;*out=NULL;return KERNEL_OPEN_FILE_STATUS_OK; }
uint32_t kernel_open_file_poll(struct kernel_open_file_description *f,uint32_t requested,struct kernel_wait_queue **out)
{ CHECK(f==&target);polls++;*out=&queue;return readable?(requested&KERNEL_POLLIN):0; }
void kernel_wait_node_init(struct kernel_wait_node *n,struct kernel_task *t)
{ initialized++;*n=(struct kernel_wait_node){.task=t}; }
enum kernel_scheduler_status kernel_wait_prepare(struct kernel_wait_queue *q,uint64_t d,int i,struct kernel_wait_token *t)
{ (void)q;(void)d;(void)i;*t=(struct kernel_wait_token){0};live++;return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_scheduler_status kernel_wait_node_bind(struct kernel_wait_queue *q,struct kernel_wait_node *n,const struct kernel_wait_token *t)
{ (void)n;(void)t;CHECK(q==&queue && live==1);binds++;if(at_bind)readable=1;return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_scheduler_status kernel_wait_park(const struct kernel_wait_token *t,enum kernel_wait_wake_reason *r)
{ (void)t;parks++;if(at_park)readable=1;*r=signalled?KERNEL_WAIT_SIGNALLED:(at_park?KERNEL_WAIT_WOKEN:KERNEL_WAIT_TIMEOUT);return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_scheduler_status kernel_wait_finish(struct kernel_wait_token *t)
{ (void)t;CHECK(live==1);live--;finishes++;return KERNEL_SCHEDULER_STATUS_OK; }
void kernel_signal_restore_temporary_mask(struct kernel_task *t,uint64_t m,int i)
{ (void)t;(void)m;(void)i; }
enum kernel_heap_status kernel_heap_allocate(struct kernel_heap *h,size_t n,void **out)
{ (void)h;*out=malloc(n);CHECK(*out);allocs++;allocated_bytes=n;return KERNEL_HEAP_STATUS_OK; }
enum kernel_heap_status kernel_heap_release(struct kernel_heap *h,void *p)
{ (void)h;CHECK(allocs==1);allocs--;free(p);return KERNEL_HEAP_STATUS_OK; }
static int run(struct kernel_pollfd *p,unsigned n,int immediate)
{ struct kernel_files f={0};int ret=core_poll_run(&f,NULL,p,n,123,immediate,0,0);CHECK(!refs&&!live&&!allocs);return ret; }
static void reset(void)
{ polls=binds=parks=finishes=initialized=0;allocated_bytes=0;readable=at_bind=at_park=signalled=0; }
int main(void)
{
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
    puts("poll snapshots: ready, immediate, register/recheck, wake, timeout, signal, invalid passed");
}
