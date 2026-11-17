#include "../../fs/tmpfs.c"
#include "pseudo_fs_fixture.h"

static void temporary_inodes(void)
{
    struct kernel_vfs_mount *m=0;
    assert(!kernel_tmpfs_create(&heap,0,"nr_inodes=2",&m));
    struct kernel_vfs_file f={0}; struct kernel_vfs_stat st;
    uint64_t root_size=stat_id(m,1).size;
    assert(!ops(m)->tmpfile(m,1,KERNEL_VFS_S_IFREG|0666,&f,0027));
    assert(!ops(m)->stat(&f,&st) && !st.nlink && (st.mode&0777)==0640);
    assert(stat_id(m,1).size==root_size);
    struct kernel_vfs_node *n=f.private_data;
    struct kernel_vfs_file other={0};
    assert(ops(m)->tmpfile(m,1,KERNEL_VFS_S_IFREG|0600,&other,0)==-KERNEL_ENOSPC);
    assert(ops(m)->link(instance(m),n->inode,1,"ordinary")==-KERNEL_ENOENT);
    fail_allocation=1;
    assert(ops(m)->link_tmpfile(instance(m),n->inode,1,"saved")==-KERNEL_ENOMEM);
    fail_allocation=0;assert(!ops(m)->stat(&f,&st)&&!st.nlink);
    assert(!ops(m)->link_tmpfile(instance(m),n->inode,1,"saved"));
    assert(!ops(m)->stat(&f,&st)&&st.nlink==1);
    assert(find(m,1,"saved",0)==n->inode);
    assert(!ops(m)->unlink(m,1,"saved"));close_file(&f);
    for (unsigned fail=1;fail<12;fail++) {
        fail_allocation=fail;
        int r=ops(m)->tmpfile(m,1,KERNEL_VFS_S_IFREG|0600,&f,0);
        fail_allocation=0;
        assert(!r || r==-KERNEL_ENOMEM);
        if (!r) close_file(&f);
        assert(tm(m->private_data)->inode_count==1);
    }
    assert(!ops(m)->unmount(m));
    puts("PASS tmpfile backend: zero links, no directory entry, mode/umask, quota, publication and allocation rollback");
}
int main(void)
{
    mm_test_attach(0); struct boot_memory_layout layout={0};layout.usable_count=1;
    layout.usable[0].base=(uintptr_t)pool;layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout));
    assert(!physical_page_allocator_bind_access(&pages,access_page));
    assert(!physical_page_allocator_finalize(&pages)&&!kernel_heap_init(&heap,&pages,page_address));
    uint64_t baseline=physical_page_available(&pages);
    temporary_inodes();
    struct kernel_heap_statistics s;kernel_heap_get_statistics(&heap,&s);
    assert(!s.live_allocations&&!s.current_pages&&!pages.shared_anon_pages);
    assert(physical_page_available(&pages)==baseline&&!physical_page_allocator_audit(&pages));
    puts("PASS tmpfile backend: final resource return");
}
