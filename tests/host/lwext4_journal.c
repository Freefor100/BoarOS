/* Real lwext4/JBD writes over a device with volatile sector writes. */
#include "block_fault.h"
#include "lwext4_operation.h"
#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_bitmap.h>
#include <ext4_fs.h>
#include <ext4_operation.h>
#include <kernel/sync.h>
#include <ext4_inode.h>
#include <ext4_journal.h>
#include <ext4_errno.h>
#include <ext4_misc.h>
#include <ext4_crc32.h>
#include <ext4_trans.h>
#include <ext4_super.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static struct fault_block disk;
static uint64_t reads, fail_read;
#ifdef TEST_JBD_CREDIT_PARALLEL
#include "lwext4_credit_parallel.h"
static struct {
    struct ext4_buf *buffer;
    struct ext4_buf *rebind_buffer;
    struct ext4_block_payload *current;
    uint32_t references, extra;
    unsigned observed, rebinds;
} publication_owner;
void __real_ext4_block_rebind(struct ext4_block *, struct ext4_block_payload *);
void __wrap_ext4_block_rebind(struct ext4_block *block, struct ext4_block_payload *next)
{
    if (block->buf == publication_owner.rebind_buffer) publication_owner.rebinds++;
    __real_ext4_block_rebind(block, next);
}
void __real_ext4_bcache_publish_journal(struct ext4_block *, struct ext4_block_payload *,
    uint64_t, bool);
void __wrap_ext4_bcache_publish_journal(struct ext4_block *block, struct ext4_block_payload *next,
    uint64_t tree_key, bool merged)
{
    struct ext4_buf *buffer = block->buf;
    if (buffer == publication_owner.buffer) {
        CHECK(credit_state_depth && buffer->current_payload == publication_owner.current);
        uint32_t references = __atomic_load_n(&publication_owner.current->references, __ATOMIC_ACQUIRE);
        fprintf(stderr, "publication owner: before=%u at_publish=%u required_extra=%u\n",
            publication_owner.references, references, publication_owner.extra);
        CHECK(references == publication_owner.references + publication_owner.extra);
        publication_owner.buffer = NULL;
        publication_owner.observed++;
    }
    __real_ext4_bcache_publish_journal(block, next, tree_key, merged);
}
static void publication_watch(struct ext4_buf *buffer, unsigned extra)
{
    CHECK(!publication_owner.buffer);
    publication_owner.buffer = buffer;
    publication_owner.current = buffer->current_payload;
    publication_owner.references = __atomic_load_n(&buffer->current_payload->references, __ATOMIC_ACQUIRE);
    publication_owner.extra = extra;
}
#endif
#ifdef TEST_JBD_RECORD_COPY
static bool count_record_copies;
static size_t record_copy_bytes;
static struct kernel_raw_lock *private_rebind_cache;
static unsigned private_rebind_cache_locks;
static bool count_journal_pool_locks;
static unsigned journal_pool_locks;
static struct ext4_bcache *private_borrow_cache;
static uint64_t private_borrow_lba;
static unsigned private_borrow_lookups;
struct ext4_buf *__real_ext4_bcache_find_get(struct ext4_bcache *, struct ext4_block *,
    uint64_t, enum ext4_bcache_read_state *);
struct ext4_buf *__wrap_ext4_bcache_find_get(struct ext4_bcache *cache, struct ext4_block *block,
    uint64_t lba, enum ext4_bcache_read_state *read)
{
    if (cache == private_borrow_cache && lba == private_borrow_lba) private_borrow_lookups++;
    return __real_ext4_bcache_find_get(cache, block, lba, read);
}
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *, struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock, struct kernel_raw_guard *guard)
{
    if (lock == private_rebind_cache) private_rebind_cache_locks++;
    if (count_journal_pool_locks && lock->rank == KERNEL_RAW_RANK_OBJECT + 1)
        journal_pool_locks++;
    __real_kernel_raw_lock_acquire(lock, guard);
}
void *__real_memcpy(void *, const void *, size_t);
void *__wrap_memcpy(void *out, const void *in, size_t bytes)
{
    if (count_record_copies) record_copy_bytes += bytes;
    return __real_memcpy(out, in, bytes);
}
#endif
static int open_device(struct ext4_blockdev *b) { (void)b; return EOK; }
static int read_device(struct ext4_blockdev *b, void *p, uint64_t n, uint32_t c)
{ (void)b; if (++reads == fail_read) return EIO; return kernel_block_read_at(&disk.device, n * 512, p, (size_t)c * 512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
static int write_device(struct ext4_blockdev *b, const void *p, uint64_t n, uint32_t c)
{
#ifdef TEST_JBD_CREDIT_PARALLEL
    if (wire_owner.io_active) {
        CHECK(wire_owner.image && wire_owner.image->active && wire_owner.image->payload.references);
        wire_owner.io_calls++;
    }
#endif
    (void)b; return kernel_block_write_at(&disk.device, n * 512, p, (size_t)c * 512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO;
}
static int flush_device(struct ext4_blockdev *b)
{ (void)b; return kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }

static void close_normal_journal(struct ext4_blockdev *dev, struct jbd_journal *journal)
{
    struct jbd_memory *memory = journal->memory;
    jbd_memory_get(memory);
    if (journal->grouped) jbd_journal_group_fini(journal);
    CHECK(journal->log_map && journal->commit_image && journal->checkpoint_image &&
        jbd_memory_cached(journal) == 0);
    CHECK(jbd_journal_stop(journal) == EOK);
    CHECK(!journal->log_map && !journal->commit_image && !journal->checkpoint_image);
    dev->fs->jbd_journal = NULL;
    dev->journal = NULL;
    CHECK(jbd_put_fs(journal->jbd_fs) == EOK);
    CHECK(ext4_umount("/") == EOK);
    struct jbd_memory_stats stats;
    jbd_memory_snapshot(memory, &stats);
    CHECK(stats.used == 0 && stats.reserved == 0 && stats.cached == 0);
    jbd_memory_put(memory);
}

struct resource_wait {
    struct jbd_journal *journal;
    enum ext4_journal_wait expected;
    uint64_t target;
    unsigned calls;
};
static uint64_t resource_now(void *context) { (void)context; return 0; }
static int resource_wait(void *context, uint64_t target, enum ext4_journal_wait kind)
{
    struct resource_wait *wait = context;
    struct jbd_journal *journal = wait->journal;
    CHECK(kind == wait->expected && target == wait->target);
    CHECK(kind == EXT4_JOURNAL_WAIT_CHECKPOINT ? target > journal->checkpoint_sequence :
        target > journal->durable_sequence);
    wait->calls++;
    if (journal->running && journal->running->sequence <= target)
        CHECK(jbd_journal_freeze(journal) == EOK);
    while (journal->durable_sequence < target) {
        if (!journal->committing)
            CHECK(jbd_journal_select_stage(journal, JBD_JOURNAL_COMMIT) == JBD_JOURNAL_COMMIT);
        int result = jbd_journal_retire_stage(journal, JBD_JOURNAL_COMMIT,
            jbd_journal_submit(journal));
        if (result) return result;
    }
    if (kind == EXT4_JOURNAL_WAIT_CHECKPOINT) {
        CHECK(jbd_journal_select_stage(journal, JBD_JOURNAL_CHECKPOINT) == JBD_JOURNAL_CHECKPOINT);
        CHECK(jbd_journal_retire_stage(journal, JBD_JOURNAL_CHECKPOINT,
            jbd_journal_checkpoint(journal)) == EOK);
    }
    return EOK;
}
struct resource_source { unsigned references; void *data; };
static void resource_retain(void *context) { ((struct resource_source *)context)->references++; }
static void resource_release(void *context)
{ CHECK(((struct resource_source *)context)->references); ((struct resource_source *)context)->references--; }

#ifdef TEST_WRITEBACK_REGRESSIONS
static struct {
    bool watch, fail;
    unsigned zeroes, writes;
} unwritten_write;

int __real_ext4_trans_write_data(struct ext4_blockdev *, uint64_t, uint32_t,
    const void *, uint32_t, bool, const struct file_data_source *);
int __wrap_ext4_trans_write_data(struct ext4_blockdev *bdev, uint64_t lba,
    uint32_t offset, const void *data, uint32_t length, bool initialize,
    const struct file_data_source *source)
{
    int result = __real_ext4_trans_write_data(bdev, lba, offset, data, length, initialize, source);
    if (!result && unwritten_write.watch) {
        if (!data && initialize) unwritten_write.zeroes++;
        if (data && ++unwritten_write.writes == 1 && unwritten_write.fail) result = EIO;
    }
    return result;
}

static unsigned try_prewrite_calls;
static int defer_prewrite(struct ext4_blockdev *dev, const struct kernel_block_span *spans,
    unsigned count)
{
    (void)dev;
    CHECK(count && spans[0].size);
    try_prewrite_calls++;
    return EAGAIN;
}
static int unexpected_prewrite(struct ext4_blockdev *dev, const struct kernel_block_span *spans,
    unsigned count)
{
    (void)dev; (void)spans; (void)count;
    CHECK(!"deferred prewrite fell back to a blocking device call");
    return EIO;
}

#ifdef TEST_JBD_RECORD_COPY
static bool borrow_during_prewrite;
static struct ext4_block during_prewrite;

static int write_checked_prewrite(struct ext4_blockdev *dev, const struct kernel_block_span *spans,
    unsigned count)
{
    if (borrow_during_prewrite)
        CHECK(ext4_block_get(dev, &during_prewrite, spans[0].offset / dev->lg_bsize) == EOK);
    for (unsigned i = 0; i < count; i++) {
        int r = write_device(dev, spans[i].buffer, spans[i].offset / 512, spans[i].size / 512);
        if (r) return r;
    }
    return EOK;
}

static void check_prewrite_cache(struct ext4_blockdev *dev, ext4_file *file,
    unsigned char *input, size_t length)
{
    struct ext4_inode_ref ref;
    struct ext4_inode_block_range range;
    CHECK(ext4_fs_get_inode_ref_read(dev->fs, file->inode, &ref) == EOK);
    CHECK(ext4_fs_get_inode_dblk_range(&ref, 0, 4, false, &range) == EOK && range.count == 4);
    CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
    ext4_bcache_invalidate_lba(dev->bc, range.block, range.count);
    struct kernel_block_span span = {range.block * dev->lg_bsize, input, length};
    dev->bdif->bwrite_batch = unexpected_prewrite;
    dev->bdif->bwrite_batch_try_phase = write_checked_prewrite;
    private_borrow_cache = dev->bc;
    private_borrow_lba = range.block;
    private_borrow_lookups = 0;
    CHECK(ext4_trans_write_data_batch(dev, &span, 1, true) == EOK);
    CHECK(!private_borrow_lookups);
    private_borrow_cache = NULL;
    memset(input, 0x92, length);
    borrow_during_prewrite = true;
    CHECK(ext4_trans_write_data_batch(dev, &span, 1, true) == EOK);
    CHECK(!memcmp(during_prewrite.data, input, dev->lg_bsize));
    borrow_during_prewrite = false;
    CHECK(ext4_block_set(dev, &during_prewrite) == EOK);
    dev->bdif->bwrite_batch = NULL;
    dev->bdif->bwrite_batch_try_phase = NULL;
}
#endif

static void write_deferred_prewrite(struct ext4_blockdev *dev)
{
    CHECK(ext4_journal_start("/") == EOK);
    const struct ext4_journal_runtime runtime = {.now_ns = resource_now, .prewrite_fresh = true};
    CHECK(ext4_journal_group_enable("/", &runtime, 4 * 1024 * 1024) == EOK);
    ext4_file file;
    struct ext4_delalloc delalloc = {0};
    CHECK(ext4_fopen(&file, "/fresh", "w+") == EOK);
    file.delalloc = &delalloc;
    size_t length = 4 * dev->lg_bsize;
    unsigned char *input = malloc(length), *output = malloc(length);
    CHECK(input && output);
    for (size_t i = 0; i < length; i++) input[i] = (unsigned char)(i / dev->lg_bsize + i);
    struct resource_source owner = {.data = input};
    struct file_write_span span = {.buffer = input, .size = length,
        .source = {.context = &owner, .bytes = length, .retain = resource_retain, .release = resource_release}};
    dev->bdif->bwrite_batch = unexpected_prewrite;
    dev->bdif->bwrite_batch_try_phase = defer_prewrite;
    kernel_io_context_current()->background_reclaim = 1;
    struct jbd_journal *journal=dev->fs->jbd_journal;
    uint64_t accepted=journal->accepted_sequence;
    struct file_writeback_wait wait={0};
    CHECK(ext4_fpwrite_batch_wait(&file, &span, 1, &wait) == EAGAIN &&
        wait.device_phase && !wait.sequence && !span.written && !owner.references);
    CHECK(journal->accepted_sequence == accepted && !ext4_fsize(&file));
    CHECK(ext4_fpwrite_batch(&file, &span, 1) == EOK && span.written == length);
    kernel_io_context_current()->background_reclaim = 0;
    CHECK(try_prewrite_calls == 2 && owner.references == 1);
    dev->bdif->bwrite_batch = NULL;
    dev->bdif->bwrite_batch_try_phase = NULL;
    CHECK(ext4_journal_group_commit("/", true) == EOK);
    CHECK(ext4_journal_group_checkpoint("/") == EOK && !owner.references);
    size_t got;
    CHECK(ext4_fpread(&file, 0, output, length, &got) == EOK && got == length);
    CHECK(memcmp(input, output, length) == 0);
#ifdef TEST_JBD_RECORD_COPY
    ext4_file live;
    CHECK(ext4_fopen(&live, "/live", "w+") == EOK);
    live.delalloc = &delalloc;
    struct file_write_span borrowed = {.buffer = input, .size = length};
    dev->bdif->bwrite_batch = unexpected_prewrite;
    dev->bdif->bwrite_batch_try_phase = defer_prewrite;
    accepted = journal->accepted_sequence;
    CHECK(ext4_fpwrite_mapped_try(&live, &borrowed, 1) == EAGAIN);
    CHECK(!borrowed.written && !ext4_fsize(&live) && journal->accepted_sequence == accepted);
    dev->bdif->bwrite_batch_try_phase = write_checked_prewrite;
    CHECK(ext4_fpwrite_mapped_try(&live, &borrowed, 1) == EOK && borrowed.written == length);
    CHECK(ext4_journal_group_commit("/", true) == EOK);
    CHECK(ext4_journal_group_checkpoint("/") == EOK);
    CHECK(ext4_fpread(&live, 0, output, length, &got) == EOK && got == length);
    CHECK(memcmp(input, output, length) == 0);
    dev->bdif->bwrite_batch = NULL;
    dev->bdif->bwrite_batch_try_phase = NULL;
    bool changed;
    CHECK(ext4_fallocate(&live, EXT4_FALLOC_KEEP_SIZE, length, length,
        &changed, UINT32_MAX) == EOK && changed);
    borrowed.offset = length;
    dev->bdif->bwrite_batch = unexpected_prewrite;
    dev->bdif->bwrite_batch_try_phase = unexpected_prewrite;
    accepted = journal->accepted_sequence;
    CHECK(ext4_fpwrite_mapped_try(&live, &borrowed, 1) == ENOTSUP);
    CHECK(!borrowed.written && ext4_fsize(&live) == length && journal->accepted_sequence == accepted);
    dev->bdif->bwrite_batch = NULL;
    dev->bdif->bwrite_batch_try_phase = NULL;
    CHECK(ext4_fclose(&live) == EOK);
    check_prewrite_cache(dev, &file, input, length);
    CHECK(ext4_fpread(&file, 0, output, length, &got) == EOK && got == length);
    CHECK(memcmp(input, output, length) == 0);
#endif
    free(input); free(output);
    CHECK(ext4_fclose(&file) == EOK && ext4_journal_stop("/") == EOK);
    CHECK(ext4_umount("/") == EOK && fault_block_crash(&disk) == 0);
    puts("PASS occupied device phase: deferred data stayed ordered and matched after commit");
}

static void write_unwritten_range(struct ext4_blockdev *dev)
{
    CHECK(ext4_journal_start("/") == EOK);
    const struct ext4_journal_runtime runtime = {.now_ns = resource_now};
    CHECK(ext4_journal_group_enable("/", &runtime, 4 * 1024 * 1024) == EOK);
    ext4_file file;
    CHECK(ext4_fopen(&file, "/probe", "r+") == EOK);
    const size_t block = dev->lg_bsize, length = 3 * block + 19;
    unsigned char *input = malloc(length), *output = malloc(5 * block);
    CHECK(input && output);
    memset(input, 0x5a, length);
    struct resource_source resource = {.data = input};
    struct file_data_source source = {
        .context = &resource, .bytes = length, .retain = resource_retain, .release = resource_release,
    };
    struct file_write_span spans[] = {
        {.offset = block, .buffer = input, .size = block, .source = source},
        {.offset = 2 * block, .buffer = input + block, .size = length - block, .source = source},
    };
    bool changed;
    CHECK(ext4_fallocate(&file, 0, block, 5 * block, &changed, UINT32_MAX) == EOK && changed);
    CHECK(ext4_journal_group_commit("/", true) == EOK);
    unwritten_write.watch = unwritten_write.fail = true;
    CHECK(ext4_fpwrite_batch(&file, spans, 2) == EIO);
    CHECK(!spans[0].written && !spans[1].written && !resource.references);
    size_t read;
    CHECK(ext4_fpread(&file, block, output, 5 * block, &read) == EOK && read == 5 * block);
    for (size_t i = 0; i < read; i++) CHECK(output[i] == 0);

    unwritten_write.fail = false;
    unwritten_write.zeroes = unwritten_write.writes = 0;
    CHECK(ext4_fpwrite_batch(&file, spans, 2) == EOK);
    unwritten_write.watch = false;
    CHECK(spans[0].written == block && spans[1].written == length - block);
    CHECK(unwritten_write.writes == 4 && unwritten_write.zeroes == 1);
    CHECK(ext4_fpread(&file, block, output, 5 * block, &read) == EOK && read == 5 * block);
    CHECK(!memcmp(input, output, length));
    for (size_t i = length; i < read; i++) CHECK(output[i] == 0);
    CHECK(ext4_fclose(&file) == EOK);
    CHECK(ext4_journal_stop("/") == EOK);
    CHECK(ext4_umount("/") == EOK);
    CHECK(!resource.references);
    CHECK(fault_block_crash(&disk) == 0);
    CHECK(ext4_mount("journal-test", "/", true) == EOK);
    CHECK(ext4_fopen(&file, "/probe", "r") == EOK);
    CHECK(ext4_fpread(&file, block, output, 5 * block, &read) == EOK && read == 5 * block);
    CHECK(!memcmp(input, output, length));
    for (size_t i = length; i < read; i++) CHECK(output[i] == 0);
    CHECK(ext4_fclose(&file) == EOK);
    CHECK(ext4_umount("/") == EOK);
    free(output);
    free(input);
    CHECK(fault_block_crash(&disk) == 0);
    puts("PASS: full unwritten blocks use supplied data, partial tail stays zero, aborted conversion remains unwritten");
}

static uint64_t ordered_now;
static uint64_t ordered_clock(void *context) { (void)context; return ordered_now; }

static void count_commit_requests(void *context, unsigned work)
{
    if (work & EXT4_JOURNAL_WORK_COMMIT) (*(unsigned *)context)++;
}

static void metadata_age_test(struct ext4_blockdev *dev)
{
    CHECK(ext4_journal_start("/") == EOK);
    ext4_file file;
    CHECK(ext4_fopen(&file,"/removable","w") == EOK && ext4_fclose(&file) == EOK);
    CHECK(ext4_fopen(&file,"/probe","r+") == EOK);
    unsigned requests = 0;
    const struct ext4_journal_runtime runtime = {
        .context = &requests, .now_ns = ordered_clock, .request = count_commit_requests,
    };
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024) == EOK);
    struct jbd_journal *journal = dev->fs->jbd_journal;
    struct ext4_journal_progress progress;
    const uint64_t start = 1;
    ordered_now = start;
    CHECK(ext4_file_set_mode(&file,0640) == EOK && requests == 1);
    uint64_t group = journal->accepted_sequence;
    ordered_now = start + UINT64_C(200000000);
    CHECK(ext4_journal_group_commit("/",false) == EOK);
    CHECK(journal->running && journal->running->sequence == group && journal->durable_sequence < group);
    ordered_now = start + UINT64_C(4900000000);
    CHECK(ext4_file_set_mode(&file,0600) == EOK && requests == 1);
    CHECK(journal->accepted_sequence == group);
    ordered_now = start + UINT64_C(5000000000);
    CHECK(ext4_journal_group_commit("/",false) == EOK && journal->durable_sequence == group);

    CHECK(ext4_file_set_mode(&file,0644) == EOK);
    uint64_t target;
    CHECK(ext4_file_sync_metadata_prepare(&file,false,&target) == EOK && target == group + 1);
    CHECK(ext4_journal_group_commit("/",false) == EOK && journal->durable_sequence == target);

    uint64_t first_dirty = ordered_now;
    CHECK(ext4_file_set_mode(&file,0640) == EOK);
    requests = 0;
    ordered_now += UINT64_C(30000000);
    CHECK(ext4_fremove("/removable") == EOK);
    CHECK(journal->running && !LIST_EMPTY(&journal->running->quarantine));
    CHECK(!requests && ext4_journal_group_progress("/",&progress) == EOK);
    CHECK(!progress.commit_ready && progress.deadline_ns == first_dirty + UINT64_C(5000000000));
    ordered_now = first_dirty + UINT64_C(100000000);
    CHECK(ext4_journal_group_commit("/",false) == EOK && journal->durable_sequence == target);
    CHECK(journal->quarantined_inodes == 1);
    CHECK(ext4_journal_group_drain("/") == EOK && journal->durable_sequence == target + 1);
    CHECK(!journal->quarantined_inodes);

    /* An allocator reaching the held inode must drive its real checkpoint
     * even though the normal metadata commit deadline is still in the future. */
    struct ext4_mount_stats stats;
    ext4_file filler;
    for (unsigned i = 0;; i++) {
        CHECK(ext4_mount_point_stats("/", &stats) == EOK);
        if (stats.free_inodes_count == 1) break;
        CHECK(i < 64 && stats.free_inodes_count > 1);
        char name[32];
        snprintf(name, sizeof(name), "/reuse-fill-%u", i);
        CHECK(ext4_fopen(&filler, name, "w") == EOK && ext4_fclose(&filler) == EOK);
    }
    CHECK(ext4_journal_group_drain("/") == EOK);
    CHECK(ext4_fremove("/reuse-fill-0") == EOK);
    CHECK(journal->quarantined_inodes == 1);
    uint64_t reuse_target = journal->accepted_sequence;
    CHECK(ext4_journal_group_progress("/", &progress) == EOK && !progress.commit_ready);
    CHECK(ext4_fopen(&filler, "/reuse-now", "w") == EOK);
    CHECK(journal->checkpoint_sequence >= reuse_target && !journal->quarantined_inodes);
    CHECK(ext4_fclose(&filler) == EOK);
    CHECK(ext4_fclose(&file) == EOK && ext4_journal_stop("/") == EOK);
    CHECK(ext4_umount("/") == EOK && fault_block_crash(&disk) == 0);
    puts("PASS metadata age cap, fixed-target sync and checkpoint-driven quarantine reuse");
}

static void ordered_idle_test(struct ext4_blockdev *dev)
{
    CHECK(ext4_journal_start("/") == EOK);
    ext4_file file;
    CHECK(ext4_fopen(&file,"/probe","r+") == EOK);
    unsigned block = dev->lg_bsize;
    CHECK(ext4_ftruncate(&file,block,UINT32_MAX) == EOK);
    const struct ext4_journal_runtime runtime = {.now_ns = ordered_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024) == EOK);
    unsigned char *bytes = malloc(block);
    CHECK(bytes);
    struct ext4_journal_progress p;
    size_t written;
    uint64_t start = 1;
    uint64_t group = 0;
    for (unsigned i = 0; i <= 5; i++) {
        ordered_now = start + UINT64_C(90000000) * i;
        memset(bytes, 'A' + i, block);
        CHECK(ext4_fpwrite(&file,0,bytes,block,&written) == EOK && written == block);
        CHECK(ext4_journal_group_progress("/",&p) == EOK);
        if (!i) group = p.accepted;
        uint64_t expected = start + (i < 5 ? UINT64_C(90000000) * i + UINT64_C(100000000) :
            UINT64_C(500000000));
        CHECK(p.accepted == group && p.durable < group && !p.commit_ready && p.deadline_ns == expected);
        CHECK(ext4_journal_group_commit("/",false) == EOK);
    }
    ordered_now = start + UINT64_C(500000000);
    CHECK(ext4_journal_group_progress("/",&p) == EOK && p.commit_ready);
    CHECK(ext4_journal_group_commit("/",false) == EOK);
    CHECK(ext4_journal_group_progress("/",&p) == EOK && p.durable == group);

    memset(bytes,'S',block);
    CHECK(ext4_fpwrite(&file,0,bytes,block,&written) == EOK && written == block);
    CHECK(ext4_journal_group_progress("/",&p) == EOK && !p.commit_ready);
    uint64_t target;
    CHECK(ext4_file_sync_metadata_prepare(&file,true,&target) == EOK && target == group + 1);
    CHECK(ext4_journal_group_progress("/",&p) == EOK && p.commit_ready);
    CHECK(ext4_journal_group_commit("/",false) == EOK);
    CHECK(ext4_journal_group_progress("/",&p) == EOK && p.durable == target);

    memset(bytes,'P',block);
    CHECK(ext4_fpwrite(&file,0,bytes,block,&written) == EOK && written == block);
    CHECK(ext4_journal_group_progress("/",&p) == EOK && !p.commit_ready);
    CHECK(ext4_journal_group_reclaim("/",true) == EOK);
    CHECK(ext4_journal_group_progress("/",&p) == EOK && p.durable == target + 1);
    CHECK(ext4_fpread(&file,0,bytes,block,&written) == EOK && written == block);
    for (unsigned i = 0; i < block; i++) CHECK(bytes[i] == 'P');
    free(bytes);
    CHECK(ext4_fclose(&file) == EOK);
    CHECK(ext4_journal_stop("/") == EOK);
    CHECK(ext4_umount("/") == EOK && fault_block_crash(&disk) == 0);
    puts("PASS rolling ordered idle, 500ms hard age, explicit sync and real reclaim targets");
}
#endif

static void resource_accept_data(struct jbd_journal *journal, ext4_fsblk_t home,
    struct resource_source *source, bool quarantine)
{
    struct jbd_trans *operation;
    struct file_data_source owner = {.context = source, .bytes = 64 * 1024,
        .retain = resource_retain, .release = resource_release};
    CHECK(jbd_journal_new_trans(journal, &operation) == EOK);
    CHECK(jbd_trans_write_data(operation, home, 0, source->data,
        journal->block_size, false, &owner) == EOK);
    if (quarantine) {
        test_operation_bind(journal->jbd_fs->inode_ref.fs, operation);
        CHECK(jbd_trans_quarantine(journal->jbd_fs->inode_ref.fs, home + 100, 1, false) == EOK);
        test_operation_bind(journal->jbd_fs->inode_ref.fs, NULL);
    }
    CHECK(jbd_trans_prepare_operation(operation) == EOK);
    CHECK(jbd_journal_accept(journal, operation, 0) == EOK);
}
static uint64_t direct_home;
static unsigned direct_home_writes;
#ifdef TEST_JBD_CREDIT_PARALLEL
static unsigned direct_paused,direct_resume;
static struct {struct jbd_journal *journal;struct resource_source *old,*replacement;ext4_fsblk_t home;} direct_replace;
static void *replace_direct_owner(void *argument)
{
    (void)argument;
    while (!__atomic_load_n(&direct_paused,__ATOMIC_ACQUIRE)) sched_yield();
    resource_accept_data(direct_replace.journal,direct_replace.home,direct_replace.replacement,false);
    CHECK(direct_replace.old->references==1); /* The pending DMA still borrows it. */
    __atomic_store_n(&direct_resume,1,__ATOMIC_RELEASE);
    return NULL;
}
#endif
static int reverse_data_phase(struct ext4_blockdev *dev,const struct ext4_block_write *first)
{
    const struct ext4_block_write *entries[128];unsigned count=0;
    for (const struct ext4_block_write *entry=first;entry;entry=entry->next) {
        CHECK(count<128);entries[count++]=entry;
    }
    while (count) {
        const struct ext4_block_write *entry=entries[--count];
        if (entry->offset==direct_home) {
#ifdef TEST_JBD_CREDIT_PARALLEL
            if (direct_replace.journal && direct_home_writes==2) {
                __atomic_store_n(&direct_paused,1,__ATOMIC_RELEASE);
                while (!__atomic_load_n(&direct_resume,__ATOMIC_ACQUIRE)) sched_yield();
            }
#endif
            direct_home_writes++;
        }
        CHECK(!(entry->offset%512) && !(entry->size%512));
        int error=write_device(dev,entry->data,entry->offset/512,entry->size/512);
        if (error) return error;
    }
    return EOK;
}

static bool mapped_device_busy;

static int try_mapped_phase(struct ext4_blockdev *dev, const struct ext4_block_write *first)
{ return mapped_device_busy ? EAGAIN : reverse_data_phase(dev, first); }

static void write_mapped_background(struct ext4_blockdev *dev)
{
    CHECK(ext4_journal_start("/") == EOK);
    const struct ext4_journal_runtime runtime = {.now_ns = resource_now};
    CHECK(ext4_journal_group_enable("/", &runtime, 4 * 1024 * 1024) == EOK);
    ext4_file file;
    CHECK(ext4_fopen(&file, "/mapped", "w+") == EOK);
    size_t block = dev->lg_bsize, done;
    unsigned char *input = malloc(2 * block), *output = malloc(2 * block);
    CHECK(input && output);
    memset(input, 'a', 2 * block);
    CHECK(ext4_fpwrite(&file, 0, input, 2 * block, &done) == EOK && done == 2 * block);
    CHECK(ext4_journal_group_commit("/", true) == EOK);
    CHECK(ext4_journal_group_checkpoint("/") == EOK);

    dev->bdif->bwrite_phase = reverse_data_phase;
    dev->bdif->bwrite_phase_try = try_mapped_phase;
    memset(input, 'b', block);
    CHECK(ext4_fpwrite(&file, 0, input, block, &done) == EOK && done == block);
    struct jbd_journal *journal = dev->fs->jbd_journal;
    CHECK(journal->running && journal->running->ordered_count == 1);
    struct resource_source owner = {.data = input + block};
    memset(input + block, 'c', block);
    struct file_write_span span = {.offset = block, .buffer = input + block, .size = block,
        .source = {.context = &owner, .bytes = block, .retain = resource_retain, .release = resource_release}};
    kernel_io_context_current()->background_reclaim = 1;
    uint64_t writes = disk.writes;
    mapped_device_busy = true;
    CHECK(ext4_fpwrite_mapped_try(&file, &span, 1) == EAGAIN &&
        !span.written && !owner.references && disk.writes == writes);
    mapped_device_busy = false;
    CHECK(ext4_fpwrite_mapped_try(&file, &span, 1) == EOK && span.written == block);
    CHECK(disk.writes > writes && !owner.references && journal->running->ordered_count == 1);
    CHECK(ext4_fpread(&file, 0, output, 2 * block, &done) == EOK && done == 2 * block);
    CHECK(!memcmp(output, input, block) && !memcmp(output + block, input + block, block));

    span.offset = 0;
    writes = disk.writes;
    CHECK(ext4_fpwrite_mapped_try(&file, &span, 1) == ENOTSUP && !span.written);
    CHECK(jbd_journal_freeze(journal) == EOK);
    CHECK(ext4_fpwrite_mapped_try(&file, &span, 1) == ENOTSUP && !span.written);
    CHECK(jbd_journal_select_stage(journal, JBD_JOURNAL_COMMIT) == JBD_JOURNAL_COMMIT);
    CHECK(ext4_fpwrite_mapped_try(&file, &span, 1) == ENOTSUP && !span.written);
    CHECK(ext4_fpwrite_batch(&file, &span, 1) == EOK && span.written == block);
    CHECK(disk.writes == writes && owner.references == 1 && journal->running->ordered_count == 1);
    kernel_io_context_current()->background_reclaim = 0;
    CHECK(jbd_journal_retire_stage(journal, JBD_JOURNAL_COMMIT, jbd_journal_submit(journal)) == EOK);
    CHECK(ext4_journal_group_commit("/", true) == EOK && !owner.references);
    CHECK(ext4_fpread(&file, 0, output, 2 * block, &done) == EOK && done == 2 * block);
    CHECK(!memcmp(output, input + block, block) && !memcmp(output + block, input + block, block));
    CHECK(ext4_fclose(&file) == EOK && ext4_journal_stop("/") == EOK);
    CHECK(ext4_umount("/") == EOK && fault_block_crash(&disk) == 0);
    free(input); free(output);
    puts("PASS mapped background overwrite: disjoint overwrite bypassed the log; an overlapping older write retained its order");
}

static void direct_ordered_data(struct ext4_blockdev *dev,struct jbd_journal *journal,
    ext4_file *file,ext4_fsblk_t home,unsigned fault)
{
    dev->fs->jbd_journal=journal;
#ifdef TEST_JBD_CREDIT_PARALLEL
    const struct ext4_lock locks={.state_lock=credit_state_lock,
        .state_read_lock=credit_state_lock,.state_unlock=credit_state_unlock};
    dev->fs->os_locks=&locks;
#endif
    CHECK(jbd_journal_group_init(journal,4*1024*1024)==EOK);
    struct resource_source source[4]={0};
    for (unsigned i=0;i<3;i++) {
        source[i].data=calloc(1,64*1024);CHECK(source[i].data);
        memset(source[i].data,'A'+i,journal->block_size);
        resource_accept_data(journal,home,&source[i],false);
        if (i<2) CHECK(jbd_journal_freeze(journal)==EOK);
        if (!i) CHECK(jbd_journal_select_stage(journal,JBD_JOURNAL_COMMIT)==JBD_JOURNAL_COMMIT);
    }
#ifdef TEST_JBD_CREDIT_PARALLEL
    pthread_t replacer;
    if (!fault) {
        source[3].data=calloc(1,64*1024);CHECK(source[3].data);
        memset(source[3].data,'D',journal->block_size);
        direct_replace=(__typeof__(direct_replace)){journal,&source[2],&source[3],home};
        direct_paused=direct_resume=0;
        CHECK(!pthread_create(&replacer,NULL,replace_direct_owner,NULL));
    }
#endif
    direct_home=home*dev->lg_bsize;direct_home_writes=0;
    dev->bdif->bwrite_phase=reverse_data_phase;
    uint64_t flushes=disk.flushes;
    if (fault) disk.fail_write=disk.writes+fault;
    int result=ext4_fdata_complete(file,0,8);
    if (fault) {
        CHECK(result==EIO && journal->error==EIO);
        for (unsigned i=0;i<3;i++) CHECK(source[i].references==1);
        unsigned writes=direct_home_writes;
        CHECK(jbd_journal_complete_data(journal,home,1)==EIO);
        CHECK(jbd_journal_submit(journal)==EIO);
        CHECK(direct_home_writes==writes && disk.flushes==flushes);
        CHECK(fault_block_crash(&disk)==0);
        puts("PASS direct ordered failure: sticky error prevents resubmission or durability success");
        return;
    }
    unsigned expected_writes=3;unsigned char expected='C';
#ifdef TEST_JBD_CREDIT_PARALLEL
    CHECK(!pthread_join(replacer,NULL));direct_replace.journal=NULL;
    CHECK(direct_paused && direct_resume && !source[2].references);
    expected_writes=4;expected='D';
#endif
    CHECK(result==EOK && direct_home_writes==expected_writes && disk.flushes==flushes);
    CHECK(!journal->durable_sequence && !journal->checkpoint_sequence);
    struct jbd_data_borrow borrow={0};
    CHECK(!jbd_journal_borrow_data(dev->fs,home,&borrow));
    unsigned char data[8];size_t done=0;
    CHECK(ext4_fpread(file,0,data,sizeof(data),&done)==EOK && done==sizeof(data));
    for (unsigned i=0;i<sizeof(data);i++) CHECK(data[i]==expected);
    CHECK(jbd_journal_freeze(journal)==EOK);
    do {
        CHECK(jbd_journal_submit(journal)==EOK);
        CHECK(jbd_journal_retire_stage(journal,JBD_JOURNAL_COMMIT,EOK)==EOK);
    } while (jbd_journal_select_stage(journal,JBD_JOURNAL_COMMIT)==JBD_JOURNAL_COMMIT);
    CHECK(direct_home_writes==expected_writes);
    journal->checkpoint_target=journal->durable_sequence;
    while (jbd_journal_select_stage(journal,JBD_JOURNAL_CHECKPOINT)==JBD_JOURNAL_CHECKPOINT) {
        CHECK(jbd_journal_checkpoint(journal)==EOK);
        CHECK(jbd_journal_retire_stage(journal,JBD_JOURNAL_CHECKPOINT,EOK)==EOK);
    }
    for (unsigned i=0;i<4;i++) {CHECK(!source[i].references);free(source[i].data);}
    close_normal_journal(dev,journal);
    CHECK(fault_block_crash(&disk)==0);
    puts("PASS direct ordered extent consumer: three versions, reversed phase completion, media reads, no added FLUSH or commit rewrite, owners returned");
}

static unsigned range_phases, range_requests;
static unsigned range_mount_readers;
static bool range_expect_shared;
static void range_serial_lock(void *context) { (void)context; CHECK(!range_mount_readers); }
static void range_read_lock(void *context) { (void)context; range_mount_readers++; }
static void range_unlock(void *context)
{ (void)context; if (range_mount_readers) range_mount_readers--; }
static bool range_serial_writer(void *context)
{ (void)context; return !range_mount_readers; }

static int range_data_phase(struct ext4_blockdev *dev, const struct ext4_block_write *first)
{
    if (range_expect_shared) CHECK(range_mount_readers == 1);
    range_phases++;
    for (const struct ext4_block_write *entry = first; entry; entry = entry->next)
        range_requests++;
    return reverse_data_phase(dev, first);
}

static void wide_ordered_range(struct ext4_blockdev *dev, struct jbd_journal *journal,
    ext4_file *file)
{
    enum { blocks = 96 };
    struct resource_source sources[blocks] = {0};
    struct ext4_inode_ref inode;
    dev->fs->jbd_journal = journal;
    CHECK(jbd_journal_group_init(journal,16*1024*1024)==EOK);
    CHECK(ext4_fs_get_inode_ref_read(dev->fs,file->inode,&inode)==EOK);
    for (unsigned i=0;i<blocks;i++) {
        ext4_fsblk_t home;
        CHECK(ext4_fs_get_inode_dblk_idx(&inode,i,&home,false)==EOK && home);
        sources[i].data=malloc(journal->block_size);
        CHECK(sources[i].data);
        memset(sources[i].data,i+1,journal->block_size);
        resource_accept_data(journal,home,&sources[i],false);
    }
    CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
    dev->bdif->bwrite_phase = range_data_phase;
    dev->bdif->write_phase_bytes = 512*1024;
    uint64_t flushes=disk.flushes;
    CHECK(ext4_fdata_complete(file,0,blocks*journal->block_size)==EOK);
    CHECK(range_phases==1 && range_requests==blocks && disk.flushes==flushes);
    CHECK(jbd_journal_freeze(journal)==EOK);
    CHECK(jbd_journal_select_stage(journal,JBD_JOURNAL_COMMIT)==JBD_JOURNAL_COMMIT);
    CHECK(jbd_journal_retire_stage(journal,JBD_JOURNAL_COMMIT,jbd_journal_submit(journal))==EOK);
    journal->checkpoint_target=journal->durable_sequence;
    while (jbd_journal_select_stage(journal,JBD_JOURNAL_CHECKPOINT)==JBD_JOURNAL_CHECKPOINT)
        CHECK(jbd_journal_retire_stage(journal,JBD_JOURNAL_CHECKPOINT,jbd_journal_checkpoint(journal))==EOK);
    for (unsigned i=0;i<blocks;i++) {
        char byte=0;
        size_t done;
        CHECK(ext4_fpread(file,i*journal->block_size,&byte,1,&done)==EOK &&
            done==1 && (unsigned char)byte==i+1);
        CHECK(!sources[i].references);
        free(sources[i].data);
    }
    CHECK(ext4_fclose(file)==EOK);
    close_normal_journal(dev,journal);
    CHECK(fault_block_crash(&disk)==0);
    puts("PASS wide ordered range: 96 separate buffers in one phase, contents and owners verified");
}

static void complete_ordered_range_batch(struct ext4_blockdev *dev, struct jbd_journal *journal,
    ext4_file *first, ext4_fsblk_t first_home, unsigned fault)
{
    ext4_file second, outside;
    CHECK(ext4_fopen(&second, "/again", "r+") == EOK);
    CHECK(ext4_fopen(&outside, "/outside", "r+") == EOK);
    ext4_file *files[] = {first, &second, &outside};
    ext4_fsblk_t homes[] = {first_home, 0, 0};
    for (unsigned i = 1; i < 3; i++) {
        struct ext4_inode_ref ref;
        CHECK(ext4_fs_get_inode_ref_read(dev->fs, files[i]->inode, &ref) == EOK);
        CHECK(ext4_fs_get_inode_dblk_idx(&ref, 0, &homes[i], false) == EOK && homes[i]);
        CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
    }
    const struct ext4_lock locks = {.lock=range_serial_lock, .unlock=range_unlock,
        .read_lock=range_read_lock, .serial_writer=range_serial_writer};
    CHECK(ext4_mount_setup_locks("/", &locks) == EOK);
    dev->fs->jbd_journal = journal;
    CHECK(jbd_journal_group_init(journal, 4 * 1024 * 1024) == EOK);
    struct resource_source source[3] = {0};
    for (unsigned version = 0; version < 3; version++) {
        source[version].data = calloc(1, 64 * 1024);
        CHECK(source[version].data);
        memset(source[version].data, 'A' + version, journal->block_size);
        for (unsigned i = 0; i < 3; i++) {
            if ((version == 0 && i == 1) || (version == 1 && i == 0)) continue;
            resource_accept_data(journal, homes[i], &source[version], false);
        }
        if (version < 2) CHECK(jbd_journal_freeze(journal) == EOK);
        if (!version) CHECK(jbd_journal_select_stage(journal, JBD_JOURNAL_COMMIT) == JBD_JOURNAL_COMMIT);
    }
    const struct ext4_file_data_range ranges[] = {{&second, 0, 8}, {first, 0, 8}};
    dev->bdif->bwrite_phase = range_data_phase;
    uint64_t flushes = disk.flushes;
    if (fault) disk.fail_write = disk.writes + fault;
    range_expect_shared = true;
    int result = ext4_fdata_complete_batch(ranges, 2);
    range_expect_shared = false;
    CHECK(!range_mount_readers);
    CHECK(disk.flushes == flushes && !journal->durable_sequence);
    if (fault) {
        CHECK(result == EIO && jbd_journal_error(journal) == EIO);
        CHECK(range_phases == 1 && range_requests == 2);
        CHECK(ext4_fdata_complete_batch(ranges, 2) == EIO && range_phases == 1);
        CHECK(fault_block_crash(&disk) == 0);
        puts("PASS ordered range batch failure: sticky error and no premature commit");
        return;
    }
    CHECK(result == EOK && range_phases == 2 && range_requests == 4);
    uint8_t *data = malloc(journal->block_size);
    CHECK(data);
    for (unsigned i = 0; i < 3; i++) {
        CHECK(ext4_blocks_get_direct(dev, data, homes[i], 1) == EOK);
        CHECK(data[0] == (i == 2 ? 'O' : 'C'));
    }
    free(data);
    CHECK(jbd_journal_freeze(journal) == EOK);
    do {
        CHECK(jbd_journal_submit(journal) == EOK);
        CHECK(jbd_journal_retire_stage(journal, JBD_JOURNAL_COMMIT, EOK) == EOK);
    } while (jbd_journal_select_stage(journal, JBD_JOURNAL_COMMIT) == JBD_JOURNAL_COMMIT);
    journal->checkpoint_target = journal->durable_sequence;
    while (jbd_journal_select_stage(journal, JBD_JOURNAL_CHECKPOINT) == JBD_JOURNAL_CHECKPOINT) {
        CHECK(jbd_journal_checkpoint(journal) == EOK);
        CHECK(jbd_journal_retire_stage(journal, JBD_JOURNAL_CHECKPOINT, EOK) == EOK);
    }
    for (unsigned i = 0; i < 3; i++) { CHECK(!source[i].references); free(source[i].data); }
    CHECK(ext4_fclose(&second) == EOK && ext4_fclose(&outside) == EOK);
    close_normal_journal(dev, journal);
    CHECK(fault_block_crash(&disk) == 0);
    puts("PASS ordered range batch: independent generations share a phase, repeated blocks stay ordered, excluded range intact");
}

static void resource_reclaim(struct ext4_blockdev *dev, struct jbd_journal *journal,
    ext4_fsblk_t home)
{
    struct resource_wait wait = {.journal = journal};
    struct ext4_journal_runtime runtime = {.context = &wait, .now_ns = resource_now,
        .wait = resource_wait};
    dev->fs->jbd_journal = journal;
    CHECK(ext4_journal_group_enable("/", &runtime, 4 * 1024 * 1024) == EOK);
    jbd_memory_set_cache_disabled(journal, true);
    /* An older durable owner cannot release the new ordered data loan. */
    struct jbd_trans *old;
    struct ext4_block block;
    CHECK(jbd_journal_new_trans(journal, &old) == EOK);
    CHECK(ext4_block_get(dev, &block, home + 1) == EOK);
    CHECK(jbd_trans_get_write_access_range(old, &block, 0, 32) == EOK);
    CHECK(jbd_trans_set_block_dirty_range(old, &block, 0, 32) == EOK);
    CHECK(ext4_block_set(dev, &block) == EOK);
    CHECK(jbd_trans_prepare_operation(old) == EOK && jbd_journal_accept(journal, old, 0) == EOK);
    CHECK(jbd_journal_freeze(journal) == EOK);
    CHECK(jbd_journal_select_stage(journal, JBD_JOURNAL_COMMIT) == JBD_JOURNAL_COMMIT);
    CHECK(jbd_journal_retire_stage(journal, JBD_JOURNAL_COMMIT, jbd_journal_submit(journal)) == EOK);
    CHECK(journal->durable_sequence == 1 && !journal->checkpoint_sequence);
    struct resource_source source = {.data = malloc(64 * 1024)};
    CHECK(source.data && ext4_blocks_get_direct(dev, source.data, home, 1) == EOK);
    resource_accept_data(journal, home, &source, false);
    CHECK(jbd_journal_freeze(journal) == EOK);
    resource_accept_data(journal, home, &source, true);
    CHECK(source.references == 1);
    struct jbd_journal_reclaim reuse;
    CHECK(jbd_journal_reclaim_goal(journal, JBD_JOURNAL_REUSE, &reuse) &&
        reuse.stage == JBD_JOURNAL_CHECKPOINT && reuse.sequence == 3);
    /* An already selected commit owns the same loan while doing its I/O. */
    CHECK(jbd_journal_select_stage(journal, JBD_JOURNAL_COMMIT) == JBD_JOURNAL_COMMIT);
    wait.expected = EXT4_JOURNAL_WAIT_DURABLE; wait.target = 2;
    CHECK(ext4_journal_group_reclaim("/", true) == EOK);
    CHECK(wait.calls == 1 && source.references == 1 && !journal->checkpoint_sequence);
    wait.target = 3;
    CHECK(ext4_journal_group_reclaim("/", true) == EOK);
    CHECK(wait.calls == 2 && source.references == 0 && !journal->checkpoint_sequence);
    /* Metadata/log ownership remains until checkpoint, even after durable. */
    wait.expected = EXT4_JOURNAL_WAIT_CHECKPOINT; wait.target = 1;
    CHECK(ext4_journal_group_reclaim("/", true) == EOK);
    CHECK(wait.calls == 3 && journal->checkpoint_sequence == 3);
    CHECK(ext4_journal_group_reclaim("/", true) == EOK && wait.calls == 3);
    free(source.data);
    close_normal_journal(dev, journal);
    CHECK(fault_block_crash(&disk) == 0);
    puts("PASS resource reclaim: durable data loans, shared source, older durable metadata, checkpoint retirement and no completed-target loop");
}

static void resource_admission(struct ext4_blockdev *dev, struct jbd_journal *journal,
    ext4_file *file, ext4_fsblk_t home, unsigned fault, bool defer)
{
    struct resource_wait wait = {.journal = journal, .expected = EXT4_JOURNAL_WAIT_DURABLE, .target = 1};
    struct ext4_journal_runtime runtime = {.context = &wait, .now_ns = resource_now,
        .wait = resource_wait};
    dev->fs->jbd_journal = journal;
    CHECK(ext4_journal_group_enable("/", &runtime, 4 * 1024 * 1024) == EOK);
    jbd_memory_set_cache_disabled(journal, true);
    struct resource_source old = {.data = calloc(1, 64 * 1024)}, next = {.data = calloc(1, 64 * 1024)};
    CHECK(old.data && next.data && ext4_blocks_get_direct(dev, old.data, home, 1) == EOK);
    memcpy(next.data, old.data, journal->block_size);
    resource_accept_data(journal, home, &old, false);
#ifdef TEST_JBD_CREDIT_PARALLEL
    /* A real heap OOM must return its allocation error without waiting for
     * unrelated retained memory or leaking the candidate transaction. */
    size_t occupied = jbd_memory_used(journal);
    credit_test = (__typeof__(credit_test)){.journal = journal, .paused = true, .fail_after = 1};
    credit_actor = 1;
    CHECK(ext4_transaction_begin("/") == ENOMEM);
    credit_actor = 0; credit_test.journal = NULL;
    CHECK(!wait.calls && jbd_memory_used(journal) == occupied && old.references == 1);
#endif
    struct file_data_source owner = {.context = &next, .bytes = 64 * 1024,
        .retain = resource_retain, .release = resource_release};
    struct file_write_span span = {.buffer = next.data, .size = 8, .source = owner,
        .source_buffer = next.data, .source_size = journal->block_size};
    size_t limit = jbd_memory_used(journal) + 16 * journal->block_size;
    jbd_memory_set_limit(journal, limit);
    const struct file_data_source *sources[] = {&owner};
    CHECK(jbd_journal_operation_resources(journal, NULL, 1, 0, sources, 1) == JBD_JOURNAL_MEMORY);
    if (fault == 1) disk.fail_write = disk.writes + 1;
    if (fault == 2) disk.fail_flush = disk.flushes + 1;
    if (defer) {
        struct file_writeback_wait pending = {0};
        CHECK(ext4_fpwrite_batch_wait(file, &span, 1, &pending) == EAGAIN);
        CHECK(pending.kind == wait.expected && pending.sequence == wait.target);
        CHECK(!wait.calls && !span.written && old.references == 1 && !next.references);
        CHECK(resource_wait(&wait, pending.sequence, pending.kind) == EOK);
    }
    int result = ext4_fpwrite_batch(file, &span, 1);
    if (fault) {
        CHECK(result == EIO && !span.written && journal->error == EIO && wait.calls == 1);
        CHECK(old.references == 1 && !next.references && journal->failed_trans == journal->committing);
        struct jbd_data_borrow borrow = {0};
        CHECK(jbd_journal_borrow_data(dev->fs, home, &borrow) && borrow.data == old.data);
        jbd_journal_release_borrow(&borrow);
        free(next.data);
        CHECK(fault_block_crash(&disk) == 0);
        puts("PASS resource admission EIO: original accepted source retained and readable; rejected source/operation owns nothing");
        return;
    }
    CHECK(result == EOK && span.written == 8);
    CHECK(wait.calls == 1 && !old.references && next.references == 1 && !journal->checkpoint_sequence);
    CHECK(jbd_memory_peak(journal) <= limit);
    /* The actual log deficit must select checkpoint even with ordered data. */
    struct jbd_trans *private;
    jbd_memory_set_limit(journal, 4 * 1024 * 1024);
    CHECK(jbd_journal_new_trans(journal, &private) == EOK);
    uint32_t maxlen = jbd_get32(&journal->jbd_fs->sb, maxlen);
    /* Use a short unused tail for this private reservation; restore the full
     * ring before any further I/O. No synthetic occupied/log credit state. */
    jbd_set32(&journal->jbd_fs->sb, maxlen, journal->first + 8);
    unsigned capacity = 8;
    uint32_t record = JBD_HAS_INCOMPAT_FEATURE(&journal->jbd_fs->sb, JBD_FEATURE_INCOMPAT_64BIT) ? 8 : 4;
    unsigned entries = (journal->block_size - sizeof(struct jbd_revoke_header)) / record;
    /* Reserve enough packed revokes to leave fewer than the operation's three
     * minimum log blocks. Private abort returns every credit and record. */
    unsigned revokes = (capacity - 4) * entries;
    for (unsigned i = 0; i < revokes; i++)
        CHECK(jbd_trans_revoke_block(private, i) == EOK);
    CHECK(jbd_trans_prepare_operation(private) == EOK);
    CHECK(jbd_journal_operation_resources(journal, NULL, 0, 0, NULL, 0) & JBD_JOURNAL_LOG);
    struct jbd_journal_reclaim goal;
    CHECK(jbd_journal_reclaim_goal(journal, JBD_JOURNAL_LOG, &goal) &&
        goal.stage == JBD_JOURNAL_CHECKPOINT && goal.sequence == 1);
    jbd_journal_free_trans(journal, private, true);
    jbd_set32(&journal->jbd_fs->sb, maxlen, maxlen);
    CHECK(!(jbd_journal_operation_resources(journal, NULL, 0, 0, NULL, 0) & JBD_JOURNAL_LOG));
    wait.expected = EXT4_JOURNAL_WAIT_CHECKPOINT; wait.target = journal->accepted_sequence;
    CHECK(ext4_journal_group_drain("/") == EOK && !next.references);
    free(old.data); free(next.data);
    close_normal_journal(dev, journal);
    CHECK(fault_block_crash(&disk) == 0);
    puts("PASS resource admission: real write spans wait only durable for quota; real log credits require checkpoint; abort returns credits and sources");
}

#ifdef TEST_JBD_CREDIT_PARALLEL
static void prepared_wire(struct ext4_blockdev *dev, struct jbd_journal *journal,
    ext4_fsblk_t home)
{
    const struct ext4_lock locks = {.state_lock = credit_state_lock,
        .state_read_lock = credit_state_lock, .state_unlock = credit_state_unlock};
    dev->fs->os_locks = &locks;
    jbd_memory_set_cache_disabled(journal, true);
    struct ext4_block original, private, visible;
    struct jbd_trans *operation;
    CHECK(ext4_block_get(dev, &original, home) == EOK);
    CHECK(jbd_journal_new_trans(journal, &operation) == EOK);
    CHECK(ext4_block_get(dev, &private, home) == EOK);
    CHECK(jbd_trans_get_write_access_range(operation, &private, 0, 32) == EOK);
    private.data[0] = 'W';
    CHECK(jbd_trans_set_block_dirty_range(operation, &private, 0, 32) == EOK);
    CHECK(ext4_block_set(dev, &private) == EOK);
    CHECK(jbd_trans_prepare_operation(operation) == EOK);
    credit_test.journal = journal; credit_test.paused = true;
    credit_test.fail_after = 1; credit_test.calls = 0; credit_actor = 1;
    int result = jbd_journal_commit_trans(journal, operation);
    credit_actor = 0; credit_test.journal = NULL;
    printf("prepared wire result=%d postprepare_heap_calls=%u expected=0/0\n",
        result, credit_test.calls);
    CHECK(result == EOK && !credit_test.calls && !jbd_journal_error(journal));
    CHECK(original.data[0] == 'O');
    CHECK(ext4_block_get(dev, &visible, home) == EOK && visible.data[0] == 'W');
    CHECK(ext4_block_set(dev, &visible) == EOK && ext4_block_set(dev, &original) == EOK);
    close_normal_journal(dev, journal);
    CHECK(fault_block_crash(&disk) == 0);
    puts("PASS prepared journal needs no encoding allocation; old reader and final owners survive");
}

static void publication_owners(struct ext4_blockdev *dev, struct jbd_journal *journal,
    ext4_fsblk_t home)
{
    const struct ext4_lock locks = {.state_lock = credit_state_lock,
        .state_read_lock = credit_state_lock, .state_unlock = credit_state_unlock};
    dev->fs->os_locks = &locks;
    dev->fs->jbd_journal = journal;
    CHECK(jbd_journal_group_init(journal, 4 * 1024 * 1024) == EOK);
    struct ext4_block original, older, private, visible;
    CHECK(ext4_block_get(dev, &original, home) == EOK && original.data[0] == 'O');
    for (unsigned round = 0; round < 3; round++) {
        struct jbd_trans *operation;
        CHECK(jbd_journal_new_trans(journal, &operation) == EOK);
        CHECK(ext4_block_get(dev, &private, home) == EOK);
        CHECK(jbd_trans_get_write_access_range(operation, &private, 0, 32) == EOK);
        unsigned char value = round < 2 ? 'A' + round : 'P';
        private.data[0] = value;
        CHECK(jbd_trans_set_block_dirty_range(operation, &private, 0, 32) == EOK);
        CHECK(ext4_block_set(dev, &private) == EOK);
        CHECK(jbd_trans_prepare_operation(operation) == EOK);
        bool patch = round == 2;
        if (!patch) publication_watch(original.buf, 0);
        struct ext4_block_payload *accepted = original.buf->current_payload;
        unsigned home_pins = original.buf->journal_pending;
        unsigned block_refs = original.buf->refctr;
        publication_owner.rebind_buffer = original.buf;
        publication_owner.rebinds = 0;
#ifdef TEST_JBD_RECORD_COPY
        private_rebind_cache = &original.buf->bc->metadata;
        private_rebind_cache_locks = 0;
        count_journal_pool_locks = true;
        journal_pool_locks = 0;
#endif
        CHECK(jbd_journal_accept(journal, operation, 0) == EOK && !publication_owner.buffer);
        CHECK(original.buf->current_payload == accepted ? patch : !patch);
        CHECK(original.buf->journal_pending == home_pins - (round != 0));
        CHECK(original.buf->refctr == block_refs - (round != 0));
#ifdef TEST_JBD_RECORD_COPY
        private_rebind_cache = NULL;
        count_journal_pool_locks = false;
        printf("publication cache locks: running-group merge=%u count=%u\n",
            round, private_rebind_cache_locks);
        CHECK(private_rebind_cache_locks == 1);
        printf("publication pool locks: running-group merge=%u count=%u\n",
            round, journal_pool_locks);
        if (patch) CHECK(journal_pool_locks == 2);
#endif
        publication_owner.rebind_buffer = NULL;
        printf("publication image rebinds: running-group merge=%u count=%u\n",
            round, publication_owner.rebinds);
        CHECK(publication_owner.rebinds == !patch);
        CHECK(original.data[0] == 'O');
        CHECK(ext4_block_get(dev, &visible, home) == EOK && visible.data[0] == value);
        if (!round) older = visible;
        else CHECK(ext4_block_set(dev, &visible) == EOK && older.data[0] == 'A');
    }
    CHECK(jbd_journal_freeze(journal) == EOK); credit_drain(journal);
    /* Both preparations miss the same index. B must find A's admitted
     * record, discard its candidate, and retain both admitted ranges. */
    struct jbd_trans *candidates[2];
    for (unsigned i = 0; i < 2; i++) {
        CHECK(jbd_journal_new_trans(journal, &candidates[i]) == EOK);
        CHECK(ext4_block_get(dev, &private, home) == EOK);
        CHECK(jbd_trans_get_write_access_range(candidates[i], &private, 128 + i * 32, 32) == EOK);
        private.data[128 + i * 32] = 'R' + i;
        CHECK(jbd_trans_set_block_dirty_range(candidates[i], &private, 128 + i * 32, 32) == EOK);
        CHECK(ext4_block_set(dev, &private) == EOK);
        CHECK(jbd_trans_prepare_operation(candidates[i]) == EOK);
        struct jbd_buf *jb = TAILQ_FIRST(&candidates[i]->buf_queue);
        CHECK(jb && !jb->block_rec && jb->reserved_rec);
    }
    for (unsigned i = 0; i < 2; i++) {
        publication_watch(original.buf, 0);
        CHECK(jbd_journal_accept(journal, candidates[i], 0) == EOK && !publication_owner.buffer);
    }
    CHECK(ext4_block_get(dev, &visible, home) == EOK && visible.data[128] == 'R' && visible.data[160] == 'S');
    CHECK(ext4_block_set(dev, &visible) == EOK);
    struct jbd_trans *pending;
    CHECK(jbd_journal_new_trans(journal, &pending) == EOK);
    CHECK(ext4_block_get(dev, &private, home) == EOK);
    CHECK(jbd_trans_get_write_access_range(pending, &private, 0, 32) == EOK);
    private.data[0] = 'C';
    CHECK(jbd_trans_set_block_dirty_range(pending, &private, 0, 32) == EOK);
    struct jbd_trans *sibling;
    CHECK(jbd_journal_new_trans(journal, &sibling) == EOK);
    CHECK(ext4_block_get(dev, &visible, home) == EOK);
    CHECK(jbd_trans_get_write_access_range(sibling, &visible, 32, 32) == EOK);
    visible.data[32] = 'D';
    CHECK(jbd_trans_set_block_dirty_range(sibling, &visible, 32, 32) == EOK);
    CHECK(ext4_block_set(dev, &visible) == EOK && jbd_trans_prepare_operation(sibling) == EOK);
    publication_watch(original.buf, 0);
    CHECK(jbd_journal_accept(journal, sibling, 0) == EOK && !publication_owner.buffer);
    CHECK(jbd_journal_freeze(journal) == EOK);
    credit_drain(journal);
    /* The newer accepted payload now has only its cache owner. The private
     * operation still owns a different baseline and must merge before publish
     * returns this last current reference; old readers retain their own bytes. */
    CHECK(original.buf->current_payload->references == 1);
    CHECK(jbd_trans_prepare_operation(pending) == EOK);
    publication_watch(original.buf, 0);
    CHECK(jbd_journal_accept(journal, pending, 0) == EOK && !publication_owner.buffer);
    CHECK(original.data[0] == 'O' && older.data[0] == 'A' && private.data[0] == 'C');
    CHECK(ext4_block_get(dev, &visible, home) == EOK && visible.data[0] == 'C' && visible.data[32] == 'D');
    CHECK(ext4_block_set(dev, &visible) == EOK && ext4_block_set(dev, &private) == EOK);
    CHECK(jbd_journal_freeze(journal) == EOK);
    credit_drain(journal);
    jbd_journal_group_fini(journal);

    struct jbd_trans *immediate;
    CHECK(jbd_journal_new_trans(journal, &immediate) == EOK);
    CHECK(ext4_block_get(dev, &private, home) == EOK);
    CHECK(jbd_trans_get_write_access_range(immediate, &private, 0, 32) == EOK);
    private.data[0] = 'E';
    CHECK(jbd_trans_set_block_dirty_range(immediate, &private, 0, 32) == EOK);
    CHECK(jbd_trans_prepare_operation(immediate) == EOK);
    publication_watch(original.buf, 0);
    uint64_t writes = disk.writes;
    /* An armed heap fault must remain unused after preparation. */
    credit_test.journal = journal; credit_test.paused = true;
    credit_test.fail_after = 1; credit_test.calls = 0; credit_actor = 1;
    CHECK(jbd_journal_commit_trans(journal, immediate) == EOK);
    credit_actor = 0; credit_test.journal = NULL;
    CHECK(!credit_test.calls && !publication_owner.buffer && publication_owner.observed == 7);
    CHECK(!jbd_journal_error(journal) && !journal->failed_trans && disk.writes > writes);
    CHECK(private.data[0] == 'E' && private.data[32] == 'D');
    CHECK(original.data[0] == 'O' && older.data[0] == 'A');
    CHECK(ext4_block_get(dev, &visible, home) == EOK && visible.data[0] == 'E' && visible.data[32] == 'D');
    CHECK(ext4_block_set(dev, &visible) == EOK && ext4_block_set(dev, &private) == EOK);
    CHECK(ext4_block_set(dev, &older) == EOK && ext4_block_set(dev, &original) == EOK);
    close_normal_journal(dev, journal);
    CHECK(fault_block_crash(&disk) == 0);
    printf("PASS publication owners block=%u: grouped first/merge/stale-cache-only, old readers, immediate commit with encoding heap disabled, all memory returned\n", dev->lg_bsize);
}
#endif

#ifdef TEST_JBD_CREDIT_PARALLEL
static struct {
    struct jbd_trans *owner;
    bool abort;
    unsigned waits, wakes;
} fresh_block_wait;

static void complete_fresh_on_wait(void *context)
{
    struct ext4_buf *block = context;
    struct jbd_trans *owner = fresh_block_wait.owner;
    CHECK(owner && block->loading && block->fresh_load && ++fresh_block_wait.waits == 1);
    fresh_block_wait.owner = NULL;
    if (fresh_block_wait.abort) jbd_journal_free_trans(owner->journal, owner, true);
    else CHECK(jbd_journal_accept(owner->journal, owner, 0) == EOK);
}

static void fresh_block_wake(void *context)
{
    struct ext4_buf *block = context;
    CHECK(!block->loading && !block->fresh_load);
    fresh_block_wait.wakes++;
}

static void fresh_block_test(struct ext4_blockdev *dev, struct jbd_journal *journal,
    uint64_t home, bool crash)
{
    CHECK(jbd_journal_group_init(journal, 4 * 1024 * 1024) == EOK);
    dev->fs->jbd_journal = journal;
    struct jbd_trans *denied;
    struct ext4_block retry;
    uint64_t first_read = reads;
    CHECK(jbd_journal_new_trans(journal, &denied) == EOK);
    size_t limit = jbd_memory_limit(journal);
    jbd_memory_set_limit(journal, jbd_memory_used(journal));
    test_operation_bind(dev->fs, denied);
    CHECK(ext4_trans_block_get_new(dev, &retry, home) == ENOMEM && !retry.payload && reads == first_read);
    test_operation_bind(dev->fs, NULL);
    jbd_journal_free_trans(journal, denied, true);
    jbd_memory_set_limit(journal, limit);
    CHECK(ext4_block_get(dev, &retry, home) == EOK && retry.data[0] == 'O' && reads == first_read + 1);
    CHECK(ext4_block_set(dev, &retry) == EOK);
    for (unsigned committed = 0; committed < 2; committed++) {
        ext4_bcache_invalidate_lba(dev->bc, home, 1);
        struct jbd_trans *owner;
        struct ext4_block private, waiting;
        CHECK(jbd_journal_new_trans(journal, &owner) == EOK);
        test_operation_bind(dev->fs, owner);
        uint64_t before = reads;
        CHECK(ext4_trans_block_get_new(dev, &private, home) == EOK && reads == before);
        CHECK(jbd_trans_set_block_dirty_range(owner, &private, 0, 32) == EINVAL);
        memset(private.data, 'N', dev->lg_bsize);
        CHECK(ext4_trans_set_block_dirty(private.buf) == EOK);
        CHECK(ext4_block_get(dev, &waiting, home) == EOK && waiting.payload == private.payload);
        CHECK(ext4_block_set(dev, &waiting) == EOK && ext4_block_set(dev, &private) == EOK);
        test_operation_bind(dev->fs, NULL);
        CHECK(jbd_trans_prepare_operation(owner) == EOK);
        fresh_block_wait = (__typeof__(fresh_block_wait)){.owner = owner, .abort = !committed};
        dev->bdif->wait_read = complete_fresh_on_wait;
        dev->bdif->wake_read = fresh_block_wake;
        int error;
        CHECK((committed ? ext4_block_get_batch(dev, &waiting, &home, &error, 1) :
            ext4_block_get(dev, &waiting, home)) == EOK);
        dev->bdif->wait_read = NULL;
        dev->bdif->wake_read = NULL;
        CHECK(fresh_block_wait.waits == 1 && fresh_block_wait.wakes == (committed ? 1U : 2U));
        CHECK(waiting.data[0] == (committed ? 'N' : 'O') && reads == before + !committed);
        if (committed) {
            /* A held accepted version is never overwritten by a new writer. */
            CHECK(jbd_journal_new_trans(journal, &owner) == EOK);
            test_operation_bind(dev->fs, owner);
            CHECK(ext4_trans_block_get_new(dev, &private, home) == EOK && private.data[0] == 'N');
            memset(private.data, 'X', dev->lg_bsize);
            CHECK(ext4_trans_set_block_dirty(private.buf) == EOK && ext4_block_set(dev, &private) == EOK);
            test_operation_bind(dev->fs, NULL);
            jbd_journal_free_trans(journal, owner, true);
            CHECK(waiting.data[0] == 'N');
        }
        CHECK(ext4_block_set(dev, &waiting) == EOK);
    }
    CHECK(jbd_journal_freeze(journal) == EOK);
    if (crash) {
        CHECK(jbd_journal_select_stage(journal, JBD_JOURNAL_COMMIT) == JBD_JOURNAL_COMMIT);
        CHECK(jbd_journal_retire_stage(journal, JBD_JOURNAL_COMMIT, jbd_journal_submit(journal)) == EOK);
        void *old = malloc(dev->lg_bsize);
        CHECK(old && ext4_blocks_get_direct(dev, old, home, 1) == EOK && *(uint8_t *)old == 'O');
        free(old);
        CHECK(fault_block_crash(&disk) == 0);
        return;
    }
    credit_drain(journal);
    close_normal_journal(dev, journal);
    CHECK(fault_block_crash(&disk) == 0);
    puts("PASS fresh block publication and abort transfer waiting readers without reading new contents from disk");
}

static void private_record_prepare(struct ext4_blockdev *dev,
    struct jbd_journal *journal, ext4_fsblk_t home)
{
    const struct ext4_lock locks = {.state_lock = credit_state_lock,
        .state_read_lock = credit_state_lock, .state_unlock = credit_state_unlock};
    dev->fs->os_locks = &locks; dev->fs->jbd_journal = journal;
    CHECK(jbd_journal_group_init(journal, 4 * 1024 * 1024) == EOK);
#ifdef TEST_JBD_RECORD_COPY
    /* Seed both caches and check the actual allocator lock and ownership. */
    struct jbd_image *image = jbd_image_alloc(journal);
    void *control = jbd_alloc(journal, sizeof(struct jbd_buf));
    CHECK(image && control);
    memset(control, 0xff, sizeof(struct jbd_buf));
    jbd_image_free(image);
    jbd_free(control);
    size_t used = jbd_memory_used(journal), cached = jbd_memory_cached(journal);
    void *reused = NULL;
    journal_pool_locks = 0;
    count_journal_pool_locks = true;
    CHECK(jbd_image_alloc_control(journal, sizeof(struct jbd_buf), &reused) == image);
    count_journal_pool_locks = false;
    CHECK(reused == control && journal_pool_locks == 1);
    CHECK(jbd_memory_used(journal) == used && jbd_memory_cached(journal) < cached);
    for (size_t i = 0; i < sizeof(struct jbd_buf); i++) CHECK(!((uint8_t *)reused)[i]);
    jbd_image_free(image);
    jbd_free(reused);
    CHECK(jbd_memory_cached(journal) == cached);

    /* A held reader keeps its own image, including when cache retention is
     * disabled and the other records must physically return their storage. */
    for (unsigned disabled = 0; disabled < 2; disabled++) {
        enum { records = 3 };
        struct jbd_buf *retired[records];
        for (unsigned i = 0; i < records; i++) {
            struct jbd_image *data = jbd_image_alloc_control(journal,
                sizeof(struct jbd_buf), &control);
            CHECK(data && control);
            retired[i] = control;
            retired[i]->after = data;
        }
        struct jbd_image *held = retired[1]->after;
        ((uint8_t *)held->data)[0] = 'H';
        ext4_block_payload_get(&held->payload);
        jbd_memory_set_cache_disabled(journal, disabled);
        used = jbd_memory_used(journal);
        cached = jbd_memory_cached(journal);
        size_t returned = records * jbd_allocation_charge(sizeof(struct jbd_buf)) +
            (records - 1) * jbd_image_charge(journal);
        journal_pool_locks = 0;
        count_journal_pool_locks = true;
        jbd_return_merged_buffers(retired, records);
        count_journal_pool_locks = false;
        CHECK(jbd_memory_used(journal) == used - (disabled ? returned : 0));
        CHECK(jbd_memory_cached(journal) == cached + (disabled ? 0 : returned));
        CHECK(disabled || journal_pool_locks == 1);
        CHECK(held->payload.references == 1 && ((uint8_t *)held->data)[0] == 'H');
        jbd_image_free(held);
    }
    jbd_memory_set_cache_disabled(journal, false);
    puts("PASS merged buffer retirement: shared pool lock, held reader and cache-disabled release");
#endif
    for (unsigned round = 0; round < 3; round++) {
        struct jbd_trans *operation;
        struct ext4_block blocks[4];
        CHECK(jbd_journal_new_trans(journal, &operation) == EOK);
        unsigned entries = credit_state_entries;
        for (unsigned i = 0; i < 4; i++) {
            CHECK(ext4_block_get(dev, &blocks[i], home + i) == EOK);
            CHECK(jbd_trans_get_write_access_range(operation, &blocks[i], 0, 32) == EOK);
            CHECK(jbd_trans_set_block_dirty_range(operation, &blocks[i], 0, 32) == EOK);
        }
        printf("record prepare round=%u private_LBAs=4 independent_state_acquires=%u expected=0\n",
            round, credit_state_entries - entries);
        CHECK(credit_state_entries == entries);
        /* Round one lets the previous accepted owner retire before prepare:
         * payload loans survive independently and reservation finds new records. */
        if (round == 1) {
            CHECK(jbd_journal_freeze(journal) == EOK); credit_drain(journal);
            CHECK(RB_EMPTY(&journal->block_rec_root));
        }
        entries = credit_state_entries;
        CHECK(jbd_trans_prepare_operation(operation) == EOK);
        CHECK(credit_state_entries == entries + 1);
        if (round == 2) {
            CHECK(jbd_journal_freeze(journal) == EOK); credit_drain(journal);
            struct jbd_buf *jb;
            TAILQ_FOREACH(jb, &operation->buf_queue, buf_node)
                CHECK(jb->block_rec && jb->block_rec->private_refs && !jb->block_rec->trans);
            jbd_journal_free_trans(journal, operation, true);
            CHECK(RB_EMPTY(&journal->block_rec_root));
        } else CHECK(jbd_journal_accept(journal, operation, 0) == EOK);
        for (unsigned i = 0; i < 4; i++) CHECK(ext4_block_set(dev, &blocks[i]) == EOK);
    }
    CHECK(!journal->log_reserved && !credit_state_depth);
    close_normal_journal(dev, journal);
    CHECK(fault_block_crash(&disk) == 0);
    puts("PASS private payloads need no independent STATE; four record identities join one prepare scope, retire-before/after-prepare and abort return owners");
}



static void wire_ownership(struct ext4_blockdev *dev,
    struct jbd_journal *journal, ext4_fsblk_t home, unsigned fault)
{
    CHECK(jbd_journal_group_init(journal, 4 * 1024 * 1024) == EOK);
    dev->fs->jbd_journal = journal;
    jbd_memory_set_cache_disabled(journal, true);
    struct jbd_image *commit = journal->commit_image, *checkpoint = journal->checkpoint_image;
    encoding_accept(dev, journal, home, 0, 'A');
    CHECK(journal->running->reserved_logs == 3 && journal->log_reserved == 3);
    CHECK(jbd_journal_freeze(journal) == EOK && !journal->log_reserved);
    CHECK(journal->sealed_count == 1 && journal->running == NULL);
    CHECK(jbd_journal_select_stage(journal, JBD_JOURNAL_COMMIT) == JBD_JOURNAL_COMMIT);
    if (fault == 1) disk.fail_write = disk.writes + 1;
    if (fault == 2) disk.fail_flush = disk.flushes + 1;
    wire_owner = (__typeof__(wire_owner)){.image = commit, .io_active = true};
    credit_test = (__typeof__(credit_test)){.journal = journal, .paused = true, .fail_after = 1};
    credit_actor = 1;
    int result = jbd_journal_submit(journal);
    credit_actor = 0; credit_test.journal = NULL; wire_owner.io_active = false;
    CHECK(result == (fault ? EIO : EOK) && !credit_test.calls && wire_owner.io_calls);
    CHECK(journal->commit_image == commit && journal->checkpoint_image == checkpoint);
    CHECK(jbd_journal_retire_stage(journal, JBD_JOURNAL_COMMIT, result) == result);
    if (fault) {
        CHECK(journal->failed_trans == journal->committing && journal->error == EIO);
        CHECK(commit->active && checkpoint->active && commit->payload.references);
        CHECK(fault_block_crash(&disk) == 0);
        printf("PASS wire owner block=%u fault=%u: failed journal retains real consumers and versions\n",
            dev->lg_bsize, fault);
        return;
    }
    credit_drain(journal);
    close_normal_journal(dev, journal);
    CHECK(fault_block_crash(&disk) == 0);
    puts("PASS wire consumer: scalar quota, immutable versions, no encoding allocation, DMA and final return");
}
#endif

static void private_access_oom(struct ext4_blockdev *dev, struct jbd_journal *journal,
    ext4_fsblk_t home)
{
    for(unsigned stage=0;stage<3;stage++) {
        jbd_memory_set_cache_disabled(journal,true);jbd_trim_cache(journal,0);
        size_t baseline=jbd_memory_used(journal),limit=jbd_memory_limit(journal);
        struct jbd_trans *operation;CHECK(jbd_journal_new_trans(journal,&operation)==EOK);
        test_operation_bind(dev->fs,operation);
        struct ext4_block client;
        CHECK(ext4_block_get(dev,&client,home)==EOK && !client.buf->journal_pending);
        struct ext4_block_payload *loan=client.payload;
        uint8_t *data=client.data;
        uint32_t refs=client.buf->refctr,payload_refs=loan->references;
        size_t room=0,words=(journal->block_size+2047U)/2048U;
        if(stage>0)room+=jbd_allocation_charge(sizeof(struct jbd_buf)+2*words*sizeof(uint64_t));
        if(stage>1)room+=jbd_image_charge(journal);
        uint64_t denials=jbd_memory_budget_failures(journal);
        jbd_memory_set_limit(journal,jbd_memory_used(journal)+room);
        CHECK(jbd_trans_get_write_access_range(operation,&client,0,32)==ENOMEM);
        CHECK(jbd_memory_budget_failures(journal)>denials && operation->operation.memory_reservation_failed);
        CHECK(TAILQ_EMPTY(&operation->buf_queue) && !operation->data_cnt && !client.buf->journal_pending);
        CHECK(client.payload==loan && client.data==data && client.buf->refctr==refs && loan->references==payload_refs);
        jbd_memory_set_limit(journal,limit);
        CHECK(ext4_block_set(dev,&client)==EOK);
        test_operation_bind(dev->fs,NULL);jbd_journal_free_trans(journal,operation,true);
        struct jbd_memory_stats after;jbd_memory_snapshot(journal->memory,&after);
        CHECK(after.used==baseline && !after.reserved && !after.cached);
        jbd_memory_set_cache_disabled(journal,false);
    }
#ifdef TEST_JBD_RECORD_COPY
    struct jbd_trans *operation;
    CHECK(jbd_journal_new_trans(journal, &operation) == EOK);
    test_operation_bind(dev->fs, operation);
    struct ext4_block client;
    CHECK(ext4_block_get(dev, &client, home) == EOK && !client.buf->journal_pending);
    private_rebind_cache = &client.buf->bc->metadata;
    private_rebind_cache_locks = 0;
    CHECK(jbd_trans_get_write_access_range(operation, &client, 0, 32) == EOK);
    private_rebind_cache = NULL;
    CHECK(private_rebind_cache_locks == 1 && client.buf->journal_pending == 1);
    CHECK(ext4_block_set(dev, &client) == EOK);
    test_operation_bind(dev->fs, NULL);
    jbd_journal_free_trans(journal, operation, true);
#endif
}

static void private_prepare_oom(struct ext4_blockdev *dev, struct jbd_journal *journal,
    ext4_fsblk_t home)
{
    for (unsigned scenario = 0; scenario < 3; scenario++) {
        jbd_memory_set_cache_disabled(journal, true); jbd_trim_cache(journal, 0);
        size_t baseline = jbd_memory_used(journal), limit = jbd_memory_limit(journal);
        unsigned credits = journal->log_reserved;
        struct jbd_trans *operation; CHECK(jbd_journal_new_trans(journal, &operation) == EOK);
        test_operation_bind(dev->fs, operation);
        bool typed = scenario == 1;
        uint32_t offset = typed ? EXT4_SUPERBLOCK_OFFSET % dev->lg_bsize : dev->lg_bsize - 32;
        uint32_t length = typed ? sizeof(struct ext4_sblock) : 32;
        struct ext4_block block;
        CHECK(ext4_block_get(dev, &block, typed ? EXT4_SUPERBLOCK_OFFSET / dev->lg_bsize :
            home + (scenario == 2 ? 16 : 0)) == EOK);
        CHECK(jbd_trans_get_write_access_range(operation, &block, offset, length) == EOK);
        uint8_t *original = malloc(length); CHECK(original); memcpy(original, block.data + offset, length);
        block.data[offset] ^= 1;
        CHECK(jbd_trans_set_block_dirty_range(operation, &block, offset, length) == EOK);
        if (typed) operation->operation.block_delta = -1;
        size_t used = jbd_memory_used(journal);
        uint64_t denials = jbd_memory_budget_failures(journal);
        jbd_memory_set_limit(journal, used);
        int result = jbd_trans_prepare_operation(operation);
        struct jbd_buf *jb = TAILQ_FIRST(&operation->buf_queue);
        CHECK(jb && !jb->reserved_rec);
        if (jb->block_rec) {
            CHECK(result == EOK && !operation->error);
            CHECK(jbd_memory_budget_failures(journal) == denials);
        } else {
            CHECK(result == ENOMEM && operation->error == ENOMEM);
            CHECK(jbd_memory_budget_failures(journal) > denials);
        }
        CHECK(journal->log_reserved == credits + operation->reserved_logs);
        jbd_memory_set_limit(journal, limit);
        test_operation_bind(dev->fs, NULL); jbd_journal_free_trans(journal, operation, true);
        CHECK(!memcmp(block.data + offset, original, length)); free(original);
        CHECK(ext4_block_set(dev, &block) == EOK);
        struct jbd_memory_stats after; jbd_memory_snapshot(journal->memory, &after);
        CHECK(after.used == baseline && !after.reserved && !after.cached);
        jbd_memory_set_cache_disabled(journal, false);
    }
}

static struct ext4_block private_super_interleave(struct ext4_blockdev *dev,
    struct jbd_journal *journal)
{
    uint64_t baseline = ext4_sb_get_free_blocks_cnt(&dev->fs->sb); CHECK(baseline >= 2);
    struct ext4_block held = EXT4_BLOCK_ZERO();
    struct jbd_trans *first = NULL;
    for (unsigned actor = 0; actor < 3; actor++) {
        struct jbd_trans *operation; CHECK(jbd_journal_new_trans(journal, &operation) == EOK);
        test_operation_bind(dev->fs, operation);
        operation->operation.block_delta = actor == 2 ? 2 : -1;
        struct ext4_block block;
        uint32_t offset = EXT4_SUPERBLOCK_OFFSET % dev->lg_bsize;
        uint64_t lba = EXT4_SUPERBLOCK_OFFSET / dev->lg_bsize;
        CHECK(ext4_block_get_range(dev, &block, lba, offset,
              sizeof(struct ext4_sblock), EXT4_BLOCK_READ) == EOK);
#ifdef TEST_JBD_RECORD_COPY
        record_copy_bytes = 0;
        count_record_copies = true;
#endif
        CHECK(jbd_trans_reserve_superblock(operation, &block) == EOK);
#ifdef TEST_JBD_RECORD_COPY
        count_record_copies = false;
        CHECK(!record_copy_bytes);
#endif
        if (!actor) {
            struct ext4_block reader;
            CHECK(ext4_block_get_range(dev, &reader, lba, offset,
                  sizeof(struct ext4_sblock), EXT4_BLOCK_READ) == EOK);
            CHECK(ext4_sb_get_free_blocks_cnt((void *)(reader.data + offset)) == baseline);
            CHECK(ext4_block_set(dev, &reader) == EOK);
        }
        CHECK(jbd_trans_prepare_operation(operation) == EOK);
        CHECK(TAILQ_FIRST(&operation->buf_queue)->block.payload != block.payload);
        test_operation_bind(dev->fs, NULL);
        if (!actor) { first = operation; held = block; continue; }
        CHECK(ext4_block_set(dev, &block) == EOK && jbd_journal_accept(journal, operation, 0) == EOK);
        if (actor == 1) {
            CHECK(ext4_sb_get_free_blocks_cnt(&dev->fs->sb) == baseline - 1);
            CHECK(jbd_journal_accept(journal, first, 0) == EOK);
            CHECK(ext4_sb_get_free_blocks_cnt((void *)(held.data + offset)) == baseline - 2);
        }
    }
    CHECK(ext4_sb_get_free_blocks_cnt(&dev->fs->sb) == baseline);
    CHECK(ext4_sb_get_free_blocks_cnt((void *)(held.data + EXT4_SUPERBLOCK_OFFSET % dev->lg_bsize)) == baseline - 2);
    return held;
}

static void private_super_conflict_case(struct ext4_blockdev *dev,
    struct jbd_journal *journal, uint32_t raw_offset, bool typed)
{
    uint32_t super_offset = EXT4_SUPERBLOCK_OFFSET % dev->lg_bsize;
    ext4_fsblk_t lba = EXT4_SUPERBLOCK_OFFSET / dev->lg_bsize;
    uint32_t start = typed && raw_offset < super_offset ? raw_offset : super_offset;
    uint32_t end = super_offset + sizeof(struct ext4_sblock);
    if (typed && raw_offset >= end) end = raw_offset + 32;
    uint32_t raw_length = typed ? 32 : sizeof(struct ext4_sblock);
    uint32_t competing_start = typed ? raw_offset : super_offset;
    uint32_t byte = typed ? raw_offset : super_offset + offsetof(struct ext4_sblock, mount_count);
    struct ext4_sblock accepted_super = dev->fs->sb;
    struct ext4_block visible;
    CHECK(ext4_block_get(dev, &visible, lba) == EOK);
    uint8_t *original = malloc(dev->lg_bsize); CHECK(original);
    memcpy(original, visible.data, dev->lg_bsize);
    CHECK(ext4_block_set(dev, &visible) == EOK);
    uint8_t private_value = original[byte] ^ 0x31, competing_value = original[byte] ^ 0x62;

    struct jbd_trans *first, *second;
    CHECK(jbd_journal_new_trans(journal, &first) == EOK);
    test_operation_bind(dev->fs, first);
    if (typed) first->operation.block_delta = -1;
    struct ext4_block held;
    CHECK(ext4_trans_block_get_range(dev, &held, lba, start, end - start) == EOK);
    held.data[byte] = private_value;
    if (!typed) ext4_sb_set_csum((void *)(held.data + super_offset));
    /* 一段连续 dirty run 跨 SB 边界，能抓住按整个 jb 或整个 run 跳过的实现。 */
    CHECK(jbd_trans_set_block_dirty_range(first, &held, start, end - start) == EOK);
    CHECK(jbd_trans_prepare_operation(first) == EOK);
    test_operation_bind(dev->fs, NULL);

    CHECK(jbd_journal_new_trans(journal, &second) == EOK);
    test_operation_bind(dev->fs, second);
    struct ext4_block other;
    CHECK(ext4_trans_block_get_range(dev, &other, lba, competing_start, raw_length) == EOK);
    other.data[byte] = competing_value;
    if (!typed) ext4_sb_set_csum((void *)(other.data + super_offset));
    CHECK(jbd_trans_set_block_dirty_range(second, &other, competing_start, raw_length) == EOK);
    CHECK(ext4_block_set(dev, &other) == EOK && jbd_trans_prepare_operation(second) == EOK);
    test_operation_bind(dev->fs, NULL);
    CHECK(jbd_journal_accept(journal, second, 0) == EOK);

    unsigned remaining_credits = journal->log_reserved - first->reserved_logs;
    CHECK(jbd_journal_accept(journal, first, 0) == EAGAIN);
    CHECK(!memcmp(&dev->fs->sb, &accepted_super, sizeof(accepted_super)));
    jbd_journal_free_trans(journal, first, true);
    CHECK(journal->log_reserved == remaining_credits && !jbd_journal_error(journal));
    CHECK(!memcmp(held.data + start, original + start, end - start));
    CHECK(ext4_block_set(dev, &held) == EOK);
    CHECK(ext4_block_get(dev, &visible, lba) == EOK && visible.data[byte] == competing_value);
    CHECK(ext4_block_set(dev, &visible) == EOK);

    /* raw-only SB 修改和 boot-area 冲突只用于本交错；恢复原字节后再持久化。 */
    struct jbd_trans *reset; CHECK(jbd_journal_new_trans(journal, &reset) == EOK);
    test_operation_bind(dev->fs, reset);
    CHECK(ext4_trans_block_get_range(dev, &other, lba, competing_start, raw_length) == EOK);
    memcpy(other.data + competing_start, original + competing_start, raw_length);
    CHECK(jbd_trans_set_block_dirty_range(reset, &other, competing_start, raw_length) == EOK);
    CHECK(ext4_block_set(dev, &other) == EOK && jbd_trans_prepare_operation(reset) == EOK);
    test_operation_bind(dev->fs, NULL);
    CHECK(jbd_journal_accept(journal, reset, 0) == EOK);
    CHECK(ext4_block_get(dev, &visible, lba) == EOK);
    CHECK(!memcmp(visible.data, original, dev->lg_bsize));
    CHECK(ext4_block_set(dev, &visible) == EOK);
    CHECK(!memcmp(&dev->fs->sb, &accepted_super, sizeof(accepted_super)));
    free(original);
}

static void private_super_conflicts(struct ext4_blockdev *dev, struct jbd_journal *journal)
{
    uint32_t offset = EXT4_SUPERBLOCK_OFFSET % dev->lg_bsize;
    if (dev->lg_bsize == 4096) {
        private_super_conflict_case(dev, journal, offset - 32, true);
        private_super_conflict_case(dev, journal, offset + sizeof(struct ext4_sblock), true);
    }
    private_super_conflict_case(dev, journal, offset, false);
}

static struct ext4_block private_prefix_interleave(struct ext4_blockdev *dev,
    struct jbd_journal *journal, ext4_fsblk_t home)
{
    struct jbd_trans *reset; CHECK(jbd_journal_new_trans(journal, &reset) == EOK);
    struct ext4_block block;
    CHECK(ext4_block_get(dev, &block, home) == EOK &&
        jbd_trans_get_write_access_range(reset, &block, 0, 32) == EOK);
    uint32_t normal = to_be32(0x12345678); memcpy(block.data, &normal, sizeof(normal));
    CHECK(jbd_trans_set_block_dirty_range(reset, &block, 0, 32) == EOK);
    CHECK(ext4_block_set(dev, &block) == EOK && jbd_trans_prepare_operation(reset) == EOK);
    CHECK(jbd_journal_accept(journal, reset, 0) == EOK);
    struct jbd_trans *first, *second;
    CHECK(jbd_journal_new_trans(journal, &first) == EOK && jbd_journal_new_trans(journal, &second) == EOK);
    struct ext4_block held;
    CHECK(ext4_block_get(dev, &held, home) == EOK &&
        jbd_trans_get_write_access_range(first, &held, 256, 32) == EOK);
    held.data[260] = 'P'; CHECK(jbd_trans_set_block_dirty_range(first, &held, 256, 32) == EOK);
    CHECK(jbd_trans_prepare_operation(first) == EOK);
    struct jbd_buf *jb = TAILQ_FIRST(&first->buf_queue);
    CHECK(!jb->before);
    CHECK(ext4_block_get(dev, &block, home) == EOK &&
        jbd_trans_get_write_access_range(second, &block, 0, 32) == EOK);
    memcpy(block.data, "\xc0\x3b\x39\x98", 4);
    CHECK(jbd_trans_set_block_dirty_range(second, &block, 0, 32) == EOK);
    CHECK(ext4_block_set(dev, &block) == EOK && jbd_trans_prepare_operation(second) == EOK);
    CHECK(jbd_journal_accept(journal, second, 0) == EOK);
    CHECK(jbd_journal_accept(journal, first, 0) == EOK);
    CHECK(held.data[260] == 'P' && !memcmp(held.data, "\xc0\x3b\x39\x98", 4));
    TAILQ_FOREACH(jb, &journal->running->buf_queue, buf_node)
        if (jb->block.lb_id == home) break;
    CHECK(jb && !jb->before);
    return held;
}

static void private_dirty_rebind(struct ext4_blockdev *dev,
    struct jbd_journal *journal, ext4_fsblk_t home)
{
    uint32_t middle = dev->lg_bsize / 2 - 32, clean = 96;
    struct ext4_block reader, a, b, alias;
    CHECK(ext4_block_get(dev, &reader, home) == EOK);
    uint8_t original[64], original_clean[32];
    memcpy(original, reader.data + middle, sizeof(original));
    memcpy(original_clean, reader.data + clean, sizeof(original_clean));
    struct jbd_trans *first, *second;
    CHECK(jbd_journal_new_trans(journal, &first) == EOK);
    CHECK(jbd_journal_new_trans(journal, &second) == EOK);
    CHECK(ext4_block_get(dev, &a, home) == EOK &&
        jbd_trans_get_write_access_range(first, &a, middle, 64) == EOK);
    struct jbd_buf *jb = TAILQ_FIRST(&first->buf_queue);
    struct ext4_block_payload *baseline = jb->block.payload, *private = a.payload;
    a.data[middle] = 0xa1; a.data[middle + 32] = 0xa2;
    CHECK(jbd_trans_set_block_dirty_range(first, &a, middle, 64) == EOK);
    CHECK(ext4_block_get(dev, &b, home) == EOK &&
        jbd_trans_get_write_access_range(second, &b, middle, 64) == EOK &&
        jbd_trans_get_write_access_range(second, &b, clean, 32) == EOK);
    b.data[middle] = 0xb1; b.data[middle + 32] = 0xb2; b.data[clean] = 0xb3;
    CHECK(jbd_trans_set_block_dirty_range(second, &b, middle, 64) == EOK &&
        jbd_trans_set_block_dirty_range(second, &b, clean, 32) == EOK);
    CHECK(ext4_block_set(dev, &b) == EOK && jbd_trans_prepare_operation(second) == EOK);
    CHECK(jbd_journal_accept(journal, second, 0) == EOK);
    CHECK(a.buf->current_payload != baseline && ext4_block_get(dev, &alias, home) == EOK);
    CHECK(alias.data[middle] == 0xb1 && alias.data[clean] == 0xb3);
    jbd_memory_set_cache_disabled(journal, true); jbd_trim_cache(journal, 0);
    size_t used = jbd_memory_used(journal), limit = jbd_memory_limit(journal);
    uint64_t denials = jbd_memory_budget_failures(journal);
    jbd_memory_set_limit(journal, used);
#ifdef TEST_JBD_RECORD_COPY
    private_rebind_cache = &dev->bc->metadata; private_rebind_cache_locks = 0;
#endif
    /* Both units are dirty, including bits 63/64 in a 4K block. Keep the
     * caller's bytes even after another owner accepted conflicting data. */
    CHECK(jbd_trans_get_write_access_range(first, &a, middle + 31, 2) == EOK);
    CHECK(jbd_trans_get_write_access_range(first, &alias, middle + 31, 2) == EOK);
    CHECK(a.payload == private && alias.payload == private && a.data == alias.data);
    CHECK(a.data[middle] == 0xa1 && a.data[middle + 32] == 0xa2);
    CHECK(jb->block.payload == baseline && !jb->before && jbd_memory_used(journal) == used);
    CHECK(jbd_memory_budget_failures(journal) == denials);
    CHECK(!memcmp(reader.data + middle, original, sizeof(original)));
#ifdef TEST_JBD_RECORD_COPY
    CHECK(private_rebind_cache_locks == 0);
#endif
    /* A resumed caller still needs the latest accepted clean record; dirty
     * elsewhere in this view cannot exempt the missing baseline allocation. */
    test_operation_bind(dev->fs, first);
    CHECK(jbd_trans_get_write_access_range(first, &alias, clean, 32) == ENOMEM);
    CHECK(!jb->before && jb->block.payload == baseline && alias.payload == private);
    CHECK(jbd_memory_budget_failures(journal) > denials);
#ifdef TEST_JBD_RECORD_COPY
    CHECK(private_rebind_cache_locks > 0); private_rebind_cache = NULL;
#endif
    jbd_memory_set_limit(journal, limit); jbd_memory_set_cache_disabled(journal, false);
    CHECK(jbd_trans_get_write_access_range(first, &alias, clean, 32) == EOK && jb->before);
    CHECK(alias.data[clean] == 0xb3 && a.data[middle] == 0xa1 && a.data[middle + 32] == 0xa2);
    CHECK(!memcmp(reader.data + clean, original_clean, sizeof(original_clean)));
    test_operation_bind(dev->fs, NULL);
    CHECK(jbd_trans_prepare_operation(first) == EOK);
    CHECK(jbd_journal_accept(journal, first, 0) == EAGAIN);
    jbd_journal_free_trans(journal, first, true);
    CHECK(!memcmp(a.data + middle, original, sizeof(original)) && alias.data[clean] == 0xb3);
    CHECK(ext4_block_set(dev, &a) == EOK && ext4_block_set(dev, &alias) == EOK);
    CHECK(jbd_journal_new_trans(journal, &first) == EOK && ext4_block_get(dev, &a, home) == EOK);
    CHECK(jbd_trans_get_write_access_range(first, &a, middle, 64) == EOK &&
        jbd_trans_get_write_access_range(first, &a, clean, 32) == EOK);
    memcpy(a.data + middle, original, sizeof(original));
    memcpy(a.data + clean, original_clean, sizeof(original_clean));
    CHECK(jbd_trans_set_block_dirty_range(first, &a, middle, 64) == EOK &&
        jbd_trans_set_block_dirty_range(first, &a, clean, 32) == EOK);
    CHECK(ext4_block_set(dev, &a) == EOK && ext4_block_set(dev, &reader) == EOK);
    CHECK(jbd_trans_prepare_operation(first) == EOK && jbd_journal_accept(journal, first, 0) == EOK);
}

static void private_owner_borrow(struct ext4_blockdev *dev,
    struct jbd_journal *journal, ext4_fsblk_t home)
{
    struct ext4_block reader, first, alias;
    CHECK(ext4_block_get(dev, &reader, home) == EOK);
    uint8_t original = reader.data[0];
    struct jbd_trans *operation;
    CHECK(jbd_journal_new_trans(journal, &operation) == EOK);
    test_operation_bind(dev->fs, operation);
    CHECK(ext4_trans_block_get(dev, &first, home) == EOK);
    first.data[0] = original ^ 0x35;
    CHECK(ext4_trans_set_block_dirty_range(&first, 0, 32) == EOK);
    struct ext4_block_payload *payload = first.payload;
    unsigned references = first.buf->refctr;
    uint64_t before_reads = reads;
    fail_read = reads + 1;
#ifdef TEST_JBD_RECORD_COPY
    private_borrow_cache = dev->bc; private_borrow_lba = home; private_borrow_lookups = 0;
#endif
    CHECK(ext4_block_get(dev, &alias, home) == EOK);
    CHECK(alias.payload == payload && alias.data[0] == (uint8_t)(original ^ 0x35));
    CHECK(first.buf->refctr == references + 1 && reader.data[0] == original);
    CHECK(ext4_block_set(dev, &alias) == EOK && first.buf->refctr == references);
    CHECK(ext4_trans_block_get_range(dev, &alias, home, 0, 32) == EOK);
    CHECK(alias.payload == payload && first.buf->refctr == references + 1);
    CHECK(ext4_block_set(dev, &alias) == EOK && first.buf->refctr == references);
    CHECK(ext4_block_get_noread(dev, &alias, home) == EOK);
    CHECK(alias.payload == payload && alias.data[0] == (uint8_t)(original ^ 0x35));
    CHECK(ext4_block_set(dev, &alias) == EOK && first.buf->refctr == references);
    const uint64_t lbas[2] = {home,home};
    struct ext4_block batch[2]; int errors[2];
    CHECK(ext4_block_get_batch(dev, batch, lbas, errors, 2) == EOK);
    CHECK(!errors[0] && !errors[1] && batch[0].payload == payload && batch[1].payload == payload);
    CHECK(first.buf->refctr == references + 2 && batch[0].data[0] == (uint8_t)(original ^ 0x35));
    CHECK(ext4_block_set(dev, &batch[0]) == EOK && ext4_block_set(dev, &batch[1]) == EOK &&
        first.buf->refctr == references);
    CHECK(reads == before_reads);
#ifdef TEST_JBD_RECORD_COPY
    CHECK(private_borrow_lookups == 0); private_borrow_cache = NULL;
#endif
    unsigned device_refs = dev->bdif->ph_refctr;
    dev->bdif->ph_refctr = 0;
    alias = (struct ext4_block)EXT4_BLOCK_ZERO();
    CHECK(ext4_block_get(dev, &alias, home) == EIO && !alias.payload);
    CHECK(ext4_block_get_noread(dev, &alias, home) == EIO && !alias.payload);
    dev->bdif->ph_refctr = device_refs;
    CHECK(ext4_block_get(dev, &alias, dev->lg_bcnt) == ENXIO && !alias.payload);
    CHECK(ext4_block_get_noread(dev, &alias, dev->lg_bcnt) == ENXIO && !alias.payload);
    CHECK(ext4_block_get_range(dev, &alias, home, dev->lg_bsize, 1, EXT4_BLOCK_READ) == EINVAL && !alias.payload);
    CHECK(first.buf->refctr == references);
    fail_read = 0;
    CHECK(ext4_block_get(dev, &alias, home) == EOK && alias.payload == payload);
    test_operation_bind(dev->fs, NULL);
    jbd_journal_free_trans(journal, operation, true);
    CHECK(first.data[0] == original && alias.data[0] == original && reader.data[0] == original);
    CHECK(ext4_block_set(dev, &first) == EOK && ext4_block_set(dev, &alias) == EOK &&
        ext4_block_set(dev, &reader) == EOK);
    puts("PASS private metadata owner: repeated read/write/noread borrow directly, device/range errors and rollback return real references");
}

static void private_metadata_views(struct ext4_blockdev *dev,
    struct jbd_journal *journal, ext4_fsblk_t home)
{
    CHECK(jbd_journal_group_init(journal, 4 * 1024 * 1024) == EOK);
    dev->fs->jbd_journal = journal;
    private_owner_borrow(dev,journal,home);
    private_access_oom(dev,journal,home);
    struct jbd_trans *failed; CHECK(jbd_journal_new_trans(journal, &failed) == EOK);
    struct ext4_block held, returned;
    CHECK(ext4_block_get(dev, &held, home) == EOK);
    uint32_t references = held.buf->refctr, cached = dev->bc->ref_blocks;
    test_operation_bind(dev->fs, failed); failed->error = EIO;
    CHECK(ext4_trans_block_get_range(dev, &returned, home, 0, 32) == EIO);
    CHECK(!returned.payload && held.buf->refctr == references);
    CHECK(ext4_trans_block_get_range(dev, &returned, home, dev->lg_bsize, 32) == EINVAL);
    CHECK(!returned.payload && failed->error == EIO);
    CHECK(ext4_trans_block_get_range(dev, &returned, dev->lg_bcnt, 0, 32) == ENXIO);
    CHECK(!returned.payload && failed->error == EIO);
    fail_read = reads + 1;
    CHECK(ext4_trans_block_get_range(dev, &returned, dev->lg_bcnt - 1, 0, 32) == EIO);
    CHECK(!returned.payload && dev->bc->ref_blocks == cached); fail_read = 0;
    test_operation_bind(dev->fs, NULL); jbd_journal_free_trans(journal, failed, true);
    CHECK(ext4_block_set(dev, &held) == EOK);
    struct jbd_trans *first; CHECK(jbd_journal_new_trans(journal, &first) == EOK);
    struct jbd_trans *second; CHECK(jbd_journal_new_trans(journal, &second) == EOK);
    CHECK(first && second);
    struct ext4_block a, b, visible;
    CHECK(ext4_block_get(dev, &a, home) == EOK);
    CHECK(jbd_trans_get_write_access(first, &a) == EOK);
    a.data[0] = 'A';
    CHECK(jbd_trans_set_block_dirty(first, &a) == EOK);
    CHECK(ext4_block_get(dev, &visible, home) == EOK);
    CHECK(visible.data[0] == 'O');
    CHECK(ext4_block_get(dev, &b, home) == EOK);
    CHECK(jbd_trans_get_write_access(second, &b) == EOK);
    b.data[1] = 'B';
    CHECK(jbd_trans_set_block_dirty(second, &b) == EOK);
    CHECK(ext4_block_set(dev, &b) == EOK);
    CHECK(jbd_trans_prepare_operation(second) == EOK);
    CHECK(jbd_journal_accept(journal, second, 0) == EOK);
    CHECK(visible.data[0] == 'O' && visible.data[1] == 'l');
    CHECK(ext4_block_set(dev, &visible) == EOK);
    jbd_journal_free_trans(journal, first, true);
    CHECK(a.data[0] == 'O');
    CHECK(ext4_block_set(dev, &a) == EOK);
    CHECK(ext4_block_get(dev, &visible, home) == EOK);
    CHECK(visible.data[0] == 'O' && visible.data[1] == 'B');
    CHECK(ext4_block_set(dev, &visible) == EOK);
#ifdef TEST_JBD_RECORD_COPY
    for (unsigned held = 0; held < 2; held++) {
        struct ext4_block previous, edit;
        CHECK(ext4_block_get(dev, &previous, home) == EOK);
        uintptr_t previous_version = (uintptr_t)previous.payload;
        uint8_t old_byte = previous.data[96];
        if (!held) CHECK(ext4_block_set(dev, &previous) == EOK);
        CHECK(jbd_journal_new_trans(journal, &first) == EOK);
        CHECK(ext4_block_get(dev, &edit, home) == EOK);
        CHECK(jbd_trans_get_write_access_range(first, &edit, 96, 32) == EOK);
        edit.data[96] = old_byte ^ 1;
        CHECK(jbd_trans_set_block_dirty_range(first, &edit, 96, 32) == EOK);
        CHECK(ext4_block_set(dev, &edit) == EOK);
        CHECK(jbd_trans_prepare_operation(first) == EOK);
        record_copy_bytes = 0;
        count_record_copies = true;
        CHECK(jbd_journal_accept(journal, first, 0) == EOK);
        count_record_copies = false;
        CHECK(record_copy_bytes == (held ? dev->lg_bsize - 32U : 32U));
        CHECK(ext4_block_get(dev, &visible, home) == EOK && visible.data[96] == (old_byte ^ 1));
        CHECK(((uintptr_t)visible.payload == previous_version) == !held);
        if (held) {
            CHECK(previous.data[96] == old_byte);
            CHECK(ext4_block_set(dev, &previous) == EOK);
        }
        CHECK(ext4_block_set(dev, &visible) == EOK);
    }
#endif
    CHECK(jbd_journal_new_trans(journal, &first) == EOK);
    CHECK(jbd_journal_new_trans(journal, &second) == EOK);
    CHECK(first && second);
    CHECK(ext4_block_get(dev, &a, home) == EOK);
    CHECK(jbd_trans_get_write_access(first, &a) == EOK);
    a.data[0] = 'C';
    CHECK(jbd_trans_set_block_dirty_range(first, &a, 0, 32) == EOK);
    CHECK(ext4_block_get(dev, &b, home) == EOK);
    CHECK(jbd_trans_get_write_access(second, &b) == EOK);
    b.data[32] = 'D';
    CHECK(jbd_trans_set_block_dirty_range(second, &b, 32, 32) == EOK);
    CHECK(ext4_block_set(dev, &b) == EOK);
    CHECK(jbd_trans_prepare_operation(second) == EOK);
    CHECK(jbd_journal_accept(journal, second, 0) == EOK);
    CHECK(ext4_block_get(dev, &visible, home) == EOK);
    CHECK(visible.data[0] == 'O' && visible.data[32] == 'D');
    /* Sealing the accepted group cannot consume the preparation owned by
     * first, which has not yet been accepted into any group. */
    CHECK(jbd_journal_freeze(journal) == EOK);
    CHECK(ext4_block_set(dev, &a) == EOK);
    CHECK(jbd_trans_prepare_operation(first) == EOK);
    CHECK(jbd_journal_accept(journal, first, 0) == EOK);
    CHECK(visible.data[0] == 'O' && visible.data[32] == 'D');
    /* Upgrading a reference held across acceptance starts a new private
     * view at current data, while earlier pure readers stay unchanged. */
    CHECK(jbd_journal_new_trans(journal, &first) == EOK);
    CHECK(first && jbd_trans_get_write_access(first, &visible) == EOK);
    CHECK(visible.data[0] == 'C' && visible.data[1] == 'B');
    visible.data[64] = 'E';
    CHECK(jbd_trans_set_block_dirty_range(first, &visible, 64, 32) == EOK);
    CHECK(ext4_block_set(dev, &visible) == EOK);
    CHECK(jbd_trans_prepare_operation(first) == EOK);
    CHECK(jbd_journal_accept(journal, first, 0) == EOK);
    CHECK(ext4_block_get(dev, &visible, home) == EOK);
    CHECK(visible.data[0] == 'C' && visible.data[1] == 'B' && visible.data[32] == 'D' && visible.data[64] == 'E');
    CHECK(ext4_block_set(dev, &visible) == EOK);
    CHECK(jbd_journal_new_trans(journal, &first) == EOK);
    CHECK(jbd_journal_new_trans(journal, &second) == EOK);
    CHECK(first && second && ext4_block_get(dev, &a, home) == EOK);
#ifdef TEST_JBD_RECORD_COPY
    record_copy_bytes = 0; count_record_copies = true;
#endif
    CHECK(jbd_trans_get_write_access_range(first, &a, 256, 256) == EOK);
    struct ext4_block_payload *baseline = TAILQ_FIRST(&first->buf_queue)->block.payload;
    CHECK(baseline == a.buf->current_payload && baseline != a.payload);
#ifdef TEST_JBD_RECORD_COPY
    count_record_copies = false; CHECK(record_copy_bytes == 256);
#endif
    a.data[260] = 'F';
    CHECK(ext4_block_get(dev, &b, home) == EOK);
#ifdef TEST_JBD_RECORD_COPY
    record_copy_bytes = 0; count_record_copies = true;
#endif
    CHECK(jbd_trans_get_write_access_range(second, &b, 0, 64) == EOK);
#ifdef TEST_JBD_RECORD_COPY
    count_record_copies = false; CHECK(record_copy_bytes == 64);
#endif
    memcpy(b.data, "\xc0\x3b\x39\x98", 4);
    CHECK(jbd_trans_set_block_dirty_range(second, &b, 0, 64) == EOK);
    CHECK(ext4_block_set(dev, &b) == EOK);
    CHECK(jbd_trans_prepare_operation(second) == EOK);
    CHECK(jbd_journal_accept(journal, second, 0) == EOK);
    struct ext4_block_payload *private_owner = a.payload;
    size_t repeated_access_memory = jbd_memory_used(journal);
    CHECK(jbd_trans_get_write_access_range(first, &a, 256, 256) == EOK);
    CHECK(a.payload == private_owner && a.data[260] == 'F');
    CHECK(TAILQ_FIRST(&first->buf_queue)->block.payload == baseline);
    CHECK(jbd_memory_used(journal) == repeated_access_memory);
    /* A full read must fill every untouched record after a range write;
     * the accepted prefix changed while this private operation stayed live. */
    test_operation_bind(dev->fs, first);
    jbd_memory_set_cache_disabled(journal, true); jbd_trim_cache(journal, 0);
    size_t limit = jbd_memory_limit(journal), used = jbd_memory_used(journal);
    jbd_memory_set_limit(journal, used);
    CHECK(ext4_block_get(dev, &visible, home) == ENOMEM && !visible.payload);
    CHECK(!TAILQ_FIRST(&first->buf_queue)->before && a.data[260] == 'F');
    CHECK(TAILQ_FIRST(&first->buf_queue)->block.payload == baseline);
    jbd_memory_set_limit(journal, limit); jbd_memory_set_cache_disabled(journal, false);
    CHECK(ext4_block_get(dev, &visible, home) == EOK);
    CHECK(!memcmp(visible.data, "\xc0\x3b\x39\x98", 4) && visible.data[260] == 'F');
    CHECK(a.buf->current_payload != baseline &&
        TAILQ_FIRST(&first->buf_queue)->block.payload == baseline);
    CHECK(TAILQ_FIRST(&first->buf_queue)->before);
    CHECK(visible.data[64] == 'E' && visible.data[dev->lg_bsize - 1] == 0);
    CHECK(ext4_block_set(dev, &visible) == EOK);
    test_operation_bind(dev->fs, NULL);
    CHECK(jbd_trans_set_block_dirty_range(first, &a, 256, 256) == EOK);
    CHECK(ext4_block_set(dev, &a) == EOK);
    CHECK(jbd_trans_prepare_operation(first) == EOK);
    CHECK(jbd_journal_accept(journal, first, 0) == EOK);
    CHECK(ext4_block_get(dev, &visible, home) == EOK);
    CHECK(!memcmp(visible.data, "\xc0\x3b\x39\x98", 4) && visible.data[260] == 'F');
    CHECK(ext4_block_set(dev, &visible) == EOK);
    CHECK(jbd_journal_new_trans(journal, &first) == EOK);
    CHECK(jbd_journal_new_trans(journal, &second) == EOK);
    uint32_t middle = dev->lg_bsize / 2 - 32, last = dev->lg_bsize - 32;
    CHECK(ext4_block_get(dev, &a, home) == EOK);
    CHECK(jbd_trans_get_write_access_range(first, &a, middle + 31, 2) == EOK);
    CHECK(jbd_trans_get_write_access_range(first, &a, dev->lg_bsize, 1) == EINVAL);
    CHECK(jbd_trans_set_block_dirty_range(first, &a, middle + 1, 32) == EINVAL);
    CHECK(jbd_trans_set_block_dirty_range(first, &a, middle, 33) == EINVAL);
    CHECK(jbd_trans_set_block_dirty_range(first, &a, dev->lg_bsize, 32) == EINVAL);
    CHECK(!TAILQ_FIRST(&first->buf_queue)->modified);
    a.data[middle] = 'G'; a.data[middle + 32] = 'H';
    CHECK(ext4_block_get(dev, &b, home) == EOK);
    CHECK(jbd_trans_get_write_access_range(second, &b, last, 32) == EOK);
    b.data[last] = 'L'; b.data[dev->lg_bsize - 1] = 'Z';
    CHECK(jbd_trans_set_block_dirty_range(second, &b, last, 32) == EOK);
    CHECK(ext4_block_set(dev, &b) == EOK);
    CHECK(jbd_trans_prepare_operation(second) == EOK);
    CHECK(jbd_journal_accept(journal, second, 0) == EOK);
    /* The held, not-yet-dirty pair crosses bits 63/64 for 4K blocks. A newer
     * accepted tail must fill the remaining view without replacing that pair. */
    test_operation_bind(dev->fs, first);
    CHECK(ext4_block_get(dev, &visible, home) == EOK);
    CHECK(visible.data[middle] == 'G' && visible.data[middle + 32] == 'H');
    CHECK(visible.data[last] == 'L' && visible.data[dev->lg_bsize - 1] == 'Z');
    CHECK(ext4_block_set(dev, &visible) == EOK);
    test_operation_bind(dev->fs, NULL);
    CHECK(jbd_trans_set_block_dirty_range(first, &a, middle, 64) == EOK);
    CHECK(jbd_trans_set_block_dirty_range(first, &a, dev->lg_bsize, 0) == EOK);
    CHECK(ext4_block_set(dev, &a) == EOK);
    CHECK(jbd_trans_prepare_operation(first) == EOK);
    CHECK(jbd_journal_accept(journal, first, 0) == EOK);
    CHECK(ext4_block_get(dev, &visible, home) == EOK);
    CHECK(visible.data[middle] == 'G' && visible.data[middle + 32] == 'H');
    CHECK(visible.data[last] == 'L' && visible.data[dev->lg_bsize - 1] == 'Z');
    CHECK(ext4_block_set(dev, &visible) == EOK);
    CHECK(jbd_journal_new_trans(journal, &first) == EOK);
    CHECK(jbd_journal_new_trans(journal, &second) == EOK);
    CHECK(ext4_block_get(dev, &a, home) == EOK &&
        jbd_trans_get_write_access_range(first, &a, middle, 64) == EOK);
    a.data[middle] = 'I'; a.data[middle + 32] = 'J';
    CHECK(ext4_block_get(dev, &b, home) == EOK &&
        jbd_trans_get_write_access_range(second, &b, last, 32) == EOK);
    b.data[last] = 'M'; CHECK(jbd_trans_set_block_dirty_range(second, &b, last, 32) == EOK);
    CHECK(ext4_block_set(dev, &b) == EOK && jbd_trans_prepare_operation(second) == EOK);
    CHECK(jbd_journal_accept(journal, second, 0) == EOK);
    test_operation_bind(dev->fs, first);
    CHECK(ext4_block_get(dev, &visible, home) == EOK && TAILQ_FIRST(&first->buf_queue)->before);
    CHECK(visible.data[middle] == 'I' && visible.data[middle + 32] == 'J');
    test_operation_bind(dev->fs, NULL); jbd_journal_free_trans(journal, first, true);
    CHECK(a.data[middle] == 'G' && a.data[middle + 32] == 'H' && a.data[last] == 'M');
    CHECK(visible.data[middle] == 'G' && visible.data[middle + 32] == 'H');
    CHECK(ext4_block_set(dev, &a) == EOK && ext4_block_set(dev, &visible) == EOK);
    private_dirty_rebind(dev, journal, home);
    private_prepare_oom(dev, journal, home);
    private_super_conflicts(dev, journal);
    struct ext4_block held_super = private_super_interleave(dev, journal);
    struct ext4_block held_wire = private_prefix_interleave(dev, journal, home);
    CHECK(jbd_journal_freeze(journal) == EOK);
    CHECK(!memcmp(held_wire.data, "\xc0\x3b\x39\x98", 4));
    while (journal->sealed_count) {
        CHECK(jbd_journal_select_stage(journal,JBD_JOURNAL_COMMIT)==JBD_JOURNAL_COMMIT);
        CHECK(jbd_journal_submit(journal) == EOK);
        CHECK(jbd_journal_retire_stage(journal,JBD_JOURNAL_COMMIT,EOK) == EOK);
    }
    journal->checkpoint_target = journal->accepted_sequence;
    while (!TAILQ_EMPTY(&journal->cp_queue)) {
        CHECK(jbd_journal_select_stage(journal,JBD_JOURNAL_CHECKPOINT)==JBD_JOURNAL_CHECKPOINT);
        CHECK(jbd_journal_checkpoint(journal) == EOK);
        CHECK(jbd_journal_retire_stage(journal,JBD_JOURNAL_CHECKPOINT,EOK) == EOK);
    }
    CHECK(!memcmp(held_wire.data, "\xc0\x3b\x39\x98", 4));
    CHECK(ext4_block_set(dev, &held_wire) == EOK && ext4_block_set(dev, &held_super) == EOK);
    close_normal_journal(dev, journal);
    CHECK(fault_block_crash(&disk) == 0);
    puts("PASS: private/accepted metadata, same-block record merge, stable reads and full resource return");
}

static void group_revoke_reservation(struct ext4_blockdev *dev,
    struct jbd_journal *journal, ext4_fsblk_t home, unsigned fault)
{
    unsigned capacity = to_be32(journal->jbd_fs->sb.maxlen) - journal->first;
    unsigned revokes = capacity + 64;
    CHECK(revokes < dev->lg_bcnt / 2);
    size_t baseline_memory = jbd_memory_used(journal);
    CHECK(jbd_journal_group_init(journal, 4 * 1024 * 1024) == EOK);
    dev->fs->jbd_journal = journal;
    uint64_t writes = disk.writes;
    struct jbd_trans *operation; CHECK(jbd_journal_new_trans(journal, &operation) == EOK);
    CHECK(operation != NULL);
    /* Revoke entries occupy packed control blocks. Their count may exceed
     * journal capacity without requiring one log block for every entry. */
    for (unsigned i = 0; i < revokes; i++)
        CHECK(jbd_trans_revoke_block(operation, dev->lg_bcnt - 1 - i) == EOK);
    jbd_journal_free_trans(journal, operation, true);
    CHECK(journal->log_reserved == 0 && disk.writes == writes);
    jbd_journal_group_fini(journal);
    CHECK(jbd_memory_used(journal) == baseline_memory);

    CHECK(jbd_journal_group_init(journal, 4 * 1024 * 1024) == EOK);
    /* Keep the first half running while a second private operation needs
     * the next packed revoke block and reuses the same metadata version. */
    for (unsigned half = 0; half < 2; half++) {
        CHECK(jbd_journal_new_trans(journal, &operation) == EOK);
        CHECK(operation != NULL);
        struct ext4_block block;
        CHECK(ext4_block_get(dev, &block, home) == EOK);
        CHECK(jbd_trans_get_write_access(operation, &block) == EOK);
        memset(block.data, 'N', dev->lg_bsize);
        CHECK(jbd_trans_set_block_dirty(operation, &block) == EOK);
        CHECK(ext4_block_set(dev, &block) == EOK);
        for (unsigned i = half * revokes / 2; i < (half + 1) * revokes / 2; i++)
            CHECK(jbd_trans_revoke_block(operation, dev->lg_bcnt - 1 - i) == EOK);
        CHECK(jbd_trans_prepare_operation(operation) == EOK);
        CHECK(jbd_journal_accept(journal, operation, 0) == EOK);
    }
    CHECK(disk.writes == writes);
    CHECK(jbd_journal_freeze(journal) == EOK && jbd_journal_select(journal));
    bool fail = fault == 1 || fault == 2;
    if (fail) disk.fail_flush = disk.flushes + fault;
    int result = jbd_journal_submit(journal);
    CHECK(result == (fail ? EIO : EOK));
    CHECK(jbd_journal_retire(journal, result) == result);
    if (fail) {
        CHECK(journal->failed_trans == journal->committing);
        CHECK(journal->error == EIO);
    } else if (fault == 3) {
        CHECK(jbd_journal_select(journal));
        CHECK(jbd_journal_checkpoint(journal) == EOK);
        CHECK(jbd_journal_retire(journal, EOK) == EOK);
        close_normal_journal(dev, journal);
    } /* fault=0 crashes with the committed packed records still in the log. */
    CHECK(fault_block_crash(&disk) == 0);
    printf("group revoke reservation: block=%u entries=%u abort/merge/retire fault=%u\n",
        dev->lg_bsize, revokes, fault);
}

/* Edit actual on-disk records independently of the writer/replay code. */
static void mutate_log(struct ext4_blockdev *dev, struct jbd_fs *jfs,
                       const char *kind)
{
    uint32_t sequence = to_be32(jfs->sb.sequence);
    if (!strncmp(kind, "second-", 7)) { sequence++; kind += 7; }
    uint32_t wanted = !strncmp(kind, "revoke", 6) ? JBD_REVOKE_BLOCK :
                      !strncmp(kind, "commit", 6) ? JBD_COMMIT_BLOCK :
                      JBD_DESCRIPTOR_BLOCK;
    unsigned char *data = malloc(dev->lg_bsize);
    CHECK(data != NULL);
    uint32_t position = to_be32(jfs->sb.start);
    for (; position < to_be32(jfs->sb.maxlen); position++) {
        ext4_fsblk_t physical;
        CHECK(jbd_inode_bmap(jfs, position, &physical) == EOK);
        CHECK(ext4_blocks_get_direct(dev, data, physical, 1) == EOK);
        struct jbd_bhdr *hdr = (void *)data;
        if (to_be32(hdr->magic) != JBD_MAGIC_NUMBER ||
            to_be32(hdr->blocktype) != wanted || to_be32(hdr->sequence) != sequence) continue;
        bool repair_checksum = false;
        if (!strcmp(kind, "data-csum")) {
            CHECK(jbd_inode_bmap(jfs, position + 1, &physical) == EOK);
            CHECK(ext4_blocks_get_direct(dev, data, physical, 1) == EOK);
            data[100] ^= 0x80;
        } else if (!strcmp(kind, "commit-zero")) {
            memset(data, 0, dev->lg_bsize);
        } else if (!strcmp(kind, "commit-padding")) {
            data[dev->lg_bsize - 1] ^= 0x80;
        } else if (!strncmp(kind, "commit-time", 11)) {
            struct jbd_commit_header *commit = (void *)data;
            commit->commit_sec = to_be64(!strcmp(kind, "commit-time-high") ? 100 : 50);
            commit->chksum[0] = 0;
            uint32_t sum = ext4_crc32c(EXT4_CRC32_INIT, jfs->sb.uuid, UUID_SIZE);
            commit->chksum[0] = to_be32(ext4_crc32c(sum, data, dev->lg_bsize));
        } else if (!strcmp(kind, "commit-csum")) {
            ((struct jbd_commit_header *)data)->chksum[0] ^= 1;
        } else if (!strncmp(kind, "revoke-count", 12)) {
            uint32_t count = !strcmp(kind, "revoke-count-low") ?
                sizeof(struct jbd_revoke_header) - 1 :
                !strcmp(kind, "revoke-count-high") ? dev->lg_bsize :
                sizeof(struct jbd_revoke_header) + 1;
            ((struct jbd_revoke_header *)data)->count = to_be32(count);
            repair_checksum = true;
        } else if (!strcmp(kind, "descriptor-no-last")) {
            struct jbd_block_tag3 *tag = (void *)(hdr + 1);
            tag->flags = to_be32(to_be32(tag->flags) & ~JBD_FLAG_LAST_TAG);
            unsigned char *next = (void *)(tag + 1);
            next += UUID_SIZE;
            unsigned char *end = data + dev->lg_bsize - sizeof(struct jbd_block_tail);
            while (next + sizeof(*tag) <= end) {
                struct jbd_block_tag3 *extra = (void *)next;
                *extra = *tag;
                extra->flags = to_be32(JBD_FLAG_SAME_UUID);
                next += sizeof(*tag);
            }
            repair_checksum = true;
        } else if (!strncmp(kind, "descriptor-uuid-", 16)) {
            struct jbd_block_tag3 *tag = (void *)(hdr + 1);
            CHECK(to_be32(jfs->sb.feature_incompat) & JBD_FEATURE_INCOMPAT_CSUM_V3);
            CHECK(!(to_be32(tag->flags) & JBD_FLAG_SAME_UUID));
            memset(tag + 1, !strcmp(kind,"descriptor-uuid-zero") ? 0 : 0x5a, UUID_SIZE);
            repair_checksum = true;
        } else if (!strcmp(kind, "descriptor-target")) {
            struct jbd_block_tag3 *tag = (void *)(hdr + 1);
            tag->blocknr = to_be32(dev->lg_bcnt);
            repair_checksum = true;
        } else {
            data[dev->lg_bsize - 1] ^= 0x80;
        }
        if (repair_checksum) {
            struct jbd_block_tail *tail = (void *)(data + dev->lg_bsize - sizeof(*tail));
            tail->checksum = 0;
            uint32_t sum = ext4_crc32c(EXT4_CRC32_INIT, jfs->sb.uuid, UUID_SIZE);
            tail->checksum = to_be32(ext4_crc32c(sum, data, dev->lg_bsize));
        }
        CHECK(ext4_blocks_set_direct(dev, data, physical, 1) == EOK);
        CHECK(kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK);
        free(data);
        return;
    }
    CHECK(!"journal record to mutate was missing");
}

int main(int argc, char **argv)
{
    CHECK(argc >= 3);
    CHECK(fault_block_open(&disk, argv[1]) == 0);
    unsigned char physical[512];
    struct ext4_blockdev_iface iface = {
        .open = open_device, .close = open_device,
        .bread = read_device, .bwrite = write_device,
        .ph_bsize = 512, .ph_bcnt = disk.device.capacity_bytes / 512,
        .ph_bbuf = physical,
        .flush = flush_device,
    };
    struct ext4_blockdev dev = { .bdif = &iface, .part_size = disk.device.capacity_bytes };
    CHECK(ext4_device_register(&dev, "journal-test") == EOK);
    bool read_only = !strcmp(argv[2], "recover-readonly");
    int mounted = getenv("JOURNAL_EXT3") ?
        ext4_mount_ext3("journal-test", "/", read_only) :
        ext4_mount("journal-test", "/", read_only);
    CHECK(mounted == EOK);
    if (!strcmp(argv[2], "mapped-background")) {
        write_mapped_background(&dev);
        return 0;
    }
#ifdef TEST_WRITEBACK_REGRESSIONS
    if (!strcmp(argv[2], "deferred-prewrite")) {
        write_deferred_prewrite(&dev);
        return 0;
    }
    if (!strcmp(argv[2], "write-unwritten")) {
        write_unwritten_range(&dev);
        return 0;
    }
    if (!strcmp(argv[2], "ordered-idle")) {
        ordered_idle_test(&dev);
        return 0;
    }
    if (!strcmp(argv[2], "metadata-age")) {
        metadata_age_test(&dev);
        return 0;
    }
#endif
    struct jbd_fs jfs;
    struct jbd_journal journal;
    CHECK(jbd_get_fs(dev.fs, &jfs) == EOK);
    if (!strcmp(argv[2], "recover-readonly")) {
        CHECK(jfs.sb.start != 0);
        uint64_t writes = disk.writes;
        CHECK(jbd_recover(&jfs) == EROFS);
        CHECK(disk.writes == writes);
        CHECK(jfs.sb.start != 0);
        return 0;
    }
    if (!strcmp(argv[2], "recover-no-super")) {
        CHECK(dev.fs->super_replay_required);
        uint32_t start = jfs.sb.start;
        CHECK(jbd_recover(&jfs) == EUCLEAN);
        CHECK(jfs.sb.start == start);
        CHECK(dev.fs->super_replay_required);
        CHECK(fault_block_crash(&disk) == 0);
        return 0;
    }
    if (!strcmp(argv[2], "recover-corrupt")) {
        ext4_fsblk_t journal_sb;
        unsigned char *images=malloc((size_t)dev.lg_bsize*2);
        CHECK(images != NULL);
        struct jbd_sb *before=(void *)images;
        struct jbd_sb *after=(void *)(images+dev.lg_bsize);
        CHECK(jbd_inode_bmap(&jfs, 0, &journal_sb) == EOK);
        CHECK(ext4_blocks_get_direct(&dev, before, journal_sb, 1) == EOK);
        CHECK(jbd_recover(&jfs) == atoi(argv[3]));
        CHECK(jfs.sb.start == before->start && before->start != 0);
        CHECK(fault_block_crash(&disk) == 0);
        CHECK(ext4_blocks_get_direct(&dev, after, journal_sb, 1) == EOK);
        CHECK(memcmp(before, after, dev.lg_bsize) == 0);
        free(images);
        return 0;
    }
    if (dev.fs->super_replay_required && !strncmp(argv[2], "recover", 7)) {
        CHECK(jbd_recover(&jfs) == EOK);
        CHECK(!dev.fs->super_replay_required);
    }
    ext4_file file;
    CHECK(ext4_fopen(&file, "/probe", "r+") == EOK);
    struct ext4_inode_ref inode;
    ext4_fsblk_t home;
    CHECK(ext4_fs_get_inode_ref(dev.fs, file.inode, &inode) == EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&inode, 0, &home, false) == EOK);
    CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
    if (!strcmp(argv[2], "mutate")) {
        mutate_log(&dev, &jfs, argv[3]);
        return 0;
    }
    if (!strncmp(argv[2], "recover", 7)) {
        if (!strcmp(argv[2], "recover-write-error")) disk.fail_write = disk.writes + 1;
        if (!strcmp(argv[2], "recover-flush-error")) disk.fail_flush = disk.flushes + 1;
        if (strstr(argv[2], "error")) {
            CHECK(jbd_recover(&jfs) == EIO);
            CHECK(fault_block_crash(&disk) == 0);
            return 0;
        }
        CHECK(jbd_recover(&jfs) == EOK);
        CHECK(!dev.fs->super_replay_required);
        if (!strcmp(argv[2], "recover-crash")) CHECK(fault_block_crash(&disk) == 0);
        CHECK(jbd_put_fs(&jfs) == EOK);
        unsigned char *data = malloc(dev.lg_bsize);
        CHECK(data != NULL);
        CHECK(ext4_blocks_get_direct(&dev, data, home, 1) == EOK);
        CHECK(data[0] == (unsigned char)atoi(argv[3]));
        if (atoi(argv[3]) == 192) {
            CHECK(!memcmp(data, "\xc0\x3b\x39\x98", 4));
            for (unsigned i = 4; i < dev.lg_bsize; i++) CHECK(data[i] == 'N');
        }
        free(data);
        if (!strcmp(argv[2], "recover-metadata")) {
            CHECK(ext4_fs_get_inode_ref(dev.fs, file.inode, &inode) == EOK);
            CHECK((ext4_inode_get_mode(&dev.fs->sb, inode.inode) & 0777) == 0600);
            CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
        }
        CHECK(ext4_fclose(&file) == EOK);
        CHECK(ext4_umount("/") == EOK);
        CHECK(kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK);
        return 0;
    }
    if (!strcmp(argv[2], "no-flush")) {
        iface.flush = NULL;
        CHECK(jbd_journal_start(&jfs, &journal) == ENOTSUP);
        return 0;
    }
    if (!strncmp(argv[2], "checksum2", 9) || !strncmp(argv[2], "checksum3", 9) ||
        !strcmp(argv[2], "freeze-owner")) {
        jfs.sb.feature_incompat = to_be32(JBD_FEATURE_INCOMPAT_REVOKE |
            (!strncmp(argv[2], "checksum2", 9) ? JBD_FEATURE_INCOMPAT_CSUM_V2 : JBD_FEATURE_INCOMPAT_CSUM_V3));
        jfs.sb.checksum_type = JBD_CRC32C_CHKSUM;
        if (strstr(argv[2], "64"))
            jfs.sb.feature_incompat |= to_be32(JBD_FEATURE_INCOMPAT_64BIT);
    }
    if (!strcmp(argv[2], "unsupported")) {
        jfs.sb.feature_incompat = to_be32(JBD_FEATURE_INCOMPAT_ASYNC_COMMIT);
        CHECK(jbd_journal_start(&jfs, &journal) == ENOTSUP);
        return 0;
    }
    CHECK(jbd_journal_start(&jfs, &journal) == EOK);
    CHECK(ext4_block_cache_write_back(&dev, 1) == EOK);
    if (!strcmp(argv[2], "direct-ordered")) {
        direct_ordered_data(&dev,&journal,&file,home,argc>3?(unsigned)atoi(argv[3]):0);
        return 0;
    }
    if (!strcmp(argv[2], "ordered-range-batch")) {
        complete_ordered_range_batch(&dev, &journal, &file, home,
            argc > 3 ? (unsigned)atoi(argv[3]) : 0);
        return 0;
    }
    if (!strcmp(argv[2], "wide-ordered-range")) {
        wide_ordered_range(&dev,&journal,&file);
        return 0;
    }
    if (!strcmp(argv[2], "resource-admission") || !strcmp(argv[2], "deferred-admission")) {
        resource_admission(&dev, &journal, &file, home, argc > 3 ? (unsigned)atoi(argv[3]) : 0,
            !strcmp(argv[2], "deferred-admission"));
        return 0;
    }
    if (!strcmp(argv[2], "resource-wait")) {
        resource_reclaim(&dev, &journal, home);
        return 0;
    }
#ifdef TEST_JBD_CREDIT_PARALLEL
    if (!strcmp(argv[2], "wire-owner")) {
        unsigned fault = argc > 3 ? (unsigned)atoi(argv[3]) : 0;
        CHECK(fault <= 2);
        wire_ownership(&dev, &journal, home, fault);
        return 0;
    }
    if (!strcmp(argv[2], "record-prepare")) {
        private_record_prepare(&dev, &journal, home);
        return 0;
    }
    if (!strcmp(argv[2], "fresh-block") || !strcmp(argv[2], "fresh-block-crash")) {
        fresh_block_test(&dev, &journal, home, !strcmp(argv[2], "fresh-block-crash"));
        return 0;
    }
    if (!strcmp(argv[2], "prepare-wire")) {
        prepared_wire(&dev, &journal, home);
        return 0;
    }
    if (!strcmp(argv[2], "publication-owners")) {
        publication_owners(&dev, &journal, home);
        return 0;
    }
    if (!strcmp(argv[2], "freeze-owner")) {
        unsigned fault = argc > 3 ? (unsigned)atoi(argv[3]) : 0;
        journal_freeze_owner(&dev, &journal, home, fault);
        if (!fault) close_normal_journal(&dev, &journal);
        CHECK(fault_block_crash(&disk) == 0);
        return 0;
    }
    if (!strcmp(argv[2], "credit-parallel")) {
        journal_credit_parallel(&dev, &journal, home);
        close_normal_journal(&dev, &journal);
        CHECK(fault_block_crash(&disk) == 0);
        return 0;
    }
#endif
    if (!strcmp(argv[2], "private-views")) {
        private_metadata_views(&dev, &journal, home);
        return 0;
    }
    if (!strcmp(argv[2], "group-reserve")) {
        group_revoke_reservation(&dev, &journal, home, argc > 3 ? atoi(argv[3]) : 0);
        return 0;
    }
    if (!strcmp(argv[2], "abort-fresh")) {
        /* Aborting a first modification must restore the live cache as well
         * as leave disk unchanged. No prior committed log can supply it. */
        struct jbd_trans *tx; CHECK(jbd_journal_new_trans(&journal, &tx) == EOK);
        CHECK(tx != NULL);
        dev.fs->jbd_journal = &journal;
        test_operation_bind(dev.fs, tx);
        struct ext4_block block;
        CHECK(ext4_trans_block_get(&dev, &block, home) == EOK);
        memset(block.data, 'A', dev.lg_bsize);
        CHECK(ext4_trans_set_block_dirty(block.buf) == EOK);
        test_operation_bind(dev.fs, NULL);
        jbd_journal_free_trans(&journal, tx, true);
        /* A held inode/table reference must still point at the rolled-back
         * contents; invalidating and waiting for the next get is too late. */
        CHECK(block.data[0] == 'O');
        CHECK(ext4_block_set(&dev, &block) == EOK);
        CHECK(fault_block_crash(&disk) == 0);
        return 0;
    }
    uint64_t before = disk.flushes;
    unsigned iterations = !strcmp(argv[2], "wrap") ? 1600 :
        !strcmp(argv[2], "checksum3-two") ? 2 : 1;
    for (unsigned iteration = 0; iteration < iterations; iteration++) {
    struct jbd_trans *tx; CHECK(jbd_journal_new_trans(&journal, &tx) == EOK);
    CHECK(tx != NULL);
    if (!strcmp(argv[2], "super-record") || !strcmp(argv[2], "bad-super")) {
        struct ext4_block super;
        /* Construct the logged image directly. Filesystem acceptance merges
         * its own superblock fields and recomputes their checksum; this
         * recovery fixture deliberately includes an invalid logged image. */
        CHECK(ext4_block_get(&dev, &super,
              EXT4_SUPERBLOCK_OFFSET / dev.lg_bsize) == EOK);
        CHECK(jbd_trans_get_write_access(tx, &super) == EOK);
        struct ext4_sblock *sb = (void *)(super.data + EXT4_SUPERBLOCK_OFFSET % dev.lg_bsize);
        ext4_sb_set_csum(sb);
        if (!strcmp(argv[2], "bad-super")) sb->checksum ^= 1;
        CHECK(jbd_trans_set_block_dirty(tx, &super) == EOK);
        CHECK(ext4_block_set(&dev, &super) == EOK);
    }
    if (!strcmp(argv[2], "metadata")) {
        dev.fs->jbd_journal = &journal;
        test_operation_bind(dev.fs, tx);
        CHECK(ext4_fs_get_inode_ref(dev.fs, file.inode, &inode) == EOK);
        ext4_inode_set_mode(&dev.fs->sb, inode.inode, 0100600);
        inode.dirty = true;
        CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
        test_operation_bind(dev.fs, NULL);
    }
    struct ext4_block block;
    CHECK(ext4_block_get(&dev, &block, home) == EOK);
    CHECK(jbd_trans_get_write_access(tx, &block) == EOK);
    memset(block.data, !strcmp(argv[2], "checksum3-two") && iteration ? 'S' : 'N', dev.lg_bsize);
    bool escaped = strstr(argv[2], "escape") != NULL;
    if (escaped) memcpy(block.data, "\xc0\x3b\x39\x98", 4);
    CHECK(jbd_trans_set_block_dirty(tx, &block) == EOK);
    CHECK(ext4_block_set(&dev, &block) == EOK);
    if (strstr(argv[2], "wide")) {
        /* Force more than one descriptor, preserving all home contents.
         * Recovery must parse every descriptor boundary, including LAST. */
        for (unsigned i = 0; i < 200; i++) {
            CHECK(ext4_block_get(&dev, &block, 24000 + i) == EOK);
            CHECK(jbd_trans_get_write_access(tx, &block) == EOK);
            CHECK(jbd_trans_set_block_dirty(tx, &block) == EOK);
            CHECK(ext4_block_set(&dev, &block) == EOK);
        }
    }
    if (!strcmp(argv[2], "oversize")) {
        unsigned capacity = to_be32(jfs.sb.maxlen) - to_be32(jfs.sb.first);
        for (unsigned i = 0; i < capacity; i++) {
            CHECK(ext4_block_get_noread(&dev, &block, 24000 + i) == EOK);
            CHECK(jbd_trans_get_write_access(tx, &block) == EOK);
            memset(block.data, 'X', dev.lg_bsize);
            CHECK(jbd_trans_set_block_dirty(tx, &block) == EOK);
            CHECK(ext4_block_set(&dev, &block) == EOK);
        }
    }
    if (!strcmp(argv[2], "error") && argc > 3) disk.fail_flush = disk.flushes + (uint64_t)atoi(argv[3]);
    if (!strcmp(argv[2], "write-error")) disk.fail_write = disk.writes + (uint64_t)atoi(argv[3]);
    bool escape_error = escaped && argc > 3;
    if (escape_error) disk.fail_write = disk.writes + (uint64_t)atoi(argv[3]);
    if (!strncmp(argv[2], "checksum3-revoke", 16))
        CHECK(jbd_trans_revoke_block(tx, home + 1) == EOK);
    uint32_t allocation_start = journal.last;
    int prepared = jbd_trans_prepare_operation(tx);
    int rc;
    if (!strcmp(argv[2], "oversize")) {
        CHECK(prepared == ENOSPC);
        jbd_journal_free_trans(&journal, tx, true);
        rc = prepared;
    } else {
        CHECK(prepared == EOK);
        rc = jbd_journal_commit_trans(&journal, tx);
    }
    if (!strcmp(argv[2], "oversize")) {
        CHECK(rc == ENOSPC);
        CHECK(journal.error == EOK && journal.failed_trans == NULL);
        CHECK(journal.last == allocation_start);
        struct jbd_trans *retry; CHECK(jbd_journal_new_trans(&journal, &retry) == EOK);
        CHECK(retry != NULL);
        CHECK(jbd_trans_prepare_operation(retry) == EOK);
        CHECK(jbd_journal_commit_trans(&journal, retry) == EOK);
    } else if (!strcmp(argv[2], "error") || !strcmp(argv[2], "write-error") || escape_error) {
        CHECK(rc == EIO);
    } else {
        CHECK(rc == EOK);
        CHECK(disk.flushes >= before + 2);
        CHECK(jbd_journal_sync(&journal, journal.committed_id) == EOK);
        CHECK(jbd_journal_sync(&journal, journal.committed_id + 1) == EAGAIN);
        if (!strcmp(argv[2], "abort-read-error")) {
            struct jbd_trans *aborted; CHECK(jbd_journal_new_trans(&journal, &aborted) == EOK);
            CHECK(aborted != NULL);
            CHECK(ext4_block_get(&dev, &block, home) == EOK);
            CHECK(jbd_trans_get_write_access(aborted, &block) == EOK);
            memset(block.data, 'A', dev.lg_bsize);
            CHECK(jbd_trans_set_block_dirty(aborted, &block) == EOK);
            CHECK(ext4_block_set(&dev, &block) == EOK);
            fail_read = reads + 1;
            uint64_t before_abort = reads;
            jbd_journal_free_trans(&journal, aborted, true);
            CHECK(reads == before_abort);
            CHECK(jbd_journal_sync(&journal, journal.committed_id) == EOK);
        }
        if (!strcmp(argv[2], "checkpoint-error") || !strcmp(argv[2], "checkpoint-flush-error")) {
            if (!strcmp(argv[2], "checkpoint-error")) disk.fail_write = disk.writes + (uint64_t)atoi(argv[3]);
            else disk.fail_flush = disk.flushes + (uint64_t)atoi(argv[3]);
            CHECK(jbd_journal_purge_cp_trans(&journal, true, false) == EIO);
            CHECK(jbd_journal_sync(&journal, journal.committed_id) == EIO);
            CHECK(jbd_journal_stop(&journal) == EIO);
        } else if (!strcmp(argv[2], "checkpoint")) {
            CHECK(jbd_journal_purge_cp_trans(&journal, true, false) == EOK);
        }
    }
    }
    /* Terminating without stop/unmount deliberately models a power cut. */
    CHECK(fault_block_crash(&disk) == 0);
    return 0;
}
