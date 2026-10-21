/* Real wait registration and commit owners; only the clock/execution gate is modeled. */
static struct lwext4_mount_adapter *notify_adapter;
static uint64_t notify_now,notify_deadline;
static unsigned notify_pause,notify_entered,notify_resume,notify_wakes[2];
static unsigned notify_submit_pause,notify_submit_entered,notify_submit_resume;
uint64_t __real_kernel_time_monotonic_ns(void);
uint64_t __wrap_kernel_time_monotonic_ns(void)
{
    return __atomic_load_n(&notify_adapter,__ATOMIC_ACQUIRE)?
        __atomic_load_n(&notify_now,__ATOMIC_RELAXED):__real_kernel_time_monotonic_ns();
}
enum kernel_scheduler_status __real_kernel_wait_queue_wake_all(struct kernel_wait_queue *);
enum kernel_scheduler_status __wrap_kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{
    struct lwext4_mount_adapter *adapter=__atomic_load_n(&notify_adapter,__ATOMIC_ACQUIRE);
    if(adapter && (queue==&adapter->journal_work || queue==&adapter->checkpoint_work))
        __atomic_fetch_add(&notify_wakes[queue==&adapter->checkpoint_work],1,__ATOMIC_RELAXED);
    return __real_kernel_wait_queue_wake_all(queue);
}
enum kernel_scheduler_status __real_kernel_wait_park(const struct kernel_wait_token *,enum kernel_wait_wake_reason *);
enum kernel_scheduler_status __wrap_kernel_wait_park(const struct kernel_wait_token *token,
    enum kernel_wait_wake_reason *reason)
{
    rename_inode_wait(token);
    struct lwext4_mount_adapter *adapter=__atomic_load_n(&notify_adapter,__ATOMIC_ACQUIRE);
    if(adapter && __atomic_load_n(&notify_pause,__ATOMIC_ACQUIRE)) {
        struct kernel_wait_queue *queue;
        uint64_t deadline;
        {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
         queue=kernel_wait_default_node(token->task)->queue;
         deadline=kernel_wait_record_of(token->task)->deadline;}
        if(queue==&adapter->journal_work && __atomic_exchange_n(&notify_pause,0,__ATOMIC_ACQ_REL)) {
            assert(!kernel_cpu_current()->raw_locks && !kernel_io_context_current()->locks);
            notify_deadline=deadline;
            unsigned entered=__atomic_add_fetch(&notify_entered,1,__ATOMIC_RELEASE);
            while(__atomic_load_n(&notify_resume,__ATOMIC_ACQUIRE)<entered)sched_yield();
        }
    }
    return __real_kernel_wait_park(token,reason);
}
int __real_jbd_journal_submit(struct jbd_journal *);
int __wrap_jbd_journal_submit(struct jbd_journal *journal)
{
    if(__atomic_exchange_n(&notify_submit_pause,0,__ATOMIC_ACQ_REL)) {
        assert(journal->committing && !kernel_io_context_current()->backend_state_depth);
        __atomic_store_n(&notify_submit_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&notify_submit_resume,__ATOMIC_ACQUIRE))sched_yield();
    }
    return __real_jbd_journal_submit(journal);
}
static void *notify_sync(void *argument)
{
    mm_test_attach(7);mm_test_activate_task();
    uint64_t observed;
    assert(!kernel_vfs_sync(argument,0,&observed));
    mm_test_finish();return NULL;
}
static void worker_notification_case(struct lwext4_mount_adapter *adapter,struct kernel_vfs_path *root)
{
    if(!worker_mode)return;
    struct kernel_vfs_file file={0};
    assert(!kernel_vfs_create_at(root,root,"worker-notify",0600,&file) &&
           !ext4_journal_group_drain(adapter->mount_point));
    while(!__atomic_load_n(&adapter->journal_work.head,__ATOMIC_ACQUIRE) ||
          !__atomic_load_n(&adapter->checkpoint_work.head,__ATOMIC_ACQUIRE))sched_yield();
    notify_now=1000;notify_pause=1;notify_entered=notify_resume=0;
    __atomic_store_n(&notify_adapter,adapter,__ATOMIC_RELEASE);
    {KERNEL_IRQ_SCOPE(irq);assert(!kernel_wait_queue_wake_all(&adapter->journal_work));}
    while(__atomic_load_n(&notify_entered,__ATOMIC_ACQUIRE)<1)sched_yield();
    assert(!notify_deadline);
    notify_wakes[0]=notify_wakes[1]=0;
    for(unsigned i=0;i<8;i++)assert(!kernel_vfs_file_set_mode(&file,0640+i));
    assert(notify_wakes[0]==1 && !notify_wakes[1]);
    struct jbd_journal *journal=adapter->device.fs->jbd_journal;
    assert(journal->running && journal->running->first_dirty_ns==notify_now);
    /* The first wake happens after prepare but before park. A surviving
     * notification must take the worker around to the first group's timer. */
    __atomic_store_n(&notify_pause,1,__ATOMIC_RELEASE);
    __atomic_store_n(&notify_resume,1,__ATOMIC_RELEASE);
    while(__atomic_load_n(&notify_entered,__ATOMIC_ACQUIRE)<2)sched_yield();
    assert(notify_deadline==notify_now+100000000);
    pthread_t syncing;assert(!pthread_create(&syncing,NULL,notify_sync,&file));
    uint64_t target=0;
    while(!target) {
        {KERNEL_RAW_SCOPE(guard,&adapter->journal_control);
         if(adapter->sync_waiters)target=adapter->sync_waiters->target;}
        if(!target)sched_yield();
    }
    assert(target==journal->accepted_sequence && notify_now<notify_deadline);
    notify_submit_entered=notify_submit_resume=0;notify_submit_pause=1;
    __atomic_store_n(&notify_resume,2,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&notify_submit_entered,__ATOMIC_ACQUIRE))sched_yield();
    assert(journal->committing->sequence==target);
    assert(!kernel_vfs_file_set_mode(&file,0600) && journal->accepted_sequence>target);
    __atomic_store_n(&notify_submit_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(syncing,NULL));
    adapter->locks.state_lock(adapter->locks.context);
    assert(journal->durable_sequence==target && journal->running && journal->running->sequence>target);
    adapter->locks.state_unlock(adapter->locks.context);
    __atomic_store_n(&notify_adapter,NULL,__ATOMIC_RELEASE);
    assert(!kernel_vfs_close(&file) && !kernel_vfs_unlink_at(root,root,"worker-notify",0) &&
           !ext4_journal_group_drain(adapter->mount_point));
    puts("PASS worker notifications: first dirty prepare/park wake survives, eight merges request only commit once, original 100ms deadline and immediate fixed-target fsync survive later acceptance");
}
