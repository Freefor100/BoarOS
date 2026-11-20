#include <kernel/inotify.h>
#include <kernel/async_io.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/fs_context.h>
#include <kernel/wait_internal.h>
#include "char_device_internal.h"
#include "files/private.h"
#include "open_file_internal.h"
#include "uaccess_iov_internal.h"
#include "vfs_objects.h"
#include "vfs_notify.h"
#include "../third_party/lwext4/include/misc/tree.h"
#include <string.h>

#define IN_NONBLOCK 04000U
#define IN_CLOEXEC 02000000U
#define IN_ONLYDIR 0x01000000U
#define IN_DONT_FOLLOW 0x02000000U
#define IN_EXCL_UNLINK 0x04000000U
#define IN_MASK_CREATE 0x10000000U
#define IN_MASK_ADD 0x20000000U
#define IN_ONESHOT 0x80000000U
#define IN_Q_OVERFLOW 0x4000U
#define IN_IGNORED 0x8000U
#define IN_EVENTS 0xfffU
#define IN_ALLOWED (IN_EVENTS|VFS_NOTIFY_UNMOUNT|IN_Q_OVERFLOW|IN_IGNORED|VFS_NOTIFY_ISDIR| \
    IN_ONLYDIR|IN_DONT_FOLLOW|IN_EXCL_UNLINK|IN_MASK_CREATE|IN_MASK_ADD|IN_ONESHOT)
#define IN_RING_SIZE 65536U
#define IN_MAX_GROUPS 128U
#define IN_MAX_MARKS 65536U
#define IN_BUCKETS 256U
#define IN_WD_BUCKETS 256U
#define IN_WD_MAX_CHAIN 16U

struct notify_event { int32_t wd; uint32_t mask, cookie, len; };
struct inotify_group;
RB_HEAD(inotify_descriptors,inotify_mark);
struct inotify_mark {
    struct inotify_group *group;
    struct kernel_heap *heap;
    struct kernel_vfs_instance *instance;
    struct inotify_mark *next, **previous, *group_next, **group_previous;
    struct inotify_mark *retired_next,*dispose_next;
    unsigned references, wd_overflow;
    uint64_t inode;
    int wd;
    uint32_t mask;
    union {
        struct {
            struct inotify_mark *wd_next, **wd_previous;
        };
        RB_ENTRY(inotify_mark) wd_link;
    };
};
struct inotify_group {
    struct kernel_heap *heap;
    struct kernel_uid_account *account;
    struct kernel_wait_queue queue;
    struct kernel_wait_queue async_events;
    struct kernel_wait_cursor notification;
    struct inotify_group *pending;
    struct inotify_mark *marks;
    unsigned references, head, used, last, next_wd, overflow, pending_wake;
    /* The descriptor index and the inode and group lists are published together
     * under notify_lock. Cap hash chains even when active descriptors are sparse. */
    struct inotify_mark *descriptors[IN_WD_BUCKETS];
    uint8_t descriptor_counts[IN_WD_BUCKETS];
    struct inotify_descriptors overflow_descriptors;
    unsigned char ring[IN_RING_SIZE];
};
static struct kernel_raw_lock notify_lock = {
    .rank=KERNEL_RAW_RANK_OBJECT, .initialized=KERNEL_RAW_INITIALIZED
};
static struct inotify_mark *buckets[IN_BUCKETS];
static struct inotify_group *notifications;

static const struct kernel_vfs_notify_ops inotify_ops;
static const struct kernel_char_device inotify_file_ops;

static struct inotify_mark **bucket(struct kernel_vfs_instance *instance,uint64_t inode)
{
    uintptr_t key=((uintptr_t)instance>>4) ^ inode ^ (inode>>32);
    key ^= key>>8;
    return &buckets[key & (IN_BUCKETS-1)];
}

static int descriptor_compare(const struct inotify_mark *a,const struct inotify_mark *b)
{ return (a->wd>b->wd)-(a->wd<b->wd); }
RB_GENERATE_INTERNAL(inotify_descriptors,inotify_mark,wd_link,descriptor_compare,static inline)

static unsigned descriptor_slot(int wd)
{
    return (unsigned)wd&(IN_WD_BUCKETS-1);
}

static void descriptor_insert(struct inotify_mark *mark)
{
    struct inotify_group *group=mark->group;
    unsigned slot=descriptor_slot(mark->wd);
    mark->wd_overflow=group->descriptor_counts[slot]==IN_WD_MAX_CHAIN;
    if (mark->wd_overflow) {
        if (RB_INSERT(inotify_descriptors,&group->overflow_descriptors,mark)) __builtin_trap();
        return;
    }
    struct inotify_mark **head=&group->descriptors[slot];
    mark->wd_next=*head;
    mark->wd_previous=head;
    if (*head) (*head)->wd_previous=&mark->wd_next;
    *head=mark;
    group->descriptor_counts[slot]++;
}

static void descriptor_remove(struct inotify_mark *mark)
{
    struct inotify_group *group=mark->group;
    if (mark->wd_overflow) {
        RB_REMOVE(inotify_descriptors,&group->overflow_descriptors,mark);
        return;
    }
    *mark->wd_previous=mark->wd_next;
    if (mark->wd_next) mark->wd_next->wd_previous=mark->wd_previous;
    group->descriptor_counts[descriptor_slot(mark->wd)]--;
}

static void ring_read(const struct inotify_group *group,unsigned pos,void *out,unsigned n)
{
    unsigned first=IN_RING_SIZE-pos;
    if (first>n) first=n;
    memcpy(out,group->ring+pos,first);
    if (n>first) memcpy((char *)out+first,group->ring,n-first);
}
static void ring_write(struct inotify_group *group,unsigned pos,const void *in,unsigned n)
{
    unsigned first=IN_RING_SIZE-pos;
    if (first>n) first=n;
    memcpy(group->ring+pos,in,first);
    if (n>first) memcpy(group->ring,(const char *)in+first,n-first);
}
static void group_put(struct inotify_group *group)
{
    unsigned last;
    {
        KERNEL_RAW_SCOPE(guard,&notify_lock);
        if (!group->references) __builtin_trap();
        last=!--group->references;
        if (last) group->account->inotify_groups--;
    }
    if (last) {
        if (kernel_wait_queue_destroy(&group->queue)) __builtin_trap();
        if (kernel_wait_queue_destroy(&group->async_events)) __builtin_trap();
        kernel_uid_account_put(group->account);
        kernel_heap_release(group->heap,group);
    }
}

/* Poll subscriptions and sleeping readers are borrowed before publication
 * leaves the lock. The group survives even if the last OFD is closed meanwhile. */
static void notify_readers(struct inotify_group *group)
{
    if (group->pending_wake ||
        !kernel_wait_cursor_prepare(&group->queue,&group->notification)) return;
    group->pending_wake=1;
    group->references++;
    group->pending=notifications;
    notifications=group;
}
static void drain_notifications(void)
{
    for (;;) {
        struct inotify_group *group;
        struct kernel_wait_cursor cursor;
        {
            KERNEL_RAW_SCOPE(guard,&notify_lock);
            group=notifications;
            if (!group) return;
            notifications=group->pending;
            cursor=group->notification;
            group->notification=(struct kernel_wait_cursor){0};
            group->pending_wake=0;
        }
        kernel_wait_cursor_wake(&cursor,1);
        group_put(group);
    }
}

static int enqueue(struct inotify_group *group,int wd,uint32_t mask,uint32_t cookie,
    const char *name,struct kernel_wait_cursor *async)
{
    struct notify_event event={wd,mask,cookie,0};
    unsigned length=name?(unsigned)strlen(name)+1:0;
    if (length>256) __builtin_trap();
    if (length) event.len=(length+15)&~15U;
    if (group->used) {
        struct notify_event last;
        ring_read(group,group->last,&last,sizeof(last));
        if (last.wd==wd && last.mask==mask && last.cookie==cookie && last.len==event.len) {
            if (!length) return 0;
            char old[256];
            ring_read(group,(group->last+sizeof(last))&(IN_RING_SIZE-1),old,length);
            if (!memcmp(old,name,length)) return 0;
        }
    }
    if (group->used+sizeof(event)+event.len > IN_RING_SIZE-sizeof(event)) {
        if (group->overflow) return 0;
        event=(struct notify_event){-1,IN_Q_OVERFLOW,0,0};
        length=0;
        group->overflow=1;
    }
    unsigned pos=(group->head+group->used)&(IN_RING_SIZE-1);
    group->last=pos;
    ring_write(group,pos,&event,sizeof(event));
    pos=(pos+sizeof(event))&(IN_RING_SIZE-1);
    if (length) {
        static const char zeros[16];
        ring_write(group,pos,name,length);
        if (event.len>length)
            ring_write(group,(pos+length)&(IN_RING_SIZE-1),zeros,event.len-length);
    }
    group->used+=sizeof(event)+event.len;
    notify_readers(group);
    if (async && kernel_wait_cursor_prepare_event(&group->async_events,async,KERNEL_ASYNC_POLL_IN,NULL))
        group->references++;
    return 1;
}

static void mark_get_locked(struct inotify_mark *mark)
{
    if (!mark->references || mark->references==UINT32_MAX || mark->group->references==UINT32_MAX)
        __builtin_trap();
    mark->references++;mark->group->references++;
}
static void remove_mark(struct inotify_mark *mark,struct inotify_mark **retired,int ignored,
    struct kernel_wait_cursor *async)
{
    if (ignored) enqueue(mark->group,mark->wd,IN_IGNORED,0,NULL,async);
    /* A borrowed removed mark retains its traversal successor. The dispose
     * chain is separate, so concurrent removals never overwrite a walker. */
    mark->retired_next=NULL;
    if (mark->references>1 && mark->next) {
        mark_get_locked(mark->next);mark->retired_next=mark->next;
    }
    *mark->previous=mark->next;
    if (mark->next) mark->next->previous=mark->previous;
    *mark->group_previous=mark->group_next;
    if (mark->group_next) mark->group_next->group_previous=mark->group_previous;
    descriptor_remove(mark);
    if (!--mark->instance->notify_marks)
        __atomic_store_n(&mark->instance->notify_ops,NULL,__ATOMIC_RELEASE);
    mark->group->account->inotify_marks--;
    mark->previous=NULL;mark->group_previous=NULL;
    if (!--mark->references) {mark->dispose_next=*retired;*retired=mark;}
}
static void dispose_marks(struct inotify_mark *list)
{
    while (list) {
        struct inotify_mark *next=list->dispose_next;
        kernel_heap_release(list->heap,list);
        list=next;
    }
}
static void mark_put(struct inotify_mark *mark)
{
    while (mark) {
        struct inotify_mark *next=NULL;
        struct inotify_group *group=mark->group;
        int release;
        {
            KERNEL_RAW_SCOPE(guard,&notify_lock);
            if (!mark->references) __builtin_trap();
            release=!--mark->references;
            if (release) {if (mark->previous) __builtin_trap();next=mark->retired_next;}
        }
        if (release) kernel_heap_release(mark->heap,mark);
        group_put(group);
        mark=next;
    }
}
static void deliver_async(struct inotify_group *group,struct kernel_wait_cursor *cursor)
{
    if (!cursor->queue) return;
    kernel_wait_cursor_wake(cursor,1);group_put(group);
}
static void emit(struct kernel_vfs_instance *instance,uint64_t inode,uint32_t mask,
    uint32_t cookie,const char *name,int detached)
{
    struct inotify_mark *m;
    {KERNEL_RAW_SCOPE(guard,&notify_lock);m=*bucket(instance,inode);if (m) mark_get_locked(m);}
    while (m) {
        struct inotify_mark *next,*retired=NULL;
        struct inotify_group *group=m->group;
        struct kernel_wait_cursor async[2]={{0},{0}};
        {
            KERNEL_RAW_SCOPE(guard,&notify_lock);
            next=m->previous?m->next:m->retired_next;
            if (next) mark_get_locked(next);
            if (m->previous && m->instance==instance && m->inode==inode) {
                if ((m->mask&mask&IN_EVENTS) && !(detached && (m->mask&IN_EXCL_UNLINK))) {
                    enqueue(group,m->wd,mask,cookie,name,&async[0]);
                    if (m->mask&IN_ONESHOT) remove_mark(m,&retired,1,&async[1]);
                }
                if (m->previous && (mask&VFS_NOTIFY_DELETE_SELF)) remove_mark(m,&retired,1,&async[1]);
            }
        }
        deliver_async(group,&async[0]);deliver_async(group,&async[1]);
        dispose_marks(retired);mark_put(m);m=next;
    }
    drain_notifications();
}
static void unmount(struct kernel_vfs_instance *instance)
{
    /* Last-view retirement excludes new marks. Return each bucket's storage
     * without holding the lock across the full filesystem teardown. */
    for (unsigned i=0;i<IN_BUCKETS;i++) {
        struct inotify_mark *m;
        {KERNEL_RAW_SCOPE(guard,&notify_lock);m=buckets[i];if (m)mark_get_locked(m);}
        while (m) {
            struct inotify_mark *next,*retired=NULL;
            struct inotify_group *group=m->group;
            struct kernel_wait_cursor async[2]={{0},{0}};
            {
                KERNEL_RAW_SCOPE(guard,&notify_lock);
                next=m->previous?m->next:m->retired_next;
                if (next)mark_get_locked(next);
                if (m->previous && m->instance==instance) {
                    enqueue(group,m->wd,VFS_NOTIFY_UNMOUNT,0,NULL,&async[0]);
                    remove_mark(m,&retired,1,&async[1]);
                }
            }
            deliver_async(group,&async[0]);deliver_async(group,&async[1]);
            dispose_marks(retired);mark_put(m);m=next;
        }
    }
    drain_notifications();
}
static const struct kernel_vfs_notify_ops inotify_ops={emit,unmount};

static void release_group(void *object)
{
    struct inotify_group *group=object;
    for (;;) {
        struct inotify_mark *retired=NULL;
        {
            KERNEL_RAW_SCOPE(guard,&notify_lock);
            if (!group->marks) break;
            for (unsigned i=0;i<32 && group->marks;i++)
                remove_mark(group->marks,&retired,0,NULL);
        }
        dispose_marks(retired);
    }
    if (kernel_wait_queue_close(&group->queue)) __builtin_trap();
    if (kernel_wait_queue_close(&group->async_events)) __builtin_trap();
    group_put(group);
}

static int64_t read_events(struct inotify_group *group,struct kernel_mm *mm,
    struct kernel_uaccess_iov_cursor *cursor,uint64_t count,uint32_t flags)
{
    uint64_t done=0;
    for (;;) {
        struct kernel_wait_token token;
        unsigned char record[sizeof(struct notify_event)+256];
        unsigned size=0;
        {
            KERNEL_RAW_SCOPE(guard,&notify_lock);
            if (group->used) {
                struct notify_event event;
                ring_read(group,group->head,&event,sizeof(event));
                size=sizeof(event)+event.len;
                if (size>count-done) return done?(int64_t)done:-KERNEL_EINVAL;
                ring_read(group,group->head,record,size);
                group->head=(group->head+size)&(IN_RING_SIZE-1);
                group->used-=size;
                if (event.mask==IN_Q_OVERFLOW) group->overflow=0;
            } else if (done || (flags&IN_NONBLOCK)) {
                return done?(int64_t)done:-KERNEL_EAGAIN;
            } else if (kernel_wait_prepare(&group->queue,0,1,&token)) {
                return -KERNEL_EIO;
            }
        }
        if (size) {
            size_t copied=0;
            if (kernel_copy_to_user_iov(mm,cursor,record,size,&copied) || copied!=size)
                return -KERNEL_EFAULT;
            done+=size;
        } else {
            enum kernel_wait_wake_reason reason;
            enum kernel_scheduler_status status=kernel_wait_park(&token,&reason);
            if (kernel_wait_finish(&token) || status) return -KERNEL_EIO;
            if (reason==KERNEL_WAIT_SIGNALLED) return -KERNEL_ERESTARTSYS;
        }
    }
}
static enum kernel_files_status inotify_readv(void *object,struct kernel_task *task,
    struct kernel_files *files,struct kernel_open_file_description **owner,void **iov_owner,
    struct kernel_mm *mm,const struct kernel_uaccess_iovec *iov,size_t count,
    uint64_t bytes,uint32_t flags,int64_t *result)
{
    (void)task;(void)files;(void)owner;(void)iov_owner;
    *result=0;
    /* Linux's readv adapter invokes the scalar reader per nonempty segment.
     * A record must fit in a single segment and is never split between two. */
    for (size_t i=0;i<count;i++) {
        uint64_t length=iov[i].length;
        if (length>bytes) length=bytes;
        if (!length && iov_owner) continue;
        struct kernel_uaccess_iov_cursor cursor={iov+i,1,0,0};
        int64_t got=read_events(object,mm,&cursor,length,flags);
        if (got<0) {
            if (!*result) *result=got;
            break;
        }
        *result+=got;
        bytes-=got;
        if ((uint64_t)got!=length || !bytes) break;
    }
    return KERNEL_FILES_STATUS_OK;
}
static uint32_t inotify_poll(void *object,uint32_t flags,uint32_t requested,
    struct kernel_wait_queue **queue)
{
    (void)flags;(void)requested;
    struct inotify_group *group=object;
    KERNEL_RAW_SCOPE(guard,&notify_lock);
    if (queue) *queue=&group->queue;
    return group->used?KERNEL_POLLIN|KERNEL_POLLRDNORM:0;
}
static struct kernel_wait_queue *inotify_async_source(void *object)
{return &((struct inotify_group *)object)->async_events;}
static int inotify_ioctl(void *object,struct kernel_task *task,struct kernel_files *files,
    struct kernel_open_file_description **owner,struct kernel_mm *mm,uint64_t cmd,uint64_t arg)
{
    (void)task;(void)files;(void)owner;
    if (cmd!=0x541b) return -KERNEL_ENOTTY; /* FIONREAD */
    struct inotify_group *group=object;
    uint32_t bytes;
    { KERNEL_RAW_SCOPE(guard,&notify_lock);bytes=group->used; }
    size_t copied=0;
    return kernel_copy_to_user(mm,arg,&bytes,sizeof(bytes),&copied) ||
        copied!=sizeof(bytes)?-KERNEL_EFAULT:0;
}
static const struct kernel_char_device inotify_file_ops={
    .kind=KERNEL_OPEN_FILE_KIND_INOTIFY, .release=release_group,
    .readv=inotify_readv, .poll=inotify_poll, .ioctl=inotify_ioctl,.async_source=inotify_async_source
};

static enum kernel_files_status create(struct kernel_files *files,uint32_t flags,int64_t *result)
{
    if (flags&~(IN_CLOEXEC|IN_NONBLOCK)) {*result=-KERNEL_EINVAL;return 0;}
    unsigned fd;int error;
    enum kernel_files_status status=kernel_files_find_free_fd(files,&fd,&error);
    if (status || error) {*result=error;return status;}
    KERNEL_FILES_RESERVATION_SCOPE(reservation);
    reservation.files=files;reservation.fd=fd;
    struct inotify_group *group=NULL;
    struct kernel_open_file_description *file=NULL;
    const struct kernel_credentials *cred=kernel_credentials_current();
    struct kernel_uid_account *account=kernel_uid_account_get(files->heap,cred->uid);
    if (!account) {*result=-KERNEL_ENOMEM;return 0;}
    int charged=0;
    {
        KERNEL_RAW_SCOPE(guard,&notify_lock);
        if (account->inotify_groups<IN_MAX_GROUPS) {
            account->inotify_groups++;charged=1;
        }
    }
    if (!charged) {
        kernel_uid_account_put(account);*result=-KERNEL_EMFILE;return 0;
    }
    if (kernel_heap_allocate_zeroed(files->heap,1,sizeof(*group),(void **)&group) ||
        kernel_heap_allocate_zeroed(files->heap,1,sizeof(*file),(void **)&file)) {
        if (group) kernel_heap_release(files->heap,group);
        {KERNEL_RAW_SCOPE(guard,&notify_lock);account->inotify_groups--;}
        kernel_uid_account_put(account);
        *result=-KERNEL_ENOMEM;return 0;
    }
    group->heap=files->heap;group->references=1;group->account=account;
    kernel_wait_queue_init(&group->queue);
    kernel_wait_queue_init(&group->async_events);
    file->heap=files->heap;file->references=1;file->kind=KERNEL_OPEN_FILE_KIND_INOTIFY;
    file->open_flags=flags&IN_NONBLOCK;file->device=&inotify_file_ops;file->device_instance=group;
    file->credentials=kernel_credentials_get(cred);
    kernel_mutex_init(&file->offset_lock,10,(uintptr_t)file);
    status=kernel_files_install_new_owned_at(files,fd,
        flags&IN_CLOEXEC?KERNEL_FILES_FD_CLOEXEC:0,&file);
    if (status) {
        if (kernel_open_file_release(&file)) __builtin_trap();
        return status;
    }
    reservation.files=NULL;*result=fd;return 0;
}

static int watch_install(struct inotify_group *group,struct kernel_vfs_instance *instance,
    uint64_t inode,uint32_t mask,struct inotify_mark *candidate)
{
    struct inotify_mark **head=bucket(instance,inode);
    for (struct inotify_mark *m=*head;m;m=m->next) {
        if (m->group!=group || m->instance!=instance || m->inode!=inode) continue;
        if (mask&IN_MASK_CREATE) return -KERNEL_EEXIST;
        uint32_t bits=mask&(IN_EVENTS|IN_ONESHOT|IN_EXCL_UNLINK);
        m->mask=(mask&IN_MASK_ADD)?m->mask|bits:bits;
        return m->wd;
    }
    if (!candidate) return -KERNEL_ENOMEM;
    if (group->account->inotify_marks==IN_MAX_MARKS || group->next_wd==INT32_MAX) return -KERNEL_ENOSPC;
    candidate->group=group;candidate->instance=instance;candidate->inode=inode;
    candidate->mask=mask&(IN_EVENTS|IN_ONESHOT|IN_EXCL_UNLINK);
    candidate->wd=(int)++group->next_wd;
    candidate->next=*head;candidate->previous=head;
    if (*head) (*head)->previous=&candidate->next;
    *head=candidate;
    candidate->references=1;candidate->retired_next=NULL;candidate->dispose_next=NULL;
    candidate->group_next=group->marks;candidate->group_previous=&group->marks;
    if (group->marks) group->marks->group_previous=&candidate->group_next;
    group->marks=candidate;
    descriptor_insert(candidate);
    if (!instance->notify_marks++)
        __atomic_store_n(&instance->notify_ops,&inotify_ops,__ATOMIC_RELEASE);
    group->account->inotify_marks++;
    return candidate->wd;
}
static int watch_add(struct inotify_group *group,const struct kernel_fs_context *fs,
    struct kernel_mm *mm,uint64_t user_path,uint32_t mask)
{
    char *name;
    if (kernel_heap_allocate(group->heap,KERNEL_FS_PATH_MAX,(void **)&name)) return -KERNEL_ENOMEM;
    size_t length=0;
    enum kernel_uaccess_status access=kernel_copy_string_from_user(mm,name,user_path,
        KERNEL_FS_PATH_MAX,&length);
    struct kernel_vfs_path *path=NULL;
    int error;
    if (access) error=access==KERNEL_UACCESS_STATUS_TOO_LONG?-KERNEL_ENAMETOOLONG:-KERNEL_EFAULT;
    else if (!name[0]) error=-KERNEL_ENOENT;
    else {
        KERNEL_FS_PATHS(paths,fs);
        error=kernel_vfs_path_resolve(paths.cwd,paths.root,name,!(mask&IN_DONT_FOLLOW),&path);
    }
    kernel_heap_release(group->heap,name);
    if (error) return error;
    struct kernel_vfs_node *node=path->file.private_data;
    if ((mask&IN_ONLYDIR) && (node->mode&KERNEL_VFS_S_IFMT)!=KERNEL_VFS_S_IFDIR)
        error=-KERNEL_ENOTDIR;
    else error=kernel_vfs_path_permission(path,KERNEL_VFS_MAY_READ);
    if (!error) {
        {KERNEL_RAW_SCOPE(guard,&notify_lock);error=watch_install(group,node->instance,node->inode,mask,NULL);}
        if (error==-KERNEL_ENOMEM) {
            struct inotify_mark *candidate=NULL;
            if (!kernel_heap_allocate(group->heap,sizeof(*candidate),(void **)&candidate)) {
                candidate->heap=group->heap;
                {
                    KERNEL_RAW_SCOPE(guard,&notify_lock);
                    error=watch_install(group,node->instance,node->inode,mask,candidate);
                    if (candidate==group->marks) candidate=NULL;
                }
                if (candidate) kernel_heap_release(group->heap,candidate);
            }
        }
    }
    if (kernel_vfs_path_release(&path)) __builtin_trap();
    return error;
}

static int watch_remove(struct inotify_group *group,int wd)
{
    struct inotify_mark *retired=NULL;
    struct kernel_wait_cursor async={0};
    int result=-KERNEL_EINVAL;
    {
        KERNEL_RAW_SCOPE(guard,&notify_lock);
        struct inotify_mark *m=group->descriptors[descriptor_slot(wd)];
        while (m && m->wd!=wd) m=m->wd_next;
        if (!m && !RB_EMPTY(&group->overflow_descriptors)) {
            struct inotify_mark key={.wd=wd};
            m=RB_FIND(inotify_descriptors,&group->overflow_descriptors,&key);
        }
        if (m) {
            remove_mark(m,&retired,1,&async);
            result=0;
        }
    }
    deliver_async(group,&async);dispose_marks(retired);drain_notifications();
    return result;
}

enum kernel_files_status kernel_files_inotify(struct kernel_files *files,
    const struct kernel_fs_context *fs,struct kernel_mm *mm,unsigned syscall,
    int fd,uint64_t path_or_watch,uint32_t mask,int64_t *result)
{
    if (syscall==26) return create(files,(uint32_t)fd,result);
    if (syscall==27 && (!(mask&IN_ALLOWED) || (mask&~IN_ALLOWED))) {
        *result=-KERNEL_EINVAL;return 0;
    }
    struct kernel_open_file_description *file=NULL;
    enum kernel_files_status status=kernel_files_pin(files,fd,&file,result);
    if (status || *result) return status;
    if (file->kind!=KERNEL_OPEN_FILE_KIND_INOTIFY) *result=-KERNEL_EINVAL;
    else if (syscall==27) {
        *result=(mask&IN_MASK_CREATE) && (mask&IN_MASK_ADD)?-KERNEL_EINVAL:
            watch_add(file->device_instance,fs,mm,path_or_watch,mask);
    } else {
        *result=watch_remove(file->device_instance,(int32_t)path_or_watch);
    }
    if (kernel_open_file_release(&file)) {
        if (!file) __builtin_trap();
        kernel_files_queue_description(files,file);
    }
    return 0;
}
