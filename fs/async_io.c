#include <kernel/async_io.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/irq.h>
#include <kernel/pid.h>
#include <kernel/signal.h>
#include <kernel/socket.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <kernel/wait_internal.h>
#include "char_device_internal.h"
#include "open_file_internal.h"
#include "pipe_internal.h"

struct kernel_async_io {
    struct kernel_mutex configure;
    struct kernel_raw_lock metadata;
    struct kernel_wait_node subscription;
    struct kernel_wait_queue *source;
    struct kernel_open_file_description *file; /* Final close drains node borrows. */
    struct kernel_pid *owner;
    uint32_t uid,euid;
    int owner_type,fd;
    unsigned signum,enabled,pipe_endpoint;
};

static struct kernel_wait_queue *async_source(struct kernel_open_file_description *file)
{
    if (file->kind==KERNEL_OPEN_FILE_KIND_PIPE) return &file->pipe->async_events;
    if (file->kind==KERNEL_OPEN_FILE_KIND_SOCKET) return kernel_socket_async_source(file->socket);
    return file->device && file->device->async_source ?
        file->device->async_source(file->device_instance) : NULL;
}

static void async_notify(struct kernel_wait_node *node,uint32_t reason,uint64_t event)
{
    struct kernel_async_io *async=node->context;
    if (event==KERNEL_ASYNC_INVALIDATE || reason==KERNEL_WAIT_POLL_FREE) {
        /* A console redirection can survive the ordinary tty generation. */
        if (event==KERNEL_ASYNC_INVALIDATE && async->file->device &&
            async_source(async->file)==async->source) return;
        {
            KERNEL_RAW_SCOPE(guard,&async->metadata);
            async->enabled=0;
            __atomic_fetch_and(&async->file->open_flags,~(uint32_t)KERNEL_FILES_O_ASYNC,__ATOMIC_RELAXED);
        }
        kernel_wait_queue_remove(node);
        return;
    }
    if (reason!=KERNEL_WAIT_WOKEN || event<1 || event>6) return;
    struct kernel_pid *owner=NULL;
    uint32_t uid=0,euid=0;int type=0,fd=0;unsigned signum=0;
    {
        KERNEL_RAW_SCOPE(guard,&async->metadata);
        if (!async->enabled || !async->owner) return;
        if (async->pipe_endpoint &&
            ((event==KERNEL_ASYNC_POLL_IN && !(async->pipe_endpoint&1)) ||
             (event==KERNEL_ASYNC_POLL_OUT && !(async->pipe_endpoint&2)))) return;
        owner=async->owner;kernel_task_identity_retain(owner);
        uid=async->uid;euid=async->euid;type=async->owner_type;
        fd=async->fd;signum=async->signum;
    }
    kernel_signal_sigio(owner,type,uid,euid,signum,fd,(unsigned)event);
    kernel_task_identity_put(owner);
}

static struct kernel_async_io *get_context(struct kernel_open_file_description *file)
{
    struct kernel_async_io *async=__atomic_load_n(&file->async_io,__ATOMIC_ACQUIRE);
    if (async) return async;
    struct kernel_async_io *candidate=NULL;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(file->heap,1,sizeof(*candidate),(void **)&candidate);
    if (status==KERNEL_HEAP_STATUS_EMPTY) return NULL;
    if (status) __builtin_trap();
    kernel_mutex_init(&candidate->configure,9U,(uintptr_t)&candidate->configure);
    kernel_raw_lock_init(&candidate->metadata,KERNEL_RAW_RANK_OBJECT);
    kernel_wait_node_init_callback(&candidate->subscription,async_notify,candidate);
    candidate->file=file;
    candidate->pipe_endpoint=file->kind==KERNEL_OPEN_FILE_KIND_PIPE?file->pipe_endpoint:0;
    if (!__atomic_compare_exchange_n(&file->async_io,&async,candidate,0,
            __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) kernel_heap_release(file->heap,candidate);
    else async=candidate;
    return async;
}

static int owner_number(const struct kernel_async_io *async)
{
    if (!async || !async->owner) return 0;
    enum kernel_pid_role role=async->owner_type==0?KERNEL_PID_TID:
        async->owner_type==1?KERNEL_PID_TGID:KERNEL_PID_PGID;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    return async->owner->members[role]?kernel_task_identity_user_number(async->owner):0;
}

static void replace_owner(struct kernel_async_io *async,struct kernel_pid *owner,int type)
{
    struct kernel_pid *previous;
    const struct kernel_credentials *cred=kernel_credentials_current();
    {
        KERNEL_RAW_SCOPE(guard,&async->metadata);
        previous=async->owner;async->owner=owner;async->owner_type=type;
        if (owner) {async->uid=cred->uid;async->euid=cred->euid;}
    }
    if (previous) kernel_task_identity_put(previous);
}

int64_t kernel_open_file_async_fcntl(struct kernel_open_file_description *file,unsigned command,
    uint64_t argument,struct kernel_mm *mm)
{
    struct {int32_t type,pid;} ex={0};
    int type=1,number=0;
    if (command==KERNEL_FILES_F_SETOWN) {
        number=(int32_t)argument;
        if (number==INT32_MIN) return -KERNEL_EINVAL;
        if (number<0) {type=2;number=-number;}
    } else if (command==KERNEL_FILES_F_SETOWN_EX) {
        size_t copied=0;
        if (!mm || kernel_copy_from_user(mm,&ex,argument,sizeof(ex),&copied) || copied!=sizeof(ex))
            return -KERNEL_EFAULT;
        if (ex.type<0 || ex.type>2) return -KERNEL_EINVAL;
        type=ex.type;number=ex.pid;
    } else if (command==KERNEL_FILES_F_SETSIG && argument>64) return -KERNEL_EINVAL;
    struct kernel_async_io *async=__atomic_load_n(&file->async_io,__ATOMIC_ACQUIRE);
    int mutate=command==KERNEL_FILES_F_SETOWN || command==KERNEL_FILES_F_SETOWN_EX ||
        command==KERNEL_FILES_F_SETSIG;
    if (mutate && !async && !(async=get_context(file))) return -KERNEL_ENOMEM;
    if (command==KERNEL_FILES_F_SETOWN || command==KERNEL_FILES_F_SETOWN_EX) {
        struct kernel_pid *owner=NULL;
        if (number) {
            int error=kernel_task_identity_get_user(number,&owner);
            if (error) return error;
        }
        KERNEL_LOCK_SCOPE(configure);kernel_mutex_lock(&async->configure,&configure);
        replace_owner(async,owner,type);
        return 0;
    }
    int64_t result=0;
    if (async) {
        KERNEL_RAW_SCOPE(guard,&async->metadata);
        switch (command) {
        case KERNEL_FILES_F_GETOWN:
            result=owner_number(async);if (async->owner_type==2) result=-result;break;
        case KERNEL_FILES_F_GETOWN_EX:
            ex.type=async->owner_type;ex.pid=owner_number(async);break;
        case KERNEL_FILES_F_SETSIG: async->signum=(unsigned)argument;break;
        case KERNEL_FILES_F_GETSIG: result=async->signum;break;
        default: return -KERNEL_EINVAL;
        }
    }
    if (command==KERNEL_FILES_F_GETOWN_EX) {
        size_t copied=0;
        if (!mm || kernel_copy_to_user(mm,argument,&ex,sizeof(ex),&copied) || copied!=sizeof(ex))
            return -KERNEL_EFAULT;
    }
    return result;
}

int kernel_open_file_async_flags(struct kernel_open_file_description *file,int fd,uint32_t flags)
{
    KERNEL_SERVICE_SCOPE_IF(service,file->device && file->device->async_owner);
    struct kernel_wait_queue *source=async_source(file);
    if (!source && (flags&KERNEL_FILES_O_ASYNC) && file->device && file->device->async_owner)
        return -KERNEL_ENOTTY;
    if (!source) flags&=~(uint32_t)KERNEL_FILES_O_ASYNC;
    struct kernel_async_io *async=__atomic_load_n(&file->async_io,__ATOMIC_ACQUIRE);
    if (!source && !async) {
        kernel_open_file_set_status_flags(file,flags);
        return 0;
    }
    if (!async && (flags&KERNEL_FILES_O_ASYNC) && !(async=get_context(file))) return -KERNEL_ENOMEM;
    if (!async) {kernel_open_file_set_status_flags(file,flags);return 0;}
    KERNEL_LOCK_SCOPE(configure);kernel_mutex_lock(&async->configure,&configure);
    int enable=!!(flags&KERNEL_FILES_O_ASYNC) && source;
    {
        KERNEL_RAW_SCOPE(guard,&async->metadata);
        if (enable==(int)async->enabled) {
            kernel_open_file_set_status_flags(file,flags);
            return 0;
        }
    }
    {KERNEL_RAW_SCOPE(guard,&async->metadata);async->enabled=0;}
    if (kernel_wait_node_remove_sync(&async->subscription)) __builtin_trap();
    /* Source lifetime belongs to the actual endpoint. Tty generation changes
     * use the same IRQ publication boundary and invalidate captured old nodes. */
    KERNEL_IRQ_SCOPE(irq);
    source=async_source(file);
    if (!source && (flags&KERNEL_FILES_O_ASYNC) && file->device && file->device->async_owner)
        return -KERNEL_ENOTTY;
    if (!source) flags&=~(uint32_t)KERNEL_FILES_O_ASYNC;
    enable=!!(flags&KERNEL_FILES_O_ASYNC) && source;
    if (enable && file->device && file->device->async_owner && !async->owner) {
        int type=1;
        struct kernel_pid *owner=file->device->async_owner(file->device_instance,kernel_task_current(),&type);
        if (owner) replace_owner(async,owner,type);
    }
    {
        KERNEL_RAW_SCOPE(guard,&async->metadata);
        if (enable) async->fd=fd;
        async->source=source;
        if (enable) kernel_wait_queue_add(source,&async->subscription);
        async->enabled=(unsigned)enable;
        kernel_open_file_set_status_flags(file,flags);
    }
    return 0;
}

void kernel_open_file_async_close(struct kernel_open_file_description *file)
{
    struct kernel_async_io *async=file->async_io;
    if (!async) return;
    {KERNEL_RAW_SCOPE(guard,&async->metadata);async->enabled=0;}
    if (kernel_wait_node_remove_sync(&async->subscription)) __builtin_trap();
    if (async->owner) kernel_task_identity_put(async->owner);
    file->async_io=NULL;
    kernel_heap_release(file->heap,async);
}
