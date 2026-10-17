/* Actual epoll implementation; only task, usercopy, OFD and wait boundaries are modeled. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define BOAROS_HOST_IRQ_STATE 1
#include <kernel/epoll.h>
#include <kernel/task.h>
#include <kernel/socket.h>
#ifndef EPOLL_SOURCE
#define EPOLL_SOURCE "../../fs/files/epoll.c"
#endif
#undef arch_interrupt_save
static uintptr_t epoll_test_irq_save(void);
#define arch_interrupt_save epoll_test_irq_save
#include EPOLL_SOURCE

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL epoll line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static struct kernel_heap heap;
static struct kernel_files files;
static struct kernel_open_file_description epfile, targets[2];
static struct kernel_wait_queue target_wait[2];
static struct kernel_open_file_description *fds[131];
static struct kernel_epoll_wait_request *requests[2];
static unsigned actor, actors[2];
_Thread_local uintptr_t network_host_irq = RISCV_SSTATUS_SIE;
static void (*irq_interleave)(void);
static uintptr_t epoll_test_irq_save(void)
{
    if(network_host_irq && irq_interleave) { void (*run)(void)=irq_interleave;irq_interleave=NULL;run(); }
    uintptr_t previous=network_host_irq;network_host_irq=0;return previous;
}
static unsigned preempt_depth, scopes[2], wait_live[2], protocol_cancel;
static unsigned prepares, parks, finishes, task_wakes, poll_action;
static void control(int operation,uint32_t mode,uint64_t data);
static void cancel_at_protocol_entry(void);
void kernel_preempt_disable(void) { preempt_depth++; }
void kernel_preempt_enable(void) { CHECK(preempt_depth);preempt_depth--; }
void kernel_socket_protocol_enter(struct kernel_socket_protocol_scope *scope)
{ CHECK(scopes[actor] || !wait_live[actor]); if(protocol_cancel) cancel_at_protocol_entry(); *scope=(struct kernel_socket_protocol_scope){.interrupts=network_host_irq}; scopes[actor]++;network_host_irq=RISCV_SSTATUS_SIE; }
void kernel_socket_protocol_leave(struct kernel_socket_protocol_scope *scope)
{ CHECK(scopes[actor]);scopes[actor]--;network_host_irq=scope->interrupts; }
#define registered requests[actor]
#define host_task ((struct kernel_task *)&actors[actor])
static struct kernel_mm *mm = (void *)&heap;
static size_t allocations, copy_remaining = SIZE_MAX;
static void (*allocation_interleave)(void);
static void (*poll_interleave)(void);
static unsigned poll_notification, copy_action, poll_calls, readable[2];
static jmp_buf cancelled;
static struct kernel_epoll *copy_poll_epoll;
static void cancel_at_protocol_entry(void)
{ CHECK(!scopes[actor]);protocol_cancel=0;CHECK(registered && registered->previous);kernel_epoll_abort_wait(registered);longjmp(cancelled,1); }
static unsigned notification_depth;
static uint64_t notification_sequence, callbacks;
uint64_t kernel_wait_notification_sequence(void)
{ return notification_depth ? notification_sequence : 0; }

int kernel_files_is_live(const struct kernel_files *f) { return f == &files; }
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t z,void **p)
{ (void)h; CHECK(!preempt_depth); if (allocation_interleave) { void (*run)(void)=allocation_interleave; allocation_interleave=NULL; run(); }
  *p=calloc(n,z); CHECK(*p); allocations++; return KERNEL_HEAP_STATUS_OK; }
void kernel_heap_release(struct kernel_heap *h,void *p)
{ (void)h; CHECK(p && allocations && !scopes[actor] && network_host_irq); allocations--; free(p); }
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
        CHECK(!scopes[actor] && !wait_live[actor] && network_host_irq);
        kernel_epoll_notify_file_release(f);
        if(f->kind==KERNEL_OPEN_FILE_KIND_EPOLL) CHECK(kernel_epoll_destroy(f->epoll)==KERNEL_FILES_STATUS_OK);
    }
    return KERNEL_OPEN_FILE_STATUS_OK;
}
enum kernel_files_status kernel_files_pin_data(struct kernel_files *f,int64_t fd,
    struct kernel_open_file_description **p,int64_t *result)
{
    CHECK(f==&files); *p=NULL; *result=-KERNEL_EBADF;
    if(fd>=0 && fd<(int64_t)(sizeof(fds)/sizeof(fds[0])) && fds[fd]) { *p=fds[fd]; kernel_open_file_acquire(*p); *result=0; }
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
enum kernel_scheduler_status kernel_wait_node_remove_sync(struct kernel_wait_node *node)
{ if (node->queue) kernel_wait_queue_remove(node); return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *q)
{
    if (!notification_depth++) notification_sequence++;
    for(struct kernel_wait_node *n=q->head;n;n=n->next) { if(n->callback) { callbacks++; n->callback(n,0); } else if(n->task) task_wakes++; }
    notification_depth--;
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_queue_close(struct kernel_wait_queue *q)
{ CHECK(q->initialized);q->closed=1;return kernel_wait_queue_wake_all(q); }
enum kernel_scheduler_status kernel_wait_queue_destroy(struct kernel_wait_queue *q)
{ CHECK(q->closed && !q->head && !q->tail && !q->borrows);*q=(struct kernel_wait_queue){0};return KERNEL_SCHEDULER_STATUS_OK; }
uint32_t kernel_open_file_poll(struct kernel_open_file_description *f,uint32_t requested,struct kernel_wait_queue **q)
{
    if (f->kind == KERNEL_OPEN_FILE_KIND_EPOLL) return kernel_epoll_poll(f->epoll, requested, q);
    CHECK(f>=targets && f<targets+2); unsigned i=(unsigned)(f-targets);
#ifndef EPOLL_OLD_SOURCE
    CHECK(!preempt_depth && network_host_irq && !wait_live[actor]);
#endif
    if(poll_interleave) { void (*run)(void)=poll_interleave;poll_interleave=NULL;run(); }
    CHECK(++poll_calls < 1000000); /* 单项非阻塞扫描必须结束，不能在失去就绪后自旋。 */
    struct kernel_socket_protocol_scope scope;
    int socket=f->kind==KERNEL_OPEN_FILE_KIND_SOCKET;
    if(socket) kernel_socket_protocol_enter(&scope);
    if(q)*q=&target_wait[i];
    unsigned action=poll_action;poll_action=0;
    if(action==1) control(KERNEL_EPOLL_CTL_MOD,KERNEL_EPOLLONESHOT,77);
    if(action==2) control(KERNEL_EPOLL_CTL_DEL,0,0);
    if(action==3) { struct kernel_open_file_description *closing=fds[1];fds[1]=NULL;kernel_open_file_release(&closing); }
    if(poll_notification) { poll_notification=0;kernel_wait_queue_wake_all(&target_wait[i]); }
    uint32_t result=readable[i] ? requested & KERNEL_POLLIN : 0;
    if(socket) { CHECK(scopes[actor]);kernel_socket_protocol_leave(&scope); }
    return result;
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
enum kernel_scheduler_status kernel_wait_prepare(struct kernel_wait_queue *q,uint64_t d,int i,struct kernel_wait_token *t)
{ (void)q;(void)d;(void)i;CHECK(!scopes[actor] && !wait_live[actor] && !network_host_irq);prepares++;wait_live[actor]=1;*t=(struct kernel_wait_token){host_task,1};return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_scheduler_status kernel_wait_park(const struct kernel_wait_token *t,enum kernel_wait_wake_reason *r)
{ (void)t;CHECK(!scopes[actor] && wait_live[actor] && !network_host_irq);parks++;*r=KERNEL_WAIT_TIMEOUT;return KERNEL_SCHEDULER_STATUS_OK; }
enum kernel_scheduler_status kernel_wait_finish(struct kernel_wait_token *t)
{ CHECK(wait_live[actor] && !network_host_irq);finishes++;wait_live[actor]=0;*t=(struct kernel_wait_token){0};return KERNEL_SCHEDULER_STATUS_OK; }
struct kernel_task *kernel_task_current(void) { return host_task; }
enum kernel_task_status kernel_task_epoll_register(struct kernel_task *t,struct kernel_epoll_wait_request *r,struct kernel_epoll_wait_request **previous)
{ CHECK(t==host_task);*previous=registered;registered=r;return KERNEL_TASK_STATUS_OK; }
enum kernel_task_status kernel_task_epoll_clear(struct kernel_task *t,struct kernel_epoll_wait_request *r,struct kernel_epoll_wait_request *previous)
{ CHECK(t==host_task && registered==r);registered=previous;return KERNEL_TASK_STATUS_OK; }
static void control(int operation,uint32_t mode,uint64_t data)
{
    int64_t result;
    CHECK(kernel_files_epoll_ctl(&files,0,operation,1,KERNEL_EPOLLIN|mode,data,&result)==KERNEL_FILES_STATUS_OK && !result);
}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *m,void *out,uint64_t in,size_t n,size_t *copied)
{ (void)m;memcpy(out,(void *)(uintptr_t)in,n);*copied=n;return KERNEL_UACCESS_STATUS_OK; }
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *m,uint64_t out,const void *in,size_t n,size_t *copied)
{
    (void)m; CHECK(!scopes[actor] && !preempt_depth && network_host_irq); unsigned action=copy_action;copy_action=0;
    if(action==1) control(KERNEL_EPOLL_CTL_MOD,KERNEL_EPOLLONESHOT,77);
    if(action==2) control(KERNEL_EPOLL_CTL_DEL,0,0);
    if(action==3) { kernel_epoll_abort_wait(registered); longjmp(cancelled,1); }
    if(action==4 || action==8) {
        struct kernel_open_file_description *f=fds[1];fds[1]=NULL;kernel_open_file_release(&f);
        if(action==8) { fds[1]=&targets[1];kernel_open_file_acquire(fds[1]);control(KERNEL_EPOLL_CTL_ADD,KERNEL_EPOLLONESHOT,88); }
    }
    if(action==5) {
        CHECK(!kernel_epoll_wait_pending(&epfile,NULL));
        struct linux_epoll_event event;int64_t result;
        actor=1;
        CHECK(kernel_files_epoll_pwait(&files,mm,host_task,0,(uintptr_t)&event,1,0,0,0,&result)==KERNEL_FILES_STATUS_OK && result==0);
        CHECK(!registered);actor=0;
    }
    if(action==6) kernel_wait_queue_wake_all(&target_wait[0]);
    if(action==7) { control(KERNEL_EPOLL_CTL_DEL,0,0);control(KERNEL_EPOLL_CTL_ADD,KERNEL_EPOLLONESHOT,77); }
    if(action==10) { protocol_cancel=1;kernel_epoll_poll(copy_poll_epoll,KERNEL_POLLIN,NULL);CHECK(0); }
    if(action==9) {
        /* 本次只请求一个事件，关闭尚未选中的fd必须立即归还其注册对象。 */
        size_t before=allocations;
        struct kernel_open_file_description *f=fds[2];fds[2]=NULL;
        kernel_open_file_release(&f);
        CHECK(allocations+1==before && targets[1].references==0);
    }
    *copied=n<copy_remaining?n:copy_remaining;memcpy((void *)(uintptr_t)out,in,*copied);
    if(copy_remaining!=SIZE_MAX)copy_remaining-=*copied;
    return *copied==n?KERNEL_UACCESS_STATUS_OK:KERNEL_UACCESS_STATUS_FAULT;
}
static int64_t wait_events(struct linux_epoll_event *out,int maximum)
{
    int64_t result;
    CHECK(kernel_files_epoll_pwait(&files,mm,host_task,0,(uintptr_t)out,maximum,0,0,0,&result)==KERNEL_FILES_STATUS_OK);
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
    CHECK(!allocations && !registered && !preempt_depth && !scopes[actor] && !wait_live[actor]);
}
#ifdef EPOLL_BASELINE
void kernel_epoll_abort_wait(struct kernel_epoll_wait_request *r) { (void)r; abort(); }
#endif
static int64_t interleaved_result;
static unsigned allocation_case;
static void add_during_allocation(void)
{
    CHECK(!kernel_files_epoll_ctl(&files, allocation_case ? 1 : 0,
        KERNEL_EPOLL_CTL_ADD, allocation_case ? 0 : 1, KERNEL_EPOLLIN, 7,
        &interleaved_result));
}
static void allocation_races(unsigned first)
{
    for (allocation_case=first; allocation_case<2; allocation_case++) {
        CHECK(!allocations); files.heap=&heap;
        struct kernel_open_file_description pair[2];
        for(unsigned i=0;i<2;i++) {
            pair[i]=(struct kernel_open_file_description){.kind=KERNEL_OPEN_FILE_KIND_EPOLL,.references=1};
            CHECK(!kernel_epoll_create(&heap,&pair[i].epoll));
            pair[i].epoll->file=&pair[i]; fds[i]=&pair[i];
        }
        allocation_interleave=add_during_allocation;
        int64_t result;
        CHECK(!kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_ADD,1,KERNEL_EPOLLIN,8,&result));
        CHECK(!interleaved_result);
        CHECK(result==(allocation_case ? -KERNEL_ELOOP : -KERNEL_EEXIST));
        for(unsigned i=0;i<2;i++) { struct kernel_open_file_description *f=fds[i];fds[i]=NULL;CHECK(!kernel_open_file_release(&f)); }
        CHECK(!allocations);
    }
    puts("PASS epoll allocation interleavings: duplicate key and reverse graph edge");
}
static void delete_at_irq_boundary(void) { control(KERNEL_EPOLL_CTL_DEL,0,0); }
static void competing_delete(void)
{
    setup(KERNEL_EPOLLET);
    struct kernel_epoll_item *item=epoll_find_item(epfile.epoll,1,&targets[0]);
    CHECK(item);item->references++; /* A的实际DEL查找借用，B在开始解绑前取得CPU。 */
    irq_interleave=delete_at_irq_boundary;
    CHECK(!epoll_item_unlink_and_destroy(item));
    epoll_item_put(item);
    CHECK(!epfile.epoll->items_root && allocations==1);
    teardown();
    puts("PASS epoll competing DEL: registration owner consumed once across IRQ boundary");
}
static void indexed_registrations(void)
{
    setup(KERNEL_EPOLLET);
    /* 同一OFD可由多个fd登记；fd重用后旧key直到最后一个dup关闭才消失。 */
    fds[3]=&targets[0];kernel_open_file_acquire(fds[3]);
    struct kernel_open_file_description *old=fds[1];fds[1]=NULL;kernel_open_file_release(&old);
    fds[1]=&targets[1];kernel_open_file_acquire(fds[1]);
    int64_t result;
    CHECK(!kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_ADD,1,KERNEL_EPOLLIN|KERNEL_EPOLLET,99,&result) && !result);
    CHECK(!kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_DEL,1,0,0,&result) && !result);
    struct linux_epoll_event out[128];
    CHECK(wait_events(out,128)==1 && out[0].data==42);
    old=fds[3];fds[3]=NULL;kernel_open_file_release(&old);
    CHECK(targets[0].references==0);
    for(unsigned i=3;i<131;i++) {
        fds[i]=&targets[1];kernel_open_file_acquire(fds[i]);
        CHECK(!kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_ADD,i,KERNEL_EPOLLIN|KERNEL_EPOLLET,i,&result) && !result);
    }
    unsigned order[128];for(unsigned i=0;i<128;i++)order[i]=i+3;
    unsigned state=17;
    for(unsigned i=127;i;i--) {state=state*1664525U+1013904223U;unsigned j=state%(i+1);unsigned swap=order[i];order[i]=order[j];order[j]=swap;}
    for(unsigned i=0;i<128;i++) {
        unsigned fd=order[i];
        CHECK(!kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_ADD,fd,KERNEL_EPOLLIN,0,&result) && result==-KERNEL_EEXIST);
        CHECK(!kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_MOD,fd,KERNEL_EPOLLIN|KERNEL_EPOLLET,fd+1000,&result) && !result);
        if(i%2==0) CHECK(!kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_DEL,fd,0,0,&result) && !result);
    }
    CHECK(wait_events(out,128)==64);
    unsigned seen[131]={0};for(unsigned i=0;i<64;i++){CHECK(out[i].data>=1003 && out[i].data<=1130);seen[out[i].data-1000]++;}
    for(unsigned i=0;i<128;i++) {
        unsigned fd=order[i];CHECK(seen[fd]==i%2);
        if(i%2) CHECK(!kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_DEL,fd,0,0,&result) && !result);
        struct kernel_open_file_description *f=fds[fd];fds[fd]=NULL;kernel_open_file_release(&f);
    }
    CHECK(!wait_events(out,128));teardown();
    puts("PASS epoll indexed registrations: dup, fd reuse, 128 shuffled MOD/DEL keys");
}
static void nested_poll_cancellation(void)
{
    setup(KERNEL_EPOLLET);
    struct kernel_open_file_description child={.kind=KERNEL_OPEN_FILE_KIND_EPOLL,.references=1};
    CHECK(!kernel_epoll_create(&heap,&child.epoll));child.epoll->file=&child;fds[3]=&child;
    targets[1].kind=KERNEL_OPEN_FILE_KIND_SOCKET;
    int64_t result;
    CHECK(!kernel_files_epoll_ctl(&files,3,KERNEL_EPOLL_CTL_ADD,2,KERNEL_EPOLLIN|KERNEL_EPOLLET,13,&result) && !result);
    copy_poll_epoll=child.epoll;copy_action=10;
    struct linux_epoll_event out;
    if(!setjmp(cancelled)) { wait_events(&out,1);CHECK(0); }
    CHECK(!registered && !scopes[actor] && epfile.references==1 && child.references==1);
    CHECK(targets[0].references==1 && targets[1].references==1);
    CHECK(wait_events(&out,1)==1 && out.data==42);
    struct kernel_open_file_description *f=fds[3];fds[3]=NULL;kernel_open_file_release(&f);
    teardown();
    puts("PASS epoll nested pure poll cancellation: LIFO request pins and both scans returned");
}
static void nested_query_semantics(void)
{
    const uint32_t modes[]={KERNEL_EPOLLET,KERNEL_EPOLLONESHOT};
    for(unsigned mode=0;mode<2;mode++) {
        setup(KERNEL_EPOLLET);control(KERNEL_EPOLL_CTL_DEL,0,0);
        struct kernel_open_file_description child={.kind=KERNEL_OPEN_FILE_KIND_EPOLL,.references=1};
        CHECK(!kernel_epoll_create(&heap,&child.epoll));child.epoll->file=&child;fds[3]=&child;
        int64_t result;
        CHECK(!kernel_files_epoll_ctl(&files,3,KERNEL_EPOLL_CTL_ADD,1,KERNEL_EPOLLIN|modes[mode],13,&result) && !result);
        CHECK(!kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_ADD,3,KERNEL_EPOLLIN|KERNEL_EPOLLET,17,&result) && !result);
        struct linux_epoll_event out;
        CHECK(wait_events(&out,1)==1 && out.data==17);
        callbacks=0;
        struct kernel_wait_queue *queue;
        CHECK(kernel_epoll_poll(child.epoll,KERNEL_POLLIN,&queue)==KERNEL_POLLIN);
        struct kernel_wait_node waiter={.task=host_task};kernel_wait_queue_add(queue,&waiter);
        unsigned wakes=task_wakes;
        for(unsigned i=0;i<3;i++) CHECK(kernel_epoll_poll(child.epoll,KERNEL_POLLIN,NULL)==KERNEL_POLLIN);
        CHECK(task_wakes==wakes && !callbacks && !wait_events(&out,1));
        kernel_wait_queue_remove(&waiter);
        CHECK(!kernel_files_epoll_pwait(&files,mm,host_task,3,(uintptr_t)&out,1,0,0,0,&result) && result==1 && out.data==13);
        CHECK(!kernel_epoll_poll(child.epoll,KERNEL_POLLIN,NULL));
        struct kernel_open_file_description *f=fds[3];fds[3]=NULL;kernel_open_file_release(&f);
        teardown();
    }
    setup(KERNEL_EPOLLET);
    unsigned queries=poll_calls;
    uintptr_t saved=arch_interrupt_save();struct kernel_wait_token token;
    CHECK(!kernel_wait_prepare(NULL,0,1,&token));
    struct kernel_wait_queue *queue;
    CHECK(kernel_epoll_wait_pending(&epfile,&queue) && queue==&epfile.epoll->wait_queue && poll_calls==queries);
    CHECK(!kernel_wait_finish(&token));arch_interrupt_restore(saved);
    teardown();
    setup(KERNEL_EPOLLET);readable[0]=0;poll_notification=1;poll_calls=0;
    CHECK(!kernel_epoll_poll(epfile.epoll,KERNEL_POLLIN,NULL) && poll_calls==1);
    CHECK(!kernel_epoll_poll(epfile.epoll,KERNEL_POLLIN,NULL) && poll_calls==2);
    CHECK(!kernel_epoll_poll(epfile.epoll,KERNEL_POLLIN,NULL) && poll_calls==2);
    teardown();
    puts("PASS epoll pure queries: ET/ONESHOT preserved, no synthetic parent edge, generation retry");
}
static struct kernel_open_file_description *overlap_child;
static unsigned overlap_mode, overlap_ran;
static void observe_during_query(void)
{
    overlap_ran++;
    int64_t result;
    if(overlap_mode==2) {
        /* 修改当前图时，A仍借用已经摘下的父→子候选。 */
        CHECK(!kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_DEL,3,0,0,&result) && !result);
        CHECK(!kernel_files_epoll_ctl(&files,3,KERNEL_EPOLL_CTL_ADD,0,KERNEL_EPOLLIN,19,&result) && !result);
        readable[0]=0;
    }
    actor=1;
    uint32_t ready=kernel_epoll_poll(overlap_mode==2?overlap_child->epoll:epfile.epoll,KERNEL_POLLIN,NULL);
    CHECK(ready==(overlap_mode==2?0:KERNEL_POLLIN));
    CHECK(!registered);actor=0;
}
static void overlapping_observers(void)
{
    for(overlap_mode=0;overlap_mode<3;overlap_mode++) {
        setup(KERNEL_EPOLLET);control(KERNEL_EPOLL_CTL_DEL,0,0);
        struct kernel_open_file_description child={.kind=KERNEL_OPEN_FILE_KIND_EPOLL,.references=1};
        CHECK(!kernel_epoll_create(&heap,&child.epoll));child.epoll->file=&child;fds[3]=&child;
        int64_t result;
        CHECK(!kernel_files_epoll_ctl(&files,3,KERNEL_EPOLL_CTL_ADD,1,KERNEL_EPOLLIN|KERNEL_EPOLLET,13,&result) && !result);
        CHECK(!kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_ADD,3,KERNEL_EPOLLIN|KERNEL_EPOLLET,17,&result) && !result);
        overlap_child=&child;overlap_ran=0;poll_interleave=observe_during_query;
        if(overlap_mode==1) {
            struct linux_epoll_event event;
            CHECK(!kernel_files_epoll_pwait(&files,mm,host_task,3,(uintptr_t)&event,1,0,0,0,&result) && result==1);
        } else CHECK(kernel_epoll_poll(epfile.epoll,KERNEL_POLLIN,NULL)==(overlap_mode==2?0:KERNEL_POLLIN));
        CHECK(overlap_ran==1 && !registered && !requests[1]);
        if(overlap_mode==0) CHECK(kernel_epoll_poll(epfile.epoll,KERNEL_POLLIN,NULL)==KERNEL_POLLIN);
        struct kernel_open_file_description *f=fds[3];fds[3]=NULL;kernel_open_file_release(&f);teardown();
    }
    /* ready顺序100→2，不能把链头key作为整个AVL已经访问的下界。 */
    setup(KERNEL_EPOLLET);control(KERNEL_EPOLL_CTL_DEL,0,0);
    fds[100]=&targets[0];kernel_open_file_acquire(fds[100]);int64_t result;
    CHECK(!kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_ADD,100,KERNEL_POLLIN,100,&result) && !result);
    CHECK(!kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_ADD,2,KERNEL_POLLIN,2,&result) && !result);
    readable[0]=0;poll_notification=1;poll_calls=0;
    CHECK(kernel_epoll_poll(epfile.epoll,KERNEL_POLLIN,NULL)==KERNEL_POLLIN && poll_calls==2);
    struct kernel_open_file_description *f=fds[100];fds[100]=NULL;kernel_open_file_release(&f);teardown();
    puts("PASS epoll observers: overlapping pure/delivery queries, detached edge reversal and unsorted generation retry");
}
static void sleeping_query_boundaries(void)
{
    struct linux_epoll_event out;
    setup(KERNEL_EPOLLET);poll_action=1;
    CHECK(wait_events(&out,1)==0);
    CHECK(wait_events(&out,1)==1 && out.data==77);
    CHECK(wait_events(&out,1)==0);teardown();
    setup(KERNEL_EPOLLET);poll_action=2;
    CHECK(wait_events(&out,1)==0);teardown();
    setup(KERNEL_EPOLLET);poll_action=3;
    CHECK(wait_events(&out,1)==1 && out.data==42 && targets[0].references==0);
    CHECK(wait_events(&out,1)==0);teardown();
    setup(KERNEL_EPOLLET);readable[0]=0;
    unsigned before_prepares=prepares,before_parks=parks,before_finishes=finishes;
    int64_t result;
    CHECK(!kernel_files_epoll_pwait(&files,mm,host_task,0,(uintptr_t)&out,1,1,0,0,&result) && !result);
    CHECK(prepares==before_prepares+1 && parks==before_parks+1 && finishes==before_finishes+1);
    CHECK(network_host_irq==RISCV_SSTATUS_SIE);
    teardown();
    puts("PASS epoll sleeping query boundaries: MOD, DEL, close and target owner/prepare/park/finish");
}
static void nested_notifications(void)
{
    const unsigned width = 32, layers = 4, count = width * layers;
    struct kernel_open_file_description nested[128];
    CHECK(!allocations);
    files.heap = &heap; readable[0] = 0;
    targets[0] = (struct kernel_open_file_description){.kind=KERNEL_OPEN_FILE_KIND_PIPE,.references=1};
    kernel_wait_queue_init(&target_wait[0]); fds[count] = &targets[0];
    unsigned registrations = 0;
    for (unsigned i = 0; i < count; i++) {
        nested[i] = (struct kernel_open_file_description){.kind=KERNEL_OPEN_FILE_KIND_EPOLL,.references=1};
        CHECK(!kernel_epoll_create(&heap, &nested[i].epoll));
        nested[i].epoll->file = &nested[i]; fds[i] = &nested[i];
        for (unsigned j = 0; j < (i < width ? 1 : width); j++) {
            unsigned target = i < width ? count : (i / width - 1) * width + j;
            int64_t result;
            poll_calls = 0;
            CHECK(!kernel_files_epoll_ctl(&files,i,KERNEL_EPOLL_CTL_ADD,target,
                KERNEL_EPOLLIN|KERNEL_EPOLLET,target,&result) && !result);
            registrations++;
        }
    }
    for (unsigned repeat = 0; repeat < 2; repeat++) {
        callbacks = 0;
        kernel_wait_queue_wake_all(&target_wait[0]);
        printf("epoll nested: %u objects, %u registrations, %llu callbacks\n", count, registrations,
            (unsigned long long)callbacks);
        CHECK(callbacks == registrations);
        for (unsigned i = 0; i < count; i++)
            CHECK(nested[i].references == 1 && nested[i].epoll->ready_head);
        callbacks = 0; poll_calls = 0;
        CHECK(!kernel_epoll_poll(nested[count-1].epoll,KERNEL_POLLIN,NULL));
        printf("epoll empty DAG: %u bottom queries, %llu refresh callbacks\n", poll_calls,
            (unsigned long long)callbacks);
        CHECK(poll_calls == width && !callbacks);
        unsigned previous=poll_calls;
        CHECK(!kernel_epoll_poll(nested[count-1].epoll,KERNEL_POLLIN,NULL) && poll_calls==previous);
    }
    for (unsigned i = count + 1; i; i--) {
        struct kernel_open_file_description *f = fds[i - 1]; fds[i - 1] = NULL;
        CHECK(!kernel_open_file_release(&f));
    }
    CHECK(!allocations && !target_wait[0].head);
}
int main(int argc, char **argv)
{
    if(argc>1 && !strcmp(argv[1],"query-overlap")) { overlapping_observers(); return 0; }
    if(argc>1 && !strcmp(argv[1],"dag")) { nested_notifications(); return 0; }
    if(argc>1 && (!strcmp(argv[1],"allocation") || !strcmp(argv[1],"cycle"))) { allocation_races(!strcmp(argv[1],"cycle")); return 0; }
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
    setup(KERNEL_EPOLLET);
    int64_t unrelated;
    CHECK(kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_ADD,2,KERNEL_POLLIN|KERNEL_EPOLLET,43,&unrelated)==KERNEL_FILES_STATUS_OK && !unrelated);
    copy_action=9;
    CHECK(wait_events(out,1)==1 && out[0].data==42);
    CHECK(wait_events(out,1)==0);teardown();
    setup(KERNEL_EPOLLET);
    CHECK(kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_ADD,2,KERNEL_POLLIN|KERNEL_EPOLLET,43,&unrelated)==KERNEL_FILES_STATUS_OK && !unrelated);
    readable[0]=0;
    CHECK(wait_events(out,1)==1 && out[0].data==43);
    CHECK(wait_events(out,1)==0);teardown();
    setup(KERNEL_EPOLLET);copy_action=3;
    int64_t added;
    CHECK(kernel_files_epoll_ctl(&files,0,KERNEL_EPOLL_CTL_ADD,2,KERNEL_POLLIN|KERNEL_EPOLLET,43,&added)==KERNEL_FILES_STATUS_OK && !added);
    if(!setjmp(cancelled)) { wait_events(out,16); CHECK(0); }
    CHECK(!registered && epfile.references==1 && targets[0].references==1);
    CHECK(wait_events(out,16)==2);teardown();
    allocation_races(0);
    competing_delete();
    indexed_registrations();
    nested_poll_cancellation();
    nested_query_semantics();
    overlapping_observers();
    sleeping_query_boundaries();
    nested_notifications();
    puts("PASS actual epoll: fault replay, reentrant notification, MOD, DEL, close pins, cancelled scan and nested notification coalescing");
    return 0;
}
