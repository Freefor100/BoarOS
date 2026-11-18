/* 数字物理地址0的owner与空候选必须可区分；映射回独立宿主pool。 */
#include <kernel/mm_backend.h>
#include <kernel/heap.h>
#include <kernel/vma.h>
#include <kernel/hugetlb.h>
#include <kernel/uaccess.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void mm_test_attach(unsigned);
static unsigned char pool[32 * 1024 * 1024] __attribute__((aligned(4096)));
static struct physical_page_allocator pages;
static struct arch_mmu_page_table kernel;
static unsigned access_calls, fences;
void mm_test_fence(uint64_t,int);
static void *access(uint64_t physical)
{ access_calls++; return physical < sizeof(pool) ? pool + physical : 0; }
void riscv_smp_sync_user(uint32_t targets,uint64_t address,uint64_t size,int instructions)
{ (void)targets;(void)size;mm_test_fence(address,instructions); }
void riscv_smp_sync_instructions(uint32_t targets)
{ (void)targets;mm_test_fence(0,1); }
void riscv_smp_sync_kernel(uint64_t address,uint64_t size)
{ (void)size;mm_test_fence(address,0); }
void kernel_console_putc(char c) { fputc(c, stderr); }
void mm_test_unavailable(void) { abort(); }
void mm_test_fence(uint64_t address, int instructions)
{ (void)address; (void)instructions; fences++; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static uint64_t allocate(void)
{
    uint64_t physical;
    assert(!physical_page_allocate(&pages, &physical));
    memset(access(physical), 0, BOAROS_PAGE_SIZE);
    return physical;
}
#define HUGE (UINT64_C(1)<<21)
static int address_of(const void *p,uint64_t *out) { *out=(const unsigned char *)p-pool;return 1; }
static void put(struct kernel_mm *m,uint64_t a,unsigned char b)
{ size_t n=0;assert(!kernel_copy_to_user(m,a,&b,1,&n)&&n==1); }
static unsigned char get(struct kernel_mm *m,uint64_t a)
{ unsigned char b=0;size_t n=0;assert(!kernel_copy_from_user(m,&b,a,1,&n)&&n==1);return b; }

#include <kernel/shm.h>
#include <kernel/credentials.h>
#include <kernel/task.h>
#include <kernel/errno.h>
#define IPC_FIXTURE_SHM
#include "ipc_namespace_fixture.h"
uint64_t kernel_time_realtime_ns(void) { return 1000000000; }
enum kernel_task_status kernel_task_tgid(const struct kernel_task *task,kernel_pid_t *pid)
{ (void)task;*pid=123;return KERNEL_TASK_STATUS_OK; }
static unsigned fail_allocation;
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t bytes,void **out)
{
 if (fail_allocation && !--fail_allocation) return KERNEL_HEAP_STATUS_EMPTY;
 return __real_kernel_heap_allocate_zeroed(h,n,bytes,out);
}
static int ctl(int id,int cmd,void *buf)
{ int64_t result;return kernel_shm_ctl_mm(0,id,cmd,(uintptr_t)buf,&result); }
static int create(uint64_t size,int flags)
{ int32_t id=-1;assert(!kernel_shm_get(0,0,size,0600|KERNEL_SHM_HUGETLB|flags,&id));return id; }
static void stats(struct kernel_hstate *h,unsigned free,unsigned reserved,unsigned maps)
{ struct kernel_hstate_snapshot s;kernel_hstate_snapshot(h,&s);assert(s.free==free&&s.reserved==reserved&&s.maps==maps); }
static void namespaces_and_quota(struct kernel_heap *heap,struct kernel_hstate *h,struct kernel_mm *mm)
{
 struct kernel_namespace *left=0,*right=0;
 assert(!kernel_ipc_namespace_create(heap,&left)&&!kernel_ipc_namespace_create(heap,&right));
 host_ipc_namespace=left;int32_t id,other,rejected=-1;uint64_t a;
 assert(!kernel_shm_get(0,321,KERNEL_SHMMAX,KERNEL_IPC_CREAT|KERNEL_SHM_HUGETLB|KERNEL_SHM_NORESERVE|0600,&id));
 assert(kernel_shm_get(0,0,1,0600,&rejected)==-KERNEL_ENOSPC);
 assert(!kernel_shm_at_mm(mm,NULL,id,0,0,&a));put(mm,a,61);
 assert(!ctl(id,KERNEL_IPC_RMID,0));
 assert(kernel_shm_get(0,0,1,0600,&rejected)==-KERNEL_ENOSPC);
 host_ipc_namespace=right;
 assert(!kernel_shm_get(0,321,1,KERNEL_IPC_CREAT|KERNEL_SHM_HUGETLB|0600,&other)&&other==id);
 kernel_namespace_put(left); /* Old attachment owns the old namespace through fork. */
 struct kernel_mm child={0};assert(!kernel_mm_fork(&child,mm));
 assert(!kernel_shm_dt_mm(mm,a));assert(get(&child,a)==61);
 assert(!kernel_mm_release(&child));stats(h,3,1,1);
 assert(!ctl(other,KERNEL_IPC_RMID,0));
 int large=create(KERNEL_SHMMAX,KERNEL_SHM_NORESERVE);
 assert(!ctl(large,KERNEL_IPC_RMID,0));
 int small=create(1,0);assert(!ctl(small,KERNEL_IPC_RMID,0));
 host_ipc_namespace=0;kernel_namespace_put(right);stats(h,3,0,0);
}
int main(void)
{
 mm_test_attach(0);
 struct boot_memory_layout layout={0};layout.usable_count=1;layout.usable[0].size=sizeof(pool);
 assert(!physical_page_allocator_init(&pages,&layout));assert(!physical_page_allocator_bind_access(&pages,access));assert(!physical_page_allocator_finalize(&pages));
 kernel=(struct arch_mmu_page_table){.allocator=&pages,.root_address=allocate(),.table_pages=1,.state=ARCH_MMU_STATE_ACTIVE};
 struct kernel_heap heap;assert(!kernel_heap_init(&heap,&pages,address_of));
 struct arch_mmu_user_space space={0};struct kernel_mm parent={0},child={0};
 assert(!arch_mmu_user_space_init(&space,&pages,&kernel));assert(!kernel_mm_create(&parent,&space));assert(!kernel_mm_vma_enable(&parent,&heap));
 assert(!kernel_mm_brk_initialize(&parent,0x10000,0x20000));
 assert(!kernel_mm_mmap_base_initialize(&parent,4096*HUGE));
 assert(!kernel_shm_init(&heap,&pages));
 struct kernel_hstate *h;assert(!kernel_hstate_get_or_create(&heap,&pages,9,&h));assert(!kernel_hstate_resize(h,3));
 int32_t id=-1;
 assert(kernel_shm_get(0,0,1,KERNEL_SHM_HUGETLB|(30U<<26)|0600,&id)==-KERNEL_EINVAL&&id==-1);
 assert(kernel_shm_get(0,0,4*HUGE,KERNEL_SHM_HUGETLB|0600,&id)==-KERNEL_ENOMEM&&id==-1);stats(h,3,0,0);
 struct kernel_credentials *user=kernel_credentials_clone(&heap,kernel_credentials_initial());assert(user);
 user->uid=user->euid=user->gid=user->egid=user->fsgid=1000;user->effective=user->permitted=0;
 const struct kernel_credentials *old=kernel_credentials_override(user);
 assert(kernel_shm_get(0,0,1,KERNEL_SHM_HUGETLB|KERNEL_SHM_NORESERVE|0600,&id)==-KERNEL_EPERM);
 user->effective=UINT64_C(1)<<KERNEL_CAP_IPC_LOCK;
 id=create(1,21U<<26);stats(h,3,1,1);
 kernel_credentials_override(old);kernel_credentials_put(user);
 struct kernel_shmid64_ds ds;assert(!ctl(id,KERNEL_IPC_STAT,&ds)&&ds.shm_segsz==1&&!ds.shm_nattch);
 assert(!ctl(id,KERNEL_SHM_LOCK,0));assert(!ctl(id,KERNEL_IPC_STAT,&ds)&&!(ds.shm_perm.mode&KERNEL_SHM_LOCKED));
 uint64_t a,b;
 fail_allocation=1;int32_t rejected=-1;
 assert(kernel_shm_get(0,0,1,KERNEL_SHM_HUGETLB|0600,&rejected)==-KERNEL_ENOMEM&&!fail_allocation&&rejected==-1);stats(h,3,1,1);
 for(unsigned failure=1;failure<=2;failure++) {
   fail_allocation=failure;
   assert(kernel_shm_at_mm(&parent,NULL,id,HUGE,0,&a)==-KERNEL_ENOMEM&&!fail_allocation);
   assert(!ctl(id,KERNEL_IPC_STAT,&ds)&&!ds.shm_nattch);stats(h,3,1,1);
 }

 assert(kernel_shm_at_mm(&parent,NULL,id,HUGE+4096,0,&a)==-KERNEL_EINVAL);
 assert(!kernel_shm_at_mm(&parent,NULL,id,HUGE+1,KERNEL_SHM_RND,&a)&&a==HUGE);stats(h,3,1,1);
 assert(!kernel_shm_at_mm(&parent,NULL,id,3*HUGE,0,&b));stats(h,3,1,1);
 assert(!kernel_shm_at_mm(&parent,NULL,id,3*HUGE,KERNEL_SHM_REMAP,&b));
 assert(!ctl(id,KERNEL_IPC_STAT,&ds)&&ds.shm_nattch==2);stats(h,3,1,1);
 put(&parent,a+12345,42);assert(get(&parent,b+12345)==42);stats(h,2,0,1);
 struct kernel_mm_user_pin pin={0};assert(!kernel_mm_pin_user_page(&parent,a+12345,KERNEL_MM_READ,&pin));
 assert(pin.order==9);kernel_mm_unpin_user_page(&pin);
 struct kernel_shm_info info;assert(!ctl(0,KERNEL_SHM_INFO,&info)&&info.used_ids==1&&info.shm_tot==1&&info.shm_rss==512);
 assert(!kernel_mm_fork(&child,&parent));put(&child,b+12345,99);assert(get(&parent,a+12345)==99);
 assert(!ctl(id,KERNEL_IPC_STAT,&ds)&&ds.shm_nattch==4);
 assert(!ctl(id,KERNEL_IPC_RMID,0));assert(!kernel_shm_at_mm(&parent,NULL,id,5*HUGE,KERNEL_SHM_RDONLY,&b));
 assert(kernel_mm_mprotect(&parent,b,HUGE,KERNEL_MM_READ|KERNEL_MM_WRITE)==KERNEL_MM_STATUS_ACCESS);
 assert(!kernel_shm_dt_mm(&parent,a));assert(!kernel_shm_dt_mm(&parent,3*HUGE));assert(!kernel_shm_dt_mm(&parent,b));stats(h,2,0,1);
 assert(!kernel_mm_release(&child));child=(struct kernel_mm){0};stats(h,3,0,0);
 assert(ctl(id,KERNEL_IPC_STAT,&ds)==-KERNEL_EINVAL);
 /* Creation reserves; unattached RMID and namespace destruction release it. */
 id=create(HUGE+1,0);stats(h,3,2,1);assert(!ctl(id,KERNEL_IPC_RMID,0));stats(h,3,0,0);
 id=create(4*HUGE,KERNEL_SHM_NORESERVE);assert(!kernel_shm_at_mm(&parent,NULL,id,0,0,&a));stats(h,3,0,1);
 for(unsigned i=0;i<3;i++)put(&parent,a+i*HUGE,10+i);
 assert(kernel_mm_resolve_user_fault(&parent,a+3*HUGE,KERNEL_MM_WRITE)==KERNEL_MM_STATUS_BUS_FAULT);
 assert(!ctl(id,KERNEL_IPC_RMID,0));assert(!kernel_shm_dt_mm(&parent,a));stats(h,3,0,0);
 namespaces_and_quota(&heap,h,&parent);
 assert(!kernel_mm_release(&parent));assert(!kernel_ipc_initial_shutdown());kernel_hstate_put(h);assert(!kernel_hstate_shutdown(&pages));
 struct kernel_heap_statistics final;kernel_heap_get_statistics(&heap,&final);assert(!final.live_allocations&&!pages.shared_anon_pages);
 puts("SHM_HUGETLB reservation, native leaf, aliases, fork, RMID and NORESERVE passed");
 return 0;
}
