/* Live-inode explicit timestamps and actual ext4 allocation statistics. */
#include "block_fault.h"
#include <ext4.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <ext4_block_group.h>
#include <ext4_bitmap.h>
#include <ext4_journal.h>
#include <ext4_trans.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct fault_block disk;
static unsigned allocations, fail_allocation, reads, fail_read;
static size_t payload_threshold;
static unsigned payload_allocations;
static uint64_t forbidden_lba = UINT64_MAX, forbidden_writes;
static void (*write_interleave)(void);
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%d: %s (alloc=%u fail=%u)\n",__LINE__,#x,allocations,fail_allocation); exit(1); } } while (0)
void *ext4_user_malloc(size_t n) { if(payload_threshold && n>=payload_threshold)payload_allocations++; return ++allocations == fail_allocation ? NULL : malloc(n); }
void *ext4_user_calloc(size_t n,size_t s) { if(payload_threshold && n*s>=payload_threshold)payload_allocations++; return ++allocations == fail_allocation ? NULL : calloc(n,s); }
void *ext4_user_realloc(void *p,size_t n) { return ++allocations == fail_allocation ? NULL : realloc(p,n); }
void ext4_user_free(void *p) { free(p); }
static int dev_open(struct ext4_blockdev *b) { (void)b; return EOK; }
static int dev_read(struct ext4_blockdev *b,void *p,uint64_t n,uint32_t c)
{ (void)b; if (++reads == fail_read) return EIO; return kernel_block_read_at(&disk.device,n*512,p,(size_t)c*512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
static int dev_write(struct ext4_blockdev *b,const void *p,uint64_t n,uint32_t c)
{ (void)b; if(write_interleave){void (*hook)(void)=write_interleave;write_interleave=NULL;hook();} if (n <= forbidden_lba && forbidden_lba < n+c) forbidden_writes++; return kernel_block_write_at(&disk.device,n*512,p,(size_t)c*512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
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
    CHECK(ext4_journal_group_progress("/",&large)==EOK && large.sealed>small.sealed);
    CHECK(group_now==0 && fs->jbd_journal->memory_peak<=fs->jbd_journal->memory_limit);
    CHECK(ext4_journal_group_drain("/")==EOK && ext4_fseek(&f,0,SEEK_SET)==EOK);
    for(unsigned i=0;i<96;i++) {
        CHECK(ext4_fread(&f,bytes,sizeof(bytes),&count)==EOK && count==sizeof(bytes));
        for(unsigned j=0;j<sizeof(bytes);j++) CHECK(bytes[j]==(unsigned char)i);
    }
    CHECK(ext4_fclose(&f)==EOK);
    puts("group bounds: unique data seals at constant clock; content and budget preserved");
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
    uint32_t tid=f.sync_tid;
    struct ext4_mount_stats untouched,stats;memset(&stats,0xa5,sizeof(stats));untouched=stats;
    struct ext4_timestamp t[3]={{1234,1},{5678,2},{9012,3}};
    bool stats_op=strstr(mode,"stats")!=NULL;
    bool oom=!strncmp(mode,"oom",3);
    if(stats_op)evict_clean(fs);
    if(!strcmp(mode,"read-stats"))fail_read=reads+1;
    allocations=0;if(oom)fail_allocation=point;
    if(!strcmp(mode,"write-times"))disk.fail_write=disk.writes+1;
    if(!strcmp(mode,"flush-times"))disk.fail_flush=disk.flushes+1;
    int r=stats_op?ext4_mount_point_stats("/",&stats):ext4_file_set_times(&f,7,t);
    unsigned attempts=allocations;fail_allocation=0;fail_read=0;
    disk.fail_write=disk.fail_flush=0;
    if(oom || !strcmp(mode,"read-stats")) {
        if(r!=EOK) {
            CHECK(r==(oom?ENOMEM:EIO));CHECK(!fs->jbd_journal || fs->jbd_journal->error==EOK);
            CHECK(ext4_fraw_inode_fill(&f,&after)==EOK && !memcmp(&before,&after,sizeof(before)) && f.sync_tid==tid);
            if(stats_op)CHECK(!memcmp(&untouched,&stats,sizeof(stats)));
        } else CHECK(oom && !point);
        if(stats_op)CHECK(ext4_mount_point_stats("/",&stats)==EOK && stats.overhead_blocks==expected);
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
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"bitmap")){bitmap_test();return 0;}
    CHECK(argc>=3);CHECK(fault_block_open(&disk,argv[1])==0);unsigned char scratch[512];
    struct ext4_blockdev_iface iface={.open=dev_open,.close=dev_open,.bread=dev_read,.bwrite=dev_write,.flush=dev_flush,.ph_bsize=512,.ph_bcnt=disk.device.capacity_bytes/512,.ph_bbuf=scratch};
    struct ext4_blockdev dev={.bdif=&iface,.part_size=disk.device.capacity_bytes};
    CHECK(ext4_device_register(&dev,"metadata")==EOK);
    bool readonly=!strcmp(argv[2],"readonly") || !strcmp(argv[2],"stats-ro") || !strcmp(argv[2],"read-stats");
    CHECK(ext4_mount("metadata","/",readonly)==EOK);
    if(ext4_sb_feature_com(&dev.fs->sb,EXT4_FCOM_HAS_JOURNAL)) {
        CHECK(ext4_recover("/")==EOK);CHECK(ext4_journal_start("/")==EOK);CHECK(ext4_orphan_recover("/")==EOK);
    }
    if(!strcmp(argv[2],"seed"))seed();
    else if(!strcmp(argv[2],"cost"))cost_test(dev.fs);
    else if(!strcmp(argv[2],"group"))group_test(dev.fs);
    else if(!strcmp(argv[2],"group-space"))group_space(dev.fs);
    else if(!strcmp(argv[2],"group-ring"))group_ring(dev.fs);
    else if(!strcmp(argv[2],"group-bounds")) group_bounds(dev.fs);
    else if(!strcmp(argv[2],"group-pipeline") || !strcmp(argv[2],"group-commit-crash")) {group_pipeline(dev.fs,!strcmp(argv[2],"group-commit-crash"));if(!strcmp(argv[2],"group-commit-crash"))return 0;}
    else if(!strcmp(argv[2],"group-write") || !strcmp(argv[2],"group-flush")) {CHECK(argc==4);group_fault(dev.fs,argv[2],strtoul(argv[3],NULL,10));return 0;}
    else if(!strcmp(argv[2],"group-recovery"))group_verify_recovery(dev.fs);
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
    else if(strstr(argv[2],"times") || strstr(argv[2],"stats")) {
        CHECK(argc>3);failure_test(dev.fs,argv[2],strtoull(argv[3],NULL,10),argc>4?strtoul(argv[4],NULL,10):0);
        if(!strcmp(argv[2],"write-times") || !strcmp(argv[2],"flush-times"))return 0;
    }
    else CHECK(!"unknown mode");
    CHECK(ext4_umount("/")==EOK);fault_block_close(&disk);return 0;
}
