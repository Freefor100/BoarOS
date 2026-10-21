static unsigned serial_reservation_watch,serial_reserved,serial_borrowed,serial_commit_done;
static unsigned serial_log_saved;
static uint64_t serial_reservation_lba;
static unsigned serial_heap_failure, serial_heap_calls;
void *__real_ext4_user_malloc(size_t);
void *__real_ext4_user_calloc(size_t,size_t);
static int serial_heap_denied(void)
{ return serial_heap_failure && ++serial_heap_calls==serial_heap_failure; }
void *__wrap_ext4_user_malloc(size_t bytes)
{ return serial_heap_denied()?NULL:__real_ext4_user_malloc(bytes); }
void *__wrap_ext4_user_calloc(size_t count,size_t bytes)
{ return serial_heap_denied()?NULL:__real_ext4_user_calloc(count,bytes); }
int __real_jbd_trans_prepare_operation(struct jbd_trans *);
int __wrap_jbd_trans_prepare_operation(struct jbd_trans *trans)
{
    int result=__real_jbd_trans_prepare_operation(trans);
    if(result || serial_reservation_watch!=2)return result;
    assert(ext4_fs_serial_writer(trans->journal->jbd_fs->inode_ref.fs));
    assert(trans->journal->running && !trans->checkpoint_image);
    assert(!trans->revoke_count);
    struct jbd_buf *private;
    TAILQ_FOREACH(private,&trans->buf_queue,buf_node) {
        struct jbd_buf *accepted;
        TAILQ_FOREACH(accepted,&trans->journal->running->buf_queue,buf_node)
            if(accepted->block.lb_id==private->block.lb_id)break;
        assert(accepted);
    }
    /* The queued real committer cannot replace the group held by this
     * serial writer. Updating its existing inode needs no new wire node. */
    assert(!trans->reserved_logs && !trans->reserved_log_images);
    serial_log_saved++;
    return result;
}
static struct jbd_trans *serial_log_private(struct lwext4_mount_adapter *adapter,uint64_t lba)
{
    struct jbd_trans *trans;
    struct ext4_block block=EXT4_BLOCK_ZERO();
    assert(!jbd_journal_new_trans(adapter->device.fs->jbd_journal,&trans));
    assert(!ext4_block_get(&adapter->device,&block,lba));
    assert(!jbd_trans_get_write_access(trans,&block));
    /* Keep disk contents intact; the test owns an actual writable version
     * and journal record, including for free blocks at the disk's end. */
    assert(!jbd_trans_set_block_dirty(trans,&block));
    assert(!ext4_block_set(&adapter->device,&block));
    return trans;
}
static void serial_log_incremental(struct lwext4_mount_adapter *adapter)
{
    struct jbd_journal *journal=adapter->device.fs->jbd_journal;
    adapter->locks.lock(adapter->locks.context);
    assert(ext4_fs_serial_writer(adapter->device.fs));
    if(!journal->running) {
        struct jbd_trans *seed=serial_log_private(adapter,serial_reservation_lba);
        assert(!jbd_trans_prepare_operation(seed) && !jbd_journal_accept(journal,seed,0));
    }
    jbd_memory_set_cache_disabled(journal,true);jbd_trim_cache(journal,0);
    uint64_t sequence=journal->running->sequence;
    unsigned credits=journal->log_reserved;
    size_t baseline=jbd_memory_used(journal);
    struct jbd_trans *trans=serial_log_private(adapter,serial_reservation_lba);
    size_t used=jbd_memory_used(journal),limit=jbd_memory_limit(journal);
    jbd_memory_set_limit(journal,used);
    assert(!jbd_trans_prepare_operation(trans));
    assert(trans->reservation_group_sequence==sequence && !trans->reserved_logs &&
           !trans->reserved_log_images && !trans->checkpoint_image && jbd_memory_used(journal)==used);
    jbd_memory_set_limit(journal,limit);
    adapter->locks.unlock(adapter->locks.context);
    assert(jbd_journal_accept(journal,trans,0)==EAGAIN && journal->log_reserved==credits);
    adapter->locks.lock(adapter->locks.context);
    assert(!jbd_journal_accept(journal,trans,0) && journal->log_reserved==credits);
    assert(jbd_memory_used(journal)==baseline);
    trans=serial_log_private(adapter,serial_reservation_lba);
    assert(!jbd_trans_prepare_operation(trans) && trans->reservation_group_sequence==sequence);
    adapter->locks.unlock(adapter->locks.context);
    assert(!ext4_journal_group_commit(adapter->mount_point,true));
    adapter->locks.lock(adapter->locks.context);
    assert(!journal->running && jbd_journal_accept(journal,trans,0)==EAGAIN);
    jbd_journal_free_trans(journal,trans,true);
    assert(!journal->log_reserved);
    /* A new group owns its complete standalone reservation. Subsequent
     * distinct LBAs need one wire node, plus a descriptor at its boundary. */
    trans=serial_log_private(adapter,serial_reservation_lba);
    assert(!jbd_trans_prepare_operation(trans) && !trans->reservation_group_sequence);
    assert(!jbd_journal_accept(journal,trans,0));
    struct jbd_sb *wire=&journal->jbd_fs->sb;
    unsigned tag_bytes=sizeof(struct jbd_block_tag);
    if(JBD_HAS_INCOMPAT_FEATURE(wire,JBD_FEATURE_INCOMPAT_CSUM_V3))tag_bytes=sizeof(struct jbd_block_tag3);
    else {
        if(JBD_HAS_INCOMPAT_FEATURE(wire,JBD_FEATURE_INCOMPAT_CSUM_V2))tag_bytes+=sizeof(uint16_t);
        if(!JBD_HAS_INCOMPAT_FEATURE(wire,JBD_FEATURE_INCOMPAT_64BIT))tag_bytes-=sizeof(uint32_t);
    }
    unsigned tail=(JBD_HAS_INCOMPAT_FEATURE(wire,JBD_FEATURE_INCOMPAT_CSUM_V2) ||
        JBD_HAS_INCOMPAT_FEATURE(wire,JBD_FEATURE_INCOMPAT_CSUM_V3))?sizeof(struct jbd_block_tail):0;
    unsigned tags=(journal->block_size-sizeof(struct jbd_bhdr)-tail-UUID_SIZE)/tag_bytes;
    unsigned boundary=0,changed_shape=0;
    for(unsigned index=0;index<512 && !boundary;index++) {
        uint64_t lba=adapter->device.lg_bcnt-1-index;
        struct jbd_trans *pending=NULL;
        struct ext4_buf *pending_home=NULL;
        struct ext4_block_payload *pending_accepted=NULL;
        if((unsigned)journal->running->data_cnt==tags-1) {
            pending=serial_log_private(adapter,lba-1);
            assert(!jbd_trans_prepare_operation(pending) && pending->reserved_logs==1 &&
                   !pending->reserved_log_images);
            pending_home=TAILQ_FIRST(&pending->buf_queue)->block.buf;
            pending_accepted=pending_home->current_payload;
        }
        baseline=jbd_memory_used(journal);credits=journal->log_reserved;
        unsigned images=journal->running->reserved_log_images;
        trans=serial_log_private(adapter,lba);used=jbd_memory_used(journal);
        assert(!jbd_trans_prepare_operation(trans));
        assert(trans->reservation_group_sequence==journal->running->sequence &&
               trans->reserved_logs==1+trans->reserved_log_images && trans->reserved_log_images<=1);
        if(trans->reserved_log_images) {
            boundary=1;
            size_t charge=jbd_memory_used(journal)-used;
            assert(charge && journal->log_reserved==credits+2);
            jbd_journal_free_trans(journal,trans,true);
            assert(journal->log_reserved==credits && jbd_memory_used(journal)==baseline);
            trans=serial_log_private(adapter,lba);used=jbd_memory_used(journal);
            uint64_t denials=jbd_memory_budget_failures(journal);
            jbd_memory_set_limit(journal,used+charge-1);
            assert(jbd_trans_prepare_operation(trans)==ENOMEM &&
                   jbd_memory_budget_failures(journal)>denials);
            jbd_memory_set_limit(journal,limit);
            jbd_journal_free_trans(journal,trans,true);
            assert(journal->log_reserved==credits && jbd_memory_used(journal)==baseline);
            for(unsigned failure=1;failure<=4;failure++) {
                trans=serial_log_private(adapter,lba);
                denials=jbd_memory_budget_failures(journal);
                serial_heap_failure=failure;serial_heap_calls=0;
                int result=jbd_trans_prepare_operation(trans);
                serial_heap_failure=0;
                assert(result==ENOMEM && serial_heap_calls==failure &&
                       jbd_memory_budget_failures(journal)==denials);
                jbd_journal_free_trans(journal,trans,true);
                assert(journal->log_reserved==credits && jbd_memory_used(journal)==baseline);
            }
            trans=serial_log_private(adapter,lba);used=jbd_memory_used(journal);
            jbd_memory_set_limit(journal,used+charge);
            assert(!jbd_trans_prepare_operation(trans));
            jbd_memory_set_limit(journal,limit);
        }
        assert(!jbd_journal_accept(journal,trans,0));
        assert(journal->running->reserved_log_images==images+boundary);
        if(pending) {
            assert(pending->reservation_group_sequence==journal->running->sequence);
            assert(jbd_journal_accept(journal,pending,0)==EAGAIN &&
                   pending_home->current_payload==pending_accepted);
            jbd_journal_free_trans(journal,pending,true);changed_shape++;
        }
    }
    assert(boundary && changed_shape);
    /* A duplicate revoke already has a wire entry in this same group. */
    for(unsigned duplicate=0;duplicate<2;duplicate++) {
        assert(!jbd_journal_new_trans(journal,&trans));
        assert(!jbd_trans_revoke_block(trans,adapter->device.lg_bcnt-1));
        assert(!jbd_trans_prepare_operation(trans));
        if(duplicate)assert(!trans->reserved_logs && !trans->reserved_log_images);
        assert(!jbd_journal_accept(journal,trans,0) && journal->running->revoke_count==1);
    }
    jbd_memory_set_cache_disabled(journal,false);
    adapter->locks.unlock(adapter->locks.context);
    puts("PASS serial log delta: real qualification/target loss, same-sequence shape change, duplicate LBA/revoke, descriptor boundary, exact quota and four actual OOM prefixes return owners");
}
int __real_jbd_trans_get_write_access_range(struct jbd_trans *,struct ext4_block *,uint32_t,uint32_t);
int __wrap_jbd_trans_get_write_access_range(struct jbd_trans *trans,struct ext4_block *block,
    uint32_t offset,uint32_t length)
{
    int result=__real_jbd_trans_get_write_access_range(trans,block,offset,length);
    if(result || !serial_reservation_watch)return result;
    struct jbd_buf *record;
    TAILQ_FOREACH(record,&trans->buf_queue,buf_node)if(record->block.lb_id==block->lb_id)break;
    assert(record && record->private_access && record->after && record->block.payload &&
        record->block.payload!=&record->after->payload &&
        block->payload==&record->after->payload && block->data==record->after->data);
    if(serial_reservation_watch==1) {
        assert(ext4_fs_serial_writer(trans->operation.fs));
        if(record->reserved_rec)serial_reserved++;
    } else if(block->lb_id==serial_reservation_lba) {
        if(serial_reservation_watch==2) {
            assert(ext4_fs_serial_writer(trans->operation.fs) && !record->reserved_rec);
            serial_borrowed++;
        } else {
            assert(!ext4_fs_serial_writer(trans->operation.fs) && record->reserved_rec);
            serial_reserved++;
        }
    }
    return result;
}
static void *serial_commit(void *argument)
{
    mm_test_attach(8);mm_test_activate_task();
    struct lwext4_mount_adapter *adapter=argument;
    assert(!ext4_journal_group_commit(adapter->mount_point,true));
    __atomic_store_n(&serial_commit_done,1,__ATOMIC_RELEASE);
    mm_test_finish();return NULL;
}
static void serial_reservation_case(struct lwext4_mount_adapter *adapter,int serial)
{
    char path[96];snprintf(path,sizeof(path),"%sserial-reservation",adapter->mount_point);
    ext4_file file;
    if(serial)serial_reservation_watch=1;
    assert(!ext4_fopen(&file,path,serial?"w+":"r+"));
    serial_reservation_watch=0;
    if(serial)assert(serial_reserved);
    adapter->locks.read_lock(adapter->locks.context);
    struct ext4_inode_ref inode;
    assert(!ext4_fs_get_inode_ref(adapter->device.fs,file.inode,&inode));
    serial_reservation_lba=inode.block.lb_id;
    assert(!ext4_fs_put_inode_ref(&inode));
    adapter->locks.unlock(adapter->locks.context);
    pthread_t worker;
    if(serial) {
        adapter->locks.lock(adapter->locks.context);
        assert(ext4_fs_serial_writer(adapter->device.fs));
        serial_commit_done=0;
        assert(!pthread_create(&worker,NULL,serial_commit,adapter));
        for(;;) {
            unsigned waiting;
            {KERNEL_RAW_SCOPE(guard,&adapter->backend_lock.serial.metadata);
             waiting=adapter->backend_lock.serial.pending_head!=NULL;}
            if(waiting)break;
            assert(!__atomic_load_n(&serial_commit_done,__ATOMIC_ACQUIRE));sched_yield();
        }
    }
    serial_reservation_watch=serial?2:3;serial_reserved=serial_borrowed=serial_log_saved=0;
    struct ext4_timestamp times[3]={{11223+serial,1},{44556,2},{77889,3}};
    assert(!ext4_file_set_times(&file,7,times));
    serial_reservation_watch=0;
    assert(serial?serial_borrowed:serial_reserved);
    if(serial) {
        assert(serial_log_saved);
        assert(!__atomic_load_n(&serial_commit_done,__ATOMIC_ACQUIRE));
        adapter->locks.unlock(adapter->locks.context);
        assert(!pthread_join(worker,NULL) && serial_commit_done);
        serial_log_incremental(adapter);
    }
    assert(!ext4_fclose(&file));
    if(!serial)assert(!ext4_fremove(path) && !ext4_journal_group_drain(adapter->mount_point));
    puts(serial?
        "PASS actual serial writer borrows indexed LBA record; missing records retain candidates, queued real commit cannot freeze until writer release":
        "PASS parallel operation retains its own LBA candidate without serial writer qualification; baseline stays referenced and private replacement remains owned");
}
