#include <ext4_dir.h>

static unsigned serial_reservation_watch,serial_reserved,serial_borrowed,serial_commit_done;
static unsigned serial_log_saved;
static unsigned serial_bitmap_patches;
static unsigned serial_super_mode;
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
    if (!result && serial_super_mode) {
        assert(!!trans->reused_super_sequence == (serial_super_mode != 2));
        if (serial_super_mode == 1) return ECANCELED;
    }
    if(result || !serial_reservation_watch)return result;
    bool serial=ext4_fs_serial_writer(trans->journal->jbd_fs->inode_ref.fs);
    assert(serial==(serial_reservation_watch!=3));
    struct jbd_buf *record;
    TAILQ_FOREACH(record,&trans->buf_queue,buf_node) {
        assert(record->private_access && record->modified);
        if (serial_reservation_watch==1 &&
            (record->checked_tree&7U)==EXT4_TREE_INODE_BITMAP) {
            unsigned words=(trans->journal->block_size/32U+63U)/64U, units=0;
            for (unsigned i=0;i<words;i++)
                units+=(unsigned)__builtin_popcountll(record->dirty_ranges[words+i]);
            assert(units==1);
            serial_bitmap_patches++;
        }
        if(serial_reservation_watch==1 || record->block.lb_id==serial_reservation_lba) {
            if(record->reserved_rec)serial_reserved++;
            else {assert(record->block_rec && record->block_rec->private_refs);serial_borrowed++;}
        }
    }
    if(serial_reservation_watch!=2)return result;
    assert(trans->journal->running && !trans->revoke_count);
    TAILQ_FOREACH(record,&trans->buf_queue,buf_node) {
        struct jbd_buf *accepted;
        TAILQ_FOREACH(accepted,&trans->journal->running->buf_queue,buf_node)
            if(accepted->block.lb_id==record->block.lb_id)break;
        assert(accepted);
    }
    /* The actual serial writer keeps the running group while using its
     * existing records and credits, including across lock-free preparation. */
    assert(!trans->reserved_logs);
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
           jbd_memory_used(journal)==used);
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
    unsigned boundary=0,changed_shape=0,heap_prefixes=0;
    for(unsigned index=0;index<512 && !boundary;index++) {
        uint64_t lba=adapter->device.lg_bcnt-1-index;
        struct jbd_trans *pending=NULL;
        struct ext4_buf *pending_home=NULL;
        struct ext4_block_payload *pending_accepted=NULL;
        if((unsigned)journal->running->data_cnt==tags-1) {
            pending=serial_log_private(adapter,lba-1);
            assert(!jbd_trans_prepare_operation(pending) && pending->reserved_logs==1);
            pending_home=TAILQ_FIRST(&pending->buf_queue)->block.buf;
            pending_accepted=pending_home->current_payload;
        }
        baseline=jbd_memory_used(journal);credits=journal->log_reserved;
        unsigned old_logs=journal->running->reserved_logs;
        trans=serial_log_private(adapter,lba);used=jbd_memory_used(journal);
        serial_heap_failure=UINT32_MAX;serial_heap_calls=0;
        assert(!jbd_trans_prepare_operation(trans));
        serial_heap_failure=0;
        unsigned allocations=serial_heap_calls;
        assert(trans->reservation_group_sequence==journal->running->sequence &&
               (trans->reserved_logs==1 || trans->reserved_logs==2));
        if(trans->reserved_logs==2) {
            boundary=1;
            heap_prefixes=allocations;assert(heap_prefixes);
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
            /* Count this real descriptor-boundary preparation first: batching
             * changes heap calls, but every remaining failure must roll back. */
            for(unsigned failure=1;failure<=heap_prefixes;failure++) {
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
        assert(journal->running->reserved_logs==old_logs+1+boundary);
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
        if(duplicate)assert(!trans->reserved_logs);
        assert(!jbd_journal_accept(journal,trans,0) && journal->running->revoke_count==1);
    }
    /* The operation fits alone, but the current group's union does not.
     * Only this reclaimable failure should ask the caller to drain and retry. */
    uint32_t maxlen=wire->maxlen;
    unsigned capacity=journal->running->reserved_logs+1;
    assert(capacity>4);
    wire->maxlen=to_be32(journal->first+capacity);
    credits=journal->log_reserved;baseline=jbd_memory_used(journal);
    trans=serial_log_private(adapter,adapter->device.lg_bcnt-900);
    assert(jbd_trans_prepare_operation(trans)==ENOSPC &&
        trans->operation.log_reservation_failed);
    jbd_journal_free_trans(journal,trans,true);
    assert(journal->log_reserved==credits && jbd_memory_used(journal)==baseline);
    wire->maxlen=maxlen;
    jbd_memory_set_cache_disabled(journal,false);
    adapter->locks.unlock(adapter->locks.context);
    printf("PASS serial log delta: real qualification/target loss, same-sequence shape change, duplicate LBA/revoke, descriptor boundary, exact quota, reclaimable group overflow and %u actual OOM prefixes return owners\n",heap_prefixes);
}
static int check_private_grant(struct jbd_trans *trans, struct ext4_block *block, int result)
{
    if(result || !serial_reservation_watch)return result;
    struct jbd_buf *record;
    TAILQ_FOREACH(record,&trans->buf_queue,buf_node)if(record->block.lb_id==block->lb_id)break;
    assert(record && record->private_access && record->after && record->block.payload &&
        record->block.payload!=&record->after->payload &&
        block->payload==&record->after->payload && block->data==record->after->data);
    assert(!record->block_rec && !record->reserved_rec);
    return result;
}
int __real_jbd_trans_get_write_access_range(struct jbd_trans *,struct ext4_block *,uint32_t,uint32_t);
int __wrap_jbd_trans_get_write_access_range(struct jbd_trans *trans,struct ext4_block *block,
    uint32_t offset,uint32_t length)
{
    return check_private_grant(trans,block,
        __real_jbd_trans_get_write_access_range(trans,block,offset,length));
}
int __real_jbd_trans_prepare_unowned_range(struct jbd_trans *,struct ext4_block *,uint32_t,uint32_t);
int __wrap_jbd_trans_prepare_unowned_range(struct jbd_trans *trans,struct ext4_block *block,
    uint32_t offset,uint32_t length)
{
    return check_private_grant(trans,block,
        __real_jbd_trans_prepare_unowned_range(trans,block,offset,length));
}
static void *serial_commit(void *argument)
{
    mm_test_attach(8);mm_test_activate_task();
    struct lwext4_mount_adapter *adapter=argument;
    assert(!ext4_journal_group_commit(adapter->mount_point,true));
    __atomic_store_n(&serial_commit_done,1,__ATOMIC_RELEASE);
    mm_test_finish();return NULL;
}
static void serial_leaf_readers(struct lwext4_mount_adapter *adapter)
{
    struct ext4_fs *fs=adapter->device.fs;
    struct ext4_inode_ref root;
    ext4_fsblk_t lba;
    struct ext4_block old, latest;
    adapter->locks.read_lock(adapter->locks.context);
    assert(!ext4_fs_get_inode_ref_read(fs,EXT4_ROOT_INO,&root));
    assert(!ext4_fs_get_inode_dblk_idx(&root,0,&lba,false));
    assert(!ext4_block_get(fs->bdev,&old,lba));
    assert(!ext4_fs_put_inode_ref(&root));
    adapter->locks.unlock(adapter->locks.context);

    static const char name[]="serial-leaf-reader";
    char path[96];
    snprintf(path,sizeof(path),"%s%s",adapter->mount_point,name);
    ext4_file file;
    directory_leaf_abort=true;
    assert(ext4_fopen(&file,path,"w+")==ECANCELED);
    directory_leaf_abort=false;
    assert(ext4_fopen(&file,path,"r")==ENOENT);
    assert(!ext4_fopen(&file,path,"w+") && !ext4_fclose(&file));

    adapter->locks.read_lock(adapter->locks.context);
    assert(!ext4_fs_get_inode_ref_read(fs,EXT4_ROOT_INO,&root));
    assert(!ext4_block_get(fs->bdev,&latest,lba));
    assert(latest.payload->directory_layout != 0 && latest.payload->directory_layout != UINT32_MAX);
    struct ext4_dir_search_result found;
    assert(ext4_dir_find_in_block(&old,&fs->sb,sizeof(name)-1,name,&found)==ENOENT);
    assert(!ext4_dir_find_in_block(&latest,&fs->sb,sizeof(name)-1,name,&found));
    assert(old.payload!=latest.payload &&
        ext4_dir_csum_verify(&root,(void *)old.data) && ext4_dir_csum_verify(&root,(void *)latest.data));
    assert(!ext4_block_set(fs->bdev,&latest) && !ext4_block_set(fs->bdev,&old));
    assert(!ext4_fs_put_inode_ref(&root));
    adapter->locks.unlock(adapter->locks.context);
    assert(!ext4_fremove(path));
    puts("PASS sparse directory edit: rollback preserves absence and both views retain valid checksums");
}

static size_t serial_leaf_offset(struct lwext4_mount_adapter *adapter, const char *name)
{
    struct ext4_fs *fs = adapter->device.fs;
    struct ext4_inode_ref root;
    struct ext4_dir_search_result found;
    adapter->locks.read_lock(adapter->locks.context);
    assert(!ext4_fs_get_inode_ref_read(fs, EXT4_ROOT_INO, &root));
    assert(!ext4_dir_find_entry(&found, &root, name, strlen(name), false));
    size_t offset = (uint8_t *)found.dentry - found.block.data;
    assert(ext4_dir_csum_verify(&root, (void *)found.block.data));
    assert(!ext4_dir_destroy_result(&root, &found));
    assert(!ext4_fs_put_inode_ref(&root));
    adapter->locks.unlock(adapter->locks.context);
    return offset;
}

static void serial_leaf_first_fit(struct lwext4_mount_adapter *adapter)
{
    char long_name[200];
    memset(long_name, 'w', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    const char *names[] = {"gap", "anchor", long_name, "fit"};
    char paths[4][256];
    ext4_file file;
    for (unsigned i = 0; i < 4; i++)
        snprintf(paths[i], sizeof(paths[i]), "%s%s", adapter->mount_point, names[i]);
    assert(!ext4_fopen(&file, paths[0], "w+") && !ext4_fclose(&file));
    assert(!ext4_fopen(&file, paths[1], "w+") && !ext4_fclose(&file));
    size_t gap = serial_leaf_offset(adapter, names[0]);
    assert(!ext4_fremove(paths[0]));
    assert(!ext4_fopen(&file, paths[2], "w+") && !ext4_fclose(&file));
    assert(serial_leaf_offset(adapter, names[2]) > gap);
    assert(!ext4_fopen(&file, paths[3], "w+") && !ext4_fclose(&file));
    assert(serial_leaf_offset(adapter, names[3]) == gap);
    for (unsigned i = 1; i < 4; i++) assert(!ext4_fremove(paths[i]));
    puts("PASS sparse directory first-fit: a long insertion leaves the earlier short hole reusable");
}

static void serial_reservation_case(struct lwext4_mount_adapter *adapter,int serial)
{
    char path[96];snprintf(path,sizeof(path),"%sserial-reservation",adapter->mount_point);
    ext4_file file;
    if(serial) {
        serial_reservation_watch=1;
        serial_bitmap_patches=0;
        directory_leaf_watch=1;
        directory_leaf_updates=directory_leaf_bytes=0;
    }
    assert(!ext4_fopen(&file,path,serial?"w+":"r+"));
    serial_reservation_watch=0;
    if(serial) {
        directory_leaf_watch=0;
        assert(serial_reserved);
        assert(serial_bitmap_patches==1);
        printf("existing leaf insert journals %u bytes in %u ranges\n",
            directory_leaf_bytes,directory_leaf_updates);
        assert(directory_leaf_updates==2 && directory_leaf_bytes<=128);
        struct ext4_fs *fs = adapter->device.fs;
        uint64_t lba = EXT4_SUPERBLOCK_OFFSET / adapter->device.lg_bsize;
        unsigned offset = EXT4_SUPERBLOCK_OFFSET % adapter->device.lg_bsize;
        uint32_t available = ext4_get32(&fs->sb, free_inodes_count);
        char probe[96];
        snprintf(probe,sizeof(probe),"%ssuper-reuse",adapter->mount_point);
        ext4_file opened;
        serial_super_mode = 1;
        assert(ext4_fopen(&opened,probe,"w+") == ECANCELED);
        serial_super_mode = 0;
        assert(ext4_get32(&fs->sb, free_inodes_count) == available);
        assert(ext4_fopen(&opened,probe,"r") == ENOENT);

        struct ext4_block held = EXT4_BLOCK_ZERO();
        assert(!ext4_block_get(fs->bdev, &held, lba));
        struct ext4_sblock *old = (void *)(held.data + offset);
        for (uint32_t mask = 1; mask; mask <<= 1) {
            uint32_t count = available ^ mask;
            struct ext4_sblock changed = *old;
            ext4_set32(&changed, free_inodes_count, count);
            ext4_sb_set_csum(&changed);
            assert(ext4_sb_inode_count_csum(old, count) == changed.checksum);
        }
        serial_super_mode = 2;
        assert(!ext4_fopen(&opened,probe,"w+") && !ext4_fclose(&opened));
        serial_super_mode = 0;
        assert(ext4_get32(old, free_inodes_count) == available && ext4_sb_check(old));
        assert(!ext4_block_set(fs->bdev, &held));
        assert(!ext4_fremove(probe));

        serial_super_mode = 3;
        assert(!ext4_fopen(&opened,probe,"w+") && !ext4_fclose(&opened));
        serial_super_mode = 0;
        struct ext4_block current = EXT4_BLOCK_ZERO();
        assert(!ext4_block_get(fs->bdev, &current, lba));
        struct ext4_sblock *accepted = (void *)(current.data + offset);
        assert(ext4_sb_check(accepted) && ext4_get32(accepted, free_inodes_count) == available - 1);
        assert(!ext4_block_set(fs->bdev, &current));
        assert(!ext4_fremove(probe));
        puts("PASS running superblock reuse: abort, held-reader replacement and accepted checksum");
        serial_leaf_readers(adapter);
        serial_leaf_first_fit(adapter);
    }
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
    assert(serial?serial_borrowed:serial_borrowed+serial_reserved);
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
        "PASS parallel operation owns an absent-LBA candidate or indexed-record borrow; baseline and private replacement remain owned without a serial writer");
}
