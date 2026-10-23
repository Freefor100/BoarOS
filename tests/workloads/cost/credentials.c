/* 同一 ELF 复用既有六个窗口，再比较 root/普通用户的相同文件工作量。 */
#define main existing_cost_main
#include "../../accounting/posix.c"
#undef main
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

static uint64_t credential_samples[4096];
static void credential_report(const char *identity,const char *name,uint64_t start,
    uint64_t cpu,uint64_t bytes)
{
    uint64_t elapsed=queue_clock(CLOCK_MONOTONIC)-start;
    cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
    qsort(credential_samples,4096,sizeof(uint64_t),queue_compare);
    struct rusage usage;CHECK(!getrusage(RUSAGE_SELF,&usage));
    printf("CREDENTIAL-PERF identity=%s name=%s count=4096 bytes=%llu ns=%llu p50=%llu p95=%llu max=%llu cpu_ns=%llu rss_kib=%ld\n",
        identity,name,(unsigned long long)bytes,(unsigned long long)elapsed,
        (unsigned long long)credential_samples[2048],(unsigned long long)credential_samples[3891],
        (unsigned long long)credential_samples[4095],(unsigned long long)cpu,usage.ru_maxrss);
}
static void credential_file_windows(const char *identity)
{
    static const char *names[]={"stat","open","fstat","fd-rw-sync","ids-caps"};
    unsigned char input[4096],output[4096];
    for(unsigned i=0;i<4096;i++)input[i]=(unsigned char)(i*29+7);
    int fd=open("/credential-cost/data",O_RDWR);CHECK(fd>=0);
    struct stat expected;CHECK(!fstat(fd,&expected));
    for(unsigned mode=0;mode<5;mode++) {
        uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID),start=queue_clock(CLOCK_MONOTONIC);
        for(unsigned i=0;i<4096;i++) {
            uint64_t one=queue_clock(CLOCK_MONOTONIC);
            struct stat st;
            if(mode==0)CHECK(!stat("/credential-cost/data",&st));
            if(mode==1){int opened=open("/credential-cost/data",O_RDONLY);CHECK(opened>=0&&!fstat(opened,&st)&&!close(opened));}
            if(mode==2)CHECK(!fstat(fd,&st));
            if(mode<3)CHECK(st.st_ino==expected.st_ino&&st.st_uid==1000&&st.st_gid==1000&&st.st_size==4096);
            if(mode==3){
                CHECK(pwrite(fd,input,4096,0)==4096);
                output[0]=(unsigned char)~input[0];CHECK(pread(fd,output,4096,0)==4096&&!memcmp(input,output,4096));
            }
            if(mode==4){
                struct {unsigned version;int pid;} header={0x20080522,0};
                struct {unsigned effective,permitted,inheritable;} data[2];
                CHECK(getuid()==(identity[0]=='r'?0:1000)&&getgid()==(identity[0]=='r'?0:1000));
                CHECK(!syscall(SYS_capget,&header,data));
                CHECK(data[0].effective==(identity[0]=='r'?UINT32_MAX:0));
            }
            credential_samples[i]=queue_clock(CLOCK_MONOTONIC)-one;
        }
        if(mode==3)CHECK(!fsync(fd));
        credential_report(identity,names[mode],start,cpu,mode==3?UINT64_C(4096)*8192:0);
    }
    CHECK(!close(fd));
}
int main(void)
{
    CHECK(!existing_cost_main());
    cpu_set_t affinity;CPU_ZERO(&affinity);CPU_SET(0,&affinity);CHECK(!sched_setaffinity(0,sizeof(affinity),&affinity));
    CHECK(!mkdir("/credential-cost",0755));
    int fd=open("/credential-cost/data",O_CREAT|O_EXCL|O_RDWR,0644);CHECK(fd>=0);
    unsigned char data[4096]={0};CHECK(write(fd,data,sizeof(data))==sizeof(data));
    CHECK(!fchown(fd,1000,1000)&&!fsync(fd)&&!close(fd));
    credential_file_windows("root");
    uid_t r,e,s;errno=0;long supported=syscall(SYS_getresuid,&r,&e,&s);
    if(supported==-1){CHECK(errno==ENOSYS);puts("CREDENTIAL-PERF USER unavailable");}
    else {
        CHECK(!supported);fflush(NULL);pid_t child=fork();CHECK(child>=0);
        if(!child){CHECK(!setgid(1000)&&!setuid(1000));credential_file_windows("user");_exit(0);}
        int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
        puts("CREDENTIAL-PERF USER END 5");
    }
    CHECK(!unlink("/credential-cost/data")&&!rmdir("/credential-cost"));
    puts("CREDENTIAL-PERF END 11");return 0;
}
