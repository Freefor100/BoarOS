#define main original_file_io_main
#include "file_io_concurrency.c"
#undef main
#include <kernel/xattr.h>
#include <ext4_fs.h>
#include <ext4_xattr.h>
#include <ext4_inode.h>
#include <ext4_blockdev.h>
#include <kernel/tmpfs.h>
#include <kernel/credentials.h>
static unsigned fail_attribute_allocation,fail_attribute_commit;
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *heap,size_t count,size_t size,void **owner)
{
    if (fail_attribute_allocation) {fail_attribute_allocation--;return KERNEL_HEAP_STATUS_EMPTY;}
    return __real_kernel_heap_allocate_zeroed(heap,count,size,owner);
}
int __real_ext4_xattr_set(struct ext4_inode_ref *,uint8_t,const char *,size_t,const void *,size_t);
int __wrap_ext4_xattr_set(struct ext4_inode_ref *ref,uint8_t index,const char *name,size_t length,const void *value,size_t size)
{
    int result=__real_ext4_xattr_set(ref,index,name,length,value,size);
    return !result && fail_attribute_commit ? EIO : result;
}
static void attributes(struct kernel_vfs_path *root)
{
    struct kernel_vfs_file file={0};char output[512];
    assert(!kernel_vfs_create_at(root,root,"attrs",0600,&file));
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_GET,"user.a",NULL,0,0)==-KERNEL_ENODATA);
    assert(!kernel_vfs_file_xattr(&file,KERNEL_XATTR_SET,"user.a","abc",3,KERNEL_XATTR_CREATE));
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_SET,"user.a","bad",3,KERNEL_XATTR_CREATE)==-KERNEL_EEXIST);
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_GET,"user.a",output,2,0)==-KERNEL_ERANGE);
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_GET,"user.a",output,sizeof(output),0)==3 && !memcmp(output,"abc",3));
    assert(!kernel_vfs_file_xattr(&file,KERNEL_XATTR_SET,"user.empty",NULL,0,0));
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_GET,"user.empty",NULL,0,0)==0);
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_SET,"user.missing","",0,3)==-KERNEL_ENODATA);
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_LIST,NULL,output,sizeof(output),0)==18);
    char large[701];memset(large,0x5b,sizeof(large));
    int temporary=kernel_tmpfs_is_mount(file.mount);
    if (temporary) fail_attribute_allocation=1; else fail_attribute_commit=1;
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_SET,"user.oom",large,sizeof(large),0)==
        (temporary ? -KERNEL_ENOMEM : -KERNEL_EIO));
    fail_attribute_allocation=fail_attribute_commit=0;
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_GET,"user.oom",NULL,0,0)==-KERNEL_ENODATA);
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_GET,"user.a",output,sizeof(output),0)==3 && !memcmp(output,"abc",3));
    if (!temporary) {
        struct lwext4_mount_adapter *adapter=file.mount->private_data;
        struct ext4_inode original;assert(!ext4_fraw_inode_fill(lwext4_node_file(file.private_data),&original));
        unsigned char damaged[4096] __attribute__((aligned(8)))={0};memcpy(damaged,&original,sizeof(original));
        struct ext4_inode_ref ref={.fs=adapter->device.fs,.inode=(void *)damaged};size_t count=0;
        ext4_inode_set_file_acl(ref.inode,&ref.fs->sb,0);
        ext4_inode_set_extra_isize(&ref.fs->sb,ref.inode,1024);
        assert(ext4_xattr_list(&ref,NULL,&count)==EIO);
        ext4_inode_set_extra_isize(&ref.fs->sb,ref.inode,32);
        *(uint32_t *)(damaged+128+32)=0xdeadbeef;
        assert(ext4_xattr_list(&ref,NULL,&count)==EIO);
        assert(!kernel_vfs_file_xattr(&file,KERNEL_XATTR_SET,"user.external",large,sizeof(large),0));
        assert(!ext4_fraw_inode_fill(lwext4_node_file(file.private_data),&original));
        uint64_t lba=ext4_inode_get_file_acl(&original,&adapter->device.fs->sb);assert(lba);
        struct ext4_block block;assert(!ext4_block_get(adapter->device.fs->bdev,&block,lba));
        /* No other actors are active: corrupt this borrowed read view only
         * for the call, then restore it before releasing the real owner. */
        unsigned char *checksum_byte=block.data+16; /* ext4 EA h_checksum on disk. */
        unsigned char checksum=*checksum_byte;*checksum_byte^=1;
        memcpy(damaged,&original,sizeof(original));
        assert(ext4_xattr_list(&ref,NULL,&count)==EIO);
        assert(ext4_xattr_get(&ref,1,"external",8,output,sizeof(output),&count)==EIO);
        *checksum_byte=checksum;assert(!ext4_block_set(adapter->device.fs->bdev,&block));
        assert(!kernel_vfs_file_xattr(&file,KERNEL_XATTR_REMOVE,"user.external",NULL,0,0));
    }
    assert(!kernel_vfs_unlink_at(root,root,"attrs",0));
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_GET,"user.a",output,sizeof(output),0)==3);
    assert(!kernel_vfs_file_xattr(&file,KERNEL_XATTR_REMOVE,"user.a",NULL,0,0));
    assert(!kernel_vfs_close(&file));
}
int main(int argc,char **argv)
{
    assert(argc==2);mm_test_attach(0);mm_test_activate_task();
    struct physical_page_allocator pages={0},backend_pages={0};struct kernel_heap heap={0},backend_heap={0};
    initialize(&pages,&heap,arena,sizeof(arena));initialize(&backend_pages,&backend_heap,backend,sizeof(backend));
    uint64_t available=physical_page_available(&pages),backend_available=physical_page_available(&backend_pages);
    struct kernel_vfs_mount mount={0},*temporary=NULL;struct kernel_vfs_path *root=NULL;
    assert(!kernel_tmpfs_create(&heap,0,NULL,&temporary) && !kernel_vfs_path_root(temporary,&heap,&root));
    attributes(root);assert(!kernel_vfs_path_release(&root) && !kernel_vfs_unmount(temporary));
    image=open(argv[1],O_RDWR);assert(image>=0);struct stat info;assert(!fstat(image,&info));
    struct kernel_page_cache cache={0};assert(!kernel_page_cache_init(&cache,&heap,&pages));
    struct kernel_block_device disk={.read=read_image,.write=write_image,.flush=flush_image,.capacity_bytes=info.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    assert(!kernel_block_register(&disk,KERNEL_BLOCK_DEVICE_NUMBER(105)) && !kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,0) && !kernel_vfs_path_root(&mount,&heap,&root));
    attributes(root);
    assert(!kernel_vfs_path_release(&root) && !kernel_vfs_unmount(&mount) && !kernel_page_cache_destroy(&cache) && !kernel_block_unregister(&disk));
    struct kernel_heap_statistics st;kernel_heap_get_statistics(&heap,&st);assert(!st.live_allocations);kernel_heap_get_statistics(&backend_heap,&st);assert(!st.live_allocations);
    assert(!physical_page_allocator_audit(&pages) && !physical_page_allocator_audit(&backend_pages) && physical_page_available(&pages)==available && physical_page_available(&backend_pages)==backend_available);
    assert(!close(image));mm_test_finish();puts("PASS tmpfs/ext4 inode xattrs, flags, empty values, unlink ownership and resource return");return 0;
}
