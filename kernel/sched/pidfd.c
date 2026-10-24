#include "private.h"
#include <kernel/pidfd.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/socket_task.h>
#include <kernel/tty_task.h>
#include "../../fs/open_file_internal.h"
#include "../../fs/files/private.h"

#define PIDFD_THREAD 0200U
#define PIDFD_NONBLOCK 04000U
#define PIDFD_BUCKETS 64U
struct pidfd_observer {
    struct pidfd_observer *next;
    struct kernel_pid *identity;
    struct kernel_heap *heap;
    struct kernel_wait_queue changed;
    uint32_t references;
};
struct pidfd_file {
    struct kernel_open_file_description file;
    struct pidfd_observer *observer;
};
/* Entries exist only while actual pidfd OFDs or an in-flight notification own
 * them. The PID stays in its original 64-byte allocation class. */
static struct pidfd_observer *observers[PIDFD_BUCKETS];
static unsigned observer_bucket(const struct kernel_pid *id)
{ return (unsigned)((uintptr_t)id >> 6) & (PIDFD_BUCKETS - 1U); }
static struct pidfd_observer *observer_find(const struct kernel_pid *id)
{
    for (struct pidfd_observer *o=observers[observer_bucket(id)];o;o=o->next)
        if(o->identity==id)return o;
    return 0;
}
static void observer_put(struct pidfd_observer *o)
{
    int last;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(!o->references)__builtin_trap();
        last=!--o->references;
        if(last) {
            struct pidfd_observer **at=&observers[observer_bucket(o->identity)];
            while(*at && *at!=o)at=&(*at)->next;
            if(!*at)__builtin_trap();
            *at=o->next;
        }
    }
    if(!last)return;
    if(kernel_wait_queue_close(&o->changed))__builtin_trap();
    kernel_task_identity_put(o->identity);
    kernel_heap_release(o->heap,o);
}
static void notify_identity(struct kernel_pid *id)
{
    struct pidfd_observer *o;
    struct kernel_wait_cursor cursor={0};
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        o=observer_find(id);
        if(!o)return;
        if(o->references==UINT32_MAX)__builtin_trap();
        o->references++;
        (void)kernel_wait_cursor_prepare_locked(&o->changed,&cursor,1);
    }
    if(cursor.queue)kernel_wait_cursor_wake(&cursor,1);
    observer_put(o);
}
struct kernel_pid *kernel_pidfd_exit_identity(struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_pid *id=process_identity(task,KERNEL_PID_TID);
    if(id)kernel_pid_get(id);
    return id;
}
void kernel_pidfd_notification_end(struct kernel_pid **id)
{
    if(!*id)return;
    notify_identity(*id);
    kernel_task_identity_put(*id);
    *id=0;
}
static int install(struct kernel_files *files,struct kernel_open_file_description **owner)
{
    uint32_t fd; int error=0;
    enum kernel_files_status status=kernel_files_find_free_fd(files,&fd,&error);
    if(status) {
        if(status!=KERNEL_FILES_STATUS_NO_MEMORY)__builtin_trap();
        return -KERNEL_ENOMEM;
    }
    if(error)return error;
    KERNEL_FILES_RESERVATION_SCOPE(reservation);
    reservation=(struct kernel_files_fd_reservation){files,fd};
    if(kernel_files_install_shared_acquired_at(files,fd,KERNEL_FILES_FD_CLOEXEC,owner))
        __builtin_trap();
    reservation.files=0;
    return (int)fd;
}
int64_t kernel_pidfd_open(struct kernel_task *caller,int32_t number,uint32_t flags)
{
    KERNEL_SERVICE_SCOPE(service);
    if(flags & ~(PIDFD_THREAD|PIDFD_NONBLOCK))return -KERNEL_EINVAL;
    if(number<=0)return -KERNEL_EINVAL;
    struct kernel_pid *id __attribute__((cleanup(kernel_task_identity_end)))=0;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        id=kernel_pid_find(&scheduler.identities,number);
        if(!id || !id->members[KERNEL_PID_TID]) { id=0;return -KERNEL_ESRCH; }
        kernel_pid_get(id);
        if(!(flags&PIDFD_THREAD) && !id->members[KERNEL_PID_TGID])return -KERNEL_ENOENT;
    }
    uint32_t slot;int fd_error=0;
    enum kernel_files_status reserved=kernel_files_find_free_fd(&caller->files,&slot,&fd_error);
    if(reserved) {
        if(reserved!=KERNEL_FILES_STATUS_NO_MEMORY)__builtin_trap();
        return -KERNEL_ENOMEM;
    }
    if(fd_error)return fd_error;
    KERNEL_FILES_RESERVATION_SCOPE(reservation);
    reservation=(struct kernel_files_fd_reservation){&caller->files,slot};
    struct pidfd_file *file=0; struct pidfd_observer *candidate=0;
    struct kernel_heap *heap=caller->files.heap;
    enum kernel_heap_status allocated=kernel_heap_allocate_zeroed(heap,1,sizeof(*file),(void **)&file);
    if(allocated) {
        if(allocated!=KERNEL_HEAP_STATUS_EMPTY)__builtin_trap();
        return -KERNEL_ENOMEM;
    }
    /* Prepare before publication; no observer allocation occurs at exit. */
    allocated=kernel_heap_allocate_zeroed(heap,1,sizeof(*candidate),(void **)&candidate);
    if(allocated) {
        if(allocated!=KERNEL_HEAP_STATUS_EMPTY)__builtin_trap();
        kernel_heap_release(heap,file);return -KERNEL_ENOMEM;
    }
    candidate->heap=heap;kernel_wait_queue_init(&candidate->changed);
    int gone=0;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct pidfd_observer *o=observer_find(id);
        gone=!id->members[KERNEL_PID_TID];
        if(!gone && !o) {
            o=candidate;candidate=0;o->identity=id;kernel_pid_get(id);
            unsigned bucket=observer_bucket(id);o->next=observers[bucket];observers[bucket]=o;
        }
        if(!gone) {
            if(o->references==UINT32_MAX)__builtin_trap();
            o->references++;file->observer=o;
        }
    }
    if(candidate)kernel_heap_release(heap,candidate);
    if(gone){kernel_heap_release(heap,file);return -KERNEL_ESRCH;}
    file->file.heap=heap;file->file.references=1;file->file.kind=KERNEL_OPEN_FILE_KIND_PIDFD;
    file->file.open_flags=2U|flags;
    file->file.credentials=kernel_credentials_get(kernel_credentials_current());
    kernel_mutex_init(&file->file.offset_lock,10,(uintptr_t)file);
    struct kernel_open_file_description *owner=&file->file;
    if(kernel_files_install_new_owned_at(&caller->files,slot,KERNEL_FILES_FD_CLOEXEC,&owner))
        __builtin_trap();
    reservation.files=0;
    return slot;
}
void kernel_pidfd_close(struct kernel_open_file_description *file)
{
    struct pidfd_file *p=(void *)file;
    observer_put(p->observer);p->observer=0;
}
uint32_t kernel_pidfd_poll(struct kernel_open_file_description *file,struct kernel_wait_queue **queue)
{
    struct pidfd_observer *o=((struct pidfd_file *)file)->observer;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if(queue)*queue=&o->changed;
    struct kernel_pid_member *member=o->identity->members[KERNEL_PID_TID];
    if(!member)return KERNEL_POLLIN|KERNEL_POLLRDNORM|KERNEL_POLLHUP;
    struct kernel_task *task=member->task;
    if(task->state==KERNEL_THREAD_STATE_ZOMBIE && task->group_members<=1U)
        return KERNEL_POLLIN|KERNEL_POLLRDNORM;
    return 0;
}
int32_t kernel_pidfd_number(const struct kernel_open_file_description *file)
{
    const struct kernel_pid *id=((const struct pidfd_file *)file)->observer->identity;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    return id->members[KERNEL_PID_TID]?id->number:-1;
}
uint64_t kernel_pidfd_inode(const struct kernel_open_file_description *file)
{ return ((const struct pidfd_file *)file)->observer->identity->generation; }
int kernel_pidfd_wait_target(struct kernel_files *files,int32_t fd,struct kernel_pid **owner,int *nonblock)
{
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *file=kernel_files_hold_fd(files,fd,&pin);
    if(!file || file->kind!=KERNEL_OPEN_FILE_KIND_PIDFD)return -KERNEL_EBADF;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    *owner=((struct pidfd_file *)file)->observer->identity;
    kernel_pid_get(*owner);
    *nonblock=!!(kernel_open_file_flags(file)&PIDFD_NONBLOCK);
    return 0;
}
int kernel_pidfd_signal_target(struct kernel_task *caller,int32_t fd,uint32_t flags,
    struct kernel_pid **owner,unsigned *selected_scope)
{
    KERNEL_SERVICE_SCOPE(service);
    if((flags&~7U) || (flags && (flags&(flags-1U))))return -KERNEL_EINVAL;
    struct kernel_pid *id=0;
    unsigned scope=1;
    if(fd==-10000 || fd==-10001) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        scope=fd==-10000?0:1;
        id=process_identity(caller,scope?KERNEL_PID_TGID:KERNEL_PID_TID);kernel_pid_get(id);
    } else {
        KERNEL_FILES_PIN_SCOPE(pin);
        struct kernel_open_file_description *file=kernel_files_hold_fd(&caller->files,fd,&pin);
        if(!file || file->kind!=KERNEL_OPEN_FILE_KIND_PIDFD)return -KERNEL_EBADF;
        scope=!!(file->open_flags&PIDFD_THREAD)?0:1;
        {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);id=((struct pidfd_file *)file)->observer->identity;kernel_pid_get(id);}
    }
    if(flags)scope=flags==1?0:flags==2?1:2;
    *owner=id;*selected_scope=scope;return 0;
}
int64_t kernel_pidfd_send(struct kernel_task *caller,struct kernel_pid *id,unsigned scope,
    uint32_t signal,const struct kernel_signal_wait_info *info)
{
    if(info && (info->signal!=signal))return -KERNEL_EINVAL;
    if(info && (info->code>=0 || info->code==-6)) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(scope==2 || id!=process_identity(caller,KERNEL_PID_TID))return -KERNEL_EPERM;
    }
    return kernel_signal_send_identity(id,scope,signal,info);
}
int64_t kernel_pidfd_getfd(struct kernel_task *caller,int32_t fd,int32_t targetfd,uint32_t flags)
{
    KERNEL_SERVICE_SCOPE(service);
    if(flags)return -KERNEL_EINVAL;
    struct kernel_pid *id __attribute__((cleanup(kernel_task_identity_end)))=0;
    int unused=0,error=kernel_pidfd_wait_target(&caller->files,fd,&id,&unused);
    if(error)return error;
    SCHEDULER_TASK_BORROW(target,0);
    const struct kernel_credentials *credentials=0;
    struct kernel_exec_security *security=0; struct kernel_mm_record *mm=0;
    struct kernel_files_record *files_record=0;
    struct kernel_open_file_description *owner=0;int64_t result=0;
    /* CPU0 owns exec/exit control, but a timer can switch to another CPU0
     * task. Keep the target handle stable through pin and the security recheck;
     * the FD lock is taken after dropping the higher-ranked wait domain. */
    kernel_preempt_disable();
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_pid_member *member=id->members[KERNEL_PID_TID];
        if(!member)result=-KERNEL_ESRCH;
        else {
            target=member->task;scheduler_task_get_locked(target);
            if(!kernel_credentials_may_access_locked(caller,target,0))result=-KERNEL_EPERM;
            else if(target->proc_exiting || !kernel_files_is_live(&target->files))result=-KERNEL_ESRCH;
            else {
                credentials=kernel_credentials_get(kernel_credentials_task_locked(target));
                security=kernel_exec_security_get(target->exec_security);mm=target->mm.record;
                files_record=target->files.record;
            }
        }
    }
    if(!result) {
        enum kernel_files_status status=kernel_files_pin(&target->files,targetfd,&owner,&result);
        if(status)__builtin_trap();
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(target->mm.record!=mm || target->exec_security!=security ||
            kernel_credentials_task_locked(target)!=credentials ||
            !kernel_credentials_may_access_locked(caller,target,0))result=-KERNEL_EPERM;
        else if(target->proc_exiting || target->files.record!=files_record)result=-KERNEL_ESRCH;
    }
    kernel_preempt_enable();
    kernel_credentials_put(credentials);kernel_exec_security_put(security);
    if(!result)result=install(&caller->files,&owner);
    if(owner) {
        enum kernel_open_file_status released=kernel_open_file_release(&owner);
        if(released==KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED)kernel_files_queue_description(&caller->files,owner);
        else if(released)__builtin_trap();
    }
    return result;
}
