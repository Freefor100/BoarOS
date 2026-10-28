#define main original_file_io_main
#include "file_io_concurrency.c"
#undef main
#include "../../fs/ext4_backend.h"
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <ext4_journal_memory.h>
#include <stdlib.h>
static unsigned fail_flush,fail_worker;
static void *retained_domain;
void __real_jbd_memory_set_account(struct jbd_memory *,const struct jbd_memory_account *);
void __wrap_jbd_memory_set_account(struct jbd_memory *memory,const struct jbd_memory_account *account)
{ retained_domain=account->context;__real_jbd_memory_set_account(memory,account); }
enum kernel_scheduler_status __real_kernel_thread_create_joinable(void (*)(void *),void *,struct kernel_thread_join *);
enum kernel_scheduler_status __wrap_kernel_thread_create_joinable(void (*entry)(void *),void *context,struct kernel_thread_join *owner)
{
    if (fail_worker) {fail_worker=0;return KERNEL_SCHEDULER_STATUS_NO_MEMORY;}
    return __real_kernel_thread_create_joinable(entry,context,owner);
}
static enum kernel_block_status remount_flush(void *context)
{ return fail_flush ? KERNEL_BLOCK_STATUS_IO : flush_image(context); }
int main(int argc,char **argv)
{
    assert(argc==2||argc==3);setvbuf(stdout,NULL,_IONBF,0);mm_test_attach(0);mm_test_activate_task();
    struct physical_page_allocator pages={0},backend_pages={0};struct kernel_heap heap={0},backend_heap={0};
    initialize(&pages,&heap,arena,sizeof(arena));initialize(&backend_pages,&backend_heap,backend,sizeof(backend));
    uint64_t available=physical_page_available(&pages),backend_available=physical_page_available(&backend_pages);
    image=open(argv[1],O_RDWR);assert(image>=0);struct stat info;assert(!fstat(image,&info));
    struct kernel_page_cache cache={0};assert(!kernel_page_cache_init(&cache,&heap,&pages));
    struct kernel_block_device disk={.read=read_image,.write=write_image,.flush=remount_flush,
        .capacity_bytes=info.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};assert(!kernel_block_register(&disk,KERNEL_BLOCK_DEVICE_NUMBER(107)));
    assert(!kernel_block_set_read_only(&disk,1));
    assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,1));
    struct lwext4_mount_adapter *adapter=mount.private_data;
    struct kernel_vfs_path *root=NULL;assert(!kernel_vfs_path_root(&mount,&heap,&root));
    assert(kernel_vfs_remount(root,2,NULL)==-KERNEL_EACCES && adapter->instance.read_only && !mount.nosuid);
    assert(!adapter->journal_started && !adapter->instance.writeback.domain);
    assert(!kernel_block_set_read_only(&disk,0));
    if (argc==3 && !strcmp(argv[2],"start-fault")) {
        fail_flush=1;
        assert(kernel_vfs_remount(root,0,NULL)==-KERNEL_EIO);
        assert(adapter->instance.read_only && adapter->device.fs->read_only && adapter->mount_error==EIO);
        assert(adapter->device.fs->jbd_journal && adapter->device.fs->jbd_journal->memory && retained_domain);
        struct kernel_writeback_owner borrowed={.domain=retained_domain};struct kernel_writeback_statistics pressure;
        kernel_writeback_domain_snapshot(&borrowed,&pressure);assert(pressure.metadata_bytes);
        assert(!adapter->journal_started && !adapter->instance.writeback.domain && kernel_block_claimed(&disk));
        puts("PASS journal-start flush EIO retains accounted session storage and original RO mount");return 0;
    }
    if (getenv("REMOUNT_UNSUPPORTED_RO")) {
        assert(kernel_vfs_remount(root,2,NULL)==-KERNEL_EROFS);
        assert(adapter->instance.read_only && adapter->device.fs->read_only && !adapter->mount_error && !mount.nosuid);
        struct kernel_vfs_file directory={0};
        assert(!kernel_vfs_open(&mount,"/lost+found",&directory) && !kernel_vfs_close(&directory));
        puts("PASS unsupported RO feature refusal preserves readable and unmountable RO backend");
        goto Unmount;
    }
    if (getenv("REMOUNT_WORKER_FAILURE")) {
        fail_worker=1;
        assert(kernel_vfs_remount(root,2,NULL)==-KERNEL_ENOMEM);
        assert(adapter->instance.read_only && adapter->device.fs->read_only && !adapter->mount_error && !mount.nosuid);
        assert(!adapter->journal_started && !adapter->checkpoint_started && !adapter->instance.writeback.domain);
        puts("PASS initial RO worker allocation failure returns session and writeback owners before retry");
    }
    assert(!kernel_vfs_remount(root,0,NULL) && !adapter->instance.read_only && !adapter->device.fs->read_only);
    assert(adapter->journal_started && adapter->instance.writeback.domain);
    struct kernel_vfs_file writer={0},reader={0};size_t done;char bytes[8]={0};
    assert(!kernel_vfs_create_at(root,root,"/remount-stable",0600,&writer));
    assert(!kernel_vfs_file_acquire_write(&writer));
    assert(!kernel_vfs_pwrite(&writer,0,"stable",6,&done)&&done==6);
    assert(kernel_vfs_remount(root,1|2|4,NULL)==-KERNEL_EBUSY && !adapter->instance.read_only);
    assert(!kernel_vfs_close(&writer));
    assert(!kernel_vfs_open(&mount,"/remount-stable",&reader));
    if (argc==3) {
        fail_flush=1;
        assert(kernel_vfs_remount(root,1|2|4,NULL)==-KERNEL_EIO);
        assert(!adapter->instance.read_only&&!adapter->device.fs->read_only&&!mount.nosuid&&!mount.nodev);
        assert(adapter->journal_started && kernel_block_claimed(&disk));
        assert(!kernel_vfs_close(&reader) && !kernel_vfs_path_release(&root));
        assert(kernel_vfs_unmount(&mount)==-KERNEL_EIO && mount.private_data && kernel_block_claimed(&disk));
        assert(kernel_block_unregister(&disk)==-KERNEL_EBUSY);
        puts("PASS ext4 remount flush EIO preserves RW attributes and retains journal/cache/device error owners");return 0;
    }
    assert(!kernel_vfs_remount(root,1|2|4,NULL) && adapter->instance.read_only && adapter->device.fs->read_only);
    assert(!adapter->device.fs->jbd_journal && !adapter->instance.writeback.domain && adapter->journal_started);
    assert(!kernel_vfs_pread(&reader,0,bytes,6,&done,NULL)&&done==6&&!memcmp(bytes,"stable",6));
    assert(!kernel_vfs_remount(root,0,NULL) && !adapter->instance.read_only && !adapter->device.fs->read_only);
    uint64_t observed=kernel_vfs_error_sequence(&reader);
    assert(!kernel_vfs_sync(&reader,0,&observed));
    assert(!kernel_vfs_open(&mount,"/remount-stable",&writer) && writer.private_data==reader.private_data &&
        !kernel_vfs_file_acquire_write(&writer));
    assert(!kernel_vfs_ftruncate(&writer,4096));
    assert(!kernel_vfs_fallocate(&writer,0,0,8192));
    assert(!kernel_vfs_close(&reader));
    assert(!kernel_vfs_pwrite(&writer,0,"STABLE",6,&done)&&done==6 && !kernel_vfs_close(&writer));
    assert(!kernel_vfs_remount(root,1,NULL) && !kernel_vfs_remount(root,0,NULL));
Unmount:
    assert(!kernel_vfs_path_release(&root) && !kernel_vfs_unmount(&mount));
    assert(!kernel_page_cache_destroy(&cache) && !kernel_block_unregister(&disk));
    struct kernel_heap_statistics stats;kernel_heap_get_statistics(&heap,&stats);assert(!stats.live_allocations&&!stats.current_pages);
    kernel_heap_get_statistics(&backend_heap,&stats);assert(!stats.live_allocations&&!stats.current_pages);
    assert(physical_page_available(&pages)==available && physical_page_available(&backend_pages)==backend_available);
    close(image);mm_test_finish();puts("PASS ext4 initially RO device refusal, real journal/writeback transitions, dirty data, repeated remount and final owners");return 0;
}
