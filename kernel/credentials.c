#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/io_context.h>
#include <kernel/task.h>
#include <kernel/task_state.h>
#include <kernel/wait_internal.h>
#include <string.h>

#define ACCOUNT_BUCKETS 64U
static struct kernel_uid_account root_account={.references=1};
static struct kernel_uid_account *accounts[ACCOUNT_BUCKETS]={&root_account};
static const struct kernel_credentials initial={
    .effective=KERNEL_CAP_VALID_MASK,.permitted=KERNEL_CAP_VALID_MASK,
    .bounding=KERNEL_CAP_VALID_MASK,.account=&root_account,
};

const struct kernel_credentials *kernel_credentials_initial(void) {return &initial;}
const struct kernel_credentials *kernel_credentials_task_locked(const struct kernel_task *task)
{return task&&task->credentials?task->credentials:&initial;}
#if !defined(__riscv) && !defined(__loongarch__)
static _Thread_local const struct kernel_credentials *host_override;
#endif
static struct kernel_task *executing_task(void)
{
#if defined(__riscv) || defined(__loongarch__)
    return arch_current_thread_get();
#else
    return kernel_task_current();
#endif
}
const struct kernel_credentials *kernel_credentials_current(void)
{
    struct kernel_task *task=executing_task();
    if(task&&task->credentials_override)return task->credentials_override;
#if !defined(__riscv) && !defined(__loongarch__)
    if(host_override)return host_override;
#endif
    return kernel_credentials_task_locked(task);
}
const struct kernel_credentials *kernel_credentials_override(const struct kernel_credentials *replacement)
{
    struct kernel_task *task=executing_task();
    if(task){const struct kernel_credentials *previous=task->credentials_override;task->credentials_override=replacement;return previous;}
#if !defined(__riscv) && !defined(__loongarch__)
    const struct kernel_credentials *previous=host_override;host_override=replacement;return previous;
#else
    __builtin_trap();
#endif
}
const struct kernel_credentials *kernel_credentials_get(const struct kernel_credentials *cred)
{
    if(cred&&cred->heap) {
        uint32_t before=__atomic_fetch_add((uint32_t *)&cred->references,1,__ATOMIC_RELAXED);
        if(!before||before==UINT32_MAX)__builtin_trap();
    }
    return cred;
}
const struct kernel_credentials *kernel_credentials_snapshot(const struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    return kernel_credentials_get(kernel_credentials_task_locked(task));
}
struct kernel_uid_account *kernel_uid_account_get_locked(struct kernel_uid_account *a)
{
    if(!a||!a->references||a->references==UINT64_MAX)__builtin_trap();
    if(a->heap)++a->references;
    return a;
}
struct kernel_uid_account *kernel_uid_account_put_locked(struct kernel_uid_account *a)
{
    if(!a)return 0;
    if(!a->heap)return 0;
    if(!a->references)__builtin_trap();
    if(--a->references)return 0;
    if(!a->heap||a->pending||a->processes||a->shm_locked_pages||a->mqueue_bytes||a->pipe_pages)__builtin_trap();
    struct kernel_uid_account **p=&accounts[a->uid%ACCOUNT_BUCKETS];
    while(*p&&*p!=a)p=&(*p)->next;
    if(!*p)__builtin_trap();
    *p=a->next;a->next=0;return a;
}
void kernel_uid_account_release(struct kernel_uid_account *a)
{if(a)kernel_heap_release(a->heap,a);}
void kernel_uid_account_put(struct kernel_uid_account *a)
{
    /* 初始 UID 0 账户随内核存活；其 pending 仍受调度域保护，归还静态
     * 引用无需再进入调度锁。动态 UID 账户继续在最后借用后摘链释放。 */
    if(!a||!a->heap)return;
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);a=kernel_uid_account_put_locked(a);}
    kernel_uid_account_release(a);
}
int kernel_uid_account_shm_lock(struct kernel_uid_account *account,uint64_t pages,uint64_t limit)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if(!pages || !account || !account->references)__builtin_trap();
    if(pages>limit || account->shm_locked_pages>limit-pages)return -KERNEL_ENOMEM;
    kernel_uid_account_get_locked(account);
    account->shm_locked_pages+=pages;
    return 0;
}
void kernel_uid_account_shm_unlock(struct kernel_uid_account *account,uint64_t pages)
{
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(!pages || !account || account->shm_locked_pages<pages)__builtin_trap();
        account->shm_locked_pages-=pages;
        account=kernel_uid_account_put_locked(account);
    }
    kernel_uid_account_release(account);
}
int kernel_uid_account_mqueue_charge(struct kernel_uid_account *account,uint64_t bytes,uint64_t limit)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if(!account || !account->references)__builtin_trap();
    if(bytes>limit || account->mqueue_bytes>limit-bytes)return -KERNEL_EMFILE;
    kernel_uid_account_get_locked(account);account->mqueue_bytes+=bytes;
    return 0;
}
void kernel_uid_account_mqueue_uncharge(struct kernel_uid_account *account,uint64_t bytes)
{
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(!account || account->mqueue_bytes<bytes)__builtin_trap();
        account->mqueue_bytes-=bytes;account=kernel_uid_account_put_locked(account);
    }
    kernel_uid_account_release(account);
}
uint64_t kernel_uid_account_pipe_pages(struct kernel_uid_account *account,uint64_t old_pages,uint64_t new_pages)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if(!account || !account->references || account->pipe_pages<old_pages ||
        new_pages>UINT64_MAX-(account->pipe_pages-old_pages))__builtin_trap();
    account->pipe_pages=account->pipe_pages-old_pages+new_pages;
    return account->pipe_pages;
}
struct kernel_uid_account *kernel_uid_account_get(struct kernel_heap *heap,uint32_t uid)
{
    unsigned bucket=uid%ACCOUNT_BUCKETS;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        for(struct kernel_uid_account *a=accounts[bucket];a;a=a->next)
            if(a->uid==uid)return kernel_uid_account_get_locked(a);
    }
    struct kernel_uid_account *candidate=0,*result=0;
    if(!heap||kernel_heap_allocate_zeroed(heap,1,sizeof(*candidate),(void **)&candidate)!=KERNEL_HEAP_STATUS_OK)
        return 0;
    candidate->heap=heap;candidate->uid=uid;candidate->references=1;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        for(struct kernel_uid_account *a=accounts[bucket];a;a=a->next)
            if(a->uid==uid){result=kernel_uid_account_get_locked(a);break;}
        if(!result){candidate->next=accounts[bucket];accounts[bucket]=candidate;result=candidate;candidate=0;}
    }
    if(candidate)kernel_heap_release(heap,candidate);
    return result;
}
struct kernel_credential_groups *kernel_credential_groups_allocate(struct kernel_heap *heap,uint32_t count)
{
    struct kernel_credential_groups *g=0;
    if(count>KERNEL_GROUPS_MAX||!heap||kernel_heap_allocate(heap,sizeof(*g)+(size_t)count*sizeof(uint32_t),(void **)&g)!=KERNEL_HEAP_STATUS_OK)
        return 0;
    g->heap=heap;g->references=1;g->count=count;return g;
}
void kernel_credential_groups_put(struct kernel_credential_groups *g)
{
    if(!g)return;
    uint32_t before=__atomic_fetch_sub(&g->references,1,__ATOMIC_ACQ_REL);
    if(!before)__builtin_trap();
    if(before==1)kernel_heap_release(g->heap,g);
}
/* 原地 heapsort 使 65536 个补充组也保持 O(n log n)，并保留重复项。 */
static void groups_sift(uint32_t *a,size_t root,size_t count)
{
    while(root<count/2){size_t child=root*2+1;
        if(child+1<count&&a[child]<a[child+1])++child;
        if(a[root]>=a[child])break;
        uint32_t value=a[root];a[root]=a[child];a[child]=value;root=child;}
}
void kernel_credential_groups_sort(struct kernel_credential_groups *g)
{
    size_t n=g->count;
    for(size_t i=n/2;i;i--)groups_sift(g->ids,i-1,n);
    while(n>1){uint32_t value=g->ids[0];g->ids[0]=g->ids[n-1];g->ids[n-1]=value;groups_sift(g->ids,0,--n);}
}
int kernel_credentials_in_group(const struct kernel_credentials *cred,uint32_t gid)
{
    if(cred->fsgid==gid)return 1;
    const struct kernel_credential_groups *g=cred->groups;
    size_t lo=0,hi=g?g->count:0;
    while(lo<hi){size_t mid=lo+(hi-lo)/2;uint32_t v=g->ids[mid];
        if(v==gid)return 1;
        if(v<gid)lo=mid+1;else hi=mid;}
    return 0;
}
int kernel_credentials_capable(const struct kernel_credentials *cred,unsigned cap)
{return cap<=KERNEL_CAP_LAST_CAP&&!!(cred->effective&(UINT64_C(1)<<cap));}
struct kernel_credentials *kernel_credentials_clone(struct kernel_heap *heap,const struct kernel_credentials *old)
{
    struct kernel_credentials *candidate=0;
    if(!heap||kernel_heap_allocate(heap,sizeof(*candidate),(void **)&candidate)!=KERNEL_HEAP_STATUS_OK)return 0;
    *candidate=*old;candidate->heap=heap;candidate->references=1;
    if(candidate->groups){uint32_t before=__atomic_fetch_add(&candidate->groups->references,1,__ATOMIC_RELAXED);if(!before||before==UINT32_MAX)__builtin_trap();}
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);kernel_uid_account_get_locked(candidate->account);}
    return candidate;
}
int kernel_credentials_set_account(struct kernel_credentials *cred,uint32_t uid)
{
    if(cred->account->uid==uid)return 0;
    struct kernel_uid_account *account=kernel_uid_account_get(cred->heap,uid);
    if(!account)return -KERNEL_ENOMEM;
    struct kernel_uid_account *old=cred->account;cred->account=account;
    kernel_uid_account_put(old);return 0;
}
void kernel_credentials_put(const struct kernel_credentials *cred)
{
    if(!cred||!cred->heap)return;
    uint32_t before=__atomic_fetch_sub((uint32_t *)&cred->references,1,__ATOMIC_ACQ_REL);
    if(!before)__builtin_trap();
    if(before!=1)return;
    kernel_credential_groups_put(cred->groups);
    kernel_uid_account_put(cred->account);
    kernel_heap_release(cred->heap,(void *)cred);
}
void kernel_credentials_publish(struct kernel_task *task,struct kernel_credentials *candidate)
{
    const struct kernel_credentials *old;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        old=kernel_credentials_task_locked(task);
        kernel_credentials_changed_locked(task,old,candidate);
        task->credentials=candidate;
    }
    kernel_credentials_put(old);
}

struct kernel_exec_security {
    struct kernel_heap *heap;
    uint32_t references, dumpable;
};
struct kernel_exec_security *kernel_exec_security_create(struct kernel_heap *heap,unsigned dumpable)
{
    struct kernel_exec_security *security=0;
    if(dumpable>1U)__builtin_trap();
    if(!heap || kernel_heap_allocate(heap,sizeof(*security),(void **)&security)!=KERNEL_HEAP_STATUS_OK)
        return 0;
    *security=(struct kernel_exec_security){.heap=heap,.references=1,.dumpable=dumpable};
    return security;
}
struct kernel_exec_security *kernel_exec_security_get(struct kernel_exec_security *security)
{
    if(security){
        uint32_t before=__atomic_fetch_add(&security->references,1,__ATOMIC_RELAXED);
        if(!before || before==UINT32_MAX)__builtin_trap();
    }
    return security;
}
void kernel_exec_security_put(struct kernel_exec_security *security)
{
    if(!security)return;
    uint32_t before=__atomic_fetch_sub(&security->references,1,__ATOMIC_ACQ_REL);
    if(!before)__builtin_trap();
    if(before==1)kernel_heap_release(security->heap,security);
}
unsigned kernel_exec_security_dumpable(const struct kernel_exec_security *security)
{return security?__atomic_load_n(&security->dumpable,__ATOMIC_ACQUIRE):0U;}
void kernel_exec_security_set_dumpable(struct kernel_exec_security *security,unsigned dumpable)
{
    if(!security || dumpable>1U)__builtin_trap();
    __atomic_store_n(&security->dumpable,dumpable,__ATOMIC_RELEASE);
}
