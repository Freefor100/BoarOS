#define main file_io_fixture_main
#define flush_image fixture_flush_image
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#include "file_io_concurrency.c"
#pragma GCC diagnostic pop
#undef flush_image
#undef main
#include "../../fs/ext4_backend.h"
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <kernel/time.h>
#include <stdlib.h>
enum kernel_time_status __wrap_kernel_time_deadline_from_monotonic(uint64_t target,uint64_t *deadline)
{
    *deadline=target;
    return target<=kernel_time_monotonic_ns()?KERNEL_TIME_STATUS_DEADLINE_PASSED:KERNEL_TIME_STATUS_OK;
}

static struct lwext4_mount_adapter *residual_adapter;
static unsigned shutdown_entered,recovery_live_accepts;
int __real_jbd_journal_accept_state_held(struct jbd_journal *,struct jbd_trans *,uint64_t);
int __wrap_jbd_journal_accept_state_held(struct jbd_journal *journal,struct jbd_trans *operation,uint64_t now)
{
    int result=__real_jbd_journal_accept_state_held(journal,operation,now);
    if (!result && residual_adapter && shutdown_entered && journal->accepted_sequence>journal->checkpoint_sequence) {
        if (!residual_adapter->journal_started)
            fprintf(stderr,"orphan cleanup after workers stopped: accepted=%llu checkpoint=%llu\n",
                (unsigned long long)journal->accepted_sequence,(unsigned long long)journal->checkpoint_sequence);
        assert(residual_adapter->journal_started);
        recovery_live_accepts++;
    }
    return result;
}
static unsigned hold_flush,flush_entered,release_flush,fallocate_done;
static enum kernel_block_status fallocate_flush(void *unused)
{
    (void)unused;
    assert(!kernel_cpu_current()->logical_id && !kernel_cpu_current()->raw_locks);
    if (__atomic_exchange_n(&hold_flush,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&flush_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&release_flush,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return fsync(image) ? KERNEL_BLOCK_STATUS_IO : KERNEL_BLOCK_STATUS_OK;
}
struct synchronous_fallocate { unsigned cpu,mode; int64_t result; enum kernel_files_status status; };
static void *synchronous_caller(void *argument)
{
    struct synchronous_fallocate *call=argument;
    mm_test_attach(1); mm_test_bind(&mm); mm_test_bind_files(&files); mm_test_context=context;
    kernel_cpu_current()->logical_id=call->cpu;
    call->status=kernel_files_fallocate(&files,0,call->mode,call->mode==3?13:8192,4096,&call->result);
    __atomic_store_n(&fallocate_done,1,__ATOMIC_RELEASE);
    mm_test_finish(); return NULL;
}
static void synchronous_case(struct kernel_heap *heap,struct kernel_heap *backend_heap,
    struct physical_page_allocator *pages,struct kernel_block_device *disk,unsigned flags,unsigned cpu,unsigned mode)
{
    struct kernel_page_cache cache={0}; struct kernel_vfs_mount mount={0};
    assert(!kernel_page_cache_init(&cache,heap,pages) && !kernel_vfs_mount_ext4(&mount,disk,backend_heap,&cache,0));
    files=(struct kernel_files){0}; assert(!kernel_files_create(&files,heap));
    char name[64]; snprintf(name,sizeof(name),"/sync-%u-%u-%u",flags,cpu,mode);
    int error; assert(!kernel_open_file_create_mode(heap,&mount,name,0600,&control,&error) && !error);
    control->open_flags=2; assert(!kernel_vfs_file_acquire_write(&control->file));
    unsigned char initial[8192]; memset(initial,0x6d,sizeof(initial)); size_t done;
    assert(!kernel_vfs_pwrite(&control->file,0,initial,sizeof(initial),&done) && done==sizeof(initial));
    uint64_t observed=0; assert(!kernel_vfs_sync(&control->file,0,&observed));
    control->open_flags=2|flags; control->offset=123;
    struct kernel_open_file_description *slot=control;
    assert(!kernel_open_file_acquire(slot) && !kernel_files_install_new_owned_at(&files,0,0,&slot));
    assert(!kernel_page_cache_start_worker(&cache) && !kernel_vfs_start_journal_worker(&mount));
    assert(!kernel_files_check_task_domain(&files,&mm));
    struct lwext4_mount_adapter *adapter=lwext4_instance(mount.private_data);
    struct ext4_journal_progress progress;
    assert(!ext4_journal_group_progress(adapter->mount_point,&progress) && progress.accepted==progress.durable);
    flush_entered=release_flush=fallocate_done=0; __atomic_store_n(&hold_flush,1,__ATOMIC_RELEASE);
    struct synchronous_fallocate call={.cpu=cpu,.mode=mode}; pthread_t caller;
    assert(!pthread_create(&caller,NULL,synchronous_caller,&call));
    while (!__atomic_load_n(&flush_entered,__ATOMIC_ACQUIRE) && !__atomic_load_n(&fallocate_done,__ATOMIC_ACQUIRE)) sched_yield();
    if (__atomic_load_n(&fallocate_done,__ATOMIC_ACQUIRE))
        fprintf(stderr,"premature fallocate return: flags=%o cpu=%u mode=%u result=%lld\n",flags,cpu,mode,(long long)call.result);
    assert(!__atomic_load_n(&fallocate_done,__ATOMIC_ACQUIRE) && flush_entered);
    assert(!ext4_journal_group_progress(adapter->mount_point,&progress) && progress.accepted>progress.durable);
    struct kernel_vfs_node *node=kernel_open_file_node(control); struct kernel_lock_guard operation={0},position={0};
    assert(kernel_mutex_try_lock(&node->write_operation,&operation)); kernel_lock_release(&operation);
    assert(kernel_mutex_try_lock(&control->offset_lock,&position)); kernel_lock_release(&position);
    __atomic_store_n(&release_flush,1,__ATOMIC_RELEASE);
    assert(!pthread_join(caller,NULL) && !call.status && !call.result && control->offset==123);
    assert(!ext4_journal_group_progress(adapter->mount_point,&progress) && progress.durable>=progress.accepted);
    assert(!kernel_page_cache_invalidate_node(&cache,node));
    unsigned char actual[sizeof(initial)];
    assert(!kernel_open_file_pread(control,0,actual,sizeof(actual),&done) && done==sizeof(actual));
    if (mode==3) memset(initial+13,0,4096);
    assert(!memcmp(actual,initial,sizeof(actual)));
    mm_test_attach(0); mm_test_activate_task();
    assert(!kernel_files_release(&files) && !kernel_open_file_release(&control));
    kernel_page_cache_stop_worker(&cache);
    assert(!kernel_vfs_unmount(&mount) && !kernel_page_cache_destroy(&cache));
    printf("PASS synchronous fallocate: flags=%o cpu=%u mode=%u held actual flush, released operation/offset, durable target and reloaded bytes\n",flags,cpu,mode);
}
static void residual_orphan_shutdown(struct kernel_heap *heap,struct kernel_heap *backend_heap,
    struct physical_page_allocator *pages,struct kernel_block_device *disk)
{
    struct kernel_page_cache cache={0}; struct kernel_vfs_mount mount={0};
    assert(!kernel_page_cache_init(&cache,heap,pages) && !kernel_vfs_mount_ext4(&mount,disk,backend_heap,&cache,0));
    assert(!kernel_page_cache_start_worker(&cache) && !kernel_vfs_start_journal_worker(&mount));
    residual_adapter=lwext4_instance(mount.private_data);
    char path[96]; snprintf(path,sizeof(path),"%sresidual-shrink",residual_adapter->mount_point);
    ext4_file file; assert(!ext4_fopen(&file,path,"w+"));
    unsigned char bytes[16384]; memset(bytes,0x4d,sizeof(bytes)); size_t done;
    assert(!ext4_fwrite(&file,bytes,sizeof(bytes),&done) && done==sizeof(bytes));
    /* An accepted outer shrink legitimately leaves persisted orphan work;
     * no VFS node or external file owner remains to describe that work. */
    assert(!ext4_transaction_begin(residual_adapter->mount_point));
    assert(!ext4_ftruncate(&file,4096, UINT32_MAX));
    assert(!ext4_transaction_end(residual_adapter->mount_point) && !ext4_fclose(&file));
    shutdown_entered=1;
    assert(!kernel_vfs_unmount(&mount)); residual_adapter=NULL;
    assert(recovery_live_accepts && !kernel_page_cache_destroy(&cache));
    cache=(struct kernel_page_cache){0}; mount=(struct kernel_vfs_mount){0};
    assert(!kernel_page_cache_init(&cache,heap,pages) && !kernel_vfs_mount_ext4(&mount,disk,backend_heap,&cache,0));
    struct kernel_vfs_file restored={0}; struct kernel_vfs_stat stat;
    assert(!kernel_vfs_open(&mount,"/residual-shrink",&restored) && !kernel_vfs_fstat(&restored,&stat));
    assert(stat.size==4096 && stat.blocks==8);
    assert(!kernel_vfs_pread(&restored,0,bytes,4096,&done, NULL) && done==4096);
    for (size_t i=0;i<done;i++) assert(bytes[i]==0x4d);
    assert(!kernel_vfs_close(&restored) && !kernel_vfs_unmount(&mount) && !kernel_page_cache_destroy(&cache));
    puts("PASS residual orphan shutdown: cleanup accepted and checkpointed with live workers; final stop, remount, prefix and block return");
}
int main(int argc,char **argv)
{
    assert(argc==2); setvbuf(stdout,NULL,_IONBF,0); mm_test_attach(0); mm_test_activate_task();
    struct physical_page_allocator pages={0},backend_pages={0}; struct kernel_heap heap={0},backend_heap={0};
    initialize(&pages,&heap,arena,sizeof(arena)); initialize(&backend_pages,&backend_heap,backend,sizeof(backend));
    uint64_t baseline=physical_page_available(&pages),backend_baseline=physical_page_available(&backend_pages);
    struct arch_mmu_page_table kernel={0}; struct arch_mmu_user_space user={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages)); kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&user,&pages,&kernel) && !kernel_mm_create(&mm,&user) &&
        !kernel_mm_vma_enable(&mm,&heap) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    assert(!kernel_mm_context(&mm,&context)); mm_test_context=context;
    image=open(argv[1],O_RDWR); assert(image>=0); struct stat info; assert(!fstat(image,&info));
    struct kernel_block_device disk={.read=read_image,.write=write_image,.flush=fallocate_flush,
        .capacity_bytes=info.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    if (getenv("FALLOCATE_SYNC_RESIDUAL")) residual_orphan_shutdown(&heap,&backend_heap,&pages,&disk);
    else for (unsigned kind=0;kind<2;kind++) for (unsigned cpu=0;cpu<2;cpu++) for (unsigned punch=0;punch<2;punch++)
        synchronous_case(&heap,&backend_heap,&pages,&disk,kind?KERNEL_FILES_O_SYNC:KERNEL_FILES_O_DSYNC,cpu,punch?3:1);
    assert(!close(image)); mm_test_context=0;
    assert(!kernel_mm_release(&mm) && !physical_page_release(&pages,kernel.root_address));
    struct kernel_heap_statistics stat;
    kernel_heap_get_statistics(&heap,&stat); assert(!stat.live_allocations && !stat.current_pages);
    kernel_heap_get_statistics(&backend_heap,&stat); assert(!stat.live_allocations && !stat.current_pages);
    assert(!physical_page_allocator_audit(&pages) && !physical_page_allocator_audit(&backend_pages) &&
        physical_page_available(&pages)==baseline && physical_page_available(&backend_pages)==backend_baseline);
    puts("PASS synchronous fallocate owner return"); return 0;
}
