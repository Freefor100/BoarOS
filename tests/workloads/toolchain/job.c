#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
struct counts { int live, peak, done; };
static int edit(int fd,int delta)
{
    struct flock lock={.l_type=F_WRLCK,.l_whence=SEEK_SET};
    if(fcntl(fd,F_SETLKW,&lock))return 1;
    struct counts c={0};ssize_t n=pread(fd,&c,sizeof(c),0);
    if(n!=0&&n!=sizeof(c))return 2;
    c.live+=delta;if(c.live>c.peak)c.peak=c.live;if(delta<0)c.done++;
    if(c.live<0||c.live>2||pwrite(fd,&c,sizeof(c),0)!=sizeof(c))return 3;
    char line[128];int length=snprintf(line,sizeof(line),"JOB live=%d peak=%d done=%d\n",c.live,c.peak,c.done);
    if(write(1,line,length)!=length)return 4;
    lock.l_type=F_UNLCK;return fcntl(fd,F_SETLK,&lock)!=0;
}
int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"hold")) {
        int fd=open("/evidence/hold-ready",O_CREAT|O_WRONLY,0600);if(fd<0)return 1;
        if(write(fd,"ready",5)!=5||close(fd))return 2;
        sleep(60);return 3;
    }
    int fd=open("/evidence/job-counts",O_RDWR|O_CREAT,0600);if(fd<0)return 1;
    if(edit(fd,1))return 2;usleep(300000);if(edit(fd,-1)||close(fd))return 3;return 0;
}
