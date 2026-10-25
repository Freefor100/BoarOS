/* The old completed work is shared verbatim with the resource-limit workload. */
#define RESOURCE_COST_EMBEDDED 1
#include "rlimits-pidfd.c"
#include <sys/mount.h>
#ifndef MLOCK_ONFAULT
#define MLOCK_ONFAULT 1
#endif
static uint64_t memlock_bytes(void)
{
    char data[4096];int fd=open("/proc/self/status",O_RDONLY);CHECK(fd>=0);
    ssize_t n=read(fd,data,sizeof(data)-1);CHECK(n>0&&!close(fd));data[n]=0;
    char *field=strstr(data,"VmLck:");CHECK(field);return strtoull(field+6,0,10)*1024;
}
static unsigned memlock_resident(unsigned char *p,size_t bytes)
{
    unsigned char vector[1024];CHECK(bytes/4096<=sizeof(vector)&&!mincore(p,bytes,vector));
    unsigned result=0;for(unsigned i=0;i<bytes/4096;i++)result+=vector[i]&1;return result;
}
struct memlock_work {
    pthread_barrier_t gate;
    unsigned cpus,rounds;
    uint64_t begin[4],end[4],cpu[4];
};
struct memlock_actor {struct memlock_work *work;unsigned id;};
static void *memlock_run(void *argument)
{
    struct memlock_actor *a=argument;struct memlock_work *w=a->work;unsigned id=a->id;
    queue_pin(id%w->cpus);queue_gate(&w->gate);queue_gate(&w->gate);
    uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);w->begin[id]=now();
    for(unsigned i=0;i<w->rounds;i++) {
        uint64_t begin=now();unsigned char *p=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(p!=MAP_FAILED&&!mlock(p,65536));p[0]=(unsigned char)i;CHECK(p[0]==(unsigned char)i);
        CHECK(!munlock(p,65536)&&!munmap(p,65536));mm_msg_samples[id*w->rounds+i]=now()-begin;
    }
    w->end[id]=now();w->cpu[id]=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;return 0;
}
static int memlock_cost_control=-1;
static void memlock_cost_begin(void)
{
    if(memlock_cost_control>=0)CHECK(write(memlock_cost_control,"begin\n",6)==6);
}
static void memlock_cost_end(const char *name)
{
    if(memlock_cost_control<0)return;
    CHECK(write(memlock_cost_control,"end\n",4)==4);
    int fd=open("/proc/boaros_cost",O_RDONLY);CHECK(fd>=0);
    printf("MEMLOCK-DIAG BEGIN %s\n",name);char block[4096];ssize_t n;
    while((n=read(fd,block,sizeof(block)))>0)CHECK(fwrite(block,1,(size_t)n,stdout)==(size_t)n);
    CHECK(!n&&!close(fd));printf("MEMLOCK-DIAG END %s\n",name);
}
static void memlock_initial(void)
{
    for(unsigned mode=0;mode<2;mode++) {
        size_t bytes=4*1024*1024;unsigned char *p=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);
        CHECK(!memlock_resident(p,bytes));uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID),begin=now();
        CHECK(!syscall(SYS_mlock2,p,bytes,mode?MLOCK_ONFAULT:0));
        uint64_t elapsed=now()-begin;cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;mm_msg_samples[0]=elapsed;
        CHECK(memlock_bytes()==bytes);unsigned resident=memlock_resident(p,bytes);CHECK(resident==(mode?0U:1024U));
        mm_msg_report(mode?"onfault-set-4m":"immediate-set-4m",1,bytes,elapsed,cpu);
        cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);begin=now();
        for(unsigned i=0;i<1024;i++){p[i*4096]=(unsigned char)i;CHECK(p[i*4096]==(unsigned char)i);}
        elapsed=now()-begin;cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;mm_msg_samples[0]=elapsed;
        CHECK(memlock_resident(p,bytes)==1024);mm_msg_report(mode?"onfault-touch-4m":"immediate-touch-4m",1,bytes,elapsed,cpu);
        printf("MEMLOCK-PERF state=%s locked=%zu before=%u after=1024\n",mode?"onfault":"immediate",bytes,resident);
        CHECK(!munlock(p,bytes)&&!munmap(p,bytes)&&!memlock_bytes());
    }
}
static void memlock_shared(unsigned cpus,unsigned rounds)
{
    struct memlock_work work={.cpus=cpus,.rounds=rounds};CHECK(!pthread_barrier_init(&work.gate,0,5));
    pthread_t threads[4];struct memlock_actor actors[4];
    for(unsigned i=0;i<4;i++){actors[i]=(struct memlock_actor){&work,i};CHECK(!pthread_create(&threads[i],0,memlock_run,&actors[i]));}
    queue_gate(&work.gate);memlock_cost_begin();queue_gate(&work.gate);uint64_t begin=UINT64_MAX,end=0,cpu=0;
    for(unsigned i=0;i<4;i++){CHECK(!pthread_join(threads[i],0));if(work.begin[i]<begin)begin=work.begin[i];if(work.end[i]>end)end=work.end[i];cpu+=work.cpu[i];}
    CHECK(!pthread_barrier_destroy(&work.gate)&&!memlock_bytes());
    memlock_cost_end("shared-mm");
    mm_msg_report("lock-unlock-map-four",4*rounds,(uint64_t)4*rounds*65536,end-begin,cpu);
}
struct memlock_process_result {
    uint64_t begin,end,cpu,samples[256];
};
static void memlock_processes(unsigned cpus,unsigned rounds)
{
    size_t bytes=4*sizeof(struct memlock_process_result);
    struct memlock_process_result *results=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    CHECK(results!=MAP_FAILED);memset(results,0,bytes);
    int ready[2],gate[2];CHECK(!pipe(ready)&&!pipe(gate));pid_t children[4];
    for(unsigned id=0;id<4;id++) {
        children[id]=fork();CHECK(children[id]>=0);
        if(!children[id]) {
            queue_pin(id%cpus);char token='r';CHECK(write(ready[1],&token,1)==1&&read(gate[0],&token,1)==1);
            struct memlock_process_result *r=&results[id];uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);r->begin=now();
            for(unsigned i=0;i<rounds;i++) {
                uint64_t begin=now();unsigned char *p=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
                CHECK(p!=MAP_FAILED&&!mlock(p,65536));p[0]=(unsigned char)i;CHECK(p[0]==(unsigned char)i);
                CHECK(!munlock(p,65536)&&!munmap(p,65536));r->samples[i]=now()-begin;
            }
            r->end=now();r->cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;_exit(0);
        }
    }
    char token;for(unsigned i=0;i<4;i++)CHECK(read(ready[0],&token,1)==1);
    memlock_cost_begin();CHECK(write(gate[1],"gggg",4)==4);
    for(unsigned i=0;i<4;i++){int status;CHECK(waitpid(children[i],&status,0)==children[i]&&WIFEXITED(status)&&!WEXITSTATUS(status));}
    memlock_cost_end("independent-mm");
    uint64_t begin=UINT64_MAX,end=0,cpu=0;
    for(unsigned id=0;id<4;id++) {
        struct memlock_process_result *r=&results[id];CHECK(r->end>r->begin&&r->cpu);
        if(r->begin<begin)begin=r->begin;
        if(r->end>end)end=r->end;
        cpu+=r->cpu;
        for(unsigned i=0;i<rounds;i++){CHECK(r->samples[i]);mm_msg_samples[id*rounds+i]=r->samples[i];}
    }
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(gate[0])&&!close(gate[1])&&!munmap(results,bytes));
    mm_msg_report("lock-unlock-map-processes",4*rounds,(uint64_t)4*rounds*65536,end-begin,cpu);
}
int main(void)
{
    unsigned mode,cpus;setvbuf(stdout,0,_IONBF,0);FILE *config=fopen("/account-resource","r");
    CHECK(config&&fscanf(config,"%u %u",&mode,&cpus)==2&&!fclose(config)&&mode<=3&&(cpus==1||cpus==2||cpus==4));queue_pin(0);
    CHECK(!mkdir("/proc",0755)||errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,0));
    if(mode==3){memlock_cost_control=open("/proc/boaros_cost_control",O_WRONLY);CHECK(memlock_cost_control>=0);}
    if(mode==0||mode==2){mm_msg_old(cpus);mm_msg_memory();resource_file();}
    if(mode==1||mode==2)memlock_initial();
    if(mode){unsigned rounds=mode==3?32:256;memlock_shared(cpus,rounds);memlock_processes(cpus,rounds);}
    if(memlock_cost_control>=0)CHECK(!close(memlock_cost_control));
    printf("MEMLOCK-PERF END %u\n",mm_msg_metrics);
    if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
