#ifndef BOAROS_TEST_GPT_FIXTURE_H
#define BOAROS_TEST_GPT_FIXTURE_H
#include <kernel/block.h>
#include <kernel/errno.h>
/* Test workspace, not task stack. CRC fixtures are also checked by the host
 * libblkid reader; no production parser routine is called to construct them. */
static unsigned char gpt_test_table[128 * 512];
static unsigned char gpt_test_header[512];
static struct kernel_block_device gpt_test_children[KERNEL_BLOCK_GPT_PARTITIONS];
static void gpt_test_put32(unsigned char *p, uint32_t value)
{ for (unsigned i=0;i<4;i++) p[i]=(unsigned char)(value>>(8*i)); }
static void gpt_test_put64(unsigned char *p, uint64_t value)
{ for (unsigned i=0;i<8;i++) p[i]=(unsigned char)(value>>(8*i)); }
static uint32_t gpt_test_crc(const unsigned char *p, size_t length)
{
    uint32_t table[256];
    for (unsigned i=0;i<256;i++) {
        uint32_t v=i;
        for(unsigned j=0;j<8;j++) v=(v&1)?(v>>1)^0xedb88320U:v>>1;
        table[i]=v;
    }
    uint32_t crc=~0U;
    while(length--) crc=table[(crc^*p++)&255]^(crc>>8);
    return ~crc;
}
static void gpt_test_header_crc(unsigned char *p)
{ gpt_test_put32(p+16,0);gpt_test_put32(p+16,gpt_test_crc(p,92)); }
static void gpt_test_entry(unsigned index, unsigned size, uint64_t first, uint64_t last)
{
    unsigned char *p=gpt_test_table+index*size;
    p[0]=0xaf; p[15]=0x11; p[16]=(unsigned char)(index+1);p[31]=0x22;
    gpt_test_put64(p+32,first);gpt_test_put64(p+40,last);
}
static int gpt_test_write_copies(struct kernel_block_device *disk, unsigned size)
{
    uint64_t end=disk->capacity_bytes/512-1, sectors=128*size/512;
    for(unsigned copy=0;copy<2;copy++) {
        uint64_t here=copy?end:1, entries=copy?end-sectors:2;
        __builtin_memset(gpt_test_header,0,512);
        __builtin_memcpy(gpt_test_header,"EFI PART",8);
        gpt_test_put32(gpt_test_header+8,0x10000);gpt_test_put32(gpt_test_header+12,92);
        gpt_test_put64(gpt_test_header+24,here);gpt_test_put64(gpt_test_header+32,copy?1:end);
        gpt_test_put64(gpt_test_header+40,2+sectors);gpt_test_put64(gpt_test_header+48,end-sectors-1);
        gpt_test_header[56]=0x33;gpt_test_header[71]=0x44;
        gpt_test_put64(gpt_test_header+72,entries);gpt_test_put32(gpt_test_header+80,128);
        gpt_test_put32(gpt_test_header+84,size);gpt_test_put32(gpt_test_header+88,gpt_test_crc(gpt_test_table,128*size));
        gpt_test_header_crc(gpt_test_header);
        if(kernel_block_write_at(disk,here*512,gpt_test_header,512) ||
            kernel_block_write_at(disk,entries*512,gpt_test_table,128*size)) return 1;
    }
    return 0;
}
static int gpt_test_fixture(struct kernel_block_device *disk, unsigned size)
{
    uint64_t end=disk->capacity_bytes/512-1;
    __builtin_memset(gpt_test_header,0,512);
    gpt_test_header[510]=0x55;gpt_test_header[511]=0xaa;gpt_test_header[450]=0xee;
    gpt_test_put32(gpt_test_header+454,1);gpt_test_put32(gpt_test_header+458,(uint32_t)end);
    if(kernel_block_write_at(disk,0,gpt_test_header,512))return 1;
    __builtin_memset(gpt_test_table,0,sizeof(gpt_test_table));
    gpt_test_entry(0,size,130,145);gpt_test_entry(2,size,160,175);
    return gpt_test_write_copies(disk,size);
}
static int gpt_test_release(void)
{
    for(unsigned i=0;i<KERNEL_BLOCK_GPT_PARTITIONS;i++)
        if(gpt_test_children[i].registered && kernel_block_unregister(&gpt_test_children[i]))return 1;
    __builtin_memset(gpt_test_children,0,sizeof(gpt_test_children));
    return 0;
}
static int test_gpt_partitions(struct kernel_block_device *disk)
{
    unsigned char data[512], out[512];
    int registered_here=!disk->registered;
    if(registered_here && kernel_block_register(disk,KERNEL_BLOCK_DEVICE_NUMBER(0)))return 1;
    if(gpt_test_fixture(disk,128) || kernel_block_scan_partitions(disk,gpt_test_children))return 2;
    if(gpt_test_children[0].start_bytes!=130*512 || gpt_test_children[0].capacity_bytes!=16*512 ||
        gpt_test_children[1].registered || gpt_test_children[2].start_bytes!=160*512)return 3;
    for(unsigned i=0;i<512;i++)data[i]=(unsigned char)(i*17+3);
    if(kernel_block_write_at(&gpt_test_children[2],0,data,512) ||
        kernel_block_read_at(disk,160*512,out,512) || __builtin_memcmp(data,out,512) ||
        kernel_block_read_at(&gpt_test_children[0],16*512,out,1)!=KERNEL_BLOCK_STATUS_OUT_OF_RANGE)return 4;
    if(gpt_test_release())return 5;
    /* Recovery through the real transport, then reject when both CRCs fail. */
    if(kernel_block_read_at(disk,512,out,512))return 6;
    out[16]^=1;
    if(kernel_block_write_at(disk,512,out,512) || kernel_block_scan_gpt(disk,gpt_test_children,15) || gpt_test_release())return 7;
    if(kernel_block_read_at(disk,disk->capacity_bytes-512,out,512))return 8;
    out[16]^=1;
    if(kernel_block_write_at(disk,disk->capacity_bytes-512,out,512) ||
        kernel_block_scan_gpt(disk,gpt_test_children,15)!=-KERNEL_EINVAL)return 9;
    if(registered_here && kernel_block_unregister(disk))return 10;
    return 0;
}
#endif
