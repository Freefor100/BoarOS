#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"DIRECT BLOCK FAIL line=%d errno=%d\n",__LINE__,errno); return 1; } } while (0)
static int check_device(const char *target)
{
    void *allocation=NULL;
    CHECK(!posix_memalign(&allocation,16384,32768));
    unsigned char *bytes=allocation; memset(bytes,0x6d,32768);
    int fd=open(target,O_RDWR|O_DIRECT); CHECK(fd>=0);
    CHECK(fcntl(fd,F_GETFL)&O_DIRECT);
    CHECK(pwrite(fd,bytes,16384,0)==16384);
    memset(bytes,0,16384); CHECK(pread(fd,bytes,16384,0)==16384);
    for(unsigned i=0;i<16384;i++) CHECK(bytes[i]==0x6d);
    errno=0;CHECK(pwrite(fd,bytes+1,512,0)==-1 && errno==EINVAL);
    errno=0;CHECK(pwrite(fd,bytes,511,0)==-1 && errno==EINVAL);
    errno=0;CHECK(pread(fd,bytes,512,1)==-1 && errno==EINVAL);
    struct iovec split[2]={{bytes,256},{bytes+256,256}};
    errno=0;CHECK(preadv(fd,split,2,0)==-1 && errno==EINVAL);
    struct iovec aligned[2]={{bytes,512},{bytes+512,512}};
    CHECK(pwritev(fd,aligned,2,4096)==1024);
    int duplicate=dup(fd);CHECK(duplicate>=0);
    int flags=fcntl(fd,F_GETFL);CHECK(flags>=0);
    CHECK(!fcntl(duplicate,F_SETFL,flags&~O_DIRECT));
    CHECK(!(fcntl(fd,F_GETFL)&O_DIRECT));
    bytes[1]=0x73;CHECK(pwrite(fd,bytes+1,1,1)==1);
    CHECK(!fcntl(duplicate,F_SETFL,flags|O_DIRECT));
    CHECK(pread(fd,bytes,512,0)==512 && bytes[1]==0x73);
    uint64_t capacity;
    CHECK(!ioctl(fd,0x80081272,&capacity) && capacity>=65536);
    CHECK(pwrite(fd,bytes,1024,capacity-512)==512);
    CHECK(pread(fd,bytes,1024,capacity-512)==512);
    CHECK(pread(fd,bytes,512,capacity)==0);
    errno=0;CHECK(pwrite(fd,bytes,512,capacity)==-1 && errno==ENOSPC);
    CHECK(!fsync(fd));
    CHECK(!close(duplicate) && !close(fd));
    free(allocation);
    return 0;
}

/* The BoarOS runner attaches a blank second VirtIO disk. With an argument
 * the same checks use a caller-provided private 512-byte-sector block device. */
int main(int argc,char **argv)
{
    if (argc > 1) {
        if (check_device(argv[1])) return 1;
    } else {
        const char *physical="/dev/direct-disk", *loop="/dev/direct-loop";
        CHECK(!mknod(physical,S_IFBLK|0600,makedev(252,16)));
        if (check_device(physical)) return 1;
        CHECK(!unlink(physical));
        CHECK(!mknod(loop,S_IFBLK|0600,makedev(7,0)));
        int backing=open("/direct-backing",O_CREAT|O_RDWR|O_TRUNC,0600);
        CHECK(backing>=0 && !ftruncate(backing,65536));
        int fd=open(loop,O_RDWR);
        CHECK(fd>=0 && !ioctl(fd,0x4c00,backing));
        CHECK(!close(backing) && !close(fd));
        if (check_device(loop)) return 1;
        fd=open(loop,O_RDWR);CHECK(fd>=0);
        CHECK(!ioctl(fd,0x4c01,0) && !close(fd));
        CHECK(!unlink(loop) && !unlink("/direct-backing"));
    }
    puts("DIRECT BLOCK ALL PASS");
    return 0;
}
