#include <kernel/socket_call.h>
#include <kernel/socket.h>
#include <kernel/socket_task.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/heap.h>
#include <kernel/open_file.h>
#include <kernel/pid.h>
#include <kernel/task.h>
#include <kernel/tty_task.h>
#include <kernel/uaccess.h>
#include "../../fs/files/private.h"
#include "../../fs/uaccess_iov_internal.h"

#include <string.h>

#define SOL_SOCKET 1
#define SCM_RIGHTS 1
#define SCM_CREDENTIALS 2
#define MSG_CTRUNC 8U
#define MSG_CMSG_CLOEXEC 0x40000000U
/* Fixed Linux initial net namespace optmem_max. Large ancillary imports have
 * the same bounded allocation and ENOBUFS boundary as sock_kmalloc. */
#define CONTROL_LIMIT (128U * 1024U)
struct control_header { uint64_t length; int32_t level, type; };
struct control_credentials { int32_t pid; uint32_t uid, gid; };
_Static_assert(sizeof(struct control_header)==16,"native cmsghdr");
_Static_assert(sizeof(struct control_credentials)==12,"native ucred");

int kernel_socket_call_begin(struct kernel_socket_call *call, struct kernel_files *files,
    struct kernel_open_file_description **owner)
{
    *call=(struct kernel_socket_call){.files=files,.heap=files->heap,.task=kernel_task_current(),.socket_owner=owner};
    if (kernel_task_socket_call_register(call->task,call)!=KERNEL_TASK_STATUS_OK) __builtin_trap();
    return 0;
}
static void socket_call_cleanup(struct kernel_socket_call *call, int aborting)
{
    if (call->fd_reserved) {
        struct kernel_files_fd_reservation reservation={call->files,call->reserved_fd};
        kernel_files_cancel_reservation(&reservation);
        call->fd_reserved=0;
    }
    kernel_socket_control_release(&call->message.control);
    kernel_socket_control_release(&call->message.credentials);
    if (call->import) { kernel_heap_release(call->heap,call->import); call->import=0; }
    if (call->iov_snapshot) {
        kernel_uaccess_iov_cleanup(call->iov_snapshot,call->heap);
        call->iov_snapshot=0;
    }
    if (call->address) {kernel_socket_address_release(call->address);call->address=0;}
    if (aborting && call->socket_owner) {
        enum kernel_files_status status=kernel_files_release_uninstalled_description(call->files,call->socket_owner);
        if (status!=KERNEL_FILES_STATUS_OK && status!=KERNEL_FILES_STATUS_CLEANUP_REQUIRED) __builtin_trap();
        call->socket_owner=0;
    }
    if (call->task) {
        if (kernel_task_socket_call_clear(call->task,call)!=KERNEL_TASK_STATUS_OK) __builtin_trap();
        call->task=0;
    }
}
void kernel_socket_call_abort(struct kernel_socket_call *call)
{ socket_call_cleanup(call,1); }
void kernel_socket_call_end(struct kernel_socket_call *call)
{
    /* Keep the outer pin registered while cleanup may sleep. Only normal
     * return leaves it to release_socket, after the registration is removed. */
    socket_call_cleanup(call,0);
    call->socket_owner=0;
}

static int set_credentials(struct kernel_socket_control *control, const struct control_credentials *value)
{
    const struct kernel_credentials *cred=kernel_credentials_current();
    kernel_pid_t tgid=0;
    if (kernel_task_tgid(kernel_task_current(),&tgid)!=KERNEL_TASK_STATUS_OK) __builtin_trap();
    if (value->uid==UINT32_MAX || value->gid==UINT32_MAX) return -KERNEL_EINVAL;
    if ((value->pid!=tgid && !kernel_credentials_capable(cred,KERNEL_CAP_SYS_ADMIN)) ||
        ((value->uid!=cred->uid && value->uid!=cred->euid && value->uid!=cred->suid) &&
         !kernel_credentials_capable(cred,KERNEL_CAP_SETUID)) ||
        ((value->gid!=cred->gid && value->gid!=cred->egid && value->gid!=cred->sgid) &&
         !kernel_credentials_capable(cred,KERNEL_CAP_SETGID))) return -KERNEL_EPERM;
    struct kernel_pid *pid=0;
    /* The identity helper's zero selects self for automatic credentials; an
     * explicitly supplied zero follows find_get_pid(0), which has no owner. */
    if (!value->pid) return -KERNEL_ESRCH;
    int error=kernel_task_socket_identity(value->pid,&pid);
    if (error) return error;
    if (control->pid) kernel_task_identity_put(control->pid);
    control->pid=pid; control->uid=value->uid; control->gid=value->gid;
    control->has_credentials=1;
    return 0;
}

int kernel_socket_control_import(struct kernel_socket_call *call, struct kernel_mm *mm,
    uint64_t address, uint64_t length)
{
    if (!length) return 0;
    if (length>INT32_MAX || length>=CONTROL_LIMIT) return -KERNEL_ENOBUFS;
    if (kernel_heap_allocate(call->heap,(size_t)length,&call->import)!=KERNEL_HEAP_STATUS_OK)
        return -KERNEL_ENOBUFS;
    size_t copied=0;
    if (kernel_copy_from_user(mm,call->import,address,(size_t)length,&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=length)
        return -KERNEL_EFAULT;
    /* Size planning does not report parse errors: the actual pass preserves
     * the ordering of header, FD lookup and credential checks. */
    uint32_t capacity=0;
    for (uint64_t offset=0; length-offset>=sizeof(struct control_header);) {
        struct control_header header;
        memcpy(&header,(unsigned char *)call->import+offset,sizeof(header));
        if (header.length<sizeof(header) || header.length>length-offset) break;
        if (header.level==SOL_SOCKET && header.type==SCM_RIGHTS) {
            uint64_t count=(header.length-sizeof(header))/sizeof(int32_t);
            capacity=count>KERNEL_SOCKET_SCM_MAX_FD-capacity ? KERNEL_SOCKET_SCM_MAX_FD : capacity+(uint32_t)count;
        }
        uint64_t step=(header.length+7U)&~UINT64_C(7);
        if (step>length-offset) break;
        offset+=step;
    }
    for (uint64_t offset=0; length-offset>=sizeof(struct control_header);) {
        struct control_header header;
        unsigned char *data=(unsigned char *)call->import+offset;
        memcpy(&header,data,sizeof(header));
        if (header.length<sizeof(header) || header.length>length-offset) return -KERNEL_EINVAL;
        if (header.level==SOL_SOCKET) {
            if (header.type!=SCM_RIGHTS && header.type!=SCM_CREDENTIALS) return -KERNEL_EINVAL;
            if (!call->message.control) {
                int error=kernel_socket_control_allocate(call->heap,capacity,&call->message.control);
                if (error) return error;
            }
            struct kernel_socket_control *control=call->message.control;
            if (header.type==SCM_RIGHTS) {
                uint64_t count=(header.length-sizeof(header))/sizeof(int32_t);
                if (count>KERNEL_SOCKET_SCM_MAX_FD-control->count) return -KERNEL_EINVAL;
                for (uint64_t i=0;i<count;i++) {
                    int32_t fd; int64_t error=0;
                    memcpy(&fd,data+sizeof(header)+i*sizeof(fd),sizeof(fd));
                    enum kernel_files_status status=kernel_files_pin(call->files,fd,&control->files[control->count],&error);
                    if (status) __builtin_trap();
                    if (error) return (int)error;
                    control->count++;
                }
            } else {
                if (header.length!=sizeof(header)+sizeof(struct control_credentials)) return -KERNEL_EINVAL;
                struct control_credentials credentials;
                memcpy(&credentials,data+sizeof(header),sizeof(credentials));
                int error=set_credentials(control,&credentials);
                if (error) return error;
            }
        }
        uint64_t step=(header.length+7U)&~UINT64_C(7);
        if (step>length-offset) break;
        offset+=step;
    }
    kernel_heap_release(call->heap,call->import); call->import=0;
    return 0;
}

static int copy_output(struct kernel_mm *mm,uint64_t address,const void *data,size_t length)
{
    size_t copied=0;
    return kernel_copy_to_user(mm,address,data,length,&copied)!=KERNEL_UACCESS_STATUS_OK || copied!=length;
}
void kernel_socket_control_export(struct kernel_socket_call *call,struct kernel_mm *mm,
    uint64_t address,uint64_t capacity,uint32_t flags,int passcred,uint64_t *used,uint32_t *output_flags)
{
    struct kernel_socket_control *control=call->message.control;
    uint32_t missing=call->message.stream_cookie==KERNEL_SOCKET_STREAM_COOKIE_EMPTY?0U:65534U;
    struct kernel_socket_control defaults={.uid=missing,.gid=missing,.has_credentials=1};
    *used=0;
    if (!control) { if (!passcred) return; control=&defaults; }
    if (!address) {
        if (passcred || (control->count && call->message.stream_cookie!=KERNEL_SOCKET_STREAM_COOKIE_FAULT))
            *output_flags|=MSG_CTRUNC;
        return;
    }
    if (passcred) {
        struct {struct control_header header; struct control_credentials value;} record={
            .header={sizeof(struct control_header)+sizeof(struct control_credentials),SOL_SOCKET,SCM_CREDENTIALS},
            .value={control->has_credentials && control->pid?control->pid->number:0,control->has_credentials?control->uid:65534,control->has_credentials?control->gid:65534}};
        uint64_t length=record.header.length;
        if (capacity<length) { length=capacity; *output_flags|=MSG_CTRUNC; }
        record.header.length=length;
        /* put_cmsg can copy a truncated record; output faults consume the
         * event and do not turn the payload result into an all-or-none copy. */
        if (capacity>=sizeof(struct control_header) && !copy_output(mm,address,&record,(size_t)length)) {
            uint64_t advance=(length+7U)&~UINT64_C(7);
            if (advance>capacity) advance=capacity;
            *used=advance;address+=advance;capacity-=advance;
        }
    }
    if (call->message.stream_cookie==KERNEL_SOCKET_STREAM_COOKIE_FAULT) return;
    if (control->count) {
        uint64_t fitting=capacity>sizeof(struct control_header)?(capacity-sizeof(struct control_header))/4U:0;
        uint32_t maximum=fitting>control->count?control->count:(uint32_t)fitting;
        uint32_t delivered=0;
        for (;delivered<maximum;delivered++) {
            int error=kernel_files_receive_fd(call->files,mm,address+sizeof(struct control_header)+delivered*4U,
                (flags&MSG_CMSG_CLOEXEC)?1U:0U,&control->files[delivered],call);
            if (error) break;
        }
        if (delivered) {
            uint64_t length=sizeof(struct control_header)+delivered*4U;
            int32_t level=SOL_SOCKET,type=SCM_RIGHTS;
            if (!copy_output(mm,address+8,&level,4) && !copy_output(mm,address+12,&type,4) &&
                !copy_output(mm,address,&length,8)) {
                uint64_t advance=(length+7U)&~UINT64_C(7);
                *used+=advance>capacity?capacity:advance;
            }
        }
        if (delivered<control->count) *output_flags|=MSG_CTRUNC;
    }
}
