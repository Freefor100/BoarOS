#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <signal.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2,"EXECVEAT FAIL line=%d errno=%d %s\n",__LINE__,errno,#x); _exit(1); } } while (0)
#define ERROR(x,e) do { errno=0; CHECK((x)==-1 && errno==(e)); } while (0)
static unsigned cases;
static void pass(const char *name) { printf("EXECVEAT CASE %s PASS\n",name); cases++; }
static long execute(long fd,const char *path,const void *argv,const void *envp,long flags)
{ return syscall(SYS_execveat,fd,path,argv,envp,flags); }
static void joined(pid_t child,int code)
{ int status; CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==code); }
static void directory(const char *path) { CHECK(!mkdir(path,0755) || errno==EEXIST); }
static void text(const char *path,const char *bytes)
{ int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0755); CHECK(fd>=0); size_t n=strlen(bytes); CHECK(write(fd,bytes,n)==(ssize_t)n && !close(fd)); }
static void copy(const char *from,const char *to)
{
    int in=open(from,O_RDONLY),out=open(to,O_WRONLY|O_CREAT|O_TRUNC,0755); CHECK(in>=0 && out>=0);
    char bytes[8192]; ssize_t n;
    while ((n=read(in,bytes,sizeof(bytes)))>0) CHECK(write(out,bytes,(size_t)n)==n);
    CHECK(!n && !fchmod(out,0755) && !close(in) && !close(out));
}
static void launch(long fd,const char *path,long flags,const char *expected)
{
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        int gone=open("/ea/exe",O_RDONLY|O_CLOEXEC),kept=open("/ea/exe",O_RDONLY); CHECK(gone>=0 && kept>=0 && lseek(kept,7,SEEK_SET)==7);
        char fds[64]; CHECK(snprintf(fds,sizeof(fds),"EXECVEAT_FDS=%d:%d",gone,kept)>0);
        char *args[]={"kept-zero","--probe",(char *)expected,0}; char *env[]={"EXECVEAT_ENV=kept",fds,0};
        execute(fd,path,args,env,flags); dprintf(2,"EXECVEAT returned errno=%d path=%s\n",errno,path); _exit(90);
    }
    joined(child,42);
}
static void denied(long fd,const char *path,long flags,int error)
{
    int keep=open("/ea/exe",O_RDONLY|O_CLOEXEC); CHECK(keep>=0);
    pid_t before=getpid(); char *args[]={"kept-zero","--probe","unused",0}; char *env[]={0};
    ERROR(execute(fd,path,args,env,flags),error);
    CHECK(getpid()==before && (fcntl(keep,F_GETFD)&FD_CLOEXEC) && !close(keep));
}
static void setup(void)
{
    int proc=open("/proc/self/stat",O_RDONLY);
    if (proc<0) { CHECK(errno==ENOENT); directory("/proc"); CHECK(!mount("proc","/proc","proc",0,0)); }
    else CHECK(!close(proc));
    directory("/ea"); CHECK(!mount("tmpfs","/ea","tmpfs",0,0));
    directory("/ea/dir"); directory("/ea/deny");
    copy("/proc/self/exe","/ea/exe"); copy("/ea/exe","/ea/dir/exe");
    CHECK(!symlink("exe","/ea/link"));
    directory("/dev"); CHECK(!symlink("/proc/self/fd","/dev/fd") || errno==EEXIST);
}
static uint64_t legacy_wall_ns(void)
{
    struct timespec now; CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));
    return (uint64_t)now.tv_sec*1000000000U+(uint64_t)now.tv_nsec;
}
static uint64_t legacy_cpu_ns(void)
{
    struct rusage self,children; CHECK(!getrusage(RUSAGE_SELF,&self) && !getrusage(RUSAGE_CHILDREN,&children));
    return ((uint64_t)self.ru_utime.tv_sec+(uint64_t)self.ru_stime.tv_sec+
        (uint64_t)children.ru_utime.tv_sec+(uint64_t)children.ru_stime.tv_sec)*1000000000U+
        ((uint64_t)self.ru_utime.tv_usec+(uint64_t)self.ru_stime.tv_usec+
        (uint64_t)children.ru_utime.tv_usec+(uint64_t)children.ru_stime.tv_usec)*1000U;
}
struct legacy_window { uint64_t cpu,wall; };
static struct legacy_window legacy_begin(void)
{
    struct legacy_window window; window.cpu=legacy_cpu_ns(); window.wall=legacy_wall_ns(); return window;
}
static void legacy_report(const char *name,unsigned count,struct legacy_window window)
{
    uint64_t wall=legacy_wall_ns()-window.wall,cpu=legacy_cpu_ns()-window.cpu;
    printf("EXEC-LEGACY %s count=%u ns=%llu cpu_ns=%llu\n",name,count,(unsigned long long)wall,(unsigned long long)cpu);
}
static void legacy_cost(void)
{
    setup(); sync();
    struct legacy_window window=legacy_begin();
    pid_t observed=0;
    for (unsigned i=0;i<100000;i++) observed=getpid();
    legacy_report("getpid",100000,window); CHECK(observed>0);
    window=legacy_begin();
    for (unsigned i=0;i<4096;i++) {
        int fd=open("/ea/exe",O_RDONLY); struct stat metadata;
        CHECK(fd>=0 && !fstat(fd,&metadata) && S_ISREG(metadata.st_mode) && metadata.st_size>0 && !close(fd));
    }
    legacy_report("open",4096,window);
    window=legacy_begin();
    for (unsigned i=0;i<64;i++) {
        pid_t child=fork(); CHECK(child>=0);
        if (!child) {
            char *args[]={"exec-legacy","--legacy-child",0}; char *env[]={0};
            execve("/ea/exe",args,env); _exit(90);
        }
        joined(child,42);
    }
    legacy_report("exec",64,window);
    CHECK(!umount("/ea") && !rmdir("/ea")); sync();
    puts("EXEC-LEGACY END 3");
}
static void names(void)
{
    launch(-1,"/ea/exe",0,"/ea/exe");
    int dir=open("/ea/dir",O_PATH|O_DIRECTORY); CHECK(dir>=0);
    char expected[80]; CHECK(snprintf(expected,sizeof(expected),"/dev/fd/%d/./exe",dir)>0);
    launch(dir,"./exe",0,expected);
    CHECK(!chdir("/ea")); launch(AT_FDCWD,"./exe",0,"./exe"); CHECK(!chdir("/"));
    CHECK(!close(dir)); pass("absolute-dirfd-cwd-original-name");
}
static void empty(void)
{
    for (unsigned i=0;i<2;i++) {
        int fd=open("/ea/exe",i?O_PATH:O_RDONLY); CHECK(fd>=0);
        char expected[64]; CHECK(snprintf(expected,sizeof(expected),"/dev/fd/%d",fd)>0);
        launch(fd,"",AT_EMPTY_PATH,expected); CHECK(!close(fd));
    }
    int fd=open("/ea/exe",O_PATH|O_CLOEXEC); CHECK(fd>=0);
    char expected[64]; CHECK(snprintf(expected,sizeof(expected),"/dev/fd/%d",fd)>0);
    launch(fd,"",AT_EMPTY_PATH,expected); CHECK(!close(fd));
    pass("empty-readonly-opath-cloexec-elf");
}
static void identity(void)
{
    copy("/ea/exe","/ea/moved"); int fd=open("/ea/moved",O_PATH); CHECK(fd>=0);
    CHECK(!rename("/ea/moved","/ea/renamed")); text("/ea/moved","wrong file\n");
    char expected[64]; CHECK(snprintf(expected,sizeof(expected),"/dev/fd/%d",fd)>0);
    launch(fd,"",AT_EMPTY_PATH,expected); CHECK(!unlink("/ea/renamed"));
    launch(fd,"",AT_EMPTY_PATH,expected); CHECK(!close(fd));
    pass("renamed-unlinked-open-identity");
}
static void links(void)
{
    launch(AT_FDCWD,"/ea/link",0,"/ea/link"); denied(AT_FDCWD,"/ea/link",AT_SYMLINK_NOFOLLOW,ELOOP);
    int fd=open("/ea/link",O_PATH|O_NOFOLLOW); CHECK(fd>=0);
    denied(fd,"",AT_EMPTY_PATH,ELOOP); denied(fd,"",AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW,ELOOP);
    CHECK(!close(fd)); pass("nofollow-final-and-empty-symlink");
}
static void errors(void)
{
    char *args[]={"x",0}; char *env[]={0};
    ERROR(execute(-1,(char *)(uintptr_t)1,args,env,0x40000000),EINVAL);
    ERROR(execute(-1,"",args,env,0x40000000),EINVAL);
    ERROR(execute(-1,(char *)(uintptr_t)1,args,env,0),EFAULT);
    ERROR(execute(-1,"/ea/exe",args,env,0x40000000),EINVAL);
    ERROR(execute(-1,"x",(char *const *)(uintptr_t)1,env,0),EBADF);
    ERROR(execute(-1,"",args,env,AT_EMPTY_PATH),EBADF);
    ERROR(execute(-1,"",args,env,0),ENOENT);
    ERROR(execute(AT_FDCWD,"",args,env,AT_EMPTY_PATH),EACCES);
    int regular=open("/ea/exe",O_RDONLY); CHECK(regular>=0);
    denied(regular,"x",0,ENOTDIR); CHECK(!close(regular));
    ERROR(execute(-1,"/ea/exe",(char *const *)(uintptr_t)1,env,0),EFAULT);
    ERROR(execute(-1,"/ea/exe",args,(char *const *)(uintptr_t)1,0),EFAULT);
    denied(-1,"/ea/missing",0,ENOENT);
    denied(-1,"/ea/dir",0,EACCES);
    pass("flags-path-fd-vector-error-order-rollback");
}
static void truncation(void)
{
    launch((long)(UINT64_C(0x123400000000)|(uint32_t)AT_FDCWD),"/ea/exe",UINT64_C(0x123400000000),"/ea/exe");
    int fd=open("/ea/exe",O_PATH); CHECK(fd>=0);
    char expected[64]; CHECK(snprintf(expected,sizeof(expected),"/dev/fd/%d",fd)>0);
    launch((long)(UINT64_C(0x123400000000)|(unsigned)fd),"",UINT64_C(0x123400001000),expected); CHECK(!close(fd));
    pass("dirfd-and-flags-int32-truncation");
}
static void script(long fd,const char *path,long flags,const char *expected)
{
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        char *args[]={"discarded",(char *)expected,0}; char *env[]={0}; execute(fd,path,args,env,flags); _exit(90);
    }
    joined(child,43);
}
static void scripts(void)
{
    text("/ea/dir/script","#!/ea/exe --script\n");
    script(AT_FDCWD,"/ea/dir/script",0,"/ea/dir/script");
    int dir=open("/ea/dir",O_PATH|O_DIRECTORY),fd=open("/ea/dir/script",O_RDONLY); CHECK(dir>=0 && fd>=0);
    char expected[80]; CHECK(snprintf(expected,sizeof(expected),"/dev/fd/%d/./script",dir)>0);
    script(dir,"./script",0,expected);
    CHECK(snprintf(expected,sizeof(expected),"/dev/fd/%d",fd)>0); script(fd,"",AT_EMPTY_PATH,expected);
    CHECK(!fcntl(dir,F_SETFD,FD_CLOEXEC)); denied(dir,"script",0,ENOENT);
    CHECK(!fcntl(fd,F_SETFD,FD_CLOEXEC)); denied(fd,"",AT_EMPTY_PATH,ENOENT);
    /* Absolute names remain accessible when an unused dirfd has CLOEXEC. */
    script(dir,"/ea/dir/script",0,"/ea/dir/script");
    CHECK(!close(dir) && !close(fd)); pass("script-fd-spelling-cloexec-and-interpreter");
}
static void permission(void)
{
    copy("/ea/exe","/ea/permission"); int fd=open("/ea/permission",O_PATH); CHECK(fd>=0);
    CHECK(!chmod("/ea/permission",0644)); denied(fd,"",AT_EMPTY_PATH,EACCES);
    CHECK(!chmod("/ea/permission",0755));
    char expected[64]; CHECK(snprintf(expected,sizeof(expected),"/dev/fd/%d",fd)>0); launch(fd,"",AT_EMPTY_PATH,expected);
    CHECK(!close(fd));
    CHECK(!mount("tmpfs","/ea/deny","tmpfs",MS_NOEXEC,0)); copy("/ea/exe","/ea/deny/exe");
    fd=open("/ea/deny/exe",O_PATH); CHECK(fd>=0); denied(fd,"",AT_EMPTY_PATH,EACCES); CHECK(!close(fd));
    int writer=open("/ea/permission",O_WRONLY); fd=open("/ea/permission",O_PATH); CHECK(writer>=0 && fd>=0);
    denied(fd,"",AT_EMPTY_PATH,ETXTBSY); CHECK(!close(writer) && !close(fd));
    pass("current-permission-noexec-writer-exclusion");
}
static void *thread_exec(void *unused)
{ (void)unused; int fd=open("/ea/exe",O_PATH|O_CLOEXEC); CHECK(fd>=0); char expected[64]; CHECK(snprintf(expected,sizeof(expected),"/dev/fd/%d",fd)>0); char *args[]={"kept-zero","--probe",expected,0}; char *env[]={"EXECVEAT_ENV=kept",0}; execute(fd,"",args,env,AT_EMPTY_PATH); _exit(90); }
static void nonleader(void)
{ pid_t child=fork(); CHECK(child>=0); if (!child) { pthread_t t; CHECK(!pthread_create(&t,0,thread_exec,0)); for (;;) pause(); } joined(child,42); pass("nonleader-exec-common-transaction"); }

static void current_actor(void)
{
    copy("/ea/exe","/ea/private"); CHECK(!chmod("/ea/private",0700));
    int fd=open("/ea/private",O_PATH); CHECK(fd>=0);
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        CHECK(!syscall(SYS_setresuid,20161,20161,20161));
        denied(fd,"",AT_EMPTY_PATH,EACCES); _exit(0);
    }
    joined(child,0); CHECK(!close(fd)); pass("opened-file-authority-vs-current-execute-user");
}
struct identity_exec { int fd,nnp; unsigned euid,secure,caps; };
static int execute_identity(void *opaque)
{
    const struct identity_exec *request=opaque;
    CHECK(!syscall(SYS_setresgid,20161,20161,20161) && !syscall(SYS_setresuid,20161,20161,20161));
    if (request->nnp) CHECK(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));
    char expected[32],secure[8],caps[32]; CHECK(snprintf(expected,sizeof(expected),"%u",request->euid)>0 && snprintf(secure,sizeof(secure),"%u",request->secure)>0 && snprintf(caps,sizeof(caps),"%u",request->caps)>0);
    char *args[]={"ids","--ids",expected,secure,caps,0}; char *env[]={0};
    execute(request->fd,"",args,env,AT_EMPTY_PATH); _exit(90);
}
static void identity_run(const char *path,int nnp,int shared,unsigned euid,unsigned secure,unsigned caps)
{
    int fd=open(path,O_PATH|O_CLOEXEC); CHECK(fd>=0);
    struct identity_exec request={fd,nnp,euid,secure,caps}; pid_t child;
    if (shared) {
        void *stack=malloc(65536); CHECK(stack);
        child=clone(execute_identity,(char *)stack+65536,CLONE_FS|SIGCHLD,&request); CHECK(child>=0);
        joined(child,44); free(stack);
    } else {
        child=fork(); CHECK(child>=0); if (!child) execute_identity(&request); joined(child,44);
    }
    CHECK(!close(fd));
}
static void credentials(void)
{
    copy("/ea/exe","/ea/suid"); CHECK(!chown("/ea/suid",20162,20162) && !chmod("/ea/suid",04755));
    identity_run("/ea/suid",0,0,20162,1,0); identity_run("/ea/suid",1,0,20161,0,0);
    identity_run("/ea/suid",0,1,20161,1,0);
    directory("/ea/nosuid"); CHECK(!mount("tmpfs","/ea/nosuid","tmpfs",MS_NOSUID,0));
    copy("/ea/exe","/ea/nosuid/suid"); CHECK(!chown("/ea/nosuid/suid",20162,20162) && !chmod("/ea/nosuid/suid",04755));
    identity_run("/ea/nosuid/suid",0,0,20161,0,0);
    pass("setid-nnp-nosuid-and-unsafe-shared-fs");
}
static void filecap(void)
{
    const uint32_t capability[5]={0x02000001,0x400,0,0,0};
    copy("/ea/exe","/ea/cap"); CHECK(!setxattr("/ea/cap","security.capability",capability,sizeof(capability),0));
    identity_run("/ea/cap",0,0,20161,1,0x400); identity_run("/ea/cap",1,0,20161,1,0);
    copy("/ea/exe","/ea/nosuid/cap"); CHECK(!setxattr("/ea/nosuid/cap","security.capability",capability,sizeof(capability),0));
    identity_run("/ea/nosuid/cap",0,0,20161,0,0);
    pass("filecap-effective-nnp-and-nosuid");
}
int main(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--legacy-child")) return 42;
    setvbuf(stdout,0,_IONBF,0);
    if (argc==2 && !strcmp(argv[1],"--legacy-cost")) {
        legacy_cost();
        if (getpid()==1) { sync(); reboot(RB_POWER_OFF); }
        return 0;
    }
    if (argc==3 && !strcmp(argv[1],"--probe")) {
        CHECK(!strcmp(argv[0],"kept-zero") && !strcmp((char *)getauxval(AT_EXECFN),argv[2]));
        CHECK(getenv("EXECVEAT_ENV") && !strcmp(getenv("EXECVEAT_ENV"),"kept"));
        if (getenv("EXECVEAT_FDS")) {
            int gone,kept; CHECK(sscanf(getenv("EXECVEAT_FDS"),"%d:%d",&gone,&kept)==2);
            ERROR(fcntl(gone,F_GETFD),EBADF); CHECK(fcntl(kept,F_GETFD)==0 && lseek(kept,0,SEEK_CUR)==7);
        }
        return 42;
    }
    if (argc==4 && !strcmp(argv[1],"--script")) {
        CHECK(!strcmp(argv[0],"/ea/exe") && !strcmp(argv[2],argv[3]) && !strcmp((char *)getauxval(AT_EXECFN),argv[3]));
        int fd=open(argv[2],O_RDONLY); char bytes[2]; CHECK(fd>=0 && read(fd,bytes,2)==2 && !memcmp(bytes,"#!",2) && !close(fd)); return 43;
    }
    if (argc==5 && !strcmp(argv[1],"--ids")) {
        unsigned expected=(unsigned)strtoul(argv[2],0,10),secure=(unsigned)strtoul(argv[3],0,10);
        CHECK(getuid()==20161 && geteuid()==expected && getauxval(AT_UID)==20161 && getauxval(AT_EUID)==expected && getauxval(AT_SECURE)==secure);
        struct { uint32_t version; int32_t pid; } header={0x20080522,0};
        struct { uint32_t effective,permitted,inheritable; } data[2]={{0}};
        unsigned caps=(unsigned)strtoul(argv[4],0,10); CHECK(!syscall(SYS_capget,&header,data));
        CHECK(data[0].effective==caps && data[0].permitted==caps && !data[0].inheritable && !data[1].effective && !data[1].permitted && !data[1].inheritable);
        return 44;
    }
    /* The first syscall is also the missing-syscall baseline probe. */
    char *initial[]={"x",0}; ERROR(execute(AT_FDCWD,"",initial,0,0),ENOENT);
    setup(); names(); empty(); identity(); links(); errors(); truncation(); scripts(); permission(); nonleader(); current_actor(); credentials(); filecap();
    CHECK(!umount("/ea/nosuid") && !umount("/ea/deny") && !umount("/ea") && !rmdir("/ea"));
    printf("EXECVEAT END cases=%u failures=0\n",cases);
    if (getpid()==1) { sync(); reboot(RB_POWER_OFF); }
    return 0;
}
