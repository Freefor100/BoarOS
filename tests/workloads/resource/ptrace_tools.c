#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){dprintf(2,"PTRACE_TOOLS FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1);}}while(0)
static void emit(const char *s){size_t n=strlen(s);CHECK(write(1,s,n)==(ssize_t)n);}
static void joined(pid_t pid,int code){int s;CHECK(waitpid(pid,&s,0)==pid&&WIFEXITED(s)&&WEXITSTATUS(s)==code);}
static volatile sig_atomic_t signals;
static void handler(int sig){CHECK(sig==SIGUSR1);signals++;}
static void *thread(void *unused){(void)unused;CHECK(syscall(SYS_gettid)>0);emit("TRACE_THREAD_OK\n");return 0;}
static int exec_leaf(void *environment)
{
    char *args[]={"/ptrace_tools","--exec-leaf",0};
    execve(args[0],args,environment);
    _exit(99);
}
static void *thread_exec(void *environment)
{
    char *args[]={"/ptrace_tools","--thread-exec-leaf",0};
    execve(args[0],args,environment);
    _exit(98);
}
static void payload(char **environment)
{
    CHECK(syscall(SYS_getpid)>0);
    int fd=open("/tmp/trace-file",O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
    CHECK(write(fd,"trace-data",10)==10);char b[10];CHECK(pread(fd,b,10,0)==10&&!memcmp(b,"trace-data",10));CHECK(!close(fd)&&!unlink("/tmp/trace-file"));
    pthread_t t;CHECK(!pthread_create(&t,0,thread,0)&&!pthread_join(t,0));
    pid_t c=fork();CHECK(c>=0);if(!c){emit("TRACE_FORK_OK\n");_exit(17);}joined(c,17);
    c=fork();CHECK(c>=0);
    if(!c){CHECK(!pthread_create(&t,0,thread_exec,environment));for(;;)pause();}
    joined(c,21);
    /* Some native libc vfork wrappers intentionally use fork. Request the
     * actual shared-MM vfork protocol so both architectures exercise it. */
    void *child_stack=malloc(64*1024);CHECK(child_stack);
    c=clone(exec_leaf,(char *)child_stack+64*1024,CLONE_VM|CLONE_VFORK|SIGCHLD,environment);
    CHECK(c>=0);joined(c,19);free(child_stack);
    struct sigaction a={.sa_handler=handler};sigemptyset(&a.sa_mask);CHECK(!sigaction(SIGUSR1,&a,0)&&!kill(getpid(),SIGUSR1)&&signals==1);
    emit("TRACE_PAYLOAD_OK\n");
}
static char *logfile(const char *name)
{
    int fd=open(name,O_RDONLY);CHECK(fd>=0);char *s=malloc(1024*1024);CHECK(s);size_t at=0;
    for(;;){ssize_t n=read(fd,s+at,1024*1024-1-at);CHECK(n>=0);if(!n)break;at+=(size_t)n;CHECK(at<1024*1024-1);}
    s[at]=0;CHECK(!close(fd));return s;
}
static void follow(const char *self)
{
    pid_t p=fork();CHECK(p>=0);
    if(!p){execl("/strace","strace","-f","-o","/tmp/strace-follow",self,"--payload",NULL);_exit(98);}joined(p,0);
    char *s=logfile("/tmp/strace-follow");
    const char *wanted[]={"execve(","--payload","--exec-leaf","--thread-exec-leaf","TRACE_NONLEADER_EXEC_OK","exited with 21","CLONE_THREAD","CLONE_VFORK","getpid(","pread64(","SIGUSR1","TRACE_THREAD_OK","TRACE_FORK_OK","TRACE_EXEC_OK","TRACE_PAYLOAD_OK","exited with 17","exited with 19","exited with 0"};
    for(size_t i=0;i<sizeof(wanted)/sizeof(*wanted);i++) {
        if(!strstr(s,wanted[i]))dprintf(2,"PTRACE_TOOLS missing trace record: %s\n",wanted[i]);
        CHECK(strstr(s,wanted[i]));
    }
    free(s);CHECK(!unlink("/tmp/strace-follow"));
    puts("PTRACE_TOOLS follow-thread-fork-vfork-exec-signal PASS");
}
static long long now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec*1000000000LL+t.tv_nsec;}
static int attached(pid_t pid)
{
    char path[64],buf[4096];CHECK(snprintf(path,sizeof(path),"/proc/%d/status",pid)>0);int f=open(path,O_RDONLY);CHECK(f>=0);ssize_t n=read(f,buf,sizeof(buf)-1);CHECK(n>0&&!close(f));buf[n]=0;
    char *p=strstr(buf,"TracerPid:\t");CHECK(p);return atoi(p+11)!=0;
}
static void attach_running(void)
{
    int ready[2],go[2];CHECK(!pipe(ready)&&!pipe(go));pid_t target=fork();CHECK(target>=0);
    if(!target){char c;CHECK(write(ready[1],"r",1)==1&&read(go[0],&c,1)==1);CHECK(syscall(SYS_getpid)>0);emit("TRACE_ATTACH_OK\n");_exit(0);}
    char c;CHECK(read(ready[0],&c,1)==1);char pid[24];CHECK(snprintf(pid,sizeof(pid),"%d",target)>0);
    pid_t tracer=fork();CHECK(tracer>=0);if(!tracer){execl("/strace","strace","-f","-o","/tmp/strace-attach","-p",pid,NULL);_exit(97);}
    long long deadline=now()+10000000000LL;
    while(!attached(target)){CHECK(now()<deadline);sched_yield();}
    CHECK(write(go[1],"g",1)==1);joined(target,0);joined(tracer,0);
    char *s=logfile("/tmp/strace-attach");CHECK(strstr(s,"getpid(")&&strstr(s,"TRACE_ATTACH_OK")&&strstr(s,"exited with 0"));free(s);CHECK(!unlink("/tmp/strace-attach"));
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(go[0])&&!close(go[1]));puts("PTRACE_TOOLS attach-running-target PASS");
}
int main(int argc,char **argv,char **environment)
{
    setvbuf(stdout,0,_IONBF,0);
    if(argc==2&&!strcmp(argv[1],"--exec-leaf")){emit("TRACE_EXEC_OK\n");return 19;}
    if(argc==2&&!strcmp(argv[1],"--thread-exec-leaf")){emit("TRACE_NONLEADER_EXEC_OK\n");return 21;}
    if(argc==2&&!strcmp(argv[1],"--payload")){payload(environment);return 0;}
    CHECK(argc==1);follow(argv[0]);attach_running();puts("PTRACE_TOOLS PASS");return 0;
}
