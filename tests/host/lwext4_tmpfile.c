#include "block_fault.h"
#include <ext4.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_orphan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%d: %s\n",__LINE__,#x); exit(1); } } while(0)
static struct fault_block disk;
static int dev_open(struct ext4_blockdev *b) { (void)b; return EOK; }
static int dev_read(struct ext4_blockdev *b,void *p,uint64_t n,uint32_t c)
{ (void)b; return kernel_block_read_at(&disk.device,n*512,p,(size_t)c*512)==KERNEL_BLOCK_STATUS_OK?EOK:EIO; }
static int dev_write(struct ext4_blockdev *b,const void *p,uint64_t n,uint32_t c)
{ (void)b; return kernel_block_write_at(&disk.device,n*512,p,(size_t)c*512)==KERNEL_BLOCK_STATUS_OK?EOK:EIO; }
static int dev_flush(struct ext4_blockdev *b)
{ (void)b; return kernel_block_flush(&disk.device)==KERNEL_BLOCK_STATUS_OK?EOK:EIO; }
int main(int argc,char **argv)
{
 CHECK(argc==3 && !fault_block_open(&disk,argv[1])); unsigned char scratch[512];
 struct ext4_blockdev_iface iface={.open=dev_open,.close=dev_open,.bread=dev_read,.bwrite=dev_write,.flush=dev_flush,.ph_bsize=512,.ph_bcnt=disk.device.capacity_bytes/512,.ph_bbuf=scratch};
 struct ext4_blockdev dev={.bdif=&iface,.part_size=disk.device.capacity_bytes};
 CHECK(!ext4_device_register(&dev,"tmpfile")); CHECK(!ext4_mount("tmpfile","/",false));
 CHECK(!ext4_recover("/")); CHECK(!ext4_journal_start("/")); CHECK(!ext4_orphan_recover("/"));
 if (!strcmp(argv[2],"verify")) { uint32_t ino=0; CHECK(!ext4_orphan_peek(dev.fs,&ino) && !ino); }
 else {
 uint32_t free_before=ext4_fs_available_inodes(dev.fs); ext4_file f={0}; struct ext4_inode initial;
 CHECK(!ext4_tmpfile_child("/",2,0100640,&f,&initial,123,456));
 CHECK(!ext4_inode_get_links_cnt(&initial) && ext4_inode_get_uid(&initial)==123 && ext4_inode_get_gid(&initial)==456);
 bool present=false; CHECK(!ext4_orphan_contains(dev.fs,f.inode,&present) && present);
 CHECK(ext4_link_child("/",f.inode,2,"bad",3)==ENOENT);
 const char payload[]="anonymous data"; size_t count=0;
 CHECK(!ext4_fwrite(&f,payload,sizeof(payload),&count) && count==sizeof(payload));
 if (!strcmp(argv[2],"crash")) { CHECK(!ext4_cache_flush("/")); CHECK(!fault_block_crash(&disk)); return 0; }
 CHECK(!ext4_transaction_begin("/")); CHECK(!ext4_tmpfile_link_child("/",f.inode,2,"published",9));
 CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED);
 CHECK(!ext4_orphan_contains(dev.fs,f.inode,&present) && present);
 ext4_file probe={0}; CHECK(ext4_fopen(&probe,"/published","r")==ENOENT);
 CHECK(!ext4_tmpfile_link_child("/",f.inode,2,"published",9));
 CHECK(!ext4_orphan_contains(dev.fs,f.inode,&present) && !present);
 CHECK(ext4_tmpfile_link_child("/",f.inode,2,"again",5)==ENOENT);
 CHECK(!ext4_link_child("/",f.inode,2,"alias",5)); CHECK(!ext4_fopen(&probe,"/published","r")); char data[sizeof(payload)];
 CHECK(!ext4_fread(&probe,data,sizeof(data),&count) && count==sizeof(data) && !memcmp(data,payload,count));
 CHECK(!ext4_fclose(&probe)); CHECK(!ext4_fclose(&f)); CHECK(!ext4_fremove("/alias")); CHECK(!ext4_fremove("/published"));
 CHECK(!ext4_tmpfile_child("/",2,0100600,&f,NULL,0,0)); uint32_t ino=f.inode;
 CHECK(!ext4_fclose(&f)); CHECK(!ext4_orphan_free("/",ino)); CHECK(ext4_fs_available_inodes(dev.fs)==free_before);
 CHECK(!ext4_transaction_begin("/")); CHECK(!ext4_tmpfile_child("/",2,0100600,&f,NULL,0,0)); CHECK(!ext4_fclose(&f));
 CHECK(ext4_transaction_abort("/",ECANCELED)==ECANCELED); CHECK(ext4_fs_available_inodes(dev.fs)==free_before);
 }
 CHECK(!ext4_umount("/")); fault_block_close(&disk); puts("PASS tmpfile"); return 0;
}
