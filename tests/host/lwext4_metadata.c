/* Live-inode explicit timestamps and actual ext4 allocation statistics. */
#include "block_fault.h"
#include <ext4.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <ext4_block_group.h>
#include <ext4_journal.h>
#include <ext4_trans.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct fault_block disk;
static unsigned allocations, fail_allocation, reads, fail_read;
static uint64_t forbidden_lba = UINT64_MAX, forbidden_writes;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%d: %s (alloc=%u fail=%u)\n",__LINE__,#x,allocations,fail_allocation); exit(1); } } while (0)
void *ext4_user_malloc(size_t n) { return ++allocations == fail_allocation ? NULL : malloc(n); }
void *ext4_user_calloc(size_t n,size_t s) { return ++allocations == fail_allocation ? NULL : calloc(n,s); }
void *ext4_user_realloc(void *p,size_t n) { return ++allocations == fail_allocation ? NULL : realloc(p,n); }
void ext4_user_free(void *p) { free(p); }
static int dev_open(struct ext4_blockdev *b) { (void)b; return EOK; }
static int dev_read(struct ext4_blockdev *b,void *p,uint64_t n,uint32_t c)
{ (void)b; if (++reads == fail_read) return EIO; return kernel_block_read_at(&disk.device,n*512,p,(size_t)c*512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
static int dev_write(struct ext4_blockdev *b,const void *p,uint64_t n,uint32_t c)
{ (void)b; if (n <= forbidden_lba && forbidden_lba < n+c) forbidden_writes++; return kernel_block_write_at(&disk.device,n*512,p,(size_t)c*512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
static int dev_flush(struct ext4_blockdev *b)
{ (void)b; return kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
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
    if (readonly) { CHECK(ext4_file_set_times(&f,7,t)==EROFS);CHECK(disk.writes==writes);CHECK(ext4_fclose(&f)==EOK);return; }
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
