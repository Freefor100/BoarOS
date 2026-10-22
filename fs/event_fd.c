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
#include <kernel/time.h>
#include <arch/timer.h>
#if BOAROS_MAX_CPUS > 1
#include <arch/riscv/smp.h>
#endif

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

struct kernel_timer_fd {
    struct kernel_event_fd event;
    struct kernel_timer_fd *left,*right;
    uint64_t expires,interval,tick;
    uint32_t clock,flags,notifications;
    uint8_t indexed,expired;
    int8_t height;
};
static struct kernel_raw_lock timer_lock={.rank=KERNEL_RAW_RANK_OBJECT,.initialized=KERNEL_RAW_INITIALIZED};
static struct kernel_timer_fd *timer_root;
uint64_t kernel_timer_fd_next;
static int is_timer(const struct kernel_event_fd *event)
{ return event->file.kind==KERNEL_OPEN_FILE_KIND_TIMERFD; }
static struct kernel_raw_lock *event_lock(struct kernel_event_fd *event)
{ return is_timer(event)?&timer_lock:&event->lock; }
static uint64_t timer_now(const struct kernel_timer_fd *timer)
{ return timer->clock==0?kernel_time_realtime_ns():kernel_time_monotonic_ns(); }
static int height(const struct kernel_timer_fd *task)
{
    return task ? task->height : 0;
}

static void timer_update(struct kernel_timer_fd *task)
{
    int left = height(task->left);
    int right = height(task->right);
    task->height = (int8_t)(1 + (left > right ? left : right));
}

static struct kernel_timer_fd *timer_rotate_left(struct kernel_timer_fd *root)
{
    struct kernel_timer_fd *next = root->right;
    root->right = next->left;
    next->left = root;
    timer_update(root);
    timer_update(next);
    return next;
}

static struct kernel_timer_fd *timer_rotate_right(struct kernel_timer_fd *root)
{
    struct kernel_timer_fd *next = root->left;
    root->left = next->right;
    next->right = root;
    timer_update(root);
    timer_update(next);
    return next;
}

static struct kernel_timer_fd *timer_balance(struct kernel_timer_fd *root)
{
    timer_update(root);
    if (height(root->left) -
            height(root->right) > 1) {
        if (height(root->left->left) <
            height(root->left->right))
            root->left = timer_rotate_left(root->left);
        return timer_rotate_right(root);
    }
    if (height(root->right) -
            height(root->left) > 1) {
        if (height(root->right->right) <
            height(root->right->left))
            root->right = timer_rotate_right(root->right);
        return timer_rotate_left(root);
    }
    return root;
}

static int timer_before(const struct kernel_timer_fd *a,const struct kernel_timer_fd *b)
{ return a->tick<b->tick || (a->tick==b->tick && (uintptr_t)a<(uintptr_t)b); }

static struct kernel_timer_fd *timer_insert_node(struct kernel_timer_fd *root,
                                                struct kernel_timer_fd *task)
{
    if (root == 0) return task;
    if (timer_before(task, root))
        root->left = timer_insert_node(root->left, task);
    else
        root->right = timer_insert_node(root->right, task);
    return timer_balance(root);
}

static struct kernel_timer_fd *timer_extract_min(struct kernel_timer_fd *root,
                                                struct kernel_timer_fd **minimum)
{
    if (root->left == 0) {
        *minimum = root;
        return root->right;
    }
    root->left = timer_extract_min(root->left, minimum);
    return timer_balance(root);
}

static struct kernel_timer_fd *timer_remove_node(struct kernel_timer_fd *root,
                                                const struct kernel_timer_fd *task)
{
    if (root == 0) __builtin_trap();
    if (root != task) {
        if (timer_before(task, root))
            root->left =
                timer_remove_node(root->left, task);
        else
            root->right =
                timer_remove_node(root->right, task);
        return timer_balance(root);
    }
    if (root->left == 0) return root->right;
    if (root->right == 0) return root->left;
    struct kernel_timer_fd *next;
    struct kernel_timer_fd *right = timer_extract_min(root->right, &next);
    next->left = root->left;
    next->right = right;
    return timer_balance(next);
}

static void timer_remove(struct kernel_timer_fd *timer)
{
    if (!timer->indexed) return;
    timer_root=timer_remove_node(timer_root,timer);
    timer->left=timer->right=0;timer->height=0;timer->indexed=0;
}
static struct kernel_timer_fd *timer_first(void)
{
    struct kernel_timer_fd *first=timer_root;
    while(first&&first->left) first=first->left;
    return first;
}
static void timer_publish(void)
{
    struct kernel_timer_fd *first=timer_first();
    /* Scheduler reads this summary under its own lock; it never enters the
     * timer index while holding the wait domain. */
    __atomic_store_n(&kernel_timer_fd_next,first?first->tick:0,__ATOMIC_RELEASE);
}
static void timer_changed(void)
{
#if BOAROS_MAX_CPUS > 1
    if (kernel_cpu_current()->logical_id) {
        kernel_cpu_notify(riscv_smp_cpu(0),RISCV_IPI_DEADLINE);return;
    }
#endif
    kernel_scheduler_deadline_changed();
}
static void timer_insert(struct kernel_timer_fd *timer)
{
    uint64_t mono=timer->expires;
    if (!timer->clock) { uint64_t offset=kernel_time_boot_realtime_offset();mono=mono<=offset?0:mono-offset; }
    enum kernel_time_status status=kernel_time_deadline_from_monotonic(mono,&timer->tick);
    if (status==KERNEL_TIME_STATUS_DEADLINE_PASSED) timer->tick=arch_time_read()+1;
    else if (status) __builtin_trap();
    timer->left=timer->right=0;timer->height=1;timer->indexed=1;
    timer_root=timer_insert_node(timer_root,timer);
}
static void timer_forward(struct kernel_timer_fd *timer)
{
    if (!timer->expired||!timer->interval) return;
    uint64_t now=timer_now(timer);
    uint64_t periods=now>=timer->expires?(now-timer->expires)/timer->interval+1:1;
    timer->event.count+=periods-1;
    uint64_t advance=periods>(UINT64_MAX-timer->expires)/timer->interval?UINT64_MAX:periods*timer->interval;
    timer->expires=advance>INT64_MAX-timer->expires?INT64_MAX:timer->expires+advance;
    timer->expired=0;timer_insert(timer);
}
void kernel_timer_fd_expire(uint64_t now)
{
    for (;;) {
        struct kernel_wait_cursor cursor={0};struct kernel_timer_fd *timer;int notify;
        {
            KERNEL_RAW_SCOPE(guard,&timer_lock);
            timer=timer_first();
            if (!timer || (int64_t)(now-timer->tick)<0) break;
            timer_remove(timer);timer->expired=1;timer->event.count++;
            notify=kernel_wait_cursor_prepare(&timer->event.waiters,&cursor);
            if (notify) timer->notifications++;
            timer_publish();
        }
        if (notify) {
            kernel_wait_cursor_wake(&cursor,1);
            KERNEL_RAW_SCOPE(guard,&timer_lock);
            if (!timer->notifications) __builtin_trap();
            timer->notifications--;
        }
    }
}
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
static int allocate_event(struct kernel_files *files,size_t size,uint32_t kind,uint32_t flags,
    struct kernel_event_fd **owner)
{
    struct kernel_event_fd *event=0;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(files->heap,1,size,(void **)&event);
    if (status==KERNEL_HEAP_STATUS_EMPTY) return -KERNEL_ENOMEM;
    if (status) __builtin_trap();
    event->file.heap=files->heap;event->file.references=1;event->file.kind=kind;
    event->file.open_flags=2U|(flags&EVENT_NONBLOCK);
    kernel_mutex_init(&event->file.offset_lock,10,(uintptr_t)event);
    kernel_raw_lock_init(&event->lock,KERNEL_RAW_RANK_OBJECT);kernel_wait_queue_init(&event->waiters);
    *owner=event;return 0;
}
enum kernel_files_status kernel_files_eventfd_create(struct kernel_files *files,
    uint32_t initial, uint32_t flags, int64_t *result)
{
    if (!kernel_files_is_live(files)||!result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (flags&~(EVENT_SEMAPHORE|EVENT_NONBLOCK|EVENT_CLOEXEC)) { *result=-KERNEL_EINVAL;return 0; }
    struct kernel_event_fd *event=0;
    int error=allocate_event(files,sizeof(*event),KERNEL_OPEN_FILE_KIND_EVENTFD,flags,&event);
    if (error) { *result=error;return 0; }
    event->count=initial;event->semaphore=!!(flags&EVENT_SEMAPHORE);
    return install_event(files,event,flags,result);
}
void kernel_event_fd_close(struct kernel_open_file_description *file)
{
    struct kernel_event_fd *event=event_of(file);
    if (is_timer(event)) {
        { KERNEL_RAW_SCOPE(guard,&timer_lock);struct kernel_timer_fd *timer=(void *)event;
          timer_remove(timer);timer_publish(); }
        timer_changed();
    }
    if (kernel_wait_queue_close(&event->waiters)) __builtin_trap();
    /* A timer callback may already have selected a wait cursor before close
     * detached it. Its real borrow keeps the object until notification ends. */
    for (;;) {
        int pending=0;
        if (is_timer(event)) { KERNEL_RAW_SCOPE(guard,&timer_lock);pending=((struct kernel_timer_fd *)event)->notifications!=0; }
        if (pending) { if (kernel_scheduler_yield_current()) __builtin_trap();continue; }
        enum kernel_scheduler_status status=kernel_wait_queue_destroy(&event->waiters);
        if (!pending && status==KERNEL_SCHEDULER_STATUS_OK) break;
        if (status!=KERNEL_SCHEDULER_STATUS_BUSY) __builtin_trap();
        if (kernel_scheduler_yield_current()) __builtin_trap();
    }
}
uint32_t kernel_event_fd_poll(struct kernel_open_file_description *file,struct kernel_wait_queue **queue)
{
    struct kernel_event_fd *event=event_of(file);
    KERNEL_RAW_SCOPE(guard,event_lock(event));
    if (queue) *queue=&event->waiters;
    return (event->count?KERNEL_POLLIN:0U) | (!is_timer(event)&&event->count<UINT64_MAX-1?KERNEL_POLLOUT:0U);
}
/* The actual caller holds the OFD. State and registration share the object
 * lock, while user faults, callback invocation and parking happen outside it. */
static int event_transfer(struct kernel_event_fd *event,uint64_t *value,int writing,uint32_t flags)
{
    for (;;) {
        struct kernel_wait_token token={0};struct kernel_wait_cursor cursor={0};
        int wait=0,notify=0;
        {
            KERNEL_RAW_SCOPE(guard,event_lock(event));
            wait=writing ? *value>UINT64_MAX-1-event->count : !event->count;
            if (wait) {
                if (flags&EVENT_NONBLOCK) return -KERNEL_EAGAIN;
                if (kernel_wait_prepare(&event->waiters,0,1,&token)) __builtin_trap();
            } else {
                if (writing) event->count+=*value;
                else {
                    if (is_timer(event)) timer_forward((void *)event);
                    *value=event->semaphore?1:event->count;event->count-=*value;
                    if (is_timer(event)) { ((struct kernel_timer_fd *)event)->expired=0;timer_publish(); }
                }
                if (!is_timer(event)) notify=kernel_wait_cursor_prepare(&event->waiters,&cursor);
            }
        }
        if (notify) kernel_wait_cursor_wake(&cursor,1);
        if (!wait && is_timer(event)) timer_changed();
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
    if (writing && is_timer(event)) return -KERNEL_EINVAL;
    if (!writing) {
        if (length<8) return -KERNEL_EINVAL;
        uint64_t value=0;int error=event_transfer(event,&value,0,flags);
        if (error) return error;
        struct kernel_uaccess_iov_cursor cursor={iov,count,0,0};size_t copied=0;
        enum kernel_uaccess_status access=kernel_copy_to_user_iov(mm,&cursor,&value,8,&copied);
        if (access && access!=KERNEL_UACCESS_STATUS_FAULT) __builtin_trap();
        /* Linux eventfd consumes the count before copy, including EFAULT. */
        return is_timer(event) ? (copied?(int64_t)copied:-KERNEL_EFAULT) : copied==8?8:-KERNEL_EFAULT;
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
struct event_timespec { int64_t seconds,nanoseconds; };
struct event_itimerspec { struct event_timespec interval,value; };
static int copy_event(struct kernel_mm *mm,void *data,uint64_t address,size_t size,int output)
{
    size_t copied=0;
    enum kernel_uaccess_status status=output?kernel_copy_to_user(mm,address,data,size,&copied):kernel_copy_from_user(mm,data,address,size,&copied);
    if (status==KERNEL_UACCESS_STATUS_FAULT) return -KERNEL_EFAULT;
    if (status||copied!=size) __builtin_trap();
    return 0;
}
static int valid_time(struct event_timespec t)
{ return t.seconds>=0&&t.nanoseconds>=0&&t.nanoseconds<1000000000; }
static uint64_t time_ns(struct event_timespec t)
{
    if ((uint64_t)t.seconds>(INT64_MAX-(uint64_t)t.nanoseconds)/1000000000) return INT64_MAX;
    return (uint64_t)t.seconds*1000000000+(uint64_t)t.nanoseconds;
}
static struct event_timespec time_spec(uint64_t ns)
{ return (struct event_timespec){ns/1000000000,ns%1000000000}; }
enum kernel_files_status kernel_files_timerfd_create(struct kernel_files *files,
    int32_t clock,uint32_t flags,int64_t *result)
{
    if (!kernel_files_is_live(files)||!result) return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if ((clock!=0&&clock!=1)||(flags&~(EVENT_NONBLOCK|EVENT_CLOEXEC))) { *result=-KERNEL_EINVAL;return 0; }
    struct kernel_event_fd *event=0;
    int error=allocate_event(files,sizeof(struct kernel_timer_fd),KERNEL_OPEN_FILE_KIND_TIMERFD,flags,&event);
    if (error) { *result=error;return 0; }
    ((struct kernel_timer_fd *)event)->clock=(uint32_t)clock;
    return install_event(files,event,flags,result);
}
static enum kernel_files_status timer_time(struct kernel_files *files,struct kernel_mm *mm,
    int32_t fd,uint32_t flags,uint64_t input,uint64_t output,int setting,int64_t *result)
{
    struct event_itimerspec new={0},old={0};
    if (setting) {
        *result=copy_event(mm,&new,input,sizeof(new),0);
        if (*result) return 0;
        if ((flags&~3U)||!valid_time(new.interval)||!valid_time(new.value)) { *result=-KERNEL_EINVAL;return 0; }
    }
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *file=kernel_files_hold_fd(files,fd,&pin);
    if (!file) { *result=-KERNEL_EBADF;return 0; }
    if (file->kind!=KERNEL_OPEN_FILE_KIND_TIMERFD) { *result=-KERNEL_EINVAL;return 0; }
    struct kernel_timer_fd *timer=(void *)file;
    struct kernel_wait_cursor cursor={0};int notify=0;
    {
        KERNEL_RAW_SCOPE(guard,&timer_lock);
        timer_forward(timer);
        uint64_t now=timer_now(timer);
        old.interval=time_spec(timer->interval);
        old.value=time_spec(timer->expires>now?timer->expires-now:0);
        if (setting) {
            timer_remove(timer);timer->event.count=0;timer->expired=0;
            timer->interval=time_ns(new.interval);timer->expires=time_ns(new.value);timer->flags=flags;
            if (timer->expires) {
                if (!(flags&1U)) timer->expires=timer->expires>INT64_MAX-now?INT64_MAX:timer->expires+now;
                if (timer->expires<=now) { timer->expired=1;timer->event.count=1;notify=kernel_wait_cursor_prepare(&timer->event.waiters,&cursor); }
                else timer_insert(timer);
            }
        }
        timer_publish();
    }
    if (notify) kernel_wait_cursor_wake(&cursor,1);
    timer_changed();
    *result=(!setting||output)?copy_event(mm,&old,output,sizeof(old),1):0;
    return 0;
}
enum kernel_files_status kernel_files_timerfd_settime(struct kernel_files *files,struct kernel_mm *mm,
    int32_t fd,uint32_t flags,uint64_t input,uint64_t output,int64_t *result)
{ return timer_time(files,mm,fd,flags,input,output,1,result); }
enum kernel_files_status kernel_files_timerfd_gettime(struct kernel_files *files,struct kernel_mm *mm,
    int32_t fd,uint64_t output,int64_t *result)
{ return timer_time(files,mm,fd,0,0,output,0,result); }
