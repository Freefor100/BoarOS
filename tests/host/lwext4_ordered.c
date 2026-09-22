/* Ordered-data transactions over actual volatile sectors, not a mock journal. */
#include "block_fault.h"
#include <ext4.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_journal.h>
#include <ext4_trans.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_super.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static struct fault_block disk;
static uint64_t data_offset;
static int trace, log_writes;
static unsigned lifecycle, event, cut, reorder;
static void boundary(void)
{
    if (!lifecycle || ++event != cut) return;
    if (reorder && disk.count) CHECK(fault_block_persist(&disk,disk.count-1) == 0);
    CHECK(fault_block_crash(&disk) == 0);
    _Exit(75);
}
static int open_device(struct ext4_blockdev *d) { (void)d; return EOK; }
static int read_device(struct ext4_blockdev *d, void *p, uint64_t n, uint32_t c)
{ (void)d; return kernel_block_read_at(&disk.device,n*512,p,(size_t)c*512) == 0 ? EOK : EIO; }
static int write_device(struct ext4_blockdev *d, const void *p, uint64_t n, uint32_t c)
{
    (void)d;
    if (trace && c >= 2 && n*512 != data_offset) {
        const struct jbd_bhdr *hdr = p;
        if (to_be32(hdr->magic) == JBD_MAGIC_NUMBER &&
            (to_be32(hdr->blocktype) == JBD_DESCRIPTOR_BLOCK ||
             to_be32(hdr->blocktype) == JBD_COMMIT_BLOCK)) {
            unsigned char stable;
            CHECK(pread(disk.fd,&stable,1,(off_t)data_offset) == 1);
            CHECK(stable == 'D'); /* Data is stable before any metadata log. */
            log_writes++;
        }
        const unsigned char *s = p;
        bool all_data = true;
        for (size_t i=0; i<(size_t)c*512; i++) if (s[i] != 'D') all_data=false;
        CHECK(!all_data); /* File payload must never be copied into the log. */
    }
    int r=kernel_block_write_at(&disk.device,n*512,p,(size_t)c*512) == 0 ? EOK : EIO;
    boundary();
    return r;
}
static int flush_device(struct ext4_blockdev *d)
{ (void)d; int r=kernel_block_flush(&disk.device) == 0 ? EOK : EIO; boundary(); return r; }
int main(int argc, char **argv)
{
    CHECK(argc >= 3);
    CHECK(fault_block_open(&disk,argv[1]) == 0);
    unsigned char physical[512];
    struct ext4_blockdev_iface iface = { .open=open_device,.close=open_device,
        .bread=read_device,.bwrite=write_device,.flush=flush_device,
        .ph_bsize=512,.ph_bcnt=disk.device.capacity_bytes/512,.ph_bbuf=physical };
    struct ext4_blockdev dev = { .bdif=&iface,.part_size=disk.device.capacity_bytes };
    CHECK(ext4_device_register(&dev,"ordered") == EOK);
    CHECK(ext4_mount("ordered","/",false) == EOK);
    if (!strcmp(argv[2],"lifecycle") || !strcmp(argv[2],"recover-lifecycle")) {
        struct jbd_fs lifecycle_fs;
        struct jbd_journal lifecycle_journal;
        CHECK(jbd_get_fs(dev.fs,&lifecycle_fs) == EOK);
        if (!strcmp(argv[2],"recover-lifecycle")) {
            CHECK(jbd_recover(&lifecycle_fs) == EOK);
            CHECK(!dev.fs->super_replay_required && ext4_sb_check(&dev.fs->sb));
        } else {
            lifecycle=1;
            if (argc > 3) cut=(unsigned)strtoul(argv[3],NULL,10);
            if (argc > 4) reorder=(unsigned)strtoul(argv[4],NULL,10);
        }
        CHECK(jbd_journal_start(&lifecycle_fs,&lifecycle_journal) == EOK);
        CHECK(jbd_journal_stop(&lifecycle_journal) == EOK);
        CHECK(!(ext4_get32(&dev.fs->sb,features_incompatible)&EXT4_FINCOM_RECOVER));
        CHECK(ext4_sb_check(&dev.fs->sb));
        CHECK(fault_block_crash(&disk) == 0);
        if (lifecycle) printf("%u\n",event);
        return 0;
    }
    ext4_file file;
    CHECK(ext4_fopen(&file,"/probe","r+") == EOK);
    struct ext4_inode_ref inode;
    ext4_fsblk_t home;
    CHECK(ext4_fs_get_inode_ref(dev.fs,file.inode,&inode) == EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&inode,0,&home,false) == EOK);
    CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
    data_offset=home*dev.lg_bsize;
    struct jbd_fs jfs;
    struct jbd_journal journal;
    CHECK(jbd_get_fs(dev.fs,&jfs) == EOK);
    if (!strncmp(argv[2],"recover",7)) {
        CHECK(jbd_recover(&jfs) == EOK);
        CHECK(fault_block_crash(&disk) == 0);
        unsigned char bytes[1024];
        CHECK(ext4_blocks_get_direct(&dev,bytes,home,1) == EOK);
        CHECK(bytes[0] == 'D');
        CHECK(ext4_fs_get_inode_ref(dev.fs,file.inode,&inode) == EOK);
        CHECK((ext4_inode_get_mode(&dev.fs->sb,inode.inode)&0777) ==
              (!strcmp(argv[2],"recover") ? 0600 : 0644));
        CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
        return 0;
    }
    CHECK(jbd_journal_start(&jfs,&journal) == EOK);
    CHECK(ext4_block_cache_write_back(&dev,1) == EOK);
    dev.fs->jbd_journal=&journal;
    struct ext4_block unrelated;
    CHECK(ext4_block_get(&dev,&unrelated,home+1) == EOK);
    memset(unrelated.data,'U',dev.lg_bsize);
    ext4_bcache_set_dirty(unrelated.buf);
    /* Put it on the real dirty list: a mount-wide flush would write it. */
    CHECK(ext4_block_set(&dev,&unrelated) == EOK);
    bool data_only=!strcmp(argv[2],"data-only");
    bool access_only=!strcmp(argv[2],"access-only");
    bool reuse=!strcmp(argv[2],"reuse");
    bool critical=!strncmp(argv[2],"log-",4);
    if (reuse) {
        struct jbd_trans *old=jbd_journal_new_trans(&journal);
        struct ext4_block oldblock;
        dev.fs->curr_trans=old;
        CHECK(ext4_trans_block_get(&dev,&oldblock,home) == EOK);
        memset(oldblock.data,'P',dev.lg_bsize);
        CHECK(ext4_trans_set_block_dirty(oldblock.buf) == EOK);
        CHECK(ext4_block_set(&dev,&oldblock) == EOK);
        dev.fs->curr_trans=NULL;
        CHECK(jbd_journal_commit_trans(&journal,old) == EOK);
    }
    uint64_t writes=disk.writes;
    for (unsigned attempt=0; attempt<2; attempt++) {
        struct jbd_trans *tx=jbd_journal_new_trans(&journal);
        CHECK(tx != NULL);
        dev.fs->curr_trans=tx;
        struct ext4_block block;
        CHECK(ext4_trans_data_get_noread(&dev,&block,home) == EOK);
        memset(block.data,'D',dev.lg_bsize);
        if (!access_only) CHECK(ext4_trans_set_data_dirty(block.buf) == EOK);
        if (!data_only && !access_only) {
        CHECK(ext4_fs_get_inode_ref(dev.fs,file.inode,&inode) == EOK);
        ext4_inode_set_mode(&dev.fs->sb,inode.inode,0100600);
        inode.dirty=true;
        CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
        }
        if (!reuse) CHECK(disk.writes == writes); /* Acquiring/marking must not write. */
        dev.fs->curr_trans=NULL;
        if (!strcmp(argv[2],"abort")) {
            jbd_journal_free_trans(&journal,tx,true);
            CHECK(block.data[0] == 'O');
            CHECK(disk.writes == writes);
            return 0;
        }
        trace=1;
        bool fail=attempt == 0 &&
            (!strcmp(argv[2],"write-error") || !strcmp(argv[2],"flush-error"));
        if (fail && !strcmp(argv[2],"write-error")) disk.fail_write=disk.writes+1;
        if (fail && !strcmp(argv[2],"flush-error")) disk.fail_flush=disk.flushes+1;
        if (!strcmp(argv[2],"log-write-error")) disk.fail_write=disk.writes+2;
        if (!strcmp(argv[2],"log-flush-error")) disk.fail_flush=disk.flushes+2;
        if (!strcmp(argv[2],"log-commit-flush-error")) disk.fail_flush=disk.flushes+3;
        uint32_t last=journal.last, tid=journal.committed_id;
        int r=jbd_journal_commit_trans(&journal,tx);
        if (critical) {
            CHECK(r == EIO && journal.error == EIO);
            CHECK(journal.failed_trans == tx);
            for (unsigned retry=0; retry<2; retry++)
                CHECK(jbd_journal_commit_trans(&journal,tx) == EIO);
            CHECK(journal.failed_trans == tx);
            CHECK(jbd_journal_new_trans(&journal) == NULL);
            CHECK(fault_block_crash(&disk) == 0);
            return 0;
        }
        if (fail) {
            CHECK(r == EIO);
            CHECK(journal.error == 0 && journal.failed_trans == NULL);
            CHECK(journal.last == last && journal.committed_id == tid);
            CHECK(block.data[0] == 'O');
            CHECK(log_writes == 0);
            CHECK(ext4_block_set(&dev,&block) == EOK);
            disk.fail_write=disk.fail_flush=0;
            writes=disk.writes;
            continue;
        }
        CHECK(r == EOK);
        CHECK(log_writes == ((data_only || access_only) ? 0 : 2));
        if (data_only || access_only) CHECK(journal.committed_id == tid);
        if (access_only) {
            CHECK(block.data[0] == 'O');
            CHECK(disk.writes == writes);
            return 0;
        }
        CHECK(ext4_block_set(&dev,&block) == EOK);
        unsigned char stable;
        CHECK(pread(disk.fd,&stable,1,(off_t)((home+1)*dev.lg_bsize)) == 1);
        CHECK(stable != 'U');
        CHECK(ext4_block_get(&dev,&unrelated,home+1) == EOK);
        CHECK(ext4_bcache_test_flag(unrelated.buf,BC_DIRTY));
        CHECK(ext4_block_set(&dev,&unrelated) == EOK);
        CHECK(fault_block_crash(&disk) == 0);
        return 0;
    }
    CHECK(!"retry did not complete");
}
