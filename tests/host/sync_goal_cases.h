static void datasync_times(struct kernel_heap *heap,struct physical_page_allocator *pages,const char *path)
{
    struct kernel_page_cache cache={0};assert(!kernel_page_cache_init(&cache,heap,pages));
    current_cache=&cache;image_fd=open(path,O_RDWR);assert(image_fd>=0);
    struct stat st;assert(!fstat(image_fd,&st));
    struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
        .capacity_bytes=st.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};struct kernel_vfs_file file={0};size_t written;
    assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,0) &&
        !kernel_vfs_create(&mount,"/data-times",0600,&file) &&
        !kernel_vfs_pwrite(&file,0,"data",4,&written) && written==4);
    uint64_t observed=kernel_vfs_error_sequence(&file);
    assert(!kernel_vfs_sync(&file,0,&observed));
    assert(!kernel_vfs_file_modified(&file,0,0));
    struct lwext4_mount_adapter *adapter=mount.private_data;
    uint32_t committed=adapter->device.fs->jbd_journal->committed_id;
    times_sync_file=lwext4_node_file(kernel_vfs_file_node(&file));times_sync_calls=0;
    assert(!kernel_vfs_sync(&file,1,&observed));
    assert(!times_sync_calls && adapter->device.fs->jbd_journal->committed_id==committed);
    assert(!kernel_vfs_sync(&file,0,&observed) && times_sync_calls==1 &&
        adapter->device.fs->jbd_journal->committed_id!=committed);
    times_sync_file=NULL;
    assert(!kernel_vfs_close(&file) && !kernel_vfs_unmount(&mount) &&
        !kernel_page_cache_destroy(&cache) && !close(image_fd));current_cache=NULL;
    puts("PASS fdatasync leaves independent pending timestamps for fsync; no extra timestamp transaction");
}

struct fixed_target_sync { ext4_file *file;struct lwext4_mount_adapter *reclaim;int result; };
static void *fixed_target_sync_client(void *argument)
{
    struct fixed_target_sync *sync=argument;cache_test_attach(6);
    fixed_target_client=1;
    sync->result=sync->reclaim ? sync->reclaim->instance.ops->writeback_reclaim(&sync->reclaim->instance) : ext4_file_sync_metadata_mode(sync->file,true);
    fixed_target_client=0;return NULL;
}
static void durable_fixed_target(struct kernel_heap *heap,struct physical_page_allocator *pages,const char *path)
{
    struct kernel_page_cache cache={0};assert(!kernel_page_cache_init(&cache,heap,pages));
    current_cache=&cache;image_fd=open(path,O_RDWR);assert(image_fd>=0);
    struct stat st;assert(!fstat(image_fd,&st));
    struct kernel_block_device disk={.read=image_read,.write=image_write,.flush=image_flush,
        .capacity_bytes=st.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    struct kernel_vfs_mount mount={0};struct kernel_vfs_file writer={0},other={0};size_t written;
    assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,0) &&
        !kernel_vfs_create(&mount,"/fixed-writer",0600,&writer) &&
        !kernel_vfs_create(&mount,"/fixed-other",0600,&other));
    uint64_t observed=kernel_vfs_mount_error_sequence(&mount);
    assert(!kernel_vfs_sync_mount(&mount,&observed));
    struct lwext4_mount_adapter *adapter=mount.private_data;
    journal_wait_owner=adapter;
    assert(!kernel_vfs_start_journal_worker(&mount) && !kernel_page_cache_start_worker(&cache));
    assert(!kernel_vfs_pwrite(&writer,0,"data",4,&written) && written==4);
    durable_wait=(__typeof__(durable_wait)){.armed=1};
    struct executor_sync syncing={.file=&writer,.observed=kernel_vfs_error_sequence(&writer)};
    pthread_t first,late;assert(!pthread_create(&first,NULL,executor_sync_client,&syncing));
    wait_progress(&durable_wait.flush_entered,1);
    /* The first group owns DMA; a second accepted metadata group is still
     * inside its normal batching interval when an old target waiter joins. */
    assert(!kernel_vfs_file_set_mode(&other,0640));
    struct ext4_journal_progress progress;
    assert(!ext4_journal_group_progress(adapter->mount_point,&progress) && progress.accepted>progress.sealed);
    fixed_target_waiting=0;
    struct fixed_target_sync target={.file=lwext4_node_file(kernel_vfs_file_node(&writer))};
    assert(!pthread_create(&late,NULL,fixed_target_sync_client,&target));
    wait_progress(&fixed_target_waiting,1);
    { KERNEL_TASK_LOCK_SCOPE(guard,&adapter->journal_control);
      assert(!adapter->journal_force && "old fixed target must not force the later unrelated running group"); }
    /* Admission needs one real retirement, not a drain of the unrelated
     * accepted group created after the blocked DMA began. */
    fixed_target_waiting=0;
    struct fixed_target_sync reclaim={.reclaim=adapter};pthread_t reclaiming;
    assert(!pthread_create(&reclaiming,NULL,fixed_target_sync_client,&reclaim));
    wait_progress(&fixed_target_waiting,1);
    ext4_fs_state_read_lock(adapter->device.fs);
    assert(adapter->device.fs->jbd_journal->seal_target<=progress.sealed);
    ext4_fs_state_unlock(adapter->device.fs);
    __atomic_store_n(&durable_wait.resume_flush,1,__ATOMIC_RELEASE);
    assert(!pthread_join(reclaiming,NULL) && !reclaim.result);
    assert(!pthread_join(first,NULL) && !syncing.result && !pthread_join(late,NULL) && !target.result);
    durable_wait=(__typeof__(durable_wait)){0};
    assert(!kernel_vfs_sync_mount(&mount,&observed) &&
        !ext4_journal_group_progress(adapter->mount_point,&progress) && progress.durable==progress.accepted);
    kernel_page_cache_stop_worker(&cache);
    assert(!kernel_vfs_close(&writer) && !kernel_vfs_close(&other) && !kernel_vfs_unmount(&mount) &&
        !kernel_page_cache_destroy(&cache) && !close(image_fd));current_cache=NULL;journal_wait_owner=NULL;
    puts("PASS fixed old durable target joins existing commit without forcing the later unrelated group; syncfs persists the mount target");
}
