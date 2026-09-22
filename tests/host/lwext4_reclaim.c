/* Public unlink/truncate/orphan recovery over volatile 512-byte sectors.
 * Detects lost orphan enrollment, early inode reuse, incomplete truncation,
 * counter leaks, stale-data replay, and non-idempotent recovery. */
#include "block_fault.h"
#include <ext4.h>
#include <ext4_bitmap.h>
#include <ext4_block_group.h>
#include <ext4_errno.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_misc.h>
#include <ext4_orphan.h>
#include <ext4_super.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s (event %u, cut %u)\n",__FILE__,__LINE__,#x,event,cut); exit(1); } } while (0)
static struct fault_block disk;
static unsigned event, cut, reorder;
static bool armed;
struct facts {
    unsigned long long free_blocks, old_size, old_blocks;
    unsigned free_inodes, inode, sparse;
};
static void boundary(void)
{
    if (!armed || ++event != cut) return;
    if (reorder && disk.count) CHECK(fault_block_persist(&disk,disk.count-1) == 0);
    CHECK(fault_block_crash(&disk) == 0);
    _Exit(75);
}
static int dev_open(struct ext4_blockdev *b) { (void)b; return EOK; }
static int dev_read(struct ext4_blockdev *b, void *p, uint64_t n, uint32_t c)
{ (void)b; return kernel_block_read_at(&disk.device,n*512,p,(size_t)c*512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; }
static int dev_write(struct ext4_blockdev *b, const void *p, uint64_t n, uint32_t c)
{ (void)b; int r=kernel_block_write_at(&disk.device,n*512,p,(size_t)c*512) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; boundary(); return r; }
static int dev_flush(struct ext4_blockdev *b)
{ (void)b; int r=kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK ? EOK : EIO; boundary(); return r; }
static void save_facts(const char *path, const struct facts *f)
{
    FILE *out=fopen(path,"w"); CHECK(out != NULL);
    CHECK(fprintf(out,"%llu %u %u %llu %llu %u\n",f->free_blocks,f->free_inodes,
                  f->inode,f->old_size,f->old_blocks,f->sparse) > 0);
    CHECK(fclose(out) == 0);
}
static struct facts read_facts(const char *path)
{
    struct facts f;
    FILE *in=fopen(path,"r"); CHECK(in != NULL);
    CHECK(fscanf(in,"%llu %u %u %llu %llu %u",&f.free_blocks,&f.free_inodes,
                 &f.inode,&f.old_size,&f.old_blocks,&f.sparse) == 6);
    CHECK(fclose(in) == 0);
    return f;
}
static void positions(uint32_t block_size, uint64_t p[7])
{
    uint64_t n=block_size/4, triple=12+n+n*n;
    uint64_t values[7]={128,512,4096,8192,12000,triple+9,triple+n+3};
    memcpy(p,values,sizeof(values));
}
static unsigned char pattern(uint64_t block, unsigned byte)
{ return (unsigned char)(1+(block*17+byte)%251); }
static void fill(unsigned char *bytes, uint32_t size, uint64_t block)
{ for (uint32_t j=0;j<size;j++) bytes[j]=pattern(block,j); }
static void check_range(ext4_file *file, uint64_t position, size_t length,
                        uint32_t block_size, bool zero)
{
    unsigned char *bytes=malloc(length); CHECK(bytes != NULL);
    CHECK(ext4_fseek(file,position,SEEK_SET) == EOK);
    size_t got=0;
    CHECK(ext4_fread(file,bytes,length,&got) == EOK && got == length);
    for (size_t i=0;i<got;i++) {
        unsigned char want=zero ? 0 : pattern((position+i)/block_size,
                                              (unsigned)((position+i)%block_size));
        CHECK(bytes[i] == want);
    }
    free(bytes);
}
static bool allocated(struct ext4_fs *fs, uint32_t ino)
{
    uint32_t per=ext4_get32(&fs->sb,inodes_per_group);
    struct ext4_block_group_ref group;
    CHECK(ext4_fs_get_block_group_ref(fs,(ino-1)/per,&group) == EOK);
    ext4_fsblk_t address=ext4_bg_get_inode_bitmap(group.block_group,&fs->sb);
    CHECK(ext4_fs_put_block_group_ref(&group) == EOK);
    struct ext4_block bitmap;
    CHECK(ext4_block_get(fs->bdev,&bitmap,address) == EOK);
    bool result=ext4_bmap_is_bit_set(bitmap.data,(ino-1)%per);
    CHECK(ext4_block_set(fs->bdev,&bitmap) == EOK);
    return result;
}
static void setup(struct ext4_fs *fs, const char *path, bool sparse)
{
    struct ext4_mount_stats stats;
    CHECK(ext4_mount_point_stats("/",&stats) == EOK);
    struct facts f={.free_blocks=stats.free_blocks_count,.free_inodes=stats.free_inodes_count,
                    .sparse=sparse};
    uint32_t size=fs->bdev->lg_bsize;
    unsigned char *bytes=malloc(size); CHECK(bytes != NULL);
    ext4_file file; size_t wrote;
    CHECK(ext4_fopen(&file,"/victim","w+") == EOK);
    f.inode=file.inode;
    for (unsigned i=0;i<80;i++) {
        fill(bytes,size,i);
        CHECK(ext4_fwrite(&file,bytes,size,&wrote) == EOK && wrote == size);
    }
    if (sparse) {
        uint64_t p[7]; positions(size,p);
        for (unsigned i=0;i<7;i++) {
            fill(bytes,size,p[i]);
            CHECK(ext4_fseek(&file,p[i]*size,SEEK_SET) == EOK);
            int r=ext4_fwrite(&file,bytes,size,&wrote);
            if (r) fprintf(stderr,"sparse write logical block %llu returned %d\n",(unsigned long long)p[i],r);
            CHECK(r == EOK && wrote == size);
        }
    }
    free(bytes);
    f.old_size=ext4_fsize(&file);
    struct ext4_inode_ref inode;
    CHECK(ext4_fs_get_inode_ref(fs,f.inode,&inode) == EOK);
    f.old_blocks=ext4_inode_get_blocks_count(&fs->sb,inode.inode)/(size/512);
    CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
    CHECK(ext4_fclose(&file) == EOK);
    CHECK(ext4_umount("/") == EOK);
    save_facts(path,&f);
}
/* Count the reachable on-disk extent index, independently of the truncate
 * implementation. A legal nonempty leaf need not be collapsed into the inode.
 * Format: references/linux/Documentation/filesystems/ext4/ifork.rst,
 * commit f4cdf7ca9a1fdcca413157df19753f388a5a224e. */
static uint16_t disk16(const unsigned char *p)
{ uint16_t x; memcpy(&x,p,sizeof(x)); return to_le16(x); }
static uint32_t disk32(const unsigned char *p)
{ uint32_t x; memcpy(&x,p,sizeof(x)); return to_le32(x); }
static unsigned extent_metadata(struct ext4_fs *fs, const unsigned char *node,
                                size_t bytes, unsigned depth, unsigned *mapped)
{
    CHECK(bytes >= 12 && disk16(node) == 0xf30a);
    unsigned entries=disk16(node+2);
    CHECK(depth <= 5 && disk16(node+6) == depth);
    CHECK(entries <= disk16(node+4) && 12ULL+entries*12ULL <= bytes);
    unsigned metadata=0;
    for (unsigned i=0;i<entries;i++) {
        const unsigned char *entry=node+12+i*12;
        if (!depth) {
            uint32_t first=disk32(entry);
            unsigned length=disk16(entry+4);
            if (length > 32768) length-=32768;
            CHECK(length && (uint64_t)first+length <= 6);
            for (unsigned j=0;j<length;j++) {
                CHECK(!(*mapped & (1U << (first+j))));
                *mapped |= 1U << (first+j);
            }
        } else {
            ext4_fsblk_t physical=disk32(entry+4) | ((uint64_t)disk16(entry+8)<<32);
            CHECK(physical && physical < fs->bdev->lg_bcnt);
            struct ext4_block block;
            CHECK(ext4_block_get(fs->bdev,&block,physical) == EOK);
            metadata+=1+extent_metadata(fs,block.data,fs->bdev->lg_bsize,depth-1,mapped);
            CHECK(ext4_block_set(fs->bdev,&block) == EOK);
        }
    }
    return metadata;
}
static void verify(struct ext4_fs *fs, const struct facts *f, bool unlink,
                   bool require_new)
{
    uint32_t size=fs->bdev->lg_bsize, orphan;
    uint64_t target=5ULL*size+17;
    CHECK(ext4_orphan_peek(fs,&orphan) == EOK && orphan == 0);
    CHECK(!ext4_get32(&fs->sb,last_orphan));
    CHECK(!ext4_sb_feature_ro_com(&fs->sb,EXT4_FRO_COM_ORPHAN_PRESENT));
    ext4_file file;
    int r=ext4_fopen(&file,"/victim","r");
    bool present=r == EOK;
    CHECK(present || r == ENOENT);
    bool old=present && ext4_fsize(&file) == f->old_size;
    CHECK(!require_new || !old);
    unsigned long long blocks=0;
    if (present) {
        CHECK(file.inode == f->inode);
        CHECK(old || (!unlink && ext4_fsize(&file) == target));
        uint64_t amount=old ? 80ULL*size : target;
        for (uint64_t at=0;at<amount;) {
            size_t n=amount-at > size ? size : (size_t)(amount-at);
            check_range(&file,at,n,size,false); at+=n;
        }
        if (old && f->sparse) {
            uint64_t p[7]; positions(size,p);
            for (unsigned i=0;i<7;i++) check_range(&file,p[i]*size,size,size,false);
            check_range(&file,80ULL*size,size,size,true);
            check_range(&file,(p[6]-1)*size,size,size,true);
        }
        struct ext4_inode_ref inode;
        CHECK(ext4_fs_get_inode_ref(fs,f->inode,&inode) == EOK);
        blocks=ext4_inode_get_blocks_count(&fs->sb,inode.inode)/(size/512);
        unsigned long long expected=old ? f->old_blocks : 6;
        if (!old && ext4_inode_has_flag(inode.inode,EXT4_INODE_FLAG_EXTENTS)) {
            const unsigned char *root=(void *)inode.inode->blocks;
            unsigned mapped=0;
            expected+=extent_metadata(fs,root,sizeof(inode.inode->blocks),disk16(root+6),&mapped);
            CHECK(mapped == 0x3f);
        }
        CHECK(blocks == expected);
        CHECK(ext4_inode_get_links_cnt(inode.inode) == 1);
        CHECK(ext4_fs_put_inode_ref(&inode) == EOK);
        CHECK(ext4_fclose(&file) == EOK);
    } else CHECK(unlink);
    CHECK(allocated(fs,f->inode) == present);
    struct ext4_mount_stats stats;
    CHECK(ext4_mount_point_stats("/",&stats) == EOK);
    CHECK(stats.free_inodes_count == f->free_inodes-(present ? 1U : 0U));
    CHECK(stats.free_blocks_count == f->free_blocks-blocks);
}
int main(int argc, char **argv)
{
    CHECK(argc >= 5);
    const char *action=argv[2], *facts_path=argv[3], *kind=argv[4];
    bool readonly=!strncmp(action,"readonly",8);
    CHECK(fault_block_open(&disk,argv[1]) == 0);
    unsigned char physical[512];
    struct ext4_blockdev_iface iface={.open=dev_open,.close=dev_open,.bread=dev_read,
        .bwrite=dev_write,.flush=dev_flush,.ph_bsize=512,
        .ph_bcnt=disk.device.capacity_bytes/512,.ph_bbuf=physical};
    struct ext4_blockdev dev={.bdif=&iface,.part_size=disk.device.capacity_bytes};
    CHECK(ext4_device_register(&dev,"reclaim") == EOK);
    CHECK(ext4_mount("reclaim","/",readonly) == EOK);
    if (readonly) {
        uint64_t writes=disk.writes;
        if (!strcmp(action,"readonly-log")) CHECK(ext4_recover("/") == EROFS);
        else {
            CHECK(ext4_recover("/") == EOK);
            CHECK(ext4_orphan_recover("/") == EROFS);
        }
        CHECK(disk.writes == writes);
        return 0;
    }
    if (argc > 5) cut=(unsigned)strtoul(argv[5],NULL,10);
    if (argc > 6) reorder=(unsigned)strtoul(argv[6],NULL,10);
    bool recovery=!strcmp(action,"recover") || !strcmp(action,"verify");
    if (recovery) armed=true;
    CHECK(ext4_recover("/") == EOK);
    CHECK(ext4_journal_start("/") == EOK);
    CHECK(ext4_orphan_recover("/") == EOK);
    if (!strcmp(action,"setup")) { setup(dev.fs,facts_path,!strcmp(kind,"sparse")); return 0; }
    struct facts f=read_facts(facts_path);
    bool unlink=!strcmp(kind,"unlink");
    if (recovery) {
        verify(dev.fs,&f,unlink,!strcmp(action,"recover"));
        CHECK(ext4_orphan_recover("/") == EOK);
        CHECK(ext4_umount("/") == EOK);
    } else {
        ext4_file file;
        CHECK(ext4_fopen(&file,"/victim","r+") == EOK);
        CHECK(file.inode == f.inode && ext4_fsize(&file) == f.old_size);
        armed=true; event=0;
        CHECK(ext4_transaction_begin("/") == EOK);
        if (unlink) {
            uint32_t ino; bool orphan;
            CHECK(ext4_funlink_dentry("/victim",&ino,&orphan) == EOK);
            CHECK(ino == f.inode && orphan);
        } else CHECK(ext4_ftruncate(&file,5ULL*dev.lg_bsize+17) == EOK);
        CHECK(ext4_transaction_end("/") == EOK);
        uint32_t orphan;
        CHECK(ext4_orphan_peek(dev.fs,&orphan) == EOK && orphan == f.inode);
        if (unlink) check_range(&file,0,dev.lg_bsize,dev.lg_bsize,false);
        if (!strcmp(action,"publish")) CHECK(ext4_journal_stop("/") == EOK);
        /* Keep the opened unlink handle alive until the simulated power cut. */
    }
    printf("%u\n",event);
    CHECK(fault_block_crash(&disk) == 0);
    return 0;
}
