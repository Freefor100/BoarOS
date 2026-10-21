/* Included by the real journal fixture; only heap entrypoints are wrapped. */
#include <errno.h>
#include <pthread.h>

static pthread_mutex_t credit_state = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t credit_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t credit_changed = PTHREAD_COND_INITIALIZER;
static _Thread_local unsigned credit_actor, credit_state_depth, credit_state_entries;
static _Thread_local bool credit_encoding;
static struct {
    unsigned checksums, state_checksums;
    bool pause, paused, resume;
} encoding_test;
uint32_t __real_ext4_crc32c(uint32_t, const void *, uint32_t);
uint32_t __wrap_ext4_crc32c(uint32_t crc, const void *bytes, uint32_t length)
{
    if (credit_encoding) {
        encoding_test.checksums++;
        encoding_test.state_checksums += credit_state_depth != 0;
        if (encoding_test.pause && !encoding_test.paused) {
            CHECK(!pthread_mutex_lock(&credit_gate));
            encoding_test.paused = true;
            CHECK(!pthread_cond_broadcast(&credit_changed));
            while (!encoding_test.resume)
                CHECK(!pthread_cond_wait(&credit_changed, &credit_gate));
            CHECK(!pthread_mutex_unlock(&credit_gate));
        }
    }
    return __real_ext4_crc32c(crc, bytes, length);
}
static struct {
    struct jbd_journal *journal;
    unsigned baseline, fail_after, calls, paused_credits;
    bool paused, resume, blocked, finished;
} credit_test;

static void credit_state_lock(void *context)
{
    (void)context;
    if (credit_state_depth++) return;
    credit_state_entries++;
    if (credit_actor == 2) {
        int result = pthread_mutex_trylock(&credit_state);
        if (!result) return;
        CHECK(result == EBUSY);
        CHECK(!pthread_mutex_lock(&credit_gate));
        credit_test.blocked = true;
        CHECK(!pthread_cond_broadcast(&credit_changed));
        CHECK(!pthread_mutex_unlock(&credit_gate));
    }
    CHECK(!pthread_mutex_lock(&credit_state));
}
static void credit_state_unlock(void *context)
{
    (void)context;
    CHECK(credit_state_depth);
    if (!--credit_state_depth) CHECK(!pthread_mutex_unlock(&credit_state));
}

static bool credit_heap_failure(void)
{
    if (credit_actor != 1 || !credit_test.journal) return false;
    /* A is the only preparer until this handshake. Its first heap request
     * after the complete credit delta was claimed is the pause point. */
    if (!credit_test.paused &&
        credit_test.journal->log_reserved > credit_test.baseline) {
        CHECK(!pthread_mutex_lock(&credit_gate));
        credit_test.paused_credits = credit_test.journal->log_reserved;
        credit_test.paused = true;
        CHECK(!pthread_cond_broadcast(&credit_changed));
        while (!credit_test.resume)
            CHECK(!pthread_cond_wait(&credit_changed, &credit_gate));
        CHECK(!pthread_mutex_unlock(&credit_gate));
    }
    return credit_test.paused && ++credit_test.calls == credit_test.fail_after;
}
void *__real_calloc(size_t, size_t);
void *__real_malloc(size_t);
void *__wrap_calloc(size_t count, size_t bytes)
{ return credit_heap_failure() ? NULL : __real_calloc(count, bytes); }
void *__wrap_malloc(size_t bytes)
{ return credit_heap_failure() ? NULL : __real_malloc(bytes); }

struct credit_call {
    struct ext4_blockdev *dev;
    struct jbd_journal *journal;
    struct jbd_trans *trans;
    struct ext4_block block;
    bool private_access, freeze;
    int result;
    unsigned reserved;
};
static void *credit_prepare(void *argument)
{
    struct credit_call *call = argument;
    credit_actor = 1;
    call->result = jbd_trans_prepare_operation(call->trans);
    credit_actor = 0;
    return NULL;
}
static void *credit_compete(void *argument)
{
    struct credit_call *call = argument;
    credit_actor = 2;
    struct jbd_trans *trans;
    CHECK(jbd_journal_new_trans(call->journal, &trans) == EOK);
    CHECK(!trans->reserved_logs && !trans->reserved_log_images && !trans->checkpoint_image);
    if (call->freeze) {
        CHECK(jbd_trans_revoke_block(trans, call->dev->lg_bcnt - 2) == EOK);
        CHECK(jbd_trans_prepare_operation(trans)==EOK);
        CHECK(trans->reserved_logs == 2 && trans->reserved_log_images == 2);
        CHECK(jbd_journal_accept(call->journal, trans, 0) == EOK);
        CHECK(jbd_journal_freeze(call->journal) == EOK);
        CHECK(trans->frozen && !trans->alloc_blocks && trans->checkpoint_image);
        CHECK(trans->reserved_logs == 2 && trans->reserved_log_images == 2);
    } else jbd_journal_free_trans(call->journal, trans, true);
    call->reserved = call->journal->log_reserved;
    CHECK(!pthread_mutex_lock(&credit_gate));
    credit_test.finished = true;
    CHECK(!pthread_cond_broadcast(&credit_changed));
    CHECK(!pthread_mutex_unlock(&credit_gate));
    credit_actor = 0;
    return NULL;
}
static void credit_drain(struct jbd_journal *journal)
{
    while (journal->sealed_count) {
        CHECK(jbd_journal_select_stage(journal, JBD_JOURNAL_COMMIT) == JBD_JOURNAL_COMMIT);
        CHECK(jbd_journal_submit(journal) == EOK);
        CHECK(jbd_journal_retire_stage(journal, JBD_JOURNAL_COMMIT, EOK) == EOK);
    }
    journal->checkpoint_target = journal->accepted_sequence;
    while (!TAILQ_EMPTY(&journal->cp_queue)) {
        CHECK(jbd_journal_select_stage(journal, JBD_JOURNAL_CHECKPOINT) == JBD_JOURNAL_CHECKPOINT);
        CHECK(jbd_journal_checkpoint(journal) == EOK);
        CHECK(jbd_journal_retire_stage(journal, JBD_JOURNAL_CHECKPOINT, EOK) == EOK);
    }
}
static struct jbd_trans *encoding_accept(struct ext4_blockdev *dev,
    struct jbd_journal *journal, ext4_fsblk_t home, unsigned offset, char byte)
{
    struct jbd_trans *trans;
    struct ext4_block block;
    CHECK(jbd_journal_new_trans(journal, &trans) == EOK);
    CHECK(ext4_block_get(dev, &block, home) == EOK);
    CHECK(jbd_trans_get_write_access_range(trans, &block, offset, 32) == EOK);
    block.data[offset] = byte;
    CHECK(jbd_trans_set_block_dirty_range(trans, &block, offset, 32) == EOK);
    CHECK(ext4_block_set(dev, &block) == EOK);
    CHECK(jbd_trans_prepare_operation(trans) == EOK);
    CHECK(jbd_journal_accept(journal, trans, 0) == EOK);
    return journal->running;
}

static void *encoding_submit(void *argument)
{
    struct credit_call *call = argument;
    credit_encoding = true;
    call->result = jbd_journal_submit(call->journal);
    credit_encoding = false;
    return NULL;
}

static void journal_freeze_owner(struct ext4_blockdev *dev,
    struct jbd_journal *journal, ext4_fsblk_t home, unsigned fault)
{
    const struct ext4_lock locks = {.state_lock = credit_state_lock,
        .state_unlock = credit_state_unlock};
    CHECK(!dev->fs->os_locks);
    dev->fs->os_locks = &locks;
    dev->fs->jbd_journal = journal;
    CHECK(jbd_journal_group_init(journal, 4 * 1024 * 1024) == EOK);
    /* C's durable version remains a real checkpoint owner while A is encoded. */
    encoding_accept(dev, journal, home, 0, 'C');
    CHECK(jbd_journal_freeze(journal) == EOK);
    CHECK(jbd_journal_select_stage(journal, JBD_JOURNAL_COMMIT) == JBD_JOURNAL_COMMIT);
    CHECK(jbd_journal_submit(journal) == EOK);
    CHECK(jbd_journal_retire_stage(journal, JBD_JOURNAL_COMMIT, EOK) == EOK);
    struct jbd_trans *first = encoding_accept(dev, journal, home, 0, 'A');
    unsigned first_credits = first->reserved_logs;
    uint32_t first_start = journal->last;
    encoding_test = (__typeof__(encoding_test)){0};
    credit_encoding = true;
    CHECK(jbd_journal_freeze(journal) == EOK);
    credit_encoding = false;
    CHECK(!encoding_test.checksums);
    CHECK(first->frozen && !first->alloc_blocks && first->reserved_logs == first_credits);
    CHECK(first->start_iblock == first_start && journal->last != first_start && !journal->log_reserved);
    CHECK(jbd_journal_select_stage(journal, JBD_JOURNAL_COMMIT) == JBD_JOURNAL_COMMIT);
    encoding_test.pause = true;
    pthread_t encoder;
    struct credit_call call = {.dev = dev, .journal = journal};
    CHECK(!pthread_create(&encoder, NULL, encoding_submit, &call));
    CHECK(!pthread_mutex_lock(&credit_gate));
    while (!encoding_test.paused)
        CHECK(!pthread_cond_wait(&credit_changed, &credit_gate));
    CHECK(!pthread_mutex_unlock(&credit_gate));
    CHECK(!pthread_mutex_trylock(&credit_state));
    CHECK(!pthread_mutex_unlock(&credit_state));
    struct jbd_trans *second = encoding_accept(dev, journal, home, 32, 'B');
    unsigned second_credits = second->reserved_logs;
    uint32_t second_start = journal->last;
    CHECK(jbd_journal_freeze(journal) == EOK && second->start_iblock == second_start);
    CHECK(second->reserved_logs == second_credits && !journal->log_reserved);
    uint32_t last = journal->last;
    struct jbd_trans *aborted;
    CHECK(jbd_journal_new_trans(journal, &aborted) == EOK);
    CHECK(jbd_trans_revoke_block(aborted, dev->lg_bcnt - 1) == EOK);
    CHECK(jbd_trans_prepare_operation(aborted) == EOK && journal->log_reserved == 2);
    jbd_journal_free_trans(journal, aborted, true);
    CHECK(!journal->log_reserved && journal->last == last);
    journal->checkpoint_target = journal->durable_sequence;
    CHECK(jbd_journal_select_stage(journal, JBD_JOURNAL_CHECKPOINT) == JBD_JOURNAL_CHECKPOINT);
    struct jbd_trans *checkpoint = journal->checkpointing;
    if (fault == 3) disk.fail_flush = disk.flushes + 1;
    int checkpoint_result = jbd_journal_checkpoint(journal);
    CHECK(checkpoint_result == (fault == 3 ? EIO : EOK));
    CHECK(jbd_journal_retire_stage(journal, JBD_JOURNAL_CHECKPOINT, checkpoint_result) == checkpoint_result);
    CHECK((fault == 3 || journal->start == first_start) && journal->last == last);
    if (fault == 1) disk.fail_write = disk.writes + 1;
    if (fault == 2) disk.fail_flush = disk.flushes + 1;
    CHECK(!pthread_mutex_lock(&credit_gate));
    encoding_test.resume = true;
    CHECK(!pthread_cond_broadcast(&credit_changed));
    CHECK(!pthread_mutex_unlock(&credit_gate));
    CHECK(!pthread_join(encoder, NULL));
    CHECK(call.result == (fault == 1 || fault == 2 ? EIO : EOK));
    CHECK(encoding_test.checksums && !encoding_test.state_checksums);
    CHECK(first->alloc_blocks == (int)first_credits && !first->reserved_logs);
    CHECK(journal->last == last && !journal->log_reserved && journal->committing == first);
    CHECK(jbd_journal_retire_stage(journal, JBD_JOURNAL_COMMIT, call.result) == (fault ? EIO : EOK));
    if (fault) {
        CHECK(journal->committing == first && journal->sealed_count == 1 &&
            TAILQ_FIRST(&journal->sealed_queue) == second && !second->alloc_blocks &&
            second->reserved_logs == second_credits && journal->last == last &&
            !journal->log_reserved && journal->error == EIO);
        CHECK(journal->failed_trans == (fault == 3 ? checkpoint : first));
        CHECK((journal->checkpointing != NULL) == (fault == 3));
        struct jbd_trans *denied = first;
        CHECK(jbd_journal_new_trans(journal, &denied) == EIO && !denied);
        dev->fs->os_locks = NULL;
        printf("PASS: frozen encoder error block=%u fault=%u retains failed, committing and sealed owners\n",
            dev->lg_bsize, fault);
        return;
    }
    credit_drain(journal);
    CHECK(journal->start == last && !journal->log_reserved && !journal->failed_trans);
    struct ext4_block block;
    CHECK(ext4_block_get(dev, &block, home) == EOK);
    CHECK(block.data[0] == 'A' && block.data[32] == 'B');
    CHECK(ext4_block_set(dev, &block) == EOK);
    dev->fs->os_locks = NULL;
    printf("PASS: frozen group encoder block=%u runs outside state; accept/freeze, abort and older checkpoint progress during checksum; ring owners retained\n", dev->lg_bsize);
}

static void journal_quarantine_scan(struct ext4_blockdev *dev,
    struct jbd_journal *journal)
{
    enum { BITS=512 };
    const ext4_fsblk_t base=1024;
    uint8_t bitmap[BITS/8]={0}, reserved[BITS]={0};
    struct jbd_trans *operation;
    CHECK(jbd_journal_new_trans(journal,&operation)==EOK);
    test_operation_bind(dev->fs,operation);
    for (unsigned inode=0;inode<2;inode++) {
        for (unsigned bit=0;bit<128;bit++)
            CHECK(jbd_trans_quarantine(dev->fs,base+bit,1,inode)==EOK);
        CHECK(jbd_trans_quarantine(dev->fs,base+140,10,inode)==EOK);
        CHECK(jbd_trans_quarantine(dev->fs,base+170,1,inode)==EOK);
        CHECK(jbd_trans_quarantine(dev->fs,base+201,7,inode)==EOK);
    }
    memset(reserved,1,128);
    memset(reserved+140,1,10); reserved[170]=1; memset(reserved+201,1,7);
    unsigned entries=credit_state_entries;
    uint32_t found,length;
    CHECK(jbd_journal_find_reusable(dev->fs,bitmap,base,0,BITS,32,true,
        &found,&length)==EOK && found==128 && length==12);
    CHECK(credit_state_entries==entries+1); /* All 128 owners, one index walk. */
    for (unsigned bit=0;bit<BITS;bit++)
        if (bit%7==3 || bit>=480) ext4_bmap_bit_set(bitmap,bit);
    for (unsigned inode=0;inode<2;inode++) for (uint32_t start=0;start<=BITS;start++) {
        for (unsigned mode=0;mode<3;mode++) {
            uint32_t end=mode==0 ? BITS : start+(BITS-start)/2;
            uint32_t maximum=mode==2 ? 1 : 32, expected=start, count=0;
            while (expected<end && (ext4_bmap_is_bit_set(bitmap,expected) || reserved[expected])) expected++;
            while (count<maximum && expected+count<end &&
                !ext4_bmap_is_bit_set(bitmap,expected+count) && !reserved[expected+count]) count++;
            entries=credit_state_entries;
            int result=jbd_journal_find_reusable(dev->fs,bitmap,base,start,end,
                maximum,inode,&found,&length);
            CHECK(credit_state_entries<=entries+1);
            CHECK(result==(count ? EOK : ENOSPC) && length==count);
            if (count) CHECK(found==expected);
        }
    }
    test_operation_bind(dev->fs,NULL);
    jbd_journal_free_trans(journal,operation,true);
    CHECK(!journal->quarantined_blocks && !journal->quarantined_inodes);
    memset(bitmap,0,sizeof(bitmap));
    CHECK(jbd_journal_find_reusable(dev->fs,bitmap,base,0,BITS,32,true,
        &found,&length)==EOK && !found && length==32);
    printf("PASS: bitmap/quarantine ordered scan, 3078 oracle ranges, one state scope, abort releases both indexes\n");
}
static unsigned quarantine_records(struct jbd_trans *trans)
{
    unsigned count=0;
    struct jbd_quarantine *range;
    LIST_FOREACH(range,&trans->quarantine,node)count++;
    return count;
}
static void quarantine_add(struct ext4_blockdev *dev,struct jbd_trans *operation,
    ext4_fsblk_t first,uint32_t count,unsigned types)
{
    test_operation_bind(dev->fs,operation);
    for(unsigned type=0;type<types;type++)
        CHECK(jbd_trans_quarantine(dev->fs,first,count,type)==EOK);
    test_operation_bind(dev->fs,NULL);
}
static struct jbd_trans *quarantine_operation(struct ext4_blockdev *dev,
    struct jbd_journal *journal,ext4_fsblk_t first,uint32_t count,unsigned types)
{
    struct jbd_trans *operation;
    CHECK(jbd_journal_new_trans(journal,&operation)==EOK);
    quarantine_add(dev,operation,first,count,types);
    CHECK(jbd_trans_revoke_block(operation,dev->lg_bcnt-1)==EOK);
    return operation;
}
static void quarantine_accept(struct jbd_journal *journal,struct jbd_trans *operation)
{
    CHECK(jbd_trans_prepare_operation(operation)==EOK);
    CHECK(jbd_journal_accept(journal,operation,0)==EOK);
}
static void quarantine_interval(struct jbd_trans *group,ext4_fsblk_t first,
    uint32_t count,unsigned types)
{
    unsigned found=0;
    struct jbd_quarantine *range;
    LIST_FOREACH(range,&group->quarantine,node) {
        CHECK(range->owner==group && range->first==first && range->count==count);
        CHECK(!(found & (1U<<range->inode)));
        found|=1U<<range->inode;
    }
    CHECK(found==(1U<<types)-1);
}
static void quarantine_checkpoint_one(struct jbd_journal *journal)
{
    CHECK(jbd_journal_select_stage(journal,JBD_JOURNAL_COMMIT)==JBD_JOURNAL_COMMIT);
    CHECK(jbd_journal_submit(journal)==EOK);
    CHECK(jbd_journal_retire_stage(journal,JBD_JOURNAL_COMMIT,EOK)==EOK);
    journal->checkpoint_target=journal->durable_sequence;
    CHECK(jbd_journal_select_stage(journal,JBD_JOURNAL_CHECKPOINT)==JBD_JOURNAL_CHECKPOINT);
    CHECK(jbd_journal_checkpoint(journal)==EOK);
    CHECK(jbd_journal_retire_stage(journal,JBD_JOURNAL_CHECKPOINT,EOK)==EOK);
}
static void journal_quarantine_coalesce(struct ext4_blockdev *dev,
    struct jbd_journal *journal)
{
    enum { ROUNDS=256 };
    const ext4_fsblk_t base=1024;
    CHECK(jbd_allocation_charge(sizeof(struct jbd_quarantine))==128);
    /* The first group is its operation: its private list stays separate
     * while left/right neighbours are absorbed from that same pending list. */
    struct jbd_trans *operation=quarantine_operation(dev,journal,512+4,1,2);
    quarantine_add(dev,operation,512+0,1,2);
    quarantine_add(dev,operation,512+2,1,2);
    quarantine_add(dev,operation,512+3,1,2);
    quarantine_add(dev,operation,512+1,1,2);
    CHECK(quarantine_records(operation)==10);
    quarantine_accept(journal,operation);
    quarantine_interval(journal->running,512,5,2);
    CHECK(jbd_journal_freeze(journal)==EOK);credit_drain(journal);
    unsigned cumulative_records=0;
    for(unsigned index=0;index<ROUNDS;index++) {
        operation=quarantine_operation(dev,journal,base+index,1,2);
        CHECK(quarantine_records(operation)==2);
        quarantine_accept(journal,operation);
        unsigned records=quarantine_records(journal->running);
        cumulative_records+=records;
        CHECK(records==2);
        CHECK(journal->quarantined_blocks==index+1 && journal->quarantined_inodes==index+1);
    }
    CHECK(cumulative_records==2*ROUNDS);
    quarantine_interval(journal->running,base,ROUNDS,2);
    struct jbd_trans *private=quarantine_operation(dev,journal,base+ROUNDS,1,2);
    operation=quarantine_operation(dev,journal,base+ROUNDS+1,1,2);
    CHECK(quarantine_records(private)==2 && quarantine_records(operation)==2);
    quarantine_accept(journal,operation);
    CHECK(quarantine_records(journal->running)==4 && quarantine_records(private)==2);
    jbd_journal_free_trans(journal,private,true);
    CHECK(journal->quarantined_blocks==ROUNDS+1 && journal->quarantined_inodes==ROUNDS+1);
    uint8_t bitmap[(ROUNDS+7)/8+1]={0};uint32_t bit,length;
    CHECK(jbd_journal_find_reusable(dev->fs,bitmap,base,ROUNDS,ROUNDS+2,1,true,
        &bit,&length)==EOK && bit==ROUNDS && length==1);
    operation=quarantine_operation(dev,journal,base+ROUNDS,1,2);
    quarantine_accept(journal,operation);
    quarantine_interval(journal->running,base,ROUNDS+2,2);
    /* A failed private allocation must leave the accepted interval intact. */
    jbd_memory_set_cache_disabled(journal,true);jbd_trim_cache(journal,0);
    size_t baseline=jbd_memory_used(journal),limit=jbd_memory_limit(journal);
    CHECK(jbd_journal_new_trans(journal,&private)==EOK);
    test_operation_bind(dev->fs,private);
    jbd_memory_set_limit(journal,jbd_memory_used(journal));
    CHECK(jbd_trans_quarantine(dev->fs,base+ROUNDS+2,1,true)==ENOMEM);
    CHECK(!quarantine_records(private) && journal->quarantined_inodes==ROUNDS+2);
    jbd_memory_set_limit(journal,limit);test_operation_bind(dev->fs,NULL);
    jbd_journal_free_trans(journal,private,true);
    CHECK(jbd_memory_used(journal)==baseline);
    quarantine_interval(journal->running,base,ROUNDS+2,2);
    jbd_memory_set_cache_disabled(journal,false);
    CHECK(jbd_journal_freeze(journal)==EOK);
    operation=quarantine_operation(dev,journal,base+ROUNDS+2,1,2);
    quarantine_accept(journal,operation);
    quarantine_interval(journal->running,base+ROUNDS+2,1,2);
    CHECK(journal->quarantined_blocks==ROUNDS+3 && journal->quarantined_inodes==ROUNDS+3);
    CHECK(jbd_journal_freeze(journal)==EOK && journal->sealed_count==2);
    quarantine_checkpoint_one(journal);
    CHECK(journal->quarantined_blocks==1 && journal->quarantined_inodes==1 && journal->sealed_count==1);
    CHECK(jbd_journal_find_reusable(dev->fs,bitmap,base,0,ROUNDS+3,1,true,
        &bit,&length)==EOK && !bit && length==1);
    CHECK(jbd_journal_find_reusable(dev->fs,bitmap,base,ROUNDS+2,ROUNDS+3,1,true,
        &bit,&length)==ENOSPC && !length);
    quarantine_checkpoint_one(journal);
    CHECK(!journal->quarantined_blocks && !journal->quarantined_inodes);
    /* Block ranges can exceed the 32-bit length of one node. Preserve two
     * legal adjacent segments rather than overflowing their combined count. */
    const ext4_fsblk_t high=UINT64_C(1)<<32;
    operation=quarantine_operation(dev,journal,high,UINT32_MAX,1);
    quarantine_accept(journal,operation);
    operation=quarantine_operation(dev,journal,high+UINT32_MAX,1,1);
    quarantine_accept(journal,operation);
    CHECK(quarantine_records(journal->running)==2 &&
        journal->quarantined_blocks==(uint64_t)UINT32_MAX+1 && !journal->quarantined_inodes);
    uint8_t small=0;
    CHECK(jbd_journal_find_reusable(dev->fs,&small,high+UINT32_MAX-1,0,3,3,false,
        &bit,&length)==EOK && bit==2 && length==1);
    CHECK(jbd_journal_freeze(journal)==EOK);credit_drain(journal);
    CHECK(!journal->quarantined_blocks && !journal->quarantined_inodes);
    printf("PASS: accepted quarantine block=%u: %u operations retain two typed intervals; private abort/bridge, distinct checkpoint owners, OOM and UINT32 length boundary\n",
        dev->lg_bsize,ROUNDS);
}
static void journal_credit_parallel(struct ext4_blockdev *dev,
    struct jbd_journal *journal, ext4_fsblk_t home)
{
    const struct ext4_lock locks = {.state_lock = credit_state_lock,
        .state_unlock = credit_state_unlock};
    CHECK(!dev->fs->os_locks);
    dev->fs->os_locks = &locks;
    CHECK(jbd_journal_group_init(journal, 4 * 1024 * 1024) == EOK);
    dev->fs->jbd_journal = journal;
    journal_quarantine_scan(dev,journal);
    journal_quarantine_coalesce(dev,journal);
    struct jbd_trans *private;
    CHECK(jbd_journal_new_trans(journal,&private)==EOK);
    unsigned entries=credit_state_entries;
    for(unsigned i=0;i<16;i++)
        CHECK(jbd_trans_revoke_block(private,dev->lg_bcnt-1-i)==EOK);
    CHECK(credit_state_entries==entries); /* Only the task owns this revoke tree. */
    CHECK(!journal->log_reserved && !private->checkpoint_image);
    CHECK(jbd_trans_prepare_operation(private)==EOK && credit_state_entries==entries+1);
    jbd_journal_free_trans(journal,private,true);
    jbd_memory_set_cache_disabled(journal, true);
    jbd_trim_cache(journal, 0);
    size_t baseline_memory = jbd_memory_used(journal);
    uint64_t baseline_budget_failures = jbd_memory_budget_failures(journal);
    unsigned cases = 0, failures = 0;
    for (unsigned private_access = 0; private_access < 2; private_access++) {
        unsigned last_failure = private_access ? 9 : 8;
        for (unsigned fail_after = 0; fail_after <= last_failure; fail_after++) {
            /* Keep a genuine immutable group in the ring while both new
             * preparers own independent credits. No disk writes run yet. */
            struct jbd_trans *frozen;
            CHECK(jbd_journal_new_trans(journal, &frozen) == EOK);
            CHECK(jbd_trans_revoke_block(frozen, dev->lg_bcnt - 1) == EOK);
            CHECK(jbd_trans_prepare_operation(frozen)==EOK);
            CHECK(jbd_journal_accept(journal, frozen, 0) == EOK);
            CHECK(jbd_journal_freeze(journal) == EOK);
            CHECK(frozen->frozen && !frozen->alloc_blocks && !journal->log_reserved);
            CHECK(frozen->checkpoint_image && frozen->reserved_logs == 2 && frozen->reserved_log_images == 2);
            struct jbd_image *checkpoint = frozen->checkpoint_image;
            uint32_t frozen_start = frozen->start_iblock, frozen_id = frozen->trans_id;
            uint32_t last = journal->last;
            struct credit_call a = {.dev = dev, .journal = journal,
                .private_access = private_access};
            CHECK(jbd_journal_new_trans(journal, &a.trans) == EOK);
            if (private_access) {
                CHECK(ext4_block_get(dev, &a.block, home) == EOK);
                CHECK(jbd_trans_get_write_access(a.trans,&a.block)==EOK);
                a.block.data[0]^=1;
                CHECK(jbd_trans_set_block_dirty(a.trans,&a.block)==EOK);
            } else CHECK(jbd_trans_revoke_block(a.trans,dev->lg_bcnt-3)==EOK);
            credit_test = (__typeof__(credit_test)){.journal = journal,
                .baseline = journal->log_reserved, .fail_after = fail_after};
            pthread_t first, second;
            CHECK(!pthread_create(&first, NULL, credit_prepare, &a));
            CHECK(!pthread_mutex_lock(&credit_gate));
            while (!credit_test.paused)
                CHECK(!pthread_cond_wait(&credit_changed, &credit_gate));
            CHECK(!pthread_mutex_unlock(&credit_gate));
            unsigned claimed = private_access ? 3 : 2;
            CHECK(credit_test.paused_credits == claimed);
            int state_result = pthread_mutex_trylock(&credit_state);
            CHECK(!state_result || state_result == EBUSY);
            if (!state_result) {
                CHECK(journal->log_reserved == claimed);
                CHECK(!pthread_mutex_unlock(&credit_state));
            }
            struct jbd_memory_stats paused;
            jbd_memory_snapshot(journal->memory, &paused);
            CHECK(paused.reserved && paused.used + paused.reserved <= paused.limit);
            struct credit_call b = {.dev = dev, .journal = journal,
                .freeze = fail_after % 2};
            CHECK(!pthread_create(&second, NULL, credit_compete, &b));
            CHECK(!pthread_mutex_lock(&credit_gate));
            while (!credit_test.finished && !credit_test.blocked)
                CHECK(!pthread_cond_wait(&credit_changed, &credit_gate));
            bool blocked = credit_test.blocked;
            if (!blocked) {
                CHECK(!state_result);
                credit_state_lock(NULL);
                CHECK(b.reserved == claimed && journal->log_reserved == claimed);
                CHECK(frozen->frozen && frozen->start_iblock == frozen_start &&
                    frozen->trans_id == frozen_id && !frozen->alloc_blocks &&
                    frozen->checkpoint_image == checkpoint);
                CHECK(journal->sealed_count == 1U + b.freeze);
                CHECK(journal->last == last + 2 * b.freeze);
                credit_state_unlock(NULL);
            }
            credit_test.resume = true;
            CHECK(!pthread_cond_broadcast(&credit_changed));
            CHECK(!pthread_mutex_unlock(&credit_gate));
            CHECK(!pthread_join(first, NULL) && !pthread_join(second, NULL));
            CHECK(!blocked); /* B must complete before A's allocator returns. */
            CHECK(a.result == (fail_after ? ENOMEM : EOK));
            if (fail_after) CHECK(credit_test.calls == fail_after);
            if (a.trans) jbd_journal_free_trans(journal, a.trans, true);
            if (private_access) CHECK(ext4_block_set(dev, &a.block) == EOK);
            credit_test.journal = NULL;
            CHECK(!journal->log_reserved && !journal->running && !journal->error);
            struct jbd_memory_stats after;
            jbd_memory_snapshot(journal->memory, &after);
            CHECK(!after.reserved && !after.cached && after.budget_failures==baseline_budget_failures);
            credit_drain(journal);
            CHECK(!journal->log_reserved && journal->start == journal->last);
            CHECK(jbd_memory_used(journal) == baseline_memory);
            cases++; failures += fail_after != 0;
        }
    }
    dev->fs->os_locks = NULL;
    printf("PASS: journal credit parallel block=%u cases=%u heap-failures=%u; B completes while A allocation is paused, frozen groups and credits retained\n",
        dev->lg_bsize, cases, failures);
}
