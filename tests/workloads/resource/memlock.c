#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef MLOCK_ONFAULT
#define MLOCK_ONFAULT 1
#endif
#ifndef MCL_ONFAULT
#define MCL_ONFAULT 4
#endif
#define CHECK(x) do {if(!(x)){dprintf(2,"MEMLOCK FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1);}}while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1&&errno==(e));}while(0)
static size_t page;
static unsigned cases;
static void *area(unsigned n) {void *p=mmap(0,n*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);return p;}
static unsigned long locked(void)
{
    char buf[8192];int fd=open("/proc/self/status",O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,buf,sizeof(buf)-1);CHECK(n>0&&!close(fd));buf[n]=0;
    char *p=strstr(buf,"VmLck:");CHECK(p);return strtoul(p+6,0,10)*1024;
}
static void resident(void *p,unsigned n,unsigned mask)
{unsigned char v[32];CHECK(n<=32&&!mincore(p,n*page,v));for(unsigned i=0;i<n;i++){if((v[i]&1)!=((mask>>i)&1))dprintf(2,"MEMLOCK resident page=%zu index=%u got=%u wanted=%u mask=%u\n",page,i,v[i],(mask>>i)&1,mask);CHECK((v[i]&1)==((mask>>i)&1));}}
static void joined(pid_t pid){int s;CHECK(waitpid(pid,&s,0)==pid&&WIFEXITED(s)&&!WEXITSTATUS(s));}
static void pass(const char *s){printf("MEMLOCK CASE %s PASS\n",s);cases++;}
static void onfault(void)
{
    unsigned char *p=area(4);CHECK(!syscall(SYS_mlock2,p,4*page,MLOCK_ONFAULT));
    CHECK(locked()==4*page);resident(p,4,0);p[page]=73;resident(p,4,2);
    CHECK(!munlock(p,4*page)&&!locked());resident(p,4,2);CHECK(!munmap(p,4*page));pass("onfault-residency-and-unlock");
}
static void overlap(void)
{
    unsigned char *p=area(4);CHECK(!mlock(p+1,page));CHECK(locked()==2*page);resident(p,4,3);
    CHECK(!mlock(p+page,2*page)&&locked()==3*page);CHECK(!munlock(p+page,page)&&locked()==2*page);
    CHECK(!mprotect(p,page,PROT_NONE));resident(p,1,1);CHECK(!mprotect(p,page,PROT_READ|PROT_WRITE));
    CHECK(!munlock(p,4*page)&&!munlock(p,4*page)&&!locked()&&!munmap(p,4*page));pass("overlap-rounding-protect");
}
static void executable_only(void)
{
    void *p=mmap(0,2*page,PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);
    resident(p,2,0);CHECK(!mlock(p,2*page));resident(p,2,3);
    CHECK(!munmap(p,2*page)&&!locked());
    p=mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);
    ERROR(mlock(p,page),ENOMEM);CHECK(locked()==page);resident(p,1,0);
    CHECK(!munmap(p,page)&&!locked());pass("execute-only-population");
}
static void abi(void)
{
    unsigned char *p=area(3);ERROR(syscall(SYS_mlock2,p,page,2),EINVAL);ERROR(mlockall(0),EINVAL);ERROR(mlockall(MCL_ONFAULT),EINVAL);
    ERROR(mlock((void *)(UINTPTR_MAX&~(page-1)),1),EINVAL);ERROR(munlock((void *)(UINTPTR_MAX&~(page-1)),1),EINVAL);
    CHECK(!mlock(p,0)&&!munlock(p,0));CHECK(!munmap(p+page,page));ERROR(mlock(p,3*page),ENOMEM);
    CHECK(!munlock(p,page));CHECK(!munmap(p,page)&&!munmap(p+2*page,page));CHECK(!munlockall()&&!locked());pass("abi-errors-and-hole-prefix");
}
static void future(void)
{
    pid_t c=fork();CHECK(c>=0);if(!c){
        CHECK(!mlockall(MCL_FUTURE|MCL_ONFAULT));unsigned char *p=area(3);resident(p,3,0);p[0]=9;resident(p,3,1);
        CHECK(!munlockall()&&!locked());CHECK(!munmap(p,3*page));
        CHECK(!mlockall(MCL_CURRENT));CHECK(locked()>0);CHECK(!munlockall()&&!locked());
        p=mmap(0,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_LOCKED,-1,0);CHECK(p!=MAP_FAILED);resident(p,2,3);
        CHECK(locked()==2*page&&!munmap(p,2*page)&&!locked());_exit(0);
    }joined(c);pass("all-future-current-map-locked");
}
static void quota(void)
{
    pid_t c=fork();CHECK(c>=0);if(!c){
        struct rlimit lim={2*page,4*page};CHECK(!setrlimit(RLIMIT_MEMLOCK,&lim));CHECK(!setgroups(0,0)&&!setgid(1001)&&!setuid(1001));
        unsigned char *p=area(4);CHECK(!mlock(p,2*page));ERROR(mlock(p+2*page,page),ENOMEM);CHECK(locked()==2*page);
        lim.rlim_cur=page;CHECK(!setrlimit(RLIMIT_MEMLOCK,&lim));CHECK(locked()==2*page);ERROR(mlock(p,2*page),ENOMEM);
        CHECK(!munlockall()&&!locked());lim.rlim_cur=0;CHECK(!setrlimit(RLIMIT_MEMLOCK,&lim));ERROR(mlock(p,0),EPERM);
        errno=0;CHECK(mmap(p,4*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|MAP_LOCKED,-1,0)==MAP_FAILED&&errno==EPERM);
        p[0]=33;CHECK(p[0]==33);CHECK(!munmap(p,4*page));_exit(0);
    }joined(c);pass("numeric-user-limit-and-fixed-preservation");
}
static void lifecycle(void)
{
    unsigned char *p=area(2);CHECK(!mlock(p,2*page));pid_t c=fork();CHECK(c>=0);if(!c){CHECK(!locked());p[0]=41;CHECK(!locked());_exit(0);}joined(c);
    CHECK(locked()==2*page);p=mremap(p,2*page,3*page,MREMAP_MAYMOVE);CHECK(p!=MAP_FAILED&&locked()==3*page);resident(p,3,7);
    CHECK(!munmap(p+page,page)&&locked()==2*page);CHECK(!munmap(p,page)&&!munmap(p+2*page,page)&&!locked());pass("fork-remap-unmap-accounting");
}
static void query(void)
{
    unsigned char *p=area(3),v[3]={0xaa,0xaa,0xaa};p[0]=8;ERROR(mincore(p+1,page,v),EINVAL);ERROR(mincore(p,page,(void *)UINTPTR_MAX),EFAULT);
    CHECK(!munmap(p+page,page));ERROR(mincore(p,3*page,v),ENOMEM);CHECK((v[0]&1)==1&&v[1]==0xaa);
    CHECK(!munmap(p,page)&&!munmap(p+2*page,page));
    int fd=open("/mincore-source",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0&&!ftruncate(fd,4*page));unsigned char *block=calloc(1,page);CHECK(block);block[0]=1;CHECK(pwrite(fd,block,page,0)==(ssize_t)page);free(block);
    p=mmap(0,4*page,PROT_READ,MAP_PRIVATE,fd,0);CHECK(p!=MAP_FAILED);resident(p,4,1);
    pid_t c=fork();CHECK(c>=0);if(!c){CHECK(!setgroups(0,0)&&!setgid(1001)&&!setuid(1001));resident(p,4,15);_exit(0);}joined(c);
    CHECK(!munmap(p,4*page)&&!close(fd)&&!unlink("/mincore-source"));pass("mincore-cache-hole-fault-and-disclosure");
}
static pthread_barrier_t barrier;
static unsigned char *race_area;
static void *mapping_actor(void *unused)
{
    (void)unused;
    CHECK(!syscall(SYS_mlock2,race_area,4*page,MLOCK_ONFAULT));
    pthread_barrier_wait(&barrier);pthread_barrier_wait(&barrier);
    unsigned char v[4]={0xaa,0xaa,0xaa,0xaa};
    ERROR(mincore(race_area,4*page,v),ENOMEM);CHECK(v[1]==0xaa&&locked()==3*page);
    return 0;
}
static void shared_mm(void)
{
    race_area=area(4);CHECK(!pthread_barrier_init(&barrier,0,2));pthread_t t;
    CHECK(!pthread_create(&t,0,mapping_actor,0));pthread_barrier_wait(&barrier);
    CHECK(!mprotect(race_area,page,PROT_NONE)&&!munmap(race_area+page,page));
    pthread_barrier_wait(&barrier);CHECK(!pthread_join(t,0));
    CHECK(!pthread_barrier_destroy(&barrier)&&!munmap(race_area,4*page)&&!locked());
    pass("shared-mm-lock-unmap-query");
}
static void file_ranges(void)
{
    int fd=open("/locked-source",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0&&!ftruncate(fd,4*page));
    unsigned char *p=mmap(0,4*page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(p!=MAP_FAILED);
    CHECK(!mlock(p,4*page)&&locked()==4*page);resident(p,4,15);p[0]=31;
    CHECK(!ftruncate(fd,page));CHECK(locked()==4*page);CHECK(!munlock(p,4*page)&&!munmap(p,4*page));
    CHECK(!ftruncate(fd,4*page));p=mmap(0,4*page,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);CHECK(p!=MAP_FAILED);
    CHECK(!mlock(p,4*page));p[0]=73;unsigned char byte=0;CHECK(pread(fd,&byte,1,0)==1&&byte==31);
    CHECK(!munmap(p,4*page)&&!locked()&&!close(fd)&&!unlink("/locked-source"));
    pass("file-lock-truncate-private-cow");
}
static void exec_case(void)
{
    unsigned char *p=area(2);CHECK(!mlock(p,2*page));pid_t c=fork();CHECK(c>=0);if(!c){CHECK(!mlock(p,page));execl("/init","/init","--after-exec",(char *)0);CHECK(0);}joined(c);
    CHECK(locked()==2*page&&!munmap(p,2*page));pass("exec-clears-lock-state");
}
int main(int argc,char **argv)
{
    setvbuf(stdout,0,_IONBF,0);page=(size_t)sysconf(_SC_PAGESIZE);alarm(30);
    if(access("/proc/self/status",F_OK)){if(mkdir("/proc",0755))CHECK(errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,0));}
    if(argc==2&&!strcmp(argv[1],"--after-exec")){CHECK(!locked());_exit(0);}
    onfault();overlap();executable_only();abi();future();quota();lifecycle();query();shared_mm();file_ranges();exec_case();
    printf("MEMLOCK END %u\n",cases);alarm(0);if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
