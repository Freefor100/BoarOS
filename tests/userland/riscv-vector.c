#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <sched.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <errno.h>
extern void vector_load(const void *);
extern void vector_store(void *);
static size_t n;
static unsigned char *a,*b,*out;
static int signals,depth,bad,relocate;
static uintptr_t alt_low,alt_high;
#ifdef VECTOR_OOM_FIXTURE
static int bus_seen;
#endif
#define CHECK(x) do{if(!(x)){printf("VECTOR FAIL line=%d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
static uint64_t vlen(void)
{ uint64_t v;asm volatile(".option push; .option arch,+v; csrr %0,vlenb; .option pop":"=r"(v));return v; }
static void initial_csrs(void)
{
    uint64_t start,vl,type,csr;
    asm volatile("csrr %0,0x008; csrr %1,0xc20; csrr %2,0xc21; csrr %3,0x00f":"=r"(start),"=r"(vl),"=r"(type),"=r"(csr));
    CHECK(!start && !vl && !type && !csr);
}
#ifdef VECTOR_OOM_FIXTURE
static void bus_handler(int sig,siginfo_t *info,void *context)
{(void)context;CHECK(sig==SIGBUS && info->si_code==128);bus_seen++;}
#endif
static void set_csrs(void)
{asm volatile(".option push; .option arch,+v; vsetivli zero,8,e16,m2,ta,ma; csrwi vcsr,5; csrwi vstart,3; .option pop":::"memory");}
static void check_csrs(void)
{
    uint64_t start,vl,type,csr;
    asm volatile("csrr %0,0x008; csrr %1,0xc20; csrr %2,0xc21; csrr %3,0x00f":"=r"(start),"=r"(vl),"=r"(type),"=r"(csr));
    CHECK(start==3 && vl==8 && type==0xc9 && csr==5);
}
static void handler(int sig,siginfo_t *si,void *context)
{
    (void)sig;(void)si;
    CHECK((uintptr_t)context>=alt_low && (uintptr_t)context+n+1024<=alt_high);
    uint32_t *hdr=(void *)((char *)context+176+256+520);
    uint64_t *v=(void *)(hdr+2);
    CHECK(hdr[0]==0x53465457 && hdr[1]==56+n);
    CHECK(v[4]*32==n && !(v[5]&15));
    CHECK(!memcmp((void *)(uintptr_t)v[5],depth?b:a,n));
    if(bad) {
        switch(bad) {
        case 1:hdr[-1]=1;break;
        case 2:hdr[0]^=1;break;
        case 3:hdr[1]-=16;break;
        case 4:v[5]=1;break;
        case 5:v[5]=UINT64_MAX-n/2;break;
        case 6:*(uint64_t *)((char *)v+48+n)=1;break;
        }
        return;
    }
    if(relocate){v[5]=(uintptr_t)b;return;}
    vector_load(b);
    if(!depth) {
        depth=1;raise(SIGUSR1);depth=0;
        vector_store(out);CHECK(!memcmp(out,b,n));
    }
    signals++;
}
static void wait_ok(pid_t p)
{int st;CHECK(waitpid(p,&st,0)==p && WIFEXITED(st)&&WEXITSTATUS(st)==0);}
static void ptrace_case(void)
{
    pid_t p=fork();CHECK(p>=0);
    if(!p){CHECK(!ptrace(PTRACE_TRACEME,0,0,0));raise(SIGSTOP);vector_store(out);CHECK(!memcmp(out,b,n));_exit(0);}
    int st;CHECK(waitpid(p,&st,0)==p && WIFSTOPPED(st));
    uint64_t *r=malloc(n+40);CHECK(r);struct iovec io={r,n+40};
    CHECK(!ptrace(PTRACE_GETREGSET,p,(void *)0x900,&io));
    CHECK(io.iov_len==n+40 && r[4]*32==n && !memcmp(r+5,a,n));
    r[4]*=2;errno=0;CHECK(ptrace(PTRACE_SETREGSET,p,(void *)0x900,&io)==-1 && errno==EINVAL);r[4]/=2;
    r[0]=0;r[1]=r[4];r[2]=0;r[3]=5;
    uint64_t save=r[1];r[1]=UINT64_MAX;errno=0;CHECK(ptrace(PTRACE_SETREGSET,p,(void *)0x900,&io)==-1 && errno==EINVAL);r[1]=save;
    r[2]=UINT64_C(1)<<63;errno=0;CHECK(ptrace(PTRACE_SETREGSET,p,(void *)0x900,&io)==-1 && errno==EINVAL);r[2]=0;
    r[3]=8;errno=0;CHECK(ptrace(PTRACE_SETREGSET,p,(void *)0x900,&io)==-1 && errno==EINVAL);r[3]=5;memcpy(r+5,b,n);
    CHECK(!ptrace(PTRACE_SETREGSET,p,(void *)0x900,&io));
    memset(r,0,n+40);io.iov_len=n+40;CHECK(!ptrace(PTRACE_GETREGSET,p,(void *)0x900,&io));
    CHECK(r[3]==5 && !memcmp(r+5,b,n));
    io.iov_len=7;errno=0;CHECK(ptrace(PTRACE_GETREGSET,p,(void *)0x900,&io)==-1 && errno==EINVAL);
    CHECK(!ptrace(PTRACE_CONT,p,0,0));wait_ok(p);free(r);
}
static void smp_case(void)
{
    cpu_set_t original,one;CPU_ZERO(&original);
    CHECK(!sched_getaffinity(0,sizeof(original),&original));
    int c0=-1,c1=-1;
    for(int i=0;i<CPU_SETSIZE;i++)if(CPU_ISSET(i,&original)){if(c0<0)c0=i;else{c1=i;break;}}
    if(c1<0)return;
    _Atomic unsigned *sync=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    CHECK(sync!=MAP_FAILED);atomic_init(sync,0);atomic_init(sync+1,0);
    pid_t p=fork();CHECK(p>=0);
    if(!p) {
        CPU_ZERO(&one);CPU_SET(c1,&one);CHECK(!sched_setaffinity(0,sizeof(one),&one));
        vector_load(b);
        for(unsigned i=1;i<=2000;i++) {
            while(atomic_load_explicit(sync,memory_order_acquire)!=i)sched_yield();
            vector_store(out);CHECK(!memcmp(out,b,n));vector_load(b);
            atomic_store_explicit(sync+1,i,memory_order_release);
        }
        _exit(0);
    }
    CPU_ZERO(&one);CPU_SET(c0,&one);CHECK(!sched_setaffinity(0,sizeof(one),&one));
    for(unsigned i=1;i<=2000;i++) {
        vector_store(out);CHECK(!memcmp(out,a,n));vector_load(a);
        atomic_store_explicit(sync,i,memory_order_release);
        while(atomic_load_explicit(sync+1,memory_order_acquire)!=i)sched_yield();
    }
    wait_ok(p);CHECK(!sched_setaffinity(0,sizeof(original),&original));munmap(sync,4096);
    puts("VECTOR SMP owners PASS");
}
int main(int argc,char **argv)
{
    setbuf(stdout,NULL);
    if(!(getauxval(16)&(1UL<<21))) {
        pid_t p=fork();CHECK(p>=0);
        if(!p){(void)vlen();_exit(3);}
        int st;CHECK(waitpid(p,&st,0)==p && WIFSIGNALED(st)&&WTERMSIG(st)==SIGILL);
        errno=0;CHECK(prctl(70,0,0,0,0)==-1 && errno==EINVAL);
        puts("VECTOR NOV PASS");return 0;
    }
    if(argc>1 && !strcmp(argv[1],"exec-off")) {
        CHECK(prctl(70,0,0,0,0)==1);
        pid_t p=fork();CHECK(p>=0);
        if(!p){(void)vlen();_exit(3);}
        int st;CHECK(waitpid(p,&st,0)==p && WIFSIGNALED(st)&&WTERMSIG(st)==SIGILL);
        CHECK(!prctl(69,2,0,0,0));
    }
#ifdef VECTOR_OOM_FIXTURE
    struct sigaction bus={.sa_sigaction=bus_handler,.sa_flags=SA_SIGINFO};sigemptyset(&bus.sa_mask);
    CHECK(!sigaction(SIGBUS,&bus,NULL));
#endif
    n=vlen()*32;CHECK(n>=512 && n<=262144);initial_csrs();
#ifdef VECTOR_OOM_FIXTURE
    if(argc==1)CHECK(bus_seen==1);
#endif
    CHECK(getauxval(51)>=n+1152);
    a=malloc(n);b=malloc(n);out=malloc(n);CHECK(a&&b&&out);
    for(size_t i=0;i<n;i++){a[i]=(unsigned char)(i*13+9);b[i]=(unsigned char)(i*29+3);}
    vector_store(out);for(size_t i=0;i<n;i++)CHECK(out[i]==0);
    if(argc>1)return 0; /* exec reset also clears every register. */
    vector_load(a);vector_store(out);CHECK(!memcmp(a,out,n));
    size_t alt_size=4*getauxval(51)+32768;void *alt=malloc(alt_size);CHECK(alt);
    alt_low=(uintptr_t)alt;alt_high=alt_low+alt_size;
    stack_t stack={.ss_sp=alt,.ss_size=alt_size};CHECK(!sigaltstack(&stack,NULL));
    struct sigaction sa={.sa_sigaction=handler,.sa_flags=SA_SIGINFO|SA_NODEFER|SA_ONSTACK};sigemptyset(&sa.sa_mask);
    CHECK(!sigaction(SIGUSR1,&sa,NULL));set_csrs();raise(SIGUSR1);CHECK(signals==2);check_csrs();
    vector_store(out);CHECK(!memcmp(a,out,n));
#ifdef VECTOR_OOM_FIXTURE
    errno=0;CHECK(fork()==-1 && errno==ENOMEM);
    vector_store(out);CHECK(!memcmp(a,out,n));
    puts("VECTOR CLONE OOM PASS");
#endif
    for(int i=1;i<=6;i++) {
        pid_t p=fork();CHECK(p>=0);
        if(!p){bad=i;raise(SIGUSR1);_exit(2);}
        int st;CHECK(waitpid(p,&st,0)==p && WIFSIGNALED(st)&&WTERMSIG(st)==SIGSEGV);
    }
    relocate=1;raise(SIGUSR1);vector_store(out);CHECK(!memcmp(b,out,n));relocate=0;vector_load(a);
    pid_t p=fork();CHECK(p>=0);
    if(!p){vector_store(out);CHECK(!memcmp(a,out,n));vector_load(b);for(int i=0;i<200;i++)sched_yield();vector_store(out);CHECK(!memcmp(b,out,n));_exit(0);}
    for(int i=0;i<200;i++)sched_yield();wait_ok(p);
    vector_store(out);CHECK(!memcmp(a,out,n));
    smp_case();ptrace_case();vector_store(out);CHECK(!memcmp(a,out,n));
    CHECK(prctl(70,0,0,0,0)==2);errno=0;CHECK(prctl(69,1,0,0,0)==-1 && errno==EPERM);
    for(int i=0;i<2;i++) {
        p=fork();CHECK(p>=0);
        if(!p){if(i)CHECK(!prctl(69,4,0,0,0));execl("/init","/init",i?"exec-off":"exec",NULL);_exit(4);}
        wait_ok(p);
    }
    stack.ss_flags=SS_DISABLE;CHECK(!sigaltstack(&stack,NULL));free(alt);
    puts("VECTOR ALL PASS");return 0;
}
