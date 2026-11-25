/* A second public query overlaps the first checksum calculation. */
#define main metadata_main
#include "lwext4_metadata.c"
#undef main
#include <ext4_dir.h>
#include <ext4_dir_idx.h>
#include <stddef.h>

static ext4_file *second_reader;
static unsigned char *observed, original[4096];
static size_t observed_size;
static uint32_t observed_seed, observed_index;
static bool armed, changed;
static uint32_t directory_reader, nested_inode, nested_mode;
static int nested_status;
static unsigned crc_calls;
static uint32_t capture_extent_bytes;
static const void *captured_extent;
static struct ext4_block_payload *invalidate_tree;
static const void *tree_check_bytes;
static struct ext4_fs *qualify_fs;
static uint32_t qualify_inode;
static struct ext4_inode_ref qualified_writer;
static const void *qualify_bytes;
uint32_t __real_ext4_crc32c_zeroed(uint32_t, const void *, uint32_t, uint32_t, uint32_t);
uint32_t __wrap_ext4_crc32c_zeroed(uint32_t seed, const void *bytes, uint32_t length,
    uint32_t first, uint32_t second)
{
    uint32_t crc = __real_ext4_crc32c_zeroed(seed, bytes, length, first, second);
    if (bytes == qualify_bytes) {
        qualify_bytes = NULL;
        /* The old verifier has computed its checksum but has not published
         * success. Expose write access before it can publish that result. */
        CHECK(ext4_fs_get_inode_ref(qualify_fs, qualify_inode, &qualified_writer) == EOK);
    }
    return crc;
}
uint32_t __real_ext4_crc32c(uint32_t, const void *, uint32_t);
uint32_t __wrap_ext4_crc32c(uint32_t seed, const void *bytes, uint32_t length)
{
    crc_calls++;
    if (length==capture_extent_bytes) captured_extent=bytes;
    if (armed && (directory_reader ? bytes == observed :
        length == sizeof(observed_index) && seed == observed_seed &&
        !memcmp(bytes, &observed_index, sizeof(observed_index)))) {
        armed = false;
        changed = memcmp(observed, original, observed_size) != 0;
        if (directory_reader)
            nested_status = ext4_lookup_child("/", directory_reader, "target", 6,
                &nested_inode, &nested_mode);
        else {
            struct ext4_inode inode;
            nested_status = ext4_fraw_inode_fill(second_reader, &inode);
        }
    }
    uint32_t result=__real_ext4_crc32c(seed, bytes, length);
    if (invalidate_tree && bytes == tree_check_bytes) {
        /* The writer's qualification wins after the calculation and before
         * the reader tries to publish its successful validation. */
        ext4_block_payload_write(invalidate_tree);
        invalidate_tree=NULL;
    }
    return result;
}

static void readonly_directory_queries(struct ext4_fs *fs, struct ext4_blockdev *dev)
{
    uint32_t ino, mode;
    CHECK(ext4_lookup_child("/", EXT4_INODE_ROOT_INDEX, "file", 4, &ino, &mode) == EOK);
    CHECK(ext4_transaction_begin("/") == EOK);
    payload_threshold = dev->lg_bsize;
    payload_allocations = 0;
    unsigned warmed_crc = crc_calls;
    for (unsigned i = 0; i < 8; i++)
        CHECK(ext4_lookup_child("/", EXT4_INODE_ROOT_INDEX, "file", 4, &ino, &mode) == EOK);
    printf("directory read: block=%u lookup-payload-allocations=%u repeated-CRC=%u\n",
        dev->lg_bsize, payload_allocations,crc_calls-warmed_crc);
    CHECK(!payload_allocations && crc_calls == warmed_crc);
    struct ext4_inode_ref directory;
    CHECK(ext4_fs_get_inode_ref_read(fs, EXT4_INODE_ROOT_INDEX, &directory) == EOK);
    payload_allocations = 0;
    struct ext4_dir_iter it;
    CHECK(ext4_dir_iterator_init(&it, &directory, 0) == EOK);
    while (it.curr) CHECK(ext4_dir_iterator_next(&it) == EOK);
    CHECK(ext4_dir_iterator_fini(&it) == EOK);
    ext4_fsblk_t home;
    struct ext4_block block;
    CHECK(ext4_fs_get_inode_dblk_idx(&directory,0,&home,false)==EOK);
    CHECK(ext4_block_get(dev,&block,home)==EOK);
    __atomic_store_n(&block.payload->tree_verified,0,__ATOMIC_RELEASE);
    tree_check_bytes=block.data;
    invalidate_tree=block.payload;
    bool indexed=ext4_inode_has_flag(directory.inode,EXT4_INODE_FLAG_INDEX);
    CHECK(indexed ? ext4_dir_dx_check(&directory,&block)==EOK :
        ext4_dir_block_csum_verify(&directory,&block));
    CHECK(!invalidate_tree && block.payload->tree_verified==UINT64_MAX);
    unsigned before=crc_calls;
    CHECK(indexed ? ext4_dir_dx_check(&directory,&block)==EOK :
        ext4_dir_block_csum_verify(&directory,&block));
    CHECK(crc_calls>before);
    CHECK(ext4_block_set(dev,&block)==EOK);
    CHECK(ext4_fs_put_inode_ref(&directory) == EOK);
    printf("directory read: block=%u iterator-payload-allocations=%u\n",
        dev->lg_bsize, payload_allocations);
    CHECK(!payload_allocations);
    payload_threshold = 0;
    ext4_file child;
    CHECK(ext4_fopen(&child, "/directory-private", "w+") == EOK);
    CHECK(ext4_lookup_child("/", EXT4_INODE_ROOT_INDEX, "directory-private", 17,
        &ino, &mode) == EOK && ino == child.inode);
    CHECK(ext4_fclose(&child) == EOK);
    CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
    CHECK(ext4_lookup_child("/", EXT4_INODE_ROOT_INDEX, "directory-private", 17,
        &ino, &mode) == ENOENT);
    CHECK(ext4_fopen(&child,"/directory-accepted","w+")==EOK);
    CHECK(ext4_fclose(&child)==EOK);
    CHECK(ext4_lookup_child("/",EXT4_INODE_ROOT_INDEX,"directory-accepted",18,&ino,&mode)==EOK);
    CHECK(ext4_fs_get_inode_ref_read(fs,EXT4_INODE_ROOT_INDEX,&directory)==EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&directory,0,&home,false)==EOK);
    CHECK(ext4_block_get(dev,&block,home)==EOK);
    CHECK(block.payload->tree_verified==ext4_fs_tree_check_key(&directory,
        ext4_inode_has_flag(directory.inode,EXT4_INODE_FLAG_INDEX)));
    CHECK(ext4_block_set(dev,&block)==EOK);
    CHECK(ext4_fs_put_inode_ref(&directory)==EOK);
}

static void directory_record_search(struct ext4_sblock *super)
{
    _Alignas(4) unsigned char storage[4097];
    struct ext4_sblock legacy=*super;
    ext4_set32(&legacy,rev_level,0);
    ext4_set16(&legacy,minor_rev_level,0);
    size_t size=ext4_sb_get_block_size(super);
    CHECK(size<=4096);
    for (unsigned offset=0;offset<=1;offset++) {
        unsigned char *data=storage+offset;
        memset(data,0,size);
        struct ext4_block block={.data=data};
        struct ext4_dir_en *empty=(void *)data,*entry=(void *)(data+12);
        ext4_dir_en_set_entry_len(empty,12);
        ext4_dir_en_set_entry_len(entry,size-12);
        ext4_dir_en_set_inode(entry,13);
        empty->name_len=entry->name_len=3;
        memcpy(empty->name,"key",3);memcpy(entry->name,"key",3);
        struct ext4_dir_search_result result={0};
        CHECK(ext4_dir_find_in_block(&block,super,3,"key",&result)==EOK);
        CHECK(result.previous==empty && result.dentry==entry);
        CHECK(ext4_dir_find_in_block(&block,super,3,"bad",&result)==ENOENT);
        entry->in.name_length_high=1;
        CHECK(ext4_dir_find_in_block(&block,super,3,"key",&result)==EOK);
        CHECK(ext4_dir_find_in_block(&block,&legacy,3,"key",&result)==ENOENT);
        entry->in.name_length_high=UINT8_MAX;
        CHECK(ext4_dir_find_in_block(&block,&legacy,3,"key",&result)==EUCLEAN);
        ext4_dir_en_set_entry_len(empty,11);
        CHECK(ext4_dir_find_in_block(&block,super,3,"key",&result)==EUCLEAN);
        ext4_dir_en_set_entry_len(empty,4);
        CHECK(ext4_dir_find_in_block(&block,super,3,"key",&result)==EUCLEAN);
        ext4_dir_en_set_entry_len(empty,(uint16_t)(size+4));
        CHECK(ext4_dir_find_in_block(&block,super,3,"key",&result)==EUCLEAN);
    }
    memset(storage,0,size);
    struct ext4_dir_en *first=(void *)storage,*later=(void *)(storage+12);
    ext4_dir_en_set_entry_len(first,12);
    ext4_dir_en_set_inode(first,13);
    first->name_len=3;
    memcpy(first->name,"key",3);
    struct ext4_block_payload payload;
    ext4_block_payload_init(&payload,storage,NULL);
    struct ext4_block block={.data=storage,.payload=&payload};
    struct ext4_dir_search_result result={0};
    CHECK(ext4_dir_find_in_block(&block,super,3,"key",&result)==EOK);
    CHECK(result.dentry==first && !result.previous && !payload.directory_layout);
    CHECK(ext4_dir_find_in_block(&block,super,3,"bad",&result)==EUCLEAN);
    ext4_dir_en_set_entry_len(later,size-12);
    CHECK(ext4_dir_find_in_block(&block,super,3,"key",&result)==EOK);
    CHECK(payload.directory_layout);
    CHECK(ext4_dir_find_in_block(&block,super,3,"bad",&result)==ENOENT);
    ext4_block_payload_write(&payload);
    ext4_dir_en_set_entry_len(later,0);
    CHECK(ext4_dir_find_in_block(&block,super,3,"bad",&result)==EUCLEAN);
    CHECK(payload.directory_layout==UINT32_MAX);
    puts("PASS directory search: aligned and unaligned records preserve legacy lengths, removed entries and corrupt bounds");
}

/* More records than the old four hash slots share one accepted inode block. */
static void metadata_record_queries(struct ext4_fs *fs)
{
    uint32_t ids[32];
    for(unsigned i=0;i<32;i++) {
        char name[40];snprintf(name,sizeof(name),"/record-%u",i);
        ext4_file file;CHECK(ext4_fopen(&file,name,"w+")==EOK);
        ids[i]=file.inode;CHECK(ext4_fclose(&file)==EOK);
    }
    struct ext4_inode_ref held[32];
    for(unsigned i=0;i<32;i++) CHECK(ext4_fs_get_inode_ref_read(fs,ids[i],&held[i])==EOK);
    for(unsigned round=0;round<2;round++) {
        unsigned warmed=crc_calls;
        for(unsigned i=0;i<32;i++) {
            struct ext4_inode_ref ref;
            CHECK(ext4_fs_get_inode_ref_read(fs,ids[i],&ref)==EOK);
            CHECK(ref.index==ids[i]);
            CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
        }
        if(round) {
            printf("metadata record reuse: inode=%u repeated CRC=%u\n",ext4_get16(&fs->sb,inode_size),crc_calls-warmed);
            CHECK(crc_calls==warmed);
        }
    }
    for(unsigned i=0;i<32;i++) CHECK(ext4_fs_put_inode_ref(&held[i])==EOK);
}

static void extent_node_queries(struct ext4_fs *fs)
{
    ext4_file file;CHECK(ext4_fopen(&file,"/extent-node-checks","w+")==EOK);
    uint32_t size=ext4_sb_get_block_size(&fs->sb);
    for(unsigned i=0;i<6;i++) {
        bool changed=false;
        CHECK(ext4_fallocate(&file,EXT4_FALLOC_KEEP_SIZE,(uint64_t)i*2*size,size,&changed,UINT32_MAX)==EOK && changed);
    }
    struct ext4_inode_ref inode;struct ext4_inode_block_range range;
    CHECK(ext4_fs_get_inode_ref_read(fs,file.inode,&inode)==EOK);
    capture_extent_bytes=size-4;captured_extent=NULL;
    CHECK(ext4_fs_get_inode_dblk_range(&inode,0,1,false,&range)==EOK && range.block);
    capture_extent_bytes=0;CHECK(captured_extent);
    struct ext4_buf *buffer;ext4_fsblk_t home=0;
    for(buffer=fs->bdev->bc->lru_head;buffer;buffer=buffer->lru_next)
        if(buffer->data==captured_extent) {home=buffer->lba;break;}
    CHECK(home);
    struct ext4_block old;CHECK(ext4_block_get(fs->bdev,&old,home)==EOK);
    ext4_fsblk_t physical=range.block;unsigned warmed=crc_calls;
    for(unsigned i=0;i<32;i++) {
        CHECK(ext4_fs_get_inode_dblk_range(&inode,0,1,false,&range)==EOK &&
            range.block==physical && range.unwritten);
    }
    printf("accepted extent node: repeated CRC=%u\n",crc_calls-warmed);
    CHECK(crc_calls==warmed);
    __atomic_store_n(&old.payload->tree_verified,0,__ATOMIC_RELEASE);
    tree_check_bytes=old.data;invalidate_tree=old.payload;
    CHECK(ext4_fs_get_inode_dblk_range(&inode,0,1,false,&range)==EOK);
    CHECK(!invalidate_tree && old.payload->tree_verified==UINT64_MAX);
    warmed=crc_calls;
    CHECK(ext4_fs_get_inode_dblk_range(&inode,0,1,false,&range)==EOK && crc_calls>warmed);
    old.data[size-1]^=1;
    CHECK(ext4_fs_get_inode_dblk_range(&inode,0,1,false,&range)==EUCLEAN);
    old.data[size-1]^=1;
    CHECK(ext4_fs_get_inode_dblk_range(&inode,0,1,false,&range)==EOK);
    unsigned char retained[128];memcpy(retained,old.data,sizeof(retained));
    CHECK(ext4_fs_put_inode_ref(&inode)==EOK);
    bool changed=false;
    CHECK(ext4_fallocate(&file,EXT4_FALLOC_KEEP_SIZE,12*(uint64_t)size,size,&changed,UINT32_MAX)==EOK && changed);
    CHECK(ext4_fs_get_inode_ref_read(fs,file.inode,&inode)==EOK);
    warmed=crc_calls;
    CHECK(ext4_fs_get_inode_dblk_range(&inode,0,1,false,&range)==EOK && range.block==physical);
    CHECK(crc_calls>warmed);
    warmed=crc_calls;
    CHECK(ext4_fs_get_inode_dblk_range(&inode,0,1,false,&range)==EOK && crc_calls==warmed);
    CHECK(!memcmp(retained,old.data,sizeof(retained)));
    CHECK(ext4_block_set(fs->bdev,&old)==EOK);
    CHECK(ext4_fs_put_inode_ref(&inode)==EOK && ext4_fclose(&file)==EOK);
}

static void reuse_queries(struct ext4_fs *fs, struct ext4_blockdev *dev)
{
    ext4_file file;
    uint32_t ino, mode;
    CHECK(ext4_lookup_child("/", EXT4_INODE_ROOT_INDEX, "file", 4, &ino, &mode) == EOK);
    CHECK(ext4_fopen_inode(&file, "/", ino, NULL) == EOK);
    /* On some geometries the test file shares the journal inode's table
     * block. Release that persistent loan to exercise the idle cache path. */
    CHECK(ext4_journal_stop("/") == EOK);
    ext4_bcache_reclaim(dev->bc);
    struct ext4_inode before, after;
    CHECK(ext4_fraw_inode_fill(&file, &before) == EOK);
    struct ext4_inode_ref cached;
    CHECK(ext4_fs_get_inode_ref_read(fs, ino, &cached) == EOK);
    uint32_t offset=(uint8_t *)cached.inode-cached.block.data;
    unsigned record=offset/ext4_get16(&fs->sb,inode_size);
    uint64_t lba=cached.block.lb_id;
    cached.block.payload->verified[record/64]&=~(UINT64_C(1)<<(record%64));
    /* Simulate unverified bytes already present in the cache. */
    cached.block.data[offset]^=1;
    CHECK(ext4_fs_put_inode_ref(&cached) == EOK);
    CHECK(!ext4_fs_copy_cached_inode(fs,ino,&after));
    struct ext4_block repair;
    CHECK(ext4_block_get(dev,&repair,lba) == EOK);
    repair.data[offset]^=1;
    CHECK(ext4_block_set(dev,&repair) == EOK);
    unsigned first_crc=crc_calls;
    CHECK(ext4_fs_copy_cached_inode(fs,ino,&after) && crc_calls>first_crc);
    CHECK(!memcmp(&before,&after,sizeof(before)));
    unsigned warmed = crc_calls;
    CHECK(ext4_fs_copy_cached_inode(fs,ino,&after) && crc_calls==warmed);
    CHECK(ext4_journal_start("/") == EOK);
    warmed = crc_calls;
    for (unsigned i = 0; i < 8; i++) {
        CHECK(ext4_fraw_inode_fill(&file, &after) == EOK);
        CHECK(!memcmp(&before, &after, sizeof(before)));
    }
    printf("checksum reuse: block=%u inode=%u warm-query-crcs=%u\n",
        dev->lg_bsize, ext4_get16(&fs->sb, inode_size), crc_calls - warmed);
    CHECK(crc_calls == warmed);

    struct ext4_inode_ref old;
    CHECK(ext4_fs_get_inode_ref_read(fs, ino, &old) == EOK);
    unsigned char old_bytes[512];
    uint32_t isize = ext4_get16(&fs->sb, inode_size);
    CHECK(isize <= sizeof(old_bytes));
    memcpy(old_bytes, old.inode, isize);
    struct ext4_timestamp times[3] = {{1234, 0}, {2345, 0}, {3456, 0}};
    CHECK(ext4_file_set_times(&file, 7, times) == EOK);
    CHECK(!memcmp(old_bytes, old.inode, isize));
    warmed = crc_calls;
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK);
    CHECK(crc_calls > warmed && to_le32(after.modification_time) == 2345);
    warmed = crc_calls;
    CHECK(ext4_fraw_inode_fill(&file, &before) == EOK && crc_calls == warmed);
    CHECK(ext4_fs_put_inode_ref(&old) == EOK);

    CHECK(ext4_transaction_begin("/") == EOK);
    times[1].seconds = 4567;
    CHECK(ext4_file_set_times(&file, 2, times) == EOK);
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK && to_le32(after.modification_time) == 4567);
    warmed = crc_calls;
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK && crc_calls > warmed);
    CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK && !memcmp(&before, &after, sizeof(after)));

    /* A new read version starts uncached. Force an overlapping writer after
     * this version's checksum, then ensure no late success was remembered. */
    CHECK(ext4_file_set_times(&file, 2, times) == EOK);
    CHECK(ext4_fs_get_inode_ref_read(fs, ino, &old) == EOK);
    struct ext4_timestamp newer[3] = {{0}, {5678, 0}, {0}};
    CHECK(ext4_file_set_times(&file, 2, newer) == EOK);
    /* The address is obtained from the existing accepted owner, without
     * invoking another metadata verifier before the instrumented query. */
    struct ext4_block current_block;
    CHECK(ext4_block_get(dev, &current_block, old.block.lb_id) == EOK);
    qualify_bytes = current_block.data + ((uint8_t *)old.inode - old.block.data);
    qualify_fs = fs; qualify_inode = ino;
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK && !qualify_bytes);
    CHECK(ext4_fs_put_inode_ref(&old) == EOK && ext4_block_set(dev, &current_block) == EOK);
    warmed = crc_calls;
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK && crc_calls > warmed);
    uint16_t mode_before = qualified_writer.inode->mode;
    qualified_writer.inode->mode ^= 1;
    CHECK(ext4_fraw_inode_fill(&file, &after) == EUCLEAN);
    qualified_writer.inode->mode = mode_before;
    CHECK(ext4_fs_put_inode_ref(&qualified_writer) == EOK);
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK && to_le32(after.modification_time) == 5678);

    uint32_t bg = (ino - 1) / ext4_get32(&fs->sb, inodes_per_group);
    struct ext4_block_group_ref group;
    CHECK(ext4_fs_get_block_group_ref_write(fs, bg, &group) == EOK);
    uint16_t checksum = group.block_group->checksum;
    group.block_group->checksum ^= 1;
    CHECK(ext4_fraw_inode_fill(&file, &after) == EUCLEAN);
    group.block_group->checksum = checksum;
    CHECK(ext4_fs_put_block_group_ref(&group) == EOK);
    CHECK(ext4_fraw_inode_fill(&file, &after) == EOK);
    printf("checksum versions: accepted/abort, old reader, late verifier, inode/GDT corruption rejected\n");
    CHECK(ext4_fclose(&file) == EOK);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    if (!strcmp(argv[2], "seed")) return metadata_main(argc, argv);
    bool gdt = !strcmp(argv[2], "gdt");
    bool dx = !strcmp(argv[2], "dx");
    bool reuse = !strcmp(argv[2], "reuse");
    bool legacy_gdt = !strcmp(argv[2], "gdt16");
    bool records = !strcmp(argv[2], "records");
    bool extent = !strcmp(argv[2], "extent");
    bool read_directory = !strcmp(argv[2], "directory-read");
    CHECK(gdt || legacy_gdt || dx || reuse || records || extent || read_directory || !strcmp(argv[2], "inode"));
    CHECK(fault_block_open(&disk, argv[1]) == 0);
    unsigned char scratch[512];
    struct ext4_blockdev_iface iface = {.open=dev_open, .close=dev_open,
        .bread=dev_read, .bwrite=dev_write, .flush=dev_flush, .ph_bsize=512,
        .ph_bcnt=disk.device.capacity_bytes/512, .ph_bbuf=scratch};
    struct ext4_blockdev dev = {.bdif=&iface, .part_size=disk.device.capacity_bytes};
    CHECK(ext4_device_register(&dev, "metadata") == EOK);
    CHECK(ext4_mount("metadata", "/", false) == EOK);
    CHECK(ext4_recover("/") == EOK && ext4_journal_start("/") == EOK);
    struct ext4_fs *fs = dev.fs;
    if (legacy_gdt) {
        CHECK(!ext4_sb_feature_ro_com(&fs->sb,EXT4_FRO_COM_METADATA_CSUM) &&
              ext4_sb_feature_ro_com(&fs->sb,EXT4_FRO_COM_GDT_CSUM));
        struct ext4_block_group_ref writable,read;
        CHECK(ext4_fs_get_block_group_ref_write(fs,0,&writable)==EOK);
        uint16_t original=writable.block_group->checksum;
        writable.block_group->checksum^=1;
        CHECK(ext4_fs_get_block_group_ref(fs,0,&read)==EUCLEAN);
        writable.block_group->checksum=original;
        CHECK(ext4_fs_put_block_group_ref(&writable)==EOK);
        CHECK(ext4_fs_get_block_group_ref(fs,0,&read)==EOK && ext4_fs_put_block_group_ref(&read)==EOK);
        CHECK(ext4_journal_stop("/")==EOK && ext4_umount("/")==EOK && !live_allocations);
        fault_block_close(&disk);puts("PASS legacy GDT_CSUM: corrupt descriptor rejected, restored descriptor accepted");
        return 0;
    }
    CHECK(ext4_sb_feature_ro_com(&fs->sb, EXT4_FRO_COM_METADATA_CSUM));
    if (reuse || read_directory || records || extent) {
        if (extent) extent_node_queries(fs);
        else
        if (records) { directory_record_search(&fs->sb);metadata_record_queries(fs); }
        else
        if (reuse) reuse_queries(fs, &dev);
        else readonly_directory_queries(fs, &dev);
        CHECK(ext4_journal_stop("/") == EOK);
        CHECK(ext4_umount("/") == EOK && !live_allocations);
        fault_block_close(&disk);
        return 0;
    }
    if (dx) {
        CHECK(ext4_dir_mk("/checksum-directory") == EOK);
        ext4_file child;
        CHECK(ext4_fopen(&child, "/checksum-directory/target", "w+") == EOK);
        ext4_dir directory;
        CHECK(ext4_dir_open(&directory, "/checksum-directory") == EOK);
        directory_reader = directory.f.inode;
        struct ext4_inode_ref ref;
        CHECK(ext4_fs_get_inode_ref(fs, directory_reader, &ref) == EOK);
        CHECK(ext4_inode_get_flags(ref.inode) & EXT4_INODE_FLAG_INDEX);
        ext4_fsblk_t address;
        CHECK(ext4_fs_get_inode_dblk_idx(&ref, 0, &address, false) == EOK);
        struct ext4_block index;
        CHECK(ext4_block_get(&dev, &index, address) == EOK);
        /* Model the first read of this byte version, even if earlier
         * directory setup already warmed its validation result. */
        __atomic_store_n(&index.payload->tree_verified,0,__ATOMIC_RELEASE);
        observed = index.data; observed_size = dev.lg_bsize;
        CHECK(observed_size <= sizeof(original));
        memcpy(original, observed, observed_size);
        struct ext4_dir_idx_climit *limits = (void *)((struct ext4_dir_idx_root *)observed)->en;
        size_t tail = 32 + (size_t)to_le16(limits->limit) * sizeof(struct ext4_dir_idx_entry);
        CHECK(tail + sizeof(struct ext4_dir_idx_tail) <= observed_size);
        CHECK(((struct ext4_dir_idx_tail *)(observed + tail))->checksum);
        uint32_t outer_inode = 0, outer_mode = 0;
        armed = true;
        int outer = ext4_lookup_child("/", directory_reader, "target", 6,
            &outer_inode, &outer_mode);
        printf("checksum read dx: block=%u inode=%u outer=%d second=%d modified=%d\n",
            dev.lg_bsize, ext4_get16(&fs->sb, inode_size), outer, nested_status, changed);
        CHECK(!armed && outer == EOK && nested_status == EOK && !changed);
        CHECK(outer_inode == child.inode && nested_inode == child.inode &&
              outer_mode == nested_mode && (outer_mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_FILE);
        CHECK(!memcmp(original, observed, observed_size));
        /* The immutable verification must still reject an actual bad tail. */
        ext4_block_payload_write(index.payload);
        observed[tail + offsetof(struct ext4_dir_idx_tail, checksum)] ^= 1;
        int corrupt = ext4_lookup_child("/", directory_reader, "target", 6,
            &outer_inode, &outer_mode);
        memcpy(observed, original, observed_size);
        printf("checksum read dx corruption: result=%d\n", corrupt);
        CHECK(corrupt == EUCLEAN);
        CHECK(ext4_lookup_child("/", directory_reader, "target", 6,
            &outer_inode, &outer_mode) == EOK && outer_inode == child.inode);
        directory_reader = 0;
        CHECK(ext4_block_set(&dev, &index) == EOK);
        CHECK(ext4_fs_put_inode_ref(&ref) == EOK);
        CHECK(ext4_dir_close(&directory) == EOK && ext4_fclose(&child) == EOK);
        CHECK(ext4_journal_stop("/") == EOK);
        CHECK(ext4_umount("/") == EOK && !live_allocations);
        fault_block_close(&disk);
        return 0;
    }
    for (unsigned i = 0; i < 32; i++) {
        char name[32]; ext4_file padding;
        snprintf(name, sizeof(name), "/checksum-pad-%u", i);
        CHECK(ext4_fopen(&padding, name, "w+") == EOK && ext4_fclose(&padding) == EOK);
    }
    ext4_file f;
    CHECK(ext4_fopen(&f, "/checksum-target", "w+") == EOK);
    struct ext4_block_group_ref group;
    uint32_t bg = (f.inode - 1) / ext4_get32(&fs->sb, inodes_per_group);
    /* An outstanding writable loan requires real verification on every
     * query, including the intentionally overlapping checksum below. */
    CHECK(ext4_fs_get_block_group_ref_write(fs, bg, &group) == EOK);
    struct ext4_inode_ref inode;
    CHECK(ext4_fs_get_inode_ref(fs, f.inode, &inode) == EOK);
    CHECK(ext4_current_trans(fs) == NULL);
    observed = gdt ? (void *)group.block_group : (void *)inode.inode;
    observed_size = gdt ? ext4_sb_get_desc_size(&fs->sb) : ext4_get16(&fs->sb, inode_size);
    CHECK(observed_size <= sizeof(original));
    memcpy(original, observed, observed_size);
    observed_seed = ext4_sb_get_csum_seed(&fs->sb);
    observed_index = to_le32(gdt ? bg : f.inode);
    second_reader = &f;
    armed = true;
    struct ext4_inode actual;
    memset(&actual, 0xa5, sizeof(actual));
    int outer = ext4_fraw_inode_fill(&f, &actual);
    printf("checksum read %s: block=%u inode=%u outer=%d second=%d modified=%d\n",
        argv[2], dev.lg_bsize, ext4_get16(&fs->sb, inode_size), outer, nested_status, changed);
    CHECK(!armed && outer == EOK && nested_status == EOK && !changed);
    if (!gdt) CHECK(!memcmp(&actual, original, observed_size < sizeof(actual) ? observed_size : sizeof(actual)));
    size_t inode_size=ext4_get16(&fs->sb, inode_size);
    for (size_t i=inode_size;i<sizeof(actual);i++) CHECK(!((unsigned char *)&actual)[i]);
    CHECK(!memcmp(original, observed, observed_size));
    CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
    CHECK(ext4_fs_put_block_group_ref(&group) == EOK);
    CHECK(ext4_fclose(&f) == EOK);
    CHECK(ext4_journal_stop("/") == EOK);
    CHECK(ext4_umount("/") == EOK && !live_allocations);
    fault_block_close(&disk);
    return 0;
}
