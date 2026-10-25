#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef SHM_LOCKED
#define SHM_LOCKED 02000
#endif
#define CHECK(x) do {if(!(x)){dprintf(2,"SHM-LOCK FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1);}}while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1&&errno==(e));}while(0)
static size_t page;
static unsigned cases;
static void pass(const char *name){printf("SHM-LOCK CASE %s PASS\n",name);cases++;}
static void joined(pid_t child){int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));}
static void send_byte(int fd){CHECK(write(fd,"x",1)==1);}
static void receive_byte(int fd){char byte;CHECK(read(fd,&byte,1)==1&&byte=='x');}
static int segment(size_t bytes){int id=shmget(IPC_PRIVATE,bytes,0600);CHECK(id>=0);return id;}
static struct shmid_ds descriptor(int id){struct shmid_ds ds;CHECK(!shmctl(id,IPC_STAT,&ds));return ds;}
static void limit(size_t soft,size_t hard){struct rlimit lim={soft,hard};CHECK(!setrlimit(RLIMIT_MEMLOCK,&lim));}
static void identity(uid_t real,uid_t effective){CHECK(!setgroups(0,0)&&!setgid(real)&&!setresuid(real,effective,0));}
static void capabilities(uint64_t bits)
{
    struct {uint32_t version;int32_t pid;} h={0x20080522,0};
    struct {uint32_t effective,permitted,inheritable;} d[2]={
        {(uint32_t)bits,(uint32_t)bits,0},{(uint32_t)(bits>>32),(uint32_t)(bits>>32),0}};
    CHECK(!syscall(SYS_capset,&h,d));
}
static void resident(void *p,unsigned n,unsigned mask)
{unsigned char v[4];CHECK(n<=4&&!mincore(p,n*page,v));for(unsigned i=0;i<n;i++)CHECK((v[i]&1)==((mask>>i)&1));}
static void mode_and_pages(void)
{
    int id=segment(2*page+1);unsigned char *p=shmat(id,0,0);CHECK(p!=(void *)-1);resident(p,3,0);
    struct shmid_ds before=descriptor(id);CHECK(!shmctl(id,SHM_LOCK,(void *)UINTPTR_MAX));
    struct shmid_ds ds=descriptor(id);CHECK((ds.shm_perm.mode&SHM_LOCKED)&&ds.shm_nattch==1&&ds.shm_ctime==before.shm_ctime&&ds.shm_lpid==before.shm_lpid);
    resident(p,3,0);CHECK(!shmctl(id,SHM_LOCK,0));ds.shm_perm.mode=0640;CHECK(!shmctl(id,IPC_SET,&ds));
    ds=descriptor(id);CHECK((ds.shm_perm.mode&(SHM_LOCKED|0777))==(SHM_LOCKED|0640));
    CHECK(!shmctl(id,SHM_UNLOCK,0)&&!shmctl(id,SHM_UNLOCK,(void *)UINTPTR_MAX));
    ds=descriptor(id);CHECK(!(ds.shm_perm.mode&SHM_LOCKED));p[page]=71;resident(p,3,2);
    CHECK(!shmdt(p)&&!shmctl(id,IPC_RMID,0));pass("mode-idempotence-and-lazy-pages");
}
static void zero_limit(void)
{
    pid_t child=fork();CHECK(child>=0);if(!child){
        limit(0,4*page);identity(2100,2100);capabilities(0);int id=segment(page);
        ERROR(shmctl(id,SHM_LOCK,0),EPERM);CHECK(!(descriptor(id).shm_perm.mode&SHM_LOCKED));CHECK(!shmctl(id,SHM_UNLOCK,0));
        limit(page,4*page);CHECK(!shmctl(id,SHM_LOCK,0));limit(0,4*page);
        ERROR(shmctl(id,SHM_LOCK,0),EPERM);CHECK(descriptor(id).shm_perm.mode&SHM_LOCKED);
        CHECK(!shmctl(id,SHM_UNLOCK,0)&&!shmctl(id,IPC_RMID,0));_exit(0);
    }joined(child);pass("numeric-owner-zero-limit-before-repeat");
}
static void shared_uid(void)
{
    int ready[2],release[2],progress[2],resume[2];CHECK(!pipe(ready)&&!pipe(release)&&!pipe(progress)&&!pipe(resume));
    pid_t a=fork();CHECK(a>=0);if(!a){
        limit(2*page,3*page);identity(2200,2200);capabilities(0);int id=segment(page+1);CHECK(!shmctl(id,SHM_LOCK,0));
        send_byte(ready[1]);receive_byte(release[0]);CHECK(!shmctl(id,IPC_RMID,0));_exit(0);
    }receive_byte(ready[0]);
    pid_t b=fork();CHECK(b>=0);if(!b){
        limit(2*page,3*page);identity(2200,2200);capabilities(0);int id=segment(page);
        ERROR(shmctl(id,SHM_LOCK,0),ENOMEM);CHECK(!(descriptor(id).shm_perm.mode&SHM_LOCKED));
        limit(3*page,3*page);CHECK(!shmctl(id,SHM_LOCK,0));limit(page,3*page);CHECK(!shmctl(id,SHM_LOCK,0));
        CHECK(!shmctl(id,SHM_UNLOCK,0));ERROR(shmctl(id,SHM_LOCK,0),ENOMEM);
        send_byte(progress[1]);receive_byte(resume[0]);CHECK(!shmctl(id,SHM_LOCK,0)&&!shmctl(id,IPC_RMID,0));_exit(0);
    }receive_byte(progress[0]);send_byte(release[1]);joined(a);send_byte(resume[1]);joined(b);
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(release[0])&&!close(release[1])&&!close(progress[0])&&!close(progress[1])&&!close(resume[0])&&!close(resume[1]));
    pass("rounded-shared-uid-and-current-caller-limit");
}
static void permission_and_capability(void)
{
    int id=segment(page);struct shmid_ds ds=descriptor(id);ds.shm_perm.uid=2300;CHECK(!shmctl(id,IPC_SET,&ds));
    pid_t child=fork();CHECK(child>=0);if(!child){
        limit(page,page);identity(2301,2301);capabilities(UINT64_C(1)<<21);
        ERROR(shmctl(id,SHM_LOCK,0),EPERM);ERROR(shmctl(id,SHM_UNLOCK,0),EPERM);_exit(0);
    }joined(child);
    child=fork();CHECK(child>=0);if(!child){limit(page,page);identity(2300,2300);capabilities(0);CHECK(!shmctl(id,SHM_LOCK,0));_exit(0);}joined(child);
    child=fork();CHECK(child>=0);if(!child){limit(0,0);capabilities(0);CHECK(!shmctl(id,SHM_UNLOCK,0));_exit(0);}joined(child);
    child=fork();CHECK(child>=0);if(!child){limit(0,0);identity(2301,2301);capabilities(UINT64_C(1)<<14);CHECK(!shmctl(id,SHM_LOCK,0));_exit(0);}joined(child);
    CHECK(descriptor(id).shm_perm.mode&SHM_LOCKED);CHECK(!shmctl(id,SHM_UNLOCK,0)&&!shmctl(id,IPC_RMID,0));
    pass("owner-creator-and-ipc-lock-capability");
}
static void changed_identity(void)
{
    int ready[2],release[2],progress[2],resume[2];CHECK(!pipe(ready)&&!pipe(release)&&!pipe(progress)&&!pipe(resume));
    pid_t a=fork();CHECK(a>=0);if(!a){
        limit(page,page);identity(2400,2401);int id=segment(page);CHECK(!shmctl(id,SHM_LOCK,0));
        CHECK(!seteuid(0)&&!setresuid(2402,2401,0));capabilities(0);
        struct shmid_ds ds=descriptor(id);ds.shm_perm.uid=2403;CHECK(!shmctl(id,IPC_SET,&ds));
        send_byte(ready[1]);receive_byte(release[0]);CHECK(!shmctl(id,SHM_UNLOCK,0)&&!shmctl(id,IPC_RMID,0));_exit(0);
    }receive_byte(ready[0]);
    pid_t b=fork();CHECK(b>=0);if(!b){
        limit(page,page);identity(2400,2401);capabilities(0);int id=segment(page);ERROR(shmctl(id,SHM_LOCK,0),ENOMEM);
        send_byte(progress[1]);receive_byte(resume[0]);CHECK(!shmctl(id,SHM_LOCK,0)&&!shmctl(id,IPC_RMID,0));_exit(0);
    }receive_byte(progress[0]);send_byte(release[1]);joined(a);send_byte(resume[1]);joined(b);
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(release[0])&&!close(release[1])&&!close(progress[0])&&!close(progress[1])&&!close(resume[0])&&!close(resume[1]));
    pass("real-uid-account-survives-uid-and-owner-change");
}
static void removed_attached(void)
{
    pid_t child=fork();CHECK(child>=0);if(!child){
        limit(page,page);identity(2500,2500);capabilities(0);int id=segment(page),next=segment(page);
        unsigned char *p=shmat(id,0,0);CHECK(p!=(void *)-1);p[0]=19;CHECK(!shmctl(id,SHM_LOCK,0));
        int ready[2],release[2];CHECK(!pipe(ready)&&!pipe(release));pid_t holder=fork();CHECK(holder>=0);
        if(!holder){CHECK(p[0]==19);send_byte(ready[1]);receive_byte(release[0]);CHECK(p[0]==19&&!shmdt(p));_exit(0);}
        receive_byte(ready[0]);CHECK(descriptor(id).shm_nattch==2&&!shmctl(id,IPC_RMID,0));
        ERROR(shmctl(next,SHM_LOCK,0),ENOMEM);CHECK(!shmdt(p));ERROR(shmctl(next,SHM_LOCK,0),ENOMEM);
        send_byte(release[1]);joined(holder);CHECK(!shmctl(next,SHM_LOCK,0)&&!shmctl(next,IPC_RMID,0));
        CHECK(!close(ready[0])&&!close(ready[1])&&!close(release[0])&&!close(release[1]));_exit(0);
    }joined(child);pass("rmid-fork-and-final-detach-return-quota");
}
static void independent_mm_quota(void)
{
    pid_t child=fork();CHECK(child>=0);if(!child){
        limit(page,page);identity(2600,2600);capabilities(0);int id=segment(page),next=segment(page);
        void *p=mmap(0,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED&&!mlock(p,page));
        CHECK(!shmctl(id,SHM_LOCK,0));ERROR(shmctl(next,SHM_LOCK,0),ENOMEM);
        CHECK(!munlock(p,page));ERROR(shmctl(next,SHM_LOCK,0),ENOMEM);CHECK(!shmctl(id,SHM_UNLOCK,0));
        CHECK(!mlock(p,page)&&!shmctl(next,SHM_LOCK,0)&&!shmctl(next,SHM_UNLOCK,0));
        CHECK(!munlock(p,page)&&!munmap(p,page)&&!shmctl(id,IPC_RMID,0)&&!shmctl(next,IPC_RMID,0));_exit(0);
    }joined(child);pass("mm-bytes-and-shm-uid-quota-are-independent");
}
static void abi_errors(void)
{
    ERROR(shmctl(-1,SHM_LOCK,0),EINVAL);ERROR(shmctl(-1,SHM_UNLOCK,0),EINVAL);
    int id=segment(page);ERROR(shmctl(id,-1,0),EINVAL);CHECK(!shmctl(id,IPC_RMID,0));
    ERROR(shmctl(id,SHM_LOCK,0),EINVAL);ERROR(shmctl(id,SHM_UNLOCK,0),EINVAL);pass("invalid-id-and-command-errors");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);page=(size_t)sysconf(_SC_PAGESIZE);CHECK(page);alarm(30);
    mode_and_pages();zero_limit();shared_uid();permission_and_capability();changed_identity();removed_attached();independent_mm_quota();abi_errors();
    printf("SHM-LOCK END %u\n",cases);alarm(0);if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
