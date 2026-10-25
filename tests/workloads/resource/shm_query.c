#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef SHM_STAT_ANY
#define SHM_STAT_ANY 15
#endif
#ifndef SHM_DEST
#define SHM_DEST 01000
#endif
/* Explicit Linux LP64 receivers are shared by the glibc and musl ELFs. */
struct usage { int32_t used_ids; uint32_t pad; uint64_t total,resident,swapped,attempts,successes; };
struct limits { uint64_t maximum,minimum,identifiers,segments,pages,unused[4]; };
_Static_assert(sizeof(struct usage)==48&&sizeof(struct limits)==72,"Linux SHM info ABI");
static size_t page;
static unsigned cases;
static int ids[3]={-1,-1,-1};
static void fail(unsigned line,const char *condition)
{
    int error=errno;
    for(unsigned i=0;i<3;i++)if(ids[i]>=0)shmctl(ids[i],IPC_RMID,0);
    dprintf(2,"SHM-QUERY FAIL line=%u errno=%d %s\n",line,error,condition);_exit(1);
}
#define CHECK(x) do {if(!(x))fail(__LINE__,#x);}while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1&&errno==(e));}while(0)
static void pass(const char *name){printf("SHM-QUERY CASE %s PASS\n",name);cases++;}
static struct usage usage(int *highest)
{
    struct usage value;memset(&value,0xa5,sizeof(value));
    int result=shmctl(0,SHM_INFO,(struct shmid_ds *)&value);CHECK(result>=0);
    CHECK(!value.pad&&!value.swapped&&!value.attempts&&!value.successes);
    if(highest)*highest=result;
    return value;
}
static void counts(const struct usage *base,int used,uint64_t total,uint64_t resident)
{
    struct usage value=usage(0);
    CHECK(value.used_ids==base->used_ids+used&&value.total==base->total+total&&value.resident==base->resident+resident);
}
static int enumerate(int wanted)
{
    int highest;struct usage value=usage(&highest);struct limits limits;
    memset(&limits,0xa5,sizeof(limits));
    CHECK(shmctl(0,IPC_INFO,(struct shmid_ds *)&limits)==highest);
    CHECK(limits.minimum==1&&limits.maximum>=page&&limits.identifiers&&limits.segments&&limits.pages);
    CHECK(!limits.unused[0]&&!limits.unused[1]&&!limits.unused[2]&&!limits.unused[3]);
    int used=0,last=0,found=-1;uint64_t pages=0;
    for(int slot=0;slot<=highest;slot++) {
        struct shmid_ds ds;int id=shmctl(slot,SHM_STAT_ANY,&ds);
        if(id<0){CHECK(errno==EINVAL);continue;}
        used++;last=slot;pages+=(ds.shm_segsz+page-1)/page;
        if(id==wanted)found=slot;
    }
    CHECK(used==value.used_ids&&last==highest&&pages==value.total);
    if(wanted>=0)CHECK(found>=0);
    return found;
}
static int32_t key(const struct shmid_ds *ds)
{ int32_t value;memcpy(&value,&ds->shm_perm,sizeof(value));return value; }
static void same_descriptor(const struct shmid_ds *a,const struct shmid_ds *b)
{
    CHECK(key(a)==key(b)&&a->shm_perm.uid==b->shm_perm.uid&&a->shm_perm.gid==b->shm_perm.gid);
    CHECK(a->shm_perm.cuid==b->shm_perm.cuid&&a->shm_perm.cgid==b->shm_perm.cgid&&a->shm_perm.mode==b->shm_perm.mode);
    CHECK(a->shm_segsz==b->shm_segsz&&a->shm_nattch==b->shm_nattch&&a->shm_cpid==b->shm_cpid&&a->shm_lpid==b->shm_lpid);
    CHECK(a->shm_atime==b->shm_atime&&a->shm_dtime==b->shm_dtime&&a->shm_ctime==b->shm_ctime);
}
static void permission(int slot)
{
    struct shmid_ds original;CHECK(!shmctl(ids[0],IPC_STAT,&original));
    struct shmid_ds restricted=original;restricted.shm_perm.mode=0;
    CHECK(!shmctl(ids[0],IPC_SET,&restricted));
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        if(!geteuid())CHECK(!setgroups(0,0)&&!setgid(2901)&&!setuid(2901));
        struct shmid_ds ds;
        ERROR(shmctl(slot,SHM_STAT,&ds),EACCES);
        ERROR(shmctl(slot,SHM_STAT,0),EACCES);
        ERROR(shmctl(ids[0],IPC_STAT,0),EACCES);
        CHECK(shmctl(slot,SHM_STAT_ANY,&ds)==ids[0]&&ds.shm_segsz==page+1&&ds.shm_perm.uid==original.shm_perm.uid);
        enumerate(ids[0]);_exit(0);
    }
    int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    CHECK(!shmctl(ids[0],IPC_SET,&original));
}
static void errors(int id,int slot)
{
    unsigned char *buffer=mmap(0,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(buffer!=MAP_FAILED&&!mprotect(buffer+page,page,PROT_NONE));
    const int commands[]={IPC_INFO,SHM_INFO,IPC_STAT,SHM_STAT,SHM_STAT_ANY};
    for(unsigned i=0;i<sizeof(commands)/sizeof(commands[0]);i++) {
        int arg=commands[i]==IPC_STAT?id:slot;
        ERROR(syscall(SYS_shmctl,arg,commands[i],0),EFAULT);
        ERROR(syscall(SYS_shmctl,arg,commands[i],buffer+page-1),EFAULT);
        ERROR(syscall(SYS_shmctl,-1,commands[i],0),EINVAL);
    }
    ERROR(syscall(SYS_shmctl,id,-1,0),EINVAL);
    ERROR(syscall(SYS_shmctl,id,99,0),EINVAL);
    CHECK(!munmap(buffer,2*page));
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);page=(size_t)sysconf(_SC_PAGESIZE);CHECK(page);alarm(30);
    int highest;struct usage base=usage(&highest);
    if(!base.used_ids)CHECK(!highest&&!base.total&&!base.resident);
    enumerate(-1);pass("namespace-limits-and-empty");
    const size_t sizes[]={page+1,1,2*page+1};
    for(unsigned i=0;i<3;i++){ids[i]=shmget(IPC_PRIVATE,sizes[i],0600);CHECK(ids[i]>=0);}
    counts(&base,3,6,0);int slot=enumerate(ids[0]);pass("allocated-pages-and-highest-index");
    struct shmid_ds expected,actual;CHECK(!shmctl(ids[0],IPC_STAT,&expected));
    CHECK(shmctl(slot,SHM_STAT,&actual)==ids[0]);same_descriptor(&expected,&actual);
    CHECK(shmctl(slot,SHM_STAT_ANY,&actual)==ids[0]);same_descriptor(&expected,&actual);
    CHECK(shmctl(ids[0],SHM_STAT_ANY,&actual)==ids[0]);same_descriptor(&expected,&actual);
    pass("slot-descriptor-and-generation");
    permission(slot);pass("permission-and-stat-any");
    unsigned char *a=shmat(ids[0],0,0),*b=shmat(ids[0],0,0),*c=shmat(ids[2],0,0);
    CHECK(a!=(void *)-1&&b!=(void *)-1&&c!=(void *)-1);counts(&base,3,6,0);
    a[0]=17;b[page]=29;c[2*page]=41;CHECK(b[0]==17&&a[page]==29);counts(&base,3,6,3);
    CHECK(shmctl(slot,SHM_STAT,&actual)==ids[0]&&actual.shm_nattch==2);
    pass("resident-shared-pages");
    int removed=ids[0];CHECK(!shmctl(removed,IPC_RMID,0));
    CHECK(shmctl(slot,SHM_STAT_ANY,&actual)==removed&&(actual.shm_perm.mode&SHM_DEST)&&!key(&actual)&&actual.shm_nattch==2);
    counts(&base,3,6,3);CHECK(!shmdt(a));counts(&base,3,6,3);
    CHECK(!shmdt(b));ids[0]=-1;counts(&base,2,4,1);
    ERROR(shmctl(removed,IPC_STAT,&actual),EINVAL);ERROR(shmctl(slot,SHM_STAT_ANY,&actual),EINVAL);
    pass("rmid-attached-and-final-detach");
    CHECK(!shmdt(c)&&!shmctl(ids[2],IPC_RMID,0));ids[2]=-1;counts(&base,1,1,0);enumerate(ids[1]);
    ids[0]=shmget(IPC_PRIVATE,page,0600);CHECK(ids[0]>=0&&ids[0]!=removed);
    slot=enumerate(ids[0]);CHECK(shmctl(slot,SHM_STAT_ANY,&actual)==ids[0]);ERROR(shmctl(removed,IPC_STAT,&actual),EINVAL);
    pass("holes-stale-ids-and-new-identity");
    errors(ids[0],slot);
    CHECK(!shmctl(ids[0],IPC_RMID,0)&&!shmctl(ids[1],IPC_RMID,0));ids[0]=ids[1]=-1;
    counts(&base,0,0,0);enumerate(-1);pass("error-order-and-user-copy");
    printf("SHM-QUERY END %u\n",cases);alarm(0);if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
