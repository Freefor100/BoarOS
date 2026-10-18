#include <kernel/loop.h>
#include <kernel/block.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/scheduler.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>
#include <kernel/irq.h>
#include <kernel/page.h>
#include "open_file_internal.h"
#include "block_file_internal.h"
#include "char_device_internal.h"
#include "files/private.h"
#include <string.h>

#define LOOP_DEFAULT_COUNT 8U
#define LOOP_MAX_NUMBER ((1U<<20)-1)
enum loop_state { LOOP_UNBOUND, LOOP_BOUND, LOOP_RUNDOWN, LOOP_DETACHING, LOOP_FAILED };
enum loop_operation { LOOP_READ, LOOP_WRITE, LOOP_FLUSH };
struct loop_request {
    struct loop_request *next;
    enum loop_operation operation;
    uint64_t offset;
    void *buffer;
    size_t size;
    int result, done;
    struct kernel_wait_queue changed;
};
struct loop_device {
    struct loop_device *next;
    struct kernel_heap *heap;
    struct kernel_block_device block;
    struct kernel_raw_lock metadata;
    struct kernel_wait_queue work, changed;
    struct loop_request *head, **tail;
    struct kernel_thread_join worker;
    struct kernel_open_file_description *backing;
    struct kernel_block_device *parent;
    struct kernel_loop_info64 info;
    uint64_t detach_sequence;
    uint32_t number;
    _Atomic uint32_t state;
    _Atomic int worker_done;
    int dynamic, check_owners, detach_error;
};
static struct kernel_mutex loop_control;
static unsigned char control_initializing;
static int control_ready, loops_ready;
static struct loop_device defaults[LOOP_DEFAULT_COUNT], *loops;
static struct kernel_heap *loop_heap;

static void control_initialize(void)
{
    while (__atomic_test_and_set(&control_initializing,__ATOMIC_ACQUIRE)) {}
    if (!control_ready) {
        kernel_mutex_init(&loop_control,5,(uintptr_t)&loop_control);
        control_ready=1;
    }
    __atomic_clear(&control_initializing,__ATOMIC_RELEASE);
}
static void wake(struct kernel_wait_queue *queue)
{
    KERNEL_IRQ_SCOPE(irq);
    enum kernel_scheduler_status status=kernel_wait_queue_wake_all(queue);
    if (status && status!=KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED) __builtin_trap();
}
static int transfer(struct kernel_mm *mm,uint64_t user,void *data,size_t size,int from_user)
{
    size_t copied=0;
    enum kernel_uaccess_status status=from_user?
        kernel_copy_from_user(mm,data,user,size,&copied):kernel_copy_to_user(mm,user,data,size,&copied);
    return status || copied!=size?-KERNEL_EFAULT:0;
}
static struct loop_device *find_loop(uint32_t number)
{
    for (struct loop_device *loop=loops;loop;loop=loop->next)
        if (loop->number==number) return loop;
    return NULL;
}
static struct loop_device *device_loop(struct kernel_block_device *device)
{
    for (struct loop_device *loop=loops;loop;loop=loop->next)
        if (&loop->block==device) return loop;
    return NULL;
}
static void release_pin(struct kernel_files *files,struct kernel_open_file_description **owner)
{
    if (!*owner) return;
    enum kernel_open_file_status status=kernel_open_file_release(owner);
    if (status==KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED) {
        kernel_files_queue_description(files,*owner);*owner=NULL;
    } else if (status) __builtin_trap();
}
static int backing_sync(struct loop_device *loop)
{
    if (!loop->backing->references) return 0; /* Retained failed-close owner. */
    if (loop->backing->kind==KERNEL_OPEN_FILE_KIND_BLOCK)
        return kernel_block_file_sync(loop->backing);
    return kernel_vfs_sync(&loop->backing->file,0,&loop->backing->observed_writeback_error);
}
static int backing_io(struct loop_device *loop,struct loop_request *request)
{
    if (request->operation==LOOP_FLUSH) return backing_sync(loop);
    size_t done=0;
    uint64_t offset=loop->info.offset+request->offset;
    int error;
    if (request->operation==LOOP_READ)
        error=kernel_open_file_pread(loop->backing,offset,request->buffer,request->size,&done);
    else if (loop->info.flags&KERNEL_LOOP_READ_ONLY) return -KERNEL_EROFS;
    else if (loop->backing->kind==KERNEL_OPEN_FILE_KIND_BLOCK)
        error=kernel_block_file_pwrite(loop->backing,offset,request->buffer,request->size,&done);
    else {
        error=kernel_vfs_file_modified(&loop->backing->file,offset,0);
        if (!error) error=kernel_vfs_pwrite(&loop->backing->file,offset,request->buffer,request->size,&done);
    }
    return error?error:done==request->size?0:-KERNEL_EIO;
}
static int detach_backing(struct loop_device *loop)
{
    int error=backing_sync(loop);
    if (!error) {
        enum kernel_open_file_status status=kernel_open_file_release(&loop->backing);
        if (status==KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED) error=-KERNEL_EIO;
        else if (status) __builtin_trap();
    }
    /* 失败时真实后备owner仍可达；不能把失败解绑伪装成空闲设备。 */
    if (!error) {
        KERNEL_LOCK_SCOPE(control);
        kernel_mutex_lock(&loop_control,&control);
        loop->parent=NULL;
        memset(&loop->info,0,sizeof(loop->info));
        __atomic_store_n(&loop->block.capacity_bytes,0,__ATOMIC_RELEASE);
        __atomic_store_n(&loop->block.read_only,0,__ATOMIC_RELEASE);
    }
    return error;
}
static void loop_worker(void *argument)
{
    struct loop_device *loop=argument;
    for (;;) {
        struct loop_request *request=NULL;
        struct kernel_wait_token token={0};
        int detach=0;
        {
            KERNEL_RAW_SCOPE(guard,&loop->metadata);
            if (loop->head) {
                request=loop->head;loop->head=request->next;
                if (!loop->head) loop->tail=&loop->head;
            } else if (loop->check_owners) {
                loop->check_owners=0;
                uint32_t references;
                kernel_block_usage(&loop->block,&references,NULL);
                if (!references && (loop->info.flags&KERNEL_LOOP_AUTOCLEAR) && loop->backing) {
                    loop->state=LOOP_DETACHING;detach=1;
                }
            }
            if (!request && !detach && kernel_wait_prepare(&loop->work,0,0,&token)) __builtin_trap();
        }
        if (detach) {
            int error=detach_backing(loop);
            {
                KERNEL_LOCK_SCOPE(control);
                kernel_mutex_lock(&loop_control,&control);
                KERNEL_RAW_SCOPE(guard,&loop->metadata);
                loop->detach_error=error;
                loop->detach_sequence++;
                loop->worker_done=!error;
                loop->state=error?LOOP_FAILED:LOOP_UNBOUND;
                wake(&loop->changed);
            }
            if (!error) return;
            continue;
        }
        if (!request) {
            enum kernel_wait_wake_reason reason;
            if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
            continue;
        }
        /* 调用者持续借用buffer并等待完成；此独立栈不继承上层文件系统锁。
         * 因而在loop上的ext4调用块层时，不倒序取得后备文件的VFS锁。 */
        int result=backing_io(loop,request);
        {
            KERNEL_RAW_SCOPE(guard,&loop->metadata);
            request->result=result;request->done=1;wake(&request->changed);
        }
    }
}
static int submit(struct loop_device *loop,enum loop_operation operation,
    uint64_t offset,void *buffer,size_t size)
{
    struct loop_request request={.operation=operation,.offset=offset,.buffer=buffer,.size=size};
    kernel_wait_queue_init(&request.changed);
    {
        KERNEL_RAW_SCOPE(guard,&loop->metadata);
        if (loop->state!=LOOP_BOUND) {
            int error=loop->state==LOOP_UNBOUND || loop->state==LOOP_RUNDOWN?-KERNEL_ENXIO:-KERNEL_EIO;
            if (kernel_wait_queue_close(&request.changed) || kernel_wait_queue_destroy(&request.changed)) __builtin_trap();
            return error;
        }
        *loop->tail=&request;loop->tail=&request.next;wake(&loop->work);
    }
    for (;;) {
        struct kernel_wait_token token={0};
        {
            KERNEL_RAW_SCOPE(guard,&loop->metadata);
            if (request.done) break;
            if (kernel_wait_prepare(&request.changed,0,0,&token)) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
    }
    if (kernel_wait_queue_close(&request.changed) || kernel_wait_queue_destroy(&request.changed)) __builtin_trap();
    return request.result;
}
static enum kernel_block_status block_status(int error)
{
    return !error?KERNEL_BLOCK_STATUS_OK:error==-KERNEL_ENOMEM?KERNEL_BLOCK_STATUS_NO_MEMORY:
        error==-KERNEL_EROFS?KERNEL_BLOCK_STATUS_READ_ONLY:KERNEL_BLOCK_STATUS_IO;
}
static enum kernel_block_status read_loop(void *context,uint64_t offset,void *buffer,size_t size)
{ return block_status(submit(context,LOOP_READ,offset,buffer,size)); }
static enum kernel_block_status write_loop(void *context,uint64_t offset,const void *buffer,size_t size)
{ return block_status(submit(context,LOOP_WRITE,offset,(void *)buffer,size)); }
static enum kernel_block_status flush_loop(void *context)
{ return block_status(submit(context,LOOP_FLUSH,0,NULL,0)); }
static int open_loop(struct kernel_block_device *device,uint32_t flags)
{
    (void)flags;struct loop_device *loop=device->context;
    KERNEL_RAW_SCOPE(guard,&loop->metadata);
    return loop->state==LOOP_DETACHING?-KERNEL_EBUSY:loop->state==LOOP_FAILED?-KERNEL_EIO:
        loop->state==LOOP_RUNDOWN?-KERNEL_ENXIO:0;
}
static void owners_changed(struct kernel_block_device *device)
{
    struct loop_device *loop=device->context;
    /* 此通知也会来自仍持有上层inode/backend锁的I/O完成者，只发布工作。
     * 后备flush/close必须由独立worker在全部真实设备owner归还后执行。 */
    KERNEL_RAW_SCOPE(guard,&loop->metadata);
    if (loop->state==LOOP_UNBOUND || loop->worker_done) return;
    loop->check_owners=1;wake(&loop->work);
}
static int64_t loop_ioctl(struct kernel_block_device *,struct kernel_files *,
    struct kernel_open_file_description **,struct kernel_mm *,uint64_t,uint64_t);
static void setup_loop(struct loop_device *loop,uint32_t number,struct kernel_heap *heap,int dynamic)
{
    memset(loop,0,sizeof(*loop));loop->heap=heap;loop->number=number;loop->dynamic=dynamic;
    loop->worker_done=1;loop->tail=&loop->head;
    kernel_raw_lock_init(&loop->metadata,KERNEL_RAW_RANK_OBJECT);
    kernel_wait_queue_init(&loop->work);kernel_wait_queue_init(&loop->changed);
    loop->block=(struct kernel_block_device){.context=loop,.read=read_loop,.write=write_loop,
        .flush=flush_loop,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK,
        .open=open_loop,.owners_changed=owners_changed,.ioctl=loop_ioctl};
}
int kernel_loop_initialize(struct kernel_heap *heap)
{
    if (!heap) return -KERNEL_EINVAL;
    control_initialize();KERNEL_LOCK_SCOPE(control);kernel_mutex_lock(&loop_control,&control);
    if (loops_ready) return heap==loop_heap?0:-KERNEL_EXDEV;
    for (unsigned i=0;i<LOOP_DEFAULT_COUNT;i++) {
        setup_loop(&defaults[i],i,heap,0);
        int error=kernel_block_register(&defaults[i].block,KERNEL_LOOP_DEVICE_NUMBER(i));
        if (error) {
            while (i) { i--;if (kernel_block_unregister(&defaults[i].block)) __builtin_trap(); }
            loops=NULL;return error;
        }
        defaults[i].next=loops;loops=&defaults[i];
    }
    loop_heap=heap;loops_ready=1;return 0;
}
static void reap_worker(struct loop_device *loop)
{
    if (loop->worker.task && __atomic_load_n(&loop->worker_done,__ATOMIC_ACQUIRE))
        kernel_thread_join(&loop->worker);
}
static int validate_info(const struct kernel_loop_info64 *info)
{
    if (info->encrypt_type || info->encrypt_key_size) return -KERNEL_EINVAL;
    if (info->flags&~(KERNEL_LOOP_READ_ONLY|KERNEL_LOOP_AUTOCLEAR)) return -KERNEL_EOPNOTSUPP;
    if (info->offset>INT64_MAX || info->sizelimit>INT64_MAX) return -KERNEL_EOVERFLOW;
    return 0;
}
static uint64_t loop_capacity(struct kernel_open_file_description *backing,const struct kernel_loop_info64 *info)
{
    uint64_t size=kernel_open_file_size(backing);
    size=size>info->offset?size-info->offset:0;
    if (info->sizelimit && size>info->sizelimit) size=info->sizelimit;
    if (size>INT64_MAX) size=INT64_MAX;
    return size&~UINT64_C(511);
}
static int validate_dependency(struct loop_device *loop,struct kernel_block_device *parent)
{
    while (parent) {
        if (parent==&loop->block) return -KERNEL_ELOOP;
        struct loop_device *next=device_loop(parent);
        if (!next) break;
        /* 非BOUND父设备没有可持续借用的后备链；rundown也不能作为终点，
         * 否则下一次绑定可把已持有的OFD连成无法释放的owner环。 */
        if (next->state!=LOOP_BOUND) return -KERNEL_EINVAL;
        parent=next->parent;
    }
    return 0;
}
static int configure_loop(struct loop_device *loop,struct kernel_files *files,int64_t fd,
    const struct kernel_loop_info64 *information,uint32_t block_size,unsigned open_flags,const void *open_owner)
{
    int error=validate_info(information);
    if (error) return error;
    if (block_size && (block_size<512 || block_size>BOAROS_PAGE_SIZE || (block_size&(block_size-1)))) return -KERNEL_EINVAL;
    struct kernel_open_file_description *backing=NULL;int64_t result;
    if (kernel_files_pin_data(files,fd,&backing,&result)) return -KERNEL_EIO;
    if (result) return (int)result;
    struct kernel_vfs_stat stat;
    struct kernel_block_device *parent=NULL;
    if (backing->kind!=KERNEL_OPEN_FILE_KIND_REGULAR && backing->kind!=KERNEL_OPEN_FILE_KIND_BLOCK) error=-KERNEL_EINVAL;
    else if (!kernel_open_file_readable(backing)) error=-KERNEL_EBADF;
    else {
        error=kernel_vfs_fstat(&backing->file,&stat);
        if (!error) parent=backing->kind==KERNEL_OPEN_FILE_KIND_BLOCK?backing->block:
            kernel_vfs_file_block_device(&backing->file);
    }
    if (!error) {
        KERNEL_LOCK_SCOPE(control);kernel_mutex_lock(&loop_control,&control);
        if (loop->state!=LOOP_UNBOUND) error=-KERNEL_EBUSY;
        else error=validate_dependency(loop,parent);
        int temporary_claim=0;
        if (!error && !(open_flags&0200U)) {
            error=kernel_block_claim(&loop->block,open_owner);
            temporary_claim=!error;
        }
        if (!error) {
            reap_worker(loop);
            KERNEL_LOCK_SCOPE(io);kernel_mutex_lock(&loop->block.io_lock,&io);
            {
            KERNEL_RAW_SCOPE(guard,&loop->metadata);
            memset(&loop->info,0,sizeof(loop->info));
            loop->info.offset=information->offset;loop->info.sizelimit=information->sizelimit;
            loop->info.flags=information->flags;
            memcpy(loop->info.file_name,information->file_name,sizeof(loop->info.file_name)-1);
            loop->info.device=stat.dev;loop->info.inode=stat.ino;loop->info.rdevice=stat.rdev;
            loop->info.number=loop->number;
            if (!kernel_open_file_writable(backing) || !(open_flags&3U)) loop->info.flags|=KERNEL_LOOP_READ_ONLY;
            loop->backing=backing;loop->parent=parent;
            loop->block.logical_block_size=block_size?block_size:512;
            loop->block.soft_block_size=loop->block.logical_block_size;
            __atomic_store_n(&loop->block.capacity_bytes,loop_capacity(backing,&loop->info),__ATOMIC_RELEASE);
            __atomic_store_n(&loop->block.read_only,!!(loop->info.flags&KERNEL_LOOP_READ_ONLY),__ATOMIC_RELEASE);
            loop->state=LOOP_BOUND;loop->worker_done=0;loop->detach_error=0;
            }
            enum kernel_scheduler_status status=kernel_thread_create_joinable(loop_worker,loop,&loop->worker);
            if (status) {
                KERNEL_RAW_SCOPE(guard,&loop->metadata);
                loop->backing=NULL;loop->parent=NULL;loop->state=LOOP_UNBOUND;loop->worker_done=1;
                __atomic_store_n(&loop->block.capacity_bytes,0,__ATOMIC_RELEASE);
                __atomic_store_n(&loop->block.read_only,0,__ATOMIC_RELEASE);
                error=status==KERNEL_SCHEDULER_STATUS_NO_MEMORY?-KERNEL_ENOMEM:-KERNEL_EIO;
            } else backing=NULL;
        }
        if (temporary_claim) kernel_block_release_claim(&loop->block,open_owner);
    }
    release_pin(files,&backing);return error;
}
static void info_from_old(struct kernel_loop_info64 *out,const struct kernel_loop_info *old)
{
    memset(out,0,sizeof(*out));out->offset=(int64_t)old->offset;out->flags=old->flags;
    out->encrypt_type=old->encrypt_type;out->encrypt_key_size=old->encrypt_key_size;
    memcpy(out->file_name,old->name,sizeof(out->file_name));
}
static int64_t loop_ioctl(struct kernel_block_device *device,struct kernel_files *files,
    struct kernel_open_file_description **owner,struct kernel_mm *mm,uint64_t command,uint64_t argument)
{
    struct loop_device *loop=device->context;
    if (command==0x4c00) {
        struct kernel_loop_info64 info={0};
        return configure_loop(loop,files,(int64_t)argument,&info,0,(*owner)->open_flags,*owner);
    }
    if (command==0x4c0a) {
        struct kernel_loop_config config;
        int error=transfer(mm,argument,&config,sizeof(config),1);if (error) return error;
        for (unsigned i=0;i<8;i++) if (config.reserved[i]) return -KERNEL_EINVAL;
        return configure_loop(loop,files,config.fd,&config.info,config.block_size,(*owner)->open_flags,*owner);
    }
    if (command==0x4c06 || command==0x4c08 || command==0x4c09) return -KERNEL_EOPNOTSUPP;
    struct kernel_loop_info64 info={0};
    if (command==0x4c02 || command==0x4c04) {
        if (!((*owner)->open_flags&3U)) return -KERNEL_EBADF;
        int error;
        if (command==0x4c02) {
            struct kernel_loop_info old;
            error=transfer(mm,argument,&old,sizeof(old),1);if (!error) info_from_old(&info,&old);
        } else error=transfer(mm,argument,&info,sizeof(info),1);
        if (!error) error=validate_info(&info);
        if (error) return error;
    }
    KERNEL_LOCK_SCOPE(control);kernel_mutex_lock(&loop_control,&control);
    KERNEL_LOCK_SCOPE(io);kernel_mutex_lock(&device->io_lock,&io);
    if (loop->state==LOOP_UNBOUND || loop->state==LOOP_RUNDOWN) return -KERNEL_ENXIO;
    if (loop->state!=LOOP_BOUND) return -KERNEL_EIO;
    if (command==0x4c01) {
        uint32_t opens;
        kernel_block_usage(device,NULL,&opens);
        int mounted=kernel_block_claimed(device);
        KERNEL_RAW_SCOPE(guard,&loop->metadata);
        loop->info.flags|=KERNEL_LOOP_AUTOCLEAR;
        /* Linux的单打开者进入rundown：保留后备到最后close，但拒绝新使用。 */
        if (opens==1 && !mounted) loop->state=LOOP_RUNDOWN;
        loop->check_owners=1;wake(&loop->work);return 0;
    }
    if (command==0x4c05) {
        info=loop->info;
        kernel_lock_release(&io);kernel_lock_release(&control);
        return argument?transfer(mm,argument,&info,sizeof(info),0):-KERNEL_EINVAL;
    }
    if (command==0x4c03) {
        struct kernel_loop_info old={.number=(int32_t)loop->number,.device=loop->info.device,
            .inode=loop->info.inode,.rdevice=loop->info.rdevice,.offset=(int32_t)loop->info.offset,
            .flags=(int32_t)loop->info.flags};
        memcpy(old.name,loop->info.file_name,sizeof(old.name));
        int error=(uint64_t)(int64_t)old.offset!=loop->info.offset ||
            old.device!=loop->info.device || old.rdevice!=loop->info.rdevice?-KERNEL_EOVERFLOW:0;
        kernel_lock_release(&io);kernel_lock_release(&control);
        return error?error:argument?transfer(mm,argument,&old,sizeof(old),0):-KERNEL_EINVAL;
    }
    if (command==0x4c02 || command==0x4c04 || command==0x4c07) {
        int error=submit(loop,LOOP_FLUSH,0,NULL,0);if (error) return error;
        if (command!=0x4c07) {
            KERNEL_RAW_SCOPE(guard,&loop->metadata);
            loop->info.offset=info.offset;loop->info.sizelimit=info.sizelimit;
            memcpy(loop->info.file_name,info.file_name,sizeof(info.file_name)-1);
            loop->info.file_name[sizeof(loop->info.file_name)-1]=0;
            loop->info.flags=(loop->info.flags&KERNEL_LOOP_READ_ONLY)|(info.flags&KERNEL_LOOP_AUTOCLEAR);
        }
        __atomic_store_n(&device->capacity_bytes,loop_capacity(loop->backing,&loop->info),__ATOMIC_RELEASE);
        return 0;
    }
    return -KERNEL_ENOTTY;
}
static int control_open(struct kernel_heap *heap,struct kernel_task *task,uint32_t flags,
    const struct kernel_vfs_file *file,void **instance)
{
    (void)task;(void)flags;(void)file;*instance=NULL;return kernel_loop_initialize(heap);
}
static int64_t control_command(struct kernel_files *files,uint64_t command,uint64_t argument)
{
    (void)files;
    if (!loops_ready) return -KERNEL_ENODEV;
    if (command!=0x4c80 && command!=0x4c81 && command!=0x4c82) return -KERNEL_ENOSYS;
    if (command!=0x4c82 && argument>LOOP_MAX_NUMBER) return -KERNEL_EINVAL;
    KERNEL_LOCK_SCOPE(control);kernel_mutex_lock(&loop_control,&control);
    uint32_t number=(uint32_t)argument;
    struct loop_device *loop=NULL;
    if (command==0x4c82) {
        for (number=0;number<=LOOP_MAX_NUMBER;number++) {
            loop=find_loop(number);
            if (!loop || (loop->state==LOOP_UNBOUND && !loop->backing)) break;
        }
        if (number>LOOP_MAX_NUMBER) return -KERNEL_ENOSPC;
        if (loop) { reap_worker(loop);return number; }
    } else loop=find_loop(number);
    if (command==0x4c81) {
        if (!loop) return -KERNEL_ENODEV;
        if (loop->state!=LOOP_UNBOUND || loop->backing) return -KERNEL_EBUSY;
        reap_worker(loop);
        int error=kernel_block_unregister(&loop->block);if (error) return error;
        struct loop_device **link=&loops;while (*link!=loop) link=&(*link)->next;
        *link=loop->next;
        if (kernel_wait_queue_close(&loop->work) || kernel_wait_queue_destroy(&loop->work) ||
            kernel_wait_queue_close(&loop->changed) || kernel_wait_queue_destroy(&loop->changed)) __builtin_trap();
        if (loop->dynamic) kernel_heap_release(loop->heap,loop);
        return 0;
    }
    if (loop) return -KERNEL_EEXIST;
    if (kernel_heap_allocate(loop_heap,sizeof(*loop),(void **)&loop)) return -KERNEL_ENOMEM;
    setup_loop(loop,number,loop_heap,1);
    int error=kernel_block_register(&loop->block,KERNEL_LOOP_DEVICE_NUMBER(number));
    if (error) { kernel_heap_release(loop_heap,loop);return error; }
    loop->next=loops;loops=loop;return number;
}
static int control_ioctl(void *instance,struct kernel_task *task,struct kernel_files *files,
    struct kernel_open_file_description **owner,struct kernel_mm *mm,uint64_t command,uint64_t argument)
{
    (void)instance;(void)task;(void)owner;(void)mm;
    return (int)control_command(files,command,argument);
}
static int control_read(void *instance,struct kernel_task *task,uint32_t flags,void *buffer,size_t size,size_t *done)
{
    (void)instance;(void)task;(void)flags;(void)buffer;(void)size;*done=0;return -KERNEL_EINVAL;
}
static int control_write(void *instance,struct kernel_task *task,uint32_t flags,const void *buffer,size_t size,size_t *done)
{
    (void)instance;(void)task;(void)flags;(void)buffer;(void)size;*done=0;return -KERNEL_EINVAL;
}
const struct kernel_char_device kernel_loop_control_device={
    .rdev=0xaed,.kind=KERNEL_OPEN_FILE_KIND_NULL,.open=control_open,.ioctl=control_ioctl,
    .positioned=1,.read=control_read,.write=control_write,
};
int kernel_loop_release_unused(void)
{
    if (!loops_ready) return 0;
    int progressed=0;
    for (;;) {
        struct loop_device *selected=NULL;uint64_t sequence=0;
        {
            KERNEL_LOCK_SCOPE(control);kernel_mutex_lock(&loop_control,&control);
            for (struct loop_device *loop=loops;loop;loop=loop->next) {
                uint32_t references;kernel_block_usage(&loop->block,&references,NULL);
                if (references) continue;
                if (loop->state==LOOP_UNBOUND) { reap_worker(loop);continue; }
                KERNEL_RAW_SCOPE(guard,&loop->metadata);
                selected=loop;sequence=loop->detach_sequence;
                loop->info.flags|=KERNEL_LOOP_AUTOCLEAR;loop->check_owners=1;wake(&loop->work);break;
            }
        }
        if (!selected) return progressed;
        for (;;) {
            struct kernel_wait_token token={0};int done,error;
            {
                KERNEL_RAW_SCOPE(guard,&selected->metadata);
                done=selected->detach_sequence!=sequence || selected->worker_done;
                error=selected->detach_error;
                if (!done && kernel_wait_prepare(&selected->changed,0,0,&token)) __builtin_trap();
            }
            if (done) { if (error) return error;break; }
            enum kernel_wait_wake_reason reason;
            if (kernel_wait_park(&token,&reason) || kernel_wait_finish(&token)) __builtin_trap();
        }
        progressed++;
    }
}
int kernel_loop_shutdown(void)
{
    if (!loops_ready) return 0;
    int error=kernel_loop_release_unused();if (error<0) return error;
    KERNEL_LOCK_SCOPE(control);kernel_mutex_lock(&loop_control,&control);
    for (struct loop_device *loop=loops;loop;loop=loop->next) {
        if (loop->state!=LOOP_UNBOUND || loop->backing) return -KERNEL_EBUSY;
        uint32_t references;kernel_block_usage(&loop->block,&references,NULL);
        if (references) return -KERNEL_EBUSY;
    }
    while (loops) {
        struct loop_device *loop=loops;
        reap_worker(loop);
        error=kernel_block_unregister(&loop->block);if (error) return error;
        loops=loop->next;
        if (kernel_wait_queue_close(&loop->work) || kernel_wait_queue_destroy(&loop->work) ||
            kernel_wait_queue_close(&loop->changed) || kernel_wait_queue_destroy(&loop->changed)) __builtin_trap();
        if (loop->dynamic) kernel_heap_release(loop->heap,loop);
    }
    loops_ready=0;loop_heap=NULL;return 0;
}

/* PID1已退出且用户owner已排空。树上的先后顺序不等于后备依赖顺序：
 * 先尝试所有叶挂载，再归还无使用者loop，使其后备所在挂载可以卸载。 */
int kernel_loop_shutdown_mounts(struct kernel_vfs_mount *root)
{
    if (!root || !root->private_data) return -KERNEL_EINVAL;
    while (root->first_child) {
        int progress=kernel_loop_release_unused();
        if (progress<0) return progress;
        struct kernel_vfs_mount *leaf=root->first_child;
        while (leaf->first_child) leaf=leaf->first_child;
        int detached=0;
        while (leaf) {
            int error=kernel_vfs_mount_prepare_detach(leaf,NULL);
            if (!error) {
                error=kernel_vfs_mount_detach(leaf,NULL);
                if (error) return error;
                error=kernel_vfs_unmount(leaf);
                if (error) { kernel_vfs_disk_defer_cleanup(leaf);return error; }
                detached=1;break;
            }
            if (error!=-KERNEL_EBUSY) return error;
            while (leaf->parent!=root && !leaf->next_sibling) leaf=leaf->parent;
            if (!leaf->next_sibling) break;
            leaf=leaf->next_sibling;
            while (leaf->first_child) leaf=leaf->first_child;
        }
        if (!detached && !progress) return -KERNEL_EBUSY;
    }
    return kernel_loop_shutdown();
}
