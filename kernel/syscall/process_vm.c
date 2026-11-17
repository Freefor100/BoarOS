#include "private.h"
#include <kernel/errno.h>
#include <kernel/remote_memory.h>
#include <kernel/task.h>
#include <kernel/task_state.h>
#include "../../fs/uaccess_iov_internal.h"

struct kernel_remote_memory_request {
    struct kernel_mm remote;
    int remote_registered;
    struct kernel_heap *heap;
    struct kernel_uaccess_iov_snapshot local_iov, remote_iov;
    struct kernel_task_io_buffer scratch;
};

int kernel_remote_memory_abort(struct kernel_task *task)
{
    struct kernel_remote_memory_request *call=task->remote_memory_request;
    if (!call) return 0;
    if (call->remote_registered) {
        kernel_mm_core_remote_end(&call->remote);call->remote_registered=0;
    }
    if (call->scratch.allocator) kernel_task_io_buffer_release(&call->scratch);
    kernel_uaccess_iov_cleanup(&call->remote_iov,call->heap);
    kernel_uaccess_iov_cleanup(&call->local_iov,call->heap);
    if (call->remote.state==KERNEL_MM_LIVE || call->remote.state==KERNEL_MM_CLEANUP) {
        enum kernel_mm_status status=kernel_mm_release(&call->remote);
        if (status==KERNEL_MM_STATUS_CLEANUP_REQUIRED) return -KERNEL_EIO;
        if (status) __builtin_trap();
    }
    task->remote_memory_request=0;
    kernel_heap_release(call->heap,call);
    return 0;
}

/* Remote descriptors have signed lengths but deliberately no local access_ok:
 * remote pages are checked as they are reached, preserving a completed prefix. */
static int import_remote(struct kernel_remote_memory_request *call,
    struct kernel_mm *mm,uint64_t address,size_t count)
{
    struct kernel_uaccess_iov_snapshot *snapshot=&call->remote_iov;
    if (count>1024) return -KERNEL_EINVAL;
    snapshot->view=snapshot->local;snapshot->count=count;
    if (!count) return 0;
    struct kernel_uaccess_iovec *iov=snapshot->local;
    if (count>8) {
        enum kernel_heap_status status=kernel_heap_allocate(call->heap,count*sizeof(*iov),&snapshot->allocation);
        if (status==KERNEL_HEAP_STATUS_EMPTY) return -KERNEL_ENOMEM;
        if (status) __builtin_trap();
        iov=snapshot->allocation;snapshot->view=iov;
    }
    size_t copied=0;
    enum kernel_uaccess_status status=kernel_copy_from_user(mm,iov,address,count*sizeof(*iov),&copied);
    if (status==KERNEL_UACCESS_STATUS_FAULT) return -KERNEL_EFAULT;
    if (status || copied!=count*sizeof(*iov)) __builtin_trap();
    for (size_t i=0;i<count;i++) {
        if (iov[i].length>INT64_MAX) return -KERNEL_EINVAL;
        /* Only non-emptiness is needed; remote totals are never summed. */
        if (iov[i].length) snapshot->total=1;
    }
    return 0;
}

static int64_t transfer(struct kernel_task *caller,const struct kernel_syscall_request *request)
{
    if (request->arguments[5]) return -KERNEL_EINVAL;
    if (request->arguments[2]>1024) return -KERNEL_EINVAL;
    struct kernel_mm *mm;
    struct kernel_files *files;
    if (kernel_task_mm_borrow_mutable(caller,&mm) || kernel_task_files_borrow(caller,&files))
        __builtin_trap();
    if (kernel_remote_memory_abort(caller)) return -KERNEL_EIO;
    struct kernel_remote_memory_request *call=0;
    enum kernel_heap_status allocated=kernel_heap_allocate_zeroed(files->heap,1,sizeof(*call),(void **)&call);
    if (allocated==KERNEL_HEAP_STATUS_EMPTY) return -KERNEL_ENOMEM;
    if (allocated) __builtin_trap();
    call->heap=files->heap;
    caller->remote_memory_request=call;
    int64_t result=0;
    if (kernel_uaccess_iov_import(&call->local_iov,call->heap,mm,request->arguments[1],
        request->arguments[2],&result)) __builtin_trap();
    if (result || !call->local_iov.total) goto out;
    result=import_remote(call,mm,request->arguments[3],request->arguments[4]);
    if (result || !call->remote_iov.total) goto out;
    result=kernel_task_remote_mm_acquire((int32_t)request->arguments[0],&call->remote);
    if (result) goto out;
    if (!kernel_mm_core_remote_begin(&call->remote)) { result=-KERNEL_ESRCH;goto out; }
    call->remote_registered=1;
    if (kernel_task_io_buffer_acquire(&call->scratch,mm->allocator)) {
        result=-KERNEL_ENOMEM;goto out;
    }
    struct kernel_uaccess_iov_cursor local={call->local_iov.view,call->local_iov.count,0,0};
    struct kernel_uaccess_iov_cursor remote={call->remote_iov.view,call->remote_iov.count,0,0};
    uint64_t completed=0;
    int writing=request->number==271;
    for (;;) {
        uint64_t local_address,remote_address;
        size_t chunk=kernel_uaccess_iov_span(&local,BOAROS_PAGE_SIZE,&local_address);
        if (!chunk) break;
        chunk=kernel_uaccess_iov_span(&remote,chunk,&remote_address);
        if (!chunk) break;
        size_t source=0,destination=0;
        int error=kernel_mm_remote_transfer(writing?mm:&call->remote,
            writing?local_address:remote_address,call->scratch.data,chunk,0,0,&source);
        /* The source pin has returned before destination fault/COW can sleep. */
        if (source) {
            int write_error=kernel_mm_remote_transfer(writing?&call->remote:mm,
                writing?remote_address:local_address,call->scratch.data,source,1,0,&destination);
            local.offset+=destination;remote.offset+=destination;completed+=destination;
            if (write_error) error=write_error;
        }
        if (error || destination!=chunk) {
            result=error?error:-KERNEL_EFAULT;break;
        }
    }
    if (completed) result=(int64_t)completed;
out:
    if (kernel_remote_memory_abort(caller) && result<=0) result=-KERNEL_EIO;
    return result;
}

enum kernel_syscall_status syscall_handle_process_vm(struct kernel_task *caller,
    const struct kernel_syscall_request *request,struct kernel_syscall_result *decoded)
{
    decoded->action=KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value=transfer(caller,request);
    return KERNEL_SYSCALL_STATUS_OK;
}
