#include "futex_internal.h"
#include <kernel/heap.h>
#include <kernel/time.h>

struct futex_waitv_user {uint64_t value,address;uint32_t flags,reserved;};
_Static_assert(sizeof(struct futex_waitv_user)==24,"native waitv ABI");

static void release_keys(struct kernel_futex_request *request)
{
    for(unsigned i=0;i<request->count;i++)futex_key_release(&request->entries[i].key);
}
static int consumed(struct kernel_futex_request *request)
{
    for(unsigned i=0;i<request->count;i++)if(__atomic_load_n(&request->entries[i].woken,__ATOMIC_ACQUIRE))return (int)i;
    return -1;
}
int64_t kernel_futex_waitv(struct kernel_task *task,uint64_t address,unsigned count,
    unsigned flags,uint64_t timeout,int clock,enum kernel_scheduler_status *status)
{
    *status=KERNEL_SCHEDULER_STATUS_OK;
    if(task!=kernel_cpu_current()->current || arch_interrupt_is_enabled()) {
        *status=KERNEL_SCHEDULER_STATUS_INVALID_STATE;return 0;
    }
    if(flags || !address || !count || count>128)return -KERNEL_EINVAL;
    uint64_t deadline=0,deadline_ns=0;int expired=0;
    if(timeout) {
        if(clock!=0 && clock!=1)return -KERNEL_EINVAL;
        struct {int64_t seconds,nanoseconds;} t;size_t copied;
        if(kernel_copy_from_user(&task->mm,&t,timeout,sizeof(t),&copied) || copied!=sizeof(t))return -KERNEL_EFAULT;
        if(t.seconds<0 || t.nanoseconds<0 || t.nanoseconds>=1000000000)return -KERNEL_EINVAL;
        __uint128_t ns=(__uint128_t)(uint64_t)t.seconds*1000000000+(uint64_t)t.nanoseconds;
        deadline_ns=ns>INT64_MAX?INT64_MAX:(uint64_t)ns;
        if(!clock) {uint64_t offset=kernel_time_boot_realtime_offset();deadline_ns=deadline_ns<=offset?0:deadline_ns-offset;}
        enum kernel_time_status result=kernel_time_deadline_from_monotonic(deadline_ns,&deadline);
        if(result==KERNEL_TIME_STATUS_DEADLINE_PASSED)expired=1;
        else if(result!=KERNEL_TIME_STATUS_OK)__builtin_trap();
    }
    struct kernel_futex_request request={.task=task,.heap=task->files.heap,.count=count};
    enum kernel_heap_status allocated=kernel_heap_allocate_zeroed(request.heap,count,sizeof(*request.entries),(void **)&request.entries);
    if(allocated)return allocated==KERNEL_HEAP_STATUS_EMPTY?-KERNEL_ENOMEM:-KERNEL_EIO;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);if(task->futex_request)__builtin_trap();task->futex_request=&request;}
    int64_t result=0;
    for(unsigned i=0;i<count;i++) {
        struct futex_waitv_user item;size_t copied;
        uint64_t at=address+(uint64_t)i*sizeof(item);
        if(at<address || kernel_copy_from_user(&task->mm,&item,at,sizeof(item),&copied) || copied!=sizeof(item)) {result=-KERNEL_EFAULT;goto out;}
        if(item.reserved || (item.flags&~UINT32_C(0x8f)) || (item.flags&3)!=2 || item.value>UINT32_MAX) {result=-KERNEL_EINVAL;goto out;}
        /* NUMA/MPOL是已知扩展，当前只提供32位PRIVATE/shared等待。 */
        if(item.flags&12) {result=-KERNEL_EOPNOTSUPP;goto out;}
        request.entries[i]=(struct kernel_futex_entry){.node={.task=task},.kind=FUTEX_ENTRY_VECTOR,
            .index=i,.address=item.address,.expected=(uint32_t)item.value,
            .operation=item.flags&FUTEX_PRIVATE,.bitset=FUTEX_BITSET_MATCH_ANY};
    }
    for(;;) {
        /* Linux先取得全部key再比较任何值；不把前项EAGAIN盖在后项地址错误上。 */
        for(unsigned i=0;i<count;i++) {
            struct kernel_futex_entry *entry=&request.entries[i];
            result=futex_prepare(task,&entry->address,1,entry->operation,0,0,&entry->key,0,status);
            if(result || *status)goto out;
        }
        {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
         if(kernel_wait_prepare_locked(0,deadline,1,&request.token))__builtin_trap();}
        unsigned retry=count;struct futex_bucket *busy=0;
        for(unsigned i=0;i<count;i++) {
            struct kernel_futex_entry *entry=&request.entries[i];
            struct kernel_mm_user_pin pin={0};
            enum kernel_mm_status prepared=kernel_mm_try_futex_pin(&task->mm,entry->address,
                !(entry->operation&FUTEX_PRIVATE),KERNEL_MM_READ,&entry->key,&pin);
            if(prepared) {if(prepared!=KERNEL_MM_STATUS_CONFLICT)*status=KERNEL_SCHEDULER_STATUS_INVALID_STATE;retry=i;break;}
            struct futex_bucket *bucket=futex_bucket(&entry->key);
            KERNEL_LOCK_SCOPE(guard);
            if(!kernel_mutex_try_lock(&bucket->operation,&guard)) {
                kernel_mm_unpin_user_page(&pin);retry=i;busy=bucket;break;
            }
            uint32_t *word=(void *)((unsigned char *)pin.page+(entry->address&BOAROS_PAGE_MASK));
            if(__atomic_load_n(word,__ATOMIC_ACQUIRE)!=entry->expected)result=-KERNEL_EAGAIN;
            else {
                KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
                if(kernel_wait_node_bind_locked(&bucket->waiters,&entry->node,&request.token))__builtin_trap();
            }
            kernel_lock_release(&guard);kernel_mm_unpin_user_page(&pin);
            if(result || consumed(&request)>=0)break;
        }
        enum kernel_wait_wake_reason reason=KERNEL_WAIT_WOKEN;
        if(!result && retry==count && consumed(&request)<0 && !expired && !*status)
            *status=kernel_wait_park(&request.token,&reason);
        /* finish会等待在途游标；此后再读取获胜项，包含清理期间真实发生的wake。 */
        if(kernel_wait_finish(&request.token))__builtin_trap();
        int winner=consumed(&request);
        if(winner>=0) {result=winner;goto out;}
        if(result || *status)goto out;
        if(retry<count) {
            release_keys(&request);
            if(busy) {KERNEL_LOCK_SCOPE(g);kernel_mutex_lock(&busy->operation,&g);}
            else {
                struct kernel_futex_entry *entry=&request.entries[retry];
                struct kernel_futex_key key={0};struct kernel_mm_user_pin pin={0};
                result=futex_prepare(task,&entry->address,1,entry->operation,0,KERNEL_MM_READ,&key,&pin,status);
                if(pin.mm)kernel_mm_unpin_user_page(&pin);
                futex_key_release(&key);
                if(result || *status)goto out;
            }
            continue;
        }
        if(timeout && (expired || kernel_time_monotonic_ns()>=deadline_ns)) {result=-KERNEL_ETIMEDOUT;goto out;}
        if(reason==KERNEL_WAIT_SIGNALLED) {kernel_signal_note_syscall_restart(task);result=-KERNEL_ERESTARTSYS;goto out;}
        release_keys(&request);
    }
out:
    kernel_futex_request_release(&request);
    return result;
}
