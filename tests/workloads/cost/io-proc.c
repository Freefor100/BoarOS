#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"IOPROC FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); } } while (0)
static uint64_t samples[4096];
static unsigned metrics;
static uint64_t clock_ns(clockid_t id) { struct timespec t; CHECK(!clock_gettime(id,&t)); return (uint64_t)t.tv_sec*1000000000U+t.tv_nsec; }
static int cmp64(const void *a,const void *b) { uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b; return (x>y)-(x<y); }
static int cmpid(const void *a,const void *b) { pid_t x=*(const pid_t *)a,y=*(const pid_t *)b; return (x>y)-(x<y); }
static void report(const char *name,unsigned count,uint64_t start,uint64_t cpu,uint64_t bytes)
{
    uint64_t elapsed=clock_ns(CLOCK_MONOTONIC)-start,used=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
    struct rusage u; CHECK(!getrusage(RUSAGE_SELF,&u)); qsort(samples,count,sizeof(*samples),cmp64);
    printf("IOPROC METRIC name=%s count=%u bytes=%llu ns=%llu p50=%llu p95=%llu max=%llu cpu_ns=%llu rss_kib=%ld\n",name,count,(unsigned long long)bytes,(unsigned long long)elapsed,(unsigned long long)samples[count/2],(unsigned long long)samples[(count*95)/100],(unsigned long long)samples[count-1],(unsigned long long)used,u.ru_maxrss);metrics++;
}
static int ready[2],gate[2];
static void *park(void *unused)
{
    (void)unused; pid_t tid=(pid_t)syscall(SYS_gettid); char done;
    CHECK(write(ready[1],&tid,sizeof(tid))==sizeof(tid)); CHECK(read(gate[0],&done,1)==0); return NULL;
}
static pid_t receive_id(void) { pid_t id; CHECK(read(ready[0],&id,sizeof(id))==sizeof(id)); return id; }
static void wait_sleep(pid_t id,int thread)
{
    char path[96],text[512];
    if(thread)snprintf(path,sizeof(path),"/proc/self/task/%d/stat",id);
    else snprintf(path,sizeof(path),"/proc/%d/stat",id);
    uint64_t deadline=clock_ns(CLOCK_MONOTONIC)+10000000000ULL;
    for(;;) {
        int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,text,sizeof(text)-1);CHECK(n>0&&!close(fd));text[n]=0;
        char *end=strrchr(text,')');CHECK(end&&end[1]==' ');
        if(end[2]=='S')return;
        CHECK(clock_ns(CLOCK_MONOTONIC)<deadline);sched_yield();
    }
}
static void proc_window(unsigned threads,unsigned others)
{
    pthread_t workers[64]; pid_t children[64],ids[65];
    CHECK(!pipe(ready)&&!pipe(gate));
    for(unsigned i=0;i<others;i++) {
        children[i]=fork();CHECK(children[i]>=0);
        if(!children[i]) { CHECK(!close(gate[1])); park(NULL); _exit(0); }
    }
    for(unsigned i=0;i<others;i++) (void)receive_id();
    ids[0]=(pid_t)syscall(SYS_gettid);
    for(unsigned i=0;i<threads;i++) CHECK(!pthread_create(&workers[i],NULL,park,NULL));
    for(unsigned i=0;i<threads;i++) ids[i+1]=receive_id();
    for(unsigned i=1;i<=threads;i++)wait_sleep(ids[i],1);
    for(unsigned i=0;i<others;i++)wait_sleep(children[i],0);
    qsort(ids,threads+1,sizeof(*ids),cmpid);
    DIR *dir=opendir("/proc/self/task");CHECK(dir);
    uint64_t cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID),start=clock_ns(CLOCK_MONOTONIC);
    for(unsigned round=0;round<64;round++) {
        uint64_t one=clock_ns(CLOCK_MONOTONIC);rewinddir(dir);unsigned found=0;unsigned char seen[65]={0};struct dirent *entry;
        while((entry=readdir(dir))) {
            if(entry->d_name[0]=='.')continue;
            char *end;long n=strtol(entry->d_name,&end,10);CHECK(!*end&&n>0);
            pid_t id=(pid_t)n;pid_t *match=bsearch(&id,ids,threads+1,sizeof(*ids),cmpid);CHECK(match);size_t index=(size_t)(match-ids);CHECK(!seen[index]);seen[index]=1;found++;
        }
        CHECK(found==threads+1);samples[round]=clock_ns(CLOCK_MONOTONIC)-one;
    }
    char name[80];snprintf(name,sizeof(name),"proc-enum-t%u-p%u",threads,others);report(name,64,start,cpu,0);CHECK(!closedir(dir));
    char path[80],data[4096];snprintf(path,sizeof(path),"/proc/self/task/%d/status",ids[threads]);
    cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID);start=clock_ns(CLOCK_MONOTONIC);
    for(unsigned round=0;round<256;round++) {
        uint64_t one=clock_ns(CLOCK_MONOTONIC);int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,data,sizeof(data)-1);CHECK(n>0);data[n]=0;CHECK(strstr(data,"SigIgn:")&&strstr(data,"SigCgt:")&&!close(fd));samples[round]=clock_ns(CLOCK_MONOTONIC)-one;
    }
    snprintf(name,sizeof(name),"proc-status-t%u-p%u",threads,others);report(name,256,start,cpu,0);
    CHECK(!close(gate[1]));for(unsigned i=0;i<threads;i++)CHECK(!pthread_join(workers[i],NULL));
    for(unsigned i=0;i<others;i++) { int status;CHECK(waitpid(children[i],&status,0)==children[i]&&WIFEXITED(status)&&!WEXITSTATUS(status)); }
    CHECK(!close(gate[0])&&!close(ready[0])&&!close(ready[1]));
}
static void vectors(const char *root)
{
    unsigned char source[4096],data[4096];for(unsigned i=0;i<sizeof(source);i++)source[i]=(unsigned char)(i*29+7);
    char path[160];snprintf(path,sizeof(path),"%s/data",root);int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0&&write(fd,source,sizeof(source))==sizeof(source));
    const unsigned counts[]={1,8,9,64};
    for(unsigned mode=0;mode<2;mode++)for(unsigned k=0;k<4;k++) {
        unsigned count=counts[k];struct iovec iov[64];size_t offset=0;
        for(unsigned i=0;i<count;i++) {size_t n=(sizeof(data)-offset)/(count-i);iov[i]=(struct iovec){data+offset,n};offset+=n;}
        int sockets[2]={-1,-1};if(mode)CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,sockets));
        struct msghdr msg={.msg_iov=iov,.msg_iovlen=count};
        uint64_t cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID),start=clock_ns(CLOCK_MONOTONIC);
        for(unsigned round=0;round<4096;round++) {
            uint64_t one=clock_ns(CLOCK_MONOTONIC);
            for(unsigned v=0;v<count;v++)*(unsigned char *)iov[v].iov_base=(unsigned char)~source[(unsigned char *)iov[v].iov_base-data];
            if(mode) { CHECK(send(sockets[0],source,sizeof(source),0)==sizeof(source));CHECK(recvmsg(sockets[1],&msg,0)==sizeof(data)); }
            else {CHECK(lseek(fd,0,SEEK_SET)==0);CHECK(readv(fd,iov,(int)count)==sizeof(data));}
            CHECK(!memcmp(data,source,sizeof(data)));samples[round]=clock_ns(CLOCK_MONOTONIC)-one;
        }
        char name[64];snprintf(name,sizeof(name),"%s-v%u",mode?"recvmsg":"readv",count);report(name,4096,start,cpu,16U*1024*1024);
        if(mode)CHECK(!close(sockets[0])&&!close(sockets[1]));
    }
    CHECK(!close(fd)&&!unlink(path));
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);int host=argc>1&&!strcmp(argv[1],"--host");
    cpu_set_t mask;CPU_ZERO(&mask);CHECK(!sched_getaffinity(0,sizeof(mask),&mask));unsigned first=0;while(first<CPU_SETSIZE&&!CPU_ISSET(first,&mask))first++;CHECK(first<CPU_SETSIZE);CPU_ZERO(&mask);CPU_SET(first,&mask);CHECK(!sched_setaffinity(0,sizeof(mask),&mask));
    char root[128];snprintf(root,sizeof(root),host?"/dev/shm/boaros-ioproc-%d":"/ioproc-%d",getpid());CHECK(!mkdir(root,0700));if(!host)CHECK(!mount("none",root,"tmpfs",0,NULL));
    int own_proc=access("/proc/self/task",F_OK)!=0;
    if(own_proc) {CHECK(!mkdir("/proc",0755)||errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,NULL));}
    printf("IOPROC BEGIN affinity=%u\n",first);
    for(unsigned t=8;t<=64;t*=8)for(unsigned p=0;p<=64;p+=64)proc_window(t,p);
    vectors(root);if(own_proc)CHECK(!umount("/proc"));if(!host)CHECK(!umount(root));CHECK(!rmdir(root));CHECK(metrics==16);puts("IOPROC END 16");return 0;
}
