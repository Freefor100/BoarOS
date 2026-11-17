/* The fixture uses the real priority queue, namespace and ownership code. */
#include "../../kernel/mqueue.c"
#define IPC_FIXTURE_MQ
#include "ipc_namespace_fixture.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

_Thread_local uintptr_t sync_test_irq=2;
static _Thread_local struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED};
static struct kernel_task tasks[3];
static struct { struct kernel_pid id; struct kernel_pid_binding ancestor; } identities[3];
static struct kernel_pid_registry root,child={.parent=&root,.depth=1};
static int signal_sender;
static struct kernel_heap heap;
static struct kernel_files files={.heap=&heap};
static struct kernel_open_file_description *fds[16];
static struct kernel_uid_account account={.uid=10,.references=1};
static struct kernel_credentials credentials={.uid=10,.euid=10,.fsuid=10,.gid=20,.egid=20,.fsgid=20,.account=&account};
static unsigned allocations,waiting;
static int privileged,signal_count,signal_number,cookie_count,cookie_reason,cookie_cancelled;
static uint64_t signal_value;
static uint64_t now_ns=1000000;
static _Thread_local int forced_reason=-1,abandon_copy,abandon_cookie,abandon_wait;
static _Thread_local int fail_at=-1;
static _Thread_local unsigned allocation_calls;
static uint64_t output_fault;
static pthread_barrier_t abandoned,abandon_release;
static void abandon_pause(void) __attribute__((noreturn));
static void abandon_pause(void)
{
    int n=pthread_barrier_wait(&abandoned);assert(!n||n==PTHREAD_BARRIER_SERIAL_THREAD);
    n=pthread_barrier_wait(&abandon_release);assert(!n||n==PTHREAD_BARRIER_SERIAL_THREAD);
    pthread_exit(0);
}
void *sync_test_cpu(void){return &cpu;}
void *sync_test_current_task(void){return cpu.current;}
struct kernel_task *kernel_task_current(void){return cpu.current;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
void kernel_assert_can_block(void){assert(!cpu.raw_locks);}
void kernel_preempt_disable(void){assert(!cpu.raw_locks);cpu.preempt_depth++;}
void kernel_preempt_enable(void){assert(!cpu.raw_locks&&cpu.preempt_depth);cpu.preempt_depth--;}
const struct kernel_credentials *kernel_credentials_current(void){return &credentials;}
const struct kernel_credentials *kernel_credentials_get(const struct kernel_credentials *c){return c;}
void kernel_credentials_put(const struct kernel_credentials *c){(void)c;}
int kernel_credentials_capable(const struct kernel_credentials *c,unsigned capability)
{(void)c;(void)capability;return privileged;}
int kernel_credentials_in_group(const struct kernel_credentials *c,uint32_t group){return c->egid==group;}
int kernel_vfs_dac_permission(uint32_t mode,uint32_t uid,uint32_t gid,unsigned access,const struct kernel_credentials *c)
{unsigned shift=c->fsuid==uid?6:c->fsgid==gid?3:0;return privileged||((mode>>shift)&access)==access?0:-KERNEL_EACCES;}
int kernel_vfs_chmod_allowed(uint32_t uid,uint32_t gid,uint32_t *mode)
{(void)gid;(void)mode;return uid==credentials.fsuid||privileged?0:-KERNEL_EPERM;}
int kernel_vfs_chown_allowed(uint32_t uid,uint32_t gid,uint32_t mode,uint32_t u,uint32_t g)
{(void)uid;(void)gid;(void)mode;(void)u;(void)g;return privileged?0:-KERNEL_EPERM;}
uint32_t kernel_vfs_chown_mode(uint32_t mode,uint32_t old,uint32_t new)
{(void)old;(void)new;return mode;}
int kernel_vfs_close(struct kernel_vfs_file *file){(void)file;assert(0);return -KERNEL_EIO;}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t z,void **p)
{
    (void)h;assert(!cpu.raw_locks);
    if((int)allocation_calls++==fail_at)return KERNEL_HEAP_STATUS_EMPTY;
    *p=calloc(n,z);assert(*p);__atomic_fetch_add(&allocations,1,__ATOMIC_RELAXED);return 0;
}
void kernel_heap_release(struct kernel_heap *h,void *p)
{(void)h;assert(p&&!cpu.raw_locks);__atomic_fetch_sub(&allocations,1,__ATOMIC_RELAXED);free(p);}
void kernel_rwlock_init(struct kernel_rwlock *lock,uint32_t rank,uintptr_t key)
{(void)lock;(void)rank;(void)key;}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *mm,void *dest,uint64_t source,size_t size,size_t *copied)
{
    (void)mm;assert(!cpu.raw_locks);
    if(abandon_copy)abandon_pause();
    if(source<4096)return KERNEL_UACCESS_STATUS_FAULT;
    memcpy(dest,(void *)(uintptr_t)source,size);*copied=size;return 0;
}
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *mm,uint64_t dest,const void *source,size_t size,size_t *copied)
{
    if(dest==output_fault||dest<4096)return KERNEL_UACCESS_STATUS_FAULT;
    return kernel_copy_from_user(mm,(void *)(uintptr_t)dest,(uintptr_t)source,size,copied);
}
uint64_t kernel_time_realtime_ns(void){return now_ns;}
uint64_t kernel_time_boot_realtime_offset(void){return 0;}
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns,uint64_t *deadline)
{*deadline=ns;return ns<=now_ns?KERNEL_TIME_STATUS_DEADLINE_PASSED:0;}
enum kernel_time_status kernel_time_deadline_from_realtime(uint64_t ns,uint64_t *deadline)
{*deadline=ns<=now_ns?now_ns+1:ns;return ns<=now_ns?KERNEL_TIME_STATUS_DEADLINE_PASSED:0;}
enum kernel_task_status kernel_task_get_rlimit(const struct kernel_task *task,uint32_t resource,struct kernel_rlimit64 *limit)
{(void)task;assert(resource==KERNEL_RLIMIT_MSGQUEUE);*limit=(struct kernel_rlimit64){819200,819200};return 0;}
int kernel_uid_account_mqueue_charge(struct kernel_uid_account *a,uint64_t bytes,uint64_t limit)
{
    KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
    if(bytes>limit||a->mqueue_bytes>limit-bytes)return -KERNEL_EMFILE;
    a->mqueue_bytes+=bytes;a->references++;return 0;
}
void kernel_uid_account_mqueue_uncharge(struct kernel_uid_account *a,uint64_t bytes)
{KERNEL_RAW_SCOPE(g,&kernel_wait_domain);assert(a->mqueue_bytes>=bytes&&a->references>1);a->mqueue_bytes-=bytes;a->references--;}
enum kernel_task_status kernel_task_files_borrow(struct kernel_task *task,struct kernel_files **out)
{(void)task;*out=&files;return 0;}
kernel_pid_t kernel_task_identity_user_number(const struct kernel_pid *id)
{return kernel_pid_number_in(id,cpu.current->group_leader->identities[KERNEL_PID_TGID].identity->registry); }
struct kernel_open_file_description *kernel_files_hold_fd(struct kernel_files *f,int64_t fd,struct kernel_files_pin_guard *guard)
{
    if(fd<0||fd>=16||!fds[fd])return 0;
    struct kernel_open_file_description *file=fds[fd];__atomic_fetch_add(&file->references,1,__ATOMIC_RELAXED);
    *guard=(struct kernel_files_pin_guard){f,file};return file;
}
enum kernel_open_file_status kernel_open_file_release(struct kernel_open_file_description **owner)
{
    struct kernel_open_file_description *f=*owner;if(!f)return 0;*owner=0;
    if(!__atomic_sub_fetch(&f->references,1,__ATOMIC_ACQ_REL)){assert(!kernel_mqueue_close(f));kernel_heap_release(f->heap,f);}
    return 0;
}
void kernel_task_identity_put(struct kernel_pid *id)
{KERNEL_RAW_SCOPE(g,&kernel_wait_domain);assert(id->references>1);id->references--;}
struct kernel_exec_security *kernel_exec_security_get(struct kernel_exec_security *s){return s;}
void kernel_exec_security_put(struct kernel_exec_security *s){(void)s;}
void kernel_signal_mqueue_notify(struct kernel_pid *id,struct kernel_exec_security *exec,int signo,uint64_t value,int32_t sender,uint32_t uid)
{(void)id;(void)exec;assert(uid==10);signal_sender=sender;signal_count++;signal_number=signo;signal_value=value;}
int kernel_socket_mqueue_cookie_prepare(struct kernel_task *task,int64_t fd,const unsigned char cookie[32],struct kernel_netlink_cookie **out)
{(void)task;assert(fd==9&&cookie[0]==42);*out=(void *)(uintptr_t)1;if(abandon_cookie)abandon_pause();return 0;}
void kernel_socket_mqueue_cookie_send(struct kernel_netlink_cookie **owner,unsigned reason)
{assert(*owner);*owner=0;cookie_count++;cookie_reason=(int)reason;}
void kernel_socket_mqueue_cookie_cancel(struct kernel_netlink_cookie **owner){if(*owner)cookie_cancelled++;*owner=0;}
struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *t){return &t->wait;}
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *t){return &t->default_wait_node;}
struct kernel_task *kernel_wait_current_task(void){return cpu.current;}
int kernel_wait_backend_initialized(void){return 1;}
int kernel_signal_has_pending(const struct kernel_task *t){return t->signal_pending!=0;}
int kernel_signal_termination_requested(const struct kernel_task *t){return t->terminate_requested!=0;}
int kernel_wait_backend_signal(struct kernel_task *t){(void)t;return 0;}
uint64_t kernel_wait_backend_time(void){return now_ns;}
void kernel_wait_backend_commit(struct kernel_task *t,uint64_t d,int i){(void)d;(void)i;t->wait.on_cpu=0;}
void kernel_wait_backend_notify(struct kernel_task *t,uint32_t reason){(void)t;(void)reason;}
void kernel_wait_backend_ready(struct kernel_task *t){(void)t;}
void kernel_wait_backend_quiesce(void){sched_yield();}
enum kernel_scheduler_status kernel_scheduler_yield_current(void){sched_yield();return 0;}
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *t)
{

    __atomic_fetch_or(&waiting,1U<<(unsigned)(t-tasks),__ATOMIC_RELEASE);
    if(abandon_wait){
        struct kernel_wait_token token={t,kernel_wait_record_generation(&t->wait)};
        assert(!kernel_wait_notify(&token,KERNEL_WAIT_SIGNALLED));assert(!kernel_wait_finish(&token));abandon_pause();
    }
    if(forced_reason>=0){struct kernel_wait_token token={t,kernel_wait_record_generation(&t->wait)};assert(!kernel_wait_notify(&token,(unsigned)forced_reason));}
    for(;;){
        {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
         if(kernel_wait_record_phase(&t->wait)==KERNEL_WAIT_NOTIFIED){t->wait.on_cpu=1;break;}}
        sched_yield();
    }
    return 0;
}
static void current(unsigned index){cpu.current=&tasks[index];}
static struct kernel_mqueue *make(const char *name,unsigned count,unsigned size)
{
    struct kernel_mqueue *q=0;int created=0;struct kernel_mq_attr a={.maxmsg=count,.msgsize=size};
    assert(!kernel_mqueue_create_name(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),cpu.current,name,0600,&a,1,&q,&created)&&created);return q;
}
static int descriptor(struct kernel_mqueue *q,unsigned flags)
{
    unsigned fd=0;while(fd<16&&fds[fd])fd++;assert(fd<16);
    assert(!file_create(&heap,q,flags,&fds[fd]));return (int)fd;
}
static void closefd(int fd)
{kernel_mqueue_close_fd(fds[fd],cpu.current);assert(!kernel_open_file_release(&fds[fd]));}
static int64_t sendq(int fd,const void *p,size_t n,unsigned priority)
{return kernel_mqueue_send(cpu.current,&cpu.current->mm,fd,(uintptr_t)p,n,priority,0);}
static int64_t recvq(int fd,void *p,size_t n,unsigned *priority)
{return kernel_mqueue_receive(cpu.current,&cpu.current->mm,fd,(uintptr_t)p,n,(uintptr_t)priority,0);}
static int notifyq(int fd,int type,int signal,uint64_t value)
{
    struct{uint64_t value;int32_t signal,type;unsigned char pad[48];} n={value,signal,type,{0}};
    return (int)kernel_mqueue_notify(cpu.current,&cpu.current->mm,fd,(uintptr_t)&n);
}
static void removeq(struct kernel_mqueue *q)
{assert(!kernel_mqueue_remove_name(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),q->name,strlen(q->name)));kernel_mqueue_put(q);}
static void priority_and_lifetime(void)
{
    privileged=1;struct kernel_mqueue *q=make("priority",4,9000);privileged=0;
    int fd=descriptor(q,2|MQ_NONBLOCK);char a[9000],b[9000];memset(a,37,sizeof(a));
    assert(!sendq(fd,a,sizeof(a),4));assert(!sendq(fd,"first",6,9));assert(!sendq(fd,"second",7,9));assert(!sendq(fd,"low",4,0));
    assert(sendq(fd,a,1,0)==-KERNEL_EAGAIN);
    unsigned p=0;assert(recvq(fd,b,sizeof(b),&p)==6&&p==9&&!strcmp(b,"first"));
    assert(recvq(fd,b,sizeof(b),&p)==7&&p==9&&!strcmp(b,"second"));
    assert(recvq(fd,b,sizeof(b),&p)==9000&&p==4&&!memcmp(a,b,sizeof(a)));
    assert(recvq(fd,b,sizeof(b),&p)==4&&p==0);assert(recvq(fd,b,sizeof(b),0)==-KERNEL_EAGAIN);
    uint64_t charge=q->charge;assert(charge==4*(9000+48)+4*48&&account.mqueue_bytes==charge);
    assert(!kernel_mqueue_remove_name(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),"priority",8));assert(!kernel_mqueue_lookup_name(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),"priority",8));
    assert(!sendq(fd,"unlinked",9,3));kernel_mqueue_put(q);
    assert(account.mqueue_bytes==charge);closefd(fd);assert(!account.mqueue_bytes);
}
static void notifications(void)
{
    struct kernel_mqueue *q=make("notify",2,32);int fd=descriptor(q,2|MQ_NONBLOCK),alias=descriptor(q,2);char out[32];
    assert(!notifyq(fd,0,35,UINT64_C(0xabcdef0123456789)));assert(notifyq(fd,1,0,0)==-KERNEL_EBUSY);
    assert(!sendq(fd,"a",2,0));assert(signal_count==1&&signal_number==35&&signal_value==UINT64_C(0xabcdef0123456789));
    assert(!sendq(fd,"b",2,0));assert(signal_count==1);assert(recvq(fd,out,32,0)==2);assert(recvq(fd,out,32,0)==2);
    unsigned char cookie[32]={42};assert(!notifyq(fd,2,9,(uintptr_t)cookie));
    current(1);closefd(alias);assert(!cookie_count);current(0);
    alias=descriptor(q,2);closefd(alias);assert(cookie_count==1&&cookie_reason==2);
    assert(!notifyq(fd,2,9,(uintptr_t)cookie));assert(!sendq(fd,"c",2,0));assert(cookie_count==2&&cookie_reason==1);
    assert(recvq(fd,out,32,0)==2);assert(!notifyq(fd,1,0,0));assert(!sendq(fd,"d",2,0));assert(!q->notification.owner);
    assert(recvq(fd,out,32,0)==2);
    tasks[2].group_leader=&tasks[0];tasks[2].identities[KERNEL_PID_TGID].identity=0;
    current(2);assert(!notifyq(fd,0,35,91)&&q->notification.owner==&identities[0].id);
    alias=descriptor(q,2);closefd(alias);assert(!q->notification.owner);current(0);
    char status[128];size_t n;
    current(1);assert(!notifyq(fd,0,35,91));
    assert(!kernel_mqueue_queue_read(q,0,status,sizeof(status)-1,&n));status[n]=0;
    assert(strstr(status,"NOTIFY_PID:1 "));
    current(2);assert(!kernel_mqueue_queue_read(q,0,status,sizeof(status)-1,&n));status[n]=0;
    assert(strstr(status,"NOTIFY_PID:42 "));
    assert(!sendq(fd,"e",2,0)&&signal_sender==0&&recvq(fd,out,32,0)==2);
    assert(!notifyq(fd,0,35,91));current(1);
    assert(!kernel_mqueue_queue_read(q,0,status,sizeof(status)-1,&n));status[n]=0;
    assert(strstr(status,"NOTIFY_PID:0 "));
    assert(!sendq(fd,"f",2,0)&&signal_sender==42&&recvq(fd,out,32,0)==2);
    current(0);
    closefd(fd);removeq(q);
}
struct worker {int fd,send;int64_t result;char text[32];};
static void *run_worker(void *data)
{
    struct worker *w=data;current(1);
    w->result=w->send?sendq(w->fd,w->text,strlen(w->text)+1,2):recvq(w->fd,w->text,sizeof(w->text),0);
    return 0;
}
static void await_waiter(void)
{while(!(__atomic_load_n(&waiting,__ATOMIC_ACQUIRE)&2U))sched_yield();}
static unsigned queue_callbacks;
static void queue_changed(struct kernel_wait_node *node,uint32_t reason,uint64_t event)
{ (void)event;(void)node;(void)reason;queue_callbacks++;}
static void handoff_and_timeout(void)
{
    struct kernel_mqueue *q=make("handoff",1,32);int fd=descriptor(q,2);pthread_t thread;
    struct kernel_wait_node poll_node;queue_callbacks=0;int prior=signal_count;
    kernel_wait_node_init_callback(&poll_node,queue_changed,0);
    kernel_wait_queue_add(&q->waiters,&poll_node);
    struct worker w={.fd=fd};waiting=0;assert(!pthread_create(&thread,0,run_worker,&w));await_waiter();
    assert(!notifyq(fd,0,36,44));
    /* One available allocation must hold a short payload delivered to an
     * existing receiver; no priority bucket or second payload owner is needed. */
    allocation_calls=0;fail_at=1;
    assert(!sendq(fd,"direct",7,1));fail_at=-1;
    assert(!pthread_join(thread,0));
    assert(w.result==7&&!strcmp(w.text,"direct")&&signal_count==prior&&q->notification.owner);
    assert(queue_callbacks==0);
    assert(!sendq(fd,"fill",5,0));assert(signal_count==prior+1&&queue_callbacks==1);
    assert(!notifyq(fd,0,35,77));
    w=(struct worker){.fd=fd,.send=1,.text="replacement"};waiting=0;
    assert(!pthread_create(&thread,0,run_worker,&w));await_waiter();char out[32];
    assert(recvq(fd,out,32,0)==5&&!strcmp(out,"fill"));assert(!pthread_join(thread,0));assert(!w.result&&queue_callbacks==1);
    assert(recvq(fd,out,32,0)==12&&!strcmp(out,"replacement"));
    assert(q->notification.owner&&signal_count==prior+1);
    struct{int64_t sec,nsec;} expired={0,0};
    assert(kernel_mqueue_receive(cpu.current,&cpu.current->mm,fd,(uintptr_t)out,32,0,(uintptr_t)&expired)==-KERNEL_ETIMEDOUT);
    forced_reason=KERNEL_WAIT_SIGNALLED;assert(recvq(fd,out,32,0)==-KERNEL_ERESTARTSYS);forced_reason=-1;
    assert(!kernel_wait_node_remove_sync(&poll_node));
    closefd(fd);removeq(q);
}
static void fault_and_quota(void)
{
    struct kernel_mqueue *q=make("fault",1,32);int fd=descriptor(q,2|MQ_NONBLOCK);char out[32];
    assert(sendq(fd,(void *)(uintptr_t)1,4,0)==-KERNEL_EFAULT);assert(!q->count);
    assert(!sendq(fd,"lost",5,0));output_fault=(uintptr_t)out;
    assert(recvq(fd,out,32,0)==-KERNEL_EFAULT&&!q->count);output_fault=0;
    struct kernel_mqueue *large=0;int made;struct kernel_mq_attr a={.maxmsg=65536,.msgsize=16777216};
    privileged=1;assert(kernel_mqueue_create_name(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),cpu.current,"overquota",0600,&a,1,&large,&made)==-KERNEL_EMFILE);privileged=0;
    assert(!large&&account.references==2);closefd(fd);removeq(q);
}
static void namespace_metadata(void)
{
    struct kernel_mqueue *q=make("root",1,32),*opened=0;int made=0;
    privileged=1;assert(!kernel_mqueue_root_set_mode(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),0000));privileged=0;
    assert(!kernel_mqueue_create_name(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),cpu.current,"root",0600,0,0,&opened,&made)&&!made);
    kernel_mqueue_put(opened);
    struct kernel_mq_attr attr={.maxmsg=1,.msgsize=32};
    assert(kernel_mqueue_create_name(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),cpu.current,"denied",0600,&attr,1,&opened,&made)==-KERNEL_EACCES);
    assert(kernel_mqueue_remove_name(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),"root",4)==-KERNEL_EACCES);
    privileged=1;assert(!kernel_mqueue_root_set_mode(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),03777));assert(!kernel_mqueue_root_set_owner(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),0,77));privileged=0;
    struct kernel_mqueue *group=make("group",1,32);assert(group->gid==20);removeq(group);
    privileged=1;assert(!kernel_mqueue_queue_set_owner(q,40,50));privileged=0;
    assert(kernel_mqueue_remove_name(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),"root",4)==-KERNEL_EPERM);
    credentials.fsuid=40;assert(!kernel_mqueue_remove_name(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),"root",4));credentials.fsuid=10;kernel_mqueue_put(q);
    privileged=1;assert(!kernel_mqueue_root_set_mode(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),01777));assert(!kernel_mqueue_root_set_owner(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),0,0));privileged=0;
}
static void *abandon_operation(void *arg)
{
    int *op=arg;current(1);
    if(op[1]==0){abandon_copy=1;(void)sendq(op[0],"import",7,0);}
    else if(op[1]==1){unsigned char cookie[32]={42};abandon_cookie=1;(void)notifyq(op[0],2,9,(uintptr_t)cookie);}
    else{char buffer[32];abandon_wait=1;(void)recvq(op[0],buffer,sizeof(buffer),0);}
    assert(0);return 0;
}
static void abandoned_owners_and_allocations(void)
{
    struct kernel_mqueue *q=make("abort",1,32);int fd=descriptor(q,2);unsigned before=allocations;
    pthread_t thread;
    for(int operation=0;operation<3;operation++){
        int op[2]={fd,operation};
        assert(!pthread_barrier_init(&abandoned,0,2)&&!pthread_barrier_init(&abandon_release,0,2));
        assert(!pthread_create(&thread,0,abandon_operation,op));
        int n=pthread_barrier_wait(&abandoned);assert(!n||n==PTHREAD_BARRIER_SERIAL_THREAD);
        /* The kernel cleans requests before releasing the stopped task stack. */
        assert(tasks[1].mqueue_request);kernel_mqueue_abort(&tasks[1]);
        n=pthread_barrier_wait(&abandon_release);assert(!n||n==PTHREAD_BARRIER_SERIAL_THREAD);
        assert(!pthread_join(thread,0));pthread_barrier_destroy(&abandoned);pthread_barrier_destroy(&abandon_release);
        assert(allocations==before&&!q->send_head&&!q->recv_head&&fds[fd]->references==1);
    }
    assert(cookie_cancelled==1);
    unsigned calls=allocation_calls;
    assert(!kernel_mqueue_getsetattr(cpu.current,&cpu.current->mm,fd,0,0));
    assert(allocation_calls==calls);
    unsigned char cookie[32]={42};assert(notifyq(-1,2,9,(uintptr_t)cookie)==-KERNEL_EBADF&&cookie_cancelled==2);
    for(int n=0;n<4;n++){
        allocation_calls=0;fail_at=n;int64_t result=sendq(fd,"allocation",11,0);fail_at=-1;
        if(!result){char out[32];assert(recvq(fd,out,sizeof(out),0)==11);}
        else assert(result==-KERNEL_ENOMEM);
        assert(allocations==before&&!tasks[0].mqueue_request&&!q->count);
    }
    closefd(fd);removeq(q);
    q=make("shutdown",1,32);fd=descriptor(q,2);kernel_mqueue_put(q);
    assert(kernel_mqueue_shutdown()==-KERNEL_EBUSY&&!kernel_mqueue_lookup_name(kernel_mqueue_namespace_tree(kernel_task_ipc_namespace(),1),"shutdown",8));
    closefd(fd);assert(!kernel_mqueue_shutdown()&&!kernel_ipc_namespace_state(kernel_ipc_initial())->mq);
}
static void scheduler_wait_order(void)
{
    current(0);struct kernel_mqueue *q=make("wait-rank",2,32);
    struct kernel_mqueue_request requests[3]={0};
    const int nice[3]={10,-5,19};
    for(unsigned i=0;i<3;i++) {
        current(i);kernel_sched_publish_priority(&tasks[i].scheduling,i==2?1:0,nice[i]);
        requests[i].task=&tasks[i];requests[i].queue=q;
        KERNEL_RAW_SCOPE(g,&q->lock);assert(request_prepare(&requests[i])==1);
    }
    assert(q->recv_head==&requests[2]&&q->recv_head->next==&requests[1]&&q->recv_tail==&requests[0]);
    for(unsigned i=0;i<3;i++) {
        {KERNEL_RAW_SCOPE(g,&q->lock);request_remove(&requests[i]);}
        current(i);assert(!kernel_wait_finish(&requests[i].token));kernel_sched_publish_priority(&tasks[i].scheduling,0,0);
    }
    current(0);removeq(q);puts("mqueue waiter order uses normal nice and effective RT rank");
}

static void ipc_namespace_lifetime(void)
{
    unsigned before=allocations;struct kernel_namespace *left=0,*right=0;
    assert(!kernel_ipc_namespace_create(&heap,&left)&&!kernel_ipc_namespace_create(&heap,&right));
    host_ipc_namespace=left;
    unsigned empty=allocations;
    assert(!kernel_mqueue_namespace_tree(left,0)&&allocations==empty);
    struct kernel_mqueue *q=make("isolated",2,32);int fd=descriptor(q,2);
    struct kernel_mqueue_tree *tree=q->tree;kernel_mqueue_tree_get(tree); /* Mount owner. */
    kernel_mqueue_put(q);
    host_ipc_namespace=right;
    assert(!kernel_mqueue_namespace_tree(right,0));
    struct kernel_mqueue *other=make("isolated",2,32);
    assert(other!=q&&other->tree!=tree&&other->account==q->account);
    assert(account.mqueue_bytes==q->charge+other->charge);
    struct kernel_vfs_stat a,b;kernel_mqueue_queue_stat(q,&a);kernel_mqueue_queue_stat(other,&b);
    assert(a.dev!=b.dev);
    kernel_namespace_put(left);
    assert(!tree->namespace&&tree->head==q);
    struct kernel_mqueue *found=kernel_mqueue_lookup_name(tree,"isolated",8);assert(found==q);kernel_mqueue_put(found);
    struct kernel_mqueue *made=0;int created=0;
    assert(kernel_mqueue_create_name(tree,cpu.current,"new",0600,0,1,&made,&created)==-KERNEL_EACCES);
    int notifications=signal_count;
    assert(!notifyq(fd,0,35,99)&&!sendq(fd,"alive",6,0)&&signal_count==notifications+1);
    char data[32];assert(recvq(fd,data,sizeof(data),0)==6&&!strcmp(data,"alive"));
    kernel_mqueue_tree_put(tree); /* Last mount drops names, OFD remains usable. */
    assert(!q->linked&&!sendq(fd,"fd",3,0)&&recvq(fd,data,sizeof(data),0)==3);
    closefd(fd);kernel_mqueue_put(other);kernel_namespace_put(right);host_ipc_namespace=0;
    assert(allocations==before&&!account.mqueue_bytes);
    puts("mqueue IPC namespace isolation, orphan mount creation and old FD notification PASS");
}
int main(void)
{
    kernel_wait_domain_init();assert(!kernel_mqueue_init(&heap));
    for(unsigned i=0;i<3;i++){tasks[i].tid=(int32_t)i+1;tasks[i].group_leader=&tasks[i];
        identities[i].id.number=i==1?1:(int32_t)i+1;identities[i].id.references=1;
        identities[i].id.registry=i==1?&child:&root;
        identities[i].ancestor=(struct kernel_pid_binding){.registry=&root,.number=42};
        tasks[i].identities[KERNEL_PID_TGID].identity=&identities[i].id;
        kernel_wait_record_init(&tasks[i].wait);kernel_wait_node_init(&tasks[i].default_wait_node,&tasks[i]);}
    current(0);scheduler_wait_order();priority_and_lifetime();notifications();handoff_and_timeout();fault_and_quota();namespace_metadata();abandoned_owners_and_allocations();
    ipc_namespace_lifetime();
    assert(!allocations&&!kernel_ipc_namespace_state(kernel_ipc_initial())->mq&&!account.mqueue_bytes&&account.references==1);
    for(unsigned i=0;i<3;i++)assert(identities[i].id.references==1&&!tasks[i].mqueue_request);
    puts("mqueue core: priority/FIFO, segmented payload, quota/name/OFD lifetime, notify, direct handoff, timeout/signal/fault, abandoned import/cookie/wait, allocation cleanup, shutdown PASS");return 0;
}

#if BOAROS_MAX_CPUS > 1
enum kernel_scheduler_status kernel_wait_backend_switch_locked(struct kernel_task *task,
    struct kernel_raw_guard *handoff)
{
    assert(handoff && handoff->lock == &kernel_wait_domain);
    kernel_raw_lock_release(handoff);
    return kernel_wait_backend_switch(task);
}
#endif
