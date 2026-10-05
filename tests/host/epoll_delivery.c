/* Actual epoll implementation; only task, usercopy, OFD and wait boundaries are modeled. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <kernel/epoll.h>
#include <kernel/task.h>
#ifndef EPOLL_SOURCE
#define EPOLL_SOURCE "../../fs/files/epoll.c"
#endif
#include EPOLL_SOURCE

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL epoll line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static struct kernel_heap heap;
static struct kernel_files files;
static struct kernel_open_file_description epfile, targets[2];
static struct kernel_wait_queue target_wait[2];
static struct kernel_open_file_description *fds[3];
static struct kernel_epoll_wait_request *requests[2];
static unsigned actor, actors[2];
#define registered requests[actor]
#define task ((struct kernel_task *)&actors[actor])
static struct kernel_mm *mm = (void *)&heap;
static size_t allocations, copy_remaining = SIZE_MAX;
static unsigned poll_notification, copy_action, poll_calls, readable[2];
static jmp_buf cancelled;

int kernel_files_is_live(const struct kernel_files *f) { return f == &files; }
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t z,void **p)
{ (void)h; *p=calloc(n,z); CHECK(*p); allocations++; return KERNEL_HEAP_STATUS_OK; }
enum kernel_heap_status kernel_heap_release(struct kernel_heap *h,void *p)
{ (void)h; CHECK(p && allocations); allocations--; free(p); return KERNEL_HEAP_STATUS_OK; }
enum kernel_open_file_kind kernel_open_file_kind(const struct kernel_open_file_description *f)
{ return f->kind; }
int kernel_open_file_supports_epoll(const struct kernel_open_file_description *f)
{ return f != NULL; }
enum kernel_open_file_status kernel_open_file_acquire(struct kernel_open_file_description *f)
{ CHECK(f && f->references); f->references++; return KERNEL_OPEN_FILE_STATUS_OK; }
enum kernel_open_file_status kernel_open_file_release(struct kernel_open_file_description **p)
{
    struct kernel_open_file_description *f=*p; CHECK(f && f->references);
    *p=NULL;
    if (!--f->references) {
        kernel_epoll_notify_file_release(f);
        if(f->kind==KERNEL_OPEN_FILE_KIND_EPOLL) CHECK(kernel_epoll_destroy(f->epoll)==KERNEL_FILES_STATUS_OK);
    }
    return KERNEL_OPEN_FILE_STATUS_OK;
}
enum kernel_files_status kernel_files_pin_data(struct kernel_files *f,int64_t fd,
    struct kernel_open_file_description **p,int64_t *result)
{
    CHECK(f==&files); *p=NULL; *result=-KERNEL_EBADF;
    if(fd>=0 && fd<3 && fds[fd]) { *p=fds[fd]; kernel_open_file_acquire(*p); *result=0; }
    return KERNEL_FILES_STATUS_OK;
}
void kernel_wait_queue_init(struct kernel_wait_queue *q) { *q=(struct kernel_wait_queue){.initialized=1}; }
void kernel_wait_node_init_callback(struct kernel_wait_node *n,kernel_wait_callback_fn fn,void *ctx)
{ *n=(struct kernel_wait_node){.callback=fn,.context=ctx}; }
void kernel_wait_queue_add(struct kernel_wait_queue *q,struct kernel_wait_node *n)
{ CHECK(!n->queue); n->queue=q; n->previous=q->tail; if(q->tail)q->tail->next=n;else q->head=n;q->tail=n; }
void kernel_wait_queue_remove(struct kernel_wait_node *n)
{ struct kernel_wait_queue *q=n->queue; CHECK(q); if(n->previous)n->previous->next=n->next;else q->head=n->next;
  if(n->next)n->next->previous=n->previous;else q->tail=n->previous;n->queue=NULL;n->next=n->previous=NULL; }
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *q)
{ for(struct kernel_wait_node *n=q->head;n;n=n->next) if(n->callback)n->callback(n,0);return KERNEL_SCHEDULER_STATUS_OK; }
uint32_t kernel_open_file_poll(struct kernel_open_file_description *f,uint32_t requested,struct kernel_wait_queue **q)
{
    CHECK(f>=targets && f<targets+2); unsigned i=(unsigned)(f-targets);
    CHECK(++poll_calls < 32); /* 单项非阻塞扫描必须结束，不能在失去就绪后自旋。 */
    if(q)*q=&target_wait[i];
    if(poll_notification) { poll_notification=0;kernel_wait_queue_wake_all(&target_wait[i]); }
    return readable[i] ? requested & KERNEL_POLLIN : 0;
}
uint64_t kernel_time_monotonic_ns(void) { return 1; }
uint64_t kernel_socket_next_timer_deadline(void) { return 0; }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t t,uint64_t *d)
{ *d=t;return KERNEL_TIME_STATUS_OK; }
enum kernel_signal_status kernel_signal_set_temporary_mask(struct kernel_task *t,uint64_t mask,uint64_t *old)
{ (void)t;(void)mask;*old=0;return KERNEL_SIGNAL_STATUS_OK; }
void kernel_signal_restore_temporary_mask(struct kernel_task *t,uint64_t m,int i) { (void)t;(void)m;(void)i; }
enum kernel_scheduler_status kernel_scheduler_block_current(struct kernel_wait_queue *q,uint64_t d,int i,enum kernel_wait_wake_reason *r)
{ (void)q;(void)d;(void)i;*r=KERNEL_WAIT_TIMEOUT;return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_task_status kernel_task_epoll_register(struct kernel_task *t,struct kernel_epoll_wait_request *r)
{ CHECK(t==task && !registered);registered=r;return KERNEL_TASK_STATUS_OK; }
enum kernel_task_status kernel_task_epoll_clear(struct kernel_task *t,struct kernel_epoll_wait_request *r)
{ CHECK(t==task && registered==r);registered=NULL;return KERNEL_TASK_STATUS_OK; }
static void control(int operation,uint32_t mode,uint64_t data)
{
    int64_t result;
    CHECK(kernel_files_epoll_ctl(&files,0,operation,1,KERNEL_EPOLLIN|mode,data,&result)==KERNEL_FILES_STATUS_OK && !result);
}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *m,void *out,uint64_t in,size_t n,size_t *copied)
{ (void)m;memcpy(out,(void *)(uintptr_t)in,n);*copied=n;return KERNEL_UACCESS_STATUS_OK; }
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *m,uint64_t out,const void *in,size_t n,size_t *copied)
{
    (void)m; unsigned action=copy_action;copy_action=0;
    if(action==1) control(KERNEL_EPOLL_CTL_MOD,KERNEL_EPOLLONESHOT,77);
    if(action==2) control(KERNEL_EPOLL_CTL_DEL,0,0);
    if(action==3) { kernel_epoll_abort_wait(registered); longjmp(cancelled,1); }
    if(action==4 || action==8) {
        struct kernel_open_file_description *f=fds[1];fds[1]=NULL;kernel_open_file_release(&f);
        if(action==8) { fds[1]=&targets[1];kernel_open_file_acquire(fds[1]);control(KERNEL_EPOLL_CTL_ADD,KERNEL_EPOLLONESHOT,88); }
    }
    if(action==5) {
        struct linux_epoll_event event;int64_t result;
        actor=1;
        CHECK(kernel_files_epoll_pwait(&files,mm,task,0,(uintptr_t)&event,1,0,0,0,&result)==KERNEL_FILES_STATUS_OK && result==0);
        CHECK(!registered);actor=0;
    }
    if(action==6) kernel_wait_queue_wake_all(&target_wait[0]);
    if(action==7) { control(KERNEL_EPOLL_CTL_DEL,0,0);control(KERNEL_EPOLL_CTL_ADD,KERNEL_EPOLLONESHOT,77); }
    *copied=n<copy_remaining?n:copy_remaining;memcpy((void *)(uintptr_t)out,in,*copied);
    if(copy_remaining!=SIZE_MAX)copy_remaining-=*copied;
    return *copied==n?KERNEL_UACCESS_STATUS_OK:KERNEL_UACCESS_STATUS_FAULT;
}
static int64_t wait_events(struct linux_epoll_event *out,int maximum)
{
    int64_t result;
    CHECK(kernel_files_epoll_pwait(&files,mm,task,0,(uintptr_t)out,maximum,0,0,0,&result)==KERNEL_FILES_STATUS_OK);
    CHECK(!registered); return result;
}
static void setup(uint32_t mode)
{
    poll_calls=0;readable[0]=readable[1]=1;
    CHECK(!allocations); memset(&files,0,sizeof(files));files.heap=&heap;
    epfile=(struct kernel_open_file_description){.kind=KERNEL_OPEN_FILE_KIND_EPOLL,.references=1};
    CHECK(!kernel_epoll_create(&heap,&epfile.epoll));epfile.epoll->file=&epfile;fds[0]=&epfile;
    for(unsigned i=0;i<2;i++) { targets[i]=(struct kernel_open_file_description){.kind=KERNEL_OPEN_FILE_KIND_PIPE,.references=1};
        fds[i+1]=&targets[i];kernel_wait_queue_init(&target_wait[i]); }
    copy_remaining=SIZE_MAX;control(KERNEL_EPOLL_CTL_ADD,mode,42);
}
static void teardown(void)
{
    for(unsigned i=0;i<3;i++)if(fds[i]) {struct kernel_open_file_description *f=fds[i];fds[i]=NULL;kernel_open_file_release(&f);}
    CHECK(!allocations && !registered);
}
#ifdef EPOLL_BASELINE
void kernel_epoll_abort_wait(struct kernel_epoll_wait_request *r) { (void)r; abort(); }
#endif
int main(int argc, char **argv)
{
    (void)argv;
    struct linux_epoll_event out[16];
    const unsigned modes[]={0,KERNEL_EPOLLET,KERNEL_EPOLLONESHOT};
    for(unsigned i=0;i<3 && argc==1;i++) {
        setup(modes[i]);copy_remaining=8;
        CHECK(wait_events(out,16)==-KERNEL_EFAULT);copy_remaining=SIZE_MAX;
        CHECK(wait_events(out,16)==1 && out[0].data==42);teardown();
    }
    setup(0);poll_notification=1;
    CHECK(wait_events(out,16)==1);
    CHECK(wait_events(out,16)==1);readable[0]=0;
    CHECK(kernel_epoll_poll(epfile.epoll,KERNEL_POLLIN,NULL)==0);
    CHECK(wait_events(out,16)==0);teardown();
    if(argc>1) return 0;
    setup(KERNEL_EPOLLONESHOT);copy_action=1;
    CHECK(wait_events(out,16)==1 && out[0].data==42);
    CHECK(wait_events(out,16)==1 && out[0].data==77);
    CHECK(wait_events(out,16)==0);teardown();
    setup(KERNEL_EPOLLET);copy_action=2;
    CHECK(wait_events(out,16)==1 && wait_events(out,16)==0);teardown();
    setup(KERNEL_EPOLLET);copy_action=4;
    CHECK(wait_events(out,16)==1 && targets[0].references==0 && wait_events(out,16)==0);teardown();
    setup(0);copy_action=5;
    CHECK(wait_events(out,16)==1 && wait_events(out,16)==1);teardown();
    setup(KERNEL_EPOLLET);copy_action=6;
    CHECK(wait_events(out,16)==1 && wait_events(out,16)==1 && wait_events(out,16)==0);teardown();
    setup(KERNEL_EPOLLONESHOT);copy_action=7;
    CHECK(wait_events(out,16)==1 && out[0].data==42);
    CHECK(wait_events(out,16)==1 && out[0].data==77);teardown();
    setup(KERNEL_EPOLLET);copy_action=8;
    CHECK(wait_events(out,16)==1 && out[0].data==42 && targets[0].references==0);
    CHECK(wait_events(out,16)==1 && out[0].data==88);teardown();
    setup(KERNEL_EPOLLET);copy_action=3;
    int64_t added;
    CHECK(kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_ADD,2,KERNEL_POLLIN|KERNEL_EPOLLET,43,&added)==KERNEL_FILES_STATUS_OK && !added);
    if(!setjmp(cancelled)) { wait_events(out,16); CHECK(0); }
    CHECK(!registered && epfile.references==1 && targets[0].references==1);
    CHECK(wait_events(out,16)==2);teardown();
    puts("PASS actual epoll: fault replay, reentrant notification, MOD, DEL, close pins and cancelled scan");
    return 0;
}
