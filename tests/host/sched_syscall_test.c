#include "../../kernel/sched/private.h"
#include <kernel/syscall.h>
#include <kernel/task.h>
#include <kernel/sched_policy.h>
#include <kernel/uaccess.h>
#include <kernel/errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
_Thread_local uintptr_t sync_test_irq = 1;
static struct kernel_cpu cpu;
static struct kernel_io_context io;
static struct kernel_task task;
struct kernel_raw_lock kernel_wait_domain;
void *sync_test_cpu(void) { return &cpu; }
void *sync_test_current_task(void) { return &task; }
struct kernel_task *process_find_identity(int32_t pid, enum kernel_pid_role kind)
{ assert(kind == KERNEL_PID_TID); return pid == 1 ? &task : 0; }
static struct kernel_sched_policy state;
static uintptr_t output_limit;
int __wrap_kernel_task_sched_get(struct kernel_task *caller,int32_t pid,struct kernel_sched_policy *out)
{ (void)caller;if(pid<0)return -KERNEL_EINVAL;if(pid>1)return -KERNEL_ESRCH;*out=state;return 0; }
int __wrap_kernel_task_sched_set(struct kernel_task *caller,int32_t pid,int policy,int priority,int keep)
{ struct kernel_sched_policy old;int r=__wrap_kernel_task_sched_get(caller,pid,&old);return r?r:kernel_sched_policy_set(&state,policy,priority,keep); }
enum kernel_task_status kernel_task_mm_borrow_mutable(struct kernel_task *task,struct kernel_mm **mm)
{(void)task;*mm=0;return KERNEL_TASK_STATUS_OK;}
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *mm,uint64_t address,const void *source,size_t size,size_t *copied)
{(void)mm;*copied=0;if(address<4096)return KERNEL_UACCESS_STATUS_FAULT;size_t available=size;if(output_limit&&address+size>output_limit)available=address<output_limit?output_limit-address:0;memcpy((void *)(uintptr_t)address,source,available);*copied=available;return available==size?KERNEL_UACCESS_STATUS_OK:KERNEL_UACCESS_STATUS_FAULT;}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *mm,void *destination,uint64_t address,size_t size,size_t *copied)
{(void)mm;*copied=0;if(address<4096)return KERNEL_UACCESS_STATUS_FAULT;memcpy(destination,(void *)(uintptr_t)address,size);*copied=size;return KERNEL_UACCESS_STATUS_OK;}
extern enum kernel_syscall_status syscall_handle_sched(struct kernel_task *,const struct kernel_syscall_request *,struct kernel_syscall_result *);
static int64_t call4(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{struct kernel_syscall_request req={.number=n,.arguments={a,b,c,d}};struct kernel_syscall_result res;assert(syscall_handle_sched(&task,&req,&res)==KERNEL_SYSCALL_STATUS_OK);return res.value;}
static int64_t call(uint64_t n,uint64_t a,uint64_t b,uint64_t c)
{return call4(n,a,b,c,0);}
static void getattr_abi(void)
{
    struct native_attr {uint32_t size,policy;uint64_t flags;int32_t nice;uint32_t priority;uint64_t runtime,deadline,period;uint32_t min,max;} attr;
    _Static_assert(sizeof(attr)==56,"native sched_attr size");
    unsigned char bytes[80],expected[56];
    for(unsigned size=48;size<=56;size++) {
        memset(bytes,0xa5,sizeof(bytes));
        assert(call(275,0,(uintptr_t)bytes,size)==0);
        memcpy(&attr,bytes,size);assert(attr.size==size&&attr.policy==1&&attr.flags==1&&attr.nice==0&&attr.priority==50);
        for(unsigned i=size;i<sizeof(bytes);i++)assert(bytes[i]==0xa5);
    }
    memset(bytes,0xa5,sizeof(bytes));assert(call(275,0,(uintptr_t)bytes,sizeof(bytes))==0);
    memcpy(&attr,bytes,56);assert(attr.size==56&&attr.runtime==0&&attr.deadline==0&&attr.period==0&&attr.min==0&&attr.max==0);
    for(unsigned i=56;i<sizeof(bytes);i++)assert(!bytes[i]);
    assert(call(275,0,0,56)==-KERNEL_EINVAL);
    assert(call(275,-1,(uintptr_t)bytes,56)==-KERNEL_EINVAL);
    assert(call(275,2,1,47)==-KERNEL_EINVAL);
    assert(call(275,0,(uintptr_t)bytes,4097)==-KERNEL_EINVAL);
    assert(call4(275,2,1,56,1)==-KERNEL_ESRCH);
    assert(call4(275,0,1,56,1)==-KERNEL_EINVAL);
    assert(call(275,2,1,56)==-KERNEL_ESRCH);
    assert(call(275,0,1,56)==-KERNEL_EFAULT);
    assert(call4(275,UINT64_C(0x100000000),(uintptr_t)bytes,UINT64_C(0x100000038),UINT64_C(0x100000000))==0);
    memcpy(expected,bytes,56);memset(bytes,0xa5,sizeof(bytes));output_limit=(uintptr_t)bytes+32;
    assert(call(275,0,(uintptr_t)bytes,56)==-KERNEL_EFAULT);
    assert(!memcmp(bytes,expected,32));for(unsigned i=32;i<sizeof(bytes);i++)assert(bytes[i]==0xa5);
    memset(bytes,0xa5,sizeof(bytes));output_limit=(uintptr_t)bytes+64;
    assert(call(275,0,(uintptr_t)bytes,80)==-KERNEL_EFAULT);
    for(unsigned i=0;i<56;i++)assert(bytes[i]==0xa5);
    for(unsigned i=56;i<64;i++)assert(!bytes[i]);
    output_limit=0;
    int priority=0;assert(call(119,0,0,(uintptr_t)&priority)==0);
    assert(call(275,0,(uintptr_t)&attr,56)==0);
    assert(!attr.policy&&!attr.priority&&!attr.flags&&attr.runtime==10000000);
}
int main(void)
{
    kernel_cpu_initialize(&cpu, 0, &io);
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    task.accounted = 1; task.arch.user_mode = 1; task.tid_owned = 1;
    int priority=50, copied=-1;
    assert(call(125,1,0,0)==99 && call(126,2,0,0)==1);
    assert(call(125,3,0,0)==0 && call(126,5,0,0)==0);
    assert(call(125,6,0,0)==0 && call(126,7,0,0)==0);
    assert(call(125,4,0,0)==-KERNEL_EINVAL);
    assert(call(119,0,2,(uintptr_t)&priority)==0);
    assert(call(120,0,0,0)==2);
    assert(call(121,0,(uintptr_t)&copied,0)==0 && copied==50);
    assert(call(119,0,1,0)==-KERNEL_EINVAL);
    assert(call(119,0,1,1)==-KERNEL_EFAULT);
    assert(call(120,2,0,0)==-KERNEL_ESRCH);
    assert(call(120,-1,0,0)==-KERNEL_EINVAL);
    uint64_t mask=1;
    assert(call(122,0,1,(uintptr_t)&mask)==0);
    mask=2;assert(call(122,0,8,(uintptr_t)&mask)==-KERNEL_EINVAL);
    assert(call(123,0,1,(uintptr_t)&mask)==-KERNEL_EINVAL);
    assert(call(123,0,8,(uintptr_t)&mask)==8 && mask==1);
    assert(call(123,0,8,1)==-KERNEL_EFAULT);
    mask=0; assert(call(122,0,8,(uintptr_t)&mask)==-KERNEL_EINVAL);
    mask=3; assert(call(122,1,8,(uintptr_t)&mask)==0);
    assert(call(122,2,8,(uintptr_t)&mask)==-KERNEL_ESRCH);
    assert(call(123,2,8,(uintptr_t)&mask)==-KERNEL_ESRCH);
    assert(call(122,-1,8,(uintptr_t)&mask)==-KERNEL_ESRCH);
    assert(call(123,-1,8,(uintptr_t)&mask)==-KERNEL_ESRCH);
    assert(call(122,0,0,(uintptr_t)&mask)==-KERNEL_EINVAL);
    assert(call(122,0,8,1)==-KERNEL_EFAULT);
    struct {int64_t sec,ns;} interval;
    assert(call(127,0,(uintptr_t)&interval,0)==0 && interval.sec==0 && interval.ns==100000000);
    assert(call(119,0,1|0x40000000,(uintptr_t)&priority)==0);
    assert(call(120,0,0,0)==(1|0x40000000));
    assert(call(127,0,(uintptr_t)&interval,0)==0 && interval.ns==0);
    getattr_abi();
    puts("scheduler syscall flags/priority/affinity ABI parsing PASS");
}
