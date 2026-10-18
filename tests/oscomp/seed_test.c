/* Host syscall fixture executes the actual startup program and checks persistence ordering. */
#include <assert.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
static int test_open(const char *, int, ...);
static int test_close(int);
static int test_stat(const char *, struct stat *);
static int test_fstat(int, struct stat *);
static int test_ioctl(int, unsigned long, ...);
static ssize_t test_pread(int, void *, size_t, off_t);
static ssize_t test_pwrite(int, const void *, size_t, off_t);
static int test_fsync(int);
static ssize_t test_getrandom(void *, size_t, unsigned);
#define open test_open
#define close test_close
#define stat test_stat
#define fstat test_fstat
#define ioctl test_ioctl
#define pread test_pread
#define pwrite test_pwrite
#define fsync test_fsync
#define getrandom test_getrandom
#define SEED_ARCH 243
#define main seed_main
/* Keep struct stat spelled correctly while replacing the function call. */
#undef stat
#define stat(path, st) test_stat(path, st)
#include "seed.c"
#undef main
#undef stat
static unsigned char disk[8192];
static int flushed, credited, writes, opened, failure;
static const uint64_t sizes[] = {64*MIB,2304*MIB,128*MIB,64*MIB,2048*MIB};
static int test_open(const char *path, int flags, ...)
{
    (void)flags;
    const char *paths[]={"/dev/vda","/dev/vdb","/dev/vdb1","/dev/vdb2","/dev/vdb3","/dev/random"};
    for(int i=0;i<6;++i) if(!strcmp(path,paths[i])) {++opened; return i+10;}
    assert(0); return -1;
}
static int test_close(int fd) {assert(fd>=10&&fd<=15);--opened;return 0;}
static int test_stat(const char *path, struct stat *st)
{assert(!strcmp(path,"/"));memset(st,0,sizeof(*st));st->st_dev=makedev(252,failure==1?16:0);return 0;}
static int test_fstat(int fd, struct stat *st)
{memset(st,0,sizeof(*st));st->st_mode=S_IFBLK;st->st_rdev=makedev(252,fd==10?0:fd-11+16);return 0;}
static int test_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;va_start(ap,request);void *arg=va_arg(ap,void *);va_end(ap);
    if(request==BLKGETSIZE64) {*(uint64_t*)arg=sizes[fd-10]+(failure==2&&fd==14?512:0);return 0;}
    if(request==BLKSSZGET) {*(int*)arg=512;return 0;}
    if(request==BLKROGET) {*(int*)arg=0;return 0;}
    if(request==RNDADDENTROPY) {
        assert(fd==15&&flushed&&writes);assert(*(int*)arg==256&&((int*)arg)[1]==64);
        for(int i=0;i<64;++i) assert(((unsigned char*)arg)[8+i]==(unsigned char)(i+1));
        for(int i=4096;i<8192;++i) assert(!disk[i]);
        ++credited;return 0;
    }
    assert(request==RNDGETENTCNT);*(int*)arg=256;return 0;
}
static ssize_t test_pread(int fd, void *p, size_t size, off_t offset)
{assert(fd==11&&offset>=0&&offset+(off_t)size<=8192);memcpy(p,disk+offset,size);return (ssize_t)size;}
static ssize_t test_pwrite(int fd,const void *p,size_t size,off_t offset)
{assert(fd==11&&offset>=4096&&offset+(off_t)size<=8192);if(failure==4){errno=EIO;return -1;}memcpy(disk+offset,p,size);++writes;return (ssize_t)size;}
static int test_fsync(int fd)
{assert(fd==11&&writes&&!credited);if(failure==5){errno=EIO;return -1;}flushed=1;return 0;}
static ssize_t test_getrandom(void *p,size_t size,unsigned flags)
{assert(credited&&size==1&&flags==GRND_NONBLOCK);*(char*)p=42;return 1;}
static void put32(unsigned char *p,uint32_t value) {for(int i=0;i<4;++i)p[i]=(unsigned char)(value>>(8*i));}
static void setup(void)
{
    memset(disk,0,sizeof(disk));flushed=credited=writes=opened=0;
    const uint32_t starts[]={2048,129*2048,193*2048},sectors[]={128*2048,64*2048,2048*2048};
    for(int i=0;i<3;++i){unsigned char *p=disk+446+i*16;p[4]=i==1?12:131;put32(p+8,starts[i]);put32(p+12,sectors[i]);}
    disk[510]=0x55;disk[511]=0xaa;
    unsigned char *s=disk+4096;memcpy(s,"BOARSEED",8);put32(s+8,1);put32(s+12,243);put32(s+16,64);
    for(int i=0;i<64;++i)s[24+i]=(unsigned char)(i+1);
    put32(s+20,~crc32(crc32(~0U,s,20),s+24,64));
    if(failure==3)s[24]^=1;
}
int main(void)
{
    for(failure=0;failure<=5;++failure){setup();assert(seed_main()==(failure?125:0));assert(!opened);assert(credited==(failure?0:1));}
    failure=0;setup();assert(seed_main()==0);assert(seed_main()==125);assert(credited==1&&!opened);
    puts("seed ordering, failure propagation, resource closure and replay rejection passed");return 0;
}
