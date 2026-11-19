#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2,"EXEC-CAP FAIL line=%d errno=%d %s\n",__LINE__,errno,#x); _exit(1); } } while (0)
static const char probe[] = "exec-capacity-child";
static const char *self;
static char script[4096];
static const char script_flag[]="--exec-script-probe";
static unsigned cases;
static void handler(int sig) { (void)sig; }

static size_t length_at(size_t bytes,size_t count,size_t index)
{ return bytes/count + (index < bytes%count); }
static void payload(char **vector,size_t count,size_t bytes,char value)
{
    if (!count) { CHECK(!bytes); return; }
    CHECK(bytes>=count);
    char *storage=malloc(bytes); CHECK(storage);
    for (size_t i=0;i<count;i++) {
        size_t length=length_at(bytes,count,i);
        vector[i]=storage; memset(storage,value,length-1); storage[length-1]=0;
        storage+=length;
    }
}
static void verify(char **vector,size_t count,size_t bytes,char value)
{
    for (size_t i=0;i<count;i++) {
        size_t length=length_at(bytes,count,i);
        CHECK(vector[i] && strlen(vector[i])==length-1);
        for (size_t j=0;j<length-1;j++) CHECK(vector[i][j]==value);
    }
    CHECK(!vector[count]);
}
static void run(const char *name,rlim_t stack,size_t ac,size_t ec,
                size_t ab,size_t eb,int expected,int bad_pointer,int at)
{
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        struct rlimit limit; CHECK(!getrlimit(RLIMIT_STACK,&limit));
        limit.rlim_cur=stack; CHECK(!setrlimit(RLIMIT_STACK,&limit));
        char **args=calloc(ac+3,sizeof(char *)),**env=calloc(ec+1,sizeof(char *));
        CHECK(args && env);
        char description[64]; memset(description,'x',sizeof(description));
        int n=snprintf(description,sizeof(description),"%zu:%zu:%zu:%zu:",ac,ec,ab,eb);
        CHECK(n>0 && n<63); description[n]='x'; description[63]=0;
        args[0]=(char *)probe; args[1]=description;
        payload(args+2,ac,ab,'A'); payload(env,ec,eb,'E');
        if (bad_pointer) {
            void *hole=mmap(0,(size_t)sysconf(_SC_PAGESIZE),PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            CHECK(hole!=MAP_FAILED); args[2]=hole;
        }
        int fd=open(self,O_RDONLY|O_CLOEXEC); CHECK(fd>=0 && lseek(fd,11,SEEK_SET)==11);
        struct sigaction action={.sa_handler=handler},observed;
        CHECK(!sigemptyset(&action.sa_mask) && !sigaction(SIGUSR1,&action,0));
        pid_t before=getpid(); volatile unsigned marker=0x13579;
        if (at) syscall(SYS_execveat,fd,"",args,env,AT_EMPTY_PATH);
        else execve(script[0] ? script : self,args,env);
        CHECK(expected && errno==expected);
        CHECK(getpid()==before && marker==0x13579 && (fcntl(fd,F_GETFD)&FD_CLOEXEC));
        CHECK(lseek(fd,0,SEEK_CUR)==11 && !sigaction(SIGUSR1,0,&observed) && observed.sa_handler==handler);
        CHECK(!close(fd));
        /* A second exec must work after the rejected transaction is cleaned. */
        char *empty[]={0}; execve(self,empty,empty); CHECK(0);
    }
    int status; CHECK(waitpid(child,&status,0)==child);
    if (!WIFEXITED(status) || WEXITSTATUS(status)!=42)
        dprintf(2,"EXEC-CAP child %s status=%x\n",name,status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status)==42);
    printf("EXEC-CAP CASE %s PASS\n",name); cases++;
}
static void boundary(const char *name,rlim_t stack,size_t budget,size_t excess,int expected)
{
    size_t ac=48,ec=48;
    size_t bytes=budget-(ac+ec+2)*sizeof(char *)-strlen(self)-1-sizeof(probe)-64+excess;
    run(name,stack,ac,ec,bytes/2,bytes-bytes/2,expected,0,0);
}
int main(int argc,char **argv,char **envp)
{
    if (argc==1 && !argv[0][0]) return 42;
    int script_child=argc>1 && !strcmp(argv[1],script_flag);
    if (!strcmp(argv[0],probe) || script_child) {
        size_t ac,ec,ab,eb,offset=script_child ? 4U : 2U;
        CHECK((size_t)argc>=offset && sscanf(argv[offset-1],"%zu:%zu:%zu:%zu:",&ac,&ec,&ab,&eb)==4);
        CHECK((size_t)argc==ac+offset);
        verify(argv+offset,ac,ab,'A'); verify(envp,ec,eb,'E'); return 42;
    }
    self=getpid()==1 ? "/init" : realpath(argv[0],0);
    CHECK(self);
    size_t single=(size_t)getauxval(AT_PAGESZ)*32;
    CHECK(single);
    setvbuf(stdout,0,_IONBF,0);
    run("large-argv-env",8U<<20,8,8,256U<<10,256U<<10,0,0,0);
    boundary("default-exact",8U<<20,2U<<20,0,0);
    boundary("default-over",8U<<20,2U<<20,1,E2BIG);
    boundary("stack-quarter-exact",1U<<20,256U<<10,0,0);
    boundary("stack-quarter-over",1U<<20,256U<<10,1,E2BIG);
    boundary("floor-exact",256U<<10,128U<<10,0,0);
    boundary("floor-over",256U<<10,128U<<10,1,E2BIG);
    boundary("ceiling-exact",32U<<20,6U<<20,0,0);
    boundary("ceiling-over",32U<<20,6U<<20,1,E2BIG);
    run("single-argv-exact",8U<<20,1,0,single,0,0,0,0);
    run("single-argv-over",8U<<20,1,0,single+1,0,E2BIG,0,0);
    run("single-env-exact",8U<<20,0,1,0,single,0,0,0);
    run("single-env-over",8U<<20,0,1,0,single+1,E2BIG,0,0);
    run("many-empty-pointers",8U<<20,20000,20000,20000,20000,0,0,0);
    run("pointer-space-over",256U<<10,10000,10000,10000,10000,E2BIG,0,0);
    run("large-then-efault",8U<<20,1,8,32,256U<<10,EFAULT,1,0);
    /* execveat uses its generated /dev/fd name but shares the same limits. */
    run("execveat-large",8U<<20,8,8,256U<<10,256U<<10,0,0,1);
    CHECK(snprintf(script,sizeof(script),"%s-script",self)>0);
    int fd=open(script,O_WRONLY|O_CREAT|O_TRUNC,0755); CHECK(fd>=0);
    CHECK(dprintf(fd,"#!%s %s\n",self,script_flag)>0 && !close(fd));
    /* The original pointer reservation must not be charged again after #!. */
    size_t bytes=(2U<<20)-98*sizeof(char *)-2*(strlen(script)+1)-strlen(self)-1-sizeof(script_flag)-64;
    run("script-exact-original-pointers",8U<<20,48,48,bytes/2,bytes-bytes/2,0,0,0);
    run("script-over",8U<<20,48,48,bytes/2,bytes-bytes/2+1,E2BIG,0,0);
    CHECK(!unlink(script)); script[0]=0;
    printf("EXEC-CAP END %u\n",cases);
    return 0;
}
