#ifndef BOAROS_TEST_LWEXT4_OPERATION_LIFECYCLE_H
#define BOAROS_TEST_LWEXT4_OPERATION_LIFECYCLE_H

static pthread_mutex_t lifecycle_state = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned lifecycle_state_depth;
static _Thread_local bool lifecycle_drain_actor;
static _Thread_local bool lifecycle_admitting;
static unsigned lifecycle_state_calls;
static pthread_mutex_t lifecycle_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lifecycle_changed = PTHREAD_COND_INITIALIZER;
static bool lifecycle_pause_accept, lifecycle_drain_start, lifecycle_drain_blocked, lifecycle_drain_finished;
static struct {
    struct ext4_fs *fs;
    struct jbd_trans *trans;
    bool watching;
    unsigned accepts, aborts, io, publications;
} lifecycle;

static void lifecycle_backend(void *context) { (void)context; }

static void lifecycle_lock(void *context)
{
    (void)context;
    /* Admission already owns this qualification; a second entry delegates
     * the same shared state instead of using its actual outer owner. */
    CHECK(!lifecycle_admitting);
    __atomic_add_fetch(&lifecycle_state_calls, 1, __ATOMIC_RELAXED);
    if (lifecycle_state_depth++) return;
    if (lifecycle_drain_actor) {
        int result = pthread_mutex_trylock(&lifecycle_state);
        if (!result) return;
        CHECK(result == EBUSY);
        CHECK(!pthread_mutex_lock(&lifecycle_gate));
        lifecycle_drain_blocked = true;
        CHECK(!pthread_cond_broadcast(&lifecycle_changed));
        CHECK(!pthread_mutex_unlock(&lifecycle_gate));
    }
    CHECK(!pthread_mutex_lock(&lifecycle_state));
}

static void lifecycle_unlock(void *context)
{
    (void)context;
    CHECK(lifecycle_state_depth);
    if (!--lifecycle_state_depth) CHECK(!pthread_mutex_unlock(&lifecycle_state));
}

int __real_jbd_journal_accept_state_held(struct jbd_journal *, struct jbd_trans *, uint64_t);
int __wrap_jbd_journal_accept_state_held(struct jbd_journal *journal, struct jbd_trans *trans, uint64_t now)
{
    if (lifecycle.watching && lifecycle.trans == trans) {
        CHECK(lifecycle_state_depth && lifecycle.fs->active_operations == 1);
        CHECK(!ext4_current_operation(lifecycle.fs));
        lifecycle.accepts++;
        if (lifecycle_pause_accept) {
            CHECK(!pthread_mutex_lock(&lifecycle_gate));
            lifecycle_drain_start = true;
            CHECK(!pthread_cond_broadcast(&lifecycle_changed));
            while (!lifecycle_drain_blocked)
                CHECK(!pthread_cond_wait(&lifecycle_changed, &lifecycle_gate));
            CHECK(!lifecycle_drain_finished && lifecycle.fs->active_operations == 1);
            CHECK(!pthread_mutex_unlock(&lifecycle_gate));
        }
    }
    bool outer=lifecycle_admitting;
    if (lifecycle.watching && lifecycle.trans==trans) lifecycle_admitting=true;
    int result=__real_jbd_journal_accept_state_held(journal, trans, now);
    lifecycle_admitting=outer;
    return result;
}

void __real_ext4_operation_publish_super(struct ext4_operation *);
void __wrap_ext4_operation_publish_super(struct ext4_operation *operation)
{
    if (lifecycle.watching && operation->fs==lifecycle.fs) {
        CHECK(lifecycle_state_depth==1 && lifecycle.fs->active_operations==1 &&
              !ext4_current_operation(lifecycle.fs));
        lifecycle.publications++;
    }
    __real_ext4_operation_publish_super(operation);
}

void __real_jbd_journal_free_trans_state_held(struct jbd_journal *, struct jbd_trans *, bool);
void __wrap_jbd_journal_free_trans_state_held(struct jbd_journal *journal, struct jbd_trans *trans, bool abort)
{
    if (lifecycle.watching && lifecycle.trans == trans && abort) {
        CHECK(lifecycle_state_depth && lifecycle.fs->active_operations == 1);
        CHECK(!ext4_current_operation(lifecycle.fs));
        lifecycle.aborts++;
    }
    bool outer=lifecycle_admitting;
    if (lifecycle.watching && lifecycle.trans==trans) lifecycle_admitting=true;
    __real_jbd_journal_free_trans_state_held(journal, trans, abort);
    lifecycle_admitting=outer;
}

static void lifecycle_commit_io(void)
{
    CHECK(!lifecycle_state_depth);
    CHECK(lifecycle.fs->active_operations == 1);
    CHECK(!ext4_current_operation(lifecycle.fs));
    lifecycle.io++;
}

static void lifecycle_watch(struct ext4_fs *fs)
{
    CHECK(fs->active_operations == 1 && ext4_current_trans(fs));
    lifecycle.fs = fs;
    lifecycle.trans = ext4_current_trans(fs);
    lifecycle.watching = true;
}

static void lifecycle_ended(struct ext4_fs *fs)
{
    lifecycle.watching = false;
    CHECK(!fs->active_operations && !ext4_current_operation(fs));
    CHECK(!lifecycle_state_depth);
}

static unsigned lifecycle_times(void *owner, struct ext4_timestamp times[3])
{
    (void)owner;
    times[1] = (struct ext4_timestamp){740, 4};
    return EXT4_TIME_MTIME;
}

static void lifecycle_request(void *context, unsigned work)
{
    CHECK(!lifecycle_state_depth);
    CHECK(!pthread_mutex_lock(&lifecycle_gate));
    group_request(context, work);
    CHECK(!pthread_mutex_unlock(&lifecycle_gate));
}

static void *lifecycle_drain(void *context)
{
    struct ext4_fs *fs = context;
    CHECK(!pthread_mutex_lock(&lifecycle_gate));
    while (!lifecycle_drain_start)
        CHECK(!pthread_cond_wait(&lifecycle_changed, &lifecycle_gate));
    CHECK(!pthread_mutex_unlock(&lifecycle_gate));
    lifecycle_drain_actor = true;
    CHECK(ext4_journal_group_drain("/") == EOK);
    CHECK(!fs->active_operations && fs->jbd_journal->checkpoint_sequence == fs->jbd_journal->accepted_sequence);
    CHECK(!pthread_mutex_lock(&lifecycle_gate));
    lifecycle_drain_finished = true;
    CHECK(!pthread_mutex_unlock(&lifecycle_gate));
    return NULL;
}

static void lifecycle_admission(struct ext4_fs *fs, ext4_file *file)
{
    struct jbd_journal *journal = fs->jbd_journal;
    CHECK(ext4_journal_group_drain("/") == EOK);
    jbd_memory_set_cache_disabled(journal, true);
    jbd_trim_cache(journal, 0);
    size_t used = jbd_memory_used(journal);
    size_t minimum = 5 * (jbd_image_charge(journal) + 256) +
        jbd_allocation_charge(sizeof(struct jbd_trans)) + 4 * 256;
    /* This is the existing admission allowance, including exactly one
     * control object. Allocating it before STATE must not charge it twice. */
    jbd_memory_set_limit(journal, used + minimum);
    unsigned calls = lifecycle_state_calls;
    CHECK(ext4_transaction_begin("/") == EOK);
    CHECK(lifecycle_state_calls == calls + 1);
    CHECK(jbd_memory_used(journal) == used + jbd_allocation_charge(sizeof(struct jbd_trans)));
    CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
    CHECK(jbd_memory_used(journal) == used && !fs->active_operations);

    jbd_memory_set_limit(journal, used + minimum - 1);
    CHECK(ext4_transaction_begin("/") == ENOMEM);
    CHECK(!ext4_current_operation(fs) && !fs->active_operations && jbd_memory_used(journal) == used);
    struct ext4_time_observer observer = {.snapshot = lifecycle_times};
    CHECK(ext4_mount_setup_time_observer("/", &observer) == EOK);
    CHECK(ext4_file_writeback_times(file, false) == EAGAIN);
    CHECK(!ext4_current_operation(fs) && !fs->active_operations && jbd_memory_used(journal) == used);

    jbd_memory_set_limit(journal, 4 * 1024 * 1024);
    uint64_t denials = jbd_memory_budget_failures(journal);
    fail_allocation = allocations + 1;
    CHECK(ext4_transaction_begin("/") == ENOMEM && allocations == fail_allocation);
    fail_allocation = 0;
    CHECK(!kernel_io_context_current()->ext4_last_memory_reservation_failed);
    CHECK(jbd_memory_budget_failures(journal) == denials && jbd_memory_used(journal) == used);
    CHECK(!fs->active_operations && !ext4_current_operation(fs));
    fail_allocation = allocations + 1;
    CHECK(ext4_file_writeback_times(file, false) == ENOMEM && allocations == fail_allocation);
    fail_allocation = 0;
    CHECK(!kernel_io_context_current()->ext4_last_memory_reservation_failed);
    CHECK(jbd_memory_budget_failures(journal) == denials && jbd_memory_used(journal) == used);
    CHECK(ext4_mount_setup_time_observer("/", NULL) == EOK);

    const struct ext4_timestamp times[3] = {{750, 5}, {760, 6}, {770, 7}};
    CHECK(ext4_file_set_times(file, 7, times) == EOK && journal->running);
    group_now += 100000000;
    unsigned requests = group_requests;
    denials = jbd_memory_budget_failures(journal);
    fail_allocation = allocations + 1;
    CHECK(ext4_transaction_begin("/") == ENOMEM && allocations == fail_allocation);
    fail_allocation = 0;
    CHECK(!journal->running && journal->sealed_count == 1 && group_requests == requests + 1);
    CHECK(jbd_memory_budget_failures(journal) == denials && !fs->active_operations);
    CHECK(ext4_journal_group_drain("/") == EOK);
    jbd_memory_set_cache_disabled(journal, false);
    puts("PASS one admission scope, one trans charge, hard-budget ENOMEM/nonwait EAGAIN, real OOM, and freeze request before OOM return");
}

static void operation_lifecycle_test(struct ext4_fs *fs)
{
    const struct ext4_lock locks = {.lock = lifecycle_backend, .unlock = lifecycle_backend,
        .state_lock = lifecycle_lock,
        .state_read_lock = lifecycle_lock, .state_unlock = lifecycle_unlock};
    CHECK(ext4_mount_setup_locks("/", &locks) == EOK);
    CHECK(ext4_transaction_begin("/") == EOK);
    ext4_file file;
    CHECK(ext4_fopen(&file, "/lifecycle", "w+") == EOK);
    CHECK(ext4_fclose(&file) == EOK);
    lifecycle_watch(fs);
    write_interleave = lifecycle_commit_io;
    CHECK(ext4_transaction_end("/") == EOK && lifecycle.io == 1);
    lifecycle_ended(fs);

    CHECK(ext4_transaction_begin("/") == EOK);
    lifecycle_watch(fs);
    CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED && lifecycle.aborts==1);
    lifecycle_ended(fs);

    const struct ext4_journal_runtime runtime = {.now_ns = group_clock, .request = lifecycle_request};
    CHECK(ext4_journal_group_enable("/", &runtime, 4 * 1024 * 1024) == EOK);
    for (unsigned mutation = 0; mutation < 2; mutation++) {
        CHECK(ext4_transaction_begin("/") == EOK);
        CHECK(ext4_transaction_begin("/") == EOK);
        struct jbd_trans *trans = ext4_current_trans(fs);
        if (mutation) {
            CHECK(ext4_fopen(&file, "/lifecycle", "r+") == EOK);
            const struct ext4_timestamp times[3] = {{710, 1}, {720, 2}, {730, 3}};
            CHECK(ext4_file_set_times(&file, 7, times) == EOK);
            CHECK(ext4_fclose(&file) == EOK);
        }
        CHECK(ext4_transaction_end("/") == EOK && ext4_current_trans(fs) == trans);
        lifecycle_watch(fs);
        unsigned accepts = lifecycle.accepts;
        pthread_t drain;
        if (mutation) {
            lifecycle_pause_accept = true;
            CHECK(!pthread_create(&drain, NULL, lifecycle_drain, fs));
        }
        CHECK(ext4_transaction_end("/") == EOK && lifecycle.accepts == accepts + 1);
        if (mutation) {
            CHECK(!pthread_join(drain, NULL));
            CHECK(lifecycle_drain_blocked && lifecycle_drain_finished);
            lifecycle_pause_accept = false;
        }
        lifecycle_ended(fs);
    }
    CHECK(ext4_transaction_begin("/") == EOK);
    lifecycle_watch(fs);
    CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED && lifecycle.aborts == 2);
    lifecycle_ended(fs);
    CHECK(ext4_journal_group_drain("/") == EOK);
    CHECK(ext4_fopen(&file, "/lifecycle", "r+") == EOK);
    lifecycle_admission(fs, &file);
    CHECK(ext4_fclose(&file) == EOK);
    CHECK(ext4_fremove("/lifecycle") == EOK);
    CHECK(ext4_journal_group_drain("/") == EOK);
    CHECK(ext4_mount_setup_locks("/", NULL) == EOK);
    puts("PASS active owner spans grouped accept/abort and non-group commit I/O; drain queues behind actual acceptance; nested/empty return once");
}

static void operation_state_error(struct ext4_fs *fs,bool flush)
{
    const struct ext4_lock locks={.lock=lifecycle_backend,.unlock=lifecycle_backend,
        .state_lock=lifecycle_lock,.state_read_lock=lifecycle_lock,.state_unlock=lifecycle_unlock};
    CHECK(ext4_mount_setup_locks("/",&locks)==EOK);
    CHECK(ext4_transaction_begin("/")==EOK);
    ext4_file file;
    CHECK(ext4_fopen(&file,"/state-io-failure","w+")==EOK && ext4_fclose(&file)==EOK);
    lifecycle_watch(fs);
    struct jbd_trans *owner=lifecycle.trans;
    unsigned publications=lifecycle.publications;
    write_interleave=lifecycle_commit_io;
    if(flush)disk.fail_flush=disk.flushes+1;
    else disk.fail_write=disk.writes+1;
    CHECK(ext4_transaction_end("/")==EIO);
    CHECK(!fs->active_operations && !ext4_current_operation(fs) && !lifecycle_state_depth);
    CHECK(lifecycle.io==1 && lifecycle.publications==publications+1 &&
          fs->jbd_journal->failed_trans==owner && owner->operation.published &&
          jbd_journal_error(fs->jbd_journal)==EIO);
    CHECK(fault_block_crash(&disk)==0);
    puts("PASS immediate I/O failure publishes under STATE after I/O, retains its failed owner and ends the active operation");
}

#endif
