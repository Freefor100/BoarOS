#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"environment:%d: %s errno=%d\n",__LINE__,#x,errno);return 1;} } while(0)
static void interrupted(int sig) { (void)sig; }
static int log_checks(void)
{
    static char data[16384];int n=klogctl(10,NULL,0);CHECK(n==16384);
    n=klogctl(3,data,sizeof(data));CHECK(n>0 && memmem(data,n,"BoarOS:",7));
    CHECK(!memmem(data,n,"user-console-only-marker",24));
    CHECK(write(1,"user-console-only-marker\n",25)==25);
    n=klogctl(3,data,sizeof(data));CHECK(n>0 && !memmem(data,n,"user-console-only-marker",24));
    CHECK(klogctl(0,NULL,0)==0 && klogctl(1,NULL,0)==0);
    CHECK(klogctl(8,NULL,0)==-1 && errno==EINVAL);
    CHECK(klogctl(8,NULL,9)==-1 && errno==EINVAL);
    CHECK(klogctl(8,NULL,1)==0 && klogctl(6,NULL,0)==0 && klogctl(7,NULL,0)==0 && klogctl(8,NULL,8)==0);
    CHECK(klogctl(11,NULL,0)==-1 && errno==EINVAL);
    CHECK(klogctl(3,NULL,1)==-1 && errno==EINVAL);
    CHECK(klogctl(3,(void *)1,sizeof(data))==-1 && errno==EFAULT);
    CHECK(klogctl(3,data,-1)==-1 && errno==EINVAL);
    CHECK(klogctl(3,data,0)==0);
    CHECK(klogctl(9,NULL,0)>0);
    CHECK(klogctl(4,data,sizeof(data))>0 && klogctl(3,data,sizeof(data))==0);
    CHECK(klogctl(5,NULL,0)==0);
    while(klogctl(9,NULL,0)>0)CHECK(klogctl(2,data,sizeof(data))>0);
    int ready[2];CHECK(pipe(ready)==0);pid_t pid=fork();CHECK(pid>=0);
    if(!pid){
        struct sigaction action={.sa_handler=interrupted};sigemptyset(&action.sa_mask);
        if(sigaction(SIGUSR1,&action,NULL) || write(ready[1],"r",1)!=1)_exit(2);
        int result=klogctl(2,data,16);_exit(result==-1 && errno==EINTR ? 0:3);
    }
    char c;CHECK(read(ready[0],&c,1)==1);char path[64];snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    int sleeping=0;
    for(unsigned i=0;i<100000 && !sleeping;i++){
        int fd=open(path,O_RDONLY);CHECK(fd>=0);n=read(fd,data,sizeof(data)-1);CHECK(n>0 && close(fd)==0);data[n]=0;
        char *end=strrchr(data,')');CHECK(end && end[1]==' ');sleeping=end[2]=='S';
    }
    CHECK(sleeping && kill(pid,SIGUSR1)==0);int status;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(close(ready[0])==0 && close(ready[1])==0);
    puts("ENV PASS log: real content, console separation, control, clear, errors and interruptible read");return 0;
}
struct rtc_time { int sec,min,hour,mday,mon,year,wday,yday,isdst; };
static int rtc_checks(void)
{
    const char *names[]={"/dev/rtc0","/dev/rtc","/dev/misc/rtc"};
    CHECK(mkdir("/dev/misc",0755)==0 || errno==EEXIST);
    for(unsigned i=0;i<3;i++)CHECK(mknod(names[i],S_IFCHR|0600,makedev(10,135))==0 || errno==EEXIST);
    int fd=open(names[0],O_RDONLY);CHECK(fd>=0);
    CHECK(open(names[1],O_RDONLY)==-1 && errno==EBUSY);int copy=dup(fd);CHECK(copy>=0 && close(fd)==0);
    CHECK(open(names[2],O_RDONLY)==-1 && errno==EBUSY);
    struct rtc_time t;CHECK(ioctl(copy,0x80247009UL,&t)==0);
    CHECK(t.year>=120 && t.mon>=0 && t.mon<12 && t.mday>=1 && t.mday<=31 && t.sec>=0 && t.sec<60 && t.hour>=0 && t.hour<24);
    struct tm tm={.tm_sec=t.sec,.tm_min=t.min,.tm_hour=t.hour,.tm_mday=t.mday,.tm_mon=t.mon,.tm_year=t.year};
    time_t hardware=timegm(&tm),now=time(NULL);CHECK(llabs((long long)now-hardware)<=2);
    CHECK(ioctl(copy,0x80247009UL,(void *)1)==-1 && errno==EFAULT);
    CHECK(ioctl(copy,0xf00dUL,NULL)==-1 && errno==ENOTTY);
    pid_t pid=fork();CHECK(pid>=0);if(!pid){if(close(copy))_exit(1);_exit(0);}
    int status;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(open(names[0],O_RDONLY)==-1 && errno==EBUSY);CHECK(close(copy)==0);
    fd=open(names[2],O_RDONLY|O_CLOEXEC);CHECK(fd>=0 && close(fd)==0);
    pid=fork();CHECK(pid>=0);
    if(!pid){
        int owned=open(names[0],O_RDONLY|O_CLOEXEC);if(owned<0)_exit(2);
        execl("/init","/init","rtc-exec",NULL);_exit(3);
    }
    CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    fd=open(names[0],O_RDONLY);CHECK(fd>=0 && close(fd)==0);
    puts("ENV PASS rtc: real UTC, aliases, exclusive OFD, dup/fork, faults and final release");return 0;
}
int main(int argc, char **argv)
{
    if(argc==2 && !strcmp(argv[1],"rtc-exec")){
        int fd=open("/dev/rtc0",O_RDONLY);CHECK(fd>=0 && close(fd)==0);return 0;
    }
    CHECK(mkdir("/dev",0755)==0 || errno==EEXIST);CHECK(mkdir("/proc",0755)==0 || errno==EEXIST);
    CHECK(mount("proc","/proc","proc",0,NULL)==0);
    if(log_checks() || rtc_checks())return 1;
    puts("ENV PASS all");return 0;
}
