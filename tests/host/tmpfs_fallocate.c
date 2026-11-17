#include "../../fs/tmpfs.c"
#include "pseudo_fs_fixture.h"
static int reserve(struct kernel_vfs_file *f,unsigned mode,uint64_t start,uint64_t size)
{ uint64_t actual;int changed;int r=ops(f->mount)->fallocate(f->private_data,mode,start,size,&actual,&changed);if(!r){assert(changed);((struct kernel_vfs_node *)f->private_data)->size=actual;}return r; }
static void test(void)
{
 struct kernel_vfs_mount *m=0;char options[80];snprintf(options,sizeof(options),"size=%llu",(unsigned long long)4*BOAROS_PAGE_SIZE);
 assert(!kernel_tmpfs_create(&heap,0,options,&m));regular(m,1,"file");struct kernel_vfs_file f={0};open_id(m,find(m,1,"file",0),&f);
 struct tmp_inode *i=ti(f.private_data);uint64_t p=BOAROS_PAGE_SIZE;struct kernel_vfs_stat st;
 assert(!reserve(&f,1,p+1,2*p));assert(!i->size&&kernel_memory_object_resident_pages(i->memory)==3);
 uint64_t address;assert(!kernel_memory_object_find_page(i->memory,2,&address));((char *)access_page(address))[11]='X';assert(!physical_page_release(&pages,address));
 assert(reserve(&f,0,0,5*p)==-KERNEL_ENOSPC);assert(!i->size&&kernel_memory_object_resident_pages(i->memory)==3);
 assert(!reserve(&f,0,0,4*p));assert(i->size==4*p);assert(!ops(m)->stat(&f,&st)&&st.blocks==4*p/512);
 assert(!kernel_memory_object_find_page(i->memory,2,&address)&&((char *)access_page(address))[11]=='X');assert(!physical_page_release(&pages,address));
 uint64_t actual;int changed;assert(!ops(m)->truncate(f.private_data,0,&actual,&changed));
 assert(!reserve(&f,1,0,4*p));assert(!ops(m)->truncate(f.private_data,p,&actual,&changed));assert(kernel_memory_object_resident_pages(i->memory)==4);
 assert(!ops(m)->truncate(f.private_data,p,&actual,&changed));assert(kernel_memory_object_resident_pages(i->memory)==1);
 assert(!reserve(&f,1,3*p,p));assert(!reserve(&f,3,3*p,p));assert(kernel_memory_object_resident_pages(i->memory)==1);
 assert(!ops(m)->truncate(f.private_data,0,&actual,&changed));
 for(unsigned fail=1;fail<=5;fail++){
  fail_allocation=fail;int r=reserve(&f,0,0,4*p);fail_allocation=0;
  assert(!r||r==-KERNEL_ENOMEM);if(r)assert(!i->size&&!kernel_memory_object_resident_pages(i->memory));
  assert(!ops(m)->truncate(f.private_data,0,&actual,&changed));assert(!tm(m->private_data)->pages.used);
 }
 /* Actual physical exhaustion, not just the metadata fault hook. */
 uint64_t held[4096];unsigned count=0;while(count<4096&&!physical_page_allocate(&pages,&held[count]))count++;
 assert(reserve(&f,0,0,p)==-KERNEL_ENOMEM);assert(!i->size&&!kernel_memory_object_resident_pages(i->memory));
 while(count)assert(!physical_page_release(&pages,held[--count]));
 close_file(&f);assert(!ops(m)->unmount(m));puts("PASS tmpfs fallocate: reservation, quota, content, truncate/punch, allocation rollback and physical exhaustion");
}
int main(void)
{
 mm_test_attach(0);struct boot_memory_layout layout={0};layout.usable_count=1;layout.usable[0].base=(uintptr_t)pool;layout.usable[0].size=sizeof(pool);
 assert(!physical_page_allocator_init(&pages,&layout));assert(!physical_page_allocator_bind_access(&pages,access_page));assert(!physical_page_allocator_finalize(&pages)&&!kernel_heap_init(&heap,&pages,page_address));uint64_t baseline=physical_page_available(&pages);test();
 struct kernel_heap_statistics s;kernel_heap_get_statistics(&heap,&s);assert(!s.live_allocations&&!s.current_pages&&!pages.shared_anon_pages);assert(physical_page_available(&pages)==baseline&&!physical_page_allocator_audit(&pages));puts("PASS tmpfs fallocate: final resource return");
}
