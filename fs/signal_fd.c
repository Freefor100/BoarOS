#include <kernel/credentials.h>
#include "open_file_internal.h"
#include "files/private.h"
#include "uaccess_iov_internal.h"
#include <kernel/signal_fd.h>
#include <kernel/signal.h>
#include <kernel/heap.h>
#include <kernel/task.h>
#include <kernel/wait_internal.h>
#include <kernel/errno.h>
#include <string.h>

struct kernel_signal_fd {
    struct kernel_open_file_description file;
    /* The wait domain protects this shared OFD mask and pending publication. */
    uint64_t mask;
};
struct signal_fd_info {
    uint32_t signal;int32_t error,code;uint32_t pid,uid;int32_t fd;
    uint32_t tid,band,overrun,trap;int32_t status,value;
    uint64_t pointer,user_time,system_time,address;
    uint16_t address_lsb,pad;int32_t syscall;
    uint64_t call_address;uint32_t arch;uint8_t padding[28];
};
_Static_assert(sizeof(struct signal_fd_info)==128,"Linux signalfd_siginfo");
static void encode(struct signal_fd_info *out,const struct kernel_signal_wait_info *info)
{
    memset(out,0,sizeof(*out));out->signal=info->signal;out->error=info->error;out->code=info->code;
    int code=info->code;unsigned signal=info->signal;
    if (code==-2) { out->tid=info->sender;out->overrun=info->uid;out->pointer=info->value;out->value=(int32_t)info->value;return; }
    int fault=code>0 && code<128 &&
        ((signal==4&&code<=11)||(signal==5&&code<=6)||(signal==7&&code<=5)||(signal==8&&code<=15)||(signal==11&&code<=10));
    if (fault) {out->address=info->address;if(signal==7&&(code==4||code==5))memcpy(&out->address_lsb,info->payload+8,2);return;}
    if (signal==17&&code>0&&code<=6) {
        out->pid=info->sender;out->uid=info->uid;out->status=info->status;
        out->user_time=info->user_time;out->system_time=info->system_time;return;
    }
    if (signal==31&&code>0&&code<=2) {
        out->call_address=info->address;memcpy(&out->syscall,info->payload+8,4);memcpy(&out->arch,info->payload+12,4);return;
    }
    if(code==-5 || (code>0&&code<=6)) {
        out->band=(uint32_t)info->address;memcpy(&out->fd,info->payload+8,4);return;
    }
    out->pid=info->sender;out->uid=info->uid;
    if(code<0){out->pointer=info->value;out->value=(int32_t)info->value;}
}
enum kernel_files_status kernel_files_signalfd(struct kernel_files *files,struct kernel_task *task,
    int32_t fd,uint64_t mask,uint32_t flags,int64_t *result)
{
    if (flags&~(04000U|02000000U)) { *result=-KERNEL_EINVAL;return 0; }
    mask&=~KERNEL_SIGNAL_UNBLOCKABLE_MASK;
    if(fd!=-1) {
        KERNEL_FILES_PIN_SCOPE(pin);
        struct kernel_open_file_description *file=kernel_files_hold_fd(files,fd,&pin);
        if(!file){*result=-KERNEL_EBADF;return 0;}
        if(file->kind!=KERNEL_OPEN_FILE_KIND_SIGNALFD){*result=-KERNEL_EINVAL;return 0;}
        struct kernel_wait_queue *source=kernel_signal_fd_source(task);
        if(!source){*result=-KERNEL_ENOMEM;return 0;}
        struct kernel_wait_cursor cursor={0};
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            ((struct kernel_signal_fd *)file)->mask=mask;
            (void)kernel_wait_cursor_prepare_locked(source,&cursor,1);
        }
        if(cursor.queue)kernel_wait_cursor_wake(&cursor,1);
        *result=fd;return 0;
    }
    if(!kernel_signal_fd_source(task)){*result=-KERNEL_ENOMEM;return 0;}
    struct kernel_signal_fd *signal=0;
    enum kernel_heap_status allocation=kernel_heap_allocate_zeroed(files->heap,1,sizeof(*signal),(void **)&signal);
    if(allocation==KERNEL_HEAP_STATUS_EMPTY){*result=-KERNEL_ENOMEM;return 0;}
    if(allocation)__builtin_trap();
    signal->file.heap=files->heap;signal->file.references=1;signal->file.kind=KERNEL_OPEN_FILE_KIND_SIGNALFD;
    signal->file.credentials=kernel_credentials_get(kernel_credentials_current());
    signal->file.open_flags=2|(flags&04000U);signal->mask=mask;
    kernel_mutex_init(&signal->file.offset_lock,10,(uintptr_t)signal);
    struct kernel_open_file_description *owner=&signal->file;
    uint32_t slot;int error;
    enum kernel_files_status status=kernel_files_find_free_fd(files,&slot,&error);
    if(status||error){if(kernel_open_file_release(&owner))__builtin_trap();*result=error;return status;}
    KERNEL_FILES_RESERVATION_SCOPE(reservation);reservation.files=files;reservation.fd=slot;
    status=kernel_files_install_new_owned_at(files,slot,(flags&02000000U)?KERNEL_FILES_FD_CLOEXEC:0,&owner);
    if(status){if(kernel_open_file_release(&owner))__builtin_trap();return status;}
    reservation.files=0;*result=slot;return 0;
}
const uint64_t *kernel_signal_fd_mask(struct kernel_open_file_description *file)
{ return file->kind==KERNEL_OPEN_FILE_KIND_SIGNALFD?&((struct kernel_signal_fd *)file)->mask:0; }
uint32_t kernel_signal_fd_poll(struct kernel_open_file_description *file,struct kernel_wait_queue **queue)
{
    struct kernel_task *task=kernel_task_current();
    struct kernel_wait_queue *source=kernel_signal_fd_source(task);
    if(!source)return KERNEL_POLLERR;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if(queue)*queue=source;
    return kernel_signal_fd_pending_locked(task,((struct kernel_signal_fd *)file)->mask)?KERNEL_POLLIN:0;
}
int64_t kernel_signal_fd_read(struct kernel_open_file_description *file,struct kernel_mm *mm,
    const struct kernel_uaccess_iovec *iov,size_t count,uint64_t length)
{
    if(length<128)return -KERNEL_EINVAL;
    struct kernel_signal_fd *signal=(void *)file;
    struct kernel_uaccess_iov_cursor cursor={iov,count,0,0};int64_t done=0;
    int nonblock=!!(kernel_open_file_flags(file)&04000U);
    while(length-(uint64_t)done>=128) {
        struct kernel_signal_wait_info info;struct signal_fd_info output;
        int error=kernel_signal_fd_take(kernel_task_current(),&signal->mask,&info,nonblock||done);
        if(error){
            if(!done&&error==-KERNEL_ERESTARTSYS)kernel_signal_note_syscall_restart(kernel_task_current());
            return done?done:error;
        }
        encode(&output,&info);size_t copied=0;
        enum kernel_uaccess_status access=kernel_copy_to_user_iov(mm,&cursor,&output,128,&copied);
        if(access&&access!=KERNEL_UACCESS_STATUS_FAULT)__builtin_trap();
        if(copied!=128)return done?done:-KERNEL_EFAULT;
        done+=128;
    }
    return done;
}
