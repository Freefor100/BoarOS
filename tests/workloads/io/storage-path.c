#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"STORAGE FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x);exit(1); } } while (0)
#define BYTES (4U*1024*1024)
static unsigned char data[65536], actual[65536];
static uint64_t samples[BYTES/4096];
static unsigned windows;
static uint64_t ns(clockid_t clock)
{
    struct timespec t;CHECK(!clock_gettime(clock,&t));
    return (uint64_t)t.tv_sec*1000000000U+t.tv_nsec;
}
static int compare(const void *a,const void *b)
{
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;
    return (x>y)-(x<y);
}
static void verify(int fd)
{
    for (unsigned at=0;at<BYTES;at+=sizeof(actual))
        CHECK(pread(fd,actual,sizeof(actual),at)==sizeof(actual) && !memcmp(actual,data,sizeof(actual)));
    struct stat s;CHECK(!fstat(fd,&s) && s.st_size==BYTES);
}
static void window(int fd,const char *backend,const char *operation,unsigned request,int writing)
{
    unsigned count=BYTES/request;
    uint64_t cpu=ns(CLOCK_PROCESS_CPUTIME_ID),start=ns(CLOCK_MONOTONIC);
    for (unsigned i=0;i<count;i++) {
        uint64_t begin=ns(CLOCK_MONOTONIC);
        if (writing) CHECK(pwrite(fd,data,request,(off_t)i*request)==request);
        else CHECK(pread(fd,actual,request,(off_t)i*request)==request && !memcmp(actual,data,request));
        samples[i]=ns(CLOCK_MONOTONIC)-begin;
    }
    uint64_t data_end=ns(CLOCK_MONOTONIC);
    if (writing) CHECK(!fsync(fd));
    uint64_t end=ns(CLOCK_MONOTONIC),used=ns(CLOCK_PROCESS_CPUTIME_ID)-cpu;
    if (writing) verify(fd);
    qsort(samples,count,sizeof(samples[0]),compare);
    struct rusage usage;CHECK(!getrusage(RUSAGE_SELF,&usage));
    printf("STORAGE METRIC backend=%s operation=%s request=%u bytes=%u count=%u ns=%llu sync_ns=%llu p50=%llu p95=%llu max=%llu cpu_ns=%llu rss_kib=%ld check=1\n",
        backend,operation,request,BYTES,count,(unsigned long long)(end-start),
        (unsigned long long)(end-data_end),(unsigned long long)samples[count/2],
        (unsigned long long)samples[count*95/100],(unsigned long long)samples[count-1],
        (unsigned long long)used,usage.ru_maxrss);
    windows++;
}
static void run(const char *directory,const char *backend,const char *device,const char *type)
{
    char path[128];CHECK(snprintf(path,sizeof(path),"%s/data",directory)>0);
    int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
    for (unsigned at=0;at<BYTES;at+=sizeof(data))
        CHECK(pwrite(fd,data,sizeof(data),at)==sizeof(data));
    CHECK(!fsync(fd));verify(fd);
    for (unsigned request=4096;request<=65536;request*=16) {
        window(fd,backend,"overwrite-fsync",request,1);
        if (device) {
            CHECK(!close(fd) && !umount(directory) && !mount(device,directory,type,0,NULL));
            fd=open(path,O_RDWR);CHECK(fd>=0);
        }
        /* Loop's upper cache is cold; its backing file can remain cached. */
        window(fd,backend,device?"cold-read":"hot-read",request,0);
    }
    CHECK(!close(fd) && !unlink(path));
    fd=open(directory,O_RDONLY|O_DIRECTORY);CHECK(fd>=0 && !fsync(fd) && !close(fd));
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    for (unsigned i=0;i<sizeof(data);i++) data[i]=(unsigned char)(i*37+19);
    CHECK(!mkdir("/dev",0755) || errno==EEXIST);
    CHECK(!mkdir("/plain",0755) && !mkdir("/fat",0755) && !mkdir("/loop",0755));
    CHECK(!mknod("/dev/vdb2",S_IFBLK|0600,makedev(252,18)));
    CHECK(!mknod("/dev/loop0",S_IFBLK|0600,makedev(7,0)));
    int backing=open("/loop.img",O_RDWR),loop=open("/dev/loop0",O_RDWR);
    CHECK(backing>=0 && loop>=0 && !ioctl(loop,0x4c00,backing) && !close(backing));
    CHECK(!mount("/dev/vdb2","/fat","vfat",0,NULL));
    CHECK(!mount("/dev/loop0","/loop","ext4",0,NULL));
    run("/plain","ext4",NULL,NULL);
    run("/fat","vfat","/dev/vdb2","vfat");
    run("/loop","loop-ext4","/dev/loop0","ext4");
    CHECK(!umount("/loop") && !ioctl(loop,0x4c01,0) && !close(loop));
    CHECK(!umount("/fat") && windows==12);
    puts("STORAGE PASS windows=12");
    return 0;
}
