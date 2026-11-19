/* Filesystem semantics and lifetime, using the production allocator/locks.
 * Scheduler membership is a deterministic fixture; guest tests cover clone. */
#include "../../fs/vfs_objects.h"
#include "../../fs/open_file_internal.h"
#include "../../fs/cgroup_internal.h"
#include <kernel/errno.h>
#include <kernel/wait_internal.h>
#include <kernel/credentials.h>
#include <string.h>
#include "pseudo_fs_fixture.h"
static int32_t moved;
static int moved_thread;
static struct kernel_cgroup *moved_group;
int kernel_cgroup_move(struct kernel_cgroup *g,int32_t tid,int thread,const struct kernel_credentials *c)
{assert(c);moved=tid;moved_thread=thread;moved_group=g;return 0;}
int kernel_cgroup_members_snapshot(struct kernel_cgroup *g,int threads,struct kernel_heap *h,char **data,size_t *size)
{(void)g;(void)threads;*size=5;if(kernel_heap_allocate(h,*size,(void **)data))return -KERNEL_ENOMEM;memcpy(*data,"4\n87\n",5);return 0;}
int kernel_cgroup_populated(struct kernel_cgroup *g){return !!g->members;}
const struct kernel_vfs_file *kernel_open_file_vfs_borrow(const struct kernel_open_file_description *f)
{return f?&f->file:NULL;}
struct kernel_vfs_path *kernel_open_file_path(const struct kernel_open_file_description *f)
{return f?f->file.path:NULL;}
static void text(struct kernel_vfs_mount *m,uint64_t dir,const char *name,const char *expected)
{
    struct kernel_vfs_file f={0};open_id(m,find(m,dir,name,NULL),&f);
    struct kernel_vfs_node *node=f.private_data;
    assert(node->max_size==UINT64_MAX);
    if(!(node->generated_control&KERNEL_VFS_CONTROL_READ)){
        char *data=NULL;size_t size=0;assert(!ops(m)->snapshot(node,&heap,NULL,NULL,&data,&size));
        assert(size==strlen(expected)&&!memcmp(data,expected,size));
        kernel_heap_release(&heap,data);close_file(&f);return;
    }
    uint64_t offset=0;char result[80]={0};size_t count=0,total=0;
    do {assert(!ops(m)->control(f.private_data,0,&offset,result+total,1,&count));total+=count;}while(count);
    assert(total==strlen(expected)&&!memcmp(result,expected,total));close_file(&f);
}
static int write_file(struct kernel_vfs_file *f,const char *s)
{size_t count=0;uint64_t offset=0;int r=ops(f->mount)->control(f->private_data,1,&offset,(char *)s,strlen(s),&count);
    if(!r)assert(count==strlen(s)&&offset==count);
    return r;}
static void cases(void)
{
    struct kernel_vfs_mount *m=NULL,*alias=NULL;
    assert(!kernel_cgroup_mount(&heap,0,&m));assert(!kernel_cgroup_mount(&heap,0,&alias));
    uint64_t root;uint32_t mode;assert(!ops(m)->root(instance(m),&root,&mode));
    absent(m,root,"cgroup.type");
    assert(space(m).type==0x63677270 && space(m).fsid==space(alias).fsid);
    assert(!ops(m)->mkdir(m,root,"one",0755,0));uint64_t dir=find(m,root,"one",NULL);
    assert(find(alias,root,"one",NULL)==dir);
    fail_allocation=1;assert(ops(m)->mkdir(m,root,"failed",0755,0)==-KERNEL_ENOMEM);
    fail_allocation=0;absent(alias,root,"failed");
    struct kernel_vfs_file d={0},procs={0},other={0};open_id(m,dir,&d);
    uint64_t position=0;
    assert(expect_entry(&d,&position,".",dir)==4);
    assert(expect_entry(&d,&position,"..",root)==4);
    const char *controls[]={"cgroup.procs","cgroup.threads","cgroup.controllers",
        "cgroup.subtree_control","cgroup.type","cgroup.events"};
    for(unsigned j=0;j<6;j++)assert(expect_entry(&d,&position,controls[j],find(m,dir,controls[j],NULL))==8);
    expect_end(&d,position);
    open_id(m,find(m,dir,"cgroup.procs",NULL),&procs);
    open_id(alias,find(alias,dir,"cgroup.procs",NULL),&other);
    text(m,dir,"cgroup.controllers","");text(m,dir,"cgroup.subtree_control","");
    text(m,dir,"cgroup.type","domain\n");text(m,dir,"cgroup.procs","4\n87\n");
    text(m,dir,"cgroup.events","populated 0\nfrozen 0\n");
    assert(!write_file(&procs," +42\n")&&moved==42&&!moved_thread);
    assert(write_file(&procs,"1 2")==-KERNEL_EINVAL);assert(write_file(&procs,"2147483648")==-KERNEL_EINVAL);
    assert(write_file(&procs,"-1")==-KERNEL_EINVAL);
    assert(write_file(&procs,"08")==-KERNEL_EINVAL);
    assert(!write_file(&procs,"010")&&moved==8);
    assert(!write_file(&procs,"0x2a")&&moved==42);
    assert(!write_file(&procs,"0X2A")&&moved==42);
    assert(!write_file(&procs,"+0")&&moved==0);
    assert(!write_file(&procs,"-0")&&moved==0);
    assert(!write_file(&procs,"2147483647")&&moved==INT32_MAX);
    assert(write_file(&procs,"0x80000000")==-KERNEL_EINVAL);
    assert(write_file(&procs,"020000000000")==-KERNEL_EINVAL);
    assert(write_file(&procs,"-2147483648")==-KERNEL_EINVAL);
    assert(write_file(&procs,"0x")==-KERNEL_EINVAL);
    assert(write_file(&procs,"999999999999999999999999999")==-KERNEL_EINVAL);
    struct kernel_vfs_file c={0};open_id(m,find(m,dir,"cgroup.subtree_control",NULL),&c);
    assert(write_file(&c,"+cpu")==-KERNEL_EINVAL);
    assert(write_file(&c,"+nosuch")==-KERNEL_EINVAL);assert(write_file(&c,"cpu")==-KERNEL_EINVAL);
    assert(!write_file(&c," \n"));close_file(&c);
    open_id(m,find(m,dir,"cgroup.type",NULL),&c);assert(write_file(&c,"threaded")==-KERNEL_EOPNOTSUPP);close_file(&c);
    struct kernel_cgroup *g=NULL;struct kernel_vfs_path path={.file=d};
    struct kernel_open_file_description ofd={.file={.path=&path,.mount=m,.mode=KERNEL_VFS_S_IFDIR}};
    assert(!kernel_cgroup_from_file(&ofd,&g));assert(g==moved_group);
    struct kernel_open_file_description bad={.file=procs};assert(kernel_cgroup_from_file(&bad,&moved_group)==-KERNEL_EBADF);
    assert(!ops(m)->set_owner(&procs,123,456));assert(!ops(m)->set_mode(&procs,0600));
    struct kernel_vfs_node *n=other.private_data;assert(n->uid==123 && n->gid==456 && (n->mode&0777)==0600);
    struct kernel_credentials delegate={.fsuid=123,.fsgid=456};
    assert(!ops(alias)->permission(n,&delegate,2));
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(!kernel_cgroup_permission(g,g,&delegate));
        assert(kernel_cgroup_permission(NULL,g,&delegate)==-KERNEL_EACCES);
        g->members++;}
    assert(ops(alias)->rmdir(alias,root,"one")==-KERNEL_EBUSY);
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);g->members--;}
    assert(!ops(m)->mkdir(m,dir,"child",0755,0));assert(ops(m)->rmdir(m,root,"one")==-KERNEL_EBUSY);
    assert(!ops(alias)->rmdir(alias,dir,"child"));
    assert(!ops(alias)->rmdir(alias,root,"one"));absent(m,root,"one");
    struct kernel_cgroup *gone=NULL;assert(kernel_cgroup_from_file(&ofd,&gone)==-KERNEL_ENOENT);
    assert(write_file(&procs,"0")==-KERNEL_ENODEV);
    struct kernel_vfs_stat st;assert(!ops(m)->stat(&d,&st)&&!st.nlink);
    close_file(&other);close_file(&procs);close_file(&d);kernel_cgroup_put(g);
    assert(!ops(m)->mkdir(m,root,"remain",0755,0));
    uint64_t remain=find(m,root,"remain",NULL);
    assert(!ops(m)->mkdir(m,remain,"nested",0755,0));
    assert(kernel_cgroup_shutdown(&heap)==-KERNEL_EBUSY);
    open_id(m,remain,&d);
    struct kernel_vfs_path remaining_path={.file=d};ofd.file.path=&remaining_path;
    struct kernel_cgroup *held=NULL;assert(!kernel_cgroup_from_file(&ofd,&held));close_file(&d);
    assert(!ops(m)->unmount(m));assert(!ops(alias)->unmount(alias));
    assert(kernel_cgroup_shutdown(&heap)==-KERNEL_EBUSY);
    kernel_cgroup_put(held);
    assert(!kernel_cgroup_shutdown(&heap));
    assert(!kernel_cgroup_shutdown(&heap));
}
int main(void)
{
    mm_test_attach(0);kernel_cpu_current()->state=KERNEL_CPU_ONLINE;kernel_wait_domain_init();
    struct boot_memory_layout layout={0};layout.usable_count=1;layout.usable[0].base=(uintptr_t)pool;layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout));assert(!physical_page_allocator_bind_access(&pages,access_page));
    assert(!physical_page_allocator_finalize(&pages)&&!kernel_heap_init(&heap,&pages,page_address));
    uint64_t baseline=physical_page_available(&pages);cases();
    struct kernel_heap_statistics s;kernel_heap_get_statistics(&heap,&s);assert(!s.live_allocations&&!s.current_pages);
    assert(physical_page_available(&pages)==baseline&&!physical_page_allocator_audit(&pages));
    puts("PASS cgroup2 shared hierarchy, partial reads, control parsing, delegation, deleted FD and final owners");
}
