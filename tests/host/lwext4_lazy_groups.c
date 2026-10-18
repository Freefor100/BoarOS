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
    int result=ext4_fallocate(&file,0,0,bytes,&changed);
    fprintf(stderr,"lazy data reservation: result=%d bytes=%llu budget=%zu failures=%llu\n",result,
        (unsigned long long)bytes,fs->jbd_journal?fs->jbd_journal->memory_limit:0,
        (unsigned long long)(fs->jbd_journal?fs->jbd_journal->budget_failures:0));
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
static void lazy_inode_rollback(struct ext4_fs *fs,unsigned group,const char *name)
{
    if (!fs->jbd_journal) return;
    struct ext4_block_group_ref ref; CHECK(ext4_fs_get_block_group_ref(fs,group,&ref)==EOK);
    unsigned desc=ext4_sb_get_desc_size(&fs->sb),block=fs->bdev->lg_bsize;
    unsigned char descriptor[EXT4_MAX_BLOCK_GROUP_DESCRIPTOR_SIZE];
    memcpy(descriptor,ref.block_group,desc);
    ext4_fsblk_t bitmap=ext4_bg_get_inode_bitmap(ref.block_group,&fs->sb);
    CHECK(ext4_fs_put_block_group_ref(&ref)==EOK);
    struct ext4_block before;
    CHECK(ext4_block_get(fs->bdev,&before,bitmap)==EOK);
    unsigned char *bits=malloc(block); CHECK(bits); memcpy(bits,before.data,block);
    CHECK(ext4_block_set(fs->bdev,&before)==EOK);
    uint32_t free_inodes=ext4_get32(&fs->sb,free_inodes_count);
    uint64_t free_blocks=ext4_sb_get_free_blocks_cnt(&fs->sb);
    unsigned failures=0;
    for (unsigned point=0;point<=128;point++) {
        fs->last_inode_bg_id=group;
        CHECK(ext4_transaction_begin("/")==EOK);
        if (point) fail_allocation=allocations+point;
        ext4_file file; int result=ext4_fopen(&file,name,"w+");
        bool injected=point && allocations>=fail_allocation;
        fail_allocation=0;
        if (injected) { CHECK(result==ENOMEM); failures++; }
        else CHECK(result==EOK);
        if (result==EOK) {
            if (!point) { unsigned char byte=0x7c; size_t done; CHECK(ext4_fwrite(&file,&byte,1,&done)==EOK && done==1); }
            CHECK(ext4_fclose(&file)==EOK);
        }
        CHECK(ext4_transaction_abort("/",result?result:ECANCELED)==(result?result:ECANCELED));
        CHECK(ext4_get32(&fs->sb,free_inodes_count)==free_inodes && ext4_sb_get_free_blocks_cnt(&fs->sb)==free_blocks);
        CHECK(ext4_fs_get_block_group_ref(fs,group,&ref)==EOK && !memcmp(descriptor,ref.block_group,desc));
        CHECK(ext4_fs_put_block_group_ref(&ref)==EOK);
        CHECK(ext4_block_get(fs->bdev,&before,bitmap)==EOK && !memcmp(bits,before.data,block));
        CHECK(ext4_block_set(fs->bdev,&before)==EOK && ext4_fopen(&file,name,"r")==ENOENT);
        if (point && !injected) break;
        CHECK(point<128);
    }
    CHECK(failures); free(bits);
    printf("PASS lazy inode group=%u: outer abort and %u OOM points restore bitmap, flags, unused count and namespace\n",group,failures);
}
static void lazy_inode(struct ext4_fs *fs)
{
    for (unsigned group=1;group<=2;group++) {
        uint32_t unused;
        uint16_t before=lazy_flags(fs,group,NULL,&unused);
        CHECK((before&EXT4_BLOCK_GROUP_INODE_UNINIT) && !(before&EXT4_BLOCK_GROUP_ITABLE_ZEROED));
        CHECK(unused==ext4_get32(&fs->sb,inodes_per_group));
        struct ext4_block_group_ref bg; CHECK(ext4_fs_get_block_group_ref(fs,group,&bg)==EOK);
        ext4_fsblk_t table=ext4_bg_get_inode_table_first_block(bg.block_group,&fs->sb);
        CHECK(ext4_fs_put_block_group_ref(&bg)==EOK);
        unsigned char *poison=calloc(1,fs->bdev->lg_bsize); CHECK(poison);
        memset(poison,0xa5,ext4_get16(&fs->sb,inode_size));
        CHECK(ext4_blocks_set_direct(fs->bdev,poison,table,1)==EOK && ext4_blockdev_flush(fs->bdev)==EOK);
        free(poison);
        fs->last_inode_bg_id=group;
        char name[32]; snprintf(name,sizeof(name),"/lazy-inode-%u",group);
        lazy_inode_rollback(fs,group,name);
        fs->last_inode_bg_id=group;
        forbidden_lba=(table+1)*fs->bdev->lg_bsize/512; forbidden_writes=0;
        ext4_file file; int result=ext4_fopen(&file,name,"w+");
        fprintf(stderr,"lazy inode allocation: group=%u result=%d budget=%zu failures=%llu\n",group,result,
            fs->jbd_journal?fs->jbd_journal->memory_limit:0,
            (unsigned long long)(fs->jbd_journal?fs->jbd_journal->budget_failures:0));
        CHECK(result==EOK && file.inode==group*unused+1);
        uint16_t after=lazy_flags(fs,group,NULL,&unused);
        CHECK(!(after&(EXT4_BLOCK_GROUP_INODE_UNINIT|EXT4_BLOCK_GROUP_BLOCK_UNINIT|EXT4_BLOCK_GROUP_ITABLE_ZEROED)));
        CHECK(unused==ext4_get32(&fs->sb,inodes_per_group)-1);
        struct ext4_inode_ref inode; CHECK(ext4_fs_get_inode_ref(fs,file.inode,&inode)==EOK);
        unsigned size=ext4_get16(&fs->sb,inode_size);
        for (unsigned i=size-16;i<size;i++) CHECK(!((unsigned char *)inode.inode)[i]);
        CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
        unsigned char marker=group,actual=0; size_t done;
        CHECK(ext4_fwrite(&file,&marker,1,&done)==EOK && done==1 && ext4_file_sync_metadata_mode(&file,false)==EOK);
        CHECK(ext4_fclose(&file)==EOK && ext4_fopen(&file,name,"r")==EOK);
        CHECK(ext4_fread(&file,&actual,1,&done)==EOK && done==1 && actual==marker && ext4_fclose(&file)==EOK);
        CHECK(!forbidden_writes); forbidden_lba=UINT64_MAX;
    }
    puts("PASS lazy inode: initialized bitmap and fresh slot, untouched table tail, truthful zeroed flag and unused count");
}
static void lazy_verify(struct ext4_fs *fs)
{
    for (unsigned group=1;group<=2;group++) {
        char name[32]; snprintf(name,sizeof(name),"/lazy-inode-%u",group);
        ext4_file file; int result=ext4_fopen(&file,name,"r");
        CHECK(result==EOK || result==ENOENT);
        if (result==ENOENT) continue;
        uint32_t unused;
        CHECK(!(lazy_flags(fs,group,NULL,&unused)&(EXT4_BLOCK_GROUP_INODE_UNINIT|EXT4_BLOCK_GROUP_ITABLE_ZEROED)));
        CHECK(unused==ext4_get32(&fs->sb,inodes_per_group)-1 && file.fsize<=1);
        struct ext4_inode_ref inode; CHECK(ext4_fs_get_inode_ref(fs,file.inode,&inode)==EOK);
        unsigned size=ext4_get16(&fs->sb,inode_size);
        for (unsigned i=size-16;i<size;i++) CHECK(!((unsigned char *)inode.inode)[i]);
        CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
        if (file.fsize) { unsigned char byte; size_t done; CHECK(ext4_fread(&file,&byte,1,&done)==EOK && done==1 && byte==group); }
        CHECK(ext4_fclose(&file)==EOK);
    }
}
int main(int argc,char **argv)
{
    CHECK((argc==3 || argc==4) && fault_block_open(&disk,argv[1])==0);
    unsigned char scratch[512];
    struct ext4_blockdev_iface iface={.open=dev_open,.close=dev_open,.bread=dev_read,.bwrite=dev_write,.flush=dev_flush,
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
