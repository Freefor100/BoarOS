/* Live-inode explicit timestamps and actual ext4 allocation statistics. */
#include "block_fault.h"
#include <ext4.h>
#include <ext4_fs.h>
#include <ext4_extent.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <ext4_block_group.h>
#include <ext4_bitmap.h>
#include <ext4_journal.h>
#include <ext4_trans.h>
#include <ext4_crc32.h>
#include <ext4_blockdev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct fault_block disk;
static unsigned allocations, fail_allocation, reads, fail_read, live_allocations;
static size_t payload_threshold;
static unsigned payload_allocations;
static uint64_t forbidden_lba = UINT64_MAX, forbidden_writes;
static void (*write_interleave)(void);
static const unsigned char *borrow_watch;
static uint64_t borrow_watch_lba;
static unsigned borrow_watch_size, borrow_watch_calls;
static bool borrow_watch_escape;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%d: %s (alloc=%u fail=%u)\n",__LINE__,#x,allocations,fail_allocation); exit(1); } } while (0)
void *ext4_user_malloc(size_t n) { if(payload_threshold && n>=payload_threshold)payload_allocations++; if(++allocations==fail_allocation)return NULL;void *p=malloc(n);if(p)live_allocations++;return p; }
void *ext4_user_calloc(size_t n,size_t s) { if(payload_threshold && n*s>=payload_threshold)payload_allocations++; if(++allocations==fail_allocation)return NULL;void *p=calloc(n,s);if(p)live_allocations++;return p; }
void *ext4_user_realloc(void *p,size_t n) { if(++allocations==fail_allocation)return NULL;void *next=realloc(p,n);if(next&&!p)live_allocations++;return next; }
void ext4_user_free(void *p) { if(p){CHECK(live_allocations);live_allocations--;}free(p); }
static int dev_open(struct ext4_blockdev *b) { (void)b; return EOK; }
static int dev_read(struct ext4_blockdev *b,void *p,uint64_t n,uint32_t c)
{ (void)b; if (++reads == fail_read) return EIO; return kernel_block_read_at(&disk.device,n*512,p,(size_t)c*512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
static int dev_write(struct ext4_blockdev *b,const void *p,uint64_t n,uint32_t c)
{
    (void)b;
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
    return kernel_block_write_at(&disk.device,n*512,p,(size_t)c*512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO;
}
static int dev_flush(struct ext4_blockdev *b)
{ (void)b; return kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
static uint32_t cost_tick;
extern int ext4_journal_group_enable(const char *, const struct ext4_journal_runtime *, size_t) __attribute__((weak));
extern int ext4_journal_group_service(const char *, bool) __attribute__((weak));
extern int ext4_journal_group_drain(const char *) __attribute__((weak));
extern int ext4_file_sync_metadata_mode(ext4_file *, bool) __attribute__((weak));
static uint64_t group_now;
static uint64_t group_clock(void *context) { (void)context; return group_now; }
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
    CHECK(ext4_ftruncate(&f,0)==EOK && ext4_journal_group_drain("/")==EOK);
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
    CHECK(group_now==0 && fs->jbd_journal->memory_peak<=fs->jbd_journal->memory_limit);
    /* Data remains governed by actual retained memory. Exceed that budget
     * at the same clock and force reclaim without a per-256-KiB commit. */
    for (unsigned i=96;i<1536;i++) {
        memset(bytes,(int)i,sizeof(bytes));
        CHECK(ext4_fwrite(&f,bytes,sizeof(bytes),&count)==EOK && count==sizeof(bytes));
    }
    CHECK(ext4_journal_group_progress("/",&large)==EOK && large.sealed>small.sealed);
    CHECK(group_now==0 && fs->jbd_journal->memory_peak<=fs->jbd_journal->memory_limit);
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
    CHECK(fs->jbd_journal->memory_peak<=fs->jbd_journal->memory_limit);
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
    struct ext4_journal_runtime runtime={.now_ns=group_clock};
    CHECK(ext4_journal_group_enable("/",&runtime,4*1024*1024)==EOK);
    ext4_file f; CHECK(ext4_fopen(&f,"/file","r+")==EOK);
    uint64_t writes=disk.writes, flushes=disk.flushes;
    uint32_t tid=fs->jbd_journal->committed_id;
    struct ext4_timestamp times[3]={{1111,1},{2222,2},{3333,3}};
    payload_threshold=fs->bdev->lg_bsize;
    for(unsigned i=0;i<4;i++)CHECK(ext4_file_set_times(&f,7,times)==EOK);
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
        CHECK(fs->jbd_journal->memory_used<=fs->jbd_journal->memory_limit);
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
    CHECK(ext4_journal_group_service("/",false)==EOK);
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
        struct jbd_trans *operation = jbd_journal_new_trans(journal);
        struct ext4_block block = EXT4_BLOCK_ZERO();
        CHECK(operation && ext4_block_get(fs->bdev, &block, home) == EOK);
        CHECK(jbd_trans_get_write_access(operation, &block) == EOK);
        for (unsigned i = 0; i < fs->bdev->lg_bsize; i++) block.data[i] = (unsigned char)(i * 13 + escaped);
        uint32_t first = to_be32(escaped ? JBD_MAGIC_NUMBER : 0x12345678);
        memcpy(block.data, &first, sizeof(first));
        CHECK(jbd_trans_set_block_dirty(operation, &block) == EOK && ext4_block_set(fs->bdev, &block) == EOK);
        CHECK(jbd_journal_accept(journal, operation, 0) == EOK);
        unsigned before_allocations = allocations;
        fail_allocation = allocations + 1;
        CHECK(jbd_journal_freeze(journal) == EOK && allocations == before_allocations);
        struct jbd_buf *version = TAILQ_FIRST(&operation->buf_queue);
        CHECK(version && version->after && !TAILQ_NEXT(version, buf_node));
        borrow_watch = version->after->data; borrow_watch_size = fs->bdev->lg_bsize;
        borrow_watch_lba = journal->log_map[version->jbd_lba] * borrow_watch_size / 512;
        borrow_watch_calls = 0; borrow_watch_escape = escaped;
        CHECK(jbd_journal_select(journal) && journal->committing == operation);
        CHECK(jbd_journal_submit(journal) == EOK && borrow_watch_calls == (escaped ? 2U : 1U));
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
    struct jbd_trans *operation = jbd_journal_new_trans(journal);
    CHECK(operation);
    for (unsigned i = 0; i < 128; i++) {
        struct ext4_block block = EXT4_BLOCK_ZERO();
        CHECK(ext4_block_get(fs->bdev, &block, homes[i]) == EOK);
        CHECK(jbd_trans_get_write_access(operation, &block) == EOK);
        block.data[9] = (unsigned char)i;
        CHECK(jbd_trans_set_block_dirty(operation, &block) == EOK && ext4_block_set(fs->bdev, &block) == EOK);
    }
    CHECK(journal->memory_used - journal->memory_cached <= 128 * (3 * fs->bdev->lg_bsize + 1024) + 65536);
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
static struct owned_file_page direct_pages[32];
static unsigned direct_batches, direct_fail_batch;
static int direct_write_batch(struct ext4_blockdev *bdev,const struct ext4_block_span *spans,unsigned count)
{
    bool payload = false;
    size_t bytes=0;
    for (unsigned i=0;i<count;i++) {
        uintptr_t p=(uintptr_t)spans[i].data;
        bool matched=false;
        for (unsigned j=0;j<32;j++) {
            uintptr_t base=(uintptr_t)direct_pages[j].bytes;
            if (base && p>=base && p-base<8192) {
                CHECK((size_t)spans[i].count*512<=8192-(p-base));
                matched=true; break;
            }
        }
        CHECK(matched); payload=true; bytes+=(size_t)spans[i].count*512;
    }
    CHECK(payload && bytes<=131072);
    if (++direct_batches==direct_fail_batch) return EIO;
    for (unsigned i=0;i<count;i++) {
        int r=dev_write(bdev,spans[i].data,spans[i].block,spans[i].count);
        if (r) return r;
    }
    return EOK;
}
static void direct_data_batch(struct ext4_fs *fs,bool fail)
{
    CHECK(!fs->jbd_journal);
    ext4_file file; CHECK(ext4_fopen(&file,"/file","r+")==EOK);
    struct ext4_inode_ref inode; ext4_fsblk_t physical;
    CHECK(ext4_fs_get_inode_ref(fs,file.inode,&inode)==EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&inode,0,&physical,true)==EOK && physical);
    CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
    struct ext4_block held=EXT4_BLOCK_ZERO(); CHECK(ext4_block_get(fs->bdev,&held,physical)==EOK);
    struct ext4_file_write_span spans[32];
    for (unsigned i=0;i<32;i++) {
        owned_page_init(&direct_pages[i],8192,0x60+i);
        spans[i]=(struct ext4_file_write_span){.offset=i*8192,.buffer=direct_pages[i].bytes,.size=8192,
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
    struct ext4_file_write_span span={.offset=17,.buffer=old.bytes+17,.size=13,
        .source={&old,owned_page_retain,owned_page_release,block},
        .source_buffer=old.bytes,.source_size=block};
    unsigned before_reads=reads;
    CHECK(ext4_fpwrite_batch(&file,&span,1)==EOK && span.written==13);
    CHECK(old.references==2 && old.retains==1 && reads==before_reads);
    CHECK(jbd_journal_read_data(fs,physical)==old.bytes && file.fsize==4096);
    owned_page_init(&replacement,block,0x5a); memset(replacement.bytes+17,0xb9,13);
    span.buffer=replacement.bytes+17; span.source.context=&replacement; span.source_buffer=replacement.bytes;
    CHECK(ext4_transaction_begin("/")==EOK && ext4_fpwrite_batch(&file,&span,1)==EOK);
    CHECK(replacement.references==2 && jbd_journal_read_data(fs,physical)==replacement.bytes);
    CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED && replacement.references==1);
    CHECK(jbd_journal_read_data(fs,physical)==old.bytes && old.references==2);
    CHECK(ext4_fpwrite_batch(&file,&span,1)==EOK && replacement.references==2 && old.references==1);
    owned_page_release(&old);
    /* The full source block covers hole zeros and EOF padding, but only one
     * logical byte grows the file. Bytes after it must never become visible. */
    size_t end=8*block+18;
    owned_page_init(&tail,block,0); tail.bytes[17]=0xdc;
    span=(struct ext4_file_write_span){.offset=end-1,.buffer=tail.bytes+17,.size=1,
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
    struct ext4_file_write_span spans[8];
    for (unsigned i=0;i<8;i++) {
        owned_page_init(&pages[i],block,0x20+i);
        spans[i]=(struct ext4_file_write_span){.offset=i*block,.buffer=pages[i].bytes,.size=block,
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
    struct ext4_file_write_span replace={.offset=0,.buffer=replacement.bytes,.size=block,
        .source={&replacement,owned_page_retain,owned_page_release,block}};
    CHECK(ext4_fpwrite_batch(&file,&replace,1)==EOK && replace.written==block);
    owned_page_release(&replacement);
    CHECK(!pages[0].references && !pages[0].bytes && replacement.references==1);
    /* A failed later operation discards its private page and preserves the
     * earlier accepted version, without writing or undoing that earlier page. */
    struct owned_file_page aborted;
    owned_page_init(&aborted,block,0xee);
    replace=(struct ext4_file_write_span){.offset=block,.buffer=aborted.bytes,.size=block,
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
        CHECK(!ext4_bcache_find_get(fs->bdev->bc,&cached,physical));
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
static int data_batch_write(struct ext4_blockdev *bdev, const struct ext4_block_span *spans, unsigned count)
{
    unsigned payloads=0;
    for (unsigned i=0;i<count;i++) for (unsigned j=0;j<8;j++) {
        uintptr_t pointer=(uintptr_t)spans[i].data, first=(uintptr_t)batch_payloads[j];
        if (pointer<first || pointer-first>=batch_payload_bytes) continue;
        CHECK(pointer==first && spans[i].block==batch_payload_blocks[j] &&
            spans[i].count==batch_payload_bytes/512);
        payloads++;
    }
    if (payloads) { CHECK(payloads==8 && count==8); payload_batches++; payload_spans+=payloads; }
    for (unsigned i=0;i<count;i++) {
        int r=dev_write(bdev,spans[i].data,spans[i].block,spans[i].count);
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
        struct ext4_file_write_span spans[8];
        for (unsigned i=0;i<8;i++) {
            /* Padding makes separate pages observably noncontiguous in RAM. */
            owned_page_init(&pages[i],page+512,0x30+i);
            batch_payloads[i]=pages[i].bytes;
            spans[i]=(struct ext4_file_write_span){.offset=offsets[i],.buffer=pages[i].bytes,.size=page,
                .source={&pages[i],owned_page_retain,owned_page_release,page+512}};
        }
        CHECK(ext4_fpwrite_batch(&file,spans,8)==EOK);
        for (unsigned i=0;i<8;i++) {
            CHECK(spans[i].written==page && pages[i].references==2 && pages[i].retains==1);
            owned_page_release(&pages[i]);
        }
        batch_payload_bytes=page; payload_batches=payload_spans=0;
        fs->bdev->bdif->bwrite_batch=data_batch_write;
        struct jbd_journal *journal=fs->jbd_journal;
        CHECK(jbd_journal_freeze(journal)==EOK && jbd_journal_select(journal));
        CHECK(jbd_journal_submit(journal)==EOK && jbd_journal_retire(journal,EOK)==EOK);
        fs->bdev->bdif->bwrite_batch=NULL;
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
    struct ext4_file_write_span span={.buffer=page.bytes,.size=bytes,
        .source={&page,owned_page_retain,owned_page_release,bytes}};
    struct jbd_journal *journal=fs->jbd_journal;
    size_t limit=journal->memory_limit;
    journal->memory_limit=journal->memory_used-journal->memory_cached+bytes+12*block+8192;
    CHECK(ext4_fpwrite_batch(&file,&span,1)==EOK && span.written==bytes);
    CHECK(page.references==2 && page.retains==1 && journal->memory_used<=journal->memory_limit);
    owned_page_release(&page);
    CHECK(ext4_journal_group_drain("/")==EOK && !page.references);
    journal->memory_limit=limit;
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
    struct ext4_data_source source={&page,owned_page_retain,owned_page_release,bytes};
    struct jbd_journal *journal=fs->jbd_journal;
    struct jbd_trans *operation=jbd_journal_new_trans(journal); CHECK(operation);
    size_t original_limit=journal->memory_limit, baseline=journal->memory_used-journal->memory_cached;
    journal->memory_limit=baseline+bytes+4096;
    fs->curr_trans=operation;
    for (unsigned i=0;i<blocks;i++) {
        size_t previous=journal->memory_used;
        CHECK(jbd_trans_write_data(operation,homes[i],0,page.bytes+i*block,block,false,&source)==EOK);
        CHECK(page.references==2 && page.retains==1 && journal->memory_used<=journal->memory_limit);
        if (!i) CHECK(journal->memory_used>=baseline+bytes);
        else CHECK(journal->memory_used-previous<block);
    }
    struct ext4_data_source invalid={&rejected,owned_page_retain,owned_page_release,bytes};
    CHECK(jbd_trans_write_data(operation,homes[0],0,rejected.bytes,block,false,&invalid)==ENOMEM);
    CHECK(rejected.references==1 && !rejected.retains && jbd_journal_read_data(fs,homes[0])==page.bytes);
    invalid=source; invalid.bytes++;
    CHECK(jbd_trans_write_data(operation,homes[0],0,page.bytes,block,false,&invalid)==EINVAL);
    invalid=source; invalid.release=owned_page_retain;
    CHECK(jbd_trans_write_data(operation,homes[0],0,page.bytes,block,false,&invalid)==EINVAL);
    invalid=(struct ext4_data_source){.bytes=1};
    CHECK(jbd_trans_write_data(operation,homes[0],0,page.bytes,block,false,&invalid)==EINVAL);
    CHECK(page.references==2 && page.retains==1 && !page.releases);
    fs->curr_trans=NULL; journal->memory_limit=original_limit;
    owned_page_release(&rejected);
    CHECK(jbd_journal_accept(journal,operation,0)==EOK && jbd_journal_freeze(journal)==EOK && jbd_journal_select(journal));
    /* Private, sealed and running records borrow the same retained owner.
     * Aborting a private operation must leave the accepted blocks readable. */
    operation=jbd_journal_new_trans(journal); CHECK(operation);
    CHECK(jbd_trans_write_data(operation,homes[0],0,page.bytes,block,false,&source)==EOK);
    jbd_journal_free_trans(journal,operation,true);
    for (unsigned i=1;i<3;i++) {
        operation=jbd_journal_new_trans(journal); CHECK(operation);
        CHECK(jbd_trans_write_data(operation,homes[i],0,page.bytes+i*block,block,false,&source)==EOK);
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
        struct ext4_file_write_span span={.offset=offsets[i]*block,
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
    struct ext4_file_read_span spans[2]={
        {block+13,readback,17,0,EIO},{2*block+29,readback+32,19,0,EIO}};
    CHECK(ext4_fpread_batch(&file,spans,2)==EOK && spans[0].completed==17 && spans[1].completed==19);
    for (unsigned i=0;i<17;i++) CHECK(readback[i]==0x43);
    for (unsigned i=0;i<19;i++) CHECK(readback[32+i]==0x42);
    CHECK(ext4_journal_group_drain("/")==EIO);
    struct ext4_file_write_span rejected={.buffer=readback,.size=1};
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
    CHECK(ext4_ftruncate(&fill,fill.fsize-65536)==EOK);
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
        CHECK(ext4_ftruncate(&f,f.fsize)==EOK);
        CHECK(ext4_ftruncate(&f,4096)==EOK);
        CHECK(ext4_ftruncate(&f,0)==EOK);
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
            CHECK(jbd_journal_quarantined(fs,retired[i],true));
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
        CHECK(jbd_journal_quarantined(fs,inode,true));
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
    CHECK(f.sync_tid==fs->jbd_journal->committed_id && f.sync_tid);
    CHECK(ext4_file_sync_metadata(&f)==EOK);
    t[0]=(struct ext4_timestamp){12345,42};t[1].nanoseconds=UINT32_MAX;t[2]=(struct ext4_timestamp){1700000001,23};
    CHECK(ext4_file_set_times(&f,5,t)==EOK);
    expect_time(&f,0,t[0],extended);expect_time(&f,1,(struct ext4_timestamp){-1,987654321},extended);expect_time(&f,2,t[2],extended);
    static const int64_t seconds[]={INT64_MIN,INT32_MIN,(int64_t)INT32_MIN+1,-1,0,INT32_MAX,INT64_C(2147483648),INT64_C(15032385535),INT64_MAX};
    for(unsigned n=0;n<sizeof(seconds)/sizeof(seconds[0]);n++) {
        for(unsigned i=0;i<3;i++)t[i]=(struct ext4_timestamp){seconds[n],123456789};
        CHECK(ext4_file_set_times(&f,7,t)==EOK);for(unsigned i=0;i<3;i++)expect_time(&f,i,t[i],extended);
    }
    CHECK(ext4_fraw_inode_fill(&f,&before)==EOK);uint32_t tid=f.sync_tid;
    CHECK(ext4_transaction_begin("/")==EOK);t[0]=(struct ext4_timestamp){9000,7};
    CHECK(ext4_file_set_times(&f,1,t)==EOK);CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED);
    CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && !memcmp(&before,&after,sizeof(before)) && f.sync_tid==tid);
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
static void owner_shared_test(struct ext4_fs *fs, bool unlink_shared, bool truncating)
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
    if(unlink_shared) {
        CHECK(ext4_fclose(&first)==EOK && ext4_fremove("/file")==EOK);
        CHECK(ext4_getxattr("/owner-other","security.capability",19,padding,sizeof(padding),&length)==EOK && length==sizeof(cap) && !memcmp(padding,cap,sizeof(cap)));
        CHECK(ext4_block_get(fs->bdev,&block,shared)==EOK);
        memcpy(&header,block.data,sizeof(header));
        CHECK(to_le32(header.refcount)==1 && ext4_block_set(fs->bdev,&block)==EOK);
        CHECK(ext4_fclose(&second)==EOK && ext4_fremove("/owner-other")==EOK);
        return;
    }
    CHECK(ext4_transaction_begin("/")==EOK && (truncating?ext4_ftruncate(&first,first.fsize):ext4_file_set_owner(&first,70001,80002))==EOK);
    CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED);
    CHECK(ext4_fraw_inode_fill(&first,&changed)==EOK && !memcmp(&changed,&initial,sizeof(initial)));
    CHECK((truncating?ext4_ftruncate(&first,first.fsize):ext4_file_set_owner(&first,70001,80002))==EOK);
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
        CHECK(ext4_file_set_owner(&f,70001,80002)==EROFS);
        CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && !memcmp(&before,&after,sizeof(before)) && disk.writes==writes);
        CHECK(ext4_fclose(&f)==EOK);return;
    }
    uint32_t cap[5]={0x02000000,1,0,0,0}; size_t length;
    CHECK(ext4_file_set_mode(&f,06755)==EOK);
    CHECK(ext4_setxattr("/file","security.capability",19,cap,sizeof(cap))==EOK);
    CHECK(ext4_fraw_inode_fill(&f,&before)==EOK);
    CHECK(ext4_transaction_begin("/")==EOK);
    CHECK(ext4_file_set_owner(&f,70001,80002)==EOK);
    CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED);
    CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && !memcmp(&before,&after,sizeof(before)));
    uint32_t value[5];
    CHECK(ext4_getxattr("/file","security.capability",19,value,sizeof(value),&length)==EOK && length==sizeof(cap) && !memcmp(value,cap,sizeof(cap)));
    CHECK(ext4_file_set_owner(&f,70001,80002)==EOK);
    CHECK(ext4_fraw_inode_fill(&f,&after)==EOK);
    CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && ext4_inode_get_uid(&after)==70001 && ext4_inode_get_gid(&after)==80002 && (ext4_inode_get_mode(&fs->sb,&after)&07777)==0755);
    CHECK(ext4_getxattr("/file","security.capability",19,value,sizeof(value),&length)==ENODATA);
    CHECK(ext4_file_set_mode(&f,06640)==EOK);
    CHECK(ext4_file_set_owner(&f,UINT32_MAX,UINT32_MAX)==EOK);
    CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && ext4_inode_get_uid(&after)==70001 && ext4_inode_get_gid(&after)==80002 && (ext4_inode_get_mode(&fs->sb,&after)&07777)==02640);
    CHECK(ext4_file_sync_metadata(&f)==EOK && ext4_fclose(&f)==EOK);
    ext4_file unlinked; uint32_t inode; bool orphan;
    CHECK(ext4_fopen(&unlinked,"/owner-unlinked","w+")==EOK);
    CHECK(ext4_funlink_dentry("/owner-unlinked",&inode,&orphan)==EOK && orphan);
    CHECK(ext4_file_set_owner(&unlinked,123,456)==EOK);
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
    CHECK(ext4_ftruncate(&f,UINT64_C(10000000))==EOK);
    CHECK(ext4_mount_point_stats("/",&after)==EOK && after.free_blocks_count==before.free_blocks_count);
    char bytes[4096]={0};size_t count;CHECK(ext4_fseek(&f,65536,SEEK_SET)==EOK);CHECK(ext4_fwrite(&f,bytes,sizeof(bytes),&count)==EOK && count==sizeof(bytes));
    CHECK(ext4_mount_point_stats("/",&after)==EOK && after.free_blocks_count<before.free_blocks_count);
    CHECK(after.overhead_blocks==before.overhead_blocks);
    CHECK(ext4_ftruncate(&f,0)==EOK);CHECK(ext4_mount_point_stats("/",&after)==EOK && after.free_blocks_count==before.free_blocks_count);
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
    uint32_t tid=f.sync_tid;
    struct ext4_mount_stats untouched,stats;memset(&stats,0xa5,sizeof(stats));untouched=stats;
    struct ext4_timestamp t[3]={{1234,1},{5678,2},{9012,3}};
    bool stats_op=strstr(mode,"stats")!=NULL;
    bool oom=!strncmp(mode,"oom",3);
    if(stats_op)evict_clean(fs);
    if(!strcmp(mode,"read-stats"))fail_read=reads+1;
    allocations=0;if(oom)fail_allocation=point;
    if(!strncmp(mode,"write-",6))disk.fail_write=disk.writes+1;
    if(!strncmp(mode,"flush-",6))disk.fail_flush=disk.flushes+1;
    int r=stats_op?ext4_mount_point_stats("/",&stats):owner_op?ext4_file_set_owner(&f,70001,80002):ext4_file_set_times(&f,7,t);
    unsigned attempts=allocations;fail_allocation=0;fail_read=0;
    disk.fail_write=disk.fail_flush=0;
    if(oom || !strcmp(mode,"read-stats")) {
        if(r!=EOK) {
            CHECK(r==(oom?ENOMEM:EIO));CHECK(!fs->jbd_journal || fs->jbd_journal->error==EOK);
            CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && !memcmp(&before,&after,sizeof(before)) && f.sync_tid==tid);
            if(stats_op)CHECK(!memcmp(&untouched,&stats,sizeof(stats)));
        } else CHECK(oom && !point);
        if(stats_op)CHECK(ext4_mount_point_stats("/",&stats)==EOK && stats.overhead_blocks==expected);
        else if(owner_op) {
            if(r!=EOK) { uint32_t cap[5];size_t length;CHECK(ext4_getxattr("/file","security.capability",19,cap,sizeof(cap),&length)==EOK && length==sizeof(cap)); }
            CHECK(ext4_file_set_owner(&f,70001,80002)==EOK);
            CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && ext4_inode_get_uid(&after)==70001 && ext4_inode_get_gid(&after)==80002);
        }
        else { CHECK(ext4_file_set_times(&f,7,t)==EOK);expect_time(&f,0,t[0],ext4_get16(&fs->sb,inode_size)>128); }
        CHECK(ext4_fclose(&f)==EOK);
        if(oom)printf("%u\n",attempts);
        return;
    }
    CHECK(r==EIO && fs->jbd_journal->error==EIO);
    CHECK(ext4_file_set_times(&f,7,t)==EIO && f.sync_tid==tid);
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

int main(int argc,char **argv)
{
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
    if(!strcmp(argv[2],"extent-read-oom") || !strcmp(argv[2],"extent-read-error"))
        extent_read_failure(&dev,!strcmp(argv[2],"extent-read-error"),argc>3?strtoul(argv[3],NULL,10):0);
    else if(!strcmp(argv[2],"seed"))seed();
    else if(!strcmp(argv[2],"cost"))cost_test(dev.fs);
    else if(!strcmp(argv[2],"group"))group_test(dev.fs);
    else if(!strcmp(argv[2],"group-log-payload"))group_log_payload(dev.fs);
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
    else if(!strcmp(argv[2],"owner-shared") || !strcmp(argv[2],"owner-shared-unlink") || !strcmp(argv[2],"truncate-shared"))
        owner_shared_test(dev.fs,!strcmp(argv[2],"owner-shared-unlink"),!strcmp(argv[2],"truncate-shared"));
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
    if(!strncmp(argv[2],"extent-read-",12))CHECK(!live_allocations);
    fault_block_close(&disk);return 0;
}
