/* One ELF keeps the old completed work and the new process-handle work fixed. */
#define MM_MSG_EMBEDDED 1
#include "mm-msg.c"
#include <poll.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#define SYS_pidfd_getfd 438
#endif

static unsigned char resource_data[4096],resource_copy[4096];
static void resource_file(void)
{
    CHECK(!mkdir("/resource-cost",0700));
    int fd=open("/resource-cost/data",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
    for(unsigned i=0;i<sizeof(resource_data);i++)resource_data[i]=(unsigned char)(i*17+31);
    uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID),begin=now();
    for(unsigned i=0;i<4096;i++) {
        uint64_t start=now();
        CHECK(pwrite(fd,resource_data,sizeof(resource_data),0)==sizeof(resource_data));
        resource_copy[0]=(unsigned char)~resource_data[0];
        CHECK(pread(fd,resource_copy,sizeof(resource_copy),0)==sizeof(resource_copy));
        CHECK(!memcmp(resource_data,resource_copy,sizeof(resource_data)));
        mm_msg_samples[i]=now()-start;
    }
    CHECK(!fsync(fd));uint64_t elapsed=now()-begin;cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
    CHECK(!close(fd)&&!unlink("/resource-cost/data")&&!rmdir("/resource-cost"));
    mm_msg_report("file-positioned-rw-4k",4096,UINT64_C(4096)*8192,elapsed,cpu);
}
#ifndef RESOURCE_COST_EMBEDDED
struct resource_handles {
    pthread_barrier_t gate;
    unsigned cpus,mode;
    int fd;
    ino_t ino;
    uint64_t begin[4],end[4],cpu[4];
    unsigned completed[4];
};
struct resource_actor {struct resource_handles *work;unsigned id;};
static void resource_reap(int pidfd,pid_t child)
{
    struct pollfd p={.fd=pidfd,.events=POLLIN};CHECK(poll(&p,1,-1)==1&&(p.revents&POLLIN));
    siginfo_t info={0};CHECK(!waitid(P_PIDFD,(id_t)pidfd,&info,WEXITED));
    CHECK(info.si_pid==child&&info.si_code==CLD_EXITED&&info.si_status==37&&!close(pidfd));
}
static void *resource_handle_run(void *argument)
{
    struct resource_actor *a=argument;struct resource_handles *w=a->work;unsigned id=a->id;
    queue_pin(id%w->cpus);
    int gate[2]={-1,-1},ready[2]={-1,-1},pidfd=-1;pid_t target=0;
    if(w->mode) {
        CHECK(!pipe(gate)&&!pipe(ready));target=fork();CHECK(target>=0);
        if(!target){CHECK(!close(gate[1])&&!close(ready[0]));char c='r';CHECK(write(ready[1],&c,1)==1&&read(gate[0],&c,1)==1);_exit(37);}
        CHECK(!close(gate[0])&&!close(ready[1]));char c;CHECK(read(ready[0],&c,1)==1&&c=='r'&&!close(ready[0]));
        pidfd=(int)syscall(SYS_pidfd_open,target,0);CHECK(pidfd>=0);
    }
    queue_gate(&w->gate);queue_gate(&w->gate);
    uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);w->begin[id]=now();
    unsigned rounds=w->mode?1024:64;
    for(unsigned i=0;i<rounds;i++) {
        uint64_t start=now();
        if(!w->mode) {
            int release[2];CHECK(!pipe(release));pid_t child=fork();CHECK(child>=0);
            if(!child){CHECK(!close(release[1]));char c;CHECK(read(release[0],&c,1)==1);_exit(37);}
            CHECK(!close(release[0]));int handle=(int)syscall(SYS_pidfd_open,child,0);CHECK(handle>=0);
            CHECK(write(release[1],"x",1)==1&&!close(release[1]));resource_reap(handle,child);
        } else {
            int received=(int)syscall(SYS_pidfd_getfd,pidfd,w->fd,0);CHECK(received>=0);
            struct stat st;unsigned char bytes[32];CHECK(!fstat(received,&st)&&S_ISREG(st.st_mode)&&st.st_ino==w->ino&&st.st_size==4096);
            CHECK(pread(received,bytes,sizeof(bytes),0)==sizeof(bytes)&&!memcmp(bytes,resource_data,sizeof(bytes)));
            if(!i)CHECK(fcntl(received,F_GETFD)==FD_CLOEXEC);
            CHECK(!close(received));
        }
        mm_msg_samples[id*rounds+i]=now()-start;w->completed[id]++;
    }
    w->end[id]=now();w->cpu[id]=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
    if(w->mode){CHECK(write(gate[1],"x",1)==1&&!close(gate[1]));resource_reap(pidfd,target);}
    return 0;
}
static void resource_handles(unsigned cpus)
{
    CHECK(!mkdir("/resource-cost",0700));int file=open("/resource-cost/data",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(file>=0);
    for(unsigned i=0;i<sizeof(resource_data);i++)resource_data[i]=(unsigned char)(i*17+31);
    CHECK(write(file,resource_data,sizeof(resource_data))==sizeof(resource_data)&&!fsync(file));struct stat st;CHECK(!fstat(file,&st));
    for(unsigned mode=0;mode<2;mode++) {
        struct resource_handles w={.cpus=cpus,.mode=mode,.fd=file,.ino=st.st_ino};
        CHECK(!pthread_barrier_init(&w.gate,0,5));pthread_t threads[4];struct resource_actor actors[4];
        for(unsigned id=0;id<4;id++){actors[id]=(struct resource_actor){&w,id};CHECK(!pthread_create(&threads[id],0,resource_handle_run,&actors[id]));}
        queue_gate(&w.gate);queue_gate(&w.gate);uint64_t begin=UINT64_MAX,end=0,cpu=0;
        for(unsigned id=0;id<4;id++){CHECK(!pthread_join(threads[id],0));CHECK(w.completed[id]==(mode?1024U:64U));if(w.begin[id]<begin)begin=w.begin[id];if(w.end[id]>end)end=w.end[id];cpu+=w.cpu[id];}
        CHECK(!pthread_barrier_destroy(&w.gate));mm_msg_report(mode?"pidfd-getfd-file":"pidfd-fork-wait-reap",mode?4096:256,mode?4096*32:0,end-begin,cpu);
    }
    CHECK(!close(file)&&!unlink("/resource-cost/data")&&!rmdir("/resource-cost"));
}
int main(void)
{
    unsigned mode,cpus;setvbuf(stdout,0,_IONBF,0);FILE *config=fopen("/account-resource","r");
    CHECK(config&&fscanf(config,"%u %u",&mode,&cpus)==2&&!fclose(config)&&mode<=1&&(cpus==1||cpus==2||cpus==4));queue_pin(0);
    printf("RESOURCE-PERF BEGIN mode=%u cpus=%u\n",mode,cpus);
    if(mode)resource_handles(cpus);
    else {mm_msg_old(cpus);mm_msg_memory();resource_file();}
    printf("RESOURCE-PERF END %u\n",mm_msg_metrics);
    if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}

#endif
