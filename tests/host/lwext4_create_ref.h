#ifndef BOAROS_TEST_LWEXT4_CREATE_REF_H
#define BOAROS_TEST_LWEXT4_CREATE_REF_H

#include <ext4_dir.h>

static struct {
    bool active, fail_after_alloc, fail_after_link;
    const char *name;
    unsigned count, gets, puts, searches;
    uint32_t inodes[8];
} create_ref;

static struct {
    bool active;
    uint32_t parent, child;
    unsigned parent_gets, child_gets, parent_puts, child_puts;
    unsigned searches, writable_searches;
} unlink_ref;

static bool create_ref_inode(uint32_t inode)
{
    for (unsigned i = 0; i < create_ref.count; i++)
        if (create_ref.inodes[i] == inode) return true;
    return false;
}

int __real_ext4_fs_alloc_inode(struct ext4_fs *, struct ext4_inode_ref *, int,
    uint32_t, const char *, uint32_t);
int __wrap_ext4_fs_alloc_inode(struct ext4_fs *fs, struct ext4_inode_ref *ref,
    int type, uint32_t parent, const char *name, uint32_t length)
{
    int result = __real_ext4_fs_alloc_inode(fs, ref, type, parent, name, length);
    if (create_ref.active && !result) {
        CHECK(create_ref.count < 8);
        create_ref.inodes[create_ref.count++] = ref->index;
        if (create_ref.fail_after_alloc) {
            create_ref.fail_after_alloc = false;
            fail_allocation = allocations + 1;
        }
    }
    return result;
}

int __real_ext4_fs_get_inode_ref(struct ext4_fs *, uint32_t, struct ext4_inode_ref *);
int __wrap_ext4_fs_get_inode_ref(struct ext4_fs *fs, uint32_t inode, struct ext4_inode_ref *ref)
{
    if (create_ref.active && create_ref_inode(inode)) create_ref.gets++;
    if (unlink_ref.active) {
        if (inode == unlink_ref.parent) unlink_ref.parent_gets++;
        if (inode == unlink_ref.child) unlink_ref.child_gets++;
    }
    return __real_ext4_fs_get_inode_ref(fs, inode, ref);
}

int __real_ext4_fs_put_inode_ref(struct ext4_inode_ref *);
int __wrap_ext4_fs_put_inode_ref(struct ext4_inode_ref *ref)
{
    if (create_ref.active && create_ref_inode(ref->index)) create_ref.puts++;
    if (unlink_ref.active) {
        if (ref->index == unlink_ref.parent) unlink_ref.parent_puts++;
        if (ref->index == unlink_ref.child) unlink_ref.child_puts++;
    }
    return __real_ext4_fs_put_inode_ref(ref);
}

int __real_ext4_dir_find_entry(struct ext4_dir_search_result *, struct ext4_inode_ref *,
    const char *, uint32_t, bool);
int __wrap_ext4_dir_find_entry(struct ext4_dir_search_result *result, struct ext4_inode_ref *parent,
    const char *name, uint32_t length, bool write)
{
    if (create_ref.active && strlen(create_ref.name) == length && !memcmp(create_ref.name, name, length))
        create_ref.searches++;
    if (unlink_ref.active && parent->index == unlink_ref.parent) {
        unlink_ref.searches++;
        if (write) unlink_ref.writable_searches++;
    }
    return __real_ext4_dir_find_entry(result, parent, name, length, write);
}

int __real_ext4_dir_add_entry(struct ext4_inode_ref *, const char *, uint32_t, struct ext4_inode_ref *);
int __wrap_ext4_dir_add_entry(struct ext4_inode_ref *parent, const char *name, uint32_t length,
    struct ext4_inode_ref *child)
{
    int result = __real_ext4_dir_add_entry(parent, name, length, child);
    if (!result && create_ref.active && create_ref.fail_after_link && create_ref_inode(child->index)) {
        create_ref.fail_after_link = false;
        fail_allocation = allocations + 1;
    }
    return result;
}

static void create_ref_begin(const char *name)
{ create_ref = (__typeof__(create_ref)){.active = true, .name = name}; }

static void create_ref_check(unsigned count)
{
    create_ref.active = false;
    CHECK(create_ref.count == count && create_ref.gets == 0 && create_ref.puts == count);
    CHECK(create_ref.searches == 1);
}

static uint32_t create_ref_seconds;
static bool create_ref_clock(struct ext4_timestamp *now)
{
    *now = (struct ext4_timestamp){++create_ref_seconds, 41};
    return true;
}

static void create_ref_test(struct ext4_fs *fs)
{
    ext4_file parent;
    CHECK(ext4_create_child("/", EXT4_INODE_ROOT_INDEX, "create-ref-parent", 0042770, 0, NULL, &parent, NULL,0,0) == EOK);
    CHECK(ext4_file_set_owner(&parent, UINT32_MAX, 17, UINT32_MAX) == EOK);
    CHECK(ext4_file_set_mode(&parent, 02770) == EOK);
    create_ref_seconds = 810;
    CHECK(ext4_mount_setup_clock("/", create_ref_clock) == EOK);
    char long_target[96]; memset(long_target, 't', sizeof(long_target) - 1); long_target[sizeof(long_target) - 1] = 0;
    struct {
        const char *name, *target;
        uint32_t mode, expected, device;
    } cases[] = {
        {"new-file",NULL,0100640,0100640,0},
        {"new-directory",NULL,0040750,0042750,0},
        {"new-fifo",NULL,0010620,0010620,0},
        {"new-char",NULL,0020600,0020600,0x1234},
        {"new-block",NULL,0060640,0060640,0x5678},
        {"new-short-link","new-file",0120777,0120777,0},
        {"new-long-link",long_target,0120777,0120777,0},
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        ext4_file file = {0};
        struct ext4_inode initial;
        memset(&initial, 0xa5, sizeof(initial));
        uint32_t created_at = create_ref_seconds + 1;
        create_ref_begin(cases[i].name);
        CHECK(ext4_create_child("/", parent.inode, cases[i].name, cases[i].mode,
            cases[i].device, cases[i].target, &file, &initial,0,17) == EOK);
        create_ref_check(1);
        CHECK(file.inode == create_ref.inodes[0] && file.mp && !file.fpos);
        struct ext4_inode inode;
        CHECK(ext4_fraw_inode_fill(&file, &inode) == EOK);
        /* 初始属性不是磁盘校验镜像；其余字段必须与独立重读逐字节一致。 */
        ext4_inode_set_csum(&fs->sb, &initial, 0);
        ext4_inode_set_csum(&fs->sb, &inode, 0);
        CHECK(!memcmp(&initial, &inode, sizeof(initial)));
        if (ext4_get16(&fs->sb, inode_size) > EXT4_GOOD_OLD_INODE_SIZE)
            CHECK((to_le32(initial.atime_extra) >> 2) == 41 &&
                (to_le32(initial.mtime_extra) >> 2) == 41 &&
                (to_le32(initial.ctime_extra) >> 2) == 41);
        CHECK(ext4_inode_get_mode(&fs->sb, &inode) == cases[i].expected && ext4_inode_get_gid(&inode) == 17);
        CHECK(ext4_inode_get_access_time(&inode) == created_at &&
            ext4_inode_get_modif_time(&inode) == created_at && ext4_inode_get_change_inode_time(&inode) == created_at);
        if (cases[i].target) {
            char bytes[128]; size_t length;
            CHECK(ext4_fread(&file, bytes, sizeof(bytes), &length) == EOK);
            CHECK(length == strlen(cases[i].target) && !memcmp(bytes, cases[i].target, length));
        } else if (cases[i].device) CHECK(ext4_inode_get_dev(&inode) == cases[i].device);
        if ((cases[i].mode & 0170000) == 0040000) {
            struct ext4_inode_ref directory; struct ext4_block block;
            ext4_fsblk_t address;
            CHECK(ext4_fs_get_inode_ref_read(fs, file.inode, &directory) == EOK);
            CHECK(ext4_fs_get_inode_dblk_idx(&directory, 0, &address, false) == EOK);
            CHECK(ext4_block_get(fs->bdev, &block, address) == EOK);
            CHECK(ext4_dir_en_get_inode((void *)block.data) == file.inode);
            CHECK(ext4_dir_en_get_inode((void *)(block.data + 12)) == parent.inode);
            CHECK(ext4_block_set(fs->bdev, &block) == EOK && ext4_fs_put_inode_ref(&directory) == EOK);
        }
        CHECK(ext4_fclose(&file) == EOK);
        file = (ext4_file){0};
        CHECK(ext4_create_child("/", parent.inode, cases[i].name, cases[i].mode,
            cases[i].device, cases[i].target, &file, &initial,0,17) == EEXIST && !file.mp);
    }
    /* The raw library also creates intermediate directories. Each newly
     * allocated reference must become the next parent rather than be reopened. */
    create_ref_begin("nested-file");
    ext4_file nested;
    CHECK(ext4_fopen2(&nested, "/create-ref-a/create-ref-b/nested-file", O_CREAT | O_EXCL | O_RDWR) == EOK);
    create_ref_check(3);
    CHECK(nested.inode == create_ref.inodes[2] && ext4_fclose(&nested) == EOK);
    for (unsigned stage = 0; stage < 2; stage++) {
        const char *name = stage ? "create-ref-target-failure" : "create-ref-link-failure";
        uint64_t blocks = ext4_sb_get_free_blocks_cnt(&fs->sb);
        uint32_t inodes = ext4_get32(&fs->sb, free_inodes_count);
        ext4_file file = {0};
        struct ext4_inode initial;
        create_ref_begin(name);
        create_ref.fail_after_alloc = !stage; create_ref.fail_after_link = stage;
        CHECK(ext4_create_child("/", parent.inode, name, stage ? 0120777 : 0100640,
            0, stage ? long_target : NULL, &file, &initial,0,17) == ENOMEM);
        create_ref_check(1); fail_allocation = 0;
        CHECK(!file.mp && !fs->active_operations && !ext4_current_operation(fs));
        uint32_t found, mode;
        CHECK(ext4_lookup_child("/", parent.inode, name, strlen(name), &found, &mode) == ENOENT);
        CHECK(ext4_sb_get_free_blocks_cnt(&fs->sb) == blocks &&
            ext4_get32(&fs->sb, free_inodes_count) == inodes && !jbd_journal_error(fs->jbd_journal));
    }
    uint64_t blocks = ext4_fs_available_blocks(fs);
    uint32_t inodes = ext4_fs_available_inodes(fs);
    struct ext4_inode provisional;
    CHECK(ext4_transaction_begin("/") == EOK);
    CHECK(ext4_create_child("/", parent.inode, "create-initial-abort", 0100640,
        0, NULL, NULL, &provisional,0,17) == EOK);
    CHECK(ext4_inode_get_gid(&provisional) == 17 &&
        ext4_inode_get_mode(&fs->sb, &provisional) == 0100640);
    CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
    uint32_t found, mode;
    CHECK(ext4_lookup_child("/", parent.inode, "create-initial-abort", 20, &found, &mode) == ENOENT);
    CHECK(ext4_fs_available_blocks(fs) == blocks && ext4_fs_available_inodes(fs) == inodes &&
        !fs->active_operations && !ext4_current_operation(fs));
    const char qualified[] = "qualified-create";
    ext4_file file = {0};
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        CHECK(ext4_file_lookup_child(&parent, qualified, sizeof(qualified)-1,
            &found, NULL, NULL, NULL, NULL) == ENOENT);
        CHECK(ext4_transaction_begin("/") == EOK);
        create_ref_begin(qualified);
        CHECK(ext4_create_regular_missing(&parent, qualified, 0640, 53, 17, &file, &provisional) == EOK);
        create_ref.active = false;
        CHECK(!create_ref.searches && create_ref.count == 1 && !create_ref.gets && create_ref.puts == 1);
        CHECK(ext4_inode_get_uid(&provisional) == 53 && ext4_inode_get_gid(&provisional) == 17);
        CHECK(ext4_inode_get_mode(&fs->sb, &provisional) == 0100640);
        if (!attempt) {
            CHECK(ext4_transaction_abort("/", ECANCELED) == ECANCELED);
            CHECK(ext4_fs_available_blocks(fs) == blocks && ext4_fs_available_inodes(fs) == inodes);
        } else {
            CHECK(ext4_transaction_end("/") == EOK);
            CHECK(ext4_file_lookup_child(&parent, qualified, sizeof(qualified)-1,
                &found, NULL, NULL, NULL, NULL) == EOK && found == file.inode);
        }
        CHECK(ext4_fclose(&file) == EOK);
    }
    CHECK(ext4_fclose(&parent) == EOK);
    CHECK(ext4_mount_setup_clock("/", NULL) == EOK);
    puts("PASS creation returns actual initial attributes without reopening; types/extra times/inheritance, failed file handles, outer abort and real OOM return owners");
}

static const char nojournal_target[] =
    "long-symlink-target-with-its-own-data-block-and-a-detectable-write-payload";
static uint64_t nojournal_dir_sector;
static unsigned nojournal_symlink_fault;

static int nojournal_symlink_write(struct ext4_blockdev *dev, const void *bytes,
                                  uint64_t sector, uint32_t count)
{
    bool directory = sector == nojournal_dir_sector;
    bool target = count * 512U >= sizeof(nojournal_target) &&
        !memcmp(bytes, nojournal_target, sizeof(nojournal_target) - 1);
    unsigned fault = nojournal_symlink_fault;
    if ((fault == 1 && target) || (fault >= 2 && directory)) {
        nojournal_symlink_fault = 0;
        if (fault == 3) CHECK(dev_write(dev, bytes, sector, count) == EOK);
        return EIO;
    }
    return dev_write(dev, bytes, sector, count);
}

static bool nojournal_symlink_fault_test(struct ext4_fs *fs, unsigned fault)
{
    const char name[] = "ordered-link";
    CHECK(!fs->jbd_journal && fault >= 1 && fault <= 3);
    struct ext4_inode_ref root;
    ext4_fsblk_t block;
    CHECK(ext4_fs_get_inode_ref_read(fs, EXT4_INODE_ROOT_INDEX, &root) == EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&root, 0, &block, false) == EOK);
    CHECK(ext4_fs_put_inode_ref(&root) == EOK);
    nojournal_dir_sector = block * fs->bdev->lg_bsize / 512U;

    /* Persist the dirty mount marker before exposing the simulated failure. */
    CHECK(ext4_blockdev_flush(fs->bdev) == EOK);
    uint64_t blocks = ext4_fs_available_blocks(fs);
    uint32_t inodes = ext4_fs_available_inodes(fs);
    ext4_file file = {0};
    create_ref_begin(name);
    nojournal_symlink_fault = fault;
    fs->bdev->bdif->bwrite = nojournal_symlink_write;
    int error = ext4_create_child("/", EXT4_INODE_ROOT_INDEX, name, 0120777,
        0, nojournal_target, &file, NULL, 70001, 80002);
    fs->bdev->bdif->bwrite = dev_write;
    create_ref.active = false;
    CHECK(error == EIO && !file.mp && create_ref.count == 1 && !nojournal_symlink_fault);
    CHECK(!ext4_current_operation(fs) && !fs->active_operations);
    if (fault == 1) {
        uint32_t found, mode;
        CHECK(!fs->nojournal_error);
        CHECK(ext4_lookup_child("/", EXT4_INODE_ROOT_INDEX, name, strlen(name),
            &found, &mode) == ENOENT);
        CHECK(ext4_fs_available_blocks(fs) == blocks &&
            ext4_fs_available_inodes(fs) == inodes);
        CHECK(ext4_create_child("/", EXT4_INODE_ROOT_INDEX, name, 0120777,
            0, nojournal_target, &file, NULL, 70001, 80002) == EOK);
        CHECK(ext4_fclose(&file) == EOK && ext4_fremove("/ordered-link") == EOK);
        CHECK(ext4_fs_available_blocks(fs) == blocks &&
            ext4_fs_available_inodes(fs) == inodes);
        puts("PASS nojournal symlink target EIO: unpublished inode and data block returned, retry succeeds");
        return false;
    }

    CHECK(fs->nojournal_error == EIO && ext4_fs_available_blocks(fs) == blocks - 1 &&
        ext4_fs_available_inodes(fs) == inodes - 1);
    uint64_t writes = disk.writes;
    CHECK(ext4_create_child("/", EXT4_INODE_ROOT_INDEX, "no-reuse", 0120777,
        0, "target", &file, NULL, 0, 0) == EIO);
    CHECK(ext4_remount("/", true) == EIO && ext4_umount("/") == EIO && disk.writes == writes);

    /* Persist exactly the writes the failed create had already submitted. */
    CHECK(kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK);
    CHECK(fault_block_crash(&disk) == 0);
    struct ext4_sblock sb;
    CHECK(ext4_sb_read(fs->bdev, &sb) == EOK);
    CHECK(!(ext4_get16(&sb, state) & EXT4_SUPERBLOCK_STATE_VALID_FS));
    printf("PASS nojournal symlink directory EIO: err=%u inode=%u retained for fsck\n",
           fault, create_ref.inodes[0]);
    return true;
}

static void unlink_ref_test(struct ext4_fs *fs)
{
    ext4_file parent;
    CHECK(ext4_create_child("/", EXT4_INODE_ROOT_INDEX, "unlink-ref-parent", 0040750, 0, NULL, &parent, NULL,0,0) == EOK);
    unsigned cases = fs->jbd_journal ? 4 : 2;
    for (unsigned i = 0; i < cases; i++) {
        char name[32], path[96];
        snprintf(name, sizeof(name), "victim-%u", i);
        snprintf(path, sizeof(path), "/unlink-ref-parent/%s", name);
        ext4_file file;
        CHECK(ext4_create_child("/", parent.inode, name, 0100640, 0, NULL, &file, NULL,0,0) == EOK);
        size_t written;
        CHECK(ext4_fwrite(&file, "old", 3, &written) == EOK && written == 3);
        bool outer = i >= 2;
        if (outer) CHECK(ext4_transaction_begin("/") == EOK);
        unlink_ref = (__typeof__(unlink_ref)){.active = true, .parent = parent.inode, .child = file.inode};
        uint32_t inode = 0; bool orphan = false;
        int result = (i & 1) ? ext4_funlink_dentry(path, &inode, &orphan) :
            ext4_unlink_child("/", parent.inode, name, false, &inode, &orphan);
        unlink_ref.active = false;
        CHECK(result == EOK && inode == file.inode && orphan);
        if (unlink_ref.parent_gets != 1 || unlink_ref.child_gets != 1 ||
            unlink_ref.parent_puts != 1 || unlink_ref.child_puts != 1)
            fprintf(stderr, "unlink actual refs case=%u: parent get/put=%u/%u child=%u/%u\n", i,
                unlink_ref.parent_gets, unlink_ref.parent_puts, unlink_ref.child_gets, unlink_ref.child_puts);
        CHECK(unlink_ref.parent_gets == 1 && unlink_ref.child_gets == 1 &&
            unlink_ref.parent_puts == 1 && unlink_ref.child_puts == 1);
        if (outer && !(i & 1))
            CHECK(unlink_ref.searches == 1 && unlink_ref.writable_searches == 1);
        if (outer) CHECK(ext4_transaction_end("/") == EOK);
        struct ext4_inode raw;
        CHECK(ext4_fraw_inode_fill(&file, &raw) == EOK && !ext4_inode_get_links_cnt(&raw));
        CHECK(ext4_fseek(&file, 0, SEEK_SET) == EOK);
        char bytes[3]; size_t got;
        CHECK(ext4_fread(&file, bytes, sizeof(bytes), &got) == EOK && got == 3 && !memcmp(bytes, "old", 3));
        CHECK(ext4_fclose(&file) == EOK && ext4_orphan_free("/", inode) == EOK);
    }
    ext4_file dir;
    CHECK(ext4_create_child("/", parent.inode, "directory", 0040750, 0, NULL, &dir, NULL,0,0) == EOK);
    uint32_t inode = 0, found, mode; bool orphan = false;
    CHECK(ext4_unlink_child("/", parent.inode, "directory", false, &inode, &orphan) == EISDIR);
    CHECK(ext4_create_child("/", dir.inode, "member", 0100600, 0, NULL, NULL, NULL,0,0) == EOK);
    CHECK(ext4_unlink_child("/", parent.inode, "directory", true, &inode, &orphan) == ENOTEMPTY);
    CHECK(ext4_funlink_dentry("/unlink-ref-parent/directory/member", &inode, &orphan) == EOK && orphan);
    CHECK(ext4_orphan_free("/", inode) == EOK);
    CHECK(ext4_fdir_unlink_dentry("/unlink-ref-parent/directory", &inode) == EOK && inode == dir.inode);
    CHECK(ext4_fclose(&dir) == EOK && ext4_orphan_free("/", inode) == EOK);
    CHECK(ext4_lookup_child("/", parent.inode, "directory", 9, &found, &mode) == ENOENT);
    if (fs->jbd_journal) {
        ext4_file victim;
        CHECK(ext4_create_child("/", parent.inode, "oom-victim", 0100640, 0, NULL, &victim, NULL,0,0) == EOK);
        unsigned attempts = 0;
        for (unsigned point = 0; point <= attempts; point++) {
            CHECK(ext4_transaction_begin("/") == EOK);
            allocations = 0; fail_allocation = point;
            int result = ext4_unlink_child("/", parent.inode, "oom-victim", false, &inode, &orphan);
            unsigned used = allocations; fail_allocation = 0;
            CHECK(result == EOK || result == ENOMEM);
            CHECK(ext4_transaction_abort("/", ECANCELED) != EOK);
            if (!point) attempts = used;
            CHECK(!fs->active_operations && !ext4_current_operation(fs));
            CHECK(ext4_lookup_child("/", parent.inode, "oom-victim", 10, &found, &mode) == EOK && found == victim.inode);
        }
        CHECK(ext4_funlink_dentry("/unlink-ref-parent/oom-victim", &inode, &orphan) == EOK && orphan);
        CHECK(ext4_fclose(&victim) == EOK && ext4_orphan_free("/", inode) == EOK);
        printf("PASS unlink OOM prefixes=%u restore name, inode and operation resources\n", attempts);
    }
    CHECK(ext4_unlink_child("/", parent.inode, "absent", false, &inode, &orphan) == ENOENT);
    CHECK(ext4_fclose(&parent) == EOK);
    puts("PASS unlink retains one parent/child ref through fullpath and parent/name lookup, bare/outer transactions, directory checks and open-file contents");
}

#endif
