#include <kernel/mqueue.h>
#include <kernel/avl.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/rlimit.h>
#include <kernel/signal.h>
#include <kernel/socket.h>
#include <kernel/socket_task.h>
#include <kernel/task_state.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>
#include <kernel/wait_internal.h>
#include "../fs/open_file_internal.h"
#include "../fs/files/private.h"
#include <stddef.h>
#include <string.h>

#define MQ_PRIO_MAX 32768U
#define MQ_CREAT 0100U
#define MQ_EXCL 0200U
#define MQ_NONBLOCK 04000U
#define MQ_SEGMENT_DATA (4096U-sizeof(void *))
/* Namespace owns one reference, each OFD/path node another. The real-UID
 * creation charge belongs to the queue until its last actual owner ends.
 * Registry(25) -> queue(30) -> scheduler/PID(40); no allocation/usercopy or
 * callback is permitted while registry or queue raw locks are held. */
struct mq_segment { struct mq_segment *next; unsigned char data[]; };
struct mq_message {
    struct mq_message *next;
    struct mq_segment *segments;
    uint64_t size;
    uint32_t priority;
};
struct mq_priority {
    struct kernel_avl_node node;
    struct mq_message *head,*tail;
};
struct mq_notification {
    struct kernel_pid *owner;
    struct kernel_exec_security *exec;
    struct kernel_netlink_cookie *cookie;
    uint64_t value;
    int32_t type,signal;
};
struct kernel_mqueue {
    struct kernel_mqueue *next;
    struct kernel_raw_lock lock;
    struct kernel_wait_queue waiters;
    struct kernel_avl_node *priorities;
    struct kernel_mqueue_request *send_head,*send_tail,*recv_head,*recv_tail;
    struct kernel_uid_account *account;
    struct mq_notification notification;
    uint64_t inode,charge,count,bytes,maxmsg,msgsize;
    struct kernel_vfs_timespec atime,mtime,ctime;
    uint32_t references,linked,uid,gid,mode,published;
    char name[256];
};
struct mq_file { struct kernel_open_file_description file; struct kernel_mqueue *queue; };
struct kernel_mqueue_request {
    struct kernel_mqueue_request *previous,*next;
    struct kernel_task *task;
    struct kernel_open_file_description *file;
    struct kernel_mqueue *queue;
    struct mq_message *message;
    struct mq_priority *priority;
    struct mq_notification pending,outgoing;
    struct kernel_wait_token token;
    uint64_t deadline;
    int32_t sender_tgid;
    uint32_t sender_uid,outgoing_reason;
    int result,sending,queued,wait_priority;
};
static struct {
    struct kernel_raw_lock lock;
    struct kernel_heap *heap;
    struct kernel_mqueue *head;
    uint64_t next_inode,named;
    uint32_t queues,root_mode,root_uid,root_gid;
    struct kernel_vfs_timespec root_atime,root_mtime,root_ctime;
} mq_table;
static void *mq_allocate(size_t size)
{
    void *p=0;
    enum kernel_heap_status s=kernel_heap_allocate_zeroed(mq_table.heap,1,size,&p);
    if(s==KERNEL_HEAP_STATUS_EMPTY) return 0;
    if(s) __builtin_trap();
    return p;
}
static void mq_free(void *p) { if(p)kernel_heap_release(mq_table.heap,p); }
static struct kernel_vfs_timespec mq_now(void)
{
    uint64_t n=kernel_time_realtime_ns();
    return (struct kernel_vfs_timespec){(int64_t)(n/1000000000),0};
}
int kernel_mqueue_init(struct kernel_heap *heap)
{
    if(!heap)return -KERNEL_EINVAL;
    if(mq_table.heap)return mq_table.heap==heap?0:-KERNEL_EINVAL;
    kernel_raw_lock_init(&mq_table.lock,25);mq_table.heap=heap;mq_table.next_inode=2;
    mq_table.root_mode=KERNEL_VFS_S_IFDIR|01777;mq_table.root_uid=mq_table.root_gid=0;
    mq_table.root_atime=mq_table.root_mtime=mq_table.root_ctime=mq_now();
    return 0;
}
int kernel_mqueue_shutdown(void)
{
    if(!mq_table.heap)return 0;
    struct kernel_mqueue *head;
    {KERNEL_RAW_SCOPE(g,&mq_table.lock);head=mq_table.head;mq_table.head=0;mq_table.named=0;
     for(struct kernel_mqueue *q=head;q;q=q->next){KERNEL_RAW_SCOPE(state,&q->lock);q->linked=0;}}
    while(head){struct kernel_mqueue *q=head;head=q->next;q->next=0;kernel_mqueue_put(q);}
    KERNEL_RAW_SCOPE(g,&mq_table.lock);
    if(mq_table.queues)return -KERNEL_EBUSY;
    mq_table.heap=0;return 0;
}
void kernel_mqueue_get(struct kernel_mqueue *q)
{
    uint32_t n=__atomic_fetch_add(&q->references,1,__ATOMIC_RELAXED);
    if(!n||n==UINT32_MAX)__builtin_trap();
}
static void message_free(struct mq_message *m)
{
    if(!m)return;
    while(m->segments){struct mq_segment *s=m->segments;m->segments=s->next;mq_free(s);}
    mq_free(m);
}
static void notification_finish(struct mq_notification *n,unsigned reason,
    int32_t sender,uint32_t uid)
{
    if(!n->owner)return;
    if(n->cookie)kernel_socket_mqueue_cookie_send(&n->cookie,reason);
    else if(reason==1 && n->type==0)
        kernel_signal_mqueue_notify(n->owner,n->exec,n->signal,n->value,sender,uid);
    if(n->exec)kernel_exec_security_put(n->exec);
    kernel_task_identity_put(n->owner);
    *n=(struct mq_notification){0};
}
void kernel_mqueue_put(struct kernel_mqueue *q)
{
    if(__atomic_sub_fetch(&q->references,1,__ATOMIC_ACQ_REL))return;
    if(q->linked||q->send_head||q->recv_head)__builtin_trap();
    if(kernel_wait_queue_close(&q->waiters))__builtin_trap();
    while(kernel_wait_queue_destroy(&q->waiters)==KERNEL_SCHEDULER_STATUS_BUSY)
        if(kernel_scheduler_yield_current())__builtin_trap();
    while(q->priorities){
        struct kernel_avl_node *node=kernel_avl_first(q->priorities);
        q->priorities=kernel_avl_remove_node(q->priorities,node);
        struct mq_priority *p=(void *)node;
        while(p->head){struct mq_message *m=p->head;p->head=m->next;message_free(m);}
        mq_free(p);
    }
    notification_finish(&q->notification,2,0,0);
    if(q->account)kernel_uid_account_mqueue_uncharge(q->account,q->charge);
    if(q->published){KERNEL_RAW_SCOPE(g,&mq_table.lock);if(!mq_table.queues)__builtin_trap();mq_table.queues--;}
    mq_free(q);
}
struct kernel_mqueue *kernel_mqueue_lookup_name(const char *name,size_t size)
{
    if(!mq_table.heap)return 0;
    KERNEL_RAW_SCOPE(g,&mq_table.lock);
    for(struct kernel_mqueue *q=mq_table.head;q;q=q->next)
        if(strlen(q->name)==size&&!memcmp(q->name,name,size)){kernel_mqueue_get(q);return q;}
    return 0;
}
struct kernel_mqueue *kernel_mqueue_lookup_inode(uint64_t id)
{
    KERNEL_RAW_SCOPE(g,&mq_table.lock);
    for(struct kernel_mqueue *q=mq_table.head;q;q=q->next)
        if(q->inode==id){kernel_mqueue_get(q);return q;}
    return 0;
}
int kernel_mqueue_queue_permission(struct kernel_mqueue *q,unsigned access)
{
    KERNEL_RAW_SCOPE(g,&q->lock);
    return kernel_vfs_dac_permission(q->mode,q->uid,q->gid,access,kernel_credentials_current());
}
int kernel_mqueue_root_stat(struct kernel_vfs_stat *stat)
{
    KERNEL_RAW_SCOPE(g,&mq_table.lock);
    *stat=(struct kernel_vfs_stat){.dev=0x19800202,.ino=1,.mode=mq_table.root_mode,.nlink=2,
        .uid=mq_table.root_uid,.gid=mq_table.root_gid,.size=40+20*mq_table.named,.blksize=BOAROS_PAGE_SIZE,
        .atime=mq_table.root_atime,.mtime=mq_table.root_mtime,.ctime=mq_table.root_ctime};return 0;
}
int kernel_mqueue_root_permission(unsigned access)
{
    KERNEL_RAW_SCOPE(g,&mq_table.lock);
    return kernel_vfs_dac_permission(mq_table.root_mode,mq_table.root_uid,mq_table.root_gid,access,kernel_credentials_current());
}
int kernel_mqueue_root_set_mode(uint32_t mode)
{
    KERNEL_RAW_SCOPE(g,&mq_table.lock);uint32_t result=KERNEL_VFS_S_IFDIR|(mode&07777);
    int error=kernel_vfs_chmod_allowed(mq_table.root_uid,mq_table.root_gid,&result);if(error)return error;
    mq_table.root_mode=result;mq_table.root_ctime=mq_now();return 0;
}
int kernel_mqueue_root_set_owner(uint32_t uid,uint32_t gid)
{
    KERNEL_RAW_SCOPE(g,&mq_table.lock);
    int error=kernel_vfs_chown_allowed(mq_table.root_uid,mq_table.root_gid,mq_table.root_mode,uid,gid);if(error)return error;
    if(uid!=UINT32_MAX)mq_table.root_uid=uid;
    if(gid!=UINT32_MAX)mq_table.root_gid=gid;
    mq_table.root_ctime=mq_now();return 0;
}
int kernel_mqueue_create_name(struct kernel_task *task,const char *name,uint32_t mode,
    const struct kernel_mq_attr *attr,int exclusive,struct kernel_mqueue **out,int *created)
{
    *out=0;*created=0;
    if(!mq_table.heap)return -KERNEL_ENOSPC;
    size_t n=strlen(name);
    if(!n||n>255)return !n?-KERNEL_ENOENT:-KERNEL_ENAMETOOLONG;
    if(!strcmp(name,".")||!strcmp(name,".."))return -KERNEL_EACCES;
    for(size_t j=0;j<n;j++)if(name[j]=='/')return -KERNEL_EACCES;
    struct kernel_mqueue *candidate=0;
    const struct kernel_credentials *c=kernel_credentials_current();
    int privileged=kernel_credentials_capable(c,KERNEL_CAP_SYS_RESOURCE);
    int result=0;
    for(;;){
        struct kernel_mqueue *found=0;
        { KERNEL_RAW_SCOPE(g,&mq_table.lock);
          for(found=mq_table.head;found;found=found->next)if(!strcmp(found->name,name))break;
          if(found){kernel_mqueue_get(found);}
          else if((result=kernel_vfs_dac_permission(mq_table.root_mode,mq_table.root_uid,mq_table.root_gid,3,c))){}
          else if(!privileged && mq_table.queues>=256)result=-KERNEL_ENOSPC;
          else if(!candidate)result=1;
          else {
              if(mq_table.next_inode==UINT64_MAX)__builtin_trap();
              candidate->inode=mq_table.next_inode++;candidate->next=mq_table.head;
              mq_table.head=candidate;mq_table.queues++;mq_table.named++;candidate->linked=candidate->published=1;
              mq_table.root_mtime=mq_table.root_ctime=mq_now();
              kernel_mqueue_get(candidate);*out=candidate;*created=1;candidate=0;result=0;
          }
        }
        if(found){if(exclusive){kernel_mqueue_put(found);result=-KERNEL_EEXIST;}else{*out=found;result=0;}break;}
        if(result!=1)break;
        int64_t count=attr?attr->maxmsg:10,size=attr?attr->msgsize:8192;
        if(count<=0||size<=0||count>(privileged?65536:10)||size>(privileged?16777216:8192)){
            result=-KERNEL_EINVAL;break;
        }
        candidate=mq_allocate(sizeof(*candidate));if(!candidate){result=-KERNEL_ENOMEM;break;}
        candidate->references=1;candidate->maxmsg=(uint64_t)count;candidate->msgsize=(uint64_t)size;
        candidate->charge=(uint64_t)count*((uint64_t)size+48)+((uint64_t)count<MQ_PRIO_MAX?(uint64_t)count:MQ_PRIO_MAX)*48;
        struct kernel_rlimit64 limit;
        if(kernel_task_get_rlimit(task,KERNEL_RLIMIT_MSGQUEUE,&limit))__builtin_trap();
        result=kernel_uid_account_mqueue_charge(c->account,candidate->charge,limit.current);
        if(result)break;
        candidate->account=c->account;candidate->uid=c->fsuid;candidate->gid=c->fsgid;
        candidate->mode=KERNEL_VFS_S_IFREG|(mode&0777);
        candidate->atime=candidate->mtime=candidate->ctime=mq_now();
        memcpy(candidate->name,name,n+1);
        kernel_raw_lock_init(&candidate->lock,KERNEL_RAW_RANK_OBJECT);
        kernel_wait_queue_init(&candidate->waiters);
    }
    if(candidate){
        if(candidate->account)kernel_mqueue_put(candidate);
        else mq_free(candidate);
    }
    return result;
}
int kernel_mqueue_remove_name(const char *name,size_t n)
{
    struct kernel_mqueue *q=0;int result=-KERNEL_ENOENT;
    const struct kernel_credentials *c=kernel_credentials_current();
    { KERNEL_RAW_SCOPE(g,&mq_table.lock);
      struct kernel_mqueue **link=&mq_table.head;
      while(*link && (strlen((*link)->name)!=n||memcmp((*link)->name,name,n)))link=&(*link)->next;
      q=*link;
      if(q){KERNEL_RAW_SCOPE(state,&q->lock);
          if((result=kernel_vfs_dac_permission(mq_table.root_mode,mq_table.root_uid,mq_table.root_gid,3,c))){}
          else if((mq_table.root_mode&01000U)&&c->fsuid!=mq_table.root_uid&&c->fsuid!=q->uid&&
              !kernel_credentials_capable(c,KERNEL_CAP_FOWNER))result=-KERNEL_EPERM;
          else{*link=q->next;q->next=0;q->linked=0;mq_table.named--;
              q->ctime=mq_table.root_mtime=mq_table.root_ctime=mq_now();result=0;}
      }
    }
    if(!result)kernel_mqueue_put(q);
    return result;
}
int kernel_mqueue_next(uint64_t *cursor,uint64_t *id,char *name,size_t capacity)
{
    KERNEL_RAW_SCOPE(g,&mq_table.lock);
    struct kernel_mqueue *best=0;
    for(struct kernel_mqueue *q=mq_table.head;q;q=q->next)
        if(q->inode>*cursor&&(!best||q->inode<best->inode))best=q;
    if(!best)return 0;
    size_t n=strlen(best->name);if(n+1>capacity)return -KERNEL_ENAMETOOLONG;
    memcpy(name,best->name,n+1);*cursor=*id=best->inode;return 1;
}
static struct kernel_mqueue *file_queue(struct kernel_open_file_description *file)
{ return ((struct mq_file *)file)->queue; }
static int file_create(struct kernel_heap *heap,struct kernel_mqueue *q,uint32_t flags,
    struct kernel_open_file_description **out)
{
    struct mq_file *f=0;
    enum kernel_heap_status s=kernel_heap_allocate_zeroed(heap,1,sizeof(*f),(void **)&f);
    if(s==KERNEL_HEAP_STATUS_EMPTY)return -KERNEL_ENOMEM;
    if(s)__builtin_trap();
    f->file.heap=heap;f->file.references=1;f->file.kind=KERNEL_OPEN_FILE_KIND_MQUEUE;
    f->file.open_flags=flags&(3U|MQ_NONBLOCK);f->file.credentials=kernel_credentials_get(kernel_credentials_current());
    kernel_mutex_init(&f->file.offset_lock,10,(uintptr_t)f);kernel_mqueue_get(q);f->queue=q;*out=&f->file;return 0;
}
int kernel_mqueue_close(struct kernel_open_file_description *file)
{
    if(file->file.private_data){int error=kernel_vfs_close(&file->file);if(error)return error;}
    kernel_mqueue_put(file_queue(file));return 0;
}
uint32_t kernel_mqueue_poll(struct kernel_open_file_description *file,uint32_t requested,
    struct kernel_wait_queue **queue)
{
    (void)requested;struct kernel_mqueue *q=file_queue(file);KERNEL_RAW_SCOPE(g,&q->lock);
    if(queue)*queue=&q->waiters;
    return(q->count?KERNEL_POLLIN|KERNEL_POLLRDNORM:0U)|(q->count<q->maxmsg?KERNEL_POLLOUT|KERNEL_POLLWRNORM:0U);
}
static int import_name(struct kernel_mm *mm,uint64_t address,char name[256])
{
    size_t n;enum kernel_uaccess_status s=kernel_copy_string_from_user(mm,name,address,256,&n);
    if(s)return s==KERNEL_UACCESS_STATUS_TOO_LONG?-KERNEL_ENAMETOOLONG:-KERNEL_EFAULT;
    if(!n)return -KERNEL_ENOENT;
    if(!strcmp(name,".")||!strcmp(name,".."))return -KERNEL_EACCES;
    for(size_t j=0;j<n;j++)if(name[j]=='/')return -KERNEL_EACCES;
    return 0;
}
int64_t kernel_mqueue_open(struct kernel_task *task,struct kernel_mm *mm,uint64_t name_address,
    uint32_t flags,uint32_t mode,uint64_t attr_address)
{
    struct kernel_mq_attr attr,*attributes=0;size_t copied;
    if(attr_address){if(kernel_copy_from_user(mm,&attr,attr_address,sizeof(attr),&copied))return -KERNEL_EFAULT;attributes=&attr;}
    char name[256];int result=import_name(mm,name_address,name);if(result)return result;
    struct kernel_files *files;
    if(kernel_task_files_borrow(task,&files))__builtin_trap();
    uint32_t fd;int error;
    if(kernel_files_find_free_fd(files,&fd,&error))__builtin_trap();
    if(error)return error;
    KERNEL_FILES_RESERVATION_SCOPE(reservation);reservation.files=files;reservation.fd=fd;
    struct kernel_mqueue *q=0;int created=0;
    if(flags&MQ_CREAT){
        const struct kernel_fs_context *fs;
        if(kernel_task_fs_context_borrow(task,&fs))__builtin_trap();
        mode&=~kernel_fs_context_umask(fs);
        result=kernel_mqueue_create_name(task,name,mode,attributes,!!(flags&MQ_EXCL),&q,&created);
    }else{q=kernel_mqueue_lookup_name(name,strlen(name));result=q?0:-KERNEL_ENOENT;}
    if(result)return result;
    if(!created)result=(flags&3U)==3U?-KERNEL_EINVAL:
        kernel_mqueue_queue_permission(q,(flags&3U)==0?4:(flags&3U)==1?2:6);
    struct kernel_open_file_description *owner=0;
    if(!result)result=file_create(files->heap,q,flags,&owner);
    kernel_mqueue_put(q);
    if(result)return result;
    if(kernel_files_install_new_owned_at(files,fd,KERNEL_FILES_FD_CLOEXEC,&owner)){
        if(kernel_open_file_release(&owner))__builtin_trap();
        __builtin_trap();
    }
    reservation.files=0;return fd;
}
int64_t kernel_mqueue_unlink(struct kernel_task *task,struct kernel_mm *mm,uint64_t address)
{
    (void)task;char name[256];int r=import_name(mm,address,name);
    return r?r:kernel_mqueue_remove_name(name,strlen(name));
}
static void request_remove(struct kernel_mqueue_request *r)
{
    if(!r->queued)return;
    struct kernel_mqueue *q=r->queue;
    struct kernel_mqueue_request **head=r->sending?&q->send_head:&q->recv_head;
    struct kernel_mqueue_request **tail=r->sending?&q->send_tail:&q->recv_tail;
    if(r->previous)r->previous->next=r->next;else *head=r->next;
    if(r->next)r->next->previous=r->previous;else *tail=r->previous;
    r->previous=r->next=0;r->queued=0;
}
static void request_release(struct kernel_mqueue_request *r)
{
    if(r->queued)__builtin_trap();
    kernel_socket_mqueue_cookie_cancel(&r->pending.cookie);
    if(r->pending.owner){
        if(r->pending.exec)kernel_exec_security_put(r->pending.exec);
        kernel_task_identity_put(r->pending.owner);
    }
    notification_finish(&r->outgoing,r->outgoing_reason,r->sender_tgid,r->sender_uid);
    message_free(r->message);mq_free(r->priority);
    if(r->file && kernel_open_file_release(&r->file))__builtin_trap();
}
static void request_finish(struct kernel_mqueue_request *r)
{
    if(r->task->mqueue_request!=r)__builtin_trap();
    r->task->mqueue_request=0;request_release(r);
}
static void request_begin(struct kernel_task *task,int sending,struct kernel_mqueue_request *r)
{
    *r=(struct kernel_mqueue_request){.task=task,.sending=sending};
    /* Exit aborts this request before releasing the stopped task's stack. */
    if(task->mqueue_request)__builtin_trap();
    task->mqueue_request=r;
}
static int request_pin(struct kernel_mqueue_request *r,int64_t fd)
{
    struct kernel_files *files;
    if(kernel_task_files_borrow(r->task,&files))__builtin_trap();
    struct kernel_files_pin_guard pin={0};
    r->file=kernel_files_hold_fd(files,fd,&pin);pin.description=0;
    if(!r->file || r->file->kind!=KERNEL_OPEN_FILE_KIND_MQUEUE){
        return -KERNEL_EBADF;
    }
    r->queue=file_queue(r->file);return 0;
}
static struct kernel_mqueue_request *request_create(struct kernel_task *task,
    int64_t fd,int sending,int *error,struct kernel_mqueue_request *r)
{
    request_begin(task,sending,r);
    *error=request_pin(r,fd);
    if(*error){request_finish(r);return 0;}
    return r;
}
void kernel_mqueue_abort(struct kernel_task *task)
{
    struct kernel_mqueue_request *r=task->mqueue_request;
    if(!r)return;
    if(r->queue){KERNEL_RAW_SCOPE(g,&r->queue->lock);request_remove(r);}
    task->mqueue_request=0;request_release(r);
}
static int import_timeout(struct kernel_mm *mm,uint64_t address,uint64_t *deadline)
{
    *deadline=0;if(!address)return 0;
    struct {int64_t sec,nsec;} ts;size_t copied;
    if(kernel_copy_from_user(mm,&ts,address,sizeof(ts),&copied))return -KERNEL_EFAULT;
    if(ts.sec<0||ts.nsec<0||ts.nsec>=1000000000)return -KERNEL_EINVAL;
    uint64_t ns=(uint64_t)ts.sec>(UINT64_MAX-(uint64_t)ts.nsec)/1000000000
        ?UINT64_MAX:(uint64_t)ts.sec*1000000000+(uint64_t)ts.nsec;
    uint64_t offset=kernel_time_boot_realtime_offset();
    ns=ns<=offset?0:ns-offset;
    enum kernel_time_status s=kernel_time_deadline_from_monotonic(ns,deadline);
    if(s==KERNEL_TIME_STATUS_DEADLINE_PASSED){*deadline=UINT64_MAX;return 0;}
    if(s)__builtin_trap();
    return 0;
}
static void request_complete(struct kernel_mqueue_request *r)
{
    request_remove(r);r->result=0;
    if(kernel_wait_notify(&r->token,KERNEL_WAIT_WOKEN))__builtin_trap();
}
static int request_prepare(struct kernel_mqueue_request *r)
{
    if(r->deadline==UINT64_MAX)return -KERNEL_ETIMEDOUT;
    if(r->deadline && (int64_t)(kernel_wait_backend_time()-r->deadline)>=0)return -KERNEL_ETIMEDOUT;
    if(kernel_signal_has_pending(r->task)||kernel_signal_termination_requested(r->task))return -KERNEL_ERESTARTSYS;
    {
        KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
        if(kernel_wait_prepare_locked(0,r->deadline,1,&r->token))__builtin_trap();
        r->wait_priority=r->task->scheduling.priority;
    }
    struct kernel_mqueue *q=r->queue;
    struct kernel_mqueue_request **head=r->sending?&q->send_head:&q->recv_head;
    struct kernel_mqueue_request **tail=r->sending?&q->send_tail:&q->recv_tail;
    struct kernel_mqueue_request *next=*head;
    while(next&&next->wait_priority>=r->wait_priority)next=next->next;
    r->next=next;r->previous=next?next->previous:*tail;
    if(r->previous)r->previous->next=r;else *head=r;
    if(next)next->previous=r;else *tail=r;
    r->queued=1;r->result=1;return 1;
}
static int request_wait(struct kernel_mqueue_request *r)
{
    enum kernel_wait_wake_reason reason;
    if(kernel_wait_park(&r->token,&reason))__builtin_trap();
    int result;
    { KERNEL_RAW_SCOPE(g,&r->queue->lock);
      if(r->queued){request_remove(r);r->result=reason==KERNEL_WAIT_TIMEOUT?-KERNEL_ETIMEDOUT:-KERNEL_ERESTARTSYS;}
      result=r->result;
    }
    if(kernel_wait_finish(&r->token))__builtin_trap();
    return result;
}
static int message_import(struct kernel_mqueue_request *r,struct kernel_mm *mm,
    uint64_t address,uint64_t size,uint32_t priority)
{
    r->message=mq_allocate(sizeof(*r->message));if(!r->message)return -KERNEL_ENOMEM;
    r->message->size=size;r->message->priority=priority;
    struct mq_segment **next=&r->message->segments;
    uint64_t done=0;
    while(done<size){
        size_t n=size-done>MQ_SEGMENT_DATA?MQ_SEGMENT_DATA:(size_t)(size-done);
        struct mq_segment *s=mq_allocate(sizeof(*s)+n);if(!s)return -KERNEL_ENOMEM;
        *next=s;next=&s->next;size_t copied;
        if(kernel_copy_from_user(mm,s->data,address+done,n,&copied))return -KERNEL_EFAULT;
        done+=n;
    }
    return 0;
}
static struct mq_priority *priority_find(struct kernel_mqueue *q,uint32_t priority)
{
    struct kernel_avl_node *node=q->priorities;
    while(node&&node->key!=priority)node=priority<node->key?node->left:node->right;
    return (void *)node;
}
static void message_enqueue(struct kernel_mqueue_request *r)
{
    struct kernel_mqueue *q=r->queue;struct mq_message *m=r->message;
    struct mq_priority *p=priority_find(q,m->priority);
    if(!p){
        p=r->priority;if(!p)__builtin_trap();r->priority=0;
        q->priorities=kernel_avl_insert_node(q->priorities,&p->node);
    }
    if(p->tail)p->tail->next=m;else p->head=m;
    p->tail=m;m->next=0;q->count++;q->bytes+=m->size;r->message=0;
}
static void message_dequeue(struct kernel_mqueue_request *r)
{
    struct kernel_mqueue *q=r->queue;struct mq_priority *p=(void *)kernel_avl_last(q->priorities);
    if(!p||!p->head)__builtin_trap();
    r->message=p->head;p->head=r->message->next;r->message->next=0;
    if(!p->head){
        p->tail=0;q->priorities=kernel_avl_remove_node(q->priorities,&p->node);r->priority=p;
    }
    q->count--;q->bytes-=r->message->size;
}
static int message_publish(struct kernel_mqueue_request *sender,struct mq_notification *notification)
{
    struct kernel_mqueue *q=sender->queue;
    int enqueued=!q->recv_head;
    if(q->recv_head){
        struct kernel_mqueue_request *receiver=q->recv_head;
        receiver->message=sender->message;sender->message=0;request_complete(receiver);
    }else{
        if(!q->count&&q->notification.owner){*notification=q->notification;q->notification=(struct mq_notification){0};}
        message_enqueue(sender);
    }
    q->mtime=q->ctime=mq_now();
    return enqueued;
}
int64_t kernel_mqueue_send(struct kernel_task *task,struct kernel_mm *mm,int64_t fd,
    uint64_t data,uint64_t size,uint32_t priority,uint64_t abstime)
{
    uint64_t deadline;int error=import_timeout(mm,abstime,&deadline);if(error)return error;
    if(priority>=MQ_PRIO_MAX)return -KERNEL_EINVAL;
    struct kernel_mqueue_request storage;
    struct kernel_mqueue_request *r=request_create(task,fd,1,&error,&storage);if(!r)return error;
    if((r->file->open_flags&3U)!=1&&(r->file->open_flags&3U)!=2){error=-KERNEL_EBADF;goto done;}
    if(size>r->queue->msgsize){error=-KERNEL_EMSGSIZE;goto done;}
    r->deadline=deadline;error=message_import(r,mm,data,size,priority);if(error)goto done;
    if(kernel_task_tgid(task,&r->sender_tgid))__builtin_trap();
    r->sender_uid=kernel_credentials_current()->uid;r->outgoing_reason=1;
    struct kernel_wait_cursor cursor={0};int notify=0;
    for(;;){
      { KERNEL_RAW_SCOPE(g,&r->queue->lock);
        if(r->queue->count<r->queue->maxmsg){
            if(!r->queue->recv_head&&!r->priority&&!priority_find(r->queue,priority))error=2;
            else{
                /* Direct delivery wakes its receiver; queue readiness is unchanged. */
                if(message_publish(r,&r->outgoing))
                    notify=kernel_wait_cursor_prepare(&r->queue->waiters,&cursor);
                error=0;
            }
        }else if(__atomic_load_n(&r->file->open_flags,__ATOMIC_RELAXED)&MQ_NONBLOCK)error=-KERNEL_EAGAIN;
        else if(!r->priority)error=2;
        else error=request_prepare(r);
      }
      if(error!=2)break;
      /* A sleeping sender must retain a possible new priority node: the old
       * bucket can disappear before a receiver accepts this payload. Direct
       * delivery and an existing immediate bucket need no spare allocation. */
      r->priority=mq_allocate(sizeof(*r->priority));
      if(!r->priority){error=-KERNEL_ENOMEM;break;}
      r->priority->node.key=priority;r->priority->node.height=1;
    }
    if(notify)kernel_wait_cursor_wake(&cursor,1);
    notification_finish(&r->outgoing,1,r->sender_tgid,r->sender_uid);
    if(error==1)error=request_wait(r);
done:request_finish(r);return error;
}
int64_t kernel_mqueue_receive(struct kernel_task *task,struct kernel_mm *mm,int64_t fd,
    uint64_t data,uint64_t size,uint64_t priority,uint64_t abstime)
{
    uint64_t deadline;int error=import_timeout(mm,abstime,&deadline);if(error)return error;
    struct kernel_mqueue_request storage;
    struct kernel_mqueue_request *r=request_create(task,fd,0,&error,&storage);if(!r)return error;
    if((r->file->open_flags&3U)!=0&&(r->file->open_flags&3U)!=2){error=-KERNEL_EBADF;goto done;}
    if(size<r->queue->msgsize){error=-KERNEL_EMSGSIZE;goto done;}
    r->deadline=deadline;
    struct kernel_wait_cursor cursor={0};int notify=0;
    { KERNEL_RAW_SCOPE(g,&r->queue->lock);
      if(r->queue->count){
          message_dequeue(r);
          if(r->queue->send_head){
              struct kernel_mqueue_request *sender=r->queue->send_head;
              message_enqueue(sender);request_complete(sender);
          }else notify=kernel_wait_cursor_prepare(&r->queue->waiters,&cursor);
          r->queue->atime=r->queue->ctime=mq_now();
          error=0;
      }else if(__atomic_load_n(&r->file->open_flags,__ATOMIC_RELAXED)&MQ_NONBLOCK)error=-KERNEL_EAGAIN;
      else error=request_prepare(r);
    }
    if(notify)kernel_wait_cursor_wake(&cursor,1);
    if(error==1)error=request_wait(r);
    if(!error){
        size_t copied;uint64_t done_bytes=0;
        if(priority && kernel_copy_to_user(mm,priority,&r->message->priority,sizeof(uint32_t),&copied))error=-KERNEL_EFAULT;
        for(struct mq_segment *s=r->message->segments;s&&!error;s=s->next){
            size_t n=r->message->size-done_bytes>MQ_SEGMENT_DATA?MQ_SEGMENT_DATA:(size_t)(r->message->size-done_bytes);
            if(kernel_copy_to_user(mm,data+done_bytes,s->data,n,&copied))error=-KERNEL_EFAULT;
            done_bytes+=n;
        }
        if(!error){int64_t result=(int64_t)r->message->size;request_finish(r);return result;}
    }
done:request_finish(r);return error;
}
int64_t kernel_mqueue_notify(struct kernel_task *task,struct kernel_mm *mm,int64_t fd,uint64_t address)
{
    struct {uint64_t value;int32_t signo,type;unsigned char reserved[48];} event;
    unsigned char cookie[32];size_t copied;
    if(address){
        if(kernel_copy_from_user(mm,&event,address,sizeof(event),&copied))return -KERNEL_EFAULT;
        if(event.type<0||event.type>2)return -KERNEL_EINVAL;
        if(!event.type&&(event.signo<=0||event.signo>64))return -KERNEL_EINVAL;
        if(event.type==2){
            if(kernel_copy_from_user(mm,cookie,event.value,sizeof(cookie),&copied))return -KERNEL_EFAULT;
        }
    }
    int error;struct kernel_mqueue_request storage,*r=&storage;
    request_begin(task,0,r);
    if(address){
        r->pending.type=event.type;r->pending.signal=event.signo;r->pending.value=event.value;
        if(event.type==2){
            error=kernel_socket_mqueue_cookie_prepare(task,event.signo,cookie,&r->pending.cookie);
            if(error){request_finish(r);return error;}
        }
    }
    error=request_pin(r,fd);if(error){request_finish(r);return error;}
    struct kernel_pid *identity;
    { KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
      struct kernel_task *leader=task->group_leader?task->group_leader:task;
      identity=leader->identities[KERNEL_PID_TGID].identity;
      if(address&&identity){kernel_pid_get(identity);r->pending.owner=identity;
          if(!event.type)r->pending.exec=kernel_exec_security_get(task->exec_security);}
    }
    if(!identity){request_finish(r);return -KERNEL_ESRCH;}
    { KERNEL_RAW_SCOPE(g,&r->queue->lock);
      if(!address){
          if(r->queue->notification.owner==identity){r->outgoing=r->queue->notification;r->outgoing_reason=2;r->queue->notification=(struct mq_notification){0};}
          error=0;
      }else if(r->queue->notification.owner)error=-KERNEL_EBUSY;
      else{r->queue->notification=r->pending;r->pending=(struct mq_notification){0};error=0;}
      if(!error)r->queue->atime=r->queue->ctime=mq_now();
    }
    notification_finish(&r->outgoing,2,0,0);
    request_finish(r);return error;
}
void kernel_mqueue_close_fd(struct kernel_open_file_description *file,struct kernel_task *task)
{
    if(file->kind!=KERNEL_OPEN_FILE_KIND_MQUEUE||!task)return;
    struct kernel_pid *identity;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
     struct kernel_task *leader=task->group_leader?task->group_leader:task;
     identity=leader->identities[KERNEL_PID_TGID].identity;}
    if(!identity)return;
    struct kernel_mqueue *q=file_queue(file);struct mq_notification removed={0};
    {KERNEL_RAW_SCOPE(g,&q->lock);
     if(q->notification.owner==identity){removed=q->notification;q->notification=(struct mq_notification){0};}}
    notification_finish(&removed,2,0,0);
}
int64_t kernel_mqueue_getsetattr(struct kernel_task *task,struct kernel_mm *mm,int64_t fd,
    uint64_t new_address,uint64_t old_address)
{
    struct kernel_mq_attr new_attr,old_attr={0};size_t copied;
    if(new_address){
        if(kernel_copy_from_user(mm,&new_attr,new_address,sizeof(new_attr),&copied))return -KERNEL_EFAULT;
        if(new_attr.flags&~MQ_NONBLOCK)return -KERNEL_EINVAL;
    }
    int error;struct kernel_mqueue_request storage;
    struct kernel_mqueue_request *r=request_create(task,fd,0,&error,&storage);if(!r)return error;
    {KERNEL_RAW_SCOPE(g,&r->queue->lock);
     old_attr.flags=__atomic_load_n(&r->file->open_flags,__ATOMIC_RELAXED)&MQ_NONBLOCK;
     old_attr.maxmsg=(int64_t)r->queue->maxmsg;old_attr.msgsize=(int64_t)r->queue->msgsize;old_attr.curmsgs=(int64_t)r->queue->count;
     if(new_address){
         uint32_t before=__atomic_load_n(&r->file->open_flags,__ATOMIC_RELAXED),after;
         do{after=(before&~MQ_NONBLOCK)|(uint32_t)new_attr.flags;}
         while(!__atomic_compare_exchange_n(&r->file->open_flags,&before,after,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
         r->queue->atime=r->queue->ctime=mq_now();
     }}
    error=old_address&&kernel_copy_to_user(mm,old_address,&old_attr,sizeof(old_attr),&copied)?-KERNEL_EFAULT:0;
    request_finish(r);return error;
}
int kernel_mqueue_queue_stat(struct kernel_mqueue *q,struct kernel_vfs_stat *stat)
{
    KERNEL_RAW_SCOPE(g,&q->lock);
    *stat=(struct kernel_vfs_stat){.dev=0x19800202U,.ino=q->inode,.mode=q->mode,.nlink=q->linked,
        .uid=q->uid,.gid=q->gid,.size=80,.blksize=BOAROS_PAGE_SIZE,.atime=q->atime,.mtime=q->mtime,.ctime=q->ctime};
    return 0;
}
int kernel_mqueue_stat(struct kernel_open_file_description *file,struct kernel_vfs_stat *stat)
{return kernel_mqueue_queue_stat(file_queue(file),stat);}
int kernel_mqueue_statfs(struct kernel_open_file_description *file,struct kernel_linux_statfs *stat)
{
    uint64_t flags=0;
    if(file->file.mount){struct kernel_vfs_statfs native;int error=kernel_vfs_mount_statfs(file->file.mount,&native);
        if(error)return error;
        flags=native.flags;}
    *stat=(struct kernel_linux_statfs){.f_type=0x19800202,.f_bsize=BOAROS_PAGE_SIZE,.f_namelen=255,
        .f_fsid={0x19800202,0},.f_frsize=BOAROS_PAGE_SIZE,.f_flags=(int64_t)flags};return 0;
}
int kernel_mqueue_queue_set_mode(struct kernel_mqueue *q,uint32_t mode)
{
    KERNEL_RAW_SCOPE(g,&q->lock);uint32_t result=(q->mode&KERNEL_VFS_S_IFMT)|(mode&07777);
    int error=kernel_vfs_chmod_allowed(q->uid,q->gid,&result);if(error)return error;
    q->mode=result;q->ctime=mq_now();return 0;
}
int kernel_mqueue_queue_set_owner(struct kernel_mqueue *q,uint32_t uid,uint32_t gid)
{
    KERNEL_RAW_SCOPE(g,&q->lock);
    int error=kernel_vfs_chown_allowed(q->uid,q->gid,q->mode,uid,gid);if(error)return error;
    uint32_t new_gid=gid==UINT32_MAX?q->gid:gid;
    q->mode=kernel_vfs_chown_mode(q->mode,q->gid,new_gid);
    if(uid!=UINT32_MAX)q->uid=uid;
    q->gid=new_gid;q->ctime=mq_now();return 0;
}
int kernel_mqueue_set_mode(struct kernel_open_file_description *f,uint32_t mode)
{return f->file.mount&&kernel_vfs_mount_is_readonly(f->file.mount)?-KERNEL_EROFS:kernel_mqueue_queue_set_mode(file_queue(f),mode);}
int kernel_mqueue_set_owner(struct kernel_open_file_description *f,uint32_t uid,uint32_t gid)
{return f->file.mount&&kernel_vfs_mount_is_readonly(f->file.mount)?-KERNEL_EROFS:kernel_mqueue_queue_set_owner(file_queue(f),uid,gid);}
static int update_times(uint32_t mode,uint32_t uid,uint32_t gid,
    struct kernel_vfs_timespec *atime,struct kernel_vfs_timespec *mtime,struct kernel_vfs_timespec *ctime,
    const struct kernel_vfs_timespec *times)
{
    if(times)for(unsigned j=0;j<2;j++)
        if(times[j].nanoseconds!=KERNEL_VFS_UTIME_NOW&&times[j].nanoseconds!=KERNEL_VFS_UTIME_OMIT&&
           (times[j].nanoseconds<0||times[j].nanoseconds>=1000000000))return -KERNEL_EINVAL;
    if(times&&times[0].nanoseconds==KERNEL_VFS_UTIME_OMIT&&times[1].nanoseconds==KERNEL_VFS_UTIME_OMIT)return 0;
    const struct kernel_credentials *c=kernel_credentials_current();
    if(c->fsuid!=uid&&!kernel_credentials_capable(c,KERNEL_CAP_FOWNER)){
        if(times&&(times[0].nanoseconds!=KERNEL_VFS_UTIME_NOW||times[1].nanoseconds!=KERNEL_VFS_UTIME_NOW))return -KERNEL_EPERM;
        int error=kernel_vfs_dac_permission(mode,uid,gid,2,c);if(error)return error;
    }
    struct kernel_vfs_timespec now=mq_now();
    if(!times||times[0].nanoseconds!=KERNEL_VFS_UTIME_OMIT)*atime=!times||times[0].nanoseconds==KERNEL_VFS_UTIME_NOW?now:times[0];
    if(!times||times[1].nanoseconds!=KERNEL_VFS_UTIME_OMIT)*mtime=!times||times[1].nanoseconds==KERNEL_VFS_UTIME_NOW?now:times[1];
    /* mqueue keeps the fixed Linux superblock default: one-second timestamps. */
    atime->nanoseconds=0;mtime->nanoseconds=0;
    *ctime=now;return 0;
}
int kernel_mqueue_queue_set_times(struct kernel_mqueue *q,const struct kernel_vfs_timespec *times)
{
    KERNEL_RAW_SCOPE(g,&q->lock);
    return update_times(q->mode,q->uid,q->gid,&q->atime,&q->mtime,&q->ctime,times);
}
int kernel_mqueue_set_times(struct kernel_open_file_description *file,const struct kernel_vfs_timespec *times)
{
    if(file->file.mount && kernel_vfs_mount_is_readonly(file->file.mount))return -KERNEL_EROFS;
    return kernel_mqueue_queue_set_times(file_queue(file),times);
}
int kernel_mqueue_root_set_times(const struct kernel_vfs_timespec *times)
{
    KERNEL_RAW_SCOPE(g,&mq_table.lock);
    return update_times(mq_table.root_mode,mq_table.root_uid,mq_table.root_gid,
        &mq_table.root_atime,&mq_table.root_mtime,&mq_table.root_ctime,times);
}
static size_t append_decimal(char *buffer,size_t at,uint64_t value,unsigned width)
{
    char digits[24];unsigned n=0;do{digits[n++]=(char)('0'+value%10);value/=10;}while(value);
    unsigned length=n;
    while(n)buffer[at++]=digits[--n];
    for(unsigned j=length;j<width;j++)buffer[at++]=' ';
    return at;
}
int kernel_mqueue_queue_read(struct kernel_mqueue *q,uint64_t offset,void *buffer,size_t size,size_t *read)
{
    char text[128];uint64_t bytes,pid,type,signo;
    {KERNEL_RAW_SCOPE(g,&q->lock);bytes=q->bytes;pid=q->notification.owner?(uint32_t)q->notification.owner->number:0;
     type=q->notification.owner?(uint32_t)q->notification.type:0;
     signo=q->notification.owner&&!q->notification.type?(uint32_t)q->notification.signal:0;
     q->atime=mq_now();}
    size_t n=0;
    memcpy(text+n,"QSIZE:",6);n+=6;n=append_decimal(text,n,bytes,10);
    memcpy(text+n," NOTIFY:",8);n+=8;n=append_decimal(text,n,type,5);
    memcpy(text+n," SIGNO:",7);n+=7;n=append_decimal(text,n,signo,5);
    memcpy(text+n," NOTIFY_PID:",12);n+=12;n=append_decimal(text,n,pid,6);text[n++]='\n';
    *read=0;if(offset>=n)return 0;
    size_t remaining=n-(size_t)offset;if(size>remaining)size=remaining;
    memcpy(buffer,text+(size_t)offset,size);*read=size;return 0;
}
int kernel_mqueue_read(struct kernel_open_file_description *f,uint64_t offset,void *buffer,size_t size,size_t *read)
{return kernel_mqueue_queue_read(file_queue(f),offset,buffer,size,read);}
int kernel_mqueue_name(struct kernel_open_file_description *f,char *buffer,size_t capacity)
{
    struct kernel_mqueue *q=file_queue(f);KERNEL_RAW_SCOPE(g,&q->lock);
    size_t n=strlen(q->name);size_t suffix=q->linked?0:10;
    if(n+suffix+2>capacity)return -KERNEL_ENAMETOOLONG;
    buffer[0]='/';memcpy(buffer+1,q->name,n);
    if(suffix)memcpy(buffer+n+1," (deleted)",10);
    buffer[n+suffix+1]=0;return 0;
}
int kernel_mqueue_reopen(struct kernel_open_file_description *source,struct kernel_heap *heap,
    uint32_t flags,struct kernel_open_file_description **owner)
{
    if((flags&3U)==3U)return -KERNEL_EINVAL;
    struct kernel_mqueue *q=file_queue(source);unsigned access=(flags&3U)==0?4:(flags&3U)==1?2:6;
    if(source->file.mount&&kernel_vfs_mount_is_readonly(source->file.mount)&&(access&2U))return -KERNEL_EROFS;
    int error=kernel_mqueue_queue_permission(q,access);
    if(!error)error=file_create(heap,q,flags,owner);
    return error;
}
int kernel_mqueue_fs_open(struct kernel_heap *heap,struct kernel_vfs_file *file,
    uint32_t flags,struct kernel_open_file_description **owner)
{
    /* fs/mqueue.c retains the queue in the node. Namespace unlink cannot
     * replace this inode while a resolved path still owns that node. */
    extern struct kernel_mqueue *kernel_mqueue_fs_file_queue(const struct kernel_vfs_file *);
    struct kernel_mqueue *q=kernel_mqueue_fs_file_queue(file);
    if(!q)return -KERNEL_EINVAL;
    int error=file_create(heap,q,flags,owner);
    if(!error){(*owner)->file=*file;*file=(struct kernel_vfs_file){0};}
    return error;
}
