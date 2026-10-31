#define main original_file_io_main
#include "file_io_concurrency.c"
#undef main
#include "../../fs/ext4_backend.h"

int main(int argc,char **argv)
{
    assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);mm_test_attach(0);mm_test_activate_task();
    struct physical_page_allocator pages={0},backend_pages={0};struct kernel_heap heap={0},backend_heap={0};
    initialize(&pages,&heap,arena,sizeof(arena));initialize(&backend_pages,&backend_heap,backend,sizeof(backend));
    uint64_t available=physical_page_available(&pages),backend_available=physical_page_available(&backend_pages);
    image=open(argv[1],O_RDWR);assert(image>=0);struct stat info;assert(!fstat(image,&info));
    struct kernel_page_cache cache={0};assert(!kernel_page_cache_init(&cache,&heap,&pages));
    struct kernel_block_device disk={.read=read_image,.write=write_image,.flush=flush_image,
        .capacity_bytes=info.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    assert(!kernel_block_register(&disk,KERNEL_BLOCK_DEVICE_NUMBER(108)));
    struct kernel_vfs_mount original={0},*parent=NULL;
    assert(!kernel_vfs_mount_ext4(&original,&disk,&backend_heap,&cache,1));
    assert(!kernel_page_cache_start_worker(&cache)&&!kernel_vfs_start_journal_worker(&original));
    struct kernel_vfs_instance *instance=original.private_data;
    struct kernel_vfs_path *root=NULL,*parent_root=NULL,*covered=NULL,*source=NULL,*view=NULL,*single=NULL;
    assert(!kernel_vfs_path_root(&original,&heap,&root));
    assert(!kernel_tmpfs_create(&heap,0,NULL,&parent)&&!kernel_vfs_path_root(parent,&heap,&parent_root));
    assert(!kernel_vfs_mkdir_at(parent_root,parent_root,"readonly",0755,0));
    assert(!kernel_vfs_path_resolve(parent_root,parent_root,"readonly",1,&covered));
    assert(!kernel_vfs_bind_mount(root,covered,0)&&!kernel_vfs_path_release(&covered));
    struct kernel_vfs_path *readonly=NULL;
    assert(!kernel_vfs_path_resolve(parent_root,parent_root,"readonly",1,&readonly));
    assert(!kernel_vfs_remount(root,0,NULL));
    assert(kernel_vfs_mkdir_at(readonly,parent_root,"forbidden",0700,0)==-KERNEL_EROFS);
    assert(!kernel_vfs_umount(readonly,0)&&!kernel_vfs_path_release(&readonly));
    assert(!kernel_vfs_mkdir_at(root,root,"sub",0755,0));
    struct kernel_vfs_file file={0};size_t done;
    assert(!kernel_vfs_create_at(root,root,"sub/data",0644,&file,0));
    assert(!kernel_vfs_pwrite(&file,0,"owner",5,&done)&&done==5&&!kernel_vfs_close(&file));
    assert(!kernel_vfs_mkdir_at(parent_root,parent_root,"original",0755,0));
    assert(!kernel_vfs_mkdir_at(parent_root,parent_root,"view",0755,0));
    assert(!kernel_vfs_create_at(parent_root,parent_root,"single",0644,&file,0)&&!kernel_vfs_close(&file));
    assert(!kernel_vfs_path_resolve(parent_root,parent_root,"original",1,&covered));
    assert(!kernel_vfs_mount_attach(&original,covered)&&!kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_path_resolve(root,root,"sub",1,&source));
    assert(!kernel_vfs_path_resolve(parent_root,parent_root,"view",1,&covered));
    assert(!kernel_vfs_bind_mount(source,covered,0)&&!kernel_vfs_path_release(&source)&&!kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_path_resolve(root,root,"sub/data",1,&source));
    assert(!kernel_vfs_path_resolve(parent_root,parent_root,"single",1,&covered));
    assert(!kernel_vfs_bind_mount(source,covered,0)&&!kernel_vfs_path_release(&source)&&!kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_unlink_at(root,root,"sub/data",0));
    assert(!kernel_vfs_remount(root,4096U|2U|4U|8U|32768U,NULL));
    assert(!kernel_vfs_umount(root,0)&&!kernel_vfs_path_release(&root));
    assert(!original.private_data&&!original.id&&!original.source_name&&!original.retired&&
        !original.nosuid&&!original.nodev&&!original.noexec&&!original.silent&&disk.claim_owner==&instance->backend_mount);
    /* Caller storage may be reused once its view is unmounted. */
    memset(&original,0xa5,sizeof(original));
    assert(!kernel_vfs_path_resolve(parent_root,parent_root,"single",1,&single));
    assert(!kernel_vfs_path_open(single,&file)&&!kernel_vfs_file_acquire_write(&file));
    char bytes[6]={0};assert(!kernel_vfs_pread(&file,0,bytes,5,&done,NULL)&&done==5&&!strcmp(bytes,"owner"));
    assert(!kernel_vfs_pwrite(&file,0,"BOUND",5,&done)&&done==5&&!kernel_vfs_close(&file));
    assert(!kernel_vfs_path_resolve(parent_root,parent_root,"view",1,&view));
    assert(!kernel_vfs_umount(view,0)&&!kernel_vfs_path_release(&view));
    assert(!kernel_vfs_umount(single,0)&&!kernel_vfs_path_release(&single));
    assert(!kernel_vfs_reap_mounts()&&!kernel_block_claimed(&disk));
    assert(!kernel_vfs_path_release(&parent_root)&&!kernel_vfs_unmount(parent));
    struct kernel_vfs_mount reused={0};
    assert(!kernel_vfs_mount_ext4(&reused,&disk,&backend_heap,&cache,1)&&!kernel_vfs_unmount(&reused));
    assert(!reused.private_data&&!reused.id&&!reused.source_name);
    assert(!kernel_vfs_mount_ext4(&reused,&disk,&backend_heap,&cache,1));
    assert(!kernel_vfs_path_root(&reused,&heap,&root)&&!kernel_vfs_path_release(&root)&&!kernel_vfs_unmount(&reused));
    assert(!kernel_page_cache_destroy(&cache)&&!kernel_block_unregister(&disk));
    struct kernel_heap_statistics stats;kernel_heap_get_statistics(&heap,&stats);assert(!stats.live_allocations&&!stats.current_pages);
    kernel_heap_get_statistics(&backend_heap,&stats);assert(!stats.live_allocations&&!stats.current_pages);
    assert(physical_page_available(&pages)==available&&physical_page_available(&backend_pages)==backend_available);
    assert(!close(image));mm_test_finish();
    puts("PASS ext4 bind views outlive reused caller mount storage; unlinked file root and canonical ancestors return worker/cache/device owners");
    return 0;
}
