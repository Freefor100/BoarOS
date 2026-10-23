/* Real lazy block/inode groups with the production journal budget. */
#define main metadata_fixture_main
#include "lwext4_metadata.c"
#undef main

static uint16_t lazy_flags(struct ext4_fs *fs,unsigned group,uint32_t *free_blocks,uint32_t *unused)
{
    struct ext4_block_group_ref ref;
    CHECK(ext4_fs_get_block_group_ref(fs,group,&ref)==EOK);
    uint16_t flags=to_le16(ref.block_group->flags);
    if (free_blocks) *free_blocks=ext4_bg_get_free_blocks_count(ref.block_group,&fs->sb);
    if (unused) *unused=ext4_bg_get_itable_unused(ref.block_group,&fs->sb);
    CHECK(ext4_fs_put_block_group_ref(&ref)==EOK);
    return flags;
}
static void lazy_data(struct ext4_fs *fs)
{
    uint32_t free0,free1;
    uint16_t flags1=lazy_flags(fs,1,&free1,NULL),flags2=lazy_flags(fs,2,NULL,NULL);
    CHECK((flags1&EXT4_BLOCK_GROUP_INODE_UNINIT) && !(flags1&EXT4_BLOCK_GROUP_ITABLE_ZEROED));
    CHECK((flags2&EXT4_BLOCK_GROUP_INODE_UNINIT) && !(flags2&EXT4_BLOCK_GROUP_ITABLE_ZEROED));
    (void)lazy_flags(fs,0,&free0,NULL);
    ext4_file file; CHECK(ext4_fopen(&file,"/lazy-data","w+")==EOK);
    uint64_t bytes=((uint64_t)free0+free1+8)*fs->bdev->lg_bsize;
    bool changed;
    int result=ext4_fallocate(&file,0,0,bytes,&changed, UINT32_MAX);
    fprintf(stderr,"lazy data reservation: result=%d bytes=%llu budget=%zu failures=%llu\n",result,
        (unsigned long long)bytes,fs->jbd_journal?jbd_memory_limit(fs->jbd_journal):0,
        (unsigned long long)(fs->jbd_journal?jbd_memory_budget_failures(fs->jbd_journal):0));
    CHECK(result==EOK && changed && file.fsize==bytes);
    CHECK(lazy_flags(fs,1,NULL,NULL)==flags1);
    CHECK((lazy_flags(fs,2,NULL,NULL)&~EXT4_BLOCK_GROUP_BLOCK_UNINIT)==
        (flags2&~EXT4_BLOCK_GROUP_BLOCK_UNINIT));
    CHECK(!(lazy_flags(fs,2,NULL,NULL)&EXT4_BLOCK_GROUP_BLOCK_UNINIT));
    struct ext4_inode_ref inode; CHECK(ext4_fs_get_inode_ref(fs,file.inode,&inode)==EOK);
    unsigned groups=0; uint64_t logical=0;
    while (logical<bytes/fs->bdev->lg_bsize) {
        struct ext4_inode_block_range range;
        CHECK(ext4_fs_get_inode_dblk_range(&inode,logical,UINT32_MAX,false,&range)==EOK &&
            range.block && range.count && range.unwritten);
        groups|=1U<<ext4_balloc_get_bgid_of_block(&fs->sb,range.block);
        groups|=1U<<ext4_balloc_get_bgid_of_block(&fs->sb,range.block+range.count-1);
        logical+=range.count;
    }
    CHECK((groups&6)==6 && ext4_fs_put_inode_ref(&inode)==EOK);
    unsigned char marker=0x6a,actual=0; size_t done;
    CHECK(ext4_fpwrite(&file,bytes-1,&marker,1,&done)==EOK && done==1);
    CHECK(ext4_file_sync_metadata_mode(&file,false)==EOK && ext4_fclose(&file)==EOK);
    CHECK(ext4_fopen(&file,"/lazy-data","r")==EOK && file.fsize==bytes);
    CHECK(ext4_fpread(&file,bytes-1,&actual,1,&done)==EOK && done==1 && actual==marker);
    CHECK(ext4_fpread(&file,bytes/2,&actual,1,&done)==EOK && done==1 && !actual);
    CHECK(ext4_fclose(&file)==EOK);
    puts("PASS lazy data: crossed physical groups, reserved write/reload; inode flags and tables remain lazy");
}
struct lazy_snapshot {
    unsigned char descriptor[EXT4_MAX_BLOCK_GROUP_DESCRIPTOR_SIZE];
    unsigned char *bitmap;
    ext4_fsblk_t bitmap_block, table;
    uint16_t flags;
    uint32_t unused;
};
static struct lazy_snapshot *lazy_snapshots;
static unsigned lazy_snapshot_count;
static bool lazy_watch_tables;
static int lazy_dev_write(struct ext4_blockdev *dev,const void *data,uint64_t sector,uint32_t count)
{
    if (lazy_watch_tables) for (unsigned i=0;i<lazy_snapshot_count;i++) {
        if (!(lazy_snapshots[i].flags&EXT4_BLOCK_GROUP_INODE_UNINIT)) continue;
        uint64_t tail=(lazy_snapshots[i].table+1)*dev->lg_bsize/512;
        CHECK(!(sector<=tail && tail<sector+count));
    }
    return dev_write(dev,data,sector,count);
}
static void lazy_snapshot_take(struct ext4_fs *fs)
{
    lazy_snapshot_count=ext4_block_group_cnt(&fs->sb);
    lazy_snapshots=calloc(lazy_snapshot_count,sizeof(*lazy_snapshots)); CHECK(lazy_snapshots);
    unsigned desc=ext4_sb_get_desc_size(&fs->sb),block=fs->bdev->lg_bsize;
    for (unsigned group=0;group<lazy_snapshot_count;group++) {
        struct lazy_snapshot *snapshot=&lazy_snapshots[group];
        struct ext4_block_group_ref ref; CHECK(ext4_fs_get_block_group_ref(fs,group,&ref)==EOK);
        memcpy(snapshot->descriptor,ref.block_group,desc);
        snapshot->flags=to_le16(ref.block_group->flags);
        snapshot->unused=ext4_bg_get_itable_unused(ref.block_group,&fs->sb);
        snapshot->bitmap_block=ext4_bg_get_inode_bitmap(ref.block_group,&fs->sb);
        snapshot->table=ext4_bg_get_inode_table_first_block(ref.block_group,&fs->sb);
        CHECK(ext4_fs_put_block_group_ref(&ref)==EOK);
        struct ext4_block bitmap; CHECK(ext4_block_get(fs->bdev,&bitmap,snapshot->bitmap_block)==EOK);
        snapshot->bitmap=malloc(block); CHECK(snapshot->bitmap); memcpy(snapshot->bitmap,bitmap.data,block);
        CHECK(ext4_block_set(fs->bdev,&bitmap)==EOK);
    }
}
static void lazy_snapshot_release(void)
{
    for (unsigned group=0;group<lazy_snapshot_count;group++) free(lazy_snapshots[group].bitmap);
    free(lazy_snapshots); lazy_snapshots=NULL; lazy_snapshot_count=0;
}
static void lazy_inode_tail(struct ext4_fs *fs,uint32_t number)
{
    struct ext4_inode_ref inode; CHECK(ext4_fs_get_inode_ref(fs,number,&inode)==EOK);
    unsigned size=ext4_get16(&fs->sb,inode_size);
    for (unsigned i=size-16;i<size;i++) CHECK(!((unsigned char *)inode.inode)[i]);
    CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
}
static void lazy_inode_rollback(struct ext4_fs *fs,const char *directory,const char *name)
{
    if (!fs->jbd_journal) return;
    unsigned desc=ext4_sb_get_desc_size(&fs->sb),block=fs->bdev->lg_bsize;
    uint32_t free_inodes=ext4_get32(&fs->sb,free_inodes_count);
    uint64_t free_blocks=ext4_sb_get_free_blocks_cnt(&fs->sb);
    unsigned failures=0;
    for (unsigned point=0;point<=256;point++) {
        CHECK(ext4_transaction_begin("/")==EOK);
        if (point) fail_allocation=allocations+point;
        int result=ext4_dir_mk(directory);
        ext4_file file;
        if (result==EOK) {
            result=ext4_fopen(&file,name,"w+");
            if (result==EOK) {
                unsigned char byte=0x7c; size_t done;
                result=ext4_fwrite(&file,&byte,1,&done);
                if (!result) CHECK(done==1);
                CHECK(ext4_fclose(&file)==EOK);
            }
        }
        bool injected=point && allocations>=fail_allocation;
        fail_allocation=0;
        if (injected) {
            if (result!=ENOMEM) fprintf(stderr,"lazy OOM point=%u result=%d allocations=%u\n",point,result,allocations);
            CHECK(result==ENOMEM); failures++;
        }
        else CHECK(result==EOK);
        CHECK(ext4_transaction_abort("/",result?result:ECANCELED)==(result?result:ECANCELED));
        CHECK(ext4_get32(&fs->sb,free_inodes_count)==free_inodes && ext4_sb_get_free_blocks_cnt(&fs->sb)==free_blocks);
        for (unsigned group=0;group<lazy_snapshot_count;group++) {
            struct lazy_snapshot *snapshot=&lazy_snapshots[group];
            struct ext4_block_group_ref ref;
            CHECK(ext4_fs_get_block_group_ref(fs,group,&ref)==EOK && !memcmp(snapshot->descriptor,ref.block_group,desc));
            CHECK(ext4_fs_put_block_group_ref(&ref)==EOK);
            struct ext4_block bitmap;
            CHECK(ext4_block_get(fs->bdev,&bitmap,snapshot->bitmap_block)==EOK && !memcmp(snapshot->bitmap,bitmap.data,block));
            CHECK(ext4_block_set(fs->bdev,&bitmap)==EOK);
        }
        ext4_dir parent;
        CHECK(ext4_dir_open(&parent,directory)==ENOENT && ext4_fopen(&file,name,"r")==ENOENT);
        if (point && !injected) break;
        CHECK(point<256);
    }
    CHECK(failures);
    printf("PASS lazy inode: outer abort and %u OOM points restore all candidate groups, counts and namespace\n",failures);
}
static void lazy_inode(struct ext4_fs *fs)
{
    uint32_t previous_group=UINT32_MAX;
    for (unsigned round=0;round<2;round++) {
        /* Directory placement chooses a real lazy group. Poison every eligible
         * first slot before that choice, then check the selected group rather
         * than steering the allocator with a test-only global cursor. */
        unsigned char *poison=calloc(1,fs->bdev->lg_bsize); CHECK(poison);
        memset(poison,0xa5,ext4_get16(&fs->sb,inode_size));
        for (unsigned group=0;group<ext4_block_group_cnt(&fs->sb);group++) {
            struct ext4_block_group_ref ref; CHECK(ext4_fs_get_block_group_ref(fs,group,&ref)==EOK);
            uint16_t flags=to_le16(ref.block_group->flags);
            ext4_fsblk_t table=ext4_bg_get_inode_table_first_block(ref.block_group,&fs->sb);
            CHECK(ext4_fs_put_block_group_ref(&ref)==EOK);
            if (flags&EXT4_BLOCK_GROUP_INODE_UNINIT)
                CHECK(ext4_blocks_set_direct(fs->bdev,poison,table,1)==EOK);
        }
        CHECK(ext4_blockdev_flush(fs->bdev)==EOK); free(poison);
        lazy_snapshot_take(fs); lazy_watch_tables=true;
        char directory[32],name[48];
        snprintf(directory,sizeof(directory),"/lazy-parent-%u",round);
        snprintf(name,sizeof(name),"%s/file",directory);
        lazy_inode_rollback(fs,directory,name);
        CHECK(ext4_dir_mk(directory)==EOK);
        ext4_dir parent; CHECK(ext4_dir_open(&parent,directory)==EOK);
        uint32_t inodes_per_group=ext4_get32(&fs->sb,inodes_per_group);
        unsigned group=(parent.f.inode-1)/inodes_per_group;
        CHECK(group<lazy_snapshot_count && group!=previous_group); previous_group=group;
        struct lazy_snapshot *before=&lazy_snapshots[group];
        CHECK((before->flags&EXT4_BLOCK_GROUP_INODE_UNINIT) && !(before->flags&EXT4_BLOCK_GROUP_ITABLE_ZEROED));
        CHECK(before->unused==inodes_per_group && parent.f.inode==group*inodes_per_group+1);
        lazy_inode_tail(fs,parent.f.inode); CHECK(ext4_dir_close(&parent)==EOK);
        ext4_file file; int result=ext4_fopen(&file,name,"w+");
        fprintf(stderr,"lazy inode allocation: parent group=%u result=%d budget=%zu failures=%llu\n",group,result,
            fs->jbd_journal?jbd_memory_limit(fs->jbd_journal):0,
            (unsigned long long)(fs->jbd_journal?jbd_memory_budget_failures(fs->jbd_journal):0));
        CHECK(result==EOK && file.inode==group*inodes_per_group+2);
        uint32_t unused; uint16_t after=lazy_flags(fs,group,NULL,&unused);
        CHECK(!(after&(EXT4_BLOCK_GROUP_INODE_UNINIT|EXT4_BLOCK_GROUP_BLOCK_UNINIT|EXT4_BLOCK_GROUP_ITABLE_ZEROED)));
        CHECK(unused==inodes_per_group-2); lazy_inode_tail(fs,file.inode);
        unsigned char marker=round+1,actual=0; size_t done;
        CHECK(ext4_fwrite(&file,&marker,1,&done)==EOK && done==1 && ext4_file_sync_metadata_mode(&file,false)==EOK);
        CHECK(ext4_fclose(&file)==EOK && ext4_fopen(&file,name,"r")==EOK);
        CHECK(ext4_fread(&file,&actual,1,&done)==EOK && done==1 && actual==marker && ext4_fclose(&file)==EOK);
        lazy_watch_tables=false; lazy_snapshot_release();
    }
    puts("PASS lazy inode: real directory group placement, parent-local child, fresh slots and untouched table tails");
}
static void lazy_verify(struct ext4_fs *fs)
{
    for (unsigned round=0;round<2;round++) {
        char directory[32],name[48];
        snprintf(directory,sizeof(directory),"/lazy-parent-%u",round);
        snprintf(name,sizeof(name),"%s/file",directory);
        ext4_dir parent; int result=ext4_dir_open(&parent,directory);
        CHECK(result==EOK || result==ENOENT);
        if (result==ENOENT) continue;
        uint32_t inodes_per_group=ext4_get32(&fs->sb,inodes_per_group);
        unsigned group=(parent.f.inode-1)/inodes_per_group;
        CHECK(parent.f.inode==group*inodes_per_group+1);
        lazy_inode_tail(fs,parent.f.inode); CHECK(ext4_dir_close(&parent)==EOK);
        ext4_file file; result=ext4_fopen(&file,name,"r");
        CHECK(result==EOK || result==ENOENT);
        uint32_t unused; unsigned allocated=result==EOK?2:1;
        CHECK(!(lazy_flags(fs,group,NULL,&unused)&(EXT4_BLOCK_GROUP_INODE_UNINIT|EXT4_BLOCK_GROUP_ITABLE_ZEROED)));
        CHECK(unused==inodes_per_group-allocated);
        if (result==ENOENT) continue;
        CHECK(file.inode==group*inodes_per_group+2 && file.fsize<=1);
        lazy_inode_tail(fs,file.inode);
        if (file.fsize) { unsigned char byte; size_t done; CHECK(ext4_fread(&file,&byte,1,&done)==EOK && done==1 && byte==round+1); }
        CHECK(ext4_fclose(&file)==EOK);
    }
}
int main(int argc,char **argv)
{
    CHECK((argc==3 || argc==4) && fault_block_open(&disk,argv[1])==0);
    unsigned char scratch[512];
    struct ext4_blockdev_iface iface={.open=dev_open,.close=dev_open,.bread=dev_read,.bwrite=lazy_dev_write,.flush=dev_flush,
        .ph_bsize=512,.ph_bcnt=disk.device.capacity_bytes/512,.ph_bbuf=scratch};
    struct ext4_blockdev dev={.bdif=&iface,.part_size=disk.device.capacity_bytes};
    CHECK(ext4_device_register(&dev,"metadata")==EOK && ext4_mount("metadata","/",false)==EOK);
    if (ext4_sb_feature_com(&dev.fs->sb,EXT4_FCOM_HAS_JOURNAL)) {
        CHECK(ext4_recover("/")==EOK && ext4_journal_start("/")==EOK && ext4_orphan_recover("/")==EOK);
        struct ext4_journal_runtime runtime={.now_ns=group_clock};
        CHECK(ext4_journal_group_enable("/",&runtime,dev.lg_bsize==4096?4*1024*1024:512*1024)==EOK);
    }
    CHECK(ext4_block_group_cnt(&dev.fs->sb)>=3);
    if (strcmp(argv[2],"verify")) independent_cut=argc==4?strtoul(argv[3],NULL,10):UINT32_MAX;
    if (!strcmp(argv[2],"data")) lazy_data(dev.fs);
    else if (!strcmp(argv[2],"inode")) lazy_inode(dev.fs);
    else { CHECK(!strcmp(argv[2],"verify")); lazy_verify(dev.fs); }
    CHECK(ext4_umount("/")==EOK && !live_allocations);
    printf("lazy boundaries=%u\n",independent_event);
    fault_block_close(&disk);
    puts("PASS lazy groups: normal journal stop and all allocation owners returned");
    return 0;
}
