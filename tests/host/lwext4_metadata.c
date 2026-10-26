/* Live-inode explicit timestamps and actual ext4 allocation statistics. */
#include "block_fault.h"
#include "lwext4_operation.h"
#include <ext4.h>
#include <ext4_fs.h>
#include <ext4_extent.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <ext4_block_group.h>
#include <ext4_bitmap.h>
#include <ext4_balloc.h>
#include <ext4_journal.h>
#include <ext4_trans.h>
#include <ext4_crc32.h>
#include <ext4_blockdev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef TEST_RANGE_WORK_COUNTERS
static uint32_t watched_bitmap_bytes, bitmap_crc_calls;
static uint32_t watched_extent_bytes, extent_crc_calls;
static unsigned range_queries, watched_indirect_gets;
static ext4_fsblk_t watched_indirect_leaf;
int __real_ext4_trans_block_get(struct ext4_blockdev *, struct ext4_block *, uint64_t);
int __wrap_ext4_trans_block_get(struct ext4_blockdev *dev, struct ext4_block *block, uint64_t address)
{
    if (watched_indirect_leaf && address == watched_indirect_leaf) watched_indirect_gets++;
    return __real_ext4_trans_block_get(dev, block, address);
}
int __real_ext4_fs_get_inode_dblk_range(struct ext4_inode_ref *,ext4_lblk_t,uint32_t,bool,struct ext4_inode_block_range *);
int __wrap_ext4_fs_get_inode_dblk_range(struct ext4_inode_ref *ref,ext4_lblk_t logical,uint32_t count,bool create,struct ext4_inode_block_range *range)
{
    range_queries++;
    return __real_ext4_fs_get_inode_dblk_range(ref,logical,count,create,range);
}
uint32_t __real_ext4_crc32c(uint32_t crc, const void *data, uint32_t bytes);
uint32_t __wrap_ext4_crc32c(uint32_t crc, const void *data, uint32_t bytes)
{
    if (bytes == watched_bitmap_bytes) bitmap_crc_calls++;
    if (watched_extent_bytes && bytes == watched_extent_bytes) extent_crc_calls++;
    return __real_ext4_crc32c(crc,data,bytes);
}
#endif
static struct fault_block disk;
static unsigned allocations, fail_allocation, reads, fail_read, live_allocations;
static size_t denied_block_buffer_size;
static unsigned denied_block_buffers;
static size_t payload_threshold;
static unsigned payload_allocations;
static uint64_t forbidden_lba = UINT64_MAX, forbidden_writes;
static void (*write_interleave)(void);
static const unsigned char *borrow_watch;
static uint64_t borrow_watch_lba;
static unsigned borrow_watch_size, borrow_watch_calls;
static bool borrow_watch_escape;
static ext4_fsblk_t checkpoint_homes[3];
static unsigned checkpoint_home_writes[3], checkpoint_watch_block;
static unsigned independent_cut, independent_event;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%d: %s (alloc=%u fail=%u)\n",__LINE__,#x,allocations,fail_allocation); exit(1); } } while (0)
static void independent_boundary(void)
{
    if (!independent_cut || ++independent_event != independent_cut) return;
    CHECK(fault_block_crash(&disk)==0);
    exit(0);
}
void *ext4_user_malloc(size_t n)
{
    if(payload_threshold && n>=payload_threshold)payload_allocations++;
    if(++allocations==fail_allocation)return NULL;
    if(denied_block_buffer_size && n==denied_block_buffer_size) {denied_block_buffers++;return NULL;}
    void *p=malloc(n);if(p)live_allocations++;return p;
}
void *ext4_user_calloc(size_t n,size_t s) { if(payload_threshold && n*s>=payload_threshold)payload_allocations++; if(++allocations==fail_allocation)return NULL;void *p=calloc(n,s);if(p)live_allocations++;return p; }
void *ext4_user_realloc(void *p,size_t n) { if(++allocations==fail_allocation)return NULL;void *next=realloc(p,n);if(next&&!p)live_allocations++;return next; }
void ext4_user_free(void *p) { if(p){CHECK(live_allocations);live_allocations--;}free(p); }
static int dev_open(struct ext4_blockdev *b) { (void)b; return EOK; }
static int dev_read(struct ext4_blockdev *b,void *p,uint64_t n,uint32_t c)
{ (void)b; if (++reads == fail_read) return EIO; return kernel_block_read_at(&disk.device,n*512,p,(size_t)c*512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
static int dev_write(struct ext4_blockdev *b,const void *p,uint64_t n,uint32_t c)
{
    (void)b;
    if (checkpoint_watch_block) for (unsigned i=0;i<3;i++) {
        uint64_t sector=checkpoint_homes[i]*checkpoint_watch_block/512;
        if (n<=sector && sector<n+c) checkpoint_home_writes[i]++;
    }
    if (borrow_watch && borrow_watch_lba == UINT64_MAX) {
        /* The commit owner assigns per-record wire addresses while encoding.
         * Observe the actual borrowed bytes at submission, then verify that
         * address against the completed record rather than a freeze layout. */
        uint32_t first;
        memcpy(&first, p, sizeof(first));
        if (p == borrow_watch || (borrow_watch_escape && !first && c == 1 &&
            !memcmp((const unsigned char *)p + 4, borrow_watch + 4, 508)))
            borrow_watch_lba = n;
    }
    if (borrow_watch && n >= borrow_watch_lba && n < borrow_watch_lba + borrow_watch_size / 512) {
        unsigned offset = (unsigned)(n - borrow_watch_lba) * 512;
        if (borrow_watch_escape && !offset) {
            uint32_t first; memcpy(&first, p, sizeof(first));
            CHECK(!first && c == 1 && !memcmp((const unsigned char *)p + 4, borrow_watch + 4, 508));
            CHECK(!memcmp(borrow_watch, "\xc0\x3b\x39\x98", 4));
        } else CHECK(p == borrow_watch + offset);
        borrow_watch_calls++;
    }
    if(write_interleave){void (*hook)(void)=write_interleave;write_interleave=NULL;hook();}
    if (n <= forbidden_lba && forbidden_lba < n+c) forbidden_writes++;
    int result=kernel_block_write_at(&disk.device,n*512,p,(size_t)c*512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO;
    independent_boundary();
    return result;
}
static int dev_flush(struct ext4_blockdev *b)
{
    (void)b;
    int result=kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO;
    independent_boundary();
    return result;
}
static uint32_t cost_tick;
extern int ext4_journal_group_enable(const char *, const struct ext4_journal_runtime *, size_t) __attribute__((weak));
extern int ext4_journal_group_service(const char *, bool) __attribute__((weak));
extern int ext4_journal_group_drain(const char *) __attribute__((weak));
extern int ext4_file_sync_metadata_mode(ext4_file *, bool) __attribute__((weak));
static uint64_t group_now;
static uint64_t group_clock(void *context) { (void)context; return group_now; }
static unsigned group_requests,group_work[2];
static void group_request(void *context,unsigned work)
{
    (void)context;CHECK(work && !(work & ~(EXT4_JOURNAL_WORK_COMMIT|EXT4_JOURNAL_WORK_CHECKPOINT)));
    group_requests++;
    if(work & EXT4_JOURNAL_WORK_COMMIT)group_work[0]++;
    if(work & EXT4_JOURNAL_WORK_CHECKPOINT)group_work[1]++;
}
static unsigned pipeline_waits;
static enum ext4_journal_wait pipeline_kind;
static int pipeline_wait(void *context, uint64_t sequence, enum ext4_journal_wait kind)
{
    (void)context; (void)sequence;
    pipeline_kind=kind;
    pipeline_waits++;
    return EAGAIN;
}
static ext4_file *pipeline_file;
static void pipeline_while_submitting(void)
{
    struct ext4_timestamp times[3]={{1111,1},{2222,2},{3333,3}};
    /* Device I/O of group one is held on this stack. New operations can
     * seal two later groups, then wait for sealing capacity, never home I/O. */
    for(unsigned group=0;group<3;group++) {
        times[0].nanoseconds=100+group;
        CHECK(ext4_file_set_times(pipeline_file,7,times)==EOK);
        /* 真实年龄条件封口后续组，直到sealed FIFO确实没有空间。 */
        group_now+=100000000;
    }
    CHECK(ext4_file_set_times(pipeline_file,7,times)==EAGAIN);
    CHECK(pipeline_waits==1 && pipeline_kind==EXT4_JOURNAL_WAIT_SEALED);
}
static void group_pipeline(struct ext4_fs *fs, bool crash)
{
    CHECK(fs->jbd_journal);
    struct ext4_journal_runtime runtime={.now_ns=group_clock,.wait=pipeline_wait};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file f; CHECK(ext4_fopen(&f,"/file","r+")==EOK);
    struct ext4_timestamp times[3]={{1111,1},{2222,2},{3333,3}};
    CHECK(ext4_file_set_times(&f,7,times)==EOK);
    CHECK(pipeline_waits==0);
    pipeline_file=&f;write_interleave=pipeline_while_submitting;
    CHECK(ext4_journal_group_service("/",true)==EOK);
    struct ext4_journal_progress progress;
    CHECK(ext4_journal_group_progress("/",&progress)==EOK);
    CHECK(progress.durable>progress.checkpoint);
    uint64_t writes=disk.writes,flushes=disk.flushes;
    /* A committed target completes full sync even while home I/O is withheld. */
    while(progress.durable<progress.accepted) {
        CHECK(ext4_journal_group_service("/",true)==EOK);
        CHECK(ext4_journal_group_progress("/",&progress)==EOK);
    }
    CHECK(progress.durable>progress.checkpoint);
    writes=disk.writes;flushes=disk.flushes;
    CHECK(ext4_file_sync_metadata(&f)==EOK);
    CHECK(pipeline_waits==1 && disk.writes==writes && disk.flushes==flushes);
    if(crash) {
        CHECK(fault_block_crash(&disk)==0);
        return;
    }
    CHECK(disk.writes>=writes && disk.flushes>=flushes);
    CHECK(ext4_journal_group_service("/",true)==EOK);
    CHECK(ext4_journal_group_drain("/")==EOK);
    CHECK(ext4_fclose(&f)==EOK);
    printf("group pipeline: age seals fill FIFO; commit and checkpoint independent\n");
}
static void group_bounds(struct ext4_fs *fs)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file f; CHECK(ext4_fopen(&f,"/file","r+")==EOK);
    CHECK(ext4_ftruncate(&f,0, UINT32_MAX)==EOK && ext4_journal_group_drain("/")==EOK);
    struct ext4_journal_progress before, small, large;
    CHECK(ext4_journal_group_progress("/",&before)==EOK);
    unsigned char bytes[4096]; size_t count;
    for(unsigned i=0;i<96;i++) {
        memset(bytes,(int)i,sizeof(bytes));
        CHECK(ext4_fwrite(&f,bytes,sizeof(bytes),&count)==EOK && count==sizeof(bytes));
        if(i==47) {
            CHECK(ext4_journal_group_progress("/",&small)==EOK && small.sealed==before.sealed);
        }
    }
    CHECK(ext4_journal_group_progress("/",&large)==EOK && large.sealed==small.sealed);
    CHECK(group_now==0 && jbd_memory_peak(fs->jbd_journal)<=jbd_memory_limit(fs->jbd_journal));
    /* Data remains governed by actual retained memory. Exceed that budget
     * at the same clock and force reclaim without a per-256-KiB commit. */
    for (unsigned i=96;i<1536;i++) {
        memset(bytes,(int)i,sizeof(bytes));
        CHECK(ext4_fwrite(&f,bytes,sizeof(bytes),&count)==EOK && count==sizeof(bytes));
    }
    CHECK(ext4_journal_group_progress("/",&large)==EOK && large.sealed>small.sealed);
    CHECK(group_now==0 && jbd_memory_peak(fs->jbd_journal)<=jbd_memory_limit(fs->jbd_journal));
    CHECK(ext4_journal_group_drain("/")==EOK && ext4_fseek(&f,0,SEEK_SET)==EOK);
    for(unsigned i=0;i<1536;i++) {
        CHECK(ext4_fread(&f,bytes,sizeof(bytes),&count)==EOK && count==sizeof(bytes));
        for(unsigned j=0;j<sizeof(bytes);j++) CHECK(bytes[j]==(unsigned char)i);
    }
    CHECK(ext4_fclose(&f)==EOK);
    puts("group bounds: file data does not cut metadata groups at 256 KiB; content and budget preserved");
}
static void group_barrier(struct ext4_fs *fs, unsigned fault, unsigned persist)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file f; CHECK(ext4_fopen(&f,"/file","r+")==EOK);
    struct ext4_timestamp times[3]={{1111,1},{2222,2},{3333,3}};
    unsigned char bytes[4096]; memset(bytes,0xa7,sizeof(bytes)); size_t count;
    CHECK(ext4_transaction_begin("/")==EOK);
    CHECK(ext4_file_set_times(&f,7,times)==EOK);
    CHECK(ext4_fpwrite(&f,4096,bytes,sizeof(bytes),&count)==EOK && count==sizeof(bytes));
    CHECK(ext4_transaction_end("/")==EOK);
    struct jbd_journal *journal=fs->jbd_journal;
    CHECK(jbd_journal_freeze(journal)==EOK && jbd_journal_select(journal));
    uint64_t flushes=disk.flushes;
    if (fault) disk.fail_flush=flushes+fault;
    int result=jbd_journal_submit(journal);
    CHECK(result==(fault?EIO:EOK) && jbd_journal_retire(journal,result)==result);
    CHECK(disk.flushes-flushes==(fault?fault:2));
    if (fault) {
        CHECK(journal->error==EIO && journal->failed_trans==journal->committing);
        CHECK(ext4_file_sync_metadata(&f)==EIO);
        /* The failed post-commit flush may have persisted none, one torn
         * sector, or the complete commit out of order. Its prerequisites
         * must already be durable in all three cases. */
        if (persist==1 && disk.count) CHECK(fault_block_persist(&disk,disk.count-1)==0);
        if (persist==2) for (size_t i=disk.count;i;i--) CHECK(fault_block_persist(&disk,i-1)==0);
    }
    CHECK(fault_block_crash(&disk)==0);
    printf("group barrier: data and log body share precommit flush; fault=%u persist=%u\n",fault,persist);
}
static void group_barrier_recovery(struct ext4_fs *fs)
{
    CHECK(!fs->jbd_journal->error);
    ext4_file f; struct ext4_inode inode; size_t count;
    CHECK(ext4_fopen(&f,"/file","r")==EOK && ext4_fraw_inode_fill(&f,&inode)==EOK);
    bool committed=to_le32(inode.access_time)==1111;
    CHECK(to_le32(inode.access_time)==(committed?1111:0));
    CHECK(to_le32(inode.modification_time)==(committed?2222:0));
    CHECK(to_le32(inode.change_inode_time)==(committed?3333:0));
    CHECK(f.fsize==(committed?8192:4096));
    unsigned char bytes[4096];
    CHECK(ext4_fpread(&f,0,bytes,sizeof(bytes),&count)==EOK && count==sizeof(bytes));
    for (unsigned i=0;i<sizeof(bytes);i++) CHECK(bytes[i]==0x5a);
    if (committed) {
        CHECK(ext4_fpread(&f,4096,bytes,sizeof(bytes),&count)==EOK && count==sizeof(bytes));
        for (unsigned i=0;i<sizeof(bytes);i++) CHECK(bytes[i]==0xa7);
    }
    CHECK(ext4_fclose(&f)==EOK);
}
static void group_ring(struct ext4_fs *fs)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,512*1024)==EOK);
    ext4_file f;CHECK(ext4_fopen(&f,"/file","r+")==EOK);
    uint32_t previous=fs->jbd_journal->last;
    bool wrapped=false;
    unsigned iterations=to_be32(fs->jbd_journal->jbd_fs->sb.maxlen)+4;
    for(unsigned i=0;i<iterations;i++) {
        struct ext4_timestamp times[3]={{1111,i},{2222,2},{3333,3}};
        CHECK(ext4_file_set_times(&f,7,times)==EOK && ext4_file_sync_metadata(&f)==EOK);
        if(fs->jbd_journal->last<previous)wrapped=true;
        previous=fs->jbd_journal->last;
    }
    CHECK(wrapped && ext4_journal_group_drain("/")==EOK);
    CHECK(jbd_memory_peak(fs->jbd_journal)<=jbd_memory_limit(fs->jbd_journal));
    CHECK(ext4_fclose(&f)==EOK);
    printf("group ring: same-block versions and checkpoint batches survive wrap within budget\n");
}
static ext4_file *interleave_file;
static void group_modify_while_submitting(void)
{
    struct ext4_timestamp times[3]={{7777,7},{8888,8},{9999,9}};
    unsigned char bytes[4096]; size_t count;
    memset(bytes,0xa6,sizeof(bytes));
    CHECK(ext4_file_set_times(interleave_file,7,times)==EOK);
    CHECK(ext4_fseek(interleave_file,0,SEEK_SET)==EOK);
    CHECK(ext4_fwrite(interleave_file,bytes,sizeof(bytes),&count)==EOK && count==sizeof(bytes));
}
static void group_test(struct ext4_fs *fs)
{
    CHECK(ext4_journal_group_enable && ext4_journal_group_service && ext4_journal_group_drain);
    struct ext4_journal_runtime runtime={.now_ns=group_clock,.request=group_request};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file f; CHECK(ext4_fopen(&f,"/file","r+")==EOK);
    uint64_t writes=disk.writes, flushes=disk.flushes;
    uint32_t tid=fs->jbd_journal->committed_id;
    struct ext4_timestamp times[3]={{1111,1},{2222,2},{3333,3}};
    payload_threshold=fs->bdev->lg_bsize;
    /* Warm actual edits: repeating equal timestamps prunes to a no-op and
     * does not borrow the log/control/checkpoint images used below. */
    for(unsigned i=0;i<4;i++) {
        times[0].nanoseconds=i;
        CHECK(ext4_file_set_times(&f,7,times)==EOK);
    }
    CHECK(group_requests==1);
    unsigned warmed_payloads=payload_allocations;
    struct ext4_journal_progress before_repeat, after_repeat;
    CHECK(ext4_journal_group_progress("/",&before_repeat)==EOK);
    for(unsigned i=4;i<128;i++) {
        times[0].nanoseconds=i;
        CHECK(ext4_file_set_times(&f,7,times)==EOK);
    }
    /* Repeated edits of one running block must reuse reserved payloads.
     * A fresh block/undo allocation per warm edit fails this cost contract. */
    CHECK(ext4_journal_group_progress("/",&after_repeat)==EOK && after_repeat.sealed==before_repeat.sealed);
    CHECK(group_requests==1 && group_work[0]==1 && !group_work[1] && after_repeat.deadline_ns==100000000);
    CHECK(payload_allocations==warmed_payloads);
    CHECK(disk.writes==writes && disk.flushes==flushes && fs->jbd_journal->committed_id==tid);
    struct ext4_inode committed, actual;
    CHECK(ext4_fraw_inode_fill(&f,&committed)==EOK);
    CHECK(ext4_transaction_begin("/")==EOK);
    times[0].seconds=9999; CHECK(ext4_file_set_times(&f,1,times)==EOK);
    CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED);
    CHECK(ext4_fraw_inode_fill(&f,&actual)==EOK && !memcmp(&actual,&committed,sizeof(actual)));
    CHECK(ext4_journal_group_service("/",false)==EOK && disk.writes==writes);
    group_now=99999999;
    CHECK(ext4_journal_group_service("/",false)==EOK && disk.writes==writes);
    group_now=100000000;
    CHECK(ext4_journal_group_service("/",false)==EOK);
    CHECK(fs->jbd_journal->committed_id==tid+1);
    CHECK(disk.flushes-flushes < 32*2);
    CHECK(ext4_fraw_inode_fill(&f,&actual)==EOK && !memcmp(&actual,&committed,sizeof(actual)));
    CHECK(ext4_file_sync_metadata(&f)==EOK);
    CHECK(ext4_file_sync_metadata_mode);
    CHECK(ext4_file_set_times(&f,7,times)==EOK);
    uint64_t before_data_sync=disk.flushes;
    CHECK(ext4_file_sync_metadata_mode(&f,true)==EOK && disk.flushes==before_data_sync);
    CHECK(ext4_file_sync_metadata_mode(&f,false)==EOK && disk.flushes>before_data_sync);
    /* Every private reservation failure preserves an earlier accepted change. */
    CHECK(ext4_file_set_times(&f,7,times)==EOK);
    CHECK(ext4_fraw_inode_fill(&f,&committed)==EOK);
    for(unsigned point=1;point<=20;point++) {
        fail_allocation=allocations+point;
        int begin=ext4_transaction_begin("/");
        int result=begin;
        if(begin==EOK) {
            struct ext4_timestamp changed[3]={{1234,4},{5678,5},{9012,6}};
            result=ext4_file_set_times(&f,7,changed);
            int abort=ext4_transaction_abort("/",ECANCELED);
            CHECK(abort==(result==EOK?ECANCELED:result));
        }
        fail_allocation=0;
        CHECK(result==EOK || result==ENOMEM);
        CHECK(ext4_fraw_inode_fill(&f,&actual)==EOK && !memcmp(&actual,&committed,sizeof(actual)));
        CHECK(jbd_memory_used(fs->jbd_journal)<=jbd_memory_limit(fs->jbd_journal));
    }
    unsigned char bytes[4096], readback[4096]; size_t count;
    memset(bytes,0x55,sizeof(bytes));
    CHECK(ext4_fseek(&f,0,SEEK_SET)==EOK);
    CHECK(ext4_fwrite(&f,bytes,sizeof(bytes),&count)==EOK && count==sizeof(bytes));
    struct ext4_inode_ref ref;
    CHECK(ext4_fs_get_inode_ref(fs,f.inode,&ref)==EOK);
    uint64_t inode_position=ref.block.lb_id*fs->bdev->lg_bsize+(unsigned char*)ref.inode-ref.block.data;
    ext4_fsblk_t data_block;
    CHECK(ext4_fs_get_inode_dblk_idx(&ref,0,&data_block,true)==EOK);
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    interleave_file=&f; write_interleave=group_modify_while_submitting;
    CHECK(ext4_journal_group_service("/",true)==EOK);
    CHECK(!write_interleave && fs->jbd_journal->running);
    /* Explicitly advance home I/O; service now completes one worker phase. */
    CHECK(ext4_journal_group_checkpoint("/")==EOK);
    CHECK(!memcmp(disk.visible+data_block*fs->bdev->lg_bsize,bytes,sizeof(bytes)));
    struct ext4_inode *stable=(void*)(disk.visible+inode_position);
    CHECK(to_le32(stable->access_time)==9999);
    CHECK(ext4_fseek(&f,0,SEEK_SET)==EOK);
    CHECK(ext4_fread(&f,readback,sizeof(readback),&count)==EOK && count==sizeof(readback));
    memset(bytes,0xa6,sizeof(bytes)); CHECK(!memcmp(bytes,readback,sizeof(bytes)));
    /* Sealing needs no new large allocation after successful acceptance. */
    unsigned before_allocations=allocations;
    fail_allocation=allocations+1;
    CHECK(ext4_journal_group_service("/",true)==EOK);
    CHECK(ext4_journal_group_drain("/")==EOK);
    fail_allocation=0;
    CHECK(allocations==before_allocations);
    CHECK(!memcmp(disk.visible+data_block*fs->bdev->lg_bsize,bytes,sizeof(bytes)));
    CHECK(to_le32(stable->access_time)==7777);
    CHECK(ext4_fclose(&f)==EOK); CHECK(ext4_journal_group_drain("/")==EOK);
    printf("group operations=128 commits=%u writes=%llu flushes=%llu\n",fs->jbd_journal->committed_id-tid,(unsigned long long)(disk.writes-writes),(unsigned long long)(disk.flushes-flushes));
}
static void group_log_payload(struct ext4_fs *fs)
{
    struct ext4_journal_runtime runtime = {.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/", &runtime, 4*1024*1024) == EOK);
    ext4_file file; CHECK(ext4_fopen(&file, "/file", "r+") == EOK);
    struct ext4_inode_ref inode; ext4_fsblk_t home;
    CHECK(ext4_fs_get_inode_ref(fs, file.inode, &inode) == EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&inode, 0, &home, false) == EOK && home);
    CHECK(ext4_fs_put_inode_ref(&inode) == EOK && ext4_journal_group_drain("/") == EOK);
    struct jbd_journal *journal = fs->jbd_journal;
    for (unsigned escaped = 0; escaped < 2; escaped++) {
        struct jbd_trans *operation; CHECK(jbd_journal_new_trans(journal, &operation) == EOK);
        struct ext4_block block = EXT4_BLOCK_ZERO();
        CHECK(operation && ext4_block_get(fs->bdev, &block, home) == EOK);
        CHECK(jbd_trans_get_write_access(operation, &block) == EOK);
        for (unsigned i = 0; i < fs->bdev->lg_bsize; i++) block.data[i] = (unsigned char)(i * 13 + escaped);
        uint32_t first = to_be32(escaped ? JBD_MAGIC_NUMBER : 0x12345678);
        memcpy(block.data, &first, sizeof(first));
        CHECK(jbd_trans_set_block_dirty(operation, &block) == EOK && ext4_block_set(fs->bdev, &block) == EOK);
        CHECK(jbd_trans_prepare_operation(operation) == EOK);
        CHECK(jbd_journal_accept(journal, operation, 0) == EOK);
        unsigned before_allocations = allocations;
        fail_allocation = allocations + 1;
        CHECK(jbd_journal_freeze(journal) == EOK && allocations == before_allocations);
        struct jbd_buf *version = TAILQ_FIRST(&operation->buf_queue);
        CHECK(version && version->after && !TAILQ_NEXT(version, buf_node));
        borrow_watch = version->after->data; borrow_watch_size = fs->bdev->lg_bsize;
        borrow_watch_lba = UINT64_MAX;
        borrow_watch_calls = 0; borrow_watch_escape = escaped;
        CHECK(jbd_journal_select(journal) && journal->committing == operation);
        CHECK(jbd_journal_submit(journal) == EOK && borrow_watch_calls == (escaped ? 2U : 1U));
        CHECK(borrow_watch_lba == journal->log_map[version->jbd_lba] * borrow_watch_size / 512);
        borrow_watch = NULL;
        CHECK(jbd_journal_retire(journal, EOK) == EOK);
        borrow_watch = version->after->data; borrow_watch_lba = home * borrow_watch_size / 512;
        borrow_watch_calls = 0; borrow_watch_escape = false;
        CHECK(jbd_journal_select(journal) && journal->checkpointing == operation);
        CHECK(jbd_journal_checkpoint(journal) == EOK && borrow_watch_calls == 1);
        CHECK(!memcmp(disk.visible + home * borrow_watch_size, borrow_watch, borrow_watch_size));
        borrow_watch = NULL;
        CHECK(jbd_journal_retire(journal, EOK) == EOK && allocations == before_allocations);
        fail_allocation = 0;
    }
    /* Sweep private-view and final prefix/log/control/checkpoint allocation,
     * including failure after the private owner is installed. The unchanged
     * immutable home payload is also the operation's rollback baseline. */
    struct ext4_block escaped_home=EXT4_BLOCK_ZERO();
    CHECK(ext4_block_get(fs->bdev,&escaped_home,home)==EOK);
    struct ext4_block_payload *escaped_baseline=escaped_home.payload;
    bool installed_failure=false;
    jbd_memory_set_cache_disabled(journal, true);
    /* Discard warmed controls too: disabling returns alone can let the first
     * operation use only cached objects and end this sweep before injection. */
    jbd_trim_cache(journal, 0);
    for (unsigned point=1;;point++) {
        CHECK(point<32);
        struct jbd_trans *operation; CHECK(jbd_journal_new_trans(journal, &operation) == EOK); CHECK(operation);
        unsigned begin=allocations;
        fail_allocation=begin+point;
        int result=jbd_trans_get_write_access(operation,&escaped_home);
        if (result==EOK) result=jbd_trans_set_block_dirty(operation,&escaped_home);
        if (result==EOK) result=jbd_trans_prepare_operation(operation);
        unsigned attempted=allocations-begin;
        fail_allocation=0;
        CHECK(result==EOK || result==ENOMEM);
        struct jbd_buf *access=TAILQ_FIRST(&operation->buf_queue);
        if (result==ENOMEM && access) {
            CHECK(access->private_access && !access->before && access->after &&
                access->block.payload==escaped_baseline &&
                escaped_home.payload==&access->after->payload &&
                escaped_home.data==access->after->data &&
                !memcmp(escaped_home.data,escaped_baseline->data,fs->bdev->lg_bsize));
            installed_failure=true;
        }
        jbd_journal_free_trans(journal,operation,true);
        CHECK(!journal->error && !journal->log_reserved &&
            !memcmp(escaped_home.data,"\xc0\x3b\x39\x98",4));
        if (result==EOK && point>attempted) break;
    }
    jbd_memory_set_cache_disabled(journal, false);
    CHECK(installed_failure && ext4_block_set(fs->bdev,&escaped_home)==EOK);
    /* Cross descriptor boundaries without reserving a whole control image
     * per payload. The private operation still owns every undo and version. */
    unsigned char bytes[4096]; size_t written;
    memset(bytes, 0x5a, sizeof(bytes));
    CHECK(ext4_fseek(&file, 0, SEEK_SET) == EOK);
    for (unsigned i = 0; i < 128; i++) {
        CHECK(ext4_fwrite(&file, bytes, fs->bdev->lg_bsize, &written) == EOK && written == fs->bdev->lg_bsize);
        if (i % 8 == 7) CHECK(ext4_journal_group_drain("/") == EOK);
    }
    ext4_fsblk_t homes[128];
    CHECK(ext4_fs_get_inode_ref(fs, file.inode, &inode) == EOK);
    for (unsigned i = 0; i < 128; i++) CHECK(ext4_fs_get_inode_dblk_idx(&inode, i, &homes[i], false) == EOK && homes[i]);
    CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
    payload_threshold=fs->bdev->lg_bsize;
    unsigned image_allocations=payload_allocations, cached_images=0;
    cached_images=jbd_memory_cached_images(journal);
    struct jbd_trans *operation; CHECK(jbd_journal_new_trans(journal, &operation) == EOK);
    CHECK(operation);
    for (unsigned i = 0; i < 128; i++) {
        struct ext4_block block = EXT4_BLOCK_ZERO();
        CHECK(ext4_block_get(fs->bdev, &block, homes[i]) == EOK);
        CHECK(jbd_trans_get_write_access(operation, &block) == EOK);
        block.data[9] = (unsigned char)i;
        CHECK(jbd_trans_set_block_dirty(operation, &block) == EOK && ext4_block_set(fs->bdev, &block) == EOK);
    }
    unsigned remaining_images=0;
    remaining_images=jbd_memory_cached_images(journal);
    /* Count actual block images, including reused cache owners: cached home,
     * undo and frozen version per metadata block, plus a few log controls.
     * A separate log payload image for every version violates this bound. */
    CHECK(payload_allocations-image_allocations+cached_images-remaining_images<=3*128+8);
    CHECK(jbd_memory_peak(journal)<=jbd_memory_limit(journal));
    payload_threshold=0;
    CHECK(jbd_trans_prepare_operation(operation) == EOK);
    CHECK(jbd_journal_accept(journal, operation, 0) == EOK);
    unsigned before_allocations = allocations;
    fail_allocation = allocations + 1;
    CHECK(jbd_journal_freeze(journal) == EOK && jbd_journal_select(journal));
    CHECK(jbd_journal_submit(journal) == EOK && jbd_journal_retire(journal, EOK) == EOK);
    CHECK(jbd_journal_select(journal) && jbd_journal_checkpoint(journal) == EOK);
    CHECK(jbd_journal_retire(journal, EOK) == EOK && allocations == before_allocations);
    fail_allocation = 0;
    for (unsigned i = 0; i < 128; i++) {
        CHECK(ext4_blocks_get_direct(fs->bdev, bytes, homes[i], 1) == EOK);
        CHECK(bytes[9] == i);
    }
    CHECK(ext4_fclose(&file) == EOK && ext4_journal_group_drain("/") == EOK);
    puts("group log payload: shared log/home version, isolated escape, packed control RAM and no freeze allocation");
}
static void group_checkpoint_collapse(struct ext4_fs *fs, unsigned fault)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file file; size_t count;
    CHECK(ext4_fopen(&file,"/file","r+")==EOK);
    unsigned block_size=fs->bdev->lg_bsize;
    unsigned char *bytes=malloc(3*block_size); CHECK(bytes);
    memset(bytes,0x5a,3*block_size);
    CHECK(ext4_fpwrite(&file,0,bytes,3*block_size,&count)==EOK && count==3*block_size);
    CHECK(ext4_journal_group_drain("/")==EOK);
    struct ext4_inode_ref inode;
    CHECK(ext4_fs_get_inode_ref(fs,file.inode,&inode)==EOK);
    for (unsigned i=0;i<3;i++)
        CHECK(ext4_fs_get_inode_dblk_idx(&inode,i,&checkpoint_homes[i],false)==EOK && checkpoint_homes[i]);
    CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
    struct jbd_journal *journal=fs->jbd_journal;
    /* Three durable groups: old A/B, revoke A/B, then a new A incarnation
     * and C. The selected prefix must write exactly new A and C. */
    for (unsigned group=0;group<3;group++) {
        struct jbd_trans *operation; CHECK(jbd_journal_new_trans(journal, &operation) == EOK); CHECK(operation);
        if (group==1) {
            CHECK(jbd_trans_revoke_block(operation,checkpoint_homes[0])==EOK);
            CHECK(jbd_trans_revoke_block(operation,checkpoint_homes[1])==EOK);
        } else for (unsigned i=0;i<2;i++) {
            unsigned home=group==0?i:2*i;
            struct ext4_block block=EXT4_BLOCK_ZERO();
            CHECK(ext4_block_get(fs->bdev,&block,checkpoint_homes[home])==EOK);
            CHECK(jbd_trans_get_write_access(operation,&block)==EOK);
            memset(block.data,group==0?0x61:0x73,block_size);
            CHECK(jbd_trans_set_block_dirty(operation,&block)==EOK && ext4_block_set(fs->bdev,&block)==EOK);
        }
        CHECK(jbd_trans_prepare_operation(operation) == EOK);
        CHECK(jbd_journal_accept(journal,operation,0)==EOK && jbd_journal_freeze(journal)==EOK);
        CHECK(jbd_journal_select(journal) && journal->committing==operation);
        CHECK(jbd_journal_submit(journal)==EOK && jbd_journal_retire(journal,EOK)==EOK);
    }
    CHECK(journal->checkpoint_count==3 && jbd_journal_select(journal) && journal->checkpointing);
    /* An accepted later version is visible to live readers while the older
     * durable prefix owns home DMA. It must not enter this checkpoint. */
    struct jbd_trans *future; CHECK(jbd_journal_new_trans(journal, &future) == EOK); CHECK(future);
    struct ext4_block future_block=EXT4_BLOCK_ZERO();
    CHECK(ext4_block_get(fs->bdev,&future_block,checkpoint_homes[0])==EOK);
    CHECK(jbd_trans_get_write_access(future,&future_block)==EOK);
    memset(future_block.data,0x88,block_size);
    CHECK(jbd_trans_set_block_dirty(future,&future_block)==EOK && ext4_block_set(fs->bdev,&future_block)==EOK);
    CHECK(jbd_trans_prepare_operation(future) == EOK);
    CHECK(jbd_journal_accept(journal,future,0)==EOK);
    size_t owned=jbd_memory_used(journal);
    checkpoint_watch_block=block_size;
    memset(checkpoint_home_writes,0,sizeof(checkpoint_home_writes));
    if (fault==1) disk.fail_write=disk.writes+1;
    if (fault==2) disk.fail_flush=disk.flushes+1;
    if (fault==3) disk.fail_write=disk.writes+3;
    if (fault==4) disk.fail_flush=disk.flushes+2;
    int result=jbd_journal_checkpoint(journal);
    checkpoint_watch_block=0;
    CHECK(result==(fault?EIO:EOK));
    if (!fault) CHECK(checkpoint_home_writes[0]==1 && !checkpoint_home_writes[1] && checkpoint_home_writes[2]==1);
    CHECK(jbd_journal_retire(journal,result)==result);
    if (fault) {
        CHECK(journal->error==EIO && journal->checkpoint_count==3 && jbd_memory_used(journal)==owned);
    } else {
        CHECK(!journal->checkpoint_count && !journal->checkpointing);
        CHECK(ext4_blocks_get_direct(fs->bdev,bytes,checkpoint_homes[0],1)==EOK);
        for (unsigned i=0;i<block_size;i++) CHECK(bytes[i]==0x73);
        CHECK(ext4_blocks_get_direct(fs->bdev,bytes,checkpoint_homes[1],1)==EOK);
        for (unsigned i=0;i<block_size;i++) CHECK(bytes[i]==0x5a);
        CHECK(ext4_blocks_get_direct(fs->bdev,bytes,checkpoint_homes[2],1)==EOK);
        for (unsigned i=0;i<block_size;i++) CHECK(bytes[i]==0x73);
    }
    free(bytes);
    CHECK(fault_block_crash(&disk)==0);
    printf("checkpoint collapse block=%u fault=%u: latest incarnation, revoke suppression, durable owners\n",block_size,fault);
}
static void group_checkpoint_recovery(struct ext4_fs *fs)
{
    ext4_file file; size_t count; unsigned size=fs->bdev->lg_bsize;
    unsigned char *bytes=malloc(3*size); CHECK(bytes);
    CHECK(ext4_fopen(&file,"/file","r")==EOK);
    CHECK(ext4_fread(&file,bytes,3*size,&count)==EOK && count==3*size);
    for (unsigned i=0;i<3*size;i++) CHECK(bytes[i]==(i/size==1?0x5a:0x73));
    CHECK(ext4_fclose(&file)==EOK); free(bytes);
}
static void group_checkpoint_batching(struct ext4_fs *fs)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file file; CHECK(ext4_fopen(&file,"/file","r+")==EOK);
    struct jbd_journal *journal=fs->jbd_journal;
    struct ext4_journal_progress progress;
    uint64_t checkpoint=journal->checkpoint_sequence;
    struct ext4_timestamp times[3]={{9123,11},{9456,22},{9789,33}};
    for (unsigned group=1;group<=4;group++) {
        times[0].nanoseconds=group;
        CHECK(ext4_file_set_times(&file,7,times)==EOK);
        CHECK(ext4_journal_group_commit("/",true)==EOK);
        CHECK(journal->checkpoint_count==group && journal->checkpoint_sequence==checkpoint);
        CHECK(ext4_journal_group_progress("/",&progress)==EOK);
        CHECK(progress.checkpoint_pending && !progress.commit_ready && progress.durable==progress.accepted);
        if (group<4) {
            CHECK(!progress.checkpoint_ready && !progress.ready);
            uint64_t writes=disk.writes,flushes=disk.flushes;
            CHECK(ext4_journal_group_service("/",false)==EOK);
            CHECK(disk.writes==writes && disk.flushes==flushes && journal->checkpoint_count==group);
        } else CHECK(progress.checkpoint_ready && progress.ready);
    }
    unsigned block_size=fs->bdev->lg_bsize;
    unsigned char *expected=malloc(block_size),*actual=malloc(block_size); CHECK(expected && actual);
    struct ext4_inode_ref inode; CHECK(ext4_fs_get_inode_ref(fs,file.inode,&inode)==EOK);
    checkpoint_homes[0]=checkpoint_homes[1]=checkpoint_homes[2]=inode.block.lb_id;
    memcpy(expected,inode.block.data,block_size);
    CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
    checkpoint_watch_block=block_size;
    memset(checkpoint_home_writes,0,sizeof(checkpoint_home_writes));
    uint64_t flushes=disk.flushes;
    CHECK(ext4_journal_group_service("/",false)==EOK);
    checkpoint_watch_block=0;
    CHECK(disk.flushes==flushes+2 && checkpoint_home_writes[0]==1);
    CHECK(journal->checkpoint_sequence==progress.durable && !journal->checkpoint_count);
    CHECK(ext4_blocks_get_direct(fs->bdev,actual,checkpoint_homes[0],1)==EOK);
    CHECK(!memcmp(actual,expected,block_size));
    CHECK(ext4_journal_group_progress("/",&progress)==EOK &&
        !progress.checkpoint_pending && !progress.checkpoint_ready && !progress.ready);
    free(actual); free(expected); CHECK(ext4_fclose(&file)==EOK);
    puts("checkpoint batching: 1/2/3 durable groups wait without I/O; fourth group merges one home version with two flushes");
}

static void group_checkpoint_target(struct ext4_fs *fs)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file file; CHECK(ext4_fopen(&file,"/file","r+")==EOK);
    struct jbd_journal *journal=fs->jbd_journal;
    struct ext4_timestamp times[3]={{10123,11},{10456,22},{10789,33}};
    for (unsigned group=0;group<10;group++) {
        times[0].nanoseconds=group;
        CHECK(ext4_file_set_times(&file,7,times)==EOK && ext4_journal_group_commit("/",true)==EOK);
    }
    CHECK(journal->checkpoint_count==10);
    uint64_t target=journal->durable_sequence,flushes=disk.flushes;
    CHECK(ext4_journal_group_checkpoint("/")==EOK && disk.flushes==flushes+2);
    CHECK(journal->checkpoint_count==2 && journal->checkpoint_sequence==target-2);
    struct ext4_journal_progress progress;
    CHECK(ext4_journal_group_progress("/",&progress)==EOK &&
        progress.checkpoint_pending && progress.checkpoint_ready);
    flushes=disk.flushes;
    CHECK(ext4_journal_group_service("/",false)==EOK && disk.flushes==flushes+2);
    CHECK(!journal->checkpoint_count && journal->checkpoint_sequence==target);

    /* Reaching this request must not turn every later commit into an
     * immediate checkpoint. A new single group has its own wait boundary. */
    times[0].nanoseconds=10;
    CHECK(ext4_file_set_times(&file,7,times)==EOK && ext4_journal_group_commit("/",true)==EOK);
    CHECK(ext4_journal_group_progress("/",&progress)==EOK &&
        progress.checkpoint_pending && !progress.checkpoint_ready && !progress.ready);
    uint64_t writes=disk.writes; flushes=disk.flushes;
    CHECK(ext4_journal_group_service("/",false)==EOK && disk.writes==writes && disk.flushes==flushes);
    CHECK(journal->checkpoint_count==1 && journal->checkpoint_sequence==target);
    CHECK(ext4_journal_group_checkpoint("/")==EOK && disk.flushes==flushes+2);
    CHECK(!journal->checkpoint_count && journal->checkpoint_sequence==journal->durable_sequence);
    CHECK(ext4_fclose(&file)==EOK);
    puts("checkpoint target: ten-group explicit request drains eight plus two, retires its target, later single group waits until explicitly requested");
}
/* A clear single-bit allocation window checks retention through each I/O
 * stage without depending on the journal's former scalar index API. */
static bool quarantine_blocks_reuse(struct ext4_fs *fs, ext4_fsblk_t address, bool inode)
{
    uint8_t bitmap=0; uint32_t found,length;
    int result=jbd_journal_find_reusable(fs,&bitmap,address,0,1,1,inode,&found,&length);
    CHECK(result==EOK || result==ENOSPC);
    CHECK(length==(result==EOK ? 1U : 0U));
    if (result==EOK) CHECK(!found);
    return result==ENOSPC;
}
static void group_quarantine(struct ext4_fs *fs, bool failed)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    struct jbd_journal *journal=fs->jbd_journal;
    ext4_file file; CHECK(ext4_fopen(&file,"/file","r+")==EOK);
    struct ext4_inode_ref inode; ext4_fsblk_t home;
    CHECK(ext4_fs_get_inode_ref(fs,file.inode,&inode)==EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&inode,0,&home,false)==EOK && home);
    CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
    struct ext4_block held=EXT4_BLOCK_ZERO(),lookup=EXT4_BLOCK_ZERO();
    CHECK(ext4_block_get(fs->bdev,&held,home)==EOK);
    CHECK(ext4_transaction_begin("/")==EOK);
    CHECK(jbd_trans_quarantine(fs,home,1,false)==EOK);
    CHECK(quarantine_blocks_reuse(fs,home,false));
    CHECK(jbd_trans_quarantine(fs,home,1,false)==EUCLEAN);
    CHECK(jbd_trans_quarantine(fs,UINT64_MAX,2,false)==EINVAL);
    for (unsigned i=0;i<64;i++) CHECK(jbd_trans_quarantine(fs,1000+4*i,2,false)==EOK);
    CHECK(jbd_trans_quarantine(fs,1001,4,false)==EUCLEAN);
    CHECK(jbd_trans_quarantine(fs,1000,2,true)==EOK);
    CHECK(journal->quarantined_blocks==129 && journal->quarantined_inodes==2);
    /* Compare whole allocation windows with independent occupied-bit and
     * quarantine arrays, including separate inode/block address spaces. */
    uint8_t empty[35]={0};
    for (unsigned kind=0;kind<2;kind++) {
        bool covered[280]={0};
        for (unsigned bit=0;bit<280;bit++) {
            ext4_fsblk_t address=990+bit;
            if (kind) covered[bit]=address>=1000 && address<1002;
            else for (unsigned i=0;i<65;i++) {
                ext4_fsblk_t first=i==64 ? home : 1000+4*i;
                unsigned count=i==64 ? 1 : 2;
                if (address>=first && address-first<count) covered[bit]=true;
            }
        }
        for (uint32_t start=0;start<280;start++) {
            uint32_t expected=start,count=0,found,length;
            while (expected<280 && covered[expected]) expected++;
            while (expected+count<280 && !covered[expected+count]) count++;
            int result=jbd_journal_find_reusable(fs,empty,990,start,280,280,kind,&found,&length);
            CHECK(result==(count ? EOK : ENOSPC) && length==count);
            if (count) CHECK(found==expected);
        }
    }
    CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED);
    CHECK(!journal->quarantined_blocks && !journal->quarantined_inodes);
    CHECK(!quarantine_blocks_reuse(fs,home,false));
    CHECK(ext4_block_get(fs->bdev,&lookup,home)==EOK && lookup.buf==held.buf);
    CHECK(ext4_block_set(fs->bdev,&lookup)==EOK);

    CHECK(ext4_transaction_begin("/")==EOK);
    size_t original_limit=jbd_memory_limit(journal);
    jbd_memory_set_limit(journal, jbd_memory_used(journal));
    unsigned reserved=0;
    for (;;) {
        CHECK(reserved<4096);
        int result=jbd_trans_quarantine(fs,2000+2*reserved,1,false);
        if (result==ENOMEM) break;
        CHECK(result==EOK);
        reserved++;
    }
    CHECK(reserved && journal->quarantined_blocks==reserved);
    CHECK(!quarantine_blocks_reuse(fs,2000+2*reserved,false));
    CHECK(jbd_memory_used(journal)<=jbd_memory_limit(journal));
    jbd_memory_set_limit(journal, original_limit);
    CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED && !journal->quarantined_blocks);

    /* Run the real bitmap allocator against separated reserved free ranges.
     * Abort owns every metadata change, so these temporary allocations never
     * become unreferenced blocks in the persisted filesystem. */
    struct ext4_block_group_ref bg; struct ext4_block bitmap=EXT4_BLOCK_ZERO();
    CHECK(ext4_fs_get_block_group_ref(fs,0,&bg)==EOK);
    CHECK(ext4_block_get(fs->bdev,&bitmap,ext4_bg_get_block_bitmap(bg.block_group,&fs->sb))==EOK);
    uint32_t bit=0,end=ext4_blocks_in_group_cnt(&fs->sb,0);
    while (bit+16<end) {
        unsigned length=0;
        while (length<16 && ext4_bmap_is_bit_clr(bitmap.data,bit+length)) length++;
        if (length==16) break;
        bit+=length+1;
    }
    CHECK(bit+16<end);
    ext4_fsblk_t first=ext4_fs_bg_idx_to_addr(&fs->sb,bit,0), allocated; uint32_t count;
    CHECK(ext4_block_set(fs->bdev,&bitmap)==EOK && ext4_fs_put_block_group_ref(&bg)==EOK);
    uint64_t free_blocks=ext4_sb_get_free_blocks_cnt(&fs->sb);
    CHECK(ext4_transaction_begin("/")==EOK);
    CHECK(jbd_trans_quarantine(fs,first+2,4,false)==EOK && jbd_trans_quarantine(fs,first+8,4,false)==EOK);
    CHECK(ext4_fs_get_inode_ref(fs,file.inode,&inode)==EOK);
    CHECK(ext4_balloc_alloc_blocks(&inode,first+2,8,&allocated,&count)==EOK && allocated==first+6 && count==2);
    CHECK(ext4_balloc_alloc_blocks(&inode,first,8,&allocated,&count)==EOK && allocated==first && count==2);
    CHECK(ext4_balloc_alloc_blocks(&inode,first+8,4,&allocated,&count)==EOK && allocated==first+12 && count==4);
    CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
    CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED);
    CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb)==free_blocks && !journal->quarantined_blocks);

    struct ext4_timestamp times[3]={{1111,1},{2222,2},{3333,3}};
    CHECK(ext4_transaction_begin("/")==EOK && ext4_file_set_times(&file,7,times)==EOK);
    CHECK(jbd_trans_quarantine(fs,home,1,false)==EOK && ext4_transaction_end("/")==EOK);
    times[0].nanoseconds++;
    CHECK(ext4_transaction_begin("/")==EOK && ext4_file_set_times(&file,7,times)==EOK);
    CHECK(jbd_trans_quarantine(fs,home+1,1,false)==EOK && ext4_transaction_end("/")==EOK);
    CHECK(journal->quarantined_blocks==2 && jbd_journal_freeze(journal)==EOK);
    CHECK(ext4_transaction_begin("/")==EOK && jbd_trans_quarantine(fs,home+2,1,false)==EOK);
    CHECK(journal->quarantined_blocks==3 && ext4_transaction_abort("/",ECANCELED)==ECANCELED);
    CHECK(journal->quarantined_blocks==2);
    CHECK(quarantine_blocks_reuse(fs,home,false) && jbd_journal_select(journal) && journal->committing);
    CHECK(jbd_journal_submit(journal)==EOK && jbd_journal_retire(journal,EOK)==EOK);
    CHECK(journal->quarantined_blocks==2 && jbd_journal_select(journal) && journal->checkpointing);
    CHECK(jbd_journal_checkpoint(journal)==EOK && quarantine_blocks_reuse(fs,home,false));
    CHECK(jbd_journal_retire(journal,EOK)==EOK && !journal->quarantined_blocks);
    CHECK(!quarantine_blocks_reuse(fs,home,false));
    /* No old block_rec/revoke exists for this data block. Its actual q owner
     * still retires lookup, while the old reader remains a stable snapshot. */
    CHECK(ext4_block_get(fs->bdev,&lookup,home)==EOK && lookup.buf!=held.buf);
    CHECK(!memcmp(lookup.data,held.data,fs->bdev->lg_bsize));
    CHECK(ext4_block_set(fs->bdev,&lookup)==EOK && ext4_block_set(fs->bdev,&held)==EOK);
    if (failed) {
        times[0].seconds=4444;
        CHECK(ext4_transaction_begin("/")==EOK && ext4_file_set_times(&file,7,times)==EOK);
        CHECK(jbd_trans_quarantine(fs,home,1,false)==EOK && ext4_transaction_end("/")==EOK);
        CHECK(jbd_journal_freeze(journal)==EOK && jbd_journal_select(journal) && journal->committing);
        CHECK(ext4_block_get(fs->bdev,&held,home)==EOK);
        disk.fail_write=disk.writes+1;
        CHECK(jbd_journal_submit(journal)==EIO && jbd_journal_retire(journal,EIO)==EIO);
        CHECK(journal->quarantined_blocks==1 && quarantine_blocks_reuse(fs,home,false));
        CHECK(ext4_block_get(fs->bdev,&lookup,home)==EOK && lookup.buf==held.buf);
        CHECK(ext4_block_set(fs->bdev,&lookup)==EOK && ext4_block_set(fs->bdev,&held)==EOK);
        CHECK(fault_block_crash(&disk)==0);
    } else CHECK(ext4_fclose(&file)==EOK);
    printf("quarantine index block=%u error=%d: interval holes, double-free, exact counts, bitmap skip, abort and durable owner lifetime\n",fs->bdev->lg_bsize,failed);
}
static void group_fault(struct ext4_fs *fs, const char *kind, unsigned point)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file f; CHECK(ext4_fopen(&f,"/file","r+")==EOK);
    struct ext4_timestamp times[3]={{1111,1},{2222,2},{3333,3}};
    CHECK(ext4_file_set_times(&f,7,times)==EOK);
    if(!strcmp(kind,"group-write"))disk.fail_write=disk.writes+point;
    else disk.fail_flush=disk.flushes+point;
    int result=ext4_journal_group_service("/",true);
    if(result==EOK)result=ext4_journal_group_drain("/");
    CHECK(result==EIO);
    CHECK(fs->jbd_journal->error==EIO && fs->jbd_journal->failed_trans &&
        (fs->jbd_journal->failed_trans==fs->jbd_journal->committing ||
         fs->jbd_journal->failed_trans==fs->jbd_journal->checkpointing));
    CHECK(ext4_file_set_times(&f,7,times)==EIO);
    CHECK(ext4_journal_group_drain("/")==EIO);
    CHECK(fault_block_crash(&disk)==0);
}
struct owned_file_page { unsigned references, retains, releases; unsigned char *bytes; };
static void owned_page_retain(void *context)
{
    struct owned_file_page *page=context;
    CHECK(page->references && page->references<100);
    page->retains++;
    page->references++;
}
static void owned_page_release(void *context)
{
    struct owned_file_page *page=context;
    CHECK(page->references);
    page->releases++;
    if (!--page->references) { free(page->bytes); page->bytes=NULL; }
}
static void owned_page_init(struct owned_file_page *page,size_t size,unsigned char value)
{
    *page=(struct owned_file_page){.references=1,.bytes=malloc(size)};
    CHECK(page->bytes); memset(page->bytes,value,size);
}
static void group_checkpoint_pressure(struct ext4_fs *fs)
{
    enum { PAGES=17, PAGE_BYTES=16384 };
    const size_t bytes=PAGES*PAGE_BYTES,block=fs->bdev->lg_bsize;
    ext4_file file; CHECK(ext4_fopen(&file,"/file","r+")==EOK);
    unsigned char *readback=malloc(bytes); CHECK(readback); memset(readback,0x5a,bytes);
    size_t count;
    CHECK(ext4_fpwrite(&file,0,readback,bytes,&count)==EOK && count==bytes);
    struct ext4_journal_runtime runtime={.now_ns=group_clock,.request=group_request};
    CHECK(ext4_journal_group_enable("/",&runtime,512*1024)==EOK);
    struct jbd_journal *journal=fs->jbd_journal;
    ext4_fsblk_t homes[PAGES*PAGE_BYTES/1024];
    struct ext4_inode_ref inode; CHECK(ext4_fs_get_inode_ref(fs,file.inode,&inode)==EOK);
    for (unsigned i=0;i<bytes/block;i++)
        CHECK(ext4_fs_get_inode_dblk_idx(&inode,i,&homes[i],false)==EOK && homes[i]);
    CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
    struct ext4_timestamp times[3]={{11123,11},{11456,22},{11789,33}};
    CHECK(ext4_file_set_times(&file,7,times)==EOK && ext4_journal_group_commit("/",true)==EOK);
    struct ext4_journal_progress progress;
    CHECK(ext4_journal_group_progress("/",&progress)==EOK &&
        progress.checkpoint_pending && !progress.checkpoint_ready && journal->checkpoint_count==1);
    uint64_t target=progress.durable;
    struct owned_file_page pages[PAGES];
    for (unsigned page=0;page<PAGES;page++) {
        owned_page_init(&pages[page],PAGE_BYTES,(unsigned char)(0x60+page));
        struct file_data_source source={&pages[page],owned_page_retain,owned_page_release,PAGE_BYTES};
        struct jbd_trans *operation; CHECK(jbd_journal_new_trans(journal, &operation) == EOK); CHECK(operation);
        for (unsigned i=0;i<PAGE_BYTES/block;i++)
            CHECK(jbd_trans_write_data(operation,homes[page*PAGE_BYTES/block+i],0,
                pages[page].bytes+i*block,block,false,&source)==EOK);
        CHECK(jbd_trans_prepare_operation(operation) == EOK);
        CHECK(jbd_journal_accept(journal,operation,0)==EOK);
        CHECK(pages[page].references==2 && pages[page].retains==1);
        owned_page_release(&pages[page]);
    }
    CHECK(jbd_journal_freeze(journal)==EOK && journal->sealed_count==1);
    unsigned checkpoint_requests=group_work[1];
    times[0].nanoseconds++;
    CHECK(ext4_file_set_times(&file,7,times)==EOK && journal->running);
    CHECK(group_work[1]==checkpoint_requests+1);
    CHECK(jbd_memory_used(journal)-jbd_memory_cached(journal)>jbd_memory_limit(journal)/2);
    CHECK(jbd_memory_used(journal)<=jbd_memory_limit(journal));
    CHECK(ext4_journal_group_progress("/",&progress)==EOK &&
        progress.commit_ready && progress.checkpoint_ready && progress.checkpoint_pending);
    struct jbd_trans *committing=TAILQ_FIRST(&journal->sealed_queue);
    CHECK(jbd_journal_select_stage(journal,JBD_JOURNAL_COMMIT)==JBD_JOURNAL_COMMIT &&
        journal->committing==committing && journal->running);
    CHECK(ext4_journal_group_progress("/",&progress)==EOK &&
        !progress.commit_ready && progress.checkpoint_ready && progress.checkpoint_pending);
    uint64_t flushes=disk.flushes;
    CHECK(ext4_journal_group_service("/",false)==EOK && disk.flushes==flushes+2);
    CHECK(journal->checkpoint_sequence==target && !journal->checkpoint_count &&
        journal->committing==committing && journal->running);
    for (unsigned page=0;page<PAGES;page++) CHECK(pages[page].references==1 && pages[page].releases==1);
    CHECK(jbd_journal_submit(journal)==EOK &&
        jbd_journal_retire_stage(journal,JBD_JOURNAL_COMMIT,EOK)==EOK);
    CHECK(ext4_journal_group_drain("/")==EOK);
    for (unsigned page=0;page<PAGES;page++)
        CHECK(!pages[page].references && !pages[page].bytes && pages[page].releases==2);
    CHECK(ext4_fpread(&file,0,readback,bytes,&count)==EOK && count==bytes);
    for (size_t i=0;i<bytes;i++) CHECK(readback[i]==0x60+i/PAGE_BYTES);
    CHECK(jbd_memory_peak(journal)<=jbd_memory_limit(journal));
    free(readback); CHECK(ext4_fclose(&file)==EOK);
    puts("checkpoint pressure: actual retained sources cross half budget; one pending group checkpoints beside committing/running owners, exact source release and readback");
}
static struct owned_file_page direct_pages[32];
static unsigned direct_batches, direct_fail_batch;
static unsigned direct_boundary_calls;
static int direct_boundary_result;
static int direct_boundary_batch(struct ext4_blockdev *bdev,
    const struct kernel_block_span *spans, unsigned count)
{
    CHECK(count==2 && spans[0].offset==bdev->part_offset+bdev->lg_bsize &&
        spans[1].offset==bdev->part_offset+4*bdev->lg_bsize &&
        spans[0].size==bdev->lg_bsize && spans[1].size==2*bdev->lg_bsize);
    direct_boundary_calls++;
    return direct_boundary_result;
}
static int direct_boundary_single(struct ext4_blockdev *bdev, const void *data,
    uint64_t block, uint32_t count)
{
    CHECK(data && direct_boundary_calls<2);
    CHECK(block==(bdev->part_offset+(direct_boundary_calls?4:1)*bdev->lg_bsize)/512 &&
        count==(direct_boundary_calls?2:1)*bdev->lg_bsize/512);
    return ++direct_boundary_calls==2 ? EIO : EOK;
}
static void direct_batch_boundaries(struct ext4_blockdev *source)
{
    struct ext4_blockdev_iface iface=*source->bdif;
    struct ext4_blockdev bdev=*source;
    bdev.bdif=&iface;bdev.part_offset=7*512;
    iface.bwrite_batch=direct_boundary_batch;
    unsigned char data[8192];
    struct kernel_block_span spans[2]={{bdev.lg_bsize,data,bdev.lg_bsize},{4*bdev.lg_bsize,data,2*bdev.lg_bsize}}, saved[2];
    memcpy(saved,spans,sizeof(saved));direct_boundary_calls=0;direct_boundary_result=EIO;
    CHECK(ext4_blocks_set_batch(&bdev,spans,2)==EIO && direct_boundary_calls==1);
    CHECK(!memcmp(spans,saved,sizeof(spans)));
    direct_boundary_result=EOK;
    CHECK(ext4_blocks_set_batch(&bdev,spans,2)==EOK && direct_boundary_calls==2);
    CHECK(!memcmp(spans,saved,sizeof(spans)));
    spans[1].offset=bdev.lg_bcnt*bdev.lg_bsize;
    memcpy(saved,spans,sizeof(saved));
    CHECK(ext4_blocks_set_batch(&bdev,spans,2)==EINVAL && direct_boundary_calls==2);
    CHECK(!memcmp(spans,saved,sizeof(spans)));
    spans[1].offset=bdev.lg_bsize;
    CHECK(ext4_blocks_set_batch(&bdev,spans,2)==EINVAL && direct_boundary_calls==2);
    spans[1].offset=4*bdev.lg_bsize;memcpy(saved,spans,sizeof(saved));
    iface.bwrite_batch=NULL;iface.bwrite=direct_boundary_single;direct_boundary_calls=0;
    CHECK(ext4_blocks_set_batch(&bdev,spans,2)==EIO && direct_boundary_calls==2);
    CHECK(!memcmp(spans,saved,sizeof(spans)));
}
static int direct_write_batch(struct ext4_blockdev *bdev,const struct kernel_block_span *spans,unsigned count)
{
    bool payload = false;
    size_t bytes=0;
    for (unsigned i=0;i<count;i++) {
        uintptr_t p=(uintptr_t)spans[i].buffer;
        bool matched=false;
        for (unsigned j=0;j<32;j++) {
            uintptr_t base=(uintptr_t)direct_pages[j].bytes;
            if (base && p>=base && p-base<8192) {
                CHECK(spans[i].size<=8192-(p-base));
                matched=true; break;
            }
        }
        CHECK(matched); payload=true; bytes+=spans[i].size;
    }
    CHECK(payload && bytes<=131072);
    if (++direct_batches==direct_fail_batch) return EIO;
    for (unsigned i=0;i<count;i++) {
        int r=dev_write(bdev,spans[i].buffer,spans[i].offset/512,spans[i].size/512);
        if (r) return r;
    }
    return EOK;
}
static struct ext4_inode *direct_cleanup_leaf;
static int direct_full_leaf_write_batch(struct ext4_blockdev *bdev,const struct kernel_block_span *spans,unsigned count)
{
    int r=direct_write_batch(bdev,spans,count);
    if(r) {
        uint16_t header[6];memcpy(header,direct_cleanup_leaf->blocks,sizeof(header));
        CHECK(to_le16(header[0])==0xf30a && to_le16(header[1])==4 &&
            to_le16(header[2])==4 && !header[3]);
        /* Cleanup may allocate extent search paths, but removing the failed
         * tail must not need a new block buffer for an extent-tree split. */
        denied_block_buffer_size=bdev->lg_bsize;
    }
    return r;
}
static void direct_data_batch(struct ext4_fs *fs,bool fail)
{
    CHECK(!fs->jbd_journal);
    direct_batch_boundaries(fs->bdev);
    ext4_file file; CHECK(ext4_fopen(&file,"/file","r+")==EOK);
    struct ext4_inode_ref inode; ext4_fsblk_t physical;
    CHECK(ext4_fs_get_inode_ref(fs,file.inode,&inode)==EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&inode,0,&physical,true)==EOK && physical);
    CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
    struct ext4_block held=EXT4_BLOCK_ZERO(); CHECK(ext4_block_get(fs->bdev,&held,physical)==EOK);
    struct file_write_span spans[32];
    for (unsigned i=0;i<32;i++) {
        owned_page_init(&direct_pages[i],8192,0x60+i);
        spans[i]=(struct file_write_span){.offset=i*8192,.buffer=direct_pages[i].bytes,.size=8192,
            .source={&direct_pages[i],owned_page_retain,owned_page_release,8192},
            .source_buffer=direct_pages[i].bytes,.source_size=8192};
    }
    memset(direct_pages[0].bytes,0x5a,17); spans[0].offset=17; spans[0].buffer=direct_pages[0].bytes+17; spans[0].size-=17;
    spans[31].size=37; memset(direct_pages[31].bytes+37,0,8192-37);
    direct_batches=0; direct_fail_batch=fail?2:0;
    fs->bdev->bdif->bwrite_batch=direct_write_batch;
    int r=ext4_fpwrite_batch(&file,spans,32);
    fs->bdev->bdif->bwrite_batch=NULL;
    CHECK(r==(fail?EIO:EOK) && direct_batches==2);
    size_t expected=fail?16*8192:31*8192+37;
    CHECK(file.fsize==expected);
    for (unsigned i=0;i<32;i++) {
        CHECK(spans[i].written==(fail && i>=16?0:spans[i].size));
        CHECK(direct_pages[i].references==1 && !direct_pages[i].retains);
    }
    CHECK(!memcmp(held.data,direct_pages[0].bytes,fs->bdev->lg_bsize));
    CHECK(ext4_block_set(fs->bdev,&held)==EOK);
    unsigned char *bytes=malloc(expected+8192); CHECK(bytes); size_t read;
    CHECK(ext4_fpread(&file,0,bytes,expected+8192,&read)==EOK && read==expected);
    for (size_t i=0;i<expected;i++) CHECK(bytes[i]==(i<17?0x5a:0x60+i/8192));
    /* Failed newly allocated blocks must not survive outside the accepted EOF. */
    CHECK(ext4_fs_get_inode_ref(fs,file.inode,&inode)==EOK);
    if (fail) for (size_t i=expected/fs->bdev->lg_bsize;i<32*8192/fs->bdev->lg_bsize;i++) {
        CHECK(ext4_fs_get_inode_dblk_idx(&inode,i,&physical,true)==EOK && !physical);
    }
    CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
    free(bytes); for (unsigned i=0;i<32;i++) owned_page_release(&direct_pages[i]);
    CHECK(ext4_fclose(&file)==EOK);
    printf("nonjournal stable batch block=%u failed=%d: 128KiB vectors, exact prefix/EOF, held cache coherent, failed mappings released\n",fs->bdev->lg_bsize,fail);
}

static void group_partial_owner(struct ext4_fs *fs, bool failed)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file file; CHECK(ext4_fopen(&file,"/file","r+")==EOK);
    size_t block=fs->bdev->lg_bsize, count;
    struct ext4_inode_ref inode;
    ext4_fsblk_t physical;
    CHECK(ext4_fs_get_inode_ref(fs,file.inode,&inode)==EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&inode,0,&physical,true)==EOK && physical);
    CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
    struct owned_file_page old, replacement, tail;
    owned_page_init(&old,block,0x5a); memset(old.bytes+17,0xa7,13);
    struct file_write_span span={.offset=17,.buffer=old.bytes+17,.size=13,
        .source={&old,owned_page_retain,owned_page_release,block},
        .source_buffer=old.bytes,.source_size=block};
    unsigned before_reads=reads;
    CHECK(ext4_fpwrite_batch(&file,&span,1)==EOK && span.written==13);
    CHECK(old.references==2 && old.retains==1 && reads==before_reads);
    struct jbd_data_borrow borrow = {0};
    CHECK(jbd_journal_borrow_data(fs,physical,&borrow) && borrow.data==old.bytes && file.fsize==4096);
    jbd_journal_release_borrow(&borrow);
    owned_page_init(&replacement,block,0x5a); memset(replacement.bytes+17,0xb9,13);
    span.buffer=replacement.bytes+17; span.source.context=&replacement; span.source_buffer=replacement.bytes;
    CHECK(ext4_transaction_begin("/")==EOK && ext4_fpwrite_batch(&file,&span,1)==EOK);
    CHECK(replacement.references==2 && jbd_journal_borrow_data(fs,physical,&borrow) && borrow.data==replacement.bytes);
    jbd_journal_release_borrow(&borrow);
    CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED && replacement.references==1);
    CHECK(jbd_journal_borrow_data(fs,physical,&borrow) && borrow.data==old.bytes && old.references==2);
    jbd_journal_release_borrow(&borrow);
    CHECK(ext4_fpwrite_batch(&file,&span,1)==EOK && replacement.references==2 && old.references==1);
    owned_page_release(&old);
    /* The full source block covers hole zeros and EOF padding, but only one
     * logical byte grows the file. Bytes after it must never become visible. */
    size_t end=8*block+18;
    owned_page_init(&tail,block,0); tail.bytes[17]=0xdc;
    span=(struct file_write_span){.offset=end-1,.buffer=tail.bytes+17,.size=1,
        .source={&tail,owned_page_retain,owned_page_release,block},
        .source_buffer=tail.bytes,.source_size=block};
    CHECK(ext4_fpwrite_batch(&file,&span,1)==EOK && span.written==1 && file.fsize==end);
    CHECK(tail.references==2);
    unsigned char *readback=malloc(end+block); CHECK(readback);
    CHECK(ext4_fpread(&file,0,readback,end+block,&count)==EOK && count==end);
    for (size_t i=0;i<end;i++) CHECK(readback[i]==(i>=17 && i<30?0xb9:i<4096?0x5a:i==end-1?0xdc:0));
    if (failed) disk.fail_write=disk.writes+1;
    CHECK(ext4_journal_group_drain("/")== (failed?EIO:EOK));
    if (failed) {
        CHECK(replacement.references==2 && tail.references==2);
        memset(readback,0,end);
        CHECK(ext4_fpread(&file,0,readback,end+block,&count)==EOK && count==end);
        for (size_t i=0;i<end;i++) CHECK(readback[i]==(i>=17 && i<30?0xb9:i<4096?0x5a:i==end-1?0xdc:0));
        CHECK(ext4_fpwrite_batch(&file,&span,1)==EIO && !span.written);
    } else {
        CHECK(replacement.references==1 && tail.references==1);
        CHECK(ext4_fpread(&file,end,readback,block,&count)==EOK && !count);
        CHECK(ext4_fclose(&file)==EOK);
    }
    owned_page_release(&replacement); owned_page_release(&tail); free(readback);
    printf("partial stable block=%zu failed=%d: zero old-data reads, exact borrow, abort/replacement, hole/EOF, latest read\n",block,failed);
}

static void group_data_owner(struct ext4_fs *fs)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file file; CHECK(ext4_fopen(&file,"/file","r+")==EOK);
    const size_t block=fs->bdev->lg_bsize;
    struct owned_file_page pages[8];
    struct file_write_span spans[8];
    for (unsigned i=0;i<8;i++) {
        owned_page_init(&pages[i],block,0x20+i);
        spans[i]=(struct file_write_span){.offset=i*block,.buffer=pages[i].bytes,.size=block,
            .source={&pages[i],owned_page_retain,owned_page_release,block}};
    }
    uint64_t writes=disk.writes,flushes=disk.flushes;
    CHECK(ext4_transaction_begin("/")==EOK);
    CHECK(ext4_fpwrite_batch(&file,spans,8)==EOK);
    for (unsigned i=0;i<8;i++) CHECK(spans[i].written==block && pages[i].references==2);
    CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED);
    for (unsigned i=0;i<8;i++) CHECK(pages[i].references==1);
    CHECK(ext4_fclose(&file)==EOK && ext4_fopen(&file,"/file","r+")==EOK);
    unsigned char *readback=malloc(8*block); CHECK(readback);
    size_t count;
    CHECK(ext4_fpread(&file,0,readback,4096,&count)==EOK && count==4096);
    for (unsigned i=0;i<4096;i++) CHECK(readback[i]==0x5a);
    CHECK(ext4_fpwrite_batch(&file,spans,8)==EOK);
    for (unsigned i=0;i<8;i++) { CHECK(pages[i].references==2); owned_page_release(&pages[i]); }
    /* Replace one running version. Its old bytes no longer have a borrower. */
    struct owned_file_page replacement;
    owned_page_init(&replacement,block,0x91);
    struct file_write_span replace={.offset=0,.buffer=replacement.bytes,.size=block,
        .source={&replacement,owned_page_retain,owned_page_release,block}};
    CHECK(ext4_fpwrite_batch(&file,&replace,1)==EOK && replace.written==block);
    owned_page_release(&replacement);
    CHECK(!pages[0].references && !pages[0].bytes && replacement.references==1);
    /* A failed later operation discards its private page and preserves the
     * earlier accepted version, without writing or undoing that earlier page. */
    struct owned_file_page aborted;
    owned_page_init(&aborted,block,0xee);
    replace=(struct file_write_span){.offset=block,.buffer=aborted.bytes,.size=block,
        .source={&aborted,owned_page_retain,owned_page_release,block}};
    CHECK(ext4_transaction_begin("/")==EOK && ext4_fpwrite_batch(&file,&replace,1)==EOK);
    CHECK(aborted.references==2 && ext4_transaction_abort("/",ECANCELED)==ECANCELED && aborted.references==1);
    owned_page_release(&aborted);
    CHECK(ext4_fclose(&file)==EOK && ext4_fopen(&file,"/file","r+")==EOK);
    CHECK(ext4_fpread(&file,0,readback,8*block,&count)==EOK && count==8*block);
    for (unsigned i=0;i<8;i++) for (size_t j=0;j<block;j++)
        CHECK(readback[i*block+j]==(i?0x20+i:0x91));
    CHECK(disk.writes==writes && disk.flushes==flushes);
    /* Full-block payload versions must not occupy a second bcache entry. */
    struct ext4_inode_ref ref; CHECK(ext4_fs_get_inode_ref(fs,file.inode,&ref)==EOK);
    for (unsigned i=0;i<8;i++) {
        ext4_fsblk_t physical;
        CHECK(ext4_fs_get_inode_dblk_idx(&ref,i,&physical,true)==EOK && physical);
        struct ext4_block cached=EXT4_BLOCK_ZERO();
        CHECK(!ext4_bcache_find_get(fs->bdev->bc,&cached,physical, NULL));
    }
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    unsigned allocated=allocations; fail_allocation=allocations+1;
    CHECK(ext4_journal_group_service("/",true)==EOK && ext4_journal_group_drain("/")==EOK);
    fail_allocation=0; CHECK(allocations==allocated);
    CHECK(!replacement.references && !replacement.bytes);
    for (unsigned i=0;i<8;i++) CHECK(!pages[i].references && !pages[i].bytes);
    CHECK(ext4_fpread(&file,0,readback,8*block,&count)==EOK && count==8*block);
    for (unsigned i=0;i<8;i++) for (size_t j=0;j<block;j++)
        CHECK(readback[i*block+j]==(i?0x20+i:0x91));
    free(readback); CHECK(ext4_fclose(&file)==EOK);
    puts("group data owner: borrowed pages survive accept, replacement/abort retire exact owners, no second data cache");
}

static const void *batch_payloads[8];
static uint64_t batch_payload_blocks[8];
static size_t batch_payload_bytes;
static unsigned payload_batches, payload_spans;
static int data_phase_write(struct ext4_blockdev *bdev, const struct ext4_block_write *first)
{
    unsigned payloads=0, nodes=0;
    for (const struct ext4_block_write *io=first;io;io=io->next) {
        nodes++;
        for (unsigned j=0;j<8;j++) {
            uintptr_t pointer=(uintptr_t)io->data, start=(uintptr_t)batch_payloads[j];
            if (pointer<start || pointer-start>=batch_payload_bytes) continue;
            CHECK(pointer==start && io->offset==batch_payload_blocks[j]*512 && io->size==batch_payload_bytes);
            payloads++;
        }
    }
    if (payloads) { CHECK(payloads==8 && nodes>=8); payload_batches++; payload_spans+=payloads; }
    for (const struct ext4_block_write *io=first;io;io=io->next) {
        int r=dev_write(bdev,io->data,io->offset/512,io->size/512);
        if (r) return r;
    }
    return EOK;
}
static void group_data_batch(struct ext4_fs *fs)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file file; CHECK(ext4_fopen(&file,"/file","r+")==EOK);
    size_t block=fs->bdev->lg_bsize;
    for (size_t page=4096;page<=16384;page*=4) {
        unsigned char *readback=malloc(10*page); CHECK(readback);
        memset(readback,0x5a,10*page);
        size_t written;
        CHECK(ext4_fseek(&file,0,SEEK_SET)==EOK && ext4_fwrite(&file,readback,10*page,&written)==EOK && written==10*page);
        CHECK(ext4_journal_group_drain("/")==EOK);
        struct ext4_inode_ref inode;
        CHECK(ext4_fs_get_inode_ref(fs,file.inode,&inode)==EOK);
        size_t offsets[8];
        unsigned selected=0;
        for (unsigned candidate=0;candidate<10 && selected<8;candidate++) {
            ext4_fsblk_t first;
            CHECK(ext4_fs_get_inode_dblk_idx(&inode,candidate*page/block,&first,true)==EOK && first);
            bool contiguous=true;
            for (size_t j=1;j<page/block;j++) {
                ext4_fsblk_t next;
                CHECK(ext4_fs_get_inode_dblk_idx(&inode,candidate*page/block+j,&next,true)==EOK && next);
                if (next!=first+j) contiguous=false;
            }
            /* Earlier small extents may leave gaps. Select actual contiguous
             * disk runs rather than making allocation geometry the oracle. */
            if (!contiguous) continue;
            offsets[selected]=candidate*page;
            batch_payload_blocks[selected++]=first*block/512;
        }
        CHECK(selected==8);
        CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
        struct owned_file_page pages[8];
        struct file_write_span spans[8];
        for (unsigned i=0;i<8;i++) {
            /* Padding makes separate pages observably noncontiguous in RAM. */
            owned_page_init(&pages[i],page+512,0x30+i);
            batch_payloads[i]=pages[i].bytes;
            spans[i]=(struct file_write_span){.offset=offsets[i],.buffer=pages[i].bytes,.size=page,
                .source={&pages[i],owned_page_retain,owned_page_release,page+512}};
        }
        CHECK(ext4_fpwrite_batch(&file,spans,8)==EOK);
        for (unsigned i=0;i<8;i++) {
            CHECK(spans[i].written==page && pages[i].references==2 && pages[i].retains==1);
            owned_page_release(&pages[i]);
        }
        batch_payload_bytes=page; payload_batches=payload_spans=0;
        fs->bdev->bdif->bwrite_phase=data_phase_write;
        struct jbd_journal *journal=fs->jbd_journal;
        CHECK(jbd_journal_freeze(journal)==EOK && jbd_journal_select(journal));
        CHECK(jbd_journal_submit(journal)==EOK && jbd_journal_retire(journal,EOK)==EOK);
        fs->bdev->bdif->bwrite_phase=NULL;
        CHECK(payload_batches==1 && payload_spans==8);
        for (unsigned i=0;i<8;i++) CHECK(!pages[i].references && !pages[i].bytes);
        CHECK(ext4_journal_group_drain("/")==EOK);
        for (unsigned i=0;i<8;i++) {
            CHECK(ext4_fpread(&file,offsets[i],readback,page,&written)==EOK && written==page);
            for (size_t j=0;j<page;j++) CHECK(readback[j]==0x30+i);
        }
        printf("group data batch block=%zu page=%zu bytes=%zu spans=%u batches=%u\n",
            block,page,8*page,payload_spans,payload_batches);
        free(readback);
    }
    CHECK(ext4_fclose(&file)==EOK);
}

static void group_borrow_admission(struct ext4_fs *fs)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file file; CHECK(ext4_fopen(&file,"/file","r+")==EOK);
    const size_t bytes=16384, block=fs->bdev->lg_bsize;
    unsigned char *initial=malloc(bytes); CHECK(initial); memset(initial,0x5a,bytes);
    size_t count;
    CHECK(ext4_fwrite(&file,initial,bytes,&count)==EOK && count==bytes);
    CHECK(ext4_journal_group_drain("/")==EOK);
    struct owned_file_page page; owned_page_init(&page,bytes,0x73);
    struct file_write_span span={.buffer=page.bytes,.size=bytes,
        .source={&page,owned_page_retain,owned_page_release,bytes}};
    struct jbd_journal *journal=fs->jbd_journal;
    size_t limit=jbd_memory_limit(journal);
    jbd_memory_set_limit(journal, jbd_memory_used(journal)-jbd_memory_cached(journal)+bytes+12*block+8192);
    CHECK(ext4_fpwrite_batch(&file,&span,1)==EOK && span.written==bytes);
    CHECK(page.references==2 && page.retains==1 && jbd_memory_used(journal)<=jbd_memory_limit(journal));
    owned_page_release(&page);
    CHECK(ext4_journal_group_drain("/")==EOK && !page.references);
    jbd_memory_set_limit(journal, limit);
    CHECK(ext4_fpread(&file,0,initial,bytes,&count)==EOK && count==bytes);
    for (size_t i=0;i<bytes;i++) CHECK(initial[i]==0x73);
    free(initial); CHECK(ext4_fclose(&file)==EOK);
    puts("group admission: borrowed payload charged as one owner and block records, without imaginary data images");
}
static void group_source_budget(struct ext4_fs *fs, bool failed)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file file; CHECK(ext4_fopen(&file,"/file","r+")==EOK);
    const size_t bytes=16384, block=fs->bdev->lg_bsize;
    unsigned blocks=bytes/block;
    unsigned char readback[16384]; memset(readback,0x5a,sizeof(readback));
    size_t count;
    CHECK(ext4_fwrite(&file,readback,bytes,&count)==EOK && count==bytes && ext4_journal_group_drain("/")==EOK);
    ext4_fsblk_t homes[16];
    struct ext4_inode_ref inode;
    CHECK(ext4_fs_get_inode_ref(fs,file.inode,&inode)==EOK);
    for (unsigned i=0;i<blocks;i++) CHECK(ext4_fs_get_inode_dblk_idx(&inode,i,&homes[i],true)==EOK && homes[i]);
    CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
    struct owned_file_page page, rejected;
    owned_page_init(&page,bytes,0x63); owned_page_init(&rejected,bytes,0xee);
    for (unsigned i=0;i<blocks;i++) memset(page.bytes+i*block,0x60+i,block);
    struct file_data_source source={&page,owned_page_retain,owned_page_release,bytes};
    struct jbd_journal *journal=fs->jbd_journal;
    struct jbd_trans *operation; CHECK(jbd_journal_new_trans(journal, &operation) == EOK); CHECK(operation);
    size_t original_limit=jbd_memory_limit(journal), baseline=jbd_memory_used(journal)-jbd_memory_cached(journal);
    jbd_memory_set_limit(journal, baseline+bytes+blocks*(2*sizeof(struct jbd_data)+2*sizeof(void *))+512);
    test_operation_bind(fs,operation);
    for (unsigned i=0;i<blocks;i++) {
        size_t previous=jbd_memory_used(journal);
        CHECK(jbd_trans_write_data(operation,homes[i],0,page.bytes+i*block,block,false,&source)==EOK);
        CHECK(page.references==2 && page.retains==1 && jbd_memory_used(journal)<=jbd_memory_limit(journal));
        if (!i) CHECK(jbd_memory_used(journal)>=baseline+bytes);
        else CHECK(jbd_memory_used(journal)-previous<block);
    }
    struct file_data_source invalid={&rejected,owned_page_retain,owned_page_release,bytes};
    CHECK(jbd_trans_write_data(operation,homes[0],0,rejected.bytes,block,false,&invalid)==ENOMEM);
    struct jbd_data_borrow borrow = {0};
    CHECK(rejected.references==1 && !rejected.retains && jbd_journal_borrow_data(fs,homes[0],&borrow) && borrow.data==page.bytes);
    jbd_journal_release_borrow(&borrow);
    invalid=source; invalid.bytes++;
    CHECK(jbd_trans_write_data(operation,homes[0],0,page.bytes,block,false,&invalid)==EINVAL);
    invalid=source; invalid.release=owned_page_retain;
    CHECK(jbd_trans_write_data(operation,homes[0],0,page.bytes,block,false,&invalid)==EINVAL);
    invalid=(struct file_data_source){.bytes=1};
    CHECK(jbd_trans_write_data(operation,homes[0],0,page.bytes,block,false,&invalid)==EINVAL);
    CHECK(page.references==2 && page.retains==1 && !page.releases);
    test_operation_bind(fs,NULL); jbd_memory_set_limit(journal, original_limit);
    owned_page_release(&rejected);
    CHECK(jbd_trans_prepare_operation(operation) == EOK);
    CHECK(jbd_journal_accept(journal,operation,0)==EOK && jbd_journal_freeze(journal)==EOK && jbd_journal_select(journal));
    /* Private, sealed and running records borrow the same retained owner.
     * Aborting a private operation must leave the accepted blocks readable. */
    CHECK(jbd_journal_new_trans(journal, &operation) == EOK); CHECK(operation);
    CHECK(jbd_trans_write_data(operation,homes[0],0,page.bytes,block,false,&source)==EOK);
    jbd_journal_free_trans(journal,operation,true);
    for (unsigned i=1;i<3;i++) {
        CHECK(jbd_journal_new_trans(journal, &operation) == EOK); CHECK(operation);
        CHECK(jbd_trans_write_data(operation,homes[i],0,page.bytes+i*block,block,false,&source)==EOK);
        CHECK(jbd_trans_prepare_operation(operation) == EOK);
        CHECK(jbd_journal_accept(journal,operation,0)==EOK);
        if (i==1) CHECK(jbd_journal_freeze(journal)==EOK);
    }
    CHECK(page.references==2 && page.retains==1 && !page.releases);
    owned_page_release(&page);
    if (failed) disk.fail_write=disk.writes+1;
    int error=jbd_journal_submit(journal);
    CHECK(error==(failed?EIO:EOK) && jbd_journal_retire(journal,error)==error);
    CHECK(page.references==1 && page.retains==1 && page.releases==1);
    if (!failed) {
        CHECK(ext4_journal_group_drain("/")==EOK);
        CHECK(!page.references && !page.bytes && page.releases==2);
    }
    CHECK(ext4_fpread(&file,0,readback,bytes,&count)==EOK && count==bytes);
    for (unsigned i=0;i<blocks;i++) for (size_t j=0;j<block;j++) CHECK(readback[i*block+j]==0x60+i);
    if (failed) CHECK(fault_block_crash(&disk)==0);
    else CHECK(ext4_fclose(&file)==EOK);
    printf("group source budget block=%zu owner=%zu blocks=%u retains=1 failed=%d: shared across private/sealed/running, exact final release\n",
        block,bytes,blocks,failed);
}

static void group_data_error(struct ext4_fs *fs, bool fail_flush)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file file; CHECK(ext4_fopen(&file,"/file","r+")==EOK);
    size_t block=fs->bdev->lg_bsize, count;
    unsigned char *readback=malloc(4*block); CHECK(readback);
    memset(readback,0x5a,4*block);
    CHECK(ext4_fwrite(&file,readback,4*block,&count)==EOK && count==4*block);
    CHECK(ext4_journal_group_drain("/")==EOK);
    struct owned_file_page pages[4];
    for (unsigned i=0;i<4;i++) owned_page_init(&pages[i],block,0x40+i);
    const unsigned offsets[4]={0,1,2,1};
    struct jbd_journal *journal=fs->jbd_journal;
    for (unsigned i=0;i<4;i++) {
        struct file_write_span span={.offset=offsets[i]*block,
            .buffer=pages[i].bytes,.size=block,
            .source={&pages[i],owned_page_retain,owned_page_release,block}};
        CHECK(ext4_fpwrite_batch(&file,&span,1)==EOK && span.written==block);
        owned_page_release(&pages[i]);
        if (i<2) CHECK(jbd_journal_freeze(journal)==EOK);
        if (!i) CHECK(jbd_journal_select(journal) && journal->committing);
    }
    CHECK(journal->running && journal->sealed_count==1 && journal->committing);
    if (fail_flush) disk.fail_flush=disk.flushes+1;
    else disk.fail_write=disk.writes+1;
    int error=jbd_journal_submit(journal);
    CHECK(error==EIO && jbd_journal_retire(journal,error)==EIO);
    CHECK(journal->error==EIO && journal->failed_trans==journal->committing);
    /* DMA has ended, but these are still the live filesystem's current
     * versions. Later sealed/running groups must keep their sources too. */
    for (unsigned i=0;i<4;i++) CHECK(pages[i].references==1 && pages[i].bytes);
    uint64_t writes=disk.writes, flushes=disk.flushes;
    unsigned before_reads=reads;
    CHECK(ext4_fpread(&file,0,readback,4*block,&count)==EOK && count==4*block);
    const unsigned char values[4]={0x40,0x43,0x42,0x5a};
    for (unsigned i=0;i<4;i++) for (size_t j=0;j<block;j++) CHECK(readback[i*block+j]==values[i]);
    CHECK(reads>before_reads); /* The unchanged cold block still reaches disk. */
    CHECK(ext4_fseek(&file,0,SEEK_SET)==EOK && ext4_fread(&file,readback,block,&count)==EOK && count==block);
    for (size_t j=0;j<block;j++) CHECK(readback[j]==0x40);
    struct file_read_span spans[2]={
        {block+13,readback,17,0,EIO},{2*block+29,readback+32,19,0,EIO}};
    struct file_read_workspace read_work;
    CHECK(ext4_fpread_batch(&file,spans,2,&read_work,UINT64_MAX, NULL)==EOK && spans[0].completed==17 && spans[1].completed==19);
    for (unsigned i=0;i<17;i++) CHECK(readback[i]==0x43);
    for (unsigned i=0;i<19;i++) CHECK(readback[32+i]==0x42);
    CHECK(ext4_journal_group_drain("/")==EIO);
    struct file_write_span rejected={.buffer=readback,.size=1};
    CHECK(ext4_fpwrite_batch(&file,&rejected,1)==EIO && !rejected.written);
    for (unsigned i=0;i<4;i++) CHECK(pages[i].references==1 && pages[i].bytes);
    CHECK(disk.writes==writes && disk.flushes==flushes);
    free(readback);
    CHECK(fault_block_crash(&disk)==0);
    puts("group data error: failed/sealed/running owners retained, cold reads survive sticky write error");
}

static void group_verify_recovery(struct ext4_fs *fs)
{
    ext4_file f;struct ext4_inode ino;
    CHECK(ext4_fopen(&f,"/file","r")==EOK && ext4_fraw_inode_fill(&f,&ino)==EOK);
    bool new_version=to_le32(ino.access_time)==1111;
    CHECK(to_le32(ino.access_time)==(new_version?1111:0));
    CHECK(to_le32(ino.modification_time)==(new_version?2222:0));
    CHECK(to_le32(ino.change_inode_time)==(new_version?3333:0));
    CHECK(!fs->jbd_journal->error && ext4_fclose(&f)==EOK);
}
static struct ext4_fs *space_fs;
static ext4_file space_other;
static unsigned space_waited, space_interleaved;
static int space_wait(void *context, uint64_t sequence, enum ext4_journal_wait kind)
{
    (void)context;
    if (space_fs->jbd_journal->committing || space_fs->jbd_journal->checkpointing) {
        CHECK(kind==EXT4_JOURNAL_WAIT_CHECKPOINT && sequence==space_fs->jbd_journal->accepted_sequence);
        space_waited++;
        /* A host fixture cannot suspend this C stack. Return a retry reason;
         * the RV64 adapter uses the same boundary to sleep on progress. */
        return EAGAIN;
    }
    for(;;) {
        struct ext4_journal_progress progress;
        CHECK(ext4_journal_group_progress("/",&progress)==EOK);
        uint64_t reached=kind==EXT4_JOURNAL_WAIT_SEALED?progress.sealed:
            kind==EXT4_JOURNAL_WAIT_DURABLE?progress.durable:progress.checkpoint;
        if(reached>=sequence)return EOK;
        int r=ext4_journal_group_service("/",true);
        if(r!=EOK)return r;
    }
}
static void space_during_checkpoint(void)
{
    struct jbd_trans *committing=space_fs->jbd_journal->checkpointing;
    if (!committing || LIST_EMPTY(&committing->quarantine)) {
        write_interleave=space_during_checkpoint;
        return;
    }
    unsigned char bytes[4096]={0};size_t count=99;
    CHECK(ext4_fpwrite(&space_other,0,bytes,sizeof(bytes),&count)==EAGAIN && count==0);
    space_interleaved++;
}
static void group_space(struct ext4_fs *fs)
{
    space_fs=fs;
    struct ext4_journal_runtime runtime={.now_ns=group_clock,.wait=space_wait};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file fill;CHECK(ext4_fopen(&space_other,"/space-other","w+")==EOK);
    CHECK(ext4_fopen(&fill,"/space-fill","w+")==EOK);
    unsigned char bytes[4096];memset(bytes,0x72,sizeof(bytes));size_t count;
    for(;;){int r=ext4_fwrite(&fill,bytes,sizeof(bytes),&count);
        if(r==ENOSPC){CHECK(count==0);break;}
        CHECK(r==EOK && count==sizeof(bytes));}
    CHECK(ext4_file_sync_metadata(&fill)==EOK && fill.fsize>65536);
    CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb)<32);
    write_interleave=space_during_checkpoint;
    CHECK(ext4_ftruncate(&fill,fill.fsize-65536, UINT32_MAX)==EOK);
    CHECK(space_waited==1 && space_interleaved==1 && !write_interleave);
    CHECK(ext4_fpwrite(&space_other,0,bytes,sizeof(bytes),&count)==EOK && count==sizeof(bytes));
    CHECK(ext4_file_sync_metadata(&space_other)==EOK);
    CHECK(ext4_fclose(&fill)==EOK && ext4_fclose(&space_other)==EOK);
    printf("group space: pending checkpoint waited; allocation retried successfully\n");
}
static bool cost_clock(struct ext4_timestamp *now)
{ *now=(struct ext4_timestamp){2000000000,++cost_tick};return true; }
static void cost_test(struct ext4_fs *fs)
{
    CHECK(fs->jbd_journal!=NULL);
    ext4_file f;unsigned char bytes[4096];size_t written;
    memset(bytes,0x5a,sizeof(bytes));CHECK(ext4_fopen(&f,"/file","r+")==EOK);
    CHECK(ext4_fwrite(&f,bytes,sizeof(bytes),&written)==EOK && written==sizeof(bytes));
    CHECK(ext4_file_sync_metadata(&f)==EOK);
    CHECK(ext4_mount_setup_clock("/",cost_clock)==EOK);
    for(unsigned mode=0;mode<3;mode++) {
        uint64_t before_writes=disk.writes,before_flushes=disk.flushes;
        uint32_t before_tid=fs->jbd_journal->committed_id;
        for(unsigned n=0;n<128;n++) {
            if(mode!=1)CHECK(ext4_file_touch(&f,EXT4_TIME_MTIME|EXT4_TIME_CTIME)==EOK);
            if(mode!=0){CHECK(ext4_fseek(&f,0,SEEK_SET)==EOK);CHECK(ext4_fwrite(&f,bytes,sizeof(bytes),&written)==EOK && written==sizeof(bytes));}
        }
        struct ext4_inode inode;CHECK(ext4_fraw_inode_fill(&f,&inode)==EOK);
        CHECK(to_le32(inode.modification_time)==2000000000U);
        CHECK((to_le32(inode.mtime_extra)>>2)==cost_tick);
        printf("{\"mode\":\"%s\",\"operations\":128,\"logical_bytes\":%u,\"writes\":%llu,\"flushes\":%llu,\"commits\":%u}\n",
            mode==0?"timestamp-only":mode==1?"backend-only":"timestamp-and-backend",
            mode==0?0:128U*4096U,(unsigned long long)(disk.writes-before_writes),
            (unsigned long long)(disk.flushes-before_flushes),fs->jbd_journal->committed_id-before_tid);
    }
    CHECK(ext4_fseek(&f,0,SEEK_SET)==EOK);
    unsigned char actual[4096];size_t read_count;
    CHECK(ext4_fread(&f,actual,sizeof(actual),&read_count)==EOK && read_count==sizeof(actual));
    CHECK(!memcmp(actual,bytes,sizeof(actual)));CHECK(ext4_fclose(&f)==EOK);
}
static bool namespace_hold;
static void bitmap_test(void)
{
    unsigned char bitmap[16];
    for(unsigned pattern=0;pattern<4;pattern++) {
        for(unsigned i=0;i<sizeof(bitmap);i++)
            bitmap[i]=(unsigned char[]){0xff,0xfe,0x55,0xa5}[pattern];
        bitmap[5]&=~0x80U;
        for(unsigned first=0;first<128;first++)
        for(unsigned end=first;end<=128;end++) {
            unsigned expected=first;
            while(expected<end && (bitmap[expected/8]&(1U<<(expected%8)))) expected++;
            uint32_t actual=UINT32_MAX;
            int r=ext4_bmap_bit_find_clr(bitmap,first,end,&actual);
            CHECK(expected==end ? r==ENOSPC : r==EOK && actual==expected);
        }
    }
    puts("bitmap: aligned/unaligned subranges match independent byte oracle");
}
static unsigned namespace_waits;
static int namespace_wait(void *context, uint64_t sequence, enum ext4_journal_wait kind)
{
    (void)context;
    namespace_waits++;
    if (namespace_hold) return EAGAIN;
    for (;;) {
        struct ext4_journal_progress progress;
        CHECK(ext4_journal_group_progress("/", &progress)==EOK);
        uint64_t reached=kind==EXT4_JOURNAL_WAIT_SEALED?progress.sealed:
            kind==EXT4_JOURNAL_WAIT_DURABLE?progress.durable:progress.checkpoint;
        if (reached>=sequence) return EOK;
        int r=ext4_journal_group_service("/",true);
        if (r!=EOK) return r;
    }
}
/* Ordinary operations must not synchronize unrelated work while version,
 * journal and reuse credits are still available. */
static void group_namespace(struct ext4_fs *fs, bool unlink_files)
{
    struct ext4_journal_runtime runtime={.now_ns=group_clock,.wait=namespace_wait};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    CHECK(ext4_mount_setup_clock("/",cost_clock)==EOK);
    ext4_file f;
    CHECK(ext4_fopen(&f,"/file","r+")==EOK);
    struct ext4_timestamp times[3]={{1111,1},{2222,2},{3333,3}};
    CHECK(ext4_file_set_times(&f,7,times)==EOK);
    unsigned waits=namespace_waits;
    uint64_t writes=disk.writes,flushes=disk.flushes;
    namespace_hold=true;
    if (!unlink_files) {
        CHECK(ext4_ftruncate(&f,f.fsize, UINT32_MAX)==EOK);
        CHECK(ext4_ftruncate(&f,4096, UINT32_MAX)==EOK);
        CHECK(ext4_ftruncate(&f,0, UINT32_MAX)==EOK);
        CHECK(ext4_fclose(&f)==EOK);
        CHECK(ext4_fopen(&f,"/namespace-new","w+")==EOK);
        CHECK(f.fsize==0 && ext4_fclose(&f)==EOK);
    } else {
        CHECK(ext4_fclose(&f)==EOK);
        /* Quarantined holes separated by a live inode must never let a new
         * directory entry alias that live inode. */
        uint32_t holes[3];
        ext4_file live;
        for(unsigned i=0;i<3;i++) {
            char name[64];snprintf(name,sizeof(name),"/namespace-hole-%u",i);
            CHECK(ext4_fopen2(&f,name,O_CREAT|O_RDWR)==EOK);
            holes[i]=f.inode;
            if(i==1)live=f;else CHECK(ext4_fclose(&f)==EOK);
        }
        CHECK(ext4_fremove("/namespace-hole-0")==EOK && ext4_fremove("/namespace-hole-2")==EOK);
        size_t alive_count;
        CHECK(ext4_fwrite(&live,"live",4,&alive_count)==EOK && alive_count==4);
        CHECK(ext4_fopen2(&f,"/namespace-after-holes",O_CREAT|O_RDWR)==EOK);
        for(unsigned i=0;i<3;i++)CHECK(f.inode!=holes[i]);
        CHECK(ext4_fclose(&f)==EOK);
        unsigned char alive[4];
        CHECK(ext4_fpread(&live,0,alive,sizeof(alive),&alive_count)==EOK && alive_count==4 && !memcmp(alive,"live",4));
        CHECK(ext4_fclose(&live)==EOK && ext4_fremove("/namespace-hole-1")==EOK && ext4_fremove("/namespace-after-holes")==EOK);
        uint32_t retired[16];
        unsigned char data[4096];memset(data,0x5a,sizeof(data));
        for(unsigned i=0;i<16;i++) {
            char name[64];snprintf(name,sizeof(name),"/namespace-%u",i);
            CHECK(ext4_fopen2(&f,name,O_CREAT|O_RDWR)==EOK);
            retired[i]=f.inode;
            for(unsigned j=0;j<i;j++) CHECK(f.inode!=retired[j]);
            if(i&1) {size_t count;CHECK(ext4_fwrite(&f,data,sizeof(data),&count)==EOK && count==sizeof(data));}
            CHECK(ext4_fclose(&f)==EOK && ext4_fremove(name)==EOK);
            CHECK(ext4_fopen(&f,name,"r")==ENOENT);
            CHECK(quarantine_blocks_reuse(fs,retired[i],true));
        }
        /* An open unlinked inode retains its content until its last owner. */
        CHECK(ext4_fopen2(&f,"/namespace-live",O_CREAT|O_RDWR)==EOK);
        size_t count;CHECK(ext4_fwrite(&f,data,sizeof(data),&count)==EOK && count==sizeof(data));
        uint32_t inode;bool orphan;
        CHECK(ext4_funlink_dentry("/namespace-live",&inode,&orphan)==EOK && orphan && inode==f.inode);
        unsigned char actual[4096];
        CHECK(ext4_fpwrite(&f,0,data,sizeof(data),&count)==EOK && count==sizeof(data));
        CHECK(ext4_fpread(&f,0,actual,sizeof(actual),&count)==EOK && count==sizeof(actual) && !memcmp(data,actual,sizeof(data)));
        CHECK(ext4_fclose(&f)==EOK && ext4_orphan_free("/",inode)==EOK);
        CHECK(quarantine_blocks_reuse(fs,inode,true));
    }
    CHECK(namespace_waits==waits && disk.writes==writes && disk.flushes==flushes);
    namespace_hold=false;
    CHECK(ext4_journal_group_drain("/")==EOK);
    printf("group namespace: %s accepted without forced I/O; reuse quarantined until checkpoint\n",unlink_files?"unlink":"truncate");
}
static struct ext4_timestamp get_time(const struct ext4_inode *inode,unsigned i,bool extended)
{
    uint32_t low[3] = {inode->access_time,inode->modification_time,inode->change_inode_time};
    uint32_t extra[3] = {inode->atime_extra,inode->mtime_extra,inode->ctime_extra};
    uint32_t high = extended ? to_le32(extra[i]) : 0;
    return (struct ext4_timestamp){(int64_t)(int32_t)to_le32(low[i])+((int64_t)(high&3)<<32),high>>2};
}
static void expect_time(ext4_file *f,unsigned i,struct ext4_timestamp want,bool extended)
{
    int64_t max = extended ? INT64_C(15032385535) : INT32_MAX;
    if (want.seconds <= INT32_MIN) { want.seconds=INT32_MIN;want.nanoseconds=0; }
    if (want.seconds >= max) { want.seconds=max;want.nanoseconds=0; }
    if (!extended) want.nanoseconds=0;
    struct ext4_inode ino;
    CHECK(ext4_fraw_inode_fill(f,&ino)==EOK);
    struct ext4_timestamp got=get_time(&ino,i,extended);
    CHECK(got.seconds==want.seconds && got.nanoseconds==want.nanoseconds);
}
static void seed(void)
{
    ext4_file f;size_t n;unsigned char bytes[4096];memset(bytes,0x5a,sizeof(bytes));
    CHECK(ext4_fopen(&f,"/file","w+")==EOK);CHECK(ext4_fwrite(&f,bytes,sizeof(bytes),&n)==EOK && n==sizeof(bytes));CHECK(ext4_fclose(&f)==EOK);
    CHECK(ext4_fopen(&f,"/other","w+")==EOK);CHECK(ext4_fwrite(&f,bytes,sizeof(bytes),&n)==EOK && n==sizeof(bytes));CHECK(ext4_fclose(&f)==EOK);
}
static struct ext4_fs *independent_fs;
static ext4_file *independent_file;
static struct jbd_trans *independent_checkpoint;
static unsigned independent_fault, independent_handshakes;
static size_t independent_owned;
static const struct ext4_timestamp independent_times[3]={{8123,11},{8456,22},{8789,33}};
static void independent_checkpoint_error_while_commit_held(void)
{
    struct jbd_journal *journal=independent_fs->jbd_journal;
    CHECK(journal->checkpointing==independent_checkpoint && journal->committing);
    unsigned point=independent_fault-14;
    if (!(point&1)) disk.fail_write=disk.writes+1+point/2;
    else disk.fail_flush=disk.flushes+1+point/2;
    CHECK(jbd_journal_checkpoint(journal)==EIO);
    CHECK(jbd_journal_retire_stage(journal,JBD_JOURNAL_CHECKPOINT,EIO)==EIO);
    CHECK(journal->failed_trans==independent_checkpoint && journal->committing);
    independent_owned=jbd_memory_used(journal);
    independent_handshakes++;
}
static void independent_fsync_while_home_held(void)
{
    struct jbd_journal *journal=independent_fs->jbd_journal;
    CHECK(journal->checkpointing==independent_checkpoint && !journal->committing);
    uint64_t checkpoint=journal->checkpoint_sequence, durable=journal->durable_sequence;
    CHECK(ext4_file_set_times(independent_file,7,independent_times)==EOK);
    if (independent_fault>=10) {
        unsigned point=independent_fault-10;
        if (!(point&1)) disk.fail_write=disk.writes+1+point/2;
        else disk.fail_flush=disk.flushes+1+point/2;
    }
    /* The older home write is still on the caller's device stack. A complete
     * fsync must seal, write and durably commit this new target before return. */
    int result=ext4_file_sync_metadata(independent_file);
    if (result!=(independent_fault>=10?EIO:EOK))
        fprintf(stderr,"held checkpoint: later fsync returned %d; durable=%llu checkpoint=%llu target=%llu\n",
            result,(unsigned long long)journal->durable_sequence,
            (unsigned long long)journal->checkpoint_sequence,(unsigned long long)journal->accepted_sequence);
    CHECK(result==(independent_fault>=10?EIO:EOK));
    CHECK(journal->checkpointing==independent_checkpoint && journal->checkpoint_sequence==checkpoint);
    if (independent_fault>=10) {
        CHECK(journal->error==EIO && journal->committing && journal->durable_sequence==durable);
        CHECK(journal->failed_trans==journal->committing && journal->checkpoint_count==1);
    } else {
        CHECK(!journal->committing && journal->durable_sequence>durable);
        CHECK(journal->durable_sequence==journal->accepted_sequence && journal->checkpoint_count==2);
    }
    independent_handshakes++;
    independent_owned=jbd_memory_used(journal);
    if (independent_fault==5) {
        CHECK(fault_block_crash(&disk)==0);
        exit(0);
    }
    if (independent_fault>=6 && independent_fault<=9)
        independent_cut=independent_fault-5;
    if (independent_fault==1) disk.fail_write=disk.writes+1;
    if (independent_fault==2) disk.fail_flush=disk.flushes+1;
    if (independent_fault==3) disk.fail_write=disk.writes+2;
    if (independent_fault==4) disk.fail_flush=disk.flushes+2;
}

static void group_independent_progress(struct ext4_fs *fs,unsigned fault)
{
    CHECK(fault<=17);
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file file; CHECK(ext4_fopen(&file,"/file","r+")==EOK);
    struct ext4_timestamp old[3]={{7123,11},{7456,22},{7789,33}};
    CHECK(ext4_file_set_times(&file,7,old)==EOK && ext4_file_sync_metadata(&file)==EOK);
    struct jbd_journal *journal=fs->jbd_journal;
    independent_checkpoint=TAILQ_FIRST(&journal->cp_queue);
    CHECK(independent_checkpoint && journal->checkpoint_count==1);
    CHECK(TAILQ_FIRST(&independent_checkpoint->buf_queue) &&
        !TAILQ_NEXT(TAILQ_FIRST(&independent_checkpoint->buf_queue),buf_node));
    uint64_t before=journal->checkpoint_sequence;
    independent_fs=fs; independent_file=&file; independent_fault=fault;
    if (fault>=14) {
        /* Reverse the completion order: checkpoint fails while the later
         * commit is in its first device write, then that commit completes all
         * its real I/O. Sticky failure must keep both actual owners. */
        CHECK(jbd_journal_select(journal) && journal->checkpointing==independent_checkpoint);
        uint64_t durable=journal->durable_sequence;
        CHECK(ext4_file_set_times(&file,7,independent_times)==EOK);
        write_interleave=independent_checkpoint_error_while_commit_held;
        CHECK(ext4_file_sync_metadata(&file)==EIO && independent_handshakes==1);
        CHECK(journal->checkpointing==independent_checkpoint && journal->committing &&
            journal->failed_trans==independent_checkpoint && journal->checkpoint_count==1);
        CHECK(journal->durable_sequence==durable && journal->checkpoint_sequence==before &&
            jbd_memory_used(journal)==independent_owned);
        CHECK(ext4_umount("/")==EIO && fs->bdev->fs==fs && fault_block_crash(&disk)==0);
        printf("independent reverse error block=%u fault=%u: checkpoint failed, completed commit owner retained\n",fs->bdev->lg_bsize,fault);
        return;
    }
    write_interleave=independent_fsync_while_home_held;
    int result=ext4_journal_group_checkpoint("/");
    CHECK(independent_handshakes==1 && !write_interleave);
    bool failed=(fault>=1 && fault<=4) || fault>=10;
    CHECK(result==(failed?EIO:EOK));
    if (failed) {
        CHECK(journal->error==EIO && journal->checkpointing==independent_checkpoint);
        CHECK(journal->checkpoint_sequence==before && jbd_memory_used(journal)==independent_owned);
        CHECK(journal->failed_trans==(fault>=10?journal->committing:independent_checkpoint));
        CHECK(ext4_file_sync_metadata(&file)==EIO && ext4_umount("/")==EIO && fs->bdev->fs==fs);
    } else {
        CHECK(journal->checkpoint_sequence>before && !journal->checkpointing);
        CHECK(journal->checkpoint_count==1 && journal->durable_sequence>journal->checkpoint_sequence);
    }
    CHECK(fault_block_crash(&disk)==0);
    printf("independent progress block=%u fault=%u: held home, later fsync, retained stage owners\n",fs->bdev->lg_bsize,fault);
}

static void group_independent_recovery(struct ext4_fs *fs, bool failed_commit)
{
    ext4_file file; struct ext4_inode inode;
    CHECK(ext4_fopen(&file,"/file","r")==EOK && ext4_fraw_inode_fill(&file,&inode)==EOK);
    bool extended=ext4_get16(&fs->sb,inode_size)>128;
    bool latest=get_time(&inode,0,extended).seconds==independent_times[0].seconds;
    CHECK(latest || failed_commit);
    for(unsigned i=0;i<3;i++) {
        struct ext4_timestamp expected=independent_times[i];
        if (!latest) expected.seconds-=1000;
        expect_time(&file,i,expected,extended);
    }
    CHECK(ext4_fclose(&file)==EOK);
}

static void times_test(struct ext4_fs *fs, bool readonly)
{
    bool extended=ext4_get16(&fs->sb,inode_size)>128;
    ext4_file f;CHECK(ext4_fopen(&f,"/file","r")==EOK);
    struct ext4_inode before,after;CHECK(ext4_fraw_inode_fill(&f,&before)==EOK);
    struct ext4_timestamp t[3]={{INT64_C(4294967296),123456789},{-1,987654321},{1700000000,17}};
    uint64_t writes=disk.writes;
    CHECK(ext4_file_set_times(&f,0,NULL)==EOK);
    CHECK(ext4_file_set_times(&f,8,t)==EINVAL);
    t[0].nanoseconds=1000000000;CHECK(ext4_file_set_times(&f,1,t)==EINVAL);t[0].nanoseconds=123456789;
    CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && !memcmp(&before,&after,sizeof(before)));
    CHECK(disk.writes==writes);
    if (readonly) { CHECK(ext4_file_set_times(&f,7,t)==EROFS);CHECK(ext4_file_set_mode(&f,0751)==EROFS);CHECK(disk.writes==writes);CHECK(ext4_fclose(&f)==EOK);return; }
    CHECK(ext4_file_set_times(&f,7,t)==EOK);
    for(unsigned i=0;i<3;i++)expect_time(&f,i,t[i],extended);
    CHECK(fs->jbd_journal->committed_id);
    CHECK(ext4_file_sync_metadata(&f)==EOK);
    t[0]=(struct ext4_timestamp){12345,42};t[1].nanoseconds=UINT32_MAX;t[2]=(struct ext4_timestamp){1700000001,23};
    CHECK(ext4_file_set_times(&f,5,t)==EOK);
    expect_time(&f,0,t[0],extended);expect_time(&f,1,(struct ext4_timestamp){-1,987654321},extended);expect_time(&f,2,t[2],extended);
    static const int64_t seconds[]={INT64_MIN,INT32_MIN,(int64_t)INT32_MIN+1,-1,0,INT32_MAX,INT64_C(2147483648),INT64_C(15032385535),INT64_MAX};
    for(unsigned n=0;n<sizeof(seconds)/sizeof(seconds[0]);n++) {
        for(unsigned i=0;i<3;i++)t[i]=(struct ext4_timestamp){seconds[n],123456789};
        CHECK(ext4_file_set_times(&f,7,t)==EOK);for(unsigned i=0;i<3;i++)expect_time(&f,i,t[i],extended);
    }
    CHECK(ext4_fraw_inode_fill(&f,&before)==EOK);uint32_t tid=fs->jbd_journal->committed_id;
    CHECK(ext4_transaction_begin("/")==EOK);t[0]=(struct ext4_timestamp){9000,7};
    CHECK(ext4_file_set_times(&f,1,t)==EOK);CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED);
    CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && !memcmp(&before,&after,sizeof(before)) && fs->jbd_journal->committed_id==tid);
    CHECK(ext4_transaction_begin("/")==EOK);CHECK(ext4_file_set_times(&f,1,t)==EOK);
    CHECK(ext4_file_set_times(&f,8,t)==EINVAL);CHECK(ext4_transaction_end("/")==EINVAL);
    CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && !memcmp(&before,&after,sizeof(before)));
    uint32_t unlinked;bool orphan;
    CHECK(ext4_funlink_dentry("/file",&unlinked,&orphan)==EOK && orphan && unlinked==f.inode);
    CHECK(ext4_file_set_times(&f,1,t)==EOK);expect_time(&f,0,t[0],extended);
    CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && !ext4_inode_get_links_cnt(&after));
    CHECK(ext4_file_set_mode(&f,0751)==EOK);
    CHECK(ext4_fraw_inode_fill(&f,&after)==EOK &&
          (ext4_inode_get_mode(&fs->sb,&after)&0177777)==0100751 &&
          !ext4_inode_get_links_cnt(&after));
    CHECK(ext4_fclose(&f)==EOK);CHECK(ext4_orphan_free("/",unlinked)==EOK);
}
static void owner_shared_test(struct ext4_fs *fs, bool unlink_shared, bool truncating, bool setting)
{
    uint32_t cap[5]={0x02000000,1,0,0,0}; unsigned char padding[48]={1};size_t length;
    ext4_file first, second;struct ext4_inode initial, other, changed;
    CHECK(ext4_fopen(&first,"/file",truncating?"r+":"r")==EOK);
    if (truncating) CHECK(ext4_file_set_mode(&first,06755)==EOK);
    CHECK(ext4_setxattr("/file","user.padding",12,padding,sizeof(padding))==EOK);
    CHECK(ext4_setxattr("/file","security.capability",19,cap,sizeof(cap))==EOK);
    CHECK(ext4_fraw_inode_fill(&first,&initial)==EOK);
    uint64_t shared=ext4_inode_get_file_acl(&initial,&fs->sb);CHECK(shared);
    CHECK(ext4_fopen(&second,"/owner-other","w+")==EOK);
    CHECK(ext4_transaction_begin("/")==EOK);
    struct ext4_inode_ref ref;struct ext4_block block;
    CHECK(ext4_fs_get_inode_ref(fs,second.inode,&ref)==EOK);
    CHECK(ext4_trans_block_get(fs->bdev,&block,shared)==EOK);
    /* 构造合法的磁盘 EA 共享关系；各阶段由独立 e2fsck 核对格式。 */
    struct { uint32_t magic,refcount,blocks,hash,checksum,reserved[3]; } header;
    memcpy(&header,block.data,sizeof(header));
    CHECK(to_le32(header.refcount)==1);header.refcount=to_le32(2);
    header.checksum=0;memcpy(block.data,&header,sizeof(header));uint64_t number=to_le64(shared);
    uint32_t crc=ext4_crc32c(ext4_sb_get_csum_seed(&fs->sb),&number,sizeof(number));
    header.checksum=to_le32(ext4_crc32c(crc,block.data,ext4_sb_get_block_size(&fs->sb)));
    memcpy(block.data,&header,sizeof(header));
    ext4_trans_set_block_dirty(block.buf);CHECK(ext4_block_set(fs->bdev,&block)==EOK);
    ext4_inode_set_file_acl(ref.inode,&fs->sb,shared);
    ext4_inode_set_blocks_count(&fs->sb,ref.inode,ext4_inode_get_blocks_count(&fs->sb,ref.inode)+ext4_sb_get_block_size(&fs->sb)/512);
    ref.dirty=true;CHECK(ext4_fs_put_inode_ref(&ref)==EOK && ext4_transaction_end("/")==EOK);
    CHECK(ext4_fraw_inode_fill(&second,&other)==EOK);
    if (setting) {
        uint32_t replacement[5]={0x02000000,4,0,0,0},value[5];
        CHECK(ext4_setxattr("/file","security.capability",19,replacement,sizeof(replacement))==EOK);
        CHECK(ext4_getxattr("/file","security.capability",19,value,sizeof(value),&length)==EOK &&
              length==sizeof(value) && !memcmp(value,replacement,sizeof(value)));
        CHECK(ext4_getxattr("/owner-other","security.capability",19,value,sizeof(value),&length)==EOK &&
              length==sizeof(value) && !memcmp(value,cap,sizeof(value)));
        CHECK(ext4_fraw_inode_fill(&first,&changed)==EOK && ext4_inode_get_file_acl(&changed,&fs->sb)!=shared);
        CHECK(ext4_fraw_inode_fill(&second,&changed)==EOK && !memcmp(&changed,&other,sizeof(other)));
        CHECK(ext4_file_sync_metadata(&first)==EOK && ext4_fclose(&first)==EOK && ext4_fclose(&second)==EOK);
        puts("PASS shared EA set: private replacement retains both owners, values and checksums");
        return;
    }
    if(unlink_shared) {
        CHECK(ext4_fclose(&first)==EOK && ext4_fremove("/file")==EOK);
        CHECK(ext4_getxattr("/owner-other","security.capability",19,padding,sizeof(padding),&length)==EOK && length==sizeof(cap) && !memcmp(padding,cap,sizeof(cap)));
        CHECK(ext4_block_get(fs->bdev,&block,shared)==EOK);
        memcpy(&header,block.data,sizeof(header));
        CHECK(to_le32(header.refcount)==1 && ext4_block_set(fs->bdev,&block)==EOK);
        CHECK(ext4_fclose(&second)==EOK && ext4_fremove("/owner-other")==EOK);
        return;
    }
    CHECK(ext4_transaction_begin("/")==EOK && (truncating?ext4_ftruncate(&first,first.fsize, UINT32_MAX):ext4_file_set_owner(&first,70001,80002, UINT32_MAX))==EOK);
    CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED);
    CHECK(ext4_fraw_inode_fill(&first,&changed)==EOK && !memcmp(&changed,&initial,sizeof(initial)));
    CHECK((truncating?ext4_ftruncate(&first,first.fsize, UINT32_MAX):ext4_file_set_owner(&first,70001,80002, UINT32_MAX))==EOK);
    CHECK(ext4_fraw_inode_fill(&first,&changed)==EOK && ext4_inode_get_file_acl(&changed,&fs->sb)!=shared);
    if (truncating) CHECK((ext4_inode_get_mode(&fs->sb,&changed)&07777)==06755);
    uint64_t expected=ext4_inode_get_blocks_count(&fs->sb,&initial);
    if(!ext4_inode_get_file_acl(&changed,&fs->sb))expected-=ext4_sb_get_block_size(&fs->sb)/512;
    CHECK(ext4_inode_get_blocks_count(&fs->sb,&changed)==expected);
    CHECK(ext4_fraw_inode_fill(&second,&changed)==EOK && !memcmp(&changed,&other,sizeof(other)));
    uint32_t value[5];
    CHECK(ext4_getxattr("/file","security.capability",19,value,sizeof(value),&length)==ENODATA);
    CHECK(ext4_getxattr("/owner-other","security.capability",19,value,sizeof(value),&length)==EOK && length==sizeof(cap) && !memcmp(value,cap,sizeof(cap)));
    CHECK(ext4_file_sync_metadata(&first)==EOK && ext4_fclose(&first)==EOK && ext4_fclose(&second)==EOK);
}

static void owner_test(struct ext4_fs *fs, bool readonly)
{
    ext4_file f; struct ext4_inode before, after;
    CHECK(ext4_fopen(&f,"/file","r")==EOK);
    CHECK(ext4_fraw_inode_fill(&f,&before)==EOK);
    uint64_t writes=disk.writes;
    if(readonly) {
        CHECK(ext4_file_set_owner(&f,70001,80002, UINT32_MAX)==EROFS);
        CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && !memcmp(&before,&after,sizeof(before)) && disk.writes==writes);
        CHECK(ext4_fclose(&f)==EOK);return;
    }
    uint32_t cap[5]={0x02000000,1,0,0,0}; size_t length;
    CHECK(ext4_file_set_mode(&f,06755)==EOK);
    CHECK(ext4_setxattr("/file","security.capability",19,cap,sizeof(cap))==EOK);
    uint32_t value[5];
    CHECK(ext4_fraw_inode_fill(&f,&before)==EOK);
    CHECK(ext4_transaction_begin("/")==EOK);
    CHECK(ext4_file_set_owner(&f,70001,80002, UINT32_MAX)==EOK);
    CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED);
    CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && !memcmp(&before,&after,sizeof(before)));
    int restored_capability=ext4_getxattr("/file","security.capability",19,value,sizeof(value),&length);
    if (restored_capability || length!=sizeof(cap) || memcmp(value,cap,sizeof(cap)))
        fprintf(stderr,"capability after abort: error=%d length=%zu value=%x,%x,%x,%x,%x\n",restored_capability,length,value[0],value[1],value[2],value[3],value[4]);
    CHECK(restored_capability==EOK && length==sizeof(cap) && !memcmp(value,cap,sizeof(cap)));
    CHECK(ext4_file_set_owner(&f,70001,80002, UINT32_MAX)==EOK);
    CHECK(ext4_fraw_inode_fill(&f,&after)==EOK);
    CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && ext4_inode_get_uid(&after)==70001 && ext4_inode_get_gid(&after)==80002 && (ext4_inode_get_mode(&fs->sb,&after)&07777)==0755);
    CHECK(ext4_getxattr("/file","security.capability",19,value,sizeof(value),&length)==ENODATA);
    CHECK(ext4_file_set_mode(&f,06640)==EOK);
    CHECK(ext4_file_set_owner(&f,UINT32_MAX,UINT32_MAX, UINT32_MAX)==EOK);
    CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && ext4_inode_get_uid(&after)==70001 && ext4_inode_get_gid(&after)==80002 && (ext4_inode_get_mode(&fs->sb,&after)&07777)==02640);
    CHECK(ext4_file_sync_metadata(&f)==EOK && ext4_fclose(&f)==EOK);
    ext4_file unlinked; uint32_t inode; bool orphan;
    CHECK(ext4_fopen(&unlinked,"/owner-unlinked","w+")==EOK);
    CHECK(ext4_funlink_dentry("/owner-unlinked",&inode,&orphan)==EOK && orphan);
    CHECK(ext4_file_set_owner(&unlinked,123,456, UINT32_MAX)==EOK);
    CHECK(ext4_fraw_inode_fill(&unlinked,&after)==EOK && ext4_inode_get_uid(&after)==123 && ext4_inode_get_gid(&after)==456 && !ext4_inode_get_links_cnt(&after));
    CHECK(ext4_fclose(&unlinked)==EOK && ext4_orphan_free("/",inode)==EOK);
}

static void stats_test(struct ext4_fs *fs,uint64_t expected)
{
    struct ext4_mount_stats s={0},before,after;
    uint64_t writes=disk.writes;
    uint32_t recorded=fs->sb.overhead_clusters;
    fs->sb.overhead_clusters=to_le32(1); /* It is a hint, not the computed geometry. */
    CHECK(ext4_mount_point_stats("/",&s)==EOK);
    fs->sb.overhead_clusters=recorded;
    CHECK(s.overhead_blocks==expected && s.overhead_blocks>0);
    CHECK(s.blocks_count==ext4_sb_get_blocks_cnt(&fs->sb));
    CHECK(s.free_blocks_count==ext4_sb_get_free_blocks_cnt(&fs->sb));
    CHECK(s.reserved_blocks_count==(((uint64_t)ext4_get32(&fs->sb,reserved_blocks_count_hi)<<32)|ext4_get32(&fs->sb,reserved_blocks_count_lo)));
    CHECK(!memcmp(s.uuid,fs->sb.uuid,16));CHECK(s.block_size==ext4_sb_get_block_size(&fs->sb));
    CHECK(s.inodes_count==ext4_get32(&fs->sb,inodes_count) && s.free_inodes_count==ext4_get32(&fs->sb,free_inodes_count));
    CHECK(s.block_group_count==(s.blocks_count-ext4_get32(&fs->sb,first_data_block)-1)/s.blocks_per_group+1);
    CHECK(disk.writes==writes);
    if(fs->read_only)return;
    before=s;ext4_file f;CHECK(ext4_fopen(&f,"/allocated","w+")==EOK);
    CHECK(ext4_mount_point_stats("/",&after)==EOK && after.free_inodes_count+1==before.free_inodes_count);
    CHECK(ext4_ftruncate(&f,UINT64_C(10000000), UINT32_MAX)==EOK);
    CHECK(ext4_mount_point_stats("/",&after)==EOK && after.free_blocks_count==before.free_blocks_count);
    char bytes[4096]={0};size_t count;CHECK(ext4_fseek(&f,65536,SEEK_SET)==EOK);CHECK(ext4_fwrite(&f,bytes,sizeof(bytes),&count)==EOK && count==sizeof(bytes));
    CHECK(ext4_mount_point_stats("/",&after)==EOK && after.free_blocks_count<before.free_blocks_count);
    CHECK(after.overhead_blocks==before.overhead_blocks);
    CHECK(ext4_ftruncate(&f,0, UINT32_MAX)==EOK);CHECK(ext4_mount_point_stats("/",&after)==EOK && after.free_blocks_count==before.free_blocks_count);
    CHECK(ext4_fclose(&f)==EOK);CHECK(ext4_fremove("/allocated")==EOK);
    CHECK(ext4_mount_point_stats("/",&after)==EOK && after.free_blocks_count==before.free_blocks_count && after.free_inodes_count==before.free_inodes_count);
}

static void evict_clean(struct ext4_fs *fs)
{
    for (unsigned i=0;i<32;i++) {
        struct ext4_block b;
        CHECK(ext4_block_get(fs->bdev,&b,ext4_sb_get_blocks_cnt(&fs->sb)-1-i)==EOK);
        CHECK(ext4_block_set(fs->bdev,&b)==EOK);
    }
}

static void unrelated_data(struct ext4_fs *fs)
{
    ext4_file f,other;struct ext4_inode_ref ref;ext4_fsblk_t lba;
    CHECK(ext4_fopen(&f,"/file","r")==EOK);CHECK(ext4_fopen(&other,"/other","r")==EOK);
    CHECK(ext4_fs_get_inode_ref(fs,other.inode,&ref)==EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&ref,0,&lba,false)==EOK && lba);
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    CHECK(ext4_cache_write_back("/",1)==EOK);
    struct ext4_block dirty;CHECK(ext4_block_get(fs->bdev,&dirty,lba)==EOK);
    dirty.data[0]^=1;CHECK(ext4_trans_set_block_dirty(dirty.buf)==EOK);
    forbidden_lba=lba*fs->bdev->lg_bsize/512;forbidden_writes=0;
    struct ext4_timestamp t[3]={{1700000000,1},{1700000001,2},{1700000002,3}};
    CHECK(ext4_file_set_times(&f,7,t)==EOK);
    CHECK(forbidden_writes==0 && ext4_bcache_test_flag(dirty.buf,BC_DIRTY));
    forbidden_lba=UINT64_MAX;
    CHECK(ext4_block_set(fs->bdev,&dirty)==EOK);CHECK(ext4_cache_write_back("/",0)==EOK);
    CHECK(ext4_fclose(&f)==EOK);CHECK(ext4_fclose(&other)==EOK);
}

static void failure_test(struct ext4_fs *fs,const char *mode,uint64_t expected,unsigned point)
{
    ext4_file f;CHECK(ext4_fopen(&f,"/file","r")==EOK);
    struct ext4_inode before,after;CHECK(ext4_fraw_inode_fill(&f,&before)==EOK);
    bool owner_op=strstr(mode,"owner")!=NULL;
    if(owner_op) {
        uint32_t cap[5]={0x02000000,1,0,0,0};
        CHECK(ext4_file_set_mode(&f,06755)==EOK);
        CHECK(ext4_setxattr("/file","security.capability",19,cap,sizeof(cap))==EOK);
        CHECK(ext4_fraw_inode_fill(&f,&before)==EOK);
    }
    uint32_t tid=fs->jbd_journal?fs->jbd_journal->committed_id:0;
    struct ext4_mount_stats untouched,stats;memset(&stats,0xa5,sizeof(stats));untouched=stats;
    struct ext4_timestamp t[3]={{1234,1},{5678,2},{9012,3}};
    bool stats_op=strstr(mode,"stats")!=NULL;
    bool oom=!strncmp(mode,"oom",3);
    if(stats_op)evict_clean(fs);
    if(!strcmp(mode,"read-stats"))fail_read=reads+1;
    allocations=0;if(oom)fail_allocation=point;
    if(!strncmp(mode,"write-",6))disk.fail_write=disk.writes+1;
    if(!strncmp(mode,"flush-",6))disk.fail_flush=disk.flushes+1;
    int r=stats_op?ext4_mount_point_stats("/",&stats):owner_op?ext4_file_set_owner(&f,70001,80002, UINT32_MAX):ext4_file_set_times(&f,7,t);
    unsigned attempts=allocations;fail_allocation=0;fail_read=0;
    disk.fail_write=disk.fail_flush=0;
    if(oom || !strcmp(mode,"read-stats")) {
        if(r!=EOK) {
            CHECK(r==(oom?ENOMEM:EIO));CHECK(!fs->jbd_journal || fs->jbd_journal->error==EOK);
            CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && !memcmp(&before,&after,sizeof(before)) &&
                (!fs->jbd_journal || fs->jbd_journal->committed_id==tid));
            if(stats_op)CHECK(!memcmp(&untouched,&stats,sizeof(stats)));
        } else CHECK(oom && !point);
        if(stats_op)CHECK(ext4_mount_point_stats("/",&stats)==EOK && stats.overhead_blocks==expected);
        else if(owner_op) {
            if(r!=EOK) { uint32_t cap[5];size_t length;CHECK(ext4_getxattr("/file","security.capability",19,cap,sizeof(cap),&length)==EOK && length==sizeof(cap)); }
            CHECK(ext4_file_set_owner(&f,70001,80002, UINT32_MAX)==EOK);
            CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && ext4_inode_get_uid(&after)==70001 && ext4_inode_get_gid(&after)==80002);
        }
        else { CHECK(ext4_file_set_times(&f,7,t)==EOK);expect_time(&f,0,t[0],ext4_get16(&fs->sb,inode_size)>128); }
        CHECK(ext4_fclose(&f)==EOK);
        if(oom)printf("%u\n",attempts);
        return;
    }
    CHECK(r==EIO && fs->jbd_journal->error==EIO);
    CHECK(ext4_file_set_times(&f,7,t)==EIO && fs->jbd_journal->committed_id==tid);
    CHECK(ext4_mount_point_stats("/",&stats)==EIO && !memcmp(&untouched,&stats,sizeof(stats)));
    CHECK(ext4_umount("/")==EIO && fs->bdev->fs==fs);
    CHECK(fault_block_crash(&disk)==0);
}

static void corrupt_stats(struct ext4_fs *fs,const char *kind)
{
    if(!strcmp(kind,"geometry"))ext4_set32(&fs->sb,blocks_per_group,0);
    else if(!strcmp(kind,"free"))ext4_sb_set_free_blocks_cnt(&fs->sb,UINT64_MAX);
    else if(!strcmp(kind,"journal")) {
        struct ext4_inode_ref ref;CHECK(ext4_fs_get_inode_ref(fs,ext4_get32(&fs->sb,journal_inode_number),&ref)==EOK);
        ref.inode->access_time^=1;CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    } else {
        if(!strcmp(kind,"location"))CHECK(ext4_transaction_begin("/")==EOK);
        struct ext4_block_group_ref ref;CHECK(ext4_fs_get_block_group_ref(fs,0,&ref)==EOK);
        if(!strcmp(kind,"location")) {
            ext4_bg_set_block_bitmap(ref.block_group,&fs->sb,ext4_sb_get_blocks_cnt(&fs->sb));
            ref.dirty=true;
        } else ref.block_group->checksum^=1;
        CHECK(ext4_fs_put_block_group_ref(&ref)==EOK);
        if(!strcmp(kind,"location")) {
            CHECK(ext4_transaction_end("/")==EOK);
            CHECK(ext4_cache_flush("/")==EOK);
        }
    }
    struct ext4_mount_stats stats,before;memset(&stats,0xa5,sizeof(stats));before=stats;
    uint64_t writes=disk.writes;
    CHECK(ext4_mount_point_stats("/",&stats)==EUCLEAN);
    if(memcmp(&stats,&before,sizeof(stats)) || disk.writes!=writes || fs->bdev->fs!=fs) fprintf(stderr,"corrupt %s bytes=%d writes=%llu/%llu owner=%d\n",kind,memcmp(&stats,&before,sizeof(stats)),(unsigned long long)disk.writes,(unsigned long long)writes,fs->bdev->fs==fs);
    CHECK(!memcmp(&stats,&before,sizeof(stats)) && disk.writes==writes && fs->bdev->fs==fs);
}
/* A read of a dirty extent must verify its bytes without rewriting its checksum. */
static void extent_lookup_checksums(struct ext4_blockdev *dev)
{
    ext4_file file;
    uint32_t block=ext4_sb_get_block_size(&dev->fs->sb);
    CHECK(ext4_fopen(&file,"/extent-checksum-lookups","w+")==EOK);
    CHECK(ext4_transaction_begin("/")==EOK);
    for (unsigned i=0;i<6;i++) {
        bool changed=false;
        CHECK(ext4_fallocate(&file,EXT4_FALLOC_KEEP_SIZE,(uint64_t)i*2*block,block,&changed, UINT32_MAX)==EOK);
        CHECK(changed);
    }
    struct ext4_inode_ref ref;
    CHECK(ext4_fs_get_inode_ref(dev->fs,file.inode,&ref)==EOK);
    struct ext4_inode_block_range range;
    ext4_fsblk_t physical=0;
#ifdef TEST_RANGE_WORK_COUNTERS
    watched_extent_bytes=block-4;extent_crc_calls=0;
#endif
    for (unsigned i=0;i<32;i++) {
        CHECK(ext4_fs_get_inode_dblk_range(&ref,0,1,false,&range)==EOK);
        CHECK(range.block && range.count==1 && range.unwritten);
        if (!i) physical=range.block;
        CHECK(range.block==physical);
    }
#ifdef TEST_RANGE_WORK_COUNTERS
    printf("extent read checksums: lookups=32 CRC=%u\n",extent_crc_calls);
    CHECK(extent_crc_calls<=32);
    watched_extent_bytes=0;
#endif
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    CHECK(ext4_transaction_end("/")==EOK);
    /* Corrupt only the held cache copy, then restore it before releasing it. */
    CHECK(ext4_fs_get_inode_ref(dev->fs,file.inode,&ref)==EOK);
    const unsigned char *root=(const unsigned char *)ref.inode->blocks;
    uint16_t depth,high;uint32_t low;
    memcpy(&depth,root+6,sizeof(depth));CHECK(to_le16(depth)==1);
    memcpy(&low,root+16,sizeof(low));memcpy(&high,root+20,sizeof(high));
    ext4_fsblk_t leaf=(uint64_t)to_le32(low)|((uint64_t)to_le16(high)<<32);
    struct ext4_block cached;
    CHECK(ext4_trans_block_get(dev,&cached,leaf)==EOK);
    cached.data[block-1]^=1;
    CHECK(ext4_fs_get_inode_dblk_range(&ref,0,1,false,&range)==EUCLEAN);
    cached.data[block-1]^=1;
    CHECK(ext4_fs_get_inode_dblk_range(&ref,0,1,false,&range)==EOK && range.block==physical);
    CHECK(ext4_block_set(dev,&cached)==EOK);
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    CHECK(ext4_fclose(&file)==EOK);
}

static void public_batch_ranges(struct ext4_blockdev *dev)
{
    (void)dev; /* Some host runners build this fixture without work counters. */
    ext4_file file;CHECK(ext4_fopen(&file,"/public-range","w+")==EOK);
    struct owned_file_page pages[32];struct file_write_span writes[32];
    struct file_read_span reads_out[32];unsigned char *outputs[32];
    struct file_read_workspace read_work;
    for(unsigned i=0;i<32;i++) {
        owned_page_init(&pages[i],4096,0x40+i);
        writes[i]=(struct file_write_span){.offset=i*4096,.buffer=pages[i].bytes,.size=4096,
            .source={&pages[i],owned_page_retain,owned_page_release,4096},
            .source_buffer=pages[i].bytes,.source_size=4096};
        outputs[i]=malloc(4096);CHECK(outputs[i]);
        reads_out[i]=(struct file_read_span){i*4096,outputs[i],4096,0,0};
    }
    CHECK(ext4_transaction_begin("/")==EOK);
#ifdef TEST_RANGE_WORK_COUNTERS
    watched_bitmap_bytes=ext4_get32(&dev->fs->sb,blocks_per_group)/8;
    bitmap_crc_calls=0;range_queries=0;
#endif
    CHECK(ext4_fpwrite_batch(&file,writes,32)==EOK);
#ifdef TEST_RANGE_WORK_COUNTERS
    printf("public 32-page write: queries=%u bitmapCRC=%u\n",range_queries,bitmap_crc_calls);
    CHECK(range_queries==1 && bitmap_crc_calls==2);
    watched_bitmap_bytes=0;range_queries=0;
#endif
    CHECK(ext4_fpread_batch(&file,reads_out,32,&read_work,UINT64_MAX, NULL)==EOK);
#ifdef TEST_RANGE_WORK_COUNTERS
    CHECK(range_queries==1);
#endif
    for(unsigned i=0;i<32;i++) {
        CHECK(writes[i].written==4096 && !reads_out[i].error && reads_out[i].completed==4096);
        for(unsigned j=0;j<4096;j++)CHECK(outputs[i][j]==0x40+i);
        free(outputs[i]);
    }
    CHECK(ext4_transaction_end("/")==EOK);
    for(unsigned i=0;i<32;i++) {
        CHECK(pages[i].references==1);
        outputs[i]=malloc(4096);CHECK(outputs[i]);
        reads_out[i]=(struct file_read_span){i*4096,outputs[i],4096,0,0};
    }
    /* A journal retains its inode reference until unmount; raw cache cleanup
     * would invalidate that owner. Cold-read through a complete remount. */
    CHECK(ext4_fclose(&file)==EOK && ext4_umount("/")==EOK);
    CHECK(ext4_mount("metadata","/",false)==EOK && ext4_journal_start("/")==EOK);
    CHECK(ext4_fopen(&file,"/public-range","r+")==EOK);
#ifdef TEST_RANGE_WORK_COUNTERS
    range_queries=0;
#endif
    CHECK(ext4_fpread_batch(&file,reads_out,32,&read_work,UINT64_MAX, NULL)==EOK);
#ifdef TEST_RANGE_WORK_COUNTERS
    CHECK(range_queries==1);
#endif
    for(unsigned i=0;i<32;i++) {
        CHECK(!reads_out[i].error && reads_out[i].completed==4096);
        for(unsigned j=0;j<4096;j++)CHECK(outputs[i][j]==0x40+i);
        free(outputs[i]);owned_page_release(&pages[i]);
    }
    CHECK(ext4_fclose(&file)==EOK && ext4_fremove("/public-range")==EOK);
    puts("public 32-page batch: discrete buffers, one write/read range and two bitmap CRC calls");
}
static void public_batch_boundaries(struct ext4_blockdev *dev)
{
    ext4_file file;CHECK(ext4_fopen(&file,"/batch-boundaries","w+")==EOK);
    unsigned char first[13],second[19],last[17];memset(first,0x31,sizeof(first));
    memset(second,0x52,sizeof(second));memset(last,0x73,sizeof(last));
    struct file_write_span spans[3]={
        {.offset=0,.buffer=first,.size=sizeof(first)},
        {.offset=sizeof(first),.buffer=second,.size=sizeof(second)},
        {.offset=2*dev->lg_bsize,.buffer=last,.size=sizeof(last)}};
    CHECK(ext4_fpwrite_batch(&file,spans,3)==EOK);
    struct ext4_inode_ref ref;CHECK(ext4_fs_get_inode_ref(dev->fs,file.inode,&ref)==EOK);
    struct ext4_inode_block_range range;
    CHECK(ext4_fs_get_inode_dblk_range(&ref,1,1,false,&range)==EOK && !range.block);
    CHECK(ext4_inode_get_blocks_count(&dev->fs->sb,ref.inode)==2*dev->lg_bsize/512);
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    size_t size=2*dev->lg_bsize+sizeof(last),done;unsigned char *out=malloc(size);CHECK(out);
    CHECK(ext4_fpread(&file,0,out,size,&done)==EOK && done==size);
    for(size_t i=0;i<size;i++)CHECK(out[i]==(i<13?0x31:i<32?0x52:i<2*dev->lg_bsize?0:0x73));
    free(out);CHECK(ext4_fclose(&file)==EOK && ext4_fremove("/batch-boundaries")==EOK);
    puts("public batch boundaries: same-block partials preserve bytes; gaps remain unallocated holes");
}
static void direct_range_error(struct ext4_fs *fs)
{
    CHECK(!fs->jbd_journal);
    ext4_file file;CHECK(ext4_fopen(&file,"/direct-range-error","w+")==EOK);
    uint64_t free_before=ext4_sb_get_free_blocks_cnt(&fs->sb);
    struct file_write_span spans[32];
    for(unsigned i=0;i<32;i++) {
        owned_page_init(&direct_pages[i],8192,0x30+i);
        spans[i]=(struct file_write_span){.offset=i*4096,.buffer=direct_pages[i].bytes,.size=4096,
            .source={&direct_pages[i],owned_page_retain,owned_page_release,8192},
            .source_buffer=direct_pages[i].bytes,.source_size=8192};
    }
    /* A copied sixth page flushes five queued borrowed pages while this one
     * allocation window still owns the remaining 27 pages in later spans. */
    spans[5].source=(struct file_data_source){0};spans[5].source_buffer=NULL;spans[5].source_size=0;
    direct_batches=0;direct_fail_batch=1;fs->bdev->bdif->bwrite_batch=direct_write_batch;
    CHECK(ext4_fpwrite_batch(&file,spans,32)==EIO && direct_batches==1 && !file.fsize);
    fs->bdev->bdif->bwrite_batch=NULL;
    struct ext4_inode_ref ref;CHECK(ext4_fs_get_inode_ref(fs,file.inode,&ref)==EOK);
    struct ext4_inode_block_range range;
    CHECK(ext4_fs_get_inode_dblk_range(&ref,0,131072/fs->bdev->lg_bsize,false,&range)==EOK && !range.block && range.count==131072/fs->bdev->lg_bsize);
    CHECK(!ext4_inode_get_blocks_count(&fs->sb,ref.inode));
    CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb)==free_before);
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    for(unsigned i=0;i<32;i++) {CHECK(!spans[i].written && direct_pages[i].references==1);owned_page_release(&direct_pages[i]);}
    CHECK(ext4_fclose(&file)==EOK && ext4_fremove("/direct-range-error")==EOK);
    puts("nonjournal cross-span failure: five queued pages and 27 mapped-ahead pages all released");
}
static void direct_range_full_leaf_error(struct ext4_fs *fs)
{
    CHECK(!fs->jbd_journal);
    ext4_file file;CHECK(ext4_fopen(&file,"/direct-full-leaf-error","w+")==EOK);
    unsigned block=fs->bdev->lg_bsize;
    unsigned char *old=malloc(block);CHECK(old);
    for(unsigned i=0;i<3;i++) {
        size_t written;memset(old,0x91+i,block);
        CHECK(ext4_fpwrite(&file,(uint64_t)i*2*block,old,block,&written)==EOK && written==block);
    }
    struct ext4_inode_ref ref;CHECK(ext4_fs_get_inode_ref(fs,file.inode,&ref)==EOK);
    ext4_fsblk_t previous[3];
    for(unsigned i=0;i<3;i++)CHECK(ext4_fs_get_inode_dblk_idx(&ref,i*2,&previous[i],true)==EOK && previous[i]);
    uint64_t free_before=ext4_sb_get_free_blocks_cnt(&fs->sb);
    uint64_t blocks_before=ext4_inode_get_blocks_count(&fs->sb,ref.inode);
    struct file_write_span spans[32];
    uint64_t start=1024*(uint64_t)block;
    for(unsigned i=0;i<32;i++) {
        owned_page_init(&direct_pages[i],8192,0x30+i);
        spans[i]=(struct file_write_span){.offset=start+i*4096,.buffer=direct_pages[i].bytes,.size=4096,
            .source={&direct_pages[i],owned_page_retain,owned_page_release,8192},
            .source_buffer=direct_pages[i].bytes,.source_size=8192};
    }
    /* Three sparse old extents plus the mapped-ahead run fill the inode leaf.
     * The sixth copied page fails the first batch before consuming the run. */
    spans[5].source=(struct file_data_source){0};spans[5].source_buffer=NULL;spans[5].source_size=0;
    direct_batches=0;direct_fail_batch=1;direct_cleanup_leaf=ref.inode;denied_block_buffers=0;
    fs->bdev->bdif->bwrite_batch=direct_full_leaf_write_batch;
    int r=ext4_fpwrite_batch(&file,spans,32);
    denied_block_buffer_size=0;direct_cleanup_leaf=NULL;fs->bdev->bdif->bwrite_batch=NULL;
    printf("full-leaf failed batch: result=%d denied-new-buffers=%u\n",r,denied_block_buffers);
    CHECK(r==EIO && direct_batches==1 && !denied_block_buffers && file.fsize==5*(uint64_t)block);
    struct ext4_inode_block_range range;
    CHECK(ext4_fs_get_inode_dblk_range(&ref,start/block,131072/block,false,&range)==EOK &&
        !range.block && range.count==131072/block);
    for(unsigned i=0;i<3;i++) {
        ext4_fsblk_t physical;CHECK(ext4_fs_get_inode_dblk_idx(&ref,i*2,&physical,true)==EOK && physical==previous[i]);
        size_t read;CHECK(ext4_fpread(&file,(uint64_t)i*2*block,old,block,&read)==EOK && read==block);
        for(unsigned j=0;j<block;j++)CHECK(old[j]==0x91+i);
    }
    CHECK(ext4_inode_get_blocks_count(&fs->sb,ref.inode)==blocks_before);
    CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb)==free_before);
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    free(old);
    for(unsigned i=0;i<32;i++) {CHECK(!spans[i].written && direct_pages[i].references==1);owned_page_release(&direct_pages[i]);}
    CHECK(ext4_fclose(&file)==EOK && ext4_fremove("/direct-full-leaf-error")==EOK);
    puts("nonjournal full-leaf failure: reverse tail cleanup preserves old sparse extents without growing the tree");
}
static void extent_middle_oom(struct ext4_fs *fs)
{
    CHECK(!fs->jbd_journal);
    unsigned block=fs->bdev->lg_bsize;
    unsigned char *bytes=malloc(block),*bitmap_before=malloc(block);CHECK(bytes && bitmap_before);
    memset(bytes,0x75,block);
    for(unsigned unwritten=0;unwritten<2;unwritten++) {
        ext4_file file;CHECK(ext4_fopen(&file,"/extent-middle-oom","w+")==EOK);
        for(unsigned i=0;i<3;i++) {
            size_t written;CHECK(ext4_fpwrite(&file,(uint64_t)i*2*block,bytes,block,&written)==EOK && written==block);
        }
        struct ext4_inode_ref ref;CHECK(ext4_fs_get_inode_ref(fs,file.inode,&ref)==EOK);
        struct ext4_inode_block_range range;
        CHECK(ext4_fs_get_inode_dblk_range(&ref,1024,32,true,&range)==EOK && range.block && range.count==32);
        ext4_fsblk_t physical=range.block;
        uint16_t header[6];memcpy(header,ref.inode->blocks,sizeof(header));
        CHECK(to_le16(header[0])==0xf30a && to_le16(header[1])==4 && to_le16(header[2])==4 && !header[3]);
        if(unwritten) {
            uint16_t encoded=to_le16(0x8000+32);
            memcpy((unsigned char *)ref.inode->blocks+12+3*12+4,&encoded,sizeof(encoded));ref.dirty=true;
        }
        unsigned char leaf_before[sizeof(ref.inode->blocks)];memcpy(leaf_before,ref.inode->blocks,sizeof(leaf_before));
        uint64_t blocks_before=ext4_inode_get_blocks_count(&fs->sb,ref.inode);
        uint64_t free_before=ext4_sb_get_free_blocks_cnt(&fs->sb);
        struct ext4_block_group_ref bg;struct ext4_block bitmap=EXT4_BLOCK_ZERO();
        CHECK(ext4_fs_get_block_group_ref(fs,ext4_balloc_get_bgid_of_block(&fs->sb,physical),&bg)==EOK);
        CHECK(ext4_block_get(fs->bdev,&bitmap,ext4_bg_get_block_bitmap(bg.block_group,&fs->sb))==EOK);
        memcpy(bitmap_before,bitmap.data,block);
        denied_block_buffers=0;denied_block_buffer_size=block;
        int r=ext4_extent_remove_space(&ref,1028,1028);
        denied_block_buffer_size=0;
        printf("full-leaf middle OOM: unwritten=%u result=%d denied-new-buffers=%u\n",unwritten,r,denied_block_buffers);
        CHECK(r==ENOMEM && denied_block_buffers==1);
        CHECK(!memcmp(leaf_before,ref.inode->blocks,sizeof(leaf_before)));
        CHECK(!memcmp(bitmap_before,bitmap.data,block));
        CHECK(ext4_inode_get_blocks_count(&fs->sb,ref.inode)==blocks_before &&
            ext4_sb_get_free_blocks_cnt(&fs->sb)==free_before && file.fsize==5*(uint64_t)block);
        CHECK(ext4_fs_get_inode_dblk_range(&ref,1024,32,false,&range)==EOK &&
            range.block==physical && range.count==32 && range.unwritten==!!unwritten);
        CHECK(ext4_block_set(fs->bdev,&bitmap)==EOK && ext4_fs_put_block_group_ref(&bg)==EOK);
        CHECK(ext4_extent_remove_space(&ref,1024,1055)==EOK);
        CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
        CHECK(ext4_fclose(&file)==EOK && ext4_fremove("/extent-middle-oom")==EOK);
    }
    free(bytes);free(bitmap_before);
    puts("middle extent removal OOM: initialized/unwritten leaf, mappings and bitmap preserved");
}
static void extent_range(struct ext4_blockdev *dev)
{
    /* This case measures a contiguous fresh run; prior independent cases may
     * still own freed ranges through checkpoint, including immediate journals. */
    CHECK(ext4_cache_flush("/")==EOK);
    if (dev->fs->jbd_journal)
        CHECK(jbd_journal_purge_cp_trans(dev->fs->jbd_journal,true,false)==EOK);
    ext4_file f; CHECK(ext4_fopen(&f,"/range","w+")==EOK);
    CHECK(ext4_transaction_begin("/")==EOK);
    struct ext4_inode_ref ref;
    CHECK(ext4_fs_get_inode_ref(dev->fs,f.inode,&ref)==EOK);
    ext4_fsblk_t physical; uint32_t count;
#ifdef TEST_RANGE_WORK_COUNTERS
    watched_bitmap_bytes=ext4_get32(&dev->fs->sb,blocks_per_group)/8;
    bitmap_crc_calls=0;
#endif
    CHECK(ext4_extent_get_blocks(&ref,4,32,&physical,true,&count)==EOK);
    CHECK(physical && count==32);
#ifdef TEST_RANGE_WORK_COUNTERS
    CHECK(bitmap_crc_calls==2); /* One verification and one update of the bitmap. */
    watched_bitmap_bytes=0;
#endif
    ext4_fsblk_t first=physical;
    struct ext4_inode_block_range range;
    unsigned before=allocations;
    CHECK(ext4_fs_get_inode_dblk_range(&ref,7,64,false,&range)==EOK);
    CHECK(range.block==first+3 && range.count==29 && !range.unwritten && !range.allocated);
    unsigned path_allocations=allocations-before;
    before=allocations;
    CHECK(ext4_fs_get_inode_dblk_range(&ref,7,1,false,&range)==EOK && range.count==1);
    CHECK(allocations-before==path_allocations); /* Range length does not repeat path allocation. */
    CHECK(ext4_fs_get_inode_dblk_range(&ref,0,64,false,&range)==EOK && !range.block && range.count==4);
    CHECK(ext4_fs_get_inode_dblk_range(&ref,36,64,false,&range)==EOK && !range.block && range.count==64);
    CHECK(ext4_fs_get_inode_dblk_range(&ref,0,0,false,&range)==EINVAL);
    CHECK(ext4_fs_get_inode_dblk_range(&ref,EXT_MAX_BLOCKS,1,false,&range)==EFBIG);
    CHECK(ext4_fs_get_inode_dblk_range(&ref,EXT_MAX_BLOCKS-1,2,false,&range)==EOK && !range.block && range.count==1);
    /* Encode an on-disk unwritten extent, rather than treating holes as its
     * proxy: physical storage remains owned while reads must return zero. */
    uint16_t *disk_len=(uint16_t *)((unsigned char *)ref.inode->blocks+16);
    CHECK(to_le16(*disk_len)==32); *disk_len=to_le16(0x8000+32); ref.dirty=true;
    CHECK(ext4_fs_get_inode_dblk_range(&ref,7,64,false,&range)==EOK &&
        range.unwritten && range.block==first+3 && range.count==29 && !range.allocated);
    CHECK(ext4_extent_get_blocks(&ref,7,64,&physical,false,&count)==EOK && !physical && count==29);
    CHECK(ext4_fs_get_inode_dblk_range(&ref,11,5,true,&range)==EOK &&
        range.block==first+7 && range.count==5 && !range.unwritten && !range.allocated);
    CHECK(ext4_fs_get_inode_dblk_range(&ref,4,64,false,&range)==EOK && range.unwritten && range.count==7);
    CHECK(ext4_fs_get_inode_dblk_range(&ref,16,64,false,&range)==EOK && range.unwritten && range.count==20);
    ext4_inode_set_size(ref.inode,36*dev->lg_bsize);ref.dirty=true;
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK && ext4_transaction_end("/")==EOK);
    unsigned char *zeroes=malloc(36*dev->lg_bsize);CHECK(zeroes);size_t done;
    memset(zeroes,0xa5,36*dev->lg_bsize);
    before=allocations;
    CHECK(ext4_fpread(&f,0,zeroes,36*dev->lg_bsize,&done)==EOK && done==36*dev->lg_bsize);
    unsigned read_allocations=allocations-before;
    CHECK(read_allocations<=6);
    for(size_t i=0;i<done;i++) { CHECK(!zeroes[i]); }
    free(zeroes);
    CHECK(ext4_fclose(&f)==EOK && ext4_fremove("/range")==EOK);
    printf("extent range: 32-block allocation, bitmap CRC=2, path allocations=%u, 36-block read allocations=%u, holes/unwritten/conversion boundaries\n",path_allocations,read_allocations);
}
static void legacy_range(struct ext4_blockdev *dev)
{
    ext4_file f;CHECK(ext4_fopen(&f,"/legacy-range","w+")==EOK);
    CHECK(ext4_transaction_begin("/")==EOK);
    struct ext4_inode_ref ref;CHECK(ext4_fs_get_inode_ref(dev->fs,f.inode,&ref)==EOK);
    ext4_inode_clear_flag(ref.inode,EXT4_INODE_FLAG_EXTENTS);
    memset(ref.inode->blocks,0,sizeof(ref.inode->blocks));ref.dirty=true;
    struct ext4_inode_block_range range;
    CHECK(ext4_fs_get_inode_dblk_range(&ref,8,32,true,&range)==EOK && range.allocated && range.count==4);
    ext4_fsblk_t direct=range.block;
    CHECK(ext4_fs_get_inode_dblk_range(&ref,12,32,true,&range)==EOK && range.allocated && range.count==32);
    ext4_fsblk_t indirect=range.block;
    CHECK(ext4_fs_get_inode_dblk_range(&ref,8,32,false,&range)==EOK && range.block==direct && range.count==4);
    CHECK(ext4_fs_get_inode_dblk_range(&ref,12,64,false,&range)==EOK && range.block==indirect && range.count==32);
    ext4_lblk_t boundary=12+dev->lg_bsize/4;
    CHECK(ext4_fs_get_inode_dblk_range(&ref,boundary-2,8,false,&range)==EOK && !range.block && range.count==2);
    CHECK(ext4_fs_get_inode_dblk_range(&ref,boundary-2,8,true,&range)==EOK && range.count==2);
    CHECK(ext4_fs_get_inode_dblk_range(&ref,boundary,6,true,&range)==EOK && range.count==6);
    CHECK(ext4_fs_get_inode_dblk_range(&ref,boundary+3,32,false,&range)==EOK && range.block && range.count==3);
    CHECK(ext4_fs_get_inode_dblk_range(&ref,boundary+6,32,false,&range)==EOK && !range.block && range.count==32);
    ext4_inode_set_size(ref.inode,(uint64_t)(boundary+6)*dev->lg_bsize);ref.dirty=true;
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK && ext4_transaction_end("/")==EOK);
    CHECK(ext4_fclose(&f)==EOK && ext4_fremove("/legacy-range")==EOK);
    puts("legacy ranges: direct/single/double-indirect leaves and holes");
}
/* A contiguous legacy allocation must visit its pointer leaf once, regardless
 * of its length. Reintroducing a block-at-a-time setter repeats this work. */
static void legacy_leaf_batch(struct ext4_blockdev *dev)
{
    uint32_t per_leaf=dev->lg_bsize/4;
    ext4_lblk_t starts[]={12,12+per_leaf,12+per_leaf+(uint64_t)per_leaf*per_leaf};
    for(unsigned depth=1;depth<=3;depth++) {
        ext4_file file;CHECK(ext4_fopen(&file,"/legacy-leaf-batch","w+")==EOK);
        CHECK(ext4_transaction_begin("/")==EOK);
        struct ext4_inode_ref ref;CHECK(ext4_fs_get_inode_ref(dev->fs,file.inode,&ref)==EOK);
        ext4_inode_clear_flag(ref.inode,EXT4_INODE_FLAG_EXTENTS);
        memset(ref.inode->blocks,0,sizeof(ref.inode->blocks));ref.dirty=true;
        struct ext4_inode_block_range range;
        CHECK(ext4_fs_get_inode_dblk_range(&ref,starts[depth-1],1,true,&range)==EOK && range.count==1);
        ext4_fsblk_t first=range.block;
        ext4_fsblk_t leaf=ext4_inode_get_indirect_block(ref.inode,depth-1);
        for(unsigned level=depth;level>1;level--) {
            struct ext4_block parent;
            CHECK(ext4_trans_block_get(dev,&parent,leaf)==EOK);
            leaf=to_le32(((uint32_t *)parent.data)[0]);
            CHECK(ext4_block_set(dev,&parent)==EOK);
        }
#ifdef TEST_RANGE_WORK_COUNTERS
        watched_indirect_leaf=leaf;watched_indirect_gets=0;
#endif
        CHECK(ext4_fs_get_inode_dblk_range(&ref,starts[depth-1]+1,32,true,&range)==EOK &&
            range.allocated && range.block && range.count==32);
        ext4_fsblk_t second=range.block;
#ifdef TEST_RANGE_WORK_COUNTERS
        watched_indirect_leaf=0;
        printf("legacy depth=%u 32-pointer allocation: leaf gets=%u\n",depth,watched_indirect_gets);
        CHECK(watched_indirect_gets==1);
#endif
        CHECK(ext4_fs_get_inode_dblk_range(&ref,starts[depth-1],1,false,&range)==EOK &&
            range.block==first && range.count==1);
        CHECK(ext4_fs_get_inode_dblk_range(&ref,starts[depth-1]+1,64,false,&range)==EOK &&
            range.block==second && range.count==32);
        CHECK(ext4_fs_get_inode_dblk_range(&ref,starts[depth-1]+33,1,false,&range)==EOK && !range.block);
        CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
        CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED);
        CHECK(ext4_fclose(&file)==EOK && ext4_fremove("/legacy-leaf-batch")==EOK);
    }
}
static void legacy_chain_abort(struct ext4_blockdev *dev)
{
    uint32_t per_leaf=dev->lg_bsize/4;
    ext4_lblk_t starts[]={12,12+per_leaf,12+per_leaf+(uint64_t)per_leaf*per_leaf};
    for(unsigned depth=1;depth<=3;depth++) for(unsigned existing=0;existing<2;existing++) {
        if(existing && depth==1)continue;
        ext4_file file;CHECK(ext4_fopen(&file,"/legacy-chain-abort","w+")==EOK);
        CHECK(ext4_transaction_begin("/")==EOK);
        struct ext4_inode_ref ref;CHECK(ext4_fs_get_inode_ref(dev->fs,file.inode,&ref)==EOK);
        ext4_inode_clear_flag(ref.inode,EXT4_INODE_FLAG_EXTENTS);
        memset(ref.inode->blocks,0,sizeof(ref.inode->blocks));ref.dirty=true;
        struct ext4_inode_block_range range;
        if(existing) {
            CHECK(ext4_fs_get_inode_dblk_range(&ref,starts[depth-1],1,true,&range)==EOK && range.count==1);
            ext4_inode_set_size(ref.inode,(uint64_t)(starts[depth-1]+1)*dev->lg_bsize);
        }
        CHECK(ext4_fs_put_inode_ref(&ref)==EOK && ext4_transaction_end("/")==EOK);
        uint64_t free_before=ext4_sb_get_free_blocks_cnt(&dev->fs->sb);
        uint32_t inode_before[15];uint64_t blocks_before;
        CHECK(ext4_fs_get_inode_ref(dev->fs,file.inode,&ref)==EOK);
        memcpy(inode_before,ref.inode->blocks,sizeof(inode_before));
        blocks_before=ext4_inode_get_blocks_count(&dev->fs->sb,ref.inode);
        CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
        ext4_lblk_t target=starts[depth-1]+(existing?per_leaf:0)+per_leaf-2;
        unsigned attempts=0,failures=0;
        for(unsigned point=0;point<=attempts;point++) {
            CHECK(ext4_transaction_begin("/")==EOK);
            CHECK(ext4_fs_get_inode_ref(dev->fs,file.inode,&ref)==EOK);
            unsigned before=allocations;
            if(point)fail_allocation=allocations+point;
            int r=ext4_fs_get_inode_dblk_range(&ref,target,8,true,&range);
            if(!point)attempts=allocations-before;
            fail_allocation=0;
            CHECK(r==EOK || r==ENOMEM);
            if(r==EOK)CHECK(range.allocated && range.block && range.count==2);
            else {failures++;CHECK(!range.block && !range.count);}
            CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
            CHECK(ext4_transaction_abort("/",r?r:ECANCELED)==(r?r:ECANCELED));
            CHECK(ext4_sb_get_free_blocks_cnt(&dev->fs->sb)==free_before);
            CHECK(ext4_fs_get_inode_ref(dev->fs,file.inode,&ref)==EOK);
            CHECK(!memcmp(inode_before,ref.inode->blocks,sizeof(inode_before)));
            CHECK(ext4_inode_get_blocks_count(&dev->fs->sb,ref.inode)==blocks_before);
            CHECK(ext4_fs_get_inode_dblk_range(&ref,target,8,false,&range)==EOK && !range.block);
            CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
        }
        CHECK(failures);
        CHECK(ext4_fclose(&file)==EOK && ext4_fremove("/legacy-chain-abort")==EOK);
        printf("legacy depth=%u existing=%u: %u OOM points restore chain, bitmap and inode counts\n",depth,existing,failures);
    }
}
static void range_abort(struct ext4_blockdev *dev)
{
    ext4_file f;CHECK(ext4_fopen(&f,"/range-abort","w+")==EOK);
    uint64_t free_before=ext4_sb_get_free_blocks_cnt(&dev->fs->sb);
    unsigned failures=0;
    for(unsigned point=0;point<=24;point++) {
        CHECK(ext4_transaction_begin("/")==EOK);
        struct ext4_inode_ref ref;CHECK(ext4_fs_get_inode_ref(dev->fs,f.inode,&ref)==EOK);
        struct ext4_inode_block_range range;
        if(point)fail_allocation=allocations+point;
        int r=ext4_fs_get_inode_dblk_range(&ref,0,32,true,&range);
        fail_allocation=0;
        CHECK(r==EOK || r==ENOMEM);
        if(r==EOK) {
            CHECK(range.allocated && range.count==32);
            CHECK(ext4_sb_get_free_blocks_cnt(&dev->fs->sb)==free_before-32);
        } else failures++;
        int put=ext4_fs_put_inode_ref(&ref);CHECK(put==EOK || put==ENOMEM);
        CHECK(ext4_transaction_abort("/",r?r:ECANCELED)==(r?r:ECANCELED));
        CHECK(ext4_sb_get_free_blocks_cnt(&dev->fs->sb)==free_before);
        CHECK(ext4_fs_get_inode_ref(dev->fs,f.inode,&ref)==EOK);
        CHECK(!ext4_inode_get_blocks_count(&dev->fs->sb,ref.inode));
        CHECK(ext4_fs_get_inode_dblk_range(&ref,0,32,false,&range)==EOK && !range.block && range.count==32);
        CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    }
    CHECK(failures);
    int close_result=ext4_fclose(&f),remove_result=ext4_fremove("/range-abort");
    if(close_result || remove_result)fprintf(stderr,"range abort close=%d remove=%d\n",close_result,remove_result);
    CHECK(close_result==EOK && remove_result==EOK);
    printf("range abort: extent, 32 bitmap bits, inode and free counts restored; %u OOM points\n",failures);
}
static void unwritten_range_abort(struct ext4_blockdev *dev)
{
    ext4_file f;CHECK(ext4_fopen(&f,"/unwritten-abort","w+")==EOK);
    CHECK(ext4_transaction_begin("/")==EOK);
    struct ext4_inode_ref ref;CHECK(ext4_fs_get_inode_ref(dev->fs,f.inode,&ref)==EOK);
    struct ext4_inode_block_range range;
    CHECK(ext4_fs_get_inode_dblk_range(&ref,0,32,true,&range)==EOK && range.count==32);
    uint16_t *disk_len=(uint16_t *)((unsigned char *)ref.inode->blocks+16);
    *disk_len=to_le16(0x8000+32);ref.dirty=true;
    for(unsigned i=0;i<3;i++)CHECK(ext4_fs_get_inode_dblk_range(&ref,64+2*i,1,true,&range)==EOK);
    ext4_inode_set_size(ref.inode,69*dev->lg_bsize);ref.dirty=true;
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK && ext4_transaction_end("/")==EOK);
    uint64_t free_before=ext4_sb_get_free_blocks_cnt(&dev->fs->sb);
    unsigned failures=0;
    for(unsigned point=0;point<=32;point++) {
        CHECK(ext4_transaction_begin("/")==EOK && ext4_fs_get_inode_ref(dev->fs,f.inode,&ref)==EOK);
        if(point)fail_allocation=allocations+point;
        int r=ext4_fs_get_inode_dblk_range(&ref,7,5,true,&range);
        bool injected=point && allocations>=fail_allocation;
        fail_allocation=0;
        CHECK(r==EOK || r==ENOMEM);
        if(injected) {CHECK(r==ENOMEM);failures++;}
        else CHECK(r==EOK && range.count==5 && !range.unwritten);
        int put=ext4_fs_put_inode_ref(&ref);CHECK(put==EOK || put==ENOMEM);
        CHECK(ext4_transaction_abort("/",r?r:ECANCELED)==(r?r:ECANCELED));
        CHECK(ext4_sb_get_free_blocks_cnt(&dev->fs->sb)==free_before);
        CHECK(ext4_fs_get_inode_ref(dev->fs,f.inode,&ref)==EOK);
        CHECK(ext4_fs_get_inode_dblk_range(&ref,0,64,false,&range)==EOK && range.unwritten && range.count==32);
        CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    }
    CHECK(failures);
    CHECK(ext4_fclose(&f)==EOK && ext4_fremove("/unwritten-abort")==EOK);
    printf("unwritten abort: split tree and bitmap restored; %u OOM points\n",failures);
}
static void fragmented_range(struct ext4_blockdev *dev)
{
    ext4_file f;CHECK(ext4_fopen(&f,"/range-fragments","w+")==EOK);
    CHECK(ext4_transaction_begin("/")==EOK);
    struct ext4_inode_ref ref;CHECK(ext4_fs_get_inode_ref(dev->fs,f.inode,&ref)==EOK);
    struct ext4_inode_block_range range;
    CHECK(ext4_fs_get_inode_dblk_range(&ref,0,32,true,&range)==EOK && range.count==32);
    ext4_fsblk_t first=range.block;
    for(unsigned i=32;i>4;i--)CHECK(ext4_fs_release_inode_dblk_idx(&ref,i-1,first+i-1)==EOK);
    for(unsigned i=3;i;i--)CHECK(ext4_fs_release_inode_dblk_idx(&ref,i-1,first+i-1)==EOK);
    ext4_inode_set_size(ref.inode,4*dev->lg_bsize);ref.dirty=true;
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK && ext4_transaction_end("/")==EOK);
    /* The freed gaps become allocator input only after their old owners have
     * checkpointed; allocation in the next transaction must not reuse undo. */
    if (dev->fs->jbd_journal)
        CHECK(jbd_journal_purge_cp_trans(dev->fs->jbd_journal,true,false)==EOK);
    CHECK(ext4_transaction_begin("/")==EOK && ext4_fs_get_inode_ref(dev->fs,f.inode,&ref)==EOK);
    uint64_t free_before=ext4_sb_get_free_blocks_cnt(&dev->fs->sb);
    ext4_fsblk_t block;uint32_t count;
    int result=ext4_balloc_alloc_blocks(&ref,first,8,&block,&count);
    if(result || block!=first || count!=3)fprintf(stderr,"fragment result=%d first=%llu block=%llu count=%u grouped=%d\n",result,(unsigned long long)first,(unsigned long long)block,count,dev->fs->jbd_journal->grouped);
    CHECK(result==EOK && block==first && count==3);
    CHECK(ext4_balloc_free_blocks(&ref,block,count)==EOK);
    CHECK(ext4_balloc_alloc_blocks(&ref,first+3,5,&block,&count)==EOK && block==first+4 && count==5);
    CHECK(ext4_balloc_free_blocks(&ref,block,count)==EOK);
    CHECK(ext4_sb_get_free_blocks_cnt(&dev->fs->sb)==free_before);
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK && ext4_transaction_end("/")==EOK);
    CHECK(ext4_fclose(&f)==EOK && ext4_fremove("/range-fragments")==EOK);
    puts("fragmented allocator: bounded prefix, occupied goal, one owner per run");
}
static int extent_prepend(struct ext4_fs *fs, bool grouped, bool tree)
{
    uint32_t size=fs->bdev->lg_bsize;
    unsigned char *bytes=malloc(2*size);
    CHECK(bytes);
    if(grouped) {
        struct ext4_journal_runtime runtime={.now_ns=group_clock};
        CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    }
    ext4_file pad,file;
    size_t count;
    CHECK(ext4_fopen(&pad,"/prepend-pad","w+")==EOK);
    memset(bytes,'P',size);
    CHECK(ext4_fwrite(&pad,bytes,size,&count)==EOK && count==size);
    CHECK(ext4_fopen(&file,"/prepend-file","w+")==EOK);
    CHECK(ext4_fseek(&file,size,SEEK_SET)==EOK);
    memset(bytes,'R',size);
    CHECK(ext4_fwrite(&file,bytes,size,&count)==EOK && count==size);
    if(tree) {
        /* 五个不连续逻辑区间使 leaf 离开 inode，同时让错误映射撞上活块。 */
        for(unsigned i=1;i<5;i++) {
            CHECK(ext4_fseek(&file,(uint64_t)(2*i+1)*size,SEEK_SET)==EOK);
            CHECK(ext4_fwrite(&file,bytes,size,&count)==EOK && count==size);
        }
    }
    struct ext4_inode_ref ref;
    ext4_fsblk_t left,right;
    CHECK(ext4_fs_get_inode_ref(fs,pad.inode,&ref)==EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&ref,0,&left,false)==EOK);
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    CHECK(ext4_fs_get_inode_ref(fs,file.inode,&ref)==EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&ref,1,&right,false)==EOK);
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    CHECK(left && right==left+1);
    /* 先归还右侧 extent 前的物理块，再补逻辑左洞，确定进入 prepend 合并。 */
    CHECK(ext4_ftruncate(&pad,0, UINT32_MAX)==EOK);
    CHECK(ext4_fclose(&pad)==EOK);
    if(grouped)CHECK(ext4_journal_group_drain("/")==EOK);
    CHECK(ext4_fseek(&file,0,SEEK_SET)==EOK);
    memset(bytes,'L',size);
    CHECK(ext4_fwrite(&file,bytes,size,&count)==EOK && count==size);
    if(grouped)CHECK(ext4_journal_group_drain("/")==EOK);
    CHECK(ext4_fseek(&file,0,SEEK_SET)==EOK);
    CHECK(ext4_fread(&file,bytes,2*size,&count)==EOK && count==2*size);
    int failed=0;
    for(uint32_t i=0;i<2*size;i++) if(bytes[i]!=(i<size?'L':'R')) {
        fprintf(stderr,"prepend content mismatch offset=%u actual=%u expected=%u left=%llu right=%llu\n",
            i,bytes[i],i<size?'L':'R',(unsigned long long)left,(unsigned long long)right);
        failed=1;break;
    }
    if(tree) {
        int result=ext4_ftruncate(&file,0, UINT32_MAX);
        if(result!=EOK) {
            fprintf(stderr,"prepend truncate failed: %d\n",result);
            failed=1;
        }
    }
    CHECK(ext4_fclose(&file)==EOK);
    free(bytes);
    return failed;
}

static void extent_read_failure(struct ext4_blockdev *dev, bool io, unsigned point)
{
    ext4_file f;unsigned char byte=0x5a;size_t written;
    uint32_t block_size=dev->lg_bsize;
    CHECK(ext4_fopen(&f,"/fragmented","w+")==EOK);
    for(unsigned i=0;i<8;i++) {
        CHECK(ext4_fseek(&f,(uint64_t)i*2*block_size,SEEK_SET)==EOK);
        CHECK(ext4_fwrite(&f,&byte,1,&written)==EOK && written==1);
    }
    /* 冷挂载让树块必须重新取得引用，避免预热缓存绕过失败路径。 */
    CHECK(ext4_fclose(&f)==EOK && ext4_umount("/")==EOK);
    CHECK(!live_allocations);
    CHECK(ext4_mount("metadata","/",false)==EOK && ext4_journal_start("/")==EOK);
    CHECK(ext4_fopen(&f,"/fragmented","r")==EOK);
    struct ext4_inode_ref ref;
    CHECK(ext4_fs_get_inode_ref(dev->fs,f.inode,&ref)==EOK);
    unsigned before=allocations;
    if(io)fail_read=reads+1;
    else if(point)fail_allocation=allocations+point;
    ext4_fsblk_t physical;uint32_t count;
    int r=ext4_extent_get_blocks(&ref,0,1,&physical,false,&count);
    unsigned attempts=allocations-before;
    fail_allocation=fail_read=0;
    CHECK(r==(io?EIO:point?ENOMEM:EOK));
    /* 失败不能消费调用者的 inode；同一 owner 随后可读并正常收口。 */
    CHECK(ext4_extent_get_blocks(&ref,0,1,&physical,false,&count)==EOK && physical && count==1);
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    CHECK(ext4_fread(&f,&byte,1,&written)==EOK && written==1 && byte==0x5a);
    CHECK(ext4_fclose(&f)==EOK);
    printf("%u\n",attempts);
}

static void *operation_observer(void *argument)
{
    struct ext4_fs *fs = argument;
    CHECK(ext4_current_trans(fs) == NULL);
    CHECK(ext4_current_operation(fs) == NULL && ext4_fs_super(fs) == &fs->sb);
    CHECK(ext4_transaction_begin("/") == EOK);
    CHECK(ext4_current_trans(fs) != NULL && fs->active_operations == 2);
    CHECK(ext4_fs_super(fs) == &fs->sb &&
        ext4_fs_available_blocks(fs) + 1 == ext4_sb_get_free_blocks_cnt(&fs->sb));
    ext4_file peer;
    CHECK(ext4_fopen2(&peer, "/operation-peer", O_RDWR | O_CREAT | O_EXCL) == EOK);
    CHECK(ext4_fclose(&peer) == EOK);
    CHECK(ext4_transaction_end("/") == EOK);
    CHECK(ext4_current_trans(fs) == NULL && fs->active_operations == 1);
    return NULL;
}

static void operation_context_test(struct ext4_fs *fs)
{
    uint32_t free_inodes = ext4_get32(&fs->sb, free_inodes_count);
    CHECK(ext4_transaction_begin("/") == EOK);
    struct jbd_trans *first = ext4_current_trans(fs);
    CHECK(first != NULL);
    CHECK(ext4_transaction_begin("/") == EOK && ext4_current_trans(fs) == first);
    uint64_t free_blocks = ext4_sb_get_free_blocks_cnt(&fs->sb);
    CHECK(ext4_fs_reserve_blocks(fs, 1) == EOK);
    ext4_fs_account_blocks(fs, -1);
    pthread_t observer;
    CHECK(!pthread_create(&observer, NULL, operation_observer, fs));
    CHECK(!pthread_join(observer, NULL));
    CHECK(ext4_current_trans(fs) == first);
    CHECK(ext4_fs_available_blocks(fs) == free_blocks - 1 &&
        ext4_sb_get_free_blocks_cnt(&fs->sb) == free_blocks && fs->reserved_blocks == 1 && !fs->reserved_inodes);
    CHECK(ext4_transaction_end("/") == EOK && ext4_current_trans(fs) == first);
    CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
    CHECK(ext4_current_trans(fs) == NULL && fs->active_operations == 0 &&
        ext4_sb_get_free_blocks_cnt(&fs->sb) == free_blocks && !fs->reserved_blocks && !fs->reserved_inodes);
    CHECK(ext4_get32(&fs->sb, free_inodes_count) == free_inodes - 1);
    ext4_file peer;
    CHECK(ext4_fopen(&peer, "/operation-peer", "r") == EOK);
    CHECK(ext4_fclose(&peer) == EOK);
    CHECK(ext4_fremove("/operation-peer") == EOK);
    CHECK(ext4_get32(&fs->sb, free_inodes_count) == free_inodes);
    puts("PASS task-owned ext4 operation context and nesting");
}

static void operation_super_delta_test(struct ext4_fs *fs)
{
    CHECK(ext4_transaction_begin("/") == EOK);
    struct ext4_operation *operation = ext4_current_operation(fs);
    uint64_t original_blocks = ext4_sb_get_free_blocks_cnt(&fs->sb);
    uint32_t original_inodes = ext4_get32(&fs->sb, free_inodes_count);
    /* Another accepted operation consumed two blocks and one inode after
     * this caller's snapshot. This caller consumes three more and two. */
    CHECK(ext4_fs_reserve_blocks(fs, 3) == EOK && ext4_fs_reserve_inodes(fs, 2) == EOK);
    ext4_fs_account_blocks(fs, -3);
    ext4_fs_account_inodes(fs, -2);
    ext4_sb_set_free_blocks_cnt(&fs->sb, original_blocks - 2);
    ext4_set32(&fs->sb, free_inodes_count, original_inodes - 1);
    struct ext4_sblock merged;
    CHECK(ext4_operation_prepare_super(operation, &merged) == EOK);
    CHECK(ext4_sb_get_free_blocks_cnt(&merged) == original_blocks - 5);
    CHECK(ext4_get32(&merged, free_inodes_count) == original_inodes - 3);
    CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb) == original_blocks - 2);
    CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
    CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb) == original_blocks - 2 &&
        ext4_get32(&fs->sb, free_inodes_count) == original_inodes - 1 &&
        !fs->reserved_blocks && !fs->reserved_inodes);
    ext4_sb_set_free_blocks_cnt(&fs->sb, original_blocks);
    ext4_set32(&fs->sb, free_inodes_count, original_inodes);
    puts("PASS operation superblock deltas preserve accepted counters and abort isolation");
}

static void *operation_reservation_observer(void *argument)
{
    struct ext4_fs *fs = argument;
    CHECK(ext4_transaction_begin("/") == EOK);
    CHECK(ext4_fs_available_blocks(fs) == 0 && ext4_fs_available_inodes(fs) == 0);
    CHECK(ext4_fs_reserve_blocks(fs, 1) == ENOSPC && ext4_fs_reserve_inodes(fs, 1) == ENOSPC);
    CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
    return NULL;
}

static void operation_reservation_test(struct ext4_fs *fs)
{
    uint64_t blocks = ext4_sb_get_free_blocks_cnt(&fs->sb);
    uint32_t inodes = ext4_get32(&fs->sb, free_inodes_count);
    CHECK(blocks <= UINT32_MAX);
    CHECK(ext4_transaction_begin("/") == EOK);
    CHECK(ext4_fs_reserve_blocks(fs, blocks) == EOK && ext4_fs_reserve_inodes(fs, inodes) == EOK);
    pthread_t observer;
    CHECK(!pthread_create(&observer, NULL, operation_reservation_observer, fs));
    CHECK(!pthread_join(observer, NULL));
    CHECK(fs->reserved_blocks == blocks && fs->reserved_inodes == inodes);
    CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
    CHECK(!fs->reserved_blocks && !fs->reserved_inodes);
    CHECK(ext4_fs_available_blocks(fs) == blocks && ext4_fs_available_inodes(fs) == inodes);
    CHECK(ext4_transaction_begin("/") == EOK);
    CHECK(ext4_fs_reserve_blocks(fs, 1) == EOK && ext4_fs_reserve_inodes(fs, 1) == EOK);
    CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
    CHECK(!fs->reserved_blocks && !fs->reserved_inodes);
    puts("PASS operation reservations exclude other actual task contexts and abort returns only their own capacity");
}

static unsigned handle_return_entered,handle_return_resume;
static _Thread_local bool handle_return_pause;
static void handle_return_written(void *owner,unsigned fields,
    const struct ext4_timestamp times[3],int error)
{
    (void)owner;(void)fields;(void)times;
    CHECK(!error);
    if (!handle_return_pause) return;
    __atomic_store_n(&handle_return_entered,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&handle_return_resume,__ATOMIC_ACQUIRE)) sched_yield();
}
static void *handle_return_first(void *argument)
{
    handle_return_pause=true;
    size_t written;
    CHECK(ext4_fpwrite(argument,0,"A",1,&written)==EOK && written==1);
    return NULL;
}
static void handle_return_test(struct ext4_fs *fs)
{
    const struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file file;
    CHECK(ext4_fopen(&file,"/handle-return","w+")==EOK);
    struct ext4_time_observer observer={.written=handle_return_written};
    CHECK(ext4_mount_setup_time_observer("/",&observer)==EOK);
    handle_return_entered=handle_return_resume=0;
    pthread_t first;
    CHECK(!pthread_create(&first,NULL,handle_return_first,&file));
    while (!__atomic_load_n(&handle_return_entered,__ATOMIC_ACQUIRE)) sched_yield();
    /* A has accepted and released every resource, but has not returned its
     * local handle. B seals that group and completes a later, larger write. */
    CHECK(ext4_journal_group_commit("/",true)==EOK);
    size_t written;
    CHECK(ext4_fpwrite(&file,8192,"B",1,&written)==EOK && written==1);
    uint64_t size=file.fsize,sequence,data;
    CHECK(ext4_file_sync_metadata_prepare(&file,false,&sequence)==EOK &&
          ext4_file_sync_metadata_prepare(&file,true,&data)==EOK);
    CHECK(size==8193 && sequence>1 && data==sequence);
    __atomic_store_n(&handle_return_resume,1,__ATOMIC_RELEASE);
    CHECK(!pthread_join(first,NULL));
    uint64_t later_sequence,later_data;
    CHECK(ext4_file_sync_metadata_prepare(&file,false,&later_sequence)==EOK &&
          ext4_file_sync_metadata_prepare(&file,true,&later_data)==EOK);
    CHECK(file.fsize==size && later_sequence==sequence && later_data==data);
    struct ext4_inode inode;
    CHECK(ext4_fraw_inode_fill(&file,&inode)==EOK && ext4_inode_get_size(&fs->sb,&inode)==size);
    unsigned char byte;size_t read;
    CHECK(ext4_fpread(&file,0,&byte,1,&read)==EOK && read==1 && byte=='A');
    CHECK(ext4_fpread(&file,8192,&byte,1,&read)==EOK && read==1 && byte=='B');
    CHECK(ext4_mount_setup_time_observer("/",NULL)==EOK && ext4_fclose(&file)==EOK);
    CHECK(ext4_fremove("/handle-return")==EOK && ext4_journal_group_drain("/")==EOK);
    puts("PASS delayed handle return: another task's larger accepted EOF and later sync/data goals survive");
}

static void handle_stale_test(struct ext4_fs *fs)
{
    ext4_file first,second;size_t written;
    CHECK(ext4_fopen(&first,"/handle-stale","w+")==EOK);
    CHECK(ext4_fwrite(&first,"old",3,&written)==EOK && written==3);
    CHECK(ext4_fopen(&second,"/handle-stale","r+")==EOK && second.fsize==3);
    CHECK(ext4_ftruncate(&second,1, UINT32_MAX)==EOK && first.fsize==3 && second.fsize==1);
    CHECK(ext4_fpwrite(&first,0,"N",1,&written)==EOK && written==1 && first.fsize==1);
    struct ext4_inode inode;
    CHECK(ext4_fraw_inode_fill(&first,&inode)==EOK && ext4_inode_get_size(&fs->sb,&inode)==1);
    CHECK(ext4_fclose(&first)==EOK && ext4_fclose(&second)==EOK && ext4_fremove("/handle-stale")==EOK);
    puts("PASS independent handles: positioned write refreshes an unchanged stale size after another handle truncates");
}

static void allocation_capacity_test(struct ext4_fs *fs)
{
    if (!fs->jbd_journal) return;
    static const char name[] = "allocation-capacity";
    struct ext4_sblock *sb = &fs->sb;
    uint32_t groups = ext4_block_group_cnt(sb);
    uint32_t origin = ext4_crc32c(EXT4_INODE_ROOT_INDEX, name, sizeof(name) - 1) % groups;
    unsigned descriptor_size = ext4_sb_get_desc_size(sb);
    unsigned char *descriptors = malloc((size_t)groups * descriptor_size);
    CHECK(descriptors);
    uint32_t free_inodes = ext4_fs_available_inodes(fs);
    uint64_t free_blocks = ext4_fs_available_blocks(fs);
    for (uint32_t group = 0; group < groups; group++) {
        struct ext4_block_group_ref ref;
        CHECK(ext4_fs_get_block_group_ref(fs, group, &ref) == EOK);
        memcpy(descriptors + (size_t)group * descriptor_size, ref.block_group, descriptor_size);
        CHECK(ext4_fs_put_block_group_ref(&ref) == EOK);
    }
    for (unsigned scenario = 0; scenario < 6; scenario++) {
        CHECK(ext4_transaction_begin("/") == EOK);
        /* These descriptor-only capacity limits model pressure without filling
         * the image. The outer abort must discard every artificial limit. */
        for (uint32_t group = 0; group < groups; group++) {
            struct ext4_block_group_ref ref;
            CHECK(ext4_fs_get_block_group_ref_write(fs, group, &ref) == EOK);
            if (scenario < 2) {
                ext4_bg_set_used_dirs_count(ref.block_group, sb, group == origin ? 0 : 10);
                if (group == origin && scenario == 0)
                    ext4_bg_set_free_blocks_count(ref.block_group, sb, 1);
                if (group == origin && scenario == 1)
                    ext4_bg_set_free_inodes_count(ref.block_group, sb, 1);
            } else if (scenario == 2) {
                ext4_bg_set_free_blocks_count(ref.block_group, sb, 1);
                ext4_bg_set_free_inodes_count(ref.block_group, sb, 1);
                ext4_bg_set_used_dirs_count(ref.block_group, sb, 1);
            } else if (scenario == 3 || (scenario == 5 && group == 0)) {
                ext4_bg_set_free_inodes_count(ref.block_group, sb, 0);
            } else if (scenario == 4) {
                ext4_bg_set_free_blocks_count(ref.block_group, sb, 0);
            }
            ref.dirty = true;
            CHECK(ext4_fs_put_block_group_ref(&ref) == EOK);
        }
        struct ext4_inode_ref inode;
        int result = ext4_fs_alloc_inode(fs, &inode,
            scenario == 5 ? EXT4_DE_REG_FILE : EXT4_DE_DIR,
            EXT4_INODE_ROOT_INDEX, name, sizeof(name) - 1);
        CHECK(result == (scenario == 3 || scenario == 4 ? ENOSPC : EOK));
        if (result == EOK) {
            uint32_t group = (inode.index - 1) / ext4_get32(sb, inodes_per_group);
            if (scenario < 2) CHECK(group != origin);
            if (scenario == 2) CHECK(group == origin);
            if (scenario == 5) CHECK(group != 0);
            CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
        }
        CHECK(ext4_transaction_abort("/", result ? result : ECANCELED) == (result ? result : ECANCELED));
        CHECK(!fs->reserved_blocks && !fs->reserved_inodes &&
            ext4_fs_available_inodes(fs) == free_inodes && ext4_fs_available_blocks(fs) == free_blocks);
        for (uint32_t group = 0; group < groups; group++) {
            struct ext4_block_group_ref ref;
            CHECK(ext4_fs_get_block_group_ref(fs, group, &ref) == EOK);
            CHECK(!memcmp(descriptors + (size_t)group * descriptor_size, ref.block_group, descriptor_size));
            CHECK(ext4_fs_put_block_group_ref(&ref) == EOK);
        }
    }
    free(descriptors);
    puts("PASS directory capacity floors, scarce-space fallback, ENOSPC, parent-group exhaustion and abort counters");
}

static void allocation_locality_test(struct ext4_fs *fs)
{
    struct ext4_sblock *sb = &fs->sb;
    static const char *names[] = {"metadata-a", "metadata-b", "locality-0", "locality-1"};
    uint32_t groups = ext4_block_group_cnt(sb), parent_groups[4];
    CHECK(groups >= 2);
    for (unsigned i = 0; i < 4; i++) {
        char directory[32], path[48];
        snprintf(directory, sizeof(directory), "/%s", names[i]);
        CHECK(ext4_dir_mk(directory) == EOK);
        ext4_dir parent;
        CHECK(ext4_dir_open(&parent, directory) == EOK);
        parent_groups[i] = (parent.f.inode - 1) / ext4_get32(sb, inodes_per_group);
        CHECK(ext4_dir_close(&parent) == EOK);
        snprintf(path, sizeof(path), "%s/child", directory);
        ext4_file file;
        CHECK(ext4_fopen(&file, path, "w+") == EOK);
        CHECK((file.inode - 1) / ext4_get32(sb, inodes_per_group) == parent_groups[i]);
        CHECK(ext4_fclose(&file) == EOK);
    }
    if (groups == 2) CHECK(parent_groups[0] != parent_groups[1]);
    unsigned distinct = 0;
    for (unsigned i = 0; i < 4; i++) {
        bool seen = false;
        for (unsigned j = 0; j < i; j++) seen |= parent_groups[j] == parent_groups[i];
        distinct += !seen;
    }
    CHECK(distinct >= 2);
    printf("PASS directory group distribution (%u,%u,%u,%u) and parent-local regular inode allocation\n",
        parent_groups[0], parent_groups[1], parent_groups[2], parent_groups[3]);
    allocation_capacity_test(fs);
}

#ifdef TEST_OPERATION_LIFECYCLE
#include "lwext4_operation_lifecycle.h"
#include "lwext4_create_ref.h"
#endif

int main(int argc,char **argv)
{
    int exit_status=0;
    if(argc==2 && !strcmp(argv[1],"bitmap")){bitmap_test();return 0;}
    CHECK(argc>=3);CHECK(fault_block_open(&disk,argv[1])==0);unsigned char scratch[512];
    struct ext4_blockdev_iface iface={.open=dev_open,.close=dev_open,.bread=dev_read,.bwrite=dev_write,.flush=dev_flush,.ph_bsize=512,.ph_bcnt=disk.device.capacity_bytes/512,.ph_bbuf=scratch};
    struct ext4_blockdev dev={.bdif=&iface,.part_size=disk.device.capacity_bytes};
    CHECK(ext4_device_register(&dev,"metadata")==EOK);
    bool readonly=!strcmp(argv[2],"owner-ro") || !strcmp(argv[2],"readonly") || !strcmp(argv[2],"stats-ro") || !strcmp(argv[2],"read-stats");
    CHECK(ext4_mount("metadata","/",readonly)==EOK);
    if(ext4_sb_feature_com(&dev.fs->sb,EXT4_FCOM_HAS_JOURNAL)) {
        CHECK(ext4_recover("/")==EOK);CHECK(ext4_journal_start("/")==EOK);CHECK(ext4_orphan_recover("/")==EOK);
    }
    if(!strcmp(argv[2],"extent-prepend") || !strcmp(argv[2],"extent-prepend-group") ||
       !strcmp(argv[2],"extent-prepend-tree") || !strcmp(argv[2],"extent-prepend-tree-group"))
        exit_status=extent_prepend(dev.fs,strstr(argv[2],"group")!=NULL,strstr(argv[2],"tree")!=NULL);
    else if(!strcmp(argv[2],"extent-read-oom") || !strcmp(argv[2],"extent-read-error"))
        extent_read_failure(&dev,!strcmp(argv[2],"extent-read-error"),argc>3?strtoul(argv[3],NULL,10):0);
    else if(!strcmp(argv[2],"extent-lookup")){extent_lookup_checksums(&dev);}
    else if(!strcmp(argv[2],"extent-range")){public_batch_ranges(&dev);public_batch_boundaries(&dev);extent_range(&dev);legacy_range(&dev);legacy_leaf_batch(&dev);legacy_chain_abort(&dev);range_abort(&dev);unwritten_range_abort(&dev);fragmented_range(&dev);}
    else if(!strcmp(argv[2],"seed"))seed();
    else if(!strcmp(argv[2],"operation-context"))operation_context_test(dev.fs);
#ifdef TEST_OPERATION_LIFECYCLE
    else if(!strcmp(argv[2],"operation-lifecycle"))operation_lifecycle_test(dev.fs);
    else if(!strcmp(argv[2],"create-ref"))create_ref_test(dev.fs);
    else if(!strcmp(argv[2],"unlink-ref"))unlink_ref_test(dev.fs);
    else if(!strcmp(argv[2],"operation-state-error")) {
        operation_state_error(dev.fs,argc>3 && atoi(argv[3]));return 0;
    }
#endif
    else if(!strcmp(argv[2],"operation-super-delta")){operation_super_delta_test(dev.fs); operation_reservation_test(dev.fs);}
    else if(!strcmp(argv[2],"handle-return"))handle_return_test(dev.fs);
    else if(!strcmp(argv[2],"handle-stale"))handle_stale_test(dev.fs);
    else if(!strcmp(argv[2],"allocation-locality"))allocation_locality_test(dev.fs);
    else if(!strcmp(argv[2],"cost"))cost_test(dev.fs);
    else if(!strcmp(argv[2],"group"))group_test(dev.fs);
    else if(!strcmp(argv[2],"group-log-payload"))group_log_payload(dev.fs);
    else if(!strcmp(argv[2],"group-independent")) {group_independent_progress(dev.fs,argc>3?strtoul(argv[3],NULL,10):0);return 0;}
    else if(!strcmp(argv[2],"group-independent-recovery"))group_independent_recovery(dev.fs,argc>3 && atoi(argv[3])>=10 && atoi(argv[3])<=13);
    else if(!strcmp(argv[2],"group-checkpoint-collapse")) {group_checkpoint_collapse(dev.fs,argc>3?strtoul(argv[3],NULL,10):0);return 0;}
    else if(!strcmp(argv[2],"group-checkpoint-recovery"))group_checkpoint_recovery(dev.fs);
    else if(!strcmp(argv[2],"group-checkpoint-batching"))group_checkpoint_batching(dev.fs);
    else if(!strcmp(argv[2],"group-checkpoint-target"))group_checkpoint_target(dev.fs);
    else if(!strcmp(argv[2],"group-checkpoint-pressure"))group_checkpoint_pressure(dev.fs);
    else if(!strcmp(argv[2],"group-quarantine") || !strcmp(argv[2],"group-quarantine-error")) {bool failed=!strcmp(argv[2],"group-quarantine-error");group_quarantine(dev.fs,failed);if(failed)return 0;}
    else if(!strcmp(argv[2],"group-namespace-truncate"))group_namespace(dev.fs,false);
    else if(!strcmp(argv[2],"group-namespace-unlink"))group_namespace(dev.fs,true);
    else if(!strcmp(argv[2],"group-space"))group_space(dev.fs);
    else if(!strcmp(argv[2],"group-ring"))group_ring(dev.fs);
    else if(!strcmp(argv[2],"group-bounds")) group_bounds(dev.fs);
    else if(!strcmp(argv[2],"group-barrier")) {group_barrier(dev.fs,argc>3?strtoul(argv[3],NULL,10):0,argc>4?strtoul(argv[4],NULL,10):0);return 0;}
    else if(!strcmp(argv[2],"group-barrier-recovery")) group_barrier_recovery(dev.fs);
    else if(!strcmp(argv[2],"group-pipeline") || !strcmp(argv[2],"group-commit-crash")) {group_pipeline(dev.fs,!strcmp(argv[2],"group-commit-crash"));if(!strcmp(argv[2],"group-commit-crash"))return 0;}
    else if(!strcmp(argv[2],"group-write") || !strcmp(argv[2],"group-flush")) {CHECK(argc==4);group_fault(dev.fs,argv[2],strtoul(argv[3],NULL,10));return 0;}
    else if(!strcmp(argv[2],"group-recovery"))group_verify_recovery(dev.fs);
    else if(!strcmp(argv[2],"direct-range-error"))direct_range_error(dev.fs);
    else if(!strcmp(argv[2],"direct-full-leaf-error"))direct_range_full_leaf_error(dev.fs);
    else if(!strcmp(argv[2],"extent-middle-oom"))extent_middle_oom(dev.fs);
    else if(!strcmp(argv[2],"direct-data-batch") || !strcmp(argv[2],"direct-data-error"))
        direct_data_batch(dev.fs,!strcmp(argv[2],"direct-data-error"));
    else if(!strcmp(argv[2],"group-partial-owner") || !strcmp(argv[2],"group-partial-error")) {
        bool failed=!strcmp(argv[2],"group-partial-error");group_partial_owner(dev.fs,failed);if(failed)return 0;
    }
    else if(!strcmp(argv[2],"group-data-owner"))group_data_owner(dev.fs);
    else if(!strcmp(argv[2],"group-data-batch"))group_data_batch(dev.fs);
    else if(!strcmp(argv[2],"group-borrow-admission"))group_borrow_admission(dev.fs);
    else if(!strcmp(argv[2],"group-source-budget") || !strcmp(argv[2],"group-source-error")) {
        bool failed=!strcmp(argv[2],"group-source-error");group_source_budget(dev.fs,failed);if(failed)return 0;
    }
    else if(!strcmp(argv[2],"group-data-write-error") || !strcmp(argv[2],"group-data-flush-error")) {
        group_data_error(dev.fs,!strcmp(argv[2],"group-data-flush-error"));return 0;
    }
    else if(!strcmp(argv[2],"owner-shared") || !strcmp(argv[2],"owner-shared-unlink") || !strcmp(argv[2],"truncate-shared") || !strcmp(argv[2],"owner-shared-set"))
        owner_shared_test(dev.fs,!strcmp(argv[2],"owner-shared-unlink"),!strcmp(argv[2],"truncate-shared"),!strcmp(argv[2],"owner-shared-set"));
    else if(!strcmp(argv[2],"verify-truncate-shared")) {
        ext4_file f;struct ext4_inode ino;uint32_t value[5];size_t length;
        CHECK(ext4_fopen(&f,"/file","r")==EOK && ext4_fraw_inode_fill(&f,&ino)==EOK);
        CHECK((ext4_inode_get_mode(&dev.fs->sb,&ino)&07777)==06755);
        CHECK(ext4_getxattr("/file","security.capability",19,value,sizeof(value),&length)==ENODATA);
        CHECK(ext4_getxattr("/owner-other","security.capability",19,value,sizeof(value),&length)==EOK && length==sizeof(value));
        CHECK(ext4_fclose(&f)==EOK);
    }
    else if(!strcmp(argv[2],"owner") || !strcmp(argv[2],"owner-ro"))owner_test(dev.fs,readonly);
    else if(!strcmp(argv[2],"verify-owner")) {
        ext4_file f;struct ext4_inode ino;
        CHECK(ext4_fopen(&f,"/file","r")==EOK && ext4_fraw_inode_fill(&f,&ino)==EOK);
        CHECK(ext4_inode_get_uid(&ino)==70001 && ext4_inode_get_gid(&ino)==80002 && (ext4_inode_get_mode(&dev.fs->sb,&ino)&07777)==02640);
        CHECK(ext4_fclose(&f)==EOK);
    }
    else if(!strcmp(argv[2],"verify-owner-error")) {
        ext4_file f;struct ext4_inode ino;uint32_t cap[5];size_t length;
        CHECK(ext4_fopen(&f,"/file","r")==EOK && ext4_fraw_inode_fill(&f,&ino)==EOK);
        bool committed=ext4_inode_get_uid(&ino)==70001;
        CHECK(ext4_inode_get_uid(&ino)==(committed?70001:0) && ext4_inode_get_gid(&ino)==(committed?80002:0));
        CHECK((ext4_inode_get_mode(&dev.fs->sb,&ino)&07777)==(committed?0755:06755));
        int r=ext4_getxattr("/file","security.capability",19,cap,sizeof(cap),&length);
        CHECK(committed?r==ENODATA:r==EOK && length==sizeof(cap));
        CHECK(ext4_fclose(&f)==EOK);
    }
    else if(!strcmp(argv[2],"times") || !strcmp(argv[2],"readonly"))times_test(dev.fs,readonly);
    else if(!strcmp(argv[2],"stats") || !strcmp(argv[2],"stats-ro")){CHECK(argc>3);stats_test(dev.fs,strtoull(argv[3],NULL,10));}
    else if(!strcmp(argv[2],"unrelated"))unrelated_data(dev.fs);
    else if(!strcmp(argv[2],"corrupt")){CHECK(argc>3);corrupt_stats(dev.fs,argv[3]);return 0;}
    else if(!strcmp(argv[2],"verify-times")) {
        ext4_file f;struct ext4_inode ino;CHECK(ext4_fopen(&f,"/file","r")==EOK);CHECK(ext4_fraw_inode_fill(&f,&ino)==EOK);
        bool extended=ext4_get16(&dev.fs->sb,inode_size)>128;
        bool committed=get_time(&ino,0,extended).seconds==1234;
        struct ext4_timestamp t[3]={{1234,1},{5678,2},{9012,3}};
        for(unsigned i=0;i<3;i++)expect_time(&f,i,committed?t[i]:(struct ext4_timestamp){0,0},extended);
        CHECK(ext4_fclose(&f)==EOK);
    }
    else if(strstr(argv[2],"times") || strstr(argv[2],"stats") || strstr(argv[2],"owner")) {
        CHECK(argc>3);failure_test(dev.fs,argv[2],strtoull(argv[3],NULL,10),argc>4?strtoul(argv[4],NULL,10):0);
        if(!strncmp(argv[2],"write-",6) || !strncmp(argv[2],"flush-",6))return 0;
    }
    else CHECK(!"unknown mode");
    CHECK(ext4_umount("/")==EOK);
    if(!strncmp(argv[2],"extent-read-",12) || !strncmp(argv[2],"extent-prepend",14))CHECK(!live_allocations);
    fault_block_close(&disk);return exit_status;
}
