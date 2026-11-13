#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { dprintf(2,"PERSONALITY FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x); exit(1); } } while(0)
#define QUERY UINT32_MAX
#define UNAME26_VALUE 0x20000U
#define NO_RANDOM 0x40000U
#define READ_EXEC 0x400000U
#define STICKY 0x4000000U
static long persona(uint64_t value) { return syscall(SYS_personality,value); }
static void joined(pid_t p) { int s; CHECK(waitpid(p,&s,0)==p && WIFEXITED(s) && !WEXITSTATUS(s)); }
static int executable(void *p)
{
    FILE *f=fopen("/proc/self/maps","r"); CHECK(f); char line[512],perms[8]; unsigned long a,b; int result=-1;
    while(fgets(line,sizeof(line),f)) if(sscanf(line,"%lx-%lx %7s",&a,&b,perms)==3 && (uintptr_t)p>=a && (uintptr_t)p<b) result=perms[2]=='x';
    CHECK(!fclose(f)); CHECK(result>=0); return result;
}
int main(int,char **);
struct layout { uintptr_t stack,main,brk,map,vdso; unsigned char random[16]; };
static void get_layout(int fd)
{
    struct layout l={0}; l.stack=(uintptr_t)&l;l.main=(uintptr_t)main;l.brk=(uintptr_t)sbrk(0);
    void *p=mmap(0,4096,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);l.map=(uintptr_t)p;
    l.vdso=getauxval(AT_SYSINFO_EHDR);void *random=(void *)getauxval(AT_RANDOM);CHECK(random);memcpy(l.random,random,16);
    CHECK(write(fd,&l,sizeof(l))==sizeof(l));CHECK(!munmap(p,4096));
}
static void exec_layout(struct layout *l)
{
    int p[2];CHECK(!pipe(p));pid_t child=fork();CHECK(child>=0);
    if(!child){CHECK(!close(p[0]));char fd[24];snprintf(fd,sizeof(fd),"%d",p[1]);char *av[]={"personality","layout",fd,0};execv("/proc/self/exe",av);_exit(99);}
    CHECK(!close(p[1]));CHECK(read(p[0],l,sizeof(*l))==sizeof(*l));CHECK(!close(p[0]));joined(child);
}
static void signal_handler(int n) { (void)n; }
static void timeout_check(unsigned flag,int select_call)
{
    CHECK(persona(flag)>=0);struct sigaction sa={.sa_handler=signal_handler};CHECK(!sigaction(SIGALRM,&sa,0));
    struct timespec ts={2,0};alarm(1);long r=select_call?syscall(SYS_pselect6,0,0,0,0,&ts,0):syscall(SYS_ppoll,0,0,&ts,0,8);
    CHECK(r==-1 && errno==EINTR);CHECK(flag ? ts.tv_sec==2 && ts.tv_nsec==0 : ts.tv_sec<2);CHECK(!alarm(0));
}
static void noexec_mapping(void)
{
    CHECK(!mkdir("/tmp/personality-noexec",0700) || errno==EEXIST);
    CHECK(!mount("tmpfs","/tmp/personality-noexec","tmpfs",MS_NOEXEC,0));
    int fd=open("/tmp/personality-noexec/data",O_CREAT|O_RDWR,0600);CHECK(fd>=0 && !ftruncate(fd,16384));
    CHECK(persona(READ_EXEC)>=0);
    void *p=mmap(0,16384,PROT_READ,MAP_PRIVATE,fd,0);CHECK(p!=MAP_FAILED && !executable(p));
    CHECK(!mprotect(p,16384,PROT_READ) && !executable(p));
    errno=0;CHECK(mprotect(p,16384,PROT_READ|PROT_EXEC)==-1 && errno==EACCES);
    errno=0;CHECK(mmap(0,16384,PROT_READ|PROT_EXEC,MAP_PRIVATE,fd,0)==MAP_FAILED && errno==EPERM);
    CHECK(!munmap(p,16384) && !close(fd));CHECK(!umount("/tmp/personality-noexec") && !rmdir("/tmp/personality-noexec"));
}
static void *thread_personality(void *unused)
{
    (void)unused;CHECK(persona(QUERY)==UNAME26_VALUE);CHECK(persona(STICKY)==UNAME26_VALUE);return 0;
}
static void setid_exec(void)
{
    int in=open("/proc/self/exe",O_RDONLY),out=open("/tmp/personality-setid",O_CREAT|O_TRUNC|O_WRONLY,0755);CHECK(in>=0 && out>=0);
    char bytes[8192];ssize_t n;while((n=read(in,bytes,sizeof(bytes)))>0)CHECK(write(out,bytes,(size_t)n)==n);
    CHECK(!n && !close(in) && !close(out) && !chmod("/tmp/personality-setid",04755));
    unsigned flags=UNAME26_VALUE|NO_RANDOM|READ_EXEC|STICKY;CHECK(persona(flags)>=0);
    int fds[2];CHECK(!pipe(fds));pid_t p=fork();CHECK(p>=0);
    if(!p){CHECK(!close(fds[0]));char fd[24];snprintf(fd,sizeof(fd),"%d",fds[1]);char *av[]={"personality","query",fd,0};execv("/tmp/personality-setid",av);_exit(99);}
    CHECK(!close(fds[1]));long got;CHECK(read(fds[0],&got,sizeof(got))==sizeof(got));CHECK(!close(fds[0]));joined(p);
    CHECK(got==(UNAME26_VALUE|STICKY) && persona(QUERY)==flags);CHECK(!unlink("/tmp/personality-setid"));
}
int main(int argc,char **argv)
{
    if(argc==3 && !strcmp(argv[1],"query")){long value=persona(QUERY);CHECK(write(atoi(argv[2]),&value,sizeof(value))==sizeof(value));return 0;}
    if(argc==3 && !strcmp(argv[1],"layout")){CHECK((persona(QUERY)&NO_RANDOM)!=0);get_layout(atoi(argv[2]));return 0;}
    setvbuf(stdout,0,_IONBF,0);long initial=persona(QUERY);CHECK(initial>=0);
    CHECK(persona(UNAME26_VALUE)==initial);CHECK(persona(UINT64_C(0x100000000)|QUERY)==UNAME26_VALUE);
    struct utsname u,v;CHECK(!uname(&u));CHECK(!strncmp(u.release,"2.6.",4));
    pid_t p=fork();CHECK(p>=0);if(!p){CHECK(persona(QUERY)==UNAME26_VALUE);CHECK(persona(0)==UNAME26_VALUE);CHECK(!uname(&v));CHECK(strncmp(v.release,"2.6.",4));_exit(0);}joined(p);
    pthread_t thread;CHECK(!pthread_create(&thread,0,thread_personality,0) && !pthread_join(thread,0));
    CHECK(persona(QUERY)==UNAME26_VALUE);char *av[]={"absent",0};CHECK(execv("/personality-absent",av)==-1 && errno==ENOENT);CHECK(persona(QUERY)==UNAME26_VALUE);
    CHECK(persona(READ_EXEC)==UNAME26_VALUE);void *map=mmap(0,4096,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(map!=MAP_FAILED && executable(map));
    CHECK(persona(0)==READ_EXEC);CHECK(!mprotect(map,4096,PROT_READ) && !executable(map));CHECK(persona(READ_EXEC)==0);CHECK(!mprotect(map,4096,PROT_READ) && executable(map));CHECK(!munmap(map,4096));
    noexec_mapping();
    CHECK(persona(NO_RANDOM)==READ_EXEC);struct layout a,b;exec_layout(&a);exec_layout(&b);CHECK(a.stack==b.stack && a.main==b.main && a.brk==b.brk && a.map==b.map && a.vdso==b.vdso);CHECK(memcmp(a.random,b.random,16));
    setid_exec();
    timeout_check(0,0);timeout_check(STICKY,0);timeout_check(0,1);timeout_check(STICKY,1);
#ifdef __riscv
    struct rlimit before,after;CHECK(!syscall(163,RLIMIT_NOFILE,&before));after=before;if(after.rlim_cur>32)after.rlim_cur=32;
    CHECK(!syscall(164,RLIMIT_NOFILE,&after));struct rlimit observed;CHECK(!prlimit(0,RLIMIT_NOFILE,0,&observed));CHECK(observed.rlim_cur==after.rlim_cur && observed.rlim_max==after.rlim_max);
    CHECK(!syscall(164,RLIMIT_NOFILE,&before));errno=0;CHECK(syscall(163,99,(void *)1)==-1 && errno==EINVAL);errno=0;CHECK(syscall(164,99,(void *)1)==-1 && errno==EFAULT);
#endif
    CHECK(persona(initial)>=0);puts("PERSONALITY PASS");return 0;
}
