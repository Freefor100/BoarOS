#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/reboot.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static size_t page;
static char maps_text[65536];
static size_t maps_bytes;
static unsigned checks;
static void check(int good, const char *what)
{
    checks++;
    if (!good) { printf("MEMORY FAIL %s errno=%d\n", what, errno); _exit(99); }
}
struct usage { uint64_t total, data, stack_start, stack_end; };
static struct usage usage(void)
{
    struct usage result={0};
    int fd=open("/proc/self/maps",O_RDONLY);
    check(fd>=0,"maps-open");
    size_t used=0;
    for (;;) {
        ssize_t n=read(fd,maps_text+used,sizeof(maps_text)-1-used);
        check(n>=0,"maps-read");
        if (!n) break;
        used+=(size_t)n;
        check(used<sizeof(maps_text)-1,"maps-fit");
    }
    check(!close(fd),"maps-close"); maps_text[used]=0; maps_bytes=used;
    char *line=maps_text;
    while (*line) {
        char *next=strchr(line,'\n'); if(next)*next=0;
        char *tail;
        uint64_t start=strtoull(line,&tail,16);
        check(*tail=='-',"maps-start");
        uint64_t end=strtoull(tail+1,&tail,16);
        while(*tail==' ')tail++;
        check(end>start&&strlen(tail)>=4,"maps-end");
        int stack=strstr(tail,"[stack]")!=0;
        if (!strstr(tail,"[vsyscall]")) result.total+=end-start;
        if(tail[1]=='w'&&tail[3]=='p'&&!stack)result.data+=end-start;
        if(stack){result.stack_start=start;result.stack_end=end;}
        if(!next)break;
        line=next+1;
    }
    return result;
}
static void limit(int which,uint64_t soft,uint64_t hard)
{
    uint64_t pair[2]={soft,hard};
    if(which==RLIMIT_STACK){
        uint64_t previous[2];check(!syscall(SYS_prlimit64,0,which,0,previous),"stack-old-limit");
        pair[1]=previous[1];
    }
    check(!syscall(SYS_prlimit64,0,which,pair,0),"prlimit");
}
static void *map(size_t n,int prot,int flags)
{
    void *p=mmap(0,n,prot,MAP_ANONYMOUS|flags,-1,0);
    check(p!=MAP_FAILED,"map-setup"); return p;
}
static int mapped(void *address,int *writable)
{
    (void)usage();
    /* usage() splits lines in place; all bytes remain in the static buffer. */
    for(char *line=maps_text;line<maps_text+maps_bytes&&*line;line+=strlen(line)+1){
        char *end;uint64_t lo=strtoull(line,&end,16),hi=strtoull(end+1,&end,16);
        if((uintptr_t)address>=lo&&(uintptr_t)address<hi){while(*end==' ')end++;if(writable)*writable=end[1]=='w';return 1;}
    }
    return 0;
}
static void as_map(void)
{
    char *p=map(page,PROT_READ|PROT_WRITE,MAP_PRIVATE);
    limit(RLIMIT_AS,0,UINT64_MAX);
    p[0]=42;check(p[0]==42,"existing-fault-below-AS");
    errno=0;check(mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED&&errno==ENOMEM,"AS-new-mmap");
    check(!munmap(p,page),"AS-shrink");
}
static void as_fixed(void)
{
    char *p=map(2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE);p[0]=37;
    struct usage u=usage();limit(RLIMIT_AS,u.total,UINT64_MAX);
    check(mmap(p,2*page,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==p,"AS-fixed-replace");
    check(!munmap(p,2*page),"AS-fixed-unmap");
}
static void data_private(void)
{
    struct usage u=usage();limit(RLIMIT_DATA,u.data+page,UINT64_MAX);
    char *p=map(page,PROT_READ|PROT_WRITE,MAP_PRIVATE);p[0]=71;
    errno=0;check(mmap(0,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED&&errno==ENOMEM,"DATA-private-denied");
    char *s=map(2*page,PROT_READ|PROT_WRITE,MAP_SHARED);s[0]=19;
    void *ro=map(2*page,PROT_READ,MAP_PRIVATE);
    check(p[0]==71&&s[0]==19,"DATA-existing-content");
    check(!munmap(p,page)&&!munmap(s,2*page)&&!munmap(ro,2*page),"DATA-unmap");
    p=map(page,PROT_READ|PROT_WRITE,MAP_PRIVATE);check(!munmap(p,page),"DATA-reclaimed");
}
static void data_zero(void)
{
    struct usage u=usage();limit(RLIMIT_DATA,0,u.data+page);
    void *p=map(page,PROT_READ|PROT_WRITE,MAP_PRIVATE);
    errno=0;check(mmap(0,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED&&errno==ENOMEM,"DATA-zero-hard");
    check(!munmap(p,page),"DATA-zero-unmap");
}
static void data_fixed(void)
{
    void *p=map(2*page,PROT_READ,MAP_PRIVATE);
    struct usage u=usage();limit(RLIMIT_DATA,u.data?u.data:1,UINT64_MAX);
    check(mmap(p,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==p,"DATA-fixed-net-growth");
    check(!munmap(p,2*page),"DATA-fixed-unmap");
}
static void protect_prefix(void)
{
    char *p=map(3*page,PROT_NONE,MAP_PRIVATE);
    check(!mprotect(p,page,PROT_READ),"prefix-split");
    struct usage u=usage();limit(RLIMIT_DATA,u.data+page,UINT64_MAX);
    errno=0;check(mprotect(p,3*page,PROT_READ|PROT_WRITE)==-1&&errno==ENOMEM,"prefix-limit");
    int writable=0;check(mapped(p,&writable)&&writable,"prefix-applied");
    check(mapped(p+page,&writable)&&!writable,"suffix-unchanged");
    p[0]=31;check(p[0]==31,"prefix-write");
    check(!munmap(p,3*page),"prefix-unmap");
}
static void protect_hole(void)
{
    char *p=map(3*page,PROT_READ,MAP_PRIVATE);check(!munmap(p+page,page),"hole-setup");
    errno=0;check(mprotect(p,3*page,PROT_READ|PROT_WRITE)==-1&&errno==ENOMEM,"hole-error");
    int writable=0;check(mapped(p,&writable)&&writable,"hole-prefix-applied");
    check(mapped(p+2*page,&writable)&&!writable,"hole-suffix-unchanged");
    check(!munmap(p,3*page),"hole-unmap");
}
static void remap_growth(void)
{
    char *p=map(page,PROT_READ|PROT_WRITE,MAP_PRIVATE);p[0]=85;
    struct usage u=usage();limit(RLIMIT_DATA,u.data,UINT64_MAX);
    errno=0;check(mremap(p,page,2*page,MREMAP_MAYMOVE)==MAP_FAILED&&errno==ENOMEM,"remap-growth-denied");
    check(p[0]==85,"remap-source-retained");check(!munmap(p,page),"remap-unmap");
}
static void remap_fixed(void)
{
    char *p=map(page,PROT_READ|PROT_WRITE,MAP_PRIVATE),*d=map(2*page,PROT_READ,MAP_PRIVATE);p[0]=86;
    struct usage u=usage();limit(RLIMIT_DATA,u.data,UINT64_MAX);
    errno=0;check(mremap(p,page,2*page,MREMAP_MAYMOVE|MREMAP_FIXED,d)==MAP_FAILED&&errno==ENOMEM,"remap-fixed-denied");
    check(p[0]==86,"remap-fixed-source");check(mapped(d,0),"remap-fixed-destination-retained");
    check(!munmap(p,page),"remap-fixed-unmap");
}
static void brk_same_page(void)
{
    uintptr_t old=(uintptr_t)syscall(SYS_brk,0);
    uintptr_t first=(old+page)&~(page-1);
    check((uintptr_t)syscall(SYS_brk,first)==first,"brk-grow-setup");
    limit(RLIMIT_DATA,0,UINT64_MAX);
    check((uintptr_t)syscall(SYS_brk,first+1)==first,"brk-zero-reject-byte");
}
static void shm_as(void)
{
    int id=shmget(IPC_PRIVATE,page,IPC_CREAT|0600);check(id>=0,"shm-create");
    limit(RLIMIT_AS,0,UINT64_MAX);
    errno=0;check(shmat(id,0,0)==(void *)-1&&errno==ENOMEM,"shm-AS-denied");
    check(!shmctl(id,IPC_RMID,0),"shm-remove");
}
static void file_and_shm_data(void)
{
    const char *path="/tmp/mm-resource-limit-file";
    int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);check(fd>=0,"file-create");
    check(!ftruncate(fd,2*page),"file-size");
    int id=shmget(IPC_PRIVATE,page,IPC_CREAT|0600);check(id>=0,"data-shm-create");
    struct usage u=usage();limit(RLIMIT_DATA,u.data?u.data:1,UINT64_MAX);
    errno=0;check(mmap(0,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0)==MAP_FAILED&&errno==ENOMEM,"file-private-DATA");
    char *shared=mmap(0,2*page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    check(shared!=MAP_FAILED,"file-shared-DATA");shared[0]=54;
    char *ro=mmap(0,2*page,PROT_READ,MAP_PRIVATE,fd,0);check(ro!=MAP_FAILED,"file-readonly-DATA");
    errno=0;check(mprotect(ro,2*page,PROT_READ|PROT_WRITE)==-1&&errno==ENOMEM,"file-protect-DATA");
    char *shm=shmat(id,0,0);check(shm!=(void *)-1,"shm-DATA-exempt");shm[0]=55;
    check(ro[0]==54&&shm[0]==55,"shared-data-content");
    check(!shmdt(shm)&&!shmctl(id,IPC_RMID,0),"data-shm-release");
    check(!munmap(shared,2*page)&&!munmap(ro,2*page),"file-unmap");
    check(!close(fd)&&!unlink(path),"file-remove");
}
static volatile unsigned shared_child_word;
static int vm_child(void *unused)
{
    (void)unused;limit(RLIMIT_AS,0,UINT64_MAX);
    errno=0;check(mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED&&errno==ENOMEM,"VM-child-own-limit");
    shared_child_word=42;
    return 0;
}
static void caller_limits(void)
{
    size_t size=65536;char *stack=map(size,PROT_READ|PROT_WRITE,MAP_PRIVATE);
    shared_child_word=0;
    pid_t child=clone(vm_child,stack+size,CLONE_VM|CLONE_VFORK|SIGCHLD,0);check(child>=0,"VM-child-create");
    int status;check(waitpid(child,&status,0)==child&&!status,"VM-child-wait");
    check(shared_child_word==42,"VM-same-memory");
    void *p=map(page,PROT_READ|PROT_WRITE,MAP_PRIVATE);
    check(!munmap(p,page)&&!munmap(stack,size),"VM-parent-own-limit");
}
struct admission_worker {
    pthread_barrier_t *start, *arrived, *release;
    unsigned cpu;
    void *address;
    int error, affinity;
};
static void *admission_worker(void *argument)
{
    struct admission_worker *w=argument;
    cpu_set_t set;CPU_ZERO(&set);CPU_SET(w->cpu,&set);
    w->affinity=sched_setaffinity(0,sizeof(set),&set);
    pthread_barrier_wait(w->start);
    errno=0;w->address=mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);w->error=errno;
    /* A successful mapping stays present until both callers have a result. */
    pthread_barrier_wait(w->arrived);
    pthread_barrier_wait(w->release);
    return 0;
}
static void shared_admission(void)
{
    pthread_barrier_t start,arrived,release;
    check(!pthread_barrier_init(&start,0,3)&&!pthread_barrier_init(&arrived,0,3)&&
        !pthread_barrier_init(&release,0,3),"admission-barriers");
    cpu_set_t available;check(!sched_getaffinity(0,sizeof(available),&available),"admission-cpus");
    unsigned cpus[2]={0},n=0;
    for(unsigned i=0;i<CPU_SETSIZE&&n<2;i++)if(CPU_ISSET(i,&available))cpus[n++]=i;
    check(n>0,"admission-cpu-present");if(n==1)cpus[1]=cpus[0];
    struct admission_worker worker[2];pthread_t thread[2];
    for(unsigned i=0;i<2;i++){
        worker[i]=(struct admission_worker){.start=&start,.arrived=&arrived,.release=&release,.cpu=cpus[i]};
        check(!pthread_create(&thread[i],0,admission_worker,&worker[i]),"admission-create");
    }
    struct usage before=usage();limit(RLIMIT_AS,before.total+page,UINT64_MAX);
    pthread_barrier_wait(&start);pthread_barrier_wait(&arrived);
    unsigned accepted=0;
    for(unsigned i=0;i<2;i++){
        check(!worker[i].affinity,"admission-affinity");
        if(worker[i].address!=MAP_FAILED)accepted++;
        else check(worker[i].error==ENOMEM,"admission-refused");
    }
    check(accepted==1,"admission-one-shared-budget");
    limit(RLIMIT_AS,UINT64_MAX,UINT64_MAX);pthread_barrier_wait(&release);
    for(unsigned i=0;i<2;i++){
        check(!pthread_join(thread[i],0),"admission-join");
        if(worker[i].address!=MAP_FAILED)check(!munmap(worker[i].address,page),"admission-release");
    }
    check(!pthread_barrier_destroy(&start)&&!pthread_barrier_destroy(&arrived)&&
        !pthread_barrier_destroy(&release),"admission-destroy");
}
static void stack_initial(void)
{
    struct usage u=usage();check(u.stack_end>u.stack_start,"stack-map-present");
    check(u.stack_end-u.stack_start<1024*1024,"stack-initial-bounded");
    volatile char *p=(void *)(uintptr_t)(u.stack_start-page);
    *p=73;check(*p==73,"stack-growth-write");
    struct usage v=usage();check(v.stack_start==u.stack_start-page,"stack-growth-start");
    check(v.total==u.total+page,"stack-growth-accounting");
}
static void stack_denial(int resource)
{
    struct usage u=usage();check(u.stack_start!=0,"stack-denial-map");
    limit(resource,resource==RLIMIT_AS?u.total:u.stack_end-u.stack_start,UINT64_MAX);
    pid_t child=fork();check(child>=0,"stack-denial-fork");
    if(!child){*(volatile char *)(uintptr_t)(u.stack_start-page)=12;_exit(98);}
    int status;check(waitpid(child,&status,0)==child,"stack-denial-wait");
    check(WIFSIGNALED(status)&&WTERMSIG(status)==SIGSEGV,"stack-denial-signal");
}
static void stack_as(void){stack_denial(RLIMIT_AS);}
static void stack_soft(void){stack_denial(RLIMIT_STACK);}
#define EXEC_AS (UINT64_C(64)*1024*1024)
#define EXEC_DATA (UINT64_C(32)*1024*1024)
static void expect_limit(int resource,uint64_t soft,uint64_t hard)
{
    uint64_t pair[2]={0,0};
    check(!syscall(SYS_prlimit64,0,resource,0,pair),"limit-query");
    check(pair[0]==soft&&pair[1]==hard,"limit-query-value");
    struct rlimit current;
    check(!getrlimit(resource,&current),"getrlimit-query");
    check((uint64_t)current.rlim_cur==soft&&(uint64_t)current.rlim_max==hard,"getrlimit-value");
}
static void limit_abi(void)
{
    const int resources[]={RLIMIT_AS,RLIMIT_DATA};
    const uint64_t softs[]={EXEC_AS,EXEC_DATA};
    for(unsigned i=0;i<2;i++){
        int resource=resources[i];uint64_t before[2],after[2];
        check(!syscall(SYS_prlimit64,0,resource,0,before),"limit-initial");
        check(before[0]==UINT64_MAX&&before[1]==UINT64_MAX,"limit-default-infinite");
        errno=0;check(syscall(SYS_prlimit64,0,resource,(void *)UINTPTR_MAX,0)==-1&&errno==EFAULT,"limit-bad-input");
        expect_limit(resource,before[0],before[1]);
        uint64_t invalid[2]={2,1};errno=0;
        check(syscall(SYS_prlimit64,0,resource,invalid,0)==-1&&errno==EINVAL,"limit-invalid-order");
        uint64_t changed[2]={softs[i],UINT64_MAX};errno=0;
        check(syscall(SYS_prlimit64,0,resource,changed,(void *)UINTPTR_MAX)==-1&&errno==EFAULT,"limit-old-output-fault");
        expect_limit(resource,changed[0],changed[1]);
        check(!syscall(SYS_prlimit64,0,resource,before,after),"limit-restore");
        check(after[0]==changed[0]&&after[1]==changed[1],"limit-old-output");
    }
}
static void *set_thread_limits(void *unused)
{
    (void)unused;limit(RLIMIT_AS,EXEC_AS-page,UINT64_MAX);limit(RLIMIT_DATA,EXEC_DATA-page,UINT64_MAX);
    return 0;
}
static void limit_thread_fork(void)
{
    limit(RLIMIT_AS,EXEC_AS,UINT64_MAX);limit(RLIMIT_DATA,EXEC_DATA,UINT64_MAX);
    pthread_t thread;check(!pthread_create(&thread,0,set_thread_limits,0),"limit-thread-create");
    check(!pthread_join(thread,0),"limit-thread-join");
    expect_limit(RLIMIT_AS,EXEC_AS-page,UINT64_MAX);expect_limit(RLIMIT_DATA,EXEC_DATA-page,UINT64_MAX);
    pid_t child=fork();check(child>=0,"limit-fork");
    if(!child){
        expect_limit(RLIMIT_AS,EXEC_AS-page,UINT64_MAX);expect_limit(RLIMIT_DATA,EXEC_DATA-page,UINT64_MAX);
        limit(RLIMIT_AS,EXEC_AS-2*page,UINT64_MAX);limit(RLIMIT_DATA,EXEC_DATA-2*page,UINT64_MAX);
        expect_limit(RLIMIT_AS,EXEC_AS-2*page,UINT64_MAX);expect_limit(RLIMIT_DATA,EXEC_DATA-2*page,UINT64_MAX);
        _exit(0);
    }
    int status;check(waitpid(child,&status,0)==child&&!status,"limit-fork-wait");
    expect_limit(RLIMIT_AS,EXEC_AS-page,UINT64_MAX);expect_limit(RLIMIT_DATA,EXEC_DATA-page,UINT64_MAX);
}
static void *exec_limits(void *unused)
{
    (void)unused;
    execl("/proc/self/exe","memory","--limit-exec",(char *)0);
    check(0,"limit-exec-returned");return 0;
}
static void limit_exec_case(int nonleader)
{
    limit(RLIMIT_AS,EXEC_AS,UINT64_MAX);limit(RLIMIT_DATA,EXEC_DATA,UINT64_MAX);
    pid_t child=fork();check(child>=0,"exec-case-fork");
    if(!child){
        if(nonleader){
            pthread_t thread;check(!pthread_create(&thread,0,exec_limits,0),"exec-thread-create");
            for(;;)pause();
        }else exec_limits(0);
        _exit(98);
    }
    int status;check(waitpid(child,&status,0)==child&&!status,"exec-case-wait");
    expect_limit(RLIMIT_AS,EXEC_AS,UINT64_MAX);expect_limit(RLIMIT_DATA,EXEC_DATA,UINT64_MAX);
}
static void limit_exec(void){limit_exec_case(0);}
static void limit_nonleader_exec(void){limit_exec_case(1);}
struct scenario {const char *name;void(*run)(void);};
int main(int argc,char **argv)
{
    setvbuf(stdout,0,_IONBF,0);page=(size_t)sysconf(_SC_PAGESIZE);
    if(argc==2&&!strcmp(argv[1],"--limit-exec")){
        expect_limit(RLIMIT_AS,EXEC_AS,UINT64_MAX);expect_limit(RLIMIT_DATA,EXEC_DATA,UINT64_MAX);
        return 0;
    }
    int proc=open("/proc/self/maps",O_RDONLY);
    if(proc<0){
        check(errno==ENOENT,"proc-probe");
        check(!mkdir("/proc",0555)||errno==EEXIST,"proc-directory");
        check(!mount("proc","/proc","proc",0,0),"proc-mount");
    }else check(!close(proc),"proc-probe-close");
    check(!mkdir("/tmp",0777)||errno==EEXIST,"tmp-directory");
    struct scenario cases[]={
        {"as-map",as_map},{"as-fixed",as_fixed},{"data-private",data_private},
        {"data-zero",data_zero},{"data-fixed",data_fixed},{"protect-prefix",protect_prefix},
        {"protect-hole",protect_hole},{"remap-growth",remap_growth},{"remap-fixed",remap_fixed},
        {"brk-same-page",brk_same_page},{"shm-as",shm_as},{"stack-initial",stack_initial},
        {"stack-as",stack_as},{"stack-soft",stack_soft},
        {"file-shm-data",file_and_shm_data},{"caller-limits",caller_limits},
        {"shared-admission",shared_admission},
        {"limit-abi",limit_abi},{"limit-thread-fork",limit_thread_fork},
        {"limit-exec",limit_exec},{"limit-nonleader-exec",limit_nonleader_exec},
    };
    unsigned failed=0;
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
        pid_t pid=fork();check(pid>=0,"case-fork");
        if(!pid){checks=0;cases[i].run();printf("MEMORY CASE %s PASS checks=%u\n",cases[i].name,checks);_exit(0);}
        int status;check(waitpid(pid,&status,0)==pid,"case-wait");
        if(status){printf("MEMORY CASE %s FAIL status=%d\n",cases[i].name,status);failed++;}
    }
    printf("MEMORY END cases=%zu failures=%u\n",sizeof(cases)/sizeof(cases[0]),failed);
    if(!failed && getpid()==1){sync();reboot(RB_POWER_OFF);}
    return failed?1:0;
}
