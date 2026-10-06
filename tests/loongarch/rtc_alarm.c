#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>
/* Fixed Linux include/uapi/linux/rtc.h, LP64 generic ioctl encoding. */
struct rtc_time { int sec,min,hour,mday,mon,year,wday,yday,isdst; };
struct rtc_wkalrm { unsigned char enabled,pending;struct rtc_time time; };
#define CHECK(x) do { if(!(x)){fprintf(stderr,"rtc-alarm:%d: %s errno=%d\n",__LINE__,#x,errno);return 1;} } while(0)
static time_t epoch(struct rtc_time *value)
{
    struct tm tm={.tm_sec=value->sec,.tm_min=value->min,.tm_hour=value->hour,
        .tm_mday=value->mday,.tm_mon=value->mon,.tm_year=value->year};
    return timegm(&tm);
}
static struct rtc_time fields(time_t value)
{
    struct tm tm;gmtime_r(&value,&tm);
    return (struct rtc_time){tm.tm_sec,tm.tm_min,tm.tm_hour,tm.tm_mday,
        tm.tm_mon,tm.tm_year,tm.tm_wday,tm.tm_yday,tm.tm_isdst};
}
int main(void)
{
    CHECK(mount("devtmpfs","/dev","devtmpfs",0,0)==0);
    int fd=open("/dev/rtc0",O_RDONLY|O_NONBLOCK);CHECK(fd>=0);
    struct rtc_time current;CHECK(ioctl(fd,0x80247009,&current)==0);
    CHECK(current.year==126 || current.year==127);
    CHECK(ioctl(fd,0x80247009,(void *)1)==-1 && errno==EFAULT);
    CHECK(ioctl(fd,0xdeadbeef,0)==-1 && errno==ENOTTY);
    struct rtc_time invalid=current;invalid.mon=12;
    CHECK(ioctl(fd,0x4024700a,&invalid)==-1 && errno==EINVAL);
    struct rtc_time set={.sec=58,.min=59,.hour=23,.mday=31,.mon=11,.year=126};
    CHECK(ioctl(fd,0x4024700a,&set)==0);
    CHECK(ioctl(fd,0x80247009,&current)==0 && epoch(&current)==epoch(&set));
    for(unsigned round=0;round<2;round++) {
        CHECK(ioctl(fd,0x80247009,&current)==0);
        struct rtc_wkalrm alarm={.enabled=1,.time=fields(epoch(&current)+2)};
        CHECK(ioctl(fd,0x4028700f,&alarm)==0);
        struct rtc_wkalrm actual={0};CHECK(ioctl(fd,0x80287010,&actual)==0);
        CHECK(actual.enabled && epoch(&actual.time)==epoch(&alarm.time));
        /* Block/RNG and UART remain active while the independent alarm arrives. */
        int data=open("/rtc-irq-data",O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(data>=0);
        unsigned char buffer[4096];memset(buffer,0x53,sizeof(buffer));
        for(unsigned i=0;i<16;i++)CHECK(write(data,buffer,sizeof(buffer))==(ssize_t)sizeof(buffer));
        CHECK(fsync(data)==0 && lseek(data,0,SEEK_SET)==0);
        for(unsigned i=0;i<16;i++) {CHECK(read(data,buffer,sizeof(buffer))==(ssize_t)sizeof(buffer));for(unsigned j=0;j<sizeof(buffer);j++)CHECK(buffer[j]==0x53);}
        CHECK(close(data)==0 && getrandom(buffer,sizeof(buffer),0)==(ssize_t)sizeof(buffer));
        printf("RTC alarm round %u waiting with block/RNG progress\n",round);fflush(stdout);
        struct pollfd wait={.fd=fd,.events=POLLIN};CHECK(poll(&wait,1,5000)==1 && (wait.revents&POLLIN));
        unsigned long event=0;CHECK(read(fd,&event,sizeof(event))==(ssize_t)sizeof(event));
        CHECK((event&0xa0)==0xa0 && (event>>8)>=1);
        CHECK(ioctl(fd,0x80247009,&current)==0 && epoch(&current)>=epoch(&alarm.time));
        CHECK(read(fd,&event,sizeof(event))==-1 && errno==EAGAIN);
        wait.revents=0;CHECK(poll(&wait,1,250)==0);
    }
    CHECK(current.year==127 && current.mon==0 && current.mday==1);
    CHECK(ioctl(fd,0x7002,0)==0 && close(fd)==0 && umount("/dev")==0);
    puts("LS7A Linux actual alarm IRQ, rearm, UTC rollover, errors and mixed device progress PASS");
    return 42;
}
