/* Reuse the existing tmpfs allocator/node-publication fixture. The backends,
 * permission metadata, immutable read data and object owners remain real. */
#include "../../fs/vfs_objects.h"
#include <kernel/errno.h>
#include <string.h>
#include "pseudo_fs_fixture.h"
#include "../../kernel/bpf/private.h"
#include <kernel/trace_events.h>
int kernel_bpf_initialize(struct kernel_heap *h){assert(h==&heap);return 0;}
static unsigned fail_physical;
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *,uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *a,uint64_t *out)
{
    if(fail_physical && !--fail_physical)return PHYSICAL_PAGE_STATUS_EMPTY;
    return __real_physical_page_allocate(a,out);
}
static unsigned char kernel_btf_bytes[]={0x9f,0xeb,1,0,24,0,0,0};
const void *kernel_bpf_kernel_btf(uint32_t *size){*size=sizeof(kernel_btf_bytes);return kernel_btf_bytes;}
void bpf_object_get(struct bpf_object *o){assert(o->references);o->references++;}
void bpf_object_put(struct bpf_object *o){assert(o->references);o->references--;}
struct kernel_cpu *riscv_smp_cpu(unsigned logical){return logical?0:kernel_cpu_current();}
int kernel_bpffs_pin_inode(struct kernel_vfs_instance *,uint64_t,const char *,struct bpf_object *,uint32_t);
static void read_text(struct kernel_vfs_mount *m,uint64_t parent,const char *name,const char *expected)
{
    struct kernel_vfs_file f={0};open_id(m,find(m,parent,name,0),&f);
    char data[2048]={0};size_t n=0;
    assert(!ops(m)->pread(f.private_data,0,data,sizeof(data)-1,&n));
    assert(n==strlen(expected)&&!memcmp(data,expected,n));
    size_t tail=0;assert(!ops(m)->pread(f.private_data,n,data,sizeof(data),&tail)&&!tail);
    close_file(&f);
}
static void pins(void)
{
    struct kernel_vfs_mount *m=0,*other=0;struct bpf_object object={.references=1};
    assert(!kernel_bpffs_mount(&heap,0,&m));assert(!strcmp(kernel_bpf_filesystem_type(m),"bpf"));assert(!kernel_bpffs_mount(&heap,0,&other));
    assert(!ops(m)->mkdir(m,1,"dir",0750,0022));uint64_t dir=find(m,1,"dir",0);
    assert(kernel_bpffs_pin_inode(instance(m),dir,"has.dot",&object,0)==-KERNEL_EPERM);
    assert(!kernel_bpffs_pin_inode(instance(m),dir,"pinned",&object,0)&&object.references==2);
    assert(kernel_bpffs_pin_inode(instance(m),dir,"pinned",&object,0)==-KERNEL_EEXIST&&object.references==2);
    struct kernel_vfs_file listing={0};open_id(m,dir,&listing);uint64_t position=0;
    assert(expect_entry(&listing,&position,".",dir)==4);
    assert(expect_entry(&listing,&position,"..",1)==4);
    assert(expect_entry(&listing,&position,"pinned",find(m,dir,"pinned",0))==8);
    expect_end(&listing,position);close_file(&listing);
    absent(other,1,"dir");struct kernel_vfs_file held={0};uint64_t id=find(m,dir,"pinned",0);open_id(m,id,&held);
    assert(ops(m)->rmdir(m,1,"dir")==-KERNEL_ENOTEMPTY);
    assert(ops(m)->unmount(m)==-KERNEL_EBUSY);
    struct kernel_vfs_rename_result result={0};
    assert(!ops(m)->rename(instance(m),dir,"pinned",1,"renamed",0,&result)&&result.changed);
    assert(find(m,1,"renamed",0)==id);absent(m,dir,"pinned");
    assert(!ops(m)->rmdir(m,1,"dir"));assert(!ops(m)->unlink(m,1,"renamed"));
    assert(object.references==2);struct kernel_vfs_stat st;assert(!ops(m)->stat(&held,&st)&&!st.nlink);
    close_file(&held);assert(object.references==1);
    assert(!kernel_bpffs_pin_inode(instance(m),1,"last",&object,0));
    assert(!ops(m)->unmount(m)&&object.references==1);assert(!ops(other)->unmount(other));
    m=0;assert(!kernel_bpffs_mount(&heap,1,&m));
    assert(kernel_bpffs_pin_inode(instance(m),1,"ro",&object,0)==-KERNEL_EROFS);
    assert(ops(m)->mkdir(m,1,"ro",0755,0)==-KERNEL_EROFS);assert(!ops(m)->unmount(m));
}
static void metadata(void)
{
    struct kernel_vfs_mount *m=0;
    for(unsigned page_failure=0;page_failure<2;page_failure++) {
        unsigned completed=0;
        for(unsigned failure=1;failure<128;failure++) {
            struct kernel_heap_statistics before,after;kernel_heap_get_statistics(&heap,&before);
            uint64_t available=physical_page_available(&pages),resident=pages.shared_anon_pages;
            if(page_failure)fail_physical=failure;else fail_allocation=failure;
            int result=kernel_tracefs_mount(&heap,0,&m);
            fail_allocation=fail_physical=0;
            if(!result){assert(!ops(m)->unmount(m));m=0;completed=1;}
            else assert(result==-KERNEL_ENOMEM&&!m);
            kernel_heap_get_statistics(&heap,&after);
            assert(before.live_allocations==after.live_allocations);
            assert(pages.shared_anon_pages==resident && physical_page_available(&pages)==available);
            if(completed)break;
        }
        assert(completed);
    }
    assert(!kernel_tracefs_mount(&heap,0,&m));
    read_text(m,1,"available_events","raw_syscalls:sys_enter\nraw_syscalls:sys_exit\n");
    uint64_t events=find(m,1,"events",0),raw=find(m,events,"raw_syscalls",0);
    uint64_t enter=find(m,raw,"sys_enter",0),exit=find(m,raw,"sys_exit",0);
    read_text(m,enter,"id","1\n");read_text(m,exit,"id","2\n");
    struct kernel_vfs_file f={0};open_id(m,find(m,enter,"format",0),&f);
    char format[2048]={0};size_t n;assert(!ops(m)->pread(f.private_data,0,format,sizeof(format)-1,&n));
    assert(strstr(format,"field:long id;\toffset:8;\tsize:8;\tsigned:1;"));
    assert(strstr(format,"field:unsigned long args[6];\toffset:16;\tsize:48;\tsigned:0;"));
    assert(sizeof(struct kernel_trace_sys_enter)==64&&sizeof(struct kernel_trace_sys_exit)==24);
    close_file(&f);assert(!ops(m)->unmount(m));
    m=0;assert(!kernel_sysfs_mount(&heap,0,&m));
    uint64_t dev=find(m,1,"devices",0),system=find(m,dev,"system",0),cpu=find(m,system,"cpu",0);
    read_text(m,cpu,"possible","0-1\n");read_text(m,cpu,"online","0\n");
    uint64_t k=find(m,1,"kernel",0),btf=find(m,k,"btf",0);open_id(m,find(m,btf,"vmlinux",0),&f);
    unsigned char bytes[20];assert(!ops(m)->pread(f.private_data,1,bytes,sizeof(bytes),&n));
    assert(n==sizeof(kernel_btf_bytes)-1&&!memcmp(bytes,kernel_btf_bytes+1,n));
    close_file(&f);assert(!ops(m)->unmount(m));
}
int main(void)
{
    mm_test_attach(0);kernel_cpu_current()->state=KERNEL_CPU_ONLINE;
    struct boot_memory_layout layout={0};layout.usable_count=1;layout.usable[0].base=(uintptr_t)pool;layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout));assert(!physical_page_allocator_bind_access(&pages,access_page));
    assert(!physical_page_allocator_finalize(&pages)&&!kernel_heap_init(&heap,&pages,page_address));
    uint64_t baseline=physical_page_available(&pages);pins();metadata();
    struct kernel_heap_statistics s;kernel_heap_get_statistics(&heap,&s);assert(!s.live_allocations&&!s.current_pages);
    assert(physical_page_available(&pages)==baseline&&!physical_page_allocator_audit(&pages));
    puts("PASS bpffs mount isolation, pin references, rename/unlink, busy/readonly, trace formats, sysfs bytes and final owners");
}
