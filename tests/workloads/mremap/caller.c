#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"MREMAP FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); } } while (0)
static unsigned cpus, cpu_ids[4], cases;
static size_t page;
static void passed(const char *name)
{ cases++; printf("MREMAP CASE %s PASS\n",name); }
static void pin(unsigned index)
{
    cpu_set_t wanted, actual;
    CPU_ZERO(&wanted); CPU_SET(cpu_ids[index],&wanted);
    CHECK(!sched_setaffinity(0,sizeof(wanted),&wanted));
    CPU_ZERO(&actual); CHECK(!sched_getaffinity(0,sizeof(actual),&actual));
    CHECK(CPU_EQUAL(&wanted,&actual));
}
static unsigned char pattern(size_t offset, unsigned seed)
{ return (unsigned char)((offset*29+(offset>>8)*7+seed*43)^0xa5); }
static void fill(unsigned char *p,size_t length,unsigned seed)
{ for(size_t i=0;i<length;i++)p[i]=pattern(i,seed); }
static void verify(const volatile unsigned char *p,size_t length,unsigned seed)
{ for(size_t i=0;i<length;i++)CHECK(p[i]==pattern(i,seed)); }
static uint64_t now(void)
{
    struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}
static uint64_t cpu_ns(const struct rusage *u)
{ return ((uint64_t)u->ru_utime.tv_sec+u->ru_stime.tv_sec)*1000000000ULL+((uint64_t)u->ru_utime.tv_usec+u->ru_stime.tv_usec)*1000ULL; }
static void realloc_case(void)
{
    const size_t small=8*1024*1024, large=16*1024*1024;
    pin(0); unsigned char *p=malloc(small); CHECK(p); fill(p,small,0);
    struct rusage begin,end; CHECK(!getrusage(RUSAGE_SELF,&begin));
    uint64_t start=now();
    for(unsigned round=0;round<8;round++) {
        unsigned char *next=realloc(p,large);
        if(!next) { verify(p,small,round); free(p); CHECK(next); }
        p=next; verify(p,small,round); fill(p,large,round+1);
        next=realloc(p,small);
        if(!next) { verify(p,large,round+1); free(p); CHECK(next); }
        p=next; verify(p,small,round+1);
    }
    uint64_t elapsed=now()-start; CHECK(!getrusage(RUSAGE_SELF,&end));
    /* The impossible allocation checks libc's failure contract outside timing. */
    volatile size_t impossible=SIZE_MAX-4095;
    errno=0; unsigned char *failed=realloc(p,impossible);
    CHECK(!failed&&errno==ENOMEM); verify(p,small,8); free(p);
    printf("MREMAP METRIC realloc rounds=8 small=%zu large=%zu wall_ns=%llu cpu_ns=%llu rss_hwm_before_kib=%ld rss_hwm_after_kib=%ld rss_scope=process-lifetime-high-water verification=bytewise setup_cleanup=excluded\n",
           small,large,(unsigned long long)elapsed,(unsigned long long)(cpu_ns(&end)-cpu_ns(&begin)),begin.ru_maxrss,end.ru_maxrss);
    printf("MREMAP CPU realloc index=0 affinity=%u content=PASS failure-original=PASS\n",cpu_ids[0]);
    passed("realloc");
}

struct remote { unsigned index,ready,go,verified,commit,done; unsigned char *old,*moved; };
static void wait_word(unsigned *word,unsigned index)
{
    while(!__atomic_load_n(word,__ATOMIC_ACQUIRE)) {
        /* CPU0 must yield to the mover on UP; remote CPUs retain their TLB. */
        if(index==0)sched_yield();
    }
}
static void *remote_reader(void *opaque)
{
    struct remote *r=opaque; pin(r->index); verify(r->old,page,11);
    __atomic_store_n(&r->ready,1,__ATOMIC_RELEASE);
    wait_word(&r->go,r->index);
    verify(r->old,page,22); verify(r->moved,page,11);
    __atomic_store_n(&r->verified,1,__ATOMIC_RELEASE);
    wait_word(&r->commit,r->index);
    r->old[r->index]=(unsigned char)(0x30+r->index);
    r->moved[r->index]=(unsigned char)(0x70+r->index);
    __atomic_store_n(&r->done,1,__ATOMIC_RELEASE);
    return 0;
}
static void remote_case(void)
{
    pin(0);
    unsigned char *old=mmap(0,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    unsigned char *target=mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(old!=MAP_FAILED&&target!=MAP_FAILED); fill(old,page,11);
    struct remote r[4]={0}; pthread_t threads[4]; unsigned first=cpus>1?1:0;
    for(unsigned i=first;i<cpus;i++) {
        r[i]=(struct remote){.index=i,.old=old,.moved=target};
        CHECK(!pthread_create(&threads[i],0,remote_reader,&r[i]));
    }
    for(unsigned i=first;i<cpus;i++)wait_word(&r[i].ready,0);
    CHECK(mremap(old,page,page,MREMAP_MAYMOVE|MREMAP_FIXED,target)==target);
    CHECK(mmap(old,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==old);
    fill(old,page,22); verify(target,page,11);
    /* All readers validate before any reader writes to their disjoint bytes. */
    for(unsigned i=first;i<cpus;i++)__atomic_store_n(&r[i].go,1,__ATOMIC_RELEASE);
    for(unsigned i=first;i<cpus;i++)wait_word(&r[i].verified,0);
    for(unsigned i=first;i<cpus;i++)__atomic_store_n(&r[i].commit,1,__ATOMIC_RELEASE);
    for(unsigned i=first;i<cpus;i++) {
        wait_word(&r[i].done,0); CHECK(!pthread_join(threads[i],0));
    }
    for(size_t n=0;n<page;n++) {
        int changed=n>=first&&n<cpus;
        CHECK(old[n]==(changed?(unsigned char)(0x30+n):pattern(n,22)));
        CHECK(target[n]==(changed?(unsigned char)(0x70+n):pattern(n,11)));
    }
    for(unsigned i=0;i<cpus;i++)printf("MREMAP CPU remote index=%u affinity=%u role=%s content=PASS\n",i,cpu_ids[i],i==0&&cpus>1?"mover":"reader");
    CHECK(!munmap(old,page)&&!munmap(target,page)); passed("remote-move-reuse");
}

static void file_case(void)
{
    CHECK(!mkdir("/tmp",0700)||errno==EEXIST);
    pin(0); char path[]="/tmp/mremap-caller-XXXXXX"; int fd=mkstemp(path);
    CHECK(fd>=0&&!unlink(path)&&!ftruncate(fd,(off_t)(2*page)));
    unsigned char *p=mmap(0,2*page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    unsigned char *alias=mmap(0,2*page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    unsigned char *target=mmap(0,2*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(p!=MAP_FAILED&&alias!=MAP_FAILED&&target!=MAP_FAILED); fill(p,2*page,33);
    CHECK(mremap(p,2*page,2*page,MREMAP_MAYMOVE|MREMAP_FIXED,target)==target);
    verify(target,2*page,33); target[page+17]=91; CHECK(alias[page+17]==91);
    CHECK(!msync(target,2*page,MS_SYNC)); unsigned char byte=0;
    CHECK(pread(fd,&byte,1,(off_t)(page+17))==1&&byte==91);
    CHECK(!ftruncate(fd,(off_t)page));
    pid_t child=fork(); CHECK(child>=0);
    if(!child) {
        CHECK(signal(SIGBUS,SIG_DFL)!=SIG_ERR);
        volatile unsigned char outside=target[page]; (void)outside; _exit(99);
    }
    int status; CHECK(waitpid(child,&status,0)==child);
    CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGBUS);
    CHECK(!ftruncate(fd,(off_t)(2*page)));
    verify(target,page,33); verify(alias,page,33);
    for(size_t i=page;i<2*page;i++)CHECK(target[i]==0&&alias[i]==0);
    CHECK(!munmap(target,2*page)&&!munmap(alias,2*page)&&!close(fd));
    passed("shared-file-msync-truncate");
}
static void sysv_case(void)
{
    pin(cpus-1);
    unsigned char *region=mmap(0,4*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    unsigned char *target=mmap(0,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(region!=MAP_FAILED&&target!=MAP_FAILED);
    int id=shmget(IPC_PRIVATE,2*page,IPC_CREAT|0600);
    CHECK(id>=0&&shmat(id,region+page,SHM_REMAP)==region+page);
    struct shmid_ds info;
    CHECK(!shmctl(id,IPC_STAT,&info)&&info.shm_nattch==1);
    /* 缩小尾部跨越另一VMA，附件计数仍须在其CPU0执行者处修改。 */
    CHECK(mremap(region,3*page,page,0)==region);
    CHECK(!shmctl(id,IPC_STAT,&info)&&info.shm_nattch==0);
    CHECK(shmat(id,region+page,SHM_REMAP)==region+page);
    CHECK(mremap(region+page,2*page,page,0)==region+page);
    CHECK(!shmctl(id,IPC_STAT,&info)&&info.shm_nattch==1);
    CHECK(!shmdt(region+page));
    CHECK(!shmctl(id,IPC_STAT,&info)&&info.shm_nattch==0);
    CHECK(shmat(id,region+page,SHM_REMAP)==region+page);
    CHECK(mremap(region,3*page,page,MREMAP_MAYMOVE|MREMAP_FIXED,target)==target);
    CHECK(!shmctl(id,IPC_STAT,&info)&&info.shm_nattch==0);
    CHECK(!shmctl(id,IPC_RMID,0)&&!munmap(region,4*page)&&!munmap(target,page));
    passed("sysv-shrink-ownership");
}
int main(int argc,char **argv)
{
    setvbuf(stdout,0,_IONBF,0); CHECK(argc<=3);
    cpu_set_t allowed; CPU_ZERO(&allowed); CHECK(!sched_getaffinity(0,sizeof(allowed),&allowed));
    for(unsigned i=0;i<CPU_SETSIZE&&cpus<4;i++)if(CPU_ISSET(i,&allowed))cpu_ids[cpus++]=i;
    CHECK(cpus); unsigned available=cpus;
    if(argc>1) { char *end; unsigned long requested=strtoul(argv[1],&end,10); CHECK(!*end&&(requested==1||requested==2||requested==4)&&requested<=available); cpus=(unsigned)requested; }
    long size=sysconf(_SC_PAGESIZE); CHECK(size>0); page=(size_t)size;
    const char *which=argc>2?argv[2]:"all";
    CHECK(!strcmp(which,"all")||!strcmp(which,"realloc")||!strcmp(which,"remote")||!strcmp(which,"file")||!strcmp(which,"sysv"));
    printf("MREMAP BEGIN cpus=%u page=%zu case=%s affinity_source=sched_getaffinity\n",cpus,page,which);
    if(!strcmp(which,"all")||!strcmp(which,"realloc"))realloc_case();
    if(!strcmp(which,"all")||!strcmp(which,"remote"))remote_case();
    if(!strcmp(which,"all")||!strcmp(which,"file"))file_case();
    if(!strcmp(which,"all")||!strcmp(which,"sysv"))sysv_case();
    printf("MREMAP END %u\n",cases); return 0;
}
