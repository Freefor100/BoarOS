#include "private.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/kcmp.h>
#include <kernel/random.h>
#include "../../fs/files/private.h"
#include "../../fs/vfs_internal.h"

enum { KCMP_FILE, KCMP_VM, KCMP_FILES, KCMP_FS, KCMP_SIGHAND,
    KCMP_IO, KCMP_SYSVSEM, KCMP_EPOLL_TFD, KCMP_TYPES };
static uint64_t cookies[KCMP_TYPES][2];

void kernel_kcmp_initialize(void)
{
    if (kernel_random_fill(cookies, sizeof(cookies))) __builtin_trap();
    for (unsigned i=0; i<KCMP_TYPES; i++) cookies[i][1] |= UINT64_C(0x8000000000000001);
}

static int compare(uint64_t a, uint64_t b, unsigned type)
{
    /* Odd multiplication is a permutation modulo 2^64: equality and stable
     * ordering survive without exposing the order of kernel addresses. */
    int64_t x=(int64_t)((a^cookies[type][0])*cookies[type][1]);
    int64_t y=(int64_t)((b^cookies[type][0])*cookies[type][1]);
    return (x<y) | ((x>y)<<1);
}

static int access_locked(struct kernel_task *caller, struct kernel_task *a,
    struct kernel_task *b, int32_t pid1, int32_t pid2)
{
    if (process_find_identity(pid1,KERNEL_PID_TID)!=a ||
        process_find_identity(pid2,KERNEL_PID_TID)!=b) return -KERNEL_ESRCH;
    if (!kernel_credentials_may_access_locked(caller,a,0) ||
        !kernel_credentials_may_access_locked(caller,b,0)) return -KERNEL_EPERM;
    return 0;
}

static uint64_t object_locked(const struct kernel_task *task, unsigned type)
{
    /* Linux retains sighand until the PID is reaped; the other resources
     * have departed by the time the zombie becomes observable. */
    if (type==KCMP_SIGHAND) return kernel_signal_identity_locked(task);
    if (task->proc_exiting) return 0;
    switch (type) {
    case KCMP_VM: return (uintptr_t)task->mm.record;
    case KCMP_FILES: return (uintptr_t)task->files.record;
    case KCMP_FS: return (uintptr_t)task->fs.record;
    case KCMP_SYSVSEM: return (uintptr_t)task->sem_undo;
    default: __builtin_trap();
    }
}

static uint64_t file_locked(const struct kernel_files_record *record, uint32_t fd)
{
    return fd<record->statistics.capacity ? (uintptr_t)record->slots[fd].description : 0;
}

int64_t kernel_kcmp(struct kernel_task *caller, int32_t pid1, int32_t pid2,
    int32_t type, uint64_t idx1, uint64_t idx2)
{
    SCHEDULER_TASK_BORROW(first,0);
    SCHEDULER_TASK_BORROW(second,0);
    struct kernel_task *a,*b;
    {
        KERNEL_RAW_SCOPE(domain,&kernel_wait_domain);
        first=scheduler_task_lookup_locked(pid1,KERNEL_PID_TID,0);
        if (!first) return -KERNEL_ESRCH;
        if (pid1!=pid2) {
            second=scheduler_task_lookup_locked(pid2,KERNEL_PID_TID,0);
            if (!second) return -KERNEL_ESRCH;
        }
        a=first;b=second?second:first;
    }
    /* Task borrows retain containers, while these short accesses prevent exit
     * from consuming the actual wrappers. Both pairs use raw address order. */
    struct kernel_task *low=(uintptr_t)a<(uintptr_t)b?a:b,*high=low==a?b:a;
    KERNEL_RAW_SCOPE(access1,&low->files_access);
    struct kernel_raw_guard access2 __attribute__((cleanup(kernel_raw_scope_release)))={0};
    if (low!=high) kernel_raw_lock_acquire(&high->files_access,&access2);
    struct kernel_files_record *files1=0,*files2=0;
    {
        KERNEL_RAW_SCOPE(domain,&kernel_wait_domain);
        int error=access_locked(caller,a,b,pid1,pid2);
        if (error) return error;
        if (type<0 || type>=KCMP_TYPES) return -KERNEL_EINVAL;
        if (type==KCMP_IO || type==KCMP_EPOLL_TFD) return -KERNEL_EOPNOTSUPP;
        if (type==KCMP_FILE) {
            if (a->proc_exiting || b->proc_exiting) return -KERNEL_EBADF;
            files1=kernel_files_is_live(&a->files)?a->files.record:0;
            files2=kernel_files_is_live(&b->files)?b->files.record:0;
            if (!files1 || !files2) return -KERNEL_EBADF;
        } else if (type!=KCMP_FS) {
            return compare(object_locked(a,type),object_locked(b,type),type);
        }
    }
    if (type==KCMP_FS) {
        KERNEL_TASK_LOCK_SCOPE(state,&kernel_vfs_state_lock);
        KERNEL_RAW_SCOPE(domain,&kernel_wait_domain);
        int error=access_locked(caller,a,b,pid1,pid2);
        return error?error:compare(object_locked(a,type),object_locked(b,type),type);
    }
    struct kernel_raw_lock *l1=&files1->metadata_lock,*l2=&files2->metadata_lock;
    if ((uintptr_t)l1>(uintptr_t)l2) { struct kernel_raw_lock *swap=l1;l1=l2;l2=swap; }
    KERNEL_RAW_SCOPE(fd1,l1);
    struct kernel_raw_guard fd2 __attribute__((cleanup(kernel_raw_scope_release)))={0};
    if (l1!=l2) kernel_raw_lock_acquire(l2,&fd2);
    KERNEL_RAW_SCOPE(domain,&kernel_wait_domain);
    int error=access_locked(caller,a,b,pid1,pid2);
    if (error) return error;
    if (a->proc_exiting || b->proc_exiting) return -KERNEL_EBADF;
    if (a->files.record!=files1 || b->files.record!=files2) __builtin_trap();
    /* Slots retain the real OFDs through comparison. No temporary table owner,
     * fput, final close or backend work is needed on the querying CPU. Linux's
     * unsigned-int FD parameter deliberately truncates the native ulong index. */
    uint64_t file1=file_locked(files1,(uint32_t)idx1),file2=file_locked(files2,(uint32_t)idx2);
    return file1 && file2 ? compare(file1,file2,KCMP_FILE) : -KERNEL_EBADF;
}
