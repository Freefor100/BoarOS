#define main original_file_io_main
#include "file_io_concurrency.c"
#undef main
#include <kernel/credentials.h>
#include <kernel/posix_acl.h>
#include <kernel/xattr.h>
#include <ext4_balloc.h>
#include <ext4_inode.h>

static int fail_acl_block_allocation;
static uint64_t fail_acl_block_lba;
static unsigned acl_block_requests;
int __real_ext4_balloc_alloc_block(struct ext4_inode_ref *,ext4_fsblk_t,ext4_fsblk_t *);
int __wrap_ext4_balloc_alloc_block(struct ext4_inode_ref *inode,ext4_fsblk_t goal,ext4_fsblk_t *block)
{
    return fail_acl_block_allocation ? ENOSPC : __real_ext4_balloc_alloc_block(inode,goal,block);
}
int __real_ext4_trans_block_get(struct ext4_blockdev *,struct ext4_block *,uint64_t);
int __wrap_ext4_trans_block_get(struct ext4_blockdev *device,struct ext4_block *block,uint64_t lba)
{
    if (fail_acl_block_lba && lba==fail_acl_block_lba && ++acl_block_requests==2) return EIO;
    return __real_ext4_trans_block_get(device,block,lba);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    mm_test_attach(0);
    mm_test_activate_task();
    struct physical_page_allocator pages = {0}, backend_pages = {0};
    struct kernel_heap heap = {0}, backend_heap = {0};
    initialize(&pages, &heap, arena, sizeof(arena));
    initialize(&backend_pages, &backend_heap, backend, sizeof(backend));
    uint64_t available = physical_page_available(&pages);
    uint64_t backend_available = physical_page_available(&backend_pages);
    image = open(argv[1], O_RDWR);
    assert(image >= 0);
    struct stat stat;
    assert(!fstat(image, &stat));
    struct kernel_page_cache cache = {0};
    assert(!kernel_page_cache_init(&cache, &heap, &pages));
    struct kernel_block_device disk = {
        .read = read_image, .write = write_image, .flush = flush_image,
        .capacity_bytes = stat.st_size, .logical_block_size = 512,
        .cache_mode = KERNEL_BLOCK_CACHE_WRITEBACK,
    };
    struct kernel_vfs_mount mount = {0};
    assert(!kernel_block_register(&disk, KERNEL_BLOCK_DEVICE_NUMBER(106)));
    assert(!kernel_vfs_mount_ext4(&mount, &disk, &backend_heap, &cache, 0));
    struct lwext4_mount_adapter *adapter=mount.private_data;
    int nonjournal=adapter->device.fs->jbd_journal==NULL;
    struct kernel_vfs_file file = {0};
    if (nonjournal) {
        assert(!kernel_vfs_open(&mount,"/acl-raw",&file));
        assert(!kernel_vfs_file_set_mode(&file,0640));
    } else assert(!kernel_vfs_create(&mount, "/acl-raw", 0640, &file));
    ext4_file *raw = lwext4_node_file(file.private_data);
    const unsigned char acl[] = {1,0,0,0, 1,0,6,0, 2,0,0,0, 57,48,0,0,
                                 4,0,4,0, 16,0,4,0, 32,0,0,0};
    struct {uint32_t version;struct kernel_acl_entry entries[5];} user_acl={
        2,{{1,6,UINT32_MAX},{2,0,12345},{4,4,UINT32_MAX},
           {16,4,UINT32_MAX},{32,0,UINT32_MAX}}};
    unsigned char output[sizeof(acl)];
    size_t size = 0;
    const char *name = "system.posix_acl_access";
    assert(!kernel_vfs_file_xattr(&file,KERNEL_XATTR_SET,name,&user_acl,sizeof(user_acl),0));
    assert(!ext4_file_getxattr(raw, name, strlen(name), output, sizeof(output), &size));
    assert(size == sizeof(acl) && !memcmp(acl, output, size));
    struct kernel_vfs_path *root=NULL,*path=NULL;
    assert(!kernel_vfs_path_root(&mount,&heap,&root));
    assert(!kernel_vfs_path_lookup(root,"acl-raw",7,&path));
    struct kernel_credentials user=*kernel_credentials_current();
    user.fsuid=12345;user.fsgid=0;user.effective=0;user.groups=NULL;
    const struct kernel_credentials *previous=kernel_credentials_override(&user);
    assert(kernel_vfs_path_permission(path,4)==-KERNEL_EACCES);
    user.fsuid=12346;
    assert(!kernel_vfs_path_permission(path,4));
    kernel_credentials_override(previous);
    const unsigned char malformed[]={2,0,0,0};
    assert(!ext4_file_setxattr(raw,name,strlen(name),malformed,sizeof(malformed),0));
    user.effective=UINT64_C(1)<<KERNEL_CAP_DAC_OVERRIDE;
    previous=kernel_credentials_override(&user);
    assert(kernel_vfs_path_permission(path,4)==-KERNEL_EINVAL);
    kernel_credentials_override(previous);
    assert(!ext4_file_setxattr(raw,name,strlen(name),acl,sizeof(acl),0));
    assert(!kernel_vfs_path_release(&path) && !kernel_vfs_path_release(&root));
    assert(ext4_file_getxattr(raw, "system.posix_acl_access.bad", 27,
                             output, sizeof(output), &size) == EINVAL);
    char list[128];
    assert(!ext4_file_listxattr(raw, list, sizeof(list), &size, false));
    assert(size == strlen(name) + 1 && !strcmp(list, name));
    assert(!kernel_vfs_file_set_mode(&file,0600));
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_GET,name,&user_acl,sizeof(user_acl),0)==(int)sizeof(user_acl));
    assert(user_acl.entries[3].perm==0);
    assert(!kernel_vfs_file_set_mode(&file,0640));
    /* Exercise the actual nonjournal update path: if the external allocation
     * fails, the old inline ACL and its associated mode must both survive. */
    struct {uint32_t version;struct kernel_acl_entry entries[23];} large={.version=2};
    large.entries[0]=(struct kernel_acl_entry){1,6,UINT32_MAX};
    for (unsigned i=0;i<19;i++) large.entries[i+1]=(struct kernel_acl_entry){2,6,1000+i};
    large.entries[20]=(struct kernel_acl_entry){4,4,UINT32_MAX};
    large.entries[21]=(struct kernel_acl_entry){16,6,UINT32_MAX};
    large.entries[22]=(struct kernel_acl_entry){32,0,UINT32_MAX};
    struct jbd_journal *journal=adapter->device.fs->jbd_journal;
    adapter->device.fs->jbd_journal=NULL;
    fail_acl_block_allocation=1;
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_SET,name,&large,sizeof(large),0)==-KERNEL_ENOSPC);
    fail_acl_block_allocation=0;
    struct kernel_vfs_stat failed_stat;
    assert(!kernel_vfs_fstat(&file,&failed_stat) && (failed_stat.mode & 0777)==0640);
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_GET,name,&user_acl,sizeof(user_acl),0)==(int)sizeof(user_acl));
    assert(user_acl.entries[3].perm==4);
    adapter->device.fs->jbd_journal=journal;
    /* Fill the external EA block, leave inline room, then fail the removal
     * during an external-to-inline move. The old ACL remains authoritative. */
    assert(!kernel_vfs_file_xattr(&file,KERNEL_XATTR_REMOVE,name,NULL,0,0));
    unsigned char padding[48]={0};
    assert(!kernel_vfs_file_xattr(&file,KERNEL_XATTR_SET,"user.pad",padding,sizeof(padding),0));
    assert(!kernel_vfs_file_xattr(&file,KERNEL_XATTR_SET,name,&user_acl,sizeof(user_acl),0));
    unsigned char bulk[4096]={0};size_t bulk_size=adapter->device.lg_bsize-112U;
    assert(bulk_size<=sizeof(bulk));
    assert(!kernel_vfs_file_xattr(&file,KERNEL_XATTR_SET,"user.bulk",bulk,bulk_size,0));
    assert(!kernel_vfs_file_xattr(&file,KERNEL_XATTR_REMOVE,"user.pad",NULL,0,0));
    struct ext4_inode external_inode;
    assert(!ext4_fraw_inode_fill(raw,&external_inode));
    fail_acl_block_lba=ext4_inode_get_file_acl(&external_inode,&adapter->device.fs->sb);
    assert(fail_acl_block_lba);
    struct {uint32_t version;struct kernel_acl_entry entries[7];} medium={
        2,{{1,6,UINT32_MAX},{2,0,12345},{2,6,23456},{2,6,34567},
           {4,4,UINT32_MAX},{16,6,UINT32_MAX},{32,0,UINT32_MAX}}};
    adapter->device.fs->jbd_journal=NULL;acl_block_requests=0;
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_SET,name,&medium,sizeof(medium),0)==-KERNEL_EIO);
    assert(acl_block_requests==2);fail_acl_block_lba=0;
    assert(!kernel_vfs_fstat(&file,&failed_stat) && (failed_stat.mode & 0777)==0640);
    assert(kernel_vfs_file_xattr(&file,KERNEL_XATTR_GET,name,&user_acl,sizeof(user_acl),0)==(int)sizeof(user_acl));
    assert(user_acl.entries[3].perm==4);
    adapter->device.fs->jbd_journal=journal;
    assert(!ext4_file_removexattr(raw, name, strlen(name)));
    assert(!ext4_file_setxattr(raw, name, strlen(name), acl, sizeof(acl), 0));
    assert(!kernel_vfs_close(&file));
    if (nonjournal) goto Cleanup;
    struct kernel_vfs_path *top=NULL,*directory=NULL,*sub=NULL;
    struct kernel_vfs_file dirfile={0},subfile={0};
    assert(!kernel_vfs_path_root(&mount,&heap,&top));
    assert(!kernel_vfs_mkdir_at(top,top,"acl-dir",0777,0));
    assert(!kernel_vfs_path_lookup(top,"acl-dir",7,&directory));
    assert(!kernel_vfs_path_open(directory,&dirfile));
    unsigned char external[4096]={1};
    size_t external_size=adapter->device.lg_bsize-64U;
    assert(external_size<=sizeof(external));
    assert(!ext4_file_setxattr(lwext4_node_file(dirfile.private_data),"user.external",13,
                               external,external_size,0));
    const unsigned char empty_acl[]={1,0,0,0};
    assert(!ext4_file_setxattr(lwext4_node_file(dirfile.private_data),KERNEL_ACL_DEFAULT,
                             strlen(KERNEL_ACL_DEFAULT),empty_acl,sizeof(empty_acl),0));
    assert(kernel_vfs_file_xattr(&dirfile,KERNEL_XATTR_GET,KERNEL_ACL_DEFAULT,
                                 &user_acl,sizeof(user_acl),0)==-KERNEL_ENODATA);
    struct kernel_vfs_file empty_file={0};struct kernel_vfs_stat empty_stat;
    assert(!kernel_vfs_create_at(directory,top,"no-default",0666,&empty_file,077));
    assert(!kernel_vfs_fstat(&empty_file,&empty_stat) && (empty_stat.mode & 0777)==0600);
    assert(!kernel_vfs_close(&empty_file));
    assert(!kernel_vfs_unlink_at(directory,top,"no-default",0));
    user_acl.entries[0].perm=7;user_acl.entries[1].perm=6;user_acl.entries[3].perm=6;
    assert(!kernel_vfs_file_xattr(&dirfile,KERNEL_XATTR_SET,KERNEL_ACL_DEFAULT,&user_acl,sizeof(user_acl),0));
    struct kernel_vfs_file inherited_file={0};struct kernel_vfs_stat inherited_stat;
    assert(!kernel_vfs_create_at(directory,top,"inherited",0666,&inherited_file,077));
    assert(!kernel_vfs_fstat(&inherited_file,&inherited_stat));
    struct ext4_inode inherited_inode;
    assert(!ext4_fraw_inode_fill(lwext4_node_file(inherited_file.private_data),&inherited_inode));
    uint32_t extra=to_le32(inherited_inode.ctime_extra);
    assert(inherited_stat.ctime.seconds==(int32_t)ext4_inode_get_change_inode_time(&inherited_inode)+
           ((int64_t)(extra&3U)<<32U));
    assert(inherited_stat.ctime.nanoseconds==(extra>>2U));
    assert(!kernel_vfs_close(&inherited_file));
    assert(!kernel_vfs_mkdir_at(directory,top,"sub",0777,077));
    assert(!kernel_vfs_path_lookup(directory,"sub",3,&sub));
    assert(!kernel_vfs_path_open(sub,&subfile));
    unsigned char inherited[128];size_t inherited_size=0;
    int raw_result=ext4_file_getxattr(lwext4_node_file(subfile.private_data),KERNEL_ACL_DEFAULT,
        strlen(KERNEL_ACL_DEFAULT),inherited,sizeof(inherited),&inherited_size);
    assert(!raw_result && inherited_size==sizeof(acl));
    int got=kernel_vfs_file_xattr(&subfile,KERNEL_XATTR_GET,KERNEL_ACL_DEFAULT,&user_acl,sizeof(user_acl),0);
    assert(got==(int)sizeof(user_acl));
    assert(!kernel_vfs_close(&subfile) && !kernel_vfs_close(&dirfile));
    assert(!kernel_vfs_path_release(&sub) && !kernel_vfs_path_release(&directory) && !kernel_vfs_path_release(&top));
Cleanup:
    assert(!kernel_vfs_unmount(&mount));
    assert(!kernel_page_cache_destroy(&cache));
    assert(!kernel_block_unregister(&disk));
    assert(physical_page_available(&pages) == available);
    assert(physical_page_available(&backend_pages) == backend_available);
    close(image);
    mm_test_finish();
    puts("ext4 ACL: permissions, inheritance, timestamps and nonjournal fault paths passed");
    return 0;
}
