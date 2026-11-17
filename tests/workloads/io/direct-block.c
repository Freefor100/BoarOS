#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
/* As PID 1 use an unmounted private loop device. On Linux pass a disposable
 * pre-sized private block device. Regular-file alignment/error policies vary
 * by filesystem and are not an equivalent differential-test target. */
int main(int argc,char **argv)
{
    const char *target=argc>1?argv[1]:"/dev/direct-loop";
    int setup=argc==1;
    if(setup) {
        (void)mkdir("/dev",0755);
        CHECK(!mknod(target,S_IFBLK|0600,makedev(7,0)));
        int backing=open("/direct-backing",O_CREAT|O_RDWR|O_TRUNC,0600);
        CHECK(backing>=0 && !ftruncate(backing,65536));
        int loop=open(target,O_RDWR);
        CHECK(loop>=0 && !ioctl(loop,0x4c00,backing));
        CHECK(!close(backing) && !close(loop));
    }
    void *allocation=NULL;
    CHECK(!posix_memalign(&allocation,16384,32768));
    unsigned char *bytes=allocation; memset(bytes,0x6d,32768);
    int fd=open(target,O_RDWR|O_DIRECT); CHECK(fd>=0);
    CHECK(fcntl(fd,F_GETFL)&O_DIRECT);
    CHECK(pwrite(fd,bytes,4096,0)==4096);
    memset(bytes,0,4096); CHECK(pread(fd,bytes,4096,0)==4096);
    for(unsigned i=0;i<4096;i++) CHECK(bytes[i]==0x6d);
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
    CHECK(!fsync(fd));
    CHECK(!close(duplicate) && !close(fd));
    if(setup) {
        fd=open(target,O_RDWR);CHECK(fd>=0);
        CHECK(!ioctl(fd,0x4c01,0) && !close(fd));
        CHECK(!unlink(target) && !unlink("/direct-backing"));
    }
    free(allocation);
    puts("DIRECT BLOCK ALL PASS");
    return 0;
}
