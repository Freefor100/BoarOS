/* Real SHM storage and UID accounts; only task identity and user copies are modeled. */
#include <kernel/shm.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/memory_object.h>
#include <kernel/page.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <arch/context.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void mm_test_attach(unsigned);
static unsigned char pool[16*1024*1024] __attribute__((aligned(16384)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static uint64_t memlock_limit=UINT64_MAX;
static unsigned fail_allocation;
static int copy_remove_id=-1;
static int copy_short;
static struct kernel_memory_object *created_object;
enum kernel_memory_object_status __real_kernel_memory_object_create(struct kernel_heap *,struct physical_page_allocator *,struct kernel_memory_object **);
enum kernel_memory_object_status __wrap_kernel_memory_object_create(struct kernel_heap *h,struct physical_page_allocator *p,struct kernel_memory_object **out)
{
    enum kernel_memory_object_status status=__real_kernel_memory_object_create(h,p,out);
    if(!status)created_object=*out;
    return status;
}
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{
    if(fail_allocation && !--fail_allocation)return KERNEL_HEAP_STATUS_EMPTY;
    return __real_kernel_heap_allocate_zeroed(h,n,size,out);
}
uint64_t kernel_task_current_memlock_limit(void) { return kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_IPC_LOCK)?UINT64_MAX:memlock_limit; }
void kernel_console_putc(char c) { fputc(c,stderr); }
void mm_test_unavailable(void) { abort(); }
void mm_test_fence(uint64_t v,int instructions) { (void)v;(void)instructions; }
uint64_t kernel_time_realtime_ns(void) { return 1000000000; }
enum kernel_task_status kernel_task_tgid(const struct kernel_task *task,kernel_pid_t *pid)
{ (void)task;*pid=123;return KERNEL_TASK_STATUS_OK; }
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *mm,uint64_t to,const void *from,size_t len,size_t *done)
{
    (void)mm;assert(arch_interrupt_is_enabled());
    if(!to){*done=0;return KERNEL_UACCESS_STATUS_FAULT;}
    if(copy_remove_id>=0) {
        int64_t result;int id=copy_remove_id;copy_remove_id=-1;
        assert(!kernel_shm_ctl_mm(0,id,KERNEL_IPC_RMID,0,&result));
    }
    memcpy((void *)(uintptr_t)to,from,len);*done=len-(copy_short!=0);
    return KERNEL_UACCESS_STATUS_OK;
}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *mm,void *to,uint64_t from,size_t len,size_t *done)
{ (void)mm;if(!from){*done=0;return KERNEL_UACCESS_STATUS_FAULT;}memcpy(to,(void *)(uintptr_t)from,len);*done=len;return KERNEL_UACCESS_STATUS_OK; }
static void *access_page(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p,uint64_t *out) { *out=(uintptr_t)p;return 1; }
static int ctl(int id,int cmd,struct kernel_shmid64_ds *ds)
{ int64_t result=-1;int status=kernel_shm_ctl_mm(0,id,cmd,(uintptr_t)ds,&result);if(!status)assert(!result);return status; }
static int segment(uint64_t bytes)
{ int32_t id;assert(!kernel_shm_get(0,0,bytes,0600,&id));return id; }
static struct kernel_credentials *user(uint32_t uid,uint32_t euid)
{
    struct kernel_credentials *c=kernel_credentials_clone(&heap,kernel_credentials_initial());assert(c);
    assert(!kernel_credentials_set_account(c,uid));c->uid=uid;c->euid=euid;c->effective=c->permitted=0;return c;
}
static void check_locked(int id,int expected)
{ struct kernel_shmid64_ds ds;assert(!ctl(id,KERNEL_IPC_STAT,&ds));assert(!!(ds.shm_perm.mode&KERNEL_SHM_LOCKED)==expected); }
static void lifecycle(void)
{
    struct kernel_credentials *a=user(1000,1001),*b=user(1000,1001),*other=user(1002,1003);
    assert(a->account==b->account);const struct kernel_credentials *old=kernel_credentials_override(a);
    for(unsigned failure=1;failure<=2;failure++) {
        struct kernel_heap_statistics before,after;kernel_heap_get_statistics(&heap,&before);
        int32_t rejected=-1;fail_allocation=failure;
        assert(kernel_shm_get(0,0,BOAROS_PAGE_SIZE,0600,&rejected)==-KERNEL_ENOMEM&&rejected==-1);
        assert(!fail_allocation&&!a->account->shm_locked_pages);
        kernel_heap_get_statistics(&heap,&after);assert(before.live_allocations==after.live_allocations);
    }
    memlock_limit=2*BOAROS_PAGE_SIZE;int id=segment(BOAROS_PAGE_SIZE+1),next=segment(BOAROS_PAGE_SIZE);
    struct kernel_heap_statistics before,after;kernel_heap_get_statistics(&heap,&before);
    assert(!ctl(id,KERNEL_SHM_LOCK,0)&&a->account->shm_locked_pages==2);check_locked(id,1);
    kernel_heap_get_statistics(&heap,&after);assert(before.live_allocations==after.live_allocations&&!pages.shared_anon_pages);
    assert(!ctl(id,KERNEL_SHM_LOCK,0)&&a->account->shm_locked_pages==2);
    memlock_limit=BOAROS_PAGE_SIZE;assert(!ctl(id,KERNEL_SHM_LOCK,0));
    memlock_limit=0;assert(ctl(id,KERNEL_SHM_LOCK,0)==-KERNEL_EPERM);check_locked(id,1);
    memlock_limit=2*BOAROS_PAGE_SIZE;kernel_credentials_override(b);
    assert(ctl(next,KERNEL_SHM_LOCK,0)==-KERNEL_ENOMEM);check_locked(next,0);
    memlock_limit=3*BOAROS_PAGE_SIZE;assert(!ctl(next,KERNEL_SHM_LOCK,0)&&a->account->shm_locked_pages==3);
    assert(!ctl(next,KERNEL_SHM_UNLOCK,0)&&!ctl(next,KERNEL_SHM_UNLOCK,0)&&a->account->shm_locked_pages==2);
    kernel_credentials_override(other);other->effective=UINT64_C(1)<<KERNEL_CAP_SYS_ADMIN;
    assert(ctl(id,KERNEL_SHM_LOCK,0)==-KERNEL_EPERM&&ctl(id,KERNEL_SHM_UNLOCK,0)==-KERNEL_EPERM);
    other->effective=UINT64_C(1)<<KERNEL_CAP_IPC_LOCK;memlock_limit=0;
    assert(!ctl(id,KERNEL_SHM_LOCK,0)&&!other->account->shm_locked_pages);
    assert(!ctl(id,KERNEL_SHM_UNLOCK,0)&&!a->account->shm_locked_pages);
    assert(!ctl(id,KERNEL_SHM_LOCK,0)&&other->account->shm_locked_pages==2);
    kernel_credentials_override(a);struct kernel_shmid64_ds ds;assert(!ctl(id,KERNEL_IPC_STAT,&ds));ds.shm_perm.uid=1009;ds.shm_perm.mode=0640;
    assert(!ctl(id,KERNEL_IPC_SET,&ds));check_locked(id,1);
    assert(!kernel_credentials_set_account(other,1010));other->uid=1010;
    struct kernel_uid_account *previous=kernel_uid_account_get(&heap,1002);assert(previous&&previous->shm_locked_pages==2);
    assert(!ctl(id,KERNEL_SHM_UNLOCK,0)&&!previous->shm_locked_pages&&!other->account->shm_locked_pages);
    kernel_uid_account_put(previous);
    memlock_limit=BOAROS_PAGE_SIZE;assert(ctl(id,KERNEL_SHM_LOCK,0)==-KERNEL_ENOMEM);check_locked(id,0);
    assert(!ctl(next,KERNEL_SHM_LOCK,0)&&a->account->shm_locked_pages==1);
    assert(!ctl(next,KERNEL_IPC_RMID,0)&&!a->account->shm_locked_pages);
    assert(!ctl(id,KERNEL_IPC_RMID,0));kernel_credentials_override(old);
    kernel_credentials_put(a);kernel_credentials_put(b);kernel_credentials_put(other);
}

/* Independent LP64 Linux UAPI receiver catches both padding and field offsets. */
struct shm_info_abi {
    int32_t used_ids;
    uint32_t padding;
    uint64_t shm_tot,shm_rss,shm_swp,swap_attempts,swap_successes;
};
static unsigned query_checks,query_failures;
#define QUERY_CHECK(x) do {query_checks++;if(!(x)){fprintf(stderr,"SHM_QUERY FAIL line=%u %s\n",__LINE__,#x);query_failures++;}}while(0)
static int query(int id,int cmd,void *out,int64_t *result)
{ return kernel_shm_ctl_mm((struct kernel_mm *)(uintptr_t)1,id,cmd,(uintptr_t)out,result); }
static void information(unsigned used,uint64_t total,uint64_t resident,int highest)
{
    struct shm_info_abi info;memset(&info,0xa5,sizeof(info));int64_t result=-1;
    int status=query(0,KERNEL_SHM_INFO,&info,&result);
    QUERY_CHECK(!status&&result==highest);
    if(!status) {
        QUERY_CHECK(info.used_ids==(int)used&&info.shm_tot==total&&info.shm_rss==resident);
        QUERY_CHECK(!info.padding&&!info.shm_swp&&!info.swap_attempts&&!info.swap_successes);
    }
    struct kernel_shminfo64 limits;memset(&limits,0xa5,sizeof(limits));result=-1;
    status=query(0,KERNEL_IPC_INFO,&limits,&result);
    QUERY_CHECK(!status&&result==highest);
    if(!status)QUERY_CHECK(limits.shmmin==1&&limits.shmmni==128&&limits.shmseg==128&&
        limits.shmmax==KERNEL_SHMMAX&&limits.shmall==KERNEL_SHMALL&&
        !limits.__unused1&&!limits.__unused2&&!limits.__unused3&&!limits.__unused4);
}
static void queries(void)
{
    information(0,0,0,0);
    int ids[3];struct kernel_memory_object *objects[3];
    const uint64_t sizes[]={BOAROS_PAGE_SIZE+1,1,2*BOAROS_PAGE_SIZE+1};
    for(unsigned i=0;i<3;i++){ids[i]=segment(sizes[i]);objects[i]=created_object;}
    information(3,6,0,2);
    assert(!ctl(ids[0],KERNEL_SHM_LOCK,0));information(3,6,0,2);
    for(unsigned i=0;i<3;i++) {
        uint64_t physical;int created;
        assert(!kernel_memory_object_get_page(objects[i?2:0],i?0:1,&physical,&created));
        assert(!physical_page_release(&pages,physical));
    }
    information(3,6,2,2);
    struct kernel_shmid64_ds expected,actual;int64_t result;
    assert(!ctl(ids[0],KERNEL_IPC_STAT,&expected));
    result=-1;QUERY_CHECK(!query(0,KERNEL_SHM_STAT,&actual,&result)&&result==ids[0]);
    if(result==ids[0])QUERY_CHECK(!memcmp(&expected,&actual,sizeof(actual)));
    result=-1;QUERY_CHECK(!query(0,15,&actual,&result)&&result==ids[0]);
    if(result==ids[0])QUERY_CHECK(!memcmp(&expected,&actual,sizeof(actual)));
    /* Slot selectors ignore a generation; IPC_STAT rejects the stale full ID. */
    int stale=ids[1];assert(!ctl(stale,KERNEL_IPC_RMID,0));
    result=-1;QUERY_CHECK(query(1,15,&actual,&result)==-KERNEL_EINVAL&&result==-1);
    ids[1]=segment(1);QUERY_CHECK(ids[1]!=stale);
    result=-1;QUERY_CHECK(!query(stale,15,&actual,&result)&&result==ids[1]);
    QUERY_CHECK(query(stale,KERNEL_IPC_STAT,&actual,&result)==-KERNEL_EINVAL);
    QUERY_CHECK(query(0,KERNEL_IPC_STAT,&actual,&result)==-KERNEL_EINVAL);
    assert(!ctl(ids[2],KERNEL_IPC_RMID,0));information(2,3,1,1);
    struct kernel_credentials *other=user(2900,2900);
    const struct kernel_credentials *old=kernel_credentials_override(other);
    result=-1;QUERY_CHECK(query(0,KERNEL_SHM_STAT,&actual,&result)==-KERNEL_EACCES&&result==-1);
    QUERY_CHECK(query(0,KERNEL_SHM_STAT,0,&result)==-KERNEL_EACCES);
    QUERY_CHECK(query(ids[0],KERNEL_IPC_STAT,0,&result)==-KERNEL_EACCES);
    QUERY_CHECK(!query(0,15,&actual,&result)&&result==ids[0]&&(actual.shm_perm.mode&KERNEL_SHM_LOCKED));
    information(2,3,1,1);
    other->effective=UINT64_C(1)<<KERNEL_CAP_IPC_OWNER;
    QUERY_CHECK(!query(0,KERNEL_SHM_STAT,&actual,&result)&&result==ids[0]);
    kernel_credentials_override(old);kernel_credentials_put(other);
    const int commands[]={KERNEL_IPC_INFO,KERNEL_SHM_INFO,KERNEL_SHM_STAT,15,KERNEL_IPC_STAT};
    for(unsigned i=0;i<sizeof(commands)/sizeof(commands[0]);i++) {
        int id=commands[i]==KERNEL_IPC_STAT?ids[0]:0;result=-1;
        QUERY_CHECK(query(id,commands[i],0,&result)==-KERNEL_EFAULT&&result==-1);
        QUERY_CHECK(query(-1,commands[i],0,&result)==-KERNEL_EINVAL&&result==-1);
    }
    result=-1;QUERY_CHECK(query(2,15,0,&result)==-KERNEL_EINVAL&&result==-1);
    QUERY_CHECK(query(0,-1,0,&result)==-KERNEL_EINVAL);
    assert(!ctl(ids[0],KERNEL_IPC_RMID,0));information(1,1,0,1);
    copy_short=1;result=-1;
    QUERY_CHECK(query(1,15,&actual,&result)==-KERNEL_EFAULT&&result==-1);
    copy_short=0;copy_remove_id=ids[1];result=-1;
    QUERY_CHECK(!query(1,15,&actual,&result)&&result==ids[1]&&actual.shm_segsz==1);
    QUERY_CHECK(copy_remove_id==-1);
    information(0,0,0,0);
    printf("SHM_QUERY page_shift=%u checks=%u failures=%u\n",BOAROS_PAGE_SHIFT,query_checks,query_failures);
}

int main(void)
{
    mm_test_attach(0);
    struct boot_memory_layout layout={0};layout.usable_count=1;layout.usable[0].base=(uintptr_t)pool;layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout)&&!physical_page_allocator_bind_access(&pages,access_page)&&!physical_page_allocator_finalize(&pages));
    assert(!kernel_heap_init(&heap,&pages,address_of)&&!kernel_shm_init(&heap,&pages));
    int id=segment(BOAROS_PAGE_SIZE+1);
    int status=ctl(id,KERNEL_SHM_LOCK,0);
    if(status){fprintf(stderr,"SHM_LOCK expected=0 actual=%d\n",status);return 1;}
    assert(!ctl(id,KERNEL_SHM_UNLOCK,0)&&!ctl(id,KERNEL_IPC_RMID,0));
    queries();
    lifecycle();
    struct kernel_heap_statistics stats;kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations&&!stats.current_pages&&!pages.shared_anon_pages&&!physical_page_allocator_audit(&pages));
    printf("SHM_LOCK page_shift=%u permissions/repeats/UID-quota/chown/account-owners/lazy-storage/OOM PASS\n",BOAROS_PAGE_SHIFT);
    return query_failures?1:0;
}
