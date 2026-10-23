#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>

/* Raw IDs stay separate from libc's opaque timer_t and callback bookkeeping. */
struct native_event { uint64_t value;int32_t signal,notify;int32_t tid;uint8_t padding[44]; };
_Static_assert(sizeof(struct native_event)==64,"native sigevent");
static unsigned failures;
#define CHECK(x) do{if(!(x)){printf("POSIX FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);failures++;}}while(0)
static int error(long value,int code){return value==-1&&errno==code;}
static int create(int clock,const struct native_event *event,int *id)
{return syscall(SYS_timer_create,clock,event,id);}
static int remove_timer(int id){return syscall(SYS_timer_delete,id);}
static int get_timer(int id,struct itimerspec *value){return syscall(SYS_timer_gettime,id,value);}
static void objects(void)
{
    struct native_event none={.notify=SIGEV_NONE};
    for(int clock=0;clock<4;clock++) {
        int id=-1;struct itimerspec value;memset(&value,0x55,sizeof(value));
        CHECK(!create(clock,&none,&id));if(id<0)return;
        CHECK(id>=0&&!get_timer(id,&value)&&!value.it_value.tv_sec&&!value.it_value.tv_nsec&&
            !value.it_interval.tv_sec&&!value.it_interval.tv_nsec);
        CHECK(!syscall(SYS_timer_getoverrun,id));CHECK(!remove_timer(id));
        CHECK(error(get_timer(id,&value),EINVAL)&&error(remove_timer(id),EINVAL));
    }
    int id;CHECK(!create(CLOCK_MONOTONIC,0,&id)&&!remove_timer(id));
    struct native_event event={.signal=SIGRTMIN,.notify=SIGEV_SIGNAL};
    CHECK(!create(CLOCK_MONOTONIC,&event,&id)&&!remove_timer(id));
    event.notify=SIGEV_THREAD;CHECK(!create(CLOCK_MONOTONIC,&event,&id)&&!remove_timer(id));
    event.notify=4;event.tid=syscall(SYS_gettid);CHECK(!create(CLOCK_MONOTONIC,&event,&id)&&!remove_timer(id));
    event.tid=INT32_MAX;CHECK(error(create(CLOCK_MONOTONIC,&event,&id),EINVAL));
    event.notify=SIGEV_SIGNAL;event.signal=0;CHECK(error(create(CLOCK_MONOTONIC,&event,&id),EINVAL));
    event.notify=SIGEV_NONE;CHECK(!create(CLOCK_MONOTONIC,&event,&id)&&!remove_timer(id));
    CHECK(error(create(INT32_MAX,&none,&id),EINVAL));
    puts("POSIX CASE objects OK");
}
static void faults_quota(void)
{
    struct native_event none={.notify=SIGEV_NONE};int id=-1,other=-1;
    CHECK(error(create(1,(void *)UINTPTR_MAX,&id),EFAULT));
    CHECK(error(create(1,&none,(void *)UINTPTR_MAX),EFAULT));
    struct rlimit original;CHECK(!getrlimit(RLIMIT_SIGPENDING,&original));
    struct rlimit one={1,original.rlim_max};CHECK(!setrlimit(RLIMIT_SIGPENDING,&one));
    CHECK(!create(1,&none,&id));CHECK(error(create(1,&none,&other),EAGAIN));
    CHECK(!remove_timer(id)&&!create(1,&none,&other)&&!remove_timer(other));
    CHECK(error(create(1,&none,(void *)UINTPTR_MAX),EFAULT));
    CHECK(!create(1,&none,&id)&&!remove_timer(id));
    CHECK(!setrlimit(RLIMIT_SIGPENDING,&original));
    puts("POSIX CASE faults-quota OK");
}
static int member_id=-1;
static void *creator(void *argument)
{(void)argument;struct native_event none={.notify=SIGEV_NONE};CHECK(!create(1,&none,&member_id));return 0;}
static void lifecycle(void)
{
    pthread_t member;CHECK(!pthread_create(&member,0,creator,0)&&!pthread_join(member,0));
    struct itimerspec value;CHECK(!get_timer(member_id,&value));
    pid_t child=fork();CHECK(child>=0);if(!child){if(!error(get_timer(member_id,&value),EINVAL))_exit(61);
        struct native_event none={.notify=SIGEV_NONE};int id;
        if(create(1,&none,&id))_exit(62);
        char number[16];snprintf(number,sizeof(number),"%d",id);
        execl("/init","/init","--posix-exec",number,(char *)0);_exit(63);}
    int status;CHECK(waitpid(child,&status,0)==child&&!status);
    CHECK(!get_timer(member_id,&value)&&!remove_timer(member_id));
    puts("POSIX CASE lifecycle OK");
}
int main(int argc,char **argv)
{
    if(argc==3&&!strcmp(argv[1],"--posix-exec")){
        struct itimerspec value;return error(get_timer(atoi(argv[2]),&value),EINVAL)?0:64;}
    setvbuf(stdout,0,_IONBF,0);alarm(20);
    struct native_event none={.notify=SIGEV_NONE};int entry=-1;
    CHECK(!create(1,&none,&entry));if(entry<0)return 1;CHECK(!remove_timer(entry));
    objects();faults_quota();lifecycle();
    printf("POSIX END failures=%u\n",failures);return failures?1:0;
}
