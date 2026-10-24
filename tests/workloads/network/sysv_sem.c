#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

union semarg { int val; struct semid_ds *buf; unsigned short *array; struct seminfo *__buf; };
static int ids[96],nids;
static unsigned cases,cpus,cpu_ids[4];
static volatile sig_atomic_t signals;
static void cleanup(void) { for(int i=0;i<nids;i++)if(ids[i]>=0)(void)semctl(ids[i],0,IPC_RMID); }
static void failed(const char *expr,int line) { int e=errno;cleanup();fprintf(stderr,"SYSV-SEM FAIL line=%d errno=%d %s\n",line,e,expr);exit(1); }
#define CHECK(x) do { if(!(x))failed(#x,__LINE__); } while(0)
#define ERROR(x,e) do { errno=0;CHECK((x)==-1 && errno==(e)); } while(0)
static void pass(const char *name) { ++cases;printf("SYSV-SEM CASE %s PASS\n",name); }
static int newset(int n) { int id=semget(IPC_PRIVATE,n,0600);CHECK(id>=0 && nids<(int)(sizeof(ids)/sizeof(ids[0])));ids[nids++]=id;return id; }
static void remove_set(int id) { CHECK(!semctl(id,0,IPC_RMID));for(int i=0;i<nids;i++)if(ids[i]==id)ids[i]=-1; }
static int val(int id,int n) { int v=semctl(id,n,GETVAL);CHECK(v>=0);return v; }
static void set(int id,int n,int v) { CHECK(!semctl(id,n,SETVAL,(union semarg){.val=v})); }
static void op(int id,unsigned n,short delta,int flags) { struct sembuf o={(unsigned short)n,delta,(short)flags};CHECK(!semop(id,&o,1)); }
static void wait_child(pid_t p) { int status;CHECK(waitpid(p,&status,0)==p && WIFEXITED(status) && !WEXITSTATUS(status)); }
static uint64_t now(void) { struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
/* A public waiter count proves the child entered the kernel wait queue. */
static void queued(int id,int n,int cmd) { uint64_t deadline=now()+5000000000ULL;while(semctl(id,n,cmd)!=1) { CHECK(now()<deadline);sched_yield(); } }
static void byte_write(int fd) { char c='Q';CHECK(write(fd,&c,1)==1); }
static void byte_read(int fd) { char c=0;CHECK(read(fd,&c,1)==1 && c=='Q'); }
static void expired(int sig) { (void)sig;cleanup();_exit(124); }
static void caught(int sig) { if(sig==SIGUSR1)++signals; }
static unsigned fd_count(void) { DIR *d=opendir("/proc/self/fd");CHECK(d);unsigned n=0;struct dirent *e;while((e=readdir(d)))if(strcmp(e->d_name,".") && strcmp(e->d_name,".."))++n;CHECK(!closedir(d));return n-1; }
static void affinity(void) { cpu_set_t m;CPU_ZERO(&m);CHECK(!sched_getaffinity(0,sizeof(m),&m));for(unsigned i=0;i<CPU_SETSIZE && cpus<4;i++)if(CPU_ISSET(i,&m))cpu_ids[cpus++]=i;CHECK(cpus); }
static void pin(unsigned i) { cpu_set_t m,a;CPU_ZERO(&m);CPU_SET(cpu_ids[i],&m);CHECK(!sched_setaffinity(0,sizeof(m),&m));CPU_ZERO(&a);CHECK(!sched_getaffinity(0,sizeof(a),&a) && !memcmp(&a,&m,sizeof(a))); }
static void abi(void)
{
    CHECK(sizeof(struct sembuf)==6 && sizeof(time_t)==8 && sizeof(struct timespec)==16);
#if defined(__riscv) || defined(__loongarch__)
    CHECK(SYS_semget==190 && SYS_semctl==191 && SYS_semtimedop==192 && SYS_semop==193);
#endif
    int id=(int)syscall(SYS_semget,IPC_PRIVATE,2,0600);CHECK(id>=0);ids[nids++]=id;
    CHECK(!syscall(SYS_semctl,id,0,SETVAL,7));struct sembuf o={0,-2,0};CHECK(!syscall(SYS_semop,id,&o,1));
    CHECK(semctl(id,0,GETVAL)==5);struct timespec t={0,0};o.sem_op=-1;CHECK(!syscall(SYS_semtimedop,id,&o,1,&t) && val(id,0)==4);remove_set(id);pass("native64-libc-raw");
}
static void creation(void)
{
    int a=newset(1),b=newset(1);CHECK(a!=b && val(a,0)==0);remove_set(a);remove_set(b);
    key_t key=(key_t)(0x42000000U|((unsigned)getpid()&0xffff));int id=semget(key,3,IPC_CREAT|IPC_EXCL|0600);CHECK(id>=0);ids[nids++]=id;
    CHECK(semget(key,0,0)==id && semget(key,2,0)==id);ERROR(semget(key,1,IPC_CREAT|IPC_EXCL|0600),EEXIST);ERROR(semget(key,4,0),EINVAL);remove_set(id);ERROR(semget(key,1,0),ENOENT);pass("private-key-create-exclusive");
}
static void metadata(void)
{
    int id=newset(3);struct semid_ds d;memset(&d,0xa5,sizeof(d));CHECK(!semctl(id,0,IPC_STAT,(union semarg){.buf=&d}));
    CHECK(d.sem_nsems==3 && (d.sem_perm.mode&0777)==0600 && d.sem_perm.uid==getuid() && d.sem_otime==0);
    d.sem_perm.mode=0640;CHECK(!semctl(id,0,IPC_SET,(union semarg){.buf=&d}));memset(&d,0,sizeof(d));CHECK(!semctl(id,0,IPC_STAT,(union semarg){.buf=&d}) && (d.sem_perm.mode&0777)==0640);
    struct seminfo info;int hi=semctl(0,0,IPC_INFO,(union semarg){.__buf=&info});CHECK(hi>=0 && info.semmsl>0 && info.semopm>0 && info.semvmx==32767);
    CHECK(semctl(0,0,SEM_INFO,(union semarg){.__buf=&info})>=0 && info.semusz>=1 && info.semaem>=3);
    int found=0;for(int index=0;index<=hi;index++) { int r=semctl(index,0,SEM_STAT,(union semarg){.buf=&d});if(r==id) { CHECK(d.sem_nsems==3);found=1;break; }CHECK(r>=0 || errno==EINVAL || errno==EACCES); }
    CHECK(found);remove_set(id);pass("stat-set-info-index");
}
static void values(void)
{
    int id=newset(3);unsigned short in[]={2,4,6},out[]={0,0,0};CHECK(!semctl(id,0,SETALL,(union semarg){.array=in}) && !semctl(id,0,GETALL,(union semarg){.array=out}) && !memcmp(in,out,sizeof(in)));
    CHECK(semctl(id,2,GETPID)==getpid());op(id,1,1,0);CHECK(val(id,1)==5 && semctl(id,1,GETPID)==getpid());struct semid_ds d;CHECK(!semctl(id,0,IPC_STAT,(union semarg){.buf=&d}) && d.sem_otime>0);remove_set(id);pass("values-all-pid-times");
}
static void atomic_duplicate(void)
{
    int id=newset(2);set(id,0,1);struct sembuf a[]={{0,2,0},{0,-3,0},{0,0,0},{1,4,0}};CHECK(!semop(id,a,4) && val(id,0)==0 && val(id,1)==4);remove_set(id);pass("atomic-duplicate-indices");
}
static void rollback(void)
{
    int id=newset(2);set(id,0,2);struct sembuf a[]={{0,-1,0},{1,-1,IPC_NOWAIT}};ERROR(semop(id,a,2),EAGAIN);CHECK(val(id,0)==2 && val(id,1)==0);a[1]=(struct sembuf){0,-2,IPC_NOWAIT};ERROR(semop(id,a,2),EAGAIN);CHECK(val(id,0)==2);remove_set(id);pass("atomic-rollback-nowait");
}
static void ranges(void)
{
    int id=newset(2);ERROR(semctl(id,2,GETVAL),EINVAL);ERROR(semctl(id,0,SETVAL,(union semarg){.val=-1}),ERANGE);ERROR(semctl(id,0,SETVAL,(union semarg){.val=32768}),ERANGE);
    set(id,0,32767);struct sembuf o={0,1,IPC_NOWAIT};ERROR(semop(id,&o,1),ERANGE);CHECK(val(id,0)==32767);o.sem_num=2;ERROR(semop(id,&o,1),EFBIG);ERROR(semop(id,&o,0),EINVAL);ERROR(semctl(id,0,999),EINVAL);
    /* Native semop declares unsigned int nsops, so the high 32 bits are ignored. */
    o=(struct sembuf){1,1,IPC_NOWAIT};CHECK(!syscall(SYS_semop,id,&o,UINT64_C(0x100000001)) && val(id,1)==1);
    remove_set(id);pass("operation-index-value-ranges");
}
static void waiter(int zero)
{
    int id=newset(1);if(zero)set(id,0,1);pid_t p=fork();CHECK(p>=0);if(!p) { op(id,0,zero?0:-1,0);_exit(val(id,0)!=0); }
    queued(id,0,zero?GETZCNT:GETNCNT);CHECK(semctl(id,0,zero?GETNCNT:GETZCNT)==0);if(zero)set(id,0,0);else op(id,0,1,0);wait_child(p);CHECK(!semctl(id,0,GETNCNT) && !semctl(id,0,GETZCNT));remove_set(id);pass(zero?"wait-zero-count-wake":"wait-negative-count-wake");
}
static void timeouts(void)
{
    int id=newset(1);struct sembuf o={0,-1,0};struct timespec t={0,1000000};struct timespec copy=t;ERROR(semtimedop(id,&o,1,&t),EAGAIN);CHECK(!memcmp(&copy,&t,sizeof(t)) && val(id,0)==0 && !semctl(id,0,GETNCNT));
    t.tv_nsec=1000000000;ERROR(semtimedop(id,&o,1,&t),EINVAL);t=(struct timespec){-1,0};ERROR(semtimedop(id,&o,1,&t),EINVAL);remove_set(id);pass("timeout-validation-no-mutation");
}
static void interrupted(void)
{
    int id=newset(1);pid_t p=fork();CHECK(p>=0);if(!p) { struct sigaction a={.sa_handler=caught};sigemptyset(&a.sa_mask);CHECK(!sigaction(SIGUSR1,&a,0));struct sembuf o={0,-1,0};ERROR(semop(id,&o,1),EINTR);CHECK(signals==1);_exit(0); }
    queued(id,0,GETNCNT);CHECK(!kill(p,SIGUSR1));wait_child(p);CHECK(val(id,0)==0 && !semctl(id,0,GETNCNT));remove_set(id);pass("signal-eintr-waiter-removed");
}
static void removed_waiters(void)
{
    int id=newset(2);set(id,1,1);pid_t p[2];for(int i=0;i<2;i++) { p[i]=fork();CHECK(p[i]>=0);if(!p[i]) { struct sembuf o={(unsigned short)i,(short)(i?0:-1),0};ERROR(semop(id,&o,1),EIDRM);_exit(0); } }
    queued(id,0,GETNCNT);queued(id,1,GETZCNT);remove_set(id);wait_child(p[0]);wait_child(p[1]);ERROR(semctl(id,0,GETVAL),EINVAL);pass("remove-wakes-both-waiters");
}
static void permissions(void)
{
    int id=newset(1);set(id,0,3);pid_t p=fork();CHECK(p>=0);if(!p) { CHECK(!setgroups(0,0) && !setresgid(23456,23456,23456) && !setresuid(23456,23456,23456));ERROR(semctl(id,0,GETVAL),EACCES);struct sembuf o={0,-1,IPC_NOWAIT};ERROR(semop(id,&o,1),EACCES);ERROR(semctl(id,0,IPC_RMID),EPERM);_exit(0); }
    wait_child(p);CHECK(val(id,0)==3);remove_set(id);pass("numeric-ids-permissions");
}
static void faults(void)
{
    int id=newset(2);size_t page=(size_t)sysconf(_SC_PAGESIZE);void *bad=mmap(0,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(bad!=MAP_FAILED);set(id,0,5);
    ERROR(syscall(SYS_semop,id,bad,1),EFAULT);ERROR(syscall(SYS_semtimedop,id,bad,1,0),EFAULT);struct sembuf o={0,-1,0};ERROR(syscall(SYS_semtimedop,id,&o,1,bad),EFAULT);
    int stale=newset(1);remove_set(stale);ERROR(syscall(SYS_semop,stale,bad,1),EFAULT);
    /* Linux imports timeout, then operations, then checks timeout values/id. */
    struct timespec negative={-1,0};ERROR(syscall(SYS_semtimedop,id,bad,1,&negative),EFAULT);
    ERROR(syscall(SYS_semtimedop,id,&o,0,bad),EFAULT);ERROR(syscall(SYS_semtimedop,-1,&o,1,bad),EFAULT);
    ERROR(semctl(id,0,IPC_STAT,(union semarg){.buf=bad}),EFAULT);ERROR(semctl(id,0,SETALL,(union semarg){.array=bad}),EFAULT);ERROR(semctl(id,0,GETALL,(union semarg){.array=bad}),EFAULT);CHECK(val(id,0)==5 && !munmap(bad,page));remove_set(id);pass("fault-import-no-sideeffects");
}
static void undo_exit(void)
{
    int id=newset(1);set(id,0,7);pid_t p=fork();CHECK(p>=0);if(!p) { op(id,0,-3,SEM_UNDO);CHECK(val(id,0)==4);_exit(0); }wait_child(p);CHECK(val(id,0)==7);remove_set(id);pass("undo-normal-exit");
}
static void undo_fork(void)
{
    int id=newset(1);set(id,0,4);op(id,0,-2,SEM_UNDO);pid_t p=fork();CHECK(p>=0);if(!p)_exit(0);wait_child(p);CHECK(val(id,0)==2);set(id,0,4);remove_set(id);pass("undo-fork-no-inheritance");
}
struct thread_data { int id;int go[2];int ready[2];int change; };
static void *undo_thread(void *v) { struct thread_data *d=v;byte_write(d->ready[1]);byte_read(d->go[0]);if(d->change)op(d->id,0,2,SEM_UNDO);return 0; }
/* Start the sharing thread before any undo entry exists in the process. */
static void undo_thread_share(void)
{
    int id=newset(1);pid_t p=fork();CHECK(p>=0);if(!p) { struct thread_data d={.id=id,.change=1};CHECK(!pipe(d.go) && !pipe(d.ready));pthread_t t;CHECK(!pthread_create(&t,0,undo_thread,&d));byte_read(d.ready[0]);byte_write(d.go[1]);CHECK(!pthread_join(t,0) && val(id,0)==2);_exit(0); }
    wait_child(p);CHECK(val(id,0)==0);remove_set(id);pass("undo-pthread-share-before-first");
}
static int clone_worker(void *v) { struct thread_data *d=v;byte_write(d->ready[1]);byte_read(d->go[0]);if(d->change)op(d->id,0,2,SEM_UNDO);return 0; }
static void undo_clone_share(int last)
{
    int id=newset(1),notify[2];CHECK(!pipe(notify));pid_t worker=fork();CHECK(worker>=0);
    if(!worker) { struct thread_data d={.id=id,.change=!last};CHECK(!pipe(d.go) && !pipe(d.ready));size_t size=256*1024;void *stack=malloc(size);CHECK(stack);pid_t c=clone(clone_worker,(char *)stack+size,CLONE_SYSVSEM|SIGCHLD,&d);CHECK(c>=0);byte_read(d.ready[0]);
        if(last) { op(id,0,3,SEM_UNDO);byte_write(notify[1]);byte_write(d.go[1]);_exit(0); }
        byte_write(d.go[1]);wait_child(c);CHECK(val(id,0)==2);free(stack);_exit(0);
    }
    if(last) { byte_read(notify[0]); }
    wait_child(worker);uint64_t end=now()+5000000000ULL;while(val(id,0)) { CHECK(now()<end);sched_yield(); }CHECK(!close(notify[0]) && !close(notify[1]));remove_set(id);pass(last?"undo-last-shared-exit":"undo-clone-share-before-first");
}
/* The exec image checks the adjustment has not run; its exit must apply it. */
static void exec_check(int id,int expected) { CHECK(val(id,0)==expected);_exit(0); }
struct exec_data { int id; };
static void *exec_thread(void *v) { struct exec_data *d=v;op(d->id,0,3,SEM_UNDO);char id[32];snprintf(id,sizeof(id),"%d",d->id);execl("/proc/self/exe","sysv-sem","--exec-check",id,"3",(char *)0);CHECK(0);return 0; }
static void undo_exec(int thread)
{
    int id=newset(1);pid_t p=fork();CHECK(p>=0);if(!p) { if(thread) { struct exec_data d={id};pthread_t t;CHECK(!pthread_create(&t,0,exec_thread,&d));for(;;)pause(); }op(id,0,3,SEM_UNDO);char text[32];snprintf(text,sizeof(text),"%d",id);execl("/proc/self/exe","sysv-sem","--exec-check",text,"3",(char *)0);CHECK(0); }
    wait_child(p);CHECK(val(id,0)==0);remove_set(id);pass(thread?"undo-nonleader-exec-preserve":"undo-exec-preserve");
}
static void undo_clear(int all)
{
    int id=newset(2),go[2],ready[2];CHECK(!pipe(go) && !pipe(ready));pid_t p=fork();CHECK(p>=0);if(!p) { op(id,0,2,SEM_UNDO);op(id,1,3,SEM_UNDO);byte_write(ready[1]);byte_read(go[0]);_exit(0); }
    byte_read(ready[0]);if(all) { unsigned short a[]={4,5};CHECK(!semctl(id,0,SETALL,(union semarg){.array=a})); }else set(id,0,4);
    byte_write(go[1]);wait_child(p);CHECK(val(id,0)==4 && val(id,1)==(all?5:0));CHECK(!close(go[0]) && !close(go[1]) && !close(ready[0]) && !close(ready[1]));remove_set(id);pass(all?"undo-setall-clears-all":"undo-setval-clears-one");
}
static void undo_clamp(void)
{
    int id=newset(1),go[2],ready[2];CHECK(!pipe(go) && !pipe(ready));pid_t p=fork();CHECK(p>=0);if(!p) { op(id,0,3,SEM_UNDO);byte_write(ready[1]);byte_read(go[0]);_exit(0); }
    byte_read(ready[0]);op(id,0,-2,0);byte_write(go[1]);wait_child(p);CHECK(val(id,0)==0);
    set(id,0,32767);p=fork();CHECK(p>=0);if(!p) { op(id,0,-3,SEM_UNDO);byte_write(ready[1]);byte_read(go[0]);_exit(0); }
    byte_read(ready[0]);op(id,0,2,0);byte_write(go[1]);wait_child(p);CHECK(val(id,0)==32767);
    CHECK(!close(go[0]) && !close(go[1]) && !close(ready[0]) && !close(ready[1]));remove_set(id);pass("undo-exit-clamps-bounds");
}
static void undo_ranges(void)
{
    int id=newset(1);pid_t p=fork();CHECK(p>=0);if(!p) { op(id,0,32767,SEM_UNDO);op(id,0,-32767,0);struct sembuf a[]={{0,2,SEM_UNDO},{0,-2,0}};ERROR(semop(id,a,2),ERANGE);CHECK(val(id,0)==0);_exit(0); }wait_child(p);CHECK(val(id,0)==0);remove_set(id);pass("undo-adjustment-range-rollback");
}
static void clone_failure(void)
{
    int id=newset(1);op(id,0,1,SEM_UNDO);ERROR(syscall(SYS_clone,CLONE_SYSVSEM|CLONE_NEWIPC|SIGCHLD,0,0,0,0),EINVAL);CHECK(val(id,0)==1);set(id,0,0);remove_set(id);pass("clone-invalid-flags-owner-retained");
}
static void limits(void)
{
    struct seminfo info;CHECK(semctl(0,0,IPC_INFO,(union semarg){.__buf=&info})>=0);CHECK(info.semopm>0 && info.semopm<=10000 && info.semmsl>0 && info.semmsl<1000000);
    ERROR(semget(IPC_PRIVATE,0,0600),EINVAL);ERROR(semget(IPC_PRIVATE,info.semmsl+1,0600),EINVAL);int id=newset(1);struct sembuf *o=calloc((size_t)info.semopm+1,sizeof(*o));CHECK(o);ERROR(semop(id,o,(size_t)info.semopm+1),E2BIG);free(o);remove_set(id);pass("published-limits-large-user-import");
}
static void caller_cpus(void)
{
    for(unsigned i=0;i<cpus;i++) { int id=newset(2);pid_t p=fork();CHECK(p>=0);if(!p) { pin(i);op(id,0,2,SEM_UNDO);op(id,1,-1,0);CHECK(val(id,0)==2 && val(id,1)==0);_exit(0); }
        queued(id,1,GETNCNT);CHECK(val(id,0)==2);op(id,1,1,0);wait_child(p);CHECK(val(id,0)==0 && val(id,1)==0);remove_set(id);printf("SYSV-SEM CPU index=%u affinity=%u commit=1 wait=1 undo=1 PASS\n",i,cpu_ids[i]); }
    pass("actual-caller-cpus");
}
static void remove_undo(void)
{
    int id=newset(1);pid_t p=fork();CHECK(p>=0);if(!p) { op(id,0,1,SEM_UNDO);CHECK(!semctl(id,0,IPC_RMID));_exit(0); }wait_child(p);for(int i=0;i<nids;i++)if(ids[i]==id)ids[i]=-1;ERROR(semctl(id,0,GETVAL),EINVAL);int next=newset(1);CHECK(val(next,0)==0);remove_set(next);pass("remove-discards-stale-undo");
}
static void read_permission(void)
{
    int id=newset(1);struct semid_ds d;CHECK(!semctl(id,0,IPC_STAT,(union semarg){.buf=&d}));d.sem_perm.mode=0444;CHECK(!semctl(id,0,IPC_SET,(union semarg){.buf=&d}));pid_t p=fork();CHECK(p>=0);if(!p) { CHECK(!setgroups(0,0) && !setresgid(23457,23457,23457) && !setresuid(23457,23457,23457));CHECK(val(id,0)==0);struct sembuf zero={0,0,IPC_NOWAIT},add={0,1,IPC_NOWAIT};CHECK(!semop(id,&zero,1));ERROR(semop(id,&add,1),EACCES);_exit(0); }wait_child(p);remove_set(id);pass("zero-operation-read-permission");
}
static void socketpair_cases(void)
{
    int p[2],dummy=open("/dev/null",O_RDONLY);CHECK(dummy>=0);CHECK(!close(dummy));
    const struct { int family,type,protocol,error; } v[]={ {47,SOCK_STREAM,0,EAFNOSUPPORT},{AF_INET,SOCK_STREAM,IPPROTO_UDP,EPROTONOSUPPORT},{AF_INET,SOCK_STREAM,0,EOPNOTSUPP},{AF_INET,SOCK_DGRAM,0,EOPNOTSUPP},{AF_UNIX,SOCK_STREAM,99,EPROTONOSUPPORT},{AF_INET,SOCK_STREAM,-1,EINVAL} };
    for(unsigned i=0;i<sizeof(v)/sizeof(*v);i++) { p[0]=p[1]=-99;ERROR(socketpair(v[i].family,v[i].type,v[i].protocol,p),v[i].error);CHECK(p[0]==dummy && p[1]==dummy+1);ERROR(fcntl(p[0],F_GETFD),EBADF); }
    puts("SOCKETPAIR CASE creation-errors-output-prefix PASS");
    size_t page=(size_t)sysconf(_SC_PAGESIZE);char *m=mmap(0,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(m!=MAP_FAILED && !mprotect(m+page,page,PROT_NONE));int *partial=(void *)(m+page-sizeof(int));*partial=-99;
    ERROR(syscall(SYS_socketpair,47,SOCK_STREAM,0,partial),EFAULT);CHECK(*partial==dummy);ERROR(syscall(SYS_socketpair,47,SOCK_STREAM,0,m+page),EFAULT);CHECK(!munmap(m,page*2));puts("SOCKETPAIR CASE fault-before-create-no-installed-fd PASS");
    p[0]=p[1]=-99;ERROR(socketpair(47,SOCK_STREAM|0x40000000,0,p),EINVAL);CHECK(p[0]==-99 && p[1]==-99);CHECK(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0,p));char c;CHECK(write(p[0],"S",1)==1 && read(p[1],&c,1)==1 && c=='S' && (fcntl(p[0],F_GETFD)&FD_CLOEXEC));CHECK(!close(p[0]) && !close(p[1]));puts("SOCKETPAIR CASE flags-and-live-unix-content PASS");
    pid_t child=fork();CHECK(child>=0);if(!child) { struct rlimit lim;CHECK(!getrlimit(RLIMIT_NOFILE,&lim));lim.rlim_cur=(rlim_t)dummy+1;CHECK(!setrlimit(RLIMIT_NOFILE,&lim));int only=open("/dev/null",O_RDONLY);CHECK(only==dummy);p[0]=p[1]=-99;ERROR(socketpair(47,SOCK_STREAM,0,p),EMFILE);CHECK(p[0]==-99 && p[1]==-99);CHECK(!close(only));ERROR(socketpair(47,SOCK_STREAM,0,p),EMFILE);CHECK(p[0]==-99 && p[1]==-99);only=open("/dev/null",O_RDONLY);CHECK(only==dummy && !close(only));_exit(0); }wait_child(child);puts("SOCKETPAIR CASE fd-reservation-before-family PASS");puts("SOCKETPAIR END 4");
}
int main(int argc,char **argv)
{
    setvbuf(stdout,0,_IONBF,0);if(argc==4 && !strcmp(argv[1],"--exec-check"))exec_check(atoi(argv[2]),atoi(argv[3]));
    CHECK(!atexit(cleanup));signal(SIGALRM,expired);alarm(100);affinity();if(getpid()==1) { CHECK(!mkdir("/proc",0555) || errno==EEXIST);CHECK(!mount("proc","/proc","proc",0,0) || errno==EBUSY); }
    struct seminfo initial,final;CHECK(semctl(0,0,SEM_INFO,(union semarg){.__buf=&initial})>=0);
    unsigned before=fd_count();printf("SYSV-SEM BEGIN cpus=%u\n",cpus);
    abi();creation();metadata();values();atomic_duplicate();rollback();ranges();waiter(1);waiter(0);timeouts();interrupted();removed_waiters();permissions();faults();undo_exit();undo_fork();undo_thread_share();undo_clone_share(0);undo_clone_share(1);undo_exec(0);undo_exec(1);undo_clear(0);undo_clear(1);undo_clamp();undo_ranges();clone_failure();limits();remove_undo();read_permission();caller_cpus();
    printf("SYSV-SEM END %u\n",cases);socketpair_cases();cleanup();unsigned after=fd_count();CHECK(semctl(0,0,SEM_INFO,(union semarg){.__buf=&final})>=0);printf("SYSV-SEM RESOURCE fd_before=%u fd_after=%u sets_before=%d sets_after=%d members_before=%d members_after=%d\n",before,after,initial.semusz,final.semusz,initial.semaem,final.semaem);CHECK(before==after && initial.semusz==final.semusz && initial.semaem==final.semaem);alarm(0);if(getpid()==1) { sync();(void)reboot(RB_POWER_OFF); }return 0;
}
