#include "open_file_internal.h"
#include "files/private.h"
#include "uaccess_iov_internal.h"
#include <kernel/event_fd.h>
#include <kernel/heap.h>
#include <kernel/errno.h>
#include <kernel/raw_lock.h>
#include <kernel/wait_internal.h>
#include <kernel/signal.h>
#include <kernel/task.h>

#define EVENT_SEMAPHORE 1U
#define EVENT_NONBLOCK 04000U
#define EVENT_CLOEXEC 02000000U
struct kernel_event_fd {
    /* The OFD allocation owns this object; dup/fork share exactly this counter. */
    struct kernel_open_file_description file;
    struct kernel_raw_lock lock;
    struct kernel_wait_queue waiters;
    uint64_t count;
    uint32_t semaphore;
};
static struct kernel_event_fd *event_of(struct kernel_open_file_description *file)
{ return (struct kernel_event_fd *)file; }

static enum kernel_files_status install_event(struct kernel_files *files,
    struct kernel_event_fd *event, uint32_t flags, int64_t *result)
{
    uint32_t fd; int error;
    enum kernel_files_status status=kernel_files_find_free_fd(files,&fd,&error);
    if (status || error) {
        struct kernel_open_file_description *owner=&event->file;
        if (kernel_open_file_release(&owner)) __builtin_trap();
        *result=error; return status;
    }
    KERNEL_FILES_RESERVATION_SCOPE(reservation); reservation.files=files;reservation.fd=fd;
    struct kernel_open_file_description *owner=&event->file;
    status=kernel_files_install_new_owned_at(files,fd,
        (flags&EVENT_CLOEXEC)?KERNEL_FILES_FD_CLOEXEC:0,&owner);
    if (status) { if (kernel_open_file_release(&owner)) __builtin_trap();return status; }
    reservation.files=0;*result=fd;return KERNEL_FILES_STATUS_OK;
}
enum kernel_files_status kernel_files_eventfd_create(struct kernel_files *files,
    uint32_t initial, uint32_t flags, int64_t *result)
{
    if (!kernel_files_is_live(files)||!result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (flags&~(EVENT_SEMAPHORE|EVENT_NONBLOCK|EVENT_CLOEXEC)) { *result=-KERNEL_EINVAL;return 0; }
    struct kernel_event_fd *event=0;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(files->heap,1,sizeof(*event),(void **)&event);
    if (status==KERNEL_HEAP_STATUS_EMPTY) { *result=-KERNEL_ENOMEM;return 0; }
    if (status) return KERNEL_FILES_STATUS_STATE;
    event->file.heap=files->heap;event->file.references=1;
    event->file.kind=KERNEL_OPEN_FILE_KIND_EVENTFD;
    event->file.open_flags=2U|(flags&EVENT_NONBLOCK);
    kernel_mutex_init(&event->file.offset_lock,10,(uintptr_t)event);
    kernel_raw_lock_init(&event->lock,KERNEL_RAW_RANK_OBJECT);
    kernel_wait_queue_init(&event->waiters);
    event->count=initial;event->semaphore=!!(flags&EVENT_SEMAPHORE);
    return install_event(files,event,flags,result);
}
void kernel_event_fd_close(struct kernel_open_file_description *file)
{
    struct kernel_event_fd *event=event_of(file);
    if (kernel_wait_queue_close(&event->waiters) || kernel_wait_queue_destroy(&event->waiters)) __builtin_trap();
}
uint32_t kernel_event_fd_poll(struct kernel_open_file_description *file,struct kernel_wait_queue **queue)
{
    struct kernel_event_fd *event=event_of(file);
    KERNEL_RAW_SCOPE(guard,&event->lock);
    if (queue) *queue=&event->waiters;
    return (event->count?KERNEL_POLLIN:0U) | (event->count<UINT64_MAX-1?KERNEL_POLLOUT:0U);
}
/* The actual caller holds the OFD. State and registration share the object
 * lock, while user faults, callback invocation and parking happen outside it. */
static int event_transfer(struct kernel_event_fd *event,uint64_t *value,int writing,uint32_t flags)
{
    for (;;) {
        struct kernel_wait_token token={0};struct kernel_wait_cursor cursor={0};
        int wait=0,notify=0;
        {
            KERNEL_RAW_SCOPE(guard,&event->lock);
            wait=writing ? *value>UINT64_MAX-1-event->count : !event->count;
            if (wait) {
                if (flags&EVENT_NONBLOCK) return -KERNEL_EAGAIN;
                if (kernel_wait_prepare(&event->waiters,0,1,&token)) __builtin_trap();
            } else {
                if (writing) event->count+=*value;
                else { *value=event->semaphore?1:event->count;event->count-=*value; }
                notify=kernel_wait_cursor_prepare(&event->waiters,&cursor);
            }
        }
        if (notify) kernel_wait_cursor_wake(&cursor,0);
        if (!wait) return 0;
        enum kernel_wait_wake_reason reason;
        enum kernel_scheduler_status status=kernel_wait_park(&token,&reason);
        if (kernel_wait_finish(&token)||status) __builtin_trap();
        if (reason==KERNEL_WAIT_SIGNALLED) {
            kernel_signal_note_syscall_restart(kernel_task_current());return -KERNEL_EINTR;
        }
    }
}
int64_t kernel_event_fd_io(struct kernel_open_file_description *file,struct kernel_mm *mm,
    const struct kernel_uaccess_iovec *iov,size_t count,uint64_t length,int writing)
{
    struct kernel_event_fd *event=event_of(file);
    uint32_t flags=kernel_open_file_flags(file);
    if (!writing) {
        if (length<8) return -KERNEL_EINVAL;
        uint64_t value=0;int error=event_transfer(event,&value,0,flags);
        if (error) return error;
        struct kernel_uaccess_iov_cursor cursor={iov,count,0,0};size_t copied=0;
        enum kernel_uaccess_status access=kernel_copy_to_user_iov(mm,&cursor,&value,8,&copied);
        if (access && access!=KERNEL_UACCESS_STATUS_FAULT) __builtin_trap();
        /* Linux eventfd consumes the count before copy, including EFAULT. */
        return copied==8?8:-KERNEL_EFAULT;
    }
    int64_t done=0;
    for (size_t i=0;i<count;i++) {
        if (!iov[i].length&&count>1) continue;
        if (iov[i].length!=8) return done?done:-KERNEL_EINVAL;
        uint64_t value=0;size_t copied=0;
        enum kernel_uaccess_status access=kernel_copy_from_user(mm,&value,iov[i].base,8,&copied);
        if (access==KERNEL_UACCESS_STATUS_FAULT) return done?done:-KERNEL_EFAULT;
        if (access||copied!=8) __builtin_trap();
        if (value==UINT64_MAX) return done?done:-KERNEL_EINVAL;
        int error=event_transfer(event,&value,1,flags);
        if (error) return done?done:error;
        done+=8;
    }
    return done;
}
