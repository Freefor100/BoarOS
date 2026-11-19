/* Production proc routing/control parser with modeled hstate provider. Pool
 * custody/reservation behavior is exercised separately by the hstate tests. */
#define main proc_other_main
#include "proc_sysvipc.c"
#undef main
static struct kernel_hstate_snapshot huge_model={.order=ARCH_MMU_HUGE_ORDER};
static unsigned pool_exists, acquired, creates;
static int pool_tag;
enum kernel_hugetlb_status kernel_hstate_get(struct physical_page_allocator *a,struct kernel_hstate **out)
{ (void)a;if(!pool_exists)return KERNEL_HUGETLB_NOT_FOUND;*out=(void *)&pool_tag;acquired++;return 0; }
enum kernel_hugetlb_status kernel_hstate_get_or_create(struct kernel_heap *h,struct physical_page_allocator *a,uint32_t order,struct kernel_hstate **out)
{ assert(h==&heap && order==ARCH_MMU_HUGE_ORDER);pool_exists=1;creates++;return kernel_hstate_get(a,out); }
void kernel_hstate_put(struct kernel_hstate *h)
{ assert(h==(void *)&pool_tag && acquired);acquired--; }
void kernel_hstate_snapshot(struct kernel_hstate *h,struct kernel_hstate_snapshot *out)
{ assert(h==(void *)&pool_tag && acquired);*out=huge_model; }
enum kernel_hugetlb_status kernel_hstate_resize(struct kernel_hstate *h,uint64_t count)
{ assert(h==(void *)&pool_tag && acquired);huge_model.total=huge_model.free=huge_model.persistent=count>4?4:count;return count>4?KERNEL_HUGETLB_NO_MEMORY:0; }
#ifndef PROC_FILTER_PROVIDER
int kernel_proc_coredump_filter(kernel_pid_t p,uint64_t i,int t,unsigned *v,int w)
{(void)p;(void)i;(void)t;(void)v;(void)w;abort();}
#endif
int kernel_proc_comm(kernel_pid_t p,uint64_t i,int t,char n[16],int w)
{(void)p;(void)i;(void)t;(void)n;(void)w;abort();}
struct kernel_namespace *kernel_task_uts_namespace(void){abort();}
int kernel_uts_name_write(struct kernel_namespace *ns,enum kernel_uts_name name,uint64_t off,const char *text,size_t len)
{(void)ns;(void)name;(void)off;(void)text;(void)len;abort();}
void kernel_uts_snapshot(struct kernel_namespace *ns,struct kernel_utsname *out)
{(void)ns;(void)out;abort();}
uint64_t kernel_sched_rr_slice_ns(void){abort();}
int kernel_sched_rr_slice_set_ms(int64_t value){(void)value;abort();}
int kernel_bpf_unprivileged_get(void){abort();}
int kernel_bpf_unprivileged_set(unsigned value){(void)value;abort();}
void kernel_scheduler_rt_bandwidth_get(int64_t *p,int64_t *r){(void)p;(void)r;abort();}
int kernel_scheduler_rt_bandwidth_set(int r,int64_t value){(void)r;(void)value;abort();}
int kernel_proc_fdinfo_access(kernel_pid_t p,uint64_t i){(void)p;(void)i;abort();}
int kernel_proc_same_group(kernel_pid_t p,uint64_t i,int t){(void)p;(void)i;(void)t;abort();}
int kernel_vfs_dac_permission(uint32_t m,uint32_t u,uint32_t g,unsigned a,const struct kernel_credentials *c)
{(void)m;(void)u;(void)g;(void)a;(void)c;abort();}
#ifndef PROC_HUGE_NO_MAIN
int main(void)
{
    uint64_t inode;uint32_t mode;
    assert(!proc_lookup(0,PROC_SYS_INODE,"vm",2,&inode,&mode) && inode==PROC_VM_INODE);
    assert(!proc_lookup(0,PROC_VM_INODE,"nr_hugepages",12,&inode,&mode) && inode==PROC_NR_HUGEPAGES_INODE && mode==(KERNEL_VFS_S_IFREG|0644));
    struct kernel_vfs_instance instance={.heap=&heap};
    struct kernel_vfs_node node={.inode=inode,.instance=&instance};
    struct kernel_credentials cred={.euid=123,.effective=UINT64_MAX};
    assert(proc_permission(&node,&cred,2)==-KERNEL_EACCES);
    assert(!proc_permission(&node,&cred,4));cred.euid=0;assert(!proc_permission(&node,&cred,6));
    uint64_t offset=0;size_t count=0;char data[24]={0};
    assert(!proc_control(&node,0,&offset,data,sizeof(data),&count) && count==2 && !memcmp(data,"0\n",2));
    assert(!creates && !acquired && !pool_exists);
    offset=0;memcpy(data,"9\n",2);
    assert(!proc_control(&node,1,&offset,data,2,&count) && count==2 && huge_model.total==4 && !acquired);
    offset=0;memset(data,0,sizeof(data));
    assert(!proc_control(&node,0,&offset,data,sizeof(data),&count) && count==2 && !memcmp(data,"4\n",2));
    offset=0;memcpy(data,"-1\n",3);assert(proc_control(&node,1,&offset,data,3,&count)==-KERNEL_EINVAL);
    offset=0;memcpy(data,"0x2\n",4);assert(!proc_control(&node,1,&offset,data,4,&count) && huge_model.total==2);
    offset=4;memcpy(data,"4\n",2);assert(!proc_control(&node,1,&offset,data,2,&count) && huge_model.total==2);
    assert(!acquired);
    puts("proc huge pool: path, EUID permissions, zero-without-create, actual partial growth and parse verified");
}

#endif
